#!/usr/bin/env node
/* =====================================================================
 * ASTRA - servidor de la página (corre en la laptop de la fisioterapeuta)
 * ---------------------------------------------------------------------
 *   Arduino UNO R4 WiFi ──TCP 9001 (señal EMG cruda)────┐
 *                                                        ├─→ este servidor ─→ página: http://localhost:8080
 *   ESP32 del atril     ──TCP 9002 (estado, órdenes)────┘        │  WebSocket /ws  (en vivo)
 *                                                                 └  HTTP /api/... (pacientes y sesiones)
 * - Cada segundo avisa por UDP (puerto 9999) "aquí está el servidor ASTRA":
 *   así las placas lo encuentran solas en la red, sin escribir la IP.
 * - Los pacientes y las sesiones se guardan en servidor/datos/ (archivos JSON y CSV).
 * - Solo usa Node.js (sin instalar paquetes). Uso:  node servidor.js
 * ===================================================================== */
'use strict';
const http = require('http'), net = require('net'), dgram = require('dgram'), fs = require('fs'), path = require('path'),
  os = require('os'), crypto = require('crypto');

const PUERTO_WEB = +process.env.PUERTO_WEB || 8080;
const PUERTO_ARDUINO = 9001, PUERTO_ATRIL = 9002, PUERTO_AVISO = 9999;
const RAIZ_WEB = path.join(__dirname, '..');                 // index.html, css/, js/
const DATOS = process.env.ASTRA_DATOS || path.join(__dirname, 'datos');
const SENALES = path.join(DATOS, 'senales');
fs.mkdirSync(SENALES, { recursive: true });

const log = (...a) => console.log(new Date().toLocaleTimeString('es-MX'), ...a);

/* ---------------- almacenamiento (JSON en disco) ---------------- */
function leer(nombre, def) { try { return JSON.parse(fs.readFileSync(path.join(DATOS, nombre), 'utf8')); } catch (e) { return def; } }
function escribir(nombre, v) {                       // escritura atómica: nunca deja un archivo a medias
  const f = path.join(DATOS, nombre), tmp = f + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(v, null, 1)); fs.renameSync(tmp, f);
}
let pacientes = leer('pacientes.json', []), sesiones = leer('sesiones.json', []);
const uid = () => Date.now().toString(36) + crypto.randomBytes(3).toString('hex');
const idValido = id => /^[a-z0-9]{6,32}$/i.test(id || '');

/* ---------------- monitores (páginas abiertas) por WebSocket ---------------- */
const monitores = new Set();
function wsFrame(op, datos) {
  const n = datos.length; let cab;
  if (n < 126) cab = Buffer.from([0x80 | op, n]);
  else if (n < 65536) { cab = Buffer.alloc(4); cab[0] = 0x80 | op; cab[1] = 126; cab.writeUInt16BE(n, 2); }
  else { cab = Buffer.alloc(10); cab[0] = 0x80 | op; cab[1] = 127; cab.writeBigUInt64BE(BigInt(n), 2); }
  return Buffer.concat([cab, datos]);
}
function enviar(ws, obj) { if (!ws.destroyed) ws.write(wsFrame(1, Buffer.from(JSON.stringify(obj)))); }
function aTodos(obj) { const f = wsFrame(1, Buffer.from(JSON.stringify(obj))); for (const ws of monitores) if (!ws.destroyed) ws.write(f); }
function senalATodos(buf) {                           // binario: paquetes 0xAA 0x55 tal como los manda el Arduino
  const f = wsFrame(2, buf);
  for (const ws of monitores) if (!ws.destroyed && ws.writableLength < 2e6) ws.write(f);   // página lenta: se salta tandas
}
function abrirWebSocket(req, sock) {
  const clave = req.headers['sec-websocket-key'];
  if (!clave || !req.url.startsWith('/ws')) { sock.destroy(); return; }
  const acepta = crypto.createHash('sha1').update(clave + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  sock.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + acepta + '\r\n\r\n');
  sock.setNoDelay(true);
  monitores.add(sock);
  let buf = Buffer.alloc(0);
  sock.on('data', d => {
    buf = Buffer.concat([buf, d]);
    for (;;) {                                         // tramas del navegador (siempre enmascaradas)
      if (buf.length < 2) return;
      const op = buf[0] & 15; let n = buf[1] & 127, i = 2;
      if (n === 126) { if (buf.length < 4) return; n = buf.readUInt16BE(2); i = 4; }
      else if (n === 127) { if (buf.length < 10) return; n = Number(buf.readBigUInt64BE(2)); i = 10; }
      const masc = buf[1] & 128 ? 4 : 0;
      if (buf.length < i + masc + n) return;
      const m = buf.slice(i, i + masc), datos = Buffer.from(buf.slice(i + masc, i + masc + n));
      if (masc) for (let k = 0; k < n; k++) datos[k] ^= m[k & 3];
      buf = buf.slice(i + masc + n);
      if (op === 8) { sock.end(wsFrame(8, Buffer.alloc(0))); return; }
      if (op === 9) { sock.write(wsFrame(10, datos)); continue; }
      if (op === 1) { try { mensajeDePagina(JSON.parse(datos.toString())); } catch (e) { /* mensaje mal formado */ } }
    }
  });
  const quitar = () => monitores.delete(sock);
  sock.on('close', quitar); sock.on('error', quitar);
  // Lo que ya se sabe, para que la página arranque completa
  enviar(sock, estadoServidor());
  if (atril.ultimo) enviar(sock, { type: 'atril', st: Object.assign({}, atril.ultimo, {
    b: atril.bloques.slice(-64), bn: atril.bn, acts: atril.acts.slice(-32), an: atril.an, logs: atril.logs.slice(-60), ln: atril.ln }) });
}
// Órdenes de la página para el atril (las mismas teclas del monitor serie + la música)
function mensajeDePagina(m) {
  if (m.type === 'cmd' && /^[cprh+\-xio]$/.test(m.c || '')) aAtril('C ' + m.c);
  else if (m.type === 'musica' && (m.l === 'abajo' || m.l === 'arriba') && Array.isArray(m.p) && m.p.length === 3 &&
           m.p.every(v => Number.isInteger(v) && v >= 1 && v <= 255)) aAtril('M ' + m.l + ' ' + m.p.join(','));
}

/* ---------------- Arduino: TCP 9001, paquetes binarios ---------------- */
const arduino = { sock: null, ip: '', desde: 0, muestras: 0, t0: 0, diag: null, perdidos: 0 };
function frecuenciaArduino() { const s = (Date.now() - arduino.t0) / 1000; return s > 2 ? arduino.muestras / s : null; }
net.createServer(sock => {
  if (arduino.sock) arduino.sock.destroy();          // una sola placa: la nueva reemplaza
  Object.assign(arduino, { sock, ip: sock.remoteAddress.replace('::ffff:', ''), desde: Date.now(), muestras: 0, t0: Date.now(), diag: null });
  sock.setNoDelay(true); sock.setKeepAlive(true, 5000);
  log('Arduino conectado desde', arduino.ip); aTodos(estadoServidor());
  let buf = Buffer.alloc(0), ultimoDato = Date.now();
  const vigia = setInterval(() => { if (Date.now() - ultimoDato > 5000) sock.destroy(); }, 1000);   // cable "fantasma"
  sock.on('data', d => {
    ultimoDato = Date.now();
    buf = Buffer.concat([buf, d]);
    const salida = []; let i = 0;
    while (i + 4 <= buf.length) {
      if (buf[i] !== 0xAA) { i++; continue; }
      if (buf[i + 1] === 0x55) {                     // datos: AA 55 seq n n*2 chk
        const n = buf[i + 3], len = 4 + 2 * n + 1;
        if (n === 0 || n > 64) { i++; continue; }
        if (i + len > buf.length) break;
        salida.push(buf.slice(i, i + len)); arduino.muestras += n; i += len;
      } else if (buf[i + 1] === 0x57) {              // diagnóstico: AA 57 n texto(n) chk
        const n = buf[i + 2], len = 3 + n + 1;
        if (i + len > buf.length) break;
        try { arduino.diag = JSON.parse(buf.slice(i + 3, i + 3 + n).toString()); } catch (e) { /* ignora */ }
        i += len;
      } else i++;
    }
    buf = buf.slice(i);
    if (salida.length) senalATodos(Buffer.concat(salida));
  });
  const fin = () => {
    clearInterval(vigia);
    if (arduino.sock === sock) { arduino.sock = null; log('Arduino desconectado'); aTodos(estadoServidor()); }
  };
  sock.on('close', fin); sock.on('error', fin);
}).listen(PUERTO_ARDUINO, () => log('Esperando al Arduino en el puerto', PUERTO_ARDUINO));

/* ---------------- Atril (ESP32): TCP 9002, una línea JSON por mensaje ---------------- */
const atril = { sock: null, ip: '', desde: 0, ultimo: null, boot: null, bloques: [], bn: 0, acts: [], an: 0, logs: [], ln: 0 };
function aAtril(linea) { if (atril.sock && !atril.sock.destroyed) atril.sock.write(linea + '\n'); }
net.createServer(sock => {
  if (atril.sock) atril.sock.destroy();
  Object.assign(atril, { sock, ip: sock.remoteAddress.replace('::ffff:', ''), desde: Date.now() });
  sock.setNoDelay(true); sock.setKeepAlive(true, 5000);
  log('Atril (ESP32) conectado desde', atril.ip); aTodos(estadoServidor());
  let txt = '', ultimoDato = Date.now();
  const vigia = setInterval(() => { if (Date.now() - ultimoDato > 5000) sock.destroy(); }, 1000);
  sock.on('data', d => {
    ultimoDato = Date.now();
    txt += d.toString('latin1');
    let k;
    while ((k = txt.indexOf('\n')) >= 0) {
      const linea = txt.slice(0, k); txt = txt.slice(k + 1);
      if (linea.trim()) try { recibirAtril(JSON.parse(linea)); } catch (e) { /* línea dañada */ }
    }
    if (txt.length > 1e5) txt = '';
  });
  const fin = () => {
    clearInterval(vigia);
    if (atril.sock === sock) { atril.sock = null; log('Atril desconectado'); aTodos(estadoServidor()); }
  };
  sock.on('close', fin); sock.on('error', fin);
}).listen(PUERTO_ATRIL, () => log('Esperando al atril (ESP32) en el puerto', PUERTO_ATRIL));

// El atril manda solo lo nuevo (bloques, activaciones y mensajes) con sus contadores totales.
// Si se reconecta lo vuelve a mandar: aquí se quita lo repetido antes de pasarlo a las páginas.
function recibirAtril(st) {
  if (st.boot !== atril.boot) { Object.assign(atril, { boot: st.boot, bloques: [], bn: 0, acts: [], an: 0, logs: [], ln: 0 }); }
  const nuevos = (arr, total, yaVisto) => arr.slice(Math.max(0, arr.length - Math.max(0, total - yaVisto)));
  st.b = nuevos(st.b || [], st.bn, atril.bn); st.acts = nuevos(st.acts || [], st.an, atril.an); st.logs = nuevos(st.logs || [], st.ln, atril.ln);
  atril.bn = Math.max(atril.bn, st.bn); atril.an = Math.max(atril.an, st.an); atril.ln = Math.max(atril.ln, st.ln);
  atril.bloques.push(...st.b); if (atril.bloques.length > 600) atril.bloques.splice(0, atril.bloques.length - 600);
  atril.acts.push(...st.acts); if (atril.acts.length > 500) atril.acts.splice(0, atril.acts.length - 500);
  atril.logs.push(...st.logs); if (atril.logs.length > 300) atril.logs.splice(0, atril.logs.length - 300);
  atril.ultimo = Object.assign({}, st, { b: [], acts: [], logs: [] });
  aTodos({ type: 'atril', st });
}

/* ---------------- estado para las páginas ---------------- */
function ipsLocales() {
  const r = [];
  for (const [nombre, l] of Object.entries(os.networkInterfaces())) for (const a of l || [])
    if (a.family === 'IPv4' && !a.internal) r.push({ nombre, ip: a.address, mascara: a.netmask });
  return r;
}
function estadoServidor() {
  return { type: 'servidor', puertoWeb: PUERTO_WEB, ips: ipsLocales().map(x => x.ip),
    arduino: { conectado: !!arduino.sock, ip: arduino.ip, desde: arduino.desde, fs: frecuenciaArduino(), diag: arduino.diag },
    atril: { conectado: !!atril.sock, ip: atril.ip, desde: atril.desde } };
}
setInterval(() => aTodos(estadoServidor()), 1000);

/* ---------------- aviso UDP: "aquí está el servidor ASTRA" ---------------- */
const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
udp.bind(() => { udp.setBroadcast(true); });
const AVISO = Buffer.from('ASTRA-SERVIDOR ' + PUERTO_ARDUINO + ' ' + PUERTO_ATRIL + '\n');
function direccionDifusion(ip, mascara) {
  const a = ip.split('.').map(Number), m = mascara.split('.').map(Number);
  return a.map((v, i) => (v & m[i]) | (~m[i] & 255)).join('.');
}
setInterval(() => {
  const destinos = new Set(['255.255.255.255', ...ipsLocales().map(x => direccionDifusion(x.ip, x.mascara))]);
  for (const d of destinos) udp.send(AVISO, PUERTO_AVISO, d, () => {});
}, 1000);

/* ---------------- HTTP: página + API ---------------- */
const TIPOS = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.json': 'application/json', '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon', '.woff2': 'font/woff2' };
function json(res, code, obj) { res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store' }); res.end(JSON.stringify(obj)); }
function cuerpo(req) {
  return new Promise((ok, mal) => {
    let t = ''; req.on('data', d => { t += d; if (t.length > 20e6) { mal(new Error('Demasiado grande')); req.destroy(); } });
    req.on('end', () => { try { ok(t ? JSON.parse(t) : {}); } catch (e) { mal(new Error('JSON no válido')); } });
  });
}
const resumenPaciente = p => {
  const ses = sesiones.filter(s => s.patientId === p.id);
  return Object.assign({}, p, { nSesiones: ses.length, ultimaSesion: ses.length ? ses.map(s => s.fecha).sort().pop() : null });
};
async function api(req, res, u) {
  const partes = u.pathname.split('/').filter(Boolean).slice(1);   // sin "api"
  const [rec, id, sub] = partes, m = req.method;
  if (rec === 'estado' && m === 'GET') return json(res, 200, estadoServidor());
  if (rec === 'datos' && m === 'GET') return json(res, 200, { pacientes, sesiones });     // la página lo carga al abrir
  if (rec === 'pacientes') {
    if (!id && m === 'GET') return json(res, 200, { pacientes: pacientes.map(resumenPaciente) });
    if (!id && m === 'POST') {
      const p = await cuerpo(req);
      if (!p.nombres || !p.apellidos) return json(res, 400, { error: 'Faltan nombre y apellidos' });
      p.id = idValido(p.id) ? p.id : uid(); p.creado = p.creado || new Date().toISOString();
      const i = pacientes.findIndex(x => x.id === p.id); if (i >= 0) pacientes[i] = p; else pacientes.push(p);
      escribir('pacientes.json', pacientes); return json(res, 200, { paciente: p });
    }
    const p = pacientes.find(x => x.id === id);
    if (!p) return json(res, 404, { error: 'No existe ese paciente' });
    if (!sub && m === 'GET') return json(res, 200, { paciente: resumenPaciente(p), sesiones: sesiones.filter(s => s.patientId === id) });
    if (!sub && m === 'PATCH') {
      const cambios = await cuerpo(req); delete cambios.id;
      Object.assign(p, cambios); escribir('pacientes.json', pacientes); return json(res, 200, { paciente: p });
    }
    if (!sub && m === 'DELETE') {
      for (const s of sesiones.filter(s => s.patientId === id)) fs.rm(path.join(SENALES, s.id + '.csv'), { force: true }, () => {});
      pacientes = pacientes.filter(x => x.id !== id); sesiones = sesiones.filter(s => s.patientId !== id);
      escribir('pacientes.json', pacientes); escribir('sesiones.json', sesiones); return json(res, 200, { ok: true });
    }
  }
  if (rec === 'sesiones') {
    if (!id && m === 'POST') {                         // { sesion, senal: [[t, rms, mediana, activacion], ...] }
      const { sesion: s, senal } = await cuerpo(req);
      if (!s || !pacientes.some(p => p.id === s.patientId)) return json(res, 400, { error: 'Sesión sin paciente válido' });
      s.id = idValido(s.id) ? s.id : uid();
      if (Array.isArray(senal) && senal.length) {
        fs.writeFileSync(path.join(SENALES, s.id + '.csv'), 'tiempo_s,rms_mV,mediana_mV,activacion\r\n' + senal.map(r => r.join(',')).join('\r\n'));
        s.conSenal = true;
      }
      const i = sesiones.findIndex(x => x.id === s.id); if (i >= 0) sesiones[i] = s; else sesiones.push(s);
      escribir('sesiones.json', sesiones); return json(res, 200, { sesion: s });
    }
    if (!idValido(id)) return json(res, 404, { error: 'No existe esa sesión' });
    if (sub === 'senal' && m === 'GET') {
      const f = path.join(SENALES, id + '.csv');
      if (!fs.existsSync(f)) return json(res, 404, { error: 'Esta sesión no tiene señal guardada' });
      res.writeHead(200, { 'Content-Type': 'text/csv; charset=utf-8' }); return fs.createReadStream(f).pipe(res);
    }
    if (!sub && m === 'DELETE') {
      sesiones = sesiones.filter(s => s.id !== id); fs.rm(path.join(SENALES, id + '.csv'), { force: true }, () => {});
      escribir('sesiones.json', sesiones); return json(res, 200, { ok: true });
    }
  }
  if (rec === 'restaurar' && m === 'POST') {          // respaldo JSON o datos de un navegador: se agregan los que falten
    const d = await cuerpo(req); let np = 0, ns = 0;
    for (const p of d.patients || d.pacientes || []) if (p && p.id && !pacientes.some(x => x.id === p.id)) { pacientes.push(p); np++; }
    for (const s of d.sessions || d.sesiones || []) if (s && s.id && !sesiones.some(x => x.id === s.id)) { sesiones.push(s); ns++; }
    escribir('pacientes.json', pacientes); escribir('sesiones.json', sesiones);
    return json(res, 200, { pacientes: np, sesiones: ns });
  }
  return json(res, 404, { error: 'Ruta no encontrada' });
}
const servidor = http.createServer(async (req, res) => {
  const u = new URL(req.url, 'http://x');
  try {
    if (u.pathname.startsWith('/api/')) return await api(req, res, u);
    let f = path.normalize(path.join(RAIZ_WEB, decodeURIComponent(u.pathname === '/' ? '/index.html' : u.pathname)));
    const rel = path.relative(RAIZ_WEB, f);
    if (rel.startsWith('..') || /^(servidor|firmware|arduino|\.git)([\\/]|$)/.test(rel) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' }); return res.end('No encontrado');
    }
    res.writeHead(200, { 'Content-Type': TIPOS[path.extname(f).toLowerCase()] || 'application/octet-stream', 'Cache-Control': 'no-cache' });
    fs.createReadStream(f).pipe(res);
  } catch (e) { json(res, 400, { error: e.message }); }
});
servidor.on('upgrade', abrirWebSocket);
servidor.listen(PUERTO_WEB, () => {
  log('==============================================================');
  log(' ASTRA - servidor listo. Abre en Chrome o Edge:  http://localhost:' + PUERTO_WEB);
  for (const x of ipsLocales()) log('   (desde otro equipo de la misma red: http://' + x.ip + ':' + PUERTO_WEB + ')');
  log(' Datos de pacientes en:', DATOS);
  log(' Deja esta ventana abierta mientras uses la página.');
  log('==============================================================');
});
