#!/usr/bin/env python3
"""
puente_emg.py - Puente local entre el Arduino UNO R4 WiFi (ASTRA) y la pagina web.

Por que hace falta: la pagina se abre desde GitHub Pages con HTTPS. Un sitio
HTTPS no puede conectarse a ws://192.168.x.x (el navegador lo bloquea como
"contenido mixto"), y el Arduino no puede servir HTTPS/WSS. Lo que SI permiten
los navegadores es conectarse a la propia computadora (ws://127.0.0.1). Este
programa corre en la laptop, recibe la senal del Arduino por la red local y se
la pasa a la pagina. No guarda nada ni usa internet.

    Arduino --TCP 5005--> [ este puente ] --WebSocket 127.0.0.1:8787--> pagina
            <--UDP 5006-- "aqui estoy" (para que el Arduino lo encuentre solo)

Uso:   python puente_emg.py
       python puente_emg.py --origen https://otro-sitio.github.io
Requisitos: Python 3.8 o mas nuevo. Sin bibliotecas extra.
"""
import argparse
import asyncio
import base64
import hashlib
import json
import socket
import struct
import sys
import time

VERSION = "1"
ORIGENES_POR_DEFECTO = ["https://aleja246.github.io"]
MAX_MUESTRAS = 255
GUID_WS = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def crc16(datos: bytes) -> int:
    """CRC-16/CCITT-FALSE (igual que emg_flujo.h y js/sources.js)."""
    c = 0xFFFF
    for b in datos:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


class Lector:
    """Separa el flujo TCP en paquetes 'EM' validos (con resincronizacion)."""

    def __init__(self):
        self.buf = bytearray()
        self.errores = 0
        self.bytes_descartados = 0

    def agregar(self, datos: bytes):
        self.buf += datos
        paquetes = []
        while True:
            i = self.buf.find(b"EM")
            if i < 0:
                if len(self.buf) > 1:
                    self.bytes_descartados += len(self.buf) - 1
                    del self.buf[:-1]
                break
            if i > 0:
                self.bytes_descartados += i
                del self.buf[:i]
            if len(self.buf) < 12:
                break
            ver, n = self.buf[2], self.buf[3]
            if ver != 1 or n == 0 or n > MAX_MUESTRAS:
                self.errores += 1
                del self.buf[:1]
                continue
            largo = 12 + 2 * n + 2
            if len(self.buf) < largo:
                break
            pkt = bytes(self.buf[:largo])
            if crc16(pkt[:-2]) != struct.unpack_from("<H", pkt, largo - 2)[0]:
                self.errores += 1
                del self.buf[:1]
                continue
            del self.buf[:largo]
            paquetes.append(pkt)
        return paquetes


class Puente:
    def __init__(self, args):
        self.args = args
        self.origenes = set(ORIGENES_POR_DEFECTO + (args.origen or []))
        self.clientes = set()          # colas de los navegadores conectados
        self.arduino = None            # (writer, ip)
        self.est = self._estadisticas_vacias()
        self.ips_locales = []

    def _estadisticas_vacias(self):
        return {"paquetes": 0, "muestras": 0, "errores": 0, "huecos": 0, "fs": None,
                "ultimo_indice": None, "desde": None, "ultimo_dato": None}

    # ---------------- Arduino (TCP) ----------------
    async def atender_arduino(self, reader, writer):
        ip = writer.get_extra_info("peername")[0]
        if self.arduino:
            log(f"Nueva conexion de {ip}; se cierra la anterior ({self.arduino[1]}).")
            self.arduino[0].close()
        self.arduino = (writer, ip)
        self.est = self._estadisticas_vacias()
        self.est["desde"] = time.time()
        sock = writer.get_extra_info("socket")
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        except OSError:
            pass
        log(f"Arduino conectado desde {ip}.")
        lector = Lector()
        try:
            while True:
                try:
                    datos = await asyncio.wait_for(reader.read(4096), timeout=10)
                except asyncio.TimeoutError:
                    log(f"El Arduino {ip} no manda datos hace 10 s; se cierra la conexion.")
                    break
                if not datos:
                    break
                for pkt in lector.agregar(datos):
                    self._recibir(pkt)
                self.est["errores"] = lector.errores
        except (ConnectionError, OSError) as e:
            log(f"Conexion con el Arduino interrumpida: {e}")
        finally:
            if self.arduino and self.arduino[0] is writer:
                self.arduino = None
                log(f"Arduino {ip} desconectado.")
            writer.close()

    def _recibir(self, pkt: bytes):
        n = pkt[3]
        fs, inicio = struct.unpack_from("<HI", pkt, 4)
        e = self.est
        if e["ultimo_indice"] is not None and inicio > e["ultimo_indice"]:
            e["huecos"] += inicio - e["ultimo_indice"]
        e["ultimo_indice"] = inicio + n
        e["paquetes"] += 1
        e["muestras"] += n
        e["fs"] = fs
        e["ultimo_dato"] = time.time()
        for q in list(self.clientes):
            self._enviar_a(q, ("bin", pkt))

    def _enviar_a(self, q, msg):
        try:
            q.put_nowait(msg)
        except asyncio.QueueFull:          # navegador que no lee (~30 s atrasado): se desconecta
            self.clientes.discard(q)
            while not q.empty():
                q.get_nowait()
            q.put_nowait(("cerrar", None))
            log("Una pagina dejo de leer datos; se desconecta (al volver se reconecta sola).")

    def estado(self):
        e = self.est
        ahora = time.time()
        return {
            "tipo": "estado", "version": VERSION,
            "arduino": self.arduino is not None,
            "ip_arduino": self.arduino[1] if self.arduino else None,
            "recibiendo": bool(e["ultimo_dato"] and ahora - e["ultimo_dato"] < 1.0),
            "fs": e["fs"], "paquetes": e["paquetes"], "muestras": e["muestras"],
            "errores": e["errores"], "huecos": e["huecos"],
            "segundos": round(ahora - e["desde"], 1) if e["desde"] else 0,
            "ips_puente": self.ips_locales,
        }

    # ---------------- Navegador (HTTP + WebSocket) ----------------
    def origen_permitido(self, origen):
        if origen is None:
            return False
        if "*" in self.origenes or origen in self.origenes or origen == "null":
            return True
        for local in ("http://localhost", "http://127.0.0.1", "https://localhost", "https://127.0.0.1"):
            if origen == local or origen.startswith(local + ":"):
                return True
        return False

    async def atender_navegador(self, reader, writer):
        try:
            cabecera = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=5)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError):
            writer.close()
            return
        lineas = cabecera.decode("latin-1").split("\r\n")
        partes = lineas[0].split(" ")
        metodo, ruta = (partes + ["", ""])[:2]
        h = {}
        for l in lineas[1:]:
            if ":" in l:
                k, v = l.split(":", 1)
                h[k.strip().lower()] = v.strip()
        origen = h.get("origin")

        if h.get("upgrade", "").lower() == "websocket":
            if not self.origen_permitido(origen):
                log(f"Rechazado: pagina con origen no permitido ({origen}). Usa --origen para agregarlo.")
                writer.write(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
                await self._cerrar(writer)
                return
            clave = h.get("sec-websocket-key", "")
            acepta = base64.b64encode(hashlib.sha1((clave + GUID_WS).encode()).digest()).decode()
            writer.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          f"Sec-WebSocket-Accept: {acepta}\r\n\r\n").encode())
            await writer.drain()
            await self._sesion_ws(reader, writer, origen)
            return

        cors = ""
        if self.origen_permitido(origen):
            cors = (f"Access-Control-Allow-Origin: {origen}\r\nVary: Origin\r\n"
                    "Access-Control-Allow-Private-Network: true\r\nAccess-Control-Allow-Methods: GET\r\n")
        if metodo == "OPTIONS":
            writer.write(f"HTTP/1.1 204 No Content\r\n{cors}Content-Length: 0\r\n\r\n".encode())
        elif ruta.startswith("/estado"):
            cuerpo = json.dumps(self.estado()).encode()
            writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n{cors}"
                         f"Cache-Control: no-store\r\nContent-Length: {len(cuerpo)}\r\n\r\n".encode() + cuerpo)
        else:
            cuerpo = "Puente EMG funcionando. La pagina se conecta por WebSocket a este puerto.\n".encode()
            writer.write(f"HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
                         f"Content-Length: {len(cuerpo)}\r\n\r\n".encode() + cuerpo)
        await self._cerrar(writer)

    async def _cerrar(self, writer):
        try:
            await writer.drain()
        except ConnectionError:
            pass
        writer.close()

    async def _sesion_ws(self, reader, writer, origen):
        q = asyncio.Queue(maxsize=600)       # ~30 s de paquetes
        self.clientes.add(q)
        log(f"Pagina conectada ({origen}). Navegadores: {len(self.clientes)}")
        q.put_nowait(("txt", json.dumps(self.estado())))

        async def escribir():
            while True:
                tipo, dato = await q.get()
                if tipo == "cerrar":
                    writer.write(trama(0x8, struct.pack(">H", 1008)))
                    await writer.drain()
                    return
                writer.write(trama(0x2 if tipo == "bin" else 0x1, dato if tipo == "bin" else dato.encode()))
                await writer.drain()

        async def leer():
            while True:
                op, datos = await leer_trama(reader)
                if op == 0x8:                     # cerrar
                    writer.write(trama(0x8, datos[:2]))
                    await writer.drain()
                    return
                if op == 0x9:                     # ping -> pong
                    writer.write(trama(0xA, datos))

        tareas = [asyncio.ensure_future(escribir()), asyncio.ensure_future(leer())]
        try:
            await asyncio.wait(tareas, return_when=asyncio.FIRST_COMPLETED)
        except Exception:
            pass
        finally:
            for t in tareas:
                t.cancel()
            self.clientes.discard(q)
            writer.close()
            log(f"Pagina desconectada. Navegadores: {len(self.clientes)}")

    async def estado_periodico(self):
        while True:
            await asyncio.sleep(1)
            msg = json.dumps(self.estado())
            for q in list(self.clientes):
                self._enviar_a(q, ("txt", msg))

    # ---------------- Aviso UDP para que el Arduino nos encuentre ----------------
    async def avisar(self):
        texto = f"ASTRA-EMG-PUENTE {VERSION} {self.args.puerto_tcp}".encode()
        socks, ultima_revision, ips_previas = {}, 0, None
        while True:
            if time.time() - ultima_revision > 10:
                ultima_revision = time.time()
                self.ips_locales = ips_locales()
                if self.ips_locales != ips_previas:
                    ips_previas = self.ips_locales
                    if self.ips_locales:
                        log("IP de esta laptop: " + ", ".join(self.ips_locales) +
                            "  (si el Arduino no encuentra el puente, pon una en SECRET_PUENTE_IP)")
                    else:
                        log("Esta laptop no tiene IP en una red local. Conectala a la misma red WiFi que el Arduino.")
                for s in socks.values():
                    s.close()
                socks = {}
                for ip in self.ips_locales:
                    try:
                        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                        s.bind((ip, 0))
                        socks[ip] = s
                    except OSError:
                        pass
            for ip, s in socks.items():
                destinos = {"255.255.255.255", ip.rsplit(".", 1)[0] + ".255"} | set(self.args.aviso or [])
                for d in destinos:
                    try:
                        s.sendto(texto, (d, self.args.puerto_udp))
                    except OSError:
                        pass
            await asyncio.sleep(1)


def trama(opcode: int, datos: bytes) -> bytes:
    n = len(datos)
    if n < 126:
        cab = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        cab = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        cab = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return cab + datos


async def leer_trama(reader):
    b0, b1 = await reader.readexactly(2)
    op, n = b0 & 0x0F, b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", await reader.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await reader.readexactly(8))[0]
    if n > 65536:
        raise ConnectionError("trama demasiado grande")
    mascara = await reader.readexactly(4) if b1 & 0x80 else b"\0\0\0\0"
    datos = bytearray(await reader.readexactly(n))
    for i in range(n):
        datos[i] ^= mascara[i % 4]
    return op, bytes(datos)


def ips_locales():
    ips = set()
    try:  # IP de la ruta por defecto (no manda ningun paquete)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return sorted(ip for ip in ips if not ip.startswith("127.") and not ip.startswith("169.254."))


def log(msg):
    print(time.strftime("[%H:%M:%S] ") + msg, flush=True)


async def principal(args):
    p = Puente(args)
    srv_ard = await asyncio.start_server(p.atender_arduino, "0.0.0.0", args.puerto_tcp)
    srv_web = await asyncio.start_server(p.atender_navegador, "127.0.0.1", args.puerto_ws)
    log(f"Puente EMG listo. Arduino -> TCP {args.puerto_tcp}  |  pagina -> ws://127.0.0.1:{args.puerto_ws}")
    log("Origenes permitidos: " + ", ".join(sorted(p.origenes)) + ", localhost y archivo local")
    tareas = [asyncio.ensure_future(p.estado_periodico())]
    if not args.sin_aviso:
        tareas.append(asyncio.ensure_future(p.avisar()))
    async with srv_ard, srv_web:
        await asyncio.gather(srv_ard.serve_forever(), srv_web.serve_forever(), *tareas)


def main():
    ap = argparse.ArgumentParser(description="Puente local Arduino UNO R4 WiFi -> pagina web (EMG ASTRA)")
    ap.add_argument("--puerto-tcp", type=int, default=5005, help="puerto donde se conecta el Arduino (5005)")
    ap.add_argument("--puerto-udp", type=int, default=5006, help="puerto del aviso para el Arduino (5006)")
    ap.add_argument("--puerto-ws", type=int, default=8787, help="puerto para la pagina (8787)")
    ap.add_argument("--origen", action="append", help="otra direccion de la pagina permitida (p. ej. https://usuario.github.io)")
    ap.add_argument("--aviso", action="append", help="direccion extra a donde mandar el aviso UDP (p. ej. 192.168.0.255)")
    ap.add_argument("--sin-aviso", action="store_true", help="no mandar el aviso UDP (usa SECRET_PUENTE_IP en el Arduino)")
    args = ap.parse_args()
    try:
        asyncio.run(principal(args))
    except KeyboardInterrupt:
        log("Puente detenido.")
    except OSError as e:
        log(f"No se pudo abrir un puerto: {e}. Ya hay otro puente abierto?")
        sys.exit(1)


if __name__ == "__main__":
    main()
