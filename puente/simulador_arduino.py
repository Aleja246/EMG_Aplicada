#!/usr/bin/env python3
"""
simulador_arduino.py - Hace de Arduino UNO R4 WiFi para probar el puente y la
pagina SIN la placa. Manda una senal EMG sintetica (ya "filtrada", en mV, con
contracciones) con el mismo protocolo que diagnostico_emg_enlace_wifi.ino.

Uso:   python simulador_arduino.py                 (busca el puente por el aviso UDP)
       python simulador_arduino.py --ip 127.0.0.1  (directo, en la misma laptop)
Opciones de prueba: --perder 0.02 (tira 2 % de paquetes -> huecos)
                    --reiniciar 30 (simula que el Arduino se reinicia a los 30 s)
                    --basura (mete bytes basura para probar la resincronizacion)
"""
import argparse, math, random, socket, struct, sys, time

def crc16(d):
    c = 0xFFFF
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c

def senal_mv(i, fs=1000):
    """EMG sintetica a la salida de Nova (mV): reposo ~25 mV rms, contracciones de 4-7 s."""
    t = i / fs
    ciclo = t % 12.0
    amp = 25.0
    if 2.0 < ciclo < 2.3 or 8.0 < ciclo < 8.3:
        amp = 300.0                                 # pico breve de movimiento de cabeza
    elif 2.3 <= ciclo < 7.5:
        amp = 450.0 + 250.0 * math.sin(0.7 * t)     # contraccion sostenida
    return random.gauss(0.0, amp)

def paquete(inicio, muestras_mv, fs=1000):
    n = len(muestras_mv)
    cab = b"EM" + bytes([1, n]) + struct.pack("<HIH", fs, inicio & 0xFFFFFFFF, 100)
    q = [max(-32767, min(32767, round(v * 10))) for v in muestras_mv]   # 0.1 mV
    cuerpo = cab + struct.pack("<%dh" % n, *q)
    return cuerpo + struct.pack("<H", crc16(cuerpo))

def buscar_puente(puerto_udp):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("", puerto_udp))
    print(f"Esperando el aviso del puente en UDP {puerto_udp}...")
    while True:
        datos, (ip, _) = s.recvfrom(128)
        txt = datos.decode(errors="replace").split()
        if len(txt) >= 3 and txt[0] == "ASTRA-EMG-PUENTE":
            s.close()
            return ip, int(txt[-1])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip"); ap.add_argument("--puerto", type=int, default=5005)
    ap.add_argument("--puerto-udp", type=int, default=5006)
    ap.add_argument("--perder", type=float, default=0.0)
    ap.add_argument("--reiniciar", type=float, default=0.0)
    ap.add_argument("--basura", action="store_true")
    ap.add_argument("--segundos", type=float, default=0.0, help="terminar despues de N s (0 = nunca)")
    ap.add_argument("--semilla", type=int, default=1)
    ap.add_argument("--registro", help="guardar lo enviado (indice,mV) en este CSV, para comparar")
    a = ap.parse_args()
    random.seed(a.semilla)
    ip, puerto = (a.ip, a.puerto) if a.ip else buscar_puente(a.puerto_udp)
    fs, por_paquete = 1000, 50
    reg = open(a.registro, "w") if a.registro else None
    while True:
        try:
            s = socket.create_connection((ip, puerto), timeout=3)
        except OSError as e:
            print(f"No conecta con {ip}:{puerto} ({e}); reintento en 2 s"); time.sleep(2); continue
        print(f"Conectado al puente {ip}:{puerto}. Mandando 1000 muestras/s.")
        indice, t0, reinicio_hecho, inicio_real = 1000, time.monotonic(), False, time.monotonic()
        try:
            while True:
                transcurrido = time.monotonic() - t0
                if a.reiniciar and not reinicio_hecho and time.monotonic() - inicio_real > a.reiniciar:
                    print("Simulando reinicio del Arduino (el indice vuelve a empezar)")
                    indice, t0, reinicio_hecho = 0, time.monotonic(), True
                    continue
                objetivo = int(transcurrido * fs) + (1000 if not reinicio_hecho else 0)
                while objetivo - indice >= por_paquete:
                    datos = [senal_mv(indice + k) for k in range(por_paquete)]
                    if random.random() >= a.perder:
                        pkt = paquete(indice, datos)
                        if a.basura and random.random() < 0.05:
                            pkt = bytes(random.getrandbits(8) for _ in range(7)) + pkt
                        s.sendall(pkt)
                        if reg:
                            reg.writelines(f"{indice + k},{max(-32767, min(32767, round(v * 10))) / 10:.1f}\n" for k, v in enumerate(datos))
                    indice += por_paquete
                if a.segundos and time.monotonic() - inicio_real > a.segundos:
                    s.close()
                    if reg: reg.close()
                    return
                time.sleep(0.01)
        except OSError as e:
            print(f"Se corto la conexion ({e}); reintentando")
            time.sleep(1)

if __name__ == "__main__":
    try: main()
    except KeyboardInterrupt: sys.exit(0)
