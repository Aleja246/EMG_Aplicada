// GENERADO desde pagina.html con herramientas/html_a_h.py: no lo edites a mano.
#pragma once
const char PAGINA[] PROGMEM = R"PAGINA(<!doctype html>
<!--
  ASTRA · página de la terapia. La sirve el ESP32 en http://192.168.4.1
  Regla: la página SOLO muestra datos y manda botones. No calcula nada:
  el conteo, los umbrales, las métricas y el CSV los hace el ESP32.
  Si la página se cierra, la terapia sigue.

  Para trabajar sin el ESP32 (datos simulados, nunca en la demo):
    abre este archivo en el navegador y agrega ?sim al final de la dirección:
    file:///.../pagina.html?sim
  Conectada a otro ESP32 (o a un simulador):  pagina.html?esp=192.168.4.1

  IMPORTANTE: después de cambiar este archivo, regenera pagina.h:
    python herramientas/html_a_h.py
-->
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ASTRA · Terapia</title>
<link rel="icon" href="data:,">
<style>
:root {
  --fondo: #f3f6f8; --tarjeta: #fff; --texto: #1b2733; --suave: #5f6f7f; --linea: #dde4ea; --rejilla: #e8edf1;
  --primario: #0f766e; --rms: #0f766e; --alto: #c2410c; --bajo: #b7791f;
  --ok: #15803d; --ok-f: #e4f4ea; --mal: #b91c1c; --mal-f: #fde7e7; --aviso: #b45309; --aviso-f: #fdf0dc; --info: #1d4ed8; --info-f: #e3ebfd;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --fondo: #0f172a; --tarjeta: #1a2436; --texto: #e2e8f0; --suave: #94a3b8; --linea: #2c3a50; --rejilla: #243247;
    --primario: #2dd4bf; --rms: #2dd4bf; --alto: #fb923c; --bajo: #facc15;
    --ok: #4ade80; --ok-f: #12301f; --mal: #f87171; --mal-f: #3a1717; --aviso: #fbbf24; --aviso-f: #3a2c10; --info: #93b4ff; --info-f: #1b2850;
  }
}
* { box-sizing: border-box; }
body { margin: 0; font: 16px/1.4 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; background: var(--fondo); color: var(--texto); }
h2 { font-size: 1.05rem; margin: 0 0 .5rem; }
[hidden] { display: none !important; }
.suave { color: var(--suave); }
.sim { background: #7c2d12; color: #fff; text-align: center; font-weight: 700; padding: .35rem 1rem; }
header { position: sticky; top: 0; z-index: 5; display: flex; flex-wrap: wrap; gap: .5rem; align-items: center;
  justify-content: space-between; padding: .5rem 1rem; background: var(--tarjeta); border-bottom: 1px solid var(--linea); }
.marca { font-size: 1.15rem; color: var(--primario); } .marca span { color: var(--suave); font-weight: 400; font-size: .9rem; }
.chips { display: flex; flex-wrap: wrap; gap: .4rem; align-items: center; }
.chip { display: inline-flex; align-items: center; gap: .4rem; padding: .25rem .65rem; border-radius: 99px; background: var(--linea); font-size: .85rem; }
.chip i { width: .65rem; height: .65rem; border-radius: 50%; background: var(--suave); }
.chip.ok { background: var(--ok-f); } .chip.ok i { background: var(--ok); }
.chip.mal { background: var(--mal-f); } .chip.mal i { background: var(--mal); }
.chip.aviso { background: var(--aviso-f); } .chip.aviso i { background: var(--aviso); }
.reloj { font: 700 1.3rem ui-monospace, Menlo, Consolas, monospace; color: var(--primario); min-width: 4.5ch; text-align: right; }
main { max-width: 1100px; margin: 0 auto; padding: 1rem; }
.tarjeta { background: var(--tarjeta); border: 1px solid var(--linea); border-radius: 12px; padding: 1rem 1.1rem; margin-bottom: 1rem; }
.fila { display: flex; flex-wrap: wrap; gap: .5rem 1rem; align-items: center; justify-content: space-between; margin-bottom: .4rem; }
.estado-grande { display: grid; grid-template-columns: 1.3fr 1fr 1fr; gap: 1rem; align-items: center; }
.etq { font-size: .8rem; font-weight: 600; color: var(--suave); text-transform: uppercase; letter-spacing: .03em; }
.estado { font-size: 1.6rem; font-weight: 800; }
.estado.ok { color: var(--ok); } .estado.mal { color: var(--mal); } .estado.aviso { color: var(--aviso); }
.estado.info { color: var(--info); } .estado.suave { color: var(--suave); }
.num { font: 800 3.6rem/1 ui-monospace, Menlo, Consolas, monospace; color: var(--primario); font-variant-numeric: tabular-nums; }
.nivel { font-size: 1.9rem; letter-spacing: .1em; color: #eab308; white-space: nowrap; }
.leyenda { display: flex; gap: .9rem; flex-wrap: wrap; font-size: .82rem; color: var(--suave); }
.leyenda i { display: inline-block; width: 1.2rem; height: 0; margin-right: .3rem; vertical-align: middle; border-top: 3px solid var(--rms); }
.leyenda .l-alto { border-top: 2px dashed var(--alto); } .leyenda .l-bajo { border-top: 2px dashed var(--bajo); }
canvas { display: block; width: 100%; height: 280px; }
.columnas { display: grid; grid-template-columns: 1fr 1fr; gap: 0 1rem; }
.botones { display: flex; flex-wrap: wrap; gap: .5rem; margin: .5rem 0 .7rem; }
button { font: inherit; font-weight: 600; padding: .6rem 1rem; border-radius: 10px; border: 2px solid var(--primario);
  background: var(--primario); color: #fff; cursor: pointer; }
button:disabled { opacity: .4; cursor: not-allowed; }
button.sec { background: transparent; color: var(--primario); }
button.ok { background: var(--ok); border-color: var(--ok); } button.mal { background: var(--mal); border-color: var(--mal); }
button.aviso { background: var(--aviso); border-color: var(--aviso); }
@media (prefers-color-scheme: dark) { button:not(.sec) { color: #0f172a; } }
label { display: block; font-size: .9rem; font-weight: 600; color: var(--suave); margin: .55rem 0; }
label.check { display: flex; gap: .5rem; align-items: center; font-weight: 500; }
input[type=text], input[type=number], input:not([type]) { font: inherit; color: var(--texto); background: var(--tarjeta);
  border: 1px solid var(--linea); border-radius: 8px; padding: .45rem .6rem; }
input:not([type]) { display: block; width: 100%; margin-top: .25rem; }
input[type=number] { width: 4.5rem; margin: 0 .3rem; }
input[type=range] { width: 100%; accent-color: var(--primario); }
.rango { display: flex; justify-content: space-between; font-size: .78rem; font-weight: 400; }
.barra { height: 10px; background: var(--linea); border-radius: 6px; overflow: hidden; margin-bottom: .6rem; }
.barra div { height: 100%; width: 0; background: var(--aviso); transition: width .3s; }
.datos { display: grid; grid-template-columns: auto 1fr; gap: .2rem .8rem; margin: 0; font-size: .92rem; }
.datos dt { color: var(--suave); } .datos dd { margin: 0; font-weight: 600; font-variant-numeric: tabular-nums; }
.kpis { display: grid; grid-template-columns: repeat(auto-fit, minmax(160px, 1fr)); gap: .7rem; }
.kpis div { border: 1px solid var(--linea); border-radius: 10px; padding: .6rem .8rem; display: flex; flex-direction: column; }
.kpis span { font-size: .8rem; color: var(--suave); font-weight: 600; }
.kpis b { font-size: 1.7rem; color: var(--primario); font-variant-numeric: tabular-nums; }
.kpis small { color: var(--suave); font-size: .75rem; }
.mensajes { list-style: none; margin: 0; padding: 0; font-size: .9rem; }
.mensajes li { padding: .3rem 0; border-bottom: 1px solid var(--linea); }
.mensajes li:first-child { font-weight: 600; }
.diag { font-size: .8rem; margin: .6rem 0 0; }
@media (max-width: 760px) {
  main { padding: .6rem; }
  .estado-grande, .columnas { grid-template-columns: 1fr; }
  .estado-grande { gap: .4rem; }
  .num { font-size: 3rem; }
  canvas { height: 220px; }
}
</style>
</head>
<body>
<div id="avisoSim" class="sim" hidden>MODO SIMULADO · datos falsos para probar la página · no usar en la demo</div>
<header>
  <b class="marca">ASTRA <span>· terapia con sEMG</span></b>
  <div class="chips">
    <span class="chip" id="chipWs"><i></i><span>ESP32: conectando…</span></span>
    <span class="chip" id="chipSenal"><i></i><span>Arduino: —</span></span>
    <span class="reloj" id="reloj" title="Tiempo de terapia">00:00</span>
  </div>
</header>
<main>
  <section class="tarjeta estado-grande">
    <div><div class="etq">Estado</div><div class="estado suave" id="estado">—</div></div>
    <div><div class="etq">Levantamientos</div><div class="num" id="n">0</div></div>
    <div><div class="etq">Estrellas</div><div class="nivel" id="nivel">☆☆☆☆☆</div><div class="suave" id="rondas">Rondas: 0</div></div>
  </section>

  <section class="tarjeta">
    <div class="fila">
      <h2>Señal RMS (mV) · últimos 30 s</h2>
      <div class="leyenda"><span><i></i>RMS</span><span><i class="l-alto"></i>Umbral alto</span><span><i class="l-bajo"></i>Umbral bajo</span></div>
    </div>
    <canvas id="grafica"></canvas>
  </section>

  <div class="columnas">
    <section class="tarjeta">
      <h2>1 · Calibración</h2>
      <p class="suave">Solo el reposo es obligatorio. No se le pide nada al niño.</p>
      <div class="botones">
        <button id="bReposo" data-cmd>Reposo · 15 s</button>
        <button id="bPasiva" class="sec" data-cmd>Pasiva · 10 s</button>
        <button id="bRef" class="sec" data-cmd>Referencia · 45 s</button>
        <button id="bCancelar" class="sec" data-cmd hidden>Cancelar</button>
      </div>
      <div class="barra" id="barraCal" hidden><div></div></div>
      <dl class="datos" id="datosCal"><dt>Reposo</dt><dd>sin calibrar</dd></dl>
    </section>

    <section class="tarjeta">
      <h2>2 · Sesión</h2>
      <label>Paciente <input id="paciente" placeholder="Nombre (solo para el archivo del resumen)" autocomplete="off"></label>
      <div class="botones">
        <button id="bIniciar" class="ok" data-cmd>▶ Iniciar terapia</button>
        <button id="bTerminar" class="mal" data-cmd>■ Terminar</button>
        <button id="bReanudar" class="aviso" data-cmd hidden>Reanudar tras el paro</button>
      </div>
      <label>Sensibilidad: k = <b id="kVal">3.0</b>
        <input type="range" id="k" min="1.5" max="6" step="0.5" value="3" data-cmd>
        <span class="rango"><span>← más sensible</span><span>menos falsos →</span></span></label>
      <label>Invitación tras <input type="number" id="inv" min="3" max="120" step="1" value="8" data-cmd> s sin activación</label>
      <label class="check"><input type="checkbox" id="bajar" checked data-cmd> Las estrellas bajan durante la invitación</label>
      <label class="check"><input type="checkbox" id="familia" data-cmd> Grabación de la familia al llegar arriba</label>
      <label>Volumen <input type="range" id="vol" min="0" max="30" step="1" value="12" data-cmd></label>
    </section>
  </div>

  <section class="tarjeta">
    <div class="fila">
      <h2>3 · Métricas de la sesión</h2>
      <button id="bCsv" class="sec" data-cmd>Descargar resumen (CSV)</button>
    </div>
    <div class="kpis">
      <div><span>Levantamientos por minuto</span><b id="mLpm">—</b><small>intentos</small></div>
      <div><span>Amplitud media</span><b id="mAmp">—</b><small>% de la referencia</small></div>
      <div><span>Duración media</span><b id="mDur">—</b><small>tiempo sobre el umbral</small></div>
      <div><span>Respuesta a la invitación</span><b id="mResp">—</b><small id="mInv">—</small></div>
      <div><span>Rondas completas</span><b id="mRondas">0</b><small>5 escalones = 40 cm</small></div>
      <div><span>Duración de la sesión</span><b id="mT">00:00</b><small>min:s</small></div>
    </div>
  </section>

  <section class="tarjeta">
    <h2>Mensajes del ESP32</h2>
    <ul id="mensajes" class="mensajes"></ul>
    <p class="suave diag" id="diag"></p>
  </section>
</main>
<script>
'use strict';
const $ = id => document.getElementById(id);
const P = new URLSearchParams(location.search);
const LOCAL = location.protocol === 'file:' || /^(localhost|127\.0\.0\.1|\[::1\])$/.test(location.hostname);
const SIM = LOCAL && P.has('sim');                     // datos simulados: solo fuera del ESP32
const HOST = LOCAL ? (P.get('esp') || '192.168.4.1') : location.host;
const BASE = LOCAL ? 'http://' + HOST : '';
const N = 600;                                         // 30 s a 20 muestras/s
const serie = { rms: new Float32Array(N).fill(NaN), alto: new Float32Array(N).fill(NaN), bajo: new Float32Array(N).fill(NaN) };
let pos = 0, sucio = true, ymax = 0.1;
let ws = null, espera = 1000, ultimoMsg = 0, conectado = false, modoPrevio = '', descargarAlTerminar = false;

const ESTADOS = {
  ESPERA: ['En espera', 'suave'], CAL_REPOSO: ['Calibrando: reposo', 'aviso'], CAL_PASIVA: ['Calibrando: pasiva', 'aviso'],
  CAL_REF: ['Calibrando: referencia', 'aviso'], BASAL: ['Basal', 'suave'], ACTIVO: ['¡Levantamiento!', 'ok'],
  INVITACION: ['Invitación', 'info'], CELEBRA: ['¡Ronda completa!', 'ok'], SIN_SENAL: ['Sin señal del Arduino', 'mal'],
  PARO: ['PARO de emergencia', 'mal']
};
const dos = n => String(n).padStart(2, '0');
const reloj = s => dos(Math.floor(s / 60)) + ':' + dos(Math.floor(s % 60));
function poner(id, v) { const el = $(id); v = String(v); if (el.textContent !== v) el.textContent = v; }
function chip(id, clase, txt) { const c = $(id); c.className = 'chip ' + clase; c.lastElementChild.textContent = txt; }
function sync(id, v) { const el = $(id); if (document.activeElement !== el && String(el.value) !== String(v)) el.value = v; }
function syncCheck(id, v) { const el = $(id); if (document.activeElement !== el) el.checked = !!v; }

// ---------------- Conexión con el ESP32 (WebSocket) ----------------
function conectar() {
  if (SIM) { $('avisoSim').hidden = false; conectado = true; chip('chipWs', 'aviso', 'Simulación'); Sim.iniciar(recibir); return; }
  chip('chipWs', 'aviso', 'ESP32: conectando…');
  try { ws = new WebSocket('ws://' + HOST + '/ws'); } catch (e) { reintentar(); return; }
  ws.onopen = () => { espera = 1000; conectado = true; ultimoMsg = Date.now(); chip('chipWs', 'ok', 'ESP32: conectado'); };
  ws.onmessage = e => recibir(e.data);
  ws.onclose = () => { ws = null; desconectado(); reintentar(); };
  ws.onerror = () => { try { ws.close(); } catch (e) { /* ya cerrado */ } };
}
function reintentar() { setTimeout(conectar, espera); espera = Math.min(espera * 2, 5000); }
function desconectado() {
  conectado = false;
  chip('chipWs', 'mal', 'ESP32: sin conexión (reintentando)');
  chip('chipSenal', '', 'Arduino: —');
  document.querySelectorAll('[data-cmd]').forEach(b => { b.disabled = true; });
}
function enviar(obj) {
  if (SIM) return Sim.comando(obj);
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
  else mensaje('Sin conexión con el ESP32: no se envió la orden.');
}
setInterval(() => {                                     // conexión "colgada": el ESP32 manda algo cada segundo
  if (!SIM && ws && ws.readyState === 1 && Date.now() - ultimoMsg > 4000) ws.close();
}, 1000);

function recibir(txt) {
  ultimoMsg = Date.now();
  let m;
  try { m = JSON.parse(txt); } catch (e) { return; }
  if (m.tipo === 'info') pintarInfo(m);
  else if (m.tipo === 'msg') mensaje(m.txt);
  else if ('rms' in m) dato(m);
}

// ---------------- Datos (cada 50 ms) ----------------
function dato(m) {
  serie.rms[pos] = m.rms == null ? NaN : m.rms;
  serie.alto[pos] = m.alto > 0 ? m.alto : NaN;
  serie.bajo[pos] = m.bajo > 0 ? m.bajo : NaN;
  pos = (pos + 1) % N;
  sucio = true;
  const e = ESTADOS[m.estado] || [m.estado, 'suave'];
  poner('estado', e[0]);
  const clase = 'estado ' + e[1];
  if ($('estado').className !== clase) $('estado').className = clase;
  poner('n', m.n);
  poner('nivel', '★'.repeat(m.nivel) + '☆'.repeat(Math.max(0, 5 - m.nivel)));
  poner('reloj', reloj(m.t));
}

// ---------------- Estado completo y métricas (cada segundo) ----------------
function pintarInfo(m) {
  chip('chipSenal', m.senal ? 'ok' : 'mal', m.senal ? 'Arduino: ' + m.lps + ' líneas/s' : 'Arduino: SIN SEÑAL');
  const espera = m.modo === 'ESPERA', cal = m.modo.indexOf('CAL_') === 0, ses = m.modo === 'SESION', paro = m.modo === 'PARO';
  document.querySelectorAll('[data-cmd]').forEach(b => { b.disabled = false; });
  $('bReposo').disabled = !espera || !m.senal;
  $('bPasiva').disabled = $('bRef').disabled = !espera || !m.senal || !m.cal;
  $('bCancelar').hidden = !cal;
  $('bIniciar').disabled = !espera || !m.senal || !m.cal;
  $('bTerminar').disabled = !(ses || paro);
  $('bReanudar').hidden = !paro;
  $('bCsv').disabled = !m.csv;
  $('barraCal').hidden = !cal;
  if (cal && m.calTotal) $('barraCal').firstElementChild.style.width = (100 * (m.calTotal - m.calRestan) / m.calTotal) + '%';

  const f = v => v.toFixed(4);
  const filas = m.cal ? [
    ['Reposo', 'media ' + f(m.media) + ' · SD ' + f(m.sd) + ' (variación ' + m.cv + ' %)'],
    ['Umbral alto / bajo', f(m.alto) + ' / ' + f(m.bajo) + '  (k = ' + m.k.toFixed(1) + ')'],
    ['Pasiva', m.pasiva ? 'hecha · piso ' + f(m.piso) : 'no se hizo (opcional)'],
    ['Referencia 100 %', m.ref > 0 ? f(m.ref) + (m.refFuente === 'calibracion' ? ' (calibración)' : ' (máximo de la sesión)') : '— (opcional)']
  ] : [['Reposo', 'sin calibrar: pulsa «Reposo · 15 s»']];
  if (cal) filas.unshift(['Calibrando', 'faltan ' + m.calRestan + ' s']);
  const html = filas.map(r => '<dt>' + r[0] + '</dt><dd>' + r[1] + '</dd>').join('');
  if ($('datosCal').innerHTML !== html) $('datosCal').innerHTML = html;

  sync('k', m.k); if (document.activeElement !== $('k')) poner('kVal', m.k.toFixed(1));
  sync('inv', m.inv); syncCheck('bajar', m.bajar); syncCheck('familia', m.familia); sync('vol', m.vol);

  const hay = m.n > 0;
  poner('mLpm', m.t >= 1 ? m.lpm.toFixed(1) : '—');
  poner('mAmp', hay && m.ref > 0 ? m.amp + ' %' : '—');
  poner('mDur', hay ? m.dur.toFixed(2) + ' s' : '—');
  poner('mResp', m.resp >= 0 ? m.resp.toFixed(1) + ' s' : '—');
  poner('mInv', m.nResp + ' de ' + m.nInv + ' invitaciones');
  poner('mRondas', m.rondas); poner('rondas', 'Rondas: ' + m.rondas);
  poner('mT', reloj(m.t)); poner('reloj', reloj(m.t));
  poner('diag', 'Estrellas: ' + m.est + ' · líneas dañadas del enlace: ' + m.malas + ' · modo ' + m.modo);

  if (descargarAlTerminar && modoPrevio === 'SESION' && m.modo === 'ESPERA') { descargarAlTerminar = false; descargar(); }
  modoPrevio = m.modo;
}

function mensaje(txt) {
  const li = document.createElement('li');
  li.textContent = new Date().toTimeString().slice(0, 8) + '  ' + txt;
  const ul = $('mensajes');
  ul.prepend(li);
  while (ul.children.length > 8) ul.lastElementChild.remove();
}

// ---------------- Gráfica: solo se redibuja cuando llega un dato ----------------
const lienzo = $('grafica'), ctx = lienzo.getContext('2d');
function pasoBonito(x) { const p = Math.pow(10, Math.floor(Math.log10(x))), r = x / p; return (r <= 1 ? 1 : r <= 2 ? 2 : r <= 5 ? 5 : 10) * p; }
function fmtEje(v) { return v === 0 ? '0' : v < 0.1 ? v.toFixed(3) : v < 10 ? v.toFixed(2) : v.toFixed(1); }
function graficar() {
  const dpr = window.devicePixelRatio || 1, w = lienzo.clientWidth, h = lienzo.clientHeight;
  if (!w || !h) return;
  if (lienzo.width !== Math.round(w * dpr) || lienzo.height !== Math.round(h * dpr)) { lienzo.width = Math.round(w * dpr); lienzo.height = Math.round(h * dpr); }
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  const css = getComputedStyle(document.documentElement), col = n => css.getPropertyValue(n).trim();
  const L = 50, R = 8, T = 8, B = 22, W = w - L - R, H = h - T - B;
  let mx = 0;
  for (let i = 0; i < N; i++) { if (serie.rms[i] > mx) mx = serie.rms[i]; if (serie.alto[i] > mx) mx = serie.alto[i]; }
  mx = Math.max(mx * 1.2, 0.01);
  ymax = mx > ymax ? mx : ymax - (ymax - mx) * 0.04;     // la escala baja despacio
  const paso = pasoBonito(ymax / 4);
  ctx.font = '11px system-ui, sans-serif'; ctx.lineWidth = 1; ctx.fillStyle = col('--suave'); ctx.strokeStyle = col('--rejilla');
  ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  for (let v = 0; v <= ymax + 1e-9; v += paso) {
    const y = T + H - v / ymax * H;
    ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + W, y); ctx.stroke(); ctx.fillText(fmtEje(v), L - 6, y);
  }
  ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  for (let s = 0; s <= 30; s += 5) { ctx.textAlign = s ? 'center' : 'right'; ctx.fillText(s ? '−' + s + ' s' : 'ahora', L + W - s / 30 * W, T + H + 6); }
  const linea = (arr, color, rayas, ancho) => {
    ctx.strokeStyle = color; ctx.setLineDash(rayas); ctx.lineWidth = ancho; ctx.beginPath();
    let pluma = false;
    for (let k = 0; k < N; k++) {
      const v = arr[(pos + k) % N];
      if (Number.isNaN(v)) { pluma = false; continue; }
      const x = L + k / (N - 1) * W, y = T + H - Math.min(v, ymax) / ymax * H;
      if (pluma) ctx.lineTo(x, y); else ctx.moveTo(x, y);
      pluma = true;
    }
    ctx.stroke(); ctx.setLineDash([]);
  };
  linea(serie.bajo, col('--bajo'), [6, 4], 1.5);
  linea(serie.alto, col('--alto'), [6, 4], 1.5);
  linea(serie.rms, col('--rms'), [], 2);
}
(function cuadro() { if (sucio) { sucio = false; graficar(); } requestAnimationFrame(cuadro); })();
window.addEventListener('resize', () => { sucio = true; });

// ---------------- Botones: solo mandan órdenes ----------------
function descargar() {
  const d = new Date();
  const fecha = d.getFullYear() + '-' + dos(d.getMonth() + 1) + '-' + dos(d.getDate()) + ' ' + dos(d.getHours()) + ':' + dos(d.getMinutes());
  const nombre = $('paciente').value.trim();
  if (SIM) { Sim.csv(nombre, fecha); return; }
  const a = document.createElement('a');
  a.href = BASE + '/resumen.csv?p=' + encodeURIComponent(nombre) + '&f=' + encodeURIComponent(fecha);
  a.download = '';
  document.body.appendChild(a); a.click(); a.remove();
}
$('bReposo').onclick = () => enviar({ cmd: 'cal_reposo' });
$('bPasiva').onclick = () => enviar({ cmd: 'cal_pasiva' });
$('bRef').onclick = () => enviar({ cmd: 'cal_ref' });
$('bCancelar').onclick = () => enviar({ cmd: 'cancelar' });
$('bIniciar').onclick = () => enviar({ cmd: 'iniciar' });
$('bTerminar').onclick = () => {
  if (!confirm('¿Terminar la terapia? Se descargará el resumen.')) return;
  descargarAlTerminar = true;
  enviar({ cmd: 'terminar' });
};
$('bReanudar').onclick = () => enviar({ cmd: 'reanudar' });
$('bCsv').onclick = descargar;
$('k').oninput = () => poner('kVal', (+$('k').value).toFixed(1));
$('k').onchange = () => enviar({ cmd: 'sens', k: +$('k').value });
$('inv').onchange = () => enviar({ cmd: 'inv', s: +$('inv').value });
$('bajar').onchange = () => enviar({ cmd: 'bajar', on: $('bajar').checked ? 1 : 0 });
$('familia').onchange = () => enviar({ cmd: 'familia', on: $('familia').checked ? 1 : 0 });
$('vol').onchange = () => enviar({ cmd: 'vol', v: +$('vol').value });
try { $('paciente').value = localStorage.getItem('astra.paciente') || ''; } catch (e) { /* sin almacenamiento */ }
$('paciente').oninput = () => { try { localStorage.setItem('astra.paciente', $('paciente').value); } catch (e) { /* sin almacenamiento */ } };

// ---------------- Simulador: SOLO con ?sim y fuera del ESP32 (nunca en la demo) ----------------
// Imita los mensajes del ESP32 para trabajar la página sin el hardware.
const Sim = (() => {
  let cb, modo = 'ESPERA', k = 3, media = 0, sd = 0, listo = false, piso = 0, pasiva = 0, refCal = 0, maxSes = 0;
  let tCal = 0, durCal = 0, muestras = [], t0 = 0, tFin = 0, haySesion = false, cuenta = 0;
  let n = 0, nivel = 0, rondas = 0, activo = false, sobre = 0, ultimaAct = 0, celebra = 0, acts = [], tAct = 0, sumaAct = 0, nAct = 0;
  let invitando = false, tInv = 0, bajadaInv = 0, nInv = 0, nResp = 0, sumaResp = 0;
  let inv = 8, bajar = 1, familia = 0, vol = 12, levanta = 0, proxLev = 0;
  const randn = () => Math.sqrt(-2 * Math.log(1 - Math.random())) * Math.cos(2 * Math.PI * Math.random());
  const salir = o => cb(JSON.stringify(o));
  const msg = txt => salir({ tipo: 'msg', txt });
  const alto = () => Math.max(media + k * Math.max(sd, 0.05 * media), piso);
  const bajo = () => media + (alto() - media) / 2;
  const tSes = t => haySesion ? ((modo === 'SESION' ? t : tFin) - t0) / 1000 : 0;
  const prom = a => a.length ? a.reduce((x, y) => x + y, 0) / a.length : 0;
  function senal(t) {
    if ((modo === 'SESION' || modo === 'CAL_REF') && !levanta && t > proxLev) levanta = t + 600 + Math.random() * 1400;
    if (levanta && t > levanta) { levanta = 0; proxLev = t + 2500 + Math.random() * (Math.random() < 0.3 ? 14000 : 6000); }
    let v = levanta ? 0.22 + 0.04 * randn() : 0.04 + 0.01 * randn();
    if (modo === 'CAL_PASIVA') v += 0.025 * Math.abs(Math.sin(t / 250));
    return Math.max(0.005, v);
  }
  function sesion(t, v) {
    maxSes = Math.max(maxSes, v);
    if (celebra) {
      if (t > celebra) { celebra = 0; rondas++; nivel = 0; ultimaAct = t; msg('Ronda ' + rondas + ' completa: las estrellas bajan a cero.'); }
      return;
    }
    if (!activo) {
      if (v <= alto()) sobre = 0;
      else if (++sobre >= 2) {
        activo = true; sobre = 0; n++; tAct = t - 50; sumaAct = 0; nAct = 0;
        if (invitando) { invitando = false; nResp++; sumaResp += (t - tInv) / 1000; }
        if (nivel < 5 && ++nivel === 5) celebra = t + 8000;   // suben, celebran 5 s y bajan
      }
    } else if (v < bajo()) {
      activo = false; ultimaAct = t;
      acts.push({ t0: (tAct - t0) / 1000, dur: (t - tAct) / 1000, media: sumaAct / Math.max(nAct, 1) });
    } else { sumaAct += v; nAct++; }
    if (!activo && !invitando && t - ultimaAct > inv * 1000) { invitando = true; tInv = bajadaInv = t; nInv++; }
    if (invitando && bajar && nivel > 0 && t - bajadaInv > 10000) { bajadaInv = t; nivel--; }
  }
  function finCal() {
    const m = modo;
    modo = 'ESPERA';
    if (m === 'CAL_REPOSO') {
      const r = muestras.slice(20);
      media = prom(r); sd = Math.sqrt(r.reduce((a, b) => a + (b - media) ** 2, 0) / (r.length - 1));
      listo = true; piso = 0; pasiva = 0; refCal = 0;
      msg('Reposo listo: media ' + media.toFixed(4) + ', SD ' + sd.toFixed(4) + '. Umbral alto ' + alto().toFixed(4) + ', bajo ' + bajo().toFixed(4) + ' (k = ' + k.toFixed(1) + ').');
    } else if (m === 'CAL_PASIVA') {
      piso = Math.max(...muestras) * 1.05; pasiva = 1;
      msg('Pasiva: el umbral alto queda en ' + alto().toFixed(4) + '.');
    } else {
      const mx = Math.max(...muestras); refCal = mx > alto() ? mx : 0;
      msg(refCal ? 'Referencia lista: 100 % = ' + refCal.toFixed(4) + '.' : 'No hubo activación clara: el 100 % será el RMS más alto de la sesión.');
    }
    info();
  }
  function info() {
    const t = performance.now(), d = tSes(t), r = refCal || maxSes, cal = modo.indexOf('CAL_') === 0;
    salir({ tipo: 'info', modo, senal: 1, est: 'OK', t: Math.floor(d), n, nivel, rondas, k, cal: listo ? 1 : 0, media, sd,
      cv: media ? Math.round(100 * sd / media) : 0, pasiva, piso, alto: listo ? alto() : 0, bajo: listo ? bajo() : 0, ref: r,
      refFuente: refCal ? 'calibracion' : 'sesion', lpm: d >= 1 ? n * 60 / d : 0, amp: r ? Math.round(100 * prom(acts.map(a => a.media)) / r) : 0,
      dur: prom(acts.map(a => a.dur)), resp: nResp ? sumaResp / nResp : -1, nInv, nResp, inv, bajar, familia, vol,
      calRestan: cal ? Math.max(0, Math.ceil((durCal - (t - tCal)) / 1000)) : 0, calTotal: cal ? durCal / 1000 : 0, lps: 20, malas: 0, csv: haySesion ? 1 : 0 });
  }
  function paso() {
    const t = performance.now(), v = senal(t);
    if (modo.indexOf('CAL_') === 0) { muestras.push(v); if (t - tCal >= durCal) finCal(); }
    else if (modo === 'SESION') sesion(t, v);
    const estado = modo !== 'SESION' ? modo : celebra ? 'CELEBRA' : activo ? 'ACTIVO' : invitando ? 'INVITACION' : 'BASAL';
    salir({ rms: +v.toFixed(4), alto: listo ? +alto().toFixed(4) : 0, bajo: listo ? +bajo().toFixed(4) : 0, estado, n, nivel, t: Math.floor(tSes(t)) });
    if (++cuenta % 20 === 0) info();
  }
  function comando(o) {
    const t = performance.now(), c = o.cmd;
    if (c.indexOf('cal_') === 0) {
      if (modo !== 'ESPERA') return msg('Para calibrar, primero termina la sesión.');
      if (c !== 'cal_reposo' && !listo) return msg('Primero calibra el REPOSO.');
      modo = { cal_reposo: 'CAL_REPOSO', cal_pasiva: 'CAL_PASIVA', cal_ref: 'CAL_REF' }[c];
      durCal = { CAL_REPOSO: 15000, CAL_PASIVA: 10000, CAL_REF: 45000 }[modo];
      tCal = t; muestras = [];
      msg('Calibrando ' + modo.slice(4).toLowerCase() + ' ' + durCal / 1000 + ' s.');
    } else if (c === 'cancelar' && modo.indexOf('CAL_') === 0) { modo = 'ESPERA'; msg('Calibración cancelada.'); }
    else if (c === 'iniciar' && modo === 'ESPERA') {
      if (!listo) return msg('Primero calibra el REPOSO.');
      modo = 'SESION'; haySesion = true; t0 = ultimaAct = t;
      n = nivel = rondas = nInv = nResp = sumaResp = maxSes = celebra = 0; acts = []; invitando = activo = false;
      msg('Sesión iniciada: umbral alto ' + alto().toFixed(4) + ', bajo ' + bajo().toFixed(4) + ' (k = ' + k.toFixed(1) + ').');
    } else if (c === 'terminar' && modo === 'SESION') {
      modo = 'ESPERA'; tFin = t; nivel = 0; activo = invitando = false; celebra = 0;
      msg('Sesión terminada: ' + n + ' levantamientos. Ya puedes descargar el resumen.');
    } else if (c === 'sens') k = Math.min(6, Math.max(1.5, o.k));
    else if (c === 'inv') inv = Math.min(120, Math.max(3, o.s));
    else if (c === 'bajar') bajar = o.on ? 1 : 0;
    else if (c === 'familia') familia = o.on ? 1 : 0;
    else if (c === 'vol') vol = Math.min(30, Math.max(0, o.v));
    info();
  }
  function csv(nombre, fecha) {
    const filas = ['ASTRA - Resumen de la sesión (SIMULADO)', 'Paciente,' + nombre.replace(/[,"\r\n]/g, ' '), 'Fecha,' + fecha,
      'Levantamientos,' + n, 'Rondas completas,' + rondas, '', 'Activación,Inicio (s),Duración (s),RMS medio']
      .concat(acts.map((a, i) => [i + 1, a.t0.toFixed(2), a.dur.toFixed(2), a.media.toFixed(4)].join(',')));
    const url = URL.createObjectURL(new Blob(['﻿' + filas.join('\r\n')], { type: 'text/csv;charset=utf-8' }));
    const a = document.createElement('a');
    a.href = url; a.download = 'astra_SIMULADO.csv';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 2000);
  }
  return { iniciar(f) { cb = f; setTimeout(() => msg('Simulación: datos falsos para probar la página.'), 200); setInterval(paso, 50); }, comando, csv };
})();

desconectado();
conectar();
</script>
</body>
</html>
)PAGINA";
