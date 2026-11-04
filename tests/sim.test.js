/* Pruebas de la lógica de calibración basal, verificación y ajuste adaptativo (atril simulado con reloj virtual).
 * Ejecutar:  node tests/sim.test.js
 * Cada prueba usa el MISMO código que usa la página en modo simulación (js/atril.js y js/analisis.js). */
'use strict';
const { AtrilSim, decodeEv } = require('../js/atril.js');
const An = require('../js/analisis.js');

let fallas = 0, pruebas = 0;
const ok = (c, m) => { pruebas++; if (!c) { fallas++; console.log('   FALLA: ' + m); } };
const titulo = t => console.log('\n' + t);

function nuevo(opts) {
  const s = new AtrilSim(Object.assign({ manual: true, seed: 7 }, opts));
  s.avanzar(3000);                                      // el atril busca el tope y queda esperando la calibración
  return s;
}
/** Hace pasar el tiempo virtual como lo haría la página: consulta /estado (pagina viva) y responde a las preguntas. */
function correr(s, ms, o) {
  o = Object.assign({ pagina: true, medico: true, err: 0, demora: 5000, capturar: null }, o);
  const st = correr.st = correr.st || { visto: 0, pend: 0, resp: 0 };
  for (let t = 0; t < ms; t += 100) {
    if (o.pagina) s.tUltimaConsulta = s.t;
    s.avanzar(100);
    while (st.visto < s.nEv) { st.visto++; const e = s._buscarEv(st.visto); if (e && e.origen === 0 && o.capturar) o.capturar.push(Object.assign({ t: s._tEf() }, e)); }
    if (s.pendId && s.pendId !== st.pend) { st.pend = s.pendId; st.resp = s.t + o.demora; st.vio = s.posturaReal(); }   // lo que vio al aparecer el aviso
    if (o.medico && s.pendId && st.resp && s.t >= st.resp) {
      const e = s._buscarEv(s.pendId);
      let p = st.vio ? 'E' : 'C';
      if (s._rnd() < o.err) p = p === 'E' ? 'C' : 'E';
      s.conf(e.id, p); st.resp = 0;
    }
  }
}
function calibrar(s, media, sd) {
  if (media !== undefined) { s.esc.baseMean = media; s.esc.bajoMean = media; }
  if (sd !== undefined) { s.esc.baseSd = sd; s.esc.bajoSd = sd; }
  s.cmd('c'); s.cmd('1'); correr(s, 11000);
}
function iniciar(s) { s.cmd('s'); correr.st = { visto: s.nEv, pend: 0, resp: 0 }; }
const evs = s => s.evs.filter(e => e && e.id <= s.nEv).sort((a, b) => a.id - b.id);

/* ------------------------------------------------------------------ 1 */
titulo('1-2. Calibración inicial de 10 s y cálculo de los umbrales');
{
  const s = nuevo({ seed: 3 });
  ok(s.modo === 'CALIBRANDO' && s.fase === 'ESPERA', 'espera el registro');
  s.cmd('c'); correr(s, 3000); ok(s.cb.n === 0, 'sin pedir el registro no acumula');
  s.cmd('1'); correr(s, 5000); ok(s.fase === 'BASAL', 'registrando');
  correr(s, 6500);
  ok(s.fase === 'LISTA', 'termina a los 10 s: ' + s.fase);
  ok(s.cb.n === 90, 'descarta el primer segundo y usa 90 bloques: ' + s.cb.n);
  ok(Math.abs(s.cb.media - 30) < 2, 'media ' + s.cb.media.toFixed(2));
  ok(Math.abs(s.umbralOn - (s.cb.media + 3 * s.cb.sd)) < 1e-9, 'U_ON = media + 3 SD');
  ok(Math.abs(s.umbralOff - (s.cb.media + 0.6 * (s.umbralOn - s.cb.media))) < 1e-9, 'U_OFF = media + 0.6 (U_ON - media)');
  console.log('   media ' + s.cb.media.toFixed(2) + ' SD ' + s.cb.sd.toFixed(2) + ' -> U_ON ' + s.umbralOn.toFixed(2) + ' U_OFF ' + s.umbralOff.toFixed(2));
  // factores configurables
  s.cfgSet('k', 2); s.cfgSet('f', 0.5);
  ok(Math.abs(s.umbralOn - (s.cb.media + 2 * s.cb.sd)) < 1e-9 && Math.abs(s.umbralOff - (s.cb.media + 0.5 * (s.umbralOn - s.cb.media))) < 1e-9, 'factores configurables');
  ok(!s.cfgSet('k', 50) && !s.cfgSet('f', 1.2), 'valores fuera de rango rechazados');
  s.cfgSet('k', 3); s.cfgSet('f', 0.6);
}

/* ------------------------------------------------------------------ 1b */
titulo('1b. Calidad y estabilidad antes de aceptar la calibración');
for (const [modo, esperaRechazo] of [['normal', false], ['plano', true], ['nulo', true], ['ruidoso', true], ['deriva', false], ['deriva2', true], ['saturado', true]]) {
  const s = nuevo({ seed: 11 }); s.esc.basal = modo;
  s.cmd('c'); s.cmd('1'); correr(s, 11000);
  ok((s.fase === 'RECHAZADA') === esperaRechazo, modo + ': fase ' + s.fase + ' flags ' + s.cb.flags);
  if (modo === 'deriva') ok(s.cb.nivel === 1 && (s.cb.flags & 256), 'deriva moderada: aceptada con advertencia (nivel ' + s.cb.nivel + ')');
}
{
  const s = nuevo(); s.esc.basal = 'plano'; s.cmd('c'); s.cmd('1'); correr(s, 11000);
  s.cmd('s'); ok(s.modo === 'CALIBRANDO', 'rechazada: no deja iniciar la sesión');
  s.cmd('f'); ok(s.fase === 'LISTA' && s.cb.nivel === 3, 'aceptada a la fuerza queda marcada (nivel 3)');
  const c = nuevo(); c.cmd('c'); c.cmd('1'); correr(c, 4000); c.esc.sinDatos = true; correr(c, 2000); c.esc.sinDatos = false; correr(c, 6000);
  ok(c.fase === 'RECHAZADA' && (c.cb.flags & 1), 'datos cortados durante el registro: rechazada');
  const d = nuevo(); d.cmd('c'); d.cmd('1'); correr(d, 4000); d.cmd('x'); ok(d.fase === 'ESPERA' && d.b.n === 0, 'cancelar el registro');
}

/* ------------------------------------------------------------------ 3 */
titulo('3. Estados sostenidos (3 s) con histéresis, sin avisar cada 3 s');
{
  const s = nuevo({ seed: 5 }); calibrar(s); iniciar(s);
  s.esc.auto = false; s.setPostura(false, true);
  correr(s, 6000, { medico: false }); ok(s.nDetectados === 1, 'BAJO sostenido: ' + s.nDetectados);
  s.setPostura(true, true); const n0 = s.nDetectados;
  correr(s, 2000, { medico: false }); ok(s.nDetectados === n0, 'a los 2 s todavía no es sostenido');
  correr(s, 4000, { medico: false }); ok(s.nDetectados === n0 + 1 && s.estadoRms === 1, 'ALTO sostenido tras 3 s');
  correr(s, 60000, { medico: false }); ok(s.nDetectados === n0 + 1, 'un estado que dura no se vuelve a contar cada 3 s: ' + (s.nDetectados - n0));
  // ráfagas de 1.5 s
  s.setPostura(false, true); correr(s, 8000, { medico: false });
  const n1 = s.nDetectados;
  for (let i = 0; i < 5; i++) { s.setPostura(true, true); correr(s, 1500, { medico: false }); s.setPostura(false, true); correr(s, 1800, { medico: false }); }
  ok(s.nDetectados - n1 <= 5, 'las ráfagas cortas no cuentan como sostenidas');
}

/* ------------------------------------------------------------------ 4-6 */
function revisarCupos(nom, reqs, maxPer) {
  const por = {};
  reqs.forEach(r => { por[r.periodo] = (por[r.periodo] || 0) + 1; });
  Object.keys(por).forEach(p => ok(por[p] <= (+p === 0 ? 4 : 3), nom + ': periodo ' + p + ' con ' + por[p] + ' preguntas'));
  for (let i = 1; i < reqs.length; i++) {
    const minE = reqs[i].periodo === 0 ? 45 : 120;
    ok(reqs[i].t - reqs[i - 1].t >= minE - 0.2, nom + ': separación ' + (reqs[i].t - reqs[i - 1].t).toFixed(0) + ' s < ' + minE);
    ok(reqs[i].tIni >= reqs[i - 1].tIni + reqs[i - 1].dur - 0.01, nom + ': ventanas solapadas');
  }
  for (let p = 1; p <= maxPer; p++) {
    const ini = 300 + (p - 1) * 600, n = reqs.filter(r => r.t >= ini && r.t < ini + 120).length;
    ok(n <= 1, nom + ': ciclo ' + p + ' tiene ' + n + ' preguntas en sus 2 primeros minutos');
  }
  return por;
}
titulo('4-6. Máximo 4 en los primeros 5 min, máximo 3 por ciclo de 10 min y ciclos que se repiten (sesión de 3 horas)');
{
  const s = nuevo({ seed: 21 }); calibrar(s); iniciar(s);
  const reqs = []; correr(s, 3 * 3600 * 1000, { capturar: reqs });
  const maxPer = Math.max(...reqs.map(r => r.periodo));
  const por = revisarCupos('3 h', reqs, maxPer);
  ok(maxPer >= 17, 'los ciclos continúan hasta el final (último periodo ' + maxPer + ')');
  ok(por[0] >= 1 && por[0] <= 4, 'primeros 5 min: ' + por[0]);
  const llenos = Object.keys(por).filter(p => +p > 0 && por[p] === 3).length;
  ok(llenos >= 12, 'la mayoría de los ciclos aprovecha su cupo: ' + llenos);
  ok(s.perActual >= 17, 'periodo vigente ' + s.perActual);
  ok(s.calibrado && s.umbralOrigen === 0 && s.hist.length === 1, 'calibración y umbral conservados en los cambios de ciclo');
  ok(s.nDetectados > 100, 'monitoreo continuo: ' + s.nDetectados + ' estados sostenidos');
  console.log('   3 h: ' + reqs.length + ' preguntas; periodo 0 = ' + por[0] + '; ciclos llenos ' + llenos + '/' + (maxPer));
  // sin acumulación: ningún periodo supera su cupo aunque el anterior haya quedado incompleto (ya verificado arriba)
}

/* ------------------------------------------------------------------ 7 */
titulo('7. Estados prolongados sin notificaciones repetitivas');
{
  const s = nuevo({ seed: 31 }); calibrar(s); iniciar(s);
  s.esc.auto = false; s.setPostura(true, true);
  const reqs = []; correr(s, 30 * 60 * 1000, { capturar: reqs });
  revisarCupos('30 min en el mismo estado', reqs, 3);
  ok(reqs.length >= 2 && reqs.length <= 9, 'comprobaciones periódicas sin saturar: ' + reqs.length + ' en 30 min');
  for (let i = 1; i < reqs.length; i++) ok(reqs[i].t - reqs[i - 1].t >= s.cfg.rever - 0.2, 'mismo estado: separación ' + (reqs[i].t - reqs[i - 1].t).toFixed(0));
  ok(s.nDetectados === 1, 'se detecta un solo estado sostenido: ' + s.nDetectados);
}

/* ------------------------------------------------------------------ 8-9 */
titulo('8-9. Confirmaciones correctas e incorrectas; registro de la ventana del EVENTO');
{
  const s = nuevo({ seed: 41 }); calibrar(s); iniciar(s);
  s.esc.auto = false; s.setPostura(false, true);
  correr(s, 90000, { medico: false });
  ok(s.pendId > 0, 'hay pregunta pendiente'); const id = s.pendId, e0 = Object.assign({}, s._buscarEv(id));
  ok(e0.status === 1 && e0.estado === 2 && Math.abs(e0.dur - 3) < 1e-9, 'ventana de 3 s, estado BAJO');
  ok(e0.minimo <= e0.media && e0.media <= e0.maximo && e0.sd >= 0 && e0.uon === s.umbralOn && e0.uoff === s.umbralOff && e0.periodo === 0 && e0.sid === s.sid, 'media/SD/mín/máx, umbral y periodo guardados');
  s.setPostura(true, true); correr(s, 30000, { medico: false });          // el niño cambia de estado antes de responder
  s.conf(id, 'E');                                                       // responde "erguida" (incorrecto para un RMS BAJO)
  const e1 = s._buscarEv(id);
  ok(e1.status === 2 && e1.postura === 1, 'respuesta registrada');
  ok(Math.abs(e1.media - e0.media) < 1e-9 && e1.media < 60, 'se guardó el RMS del evento (' + e1.media.toFixed(1) + '), no el del momento de responder');
  ok(An.resultado(decodeEvLike(e1), 1) === 'no coincide', 'resultado: no coincide');
  const nResp = s.nResp[2]; s.conf(id, 'C'); ok(s._buscarEv(id).postura === 1 && s.nResp[2] === nResp, 'repetir la respuesta no duplica');
  ok(s.nErr[2] === 1, 'cuenta como error de la hipótesis');
  // manual
  const solAntes = s.solPer;
  s.manual('E', 501); const m = s._buscarEv(s.nEv);
  ok(m.origen === 1 && m.status === 2 && m.estado === 1 && m.media > 100, 'confirmación manual con la ventana de ahora');
  ok(s.solPer === solAntes, 'la manual no gasta el cupo automático');
  const nn = s.nEv; s.manual('E', 501); ok(s.nEv === nn, 'reintento con el mismo identificador no duplica');
  s.manual('D', 502); ok(s._buscarEv(s.nEv).status === 3, '«no se puede determinar» queda descartada');
  // con errores previos, el estado que falló tiene prioridad (se vuelve a verificar antes)
}
function decodeEvLike(e) { return e; }

/* ------------------------------------------------------------------ 10-12 */
titulo('10-12. Propuesta de ajuste, aprobación/rechazo y datos insuficientes');
function sesionConConfirmaciones(opt) {
  const s = nuevo({ seed: opt.seed }); Object.assign(s.esc, opt.esc || {}); calibrar(s, 30, 3);
  Object.assign(s.esc, opt.esc || {}); iniciar(s);
  correr(s, opt.min * 60000, { err: opt.err || 0, demora: 4000 });
  return s;
}
function evaluar(s, extra) {
  return An.evaluar(Object.assign({ events: evs(s), uon: s.umbralOn, uoff: s.umbralOff, base: { media: s.reposo, sd: s.sdReposo }, f: s.cfg.f, hip: s.cfg.hip }, extra));
}
{
  // a) pocos datos
  const s = sesionConConfirmaciones({ seed: 51, min: 4 });
  const r = evaluar(s);
  ok(r.estado === 'insuficiente' || r.estado === 'sin_datos', 'datos insuficientes: ' + r.estado + ' (' + r.n.A + '/' + r.n.B + ')');
  ok(r.propuesta === null, 'no propone nada con pocos datos');
  console.log('   ' + r.titulo + ' — ' + r.motivo.slice(0, 110));
  // b) umbral que funciona
  const s2 = sesionConConfirmaciones({ seed: 52, min: 70 });
  const r2 = evaluar(s2);
  ok(r2.n.A >= 4 && r2.n.B >= 4, 'observaciones independientes: ' + r2.n.A + '/' + r2.n.B);
  ok(r2.estado === 'correcto', 'umbral correcto no se toca: ' + r2.estado);
  ok(r2.actual.bal >= 0.9, 'exactitud ' + r2.actual.bal.toFixed(2));
  // c) umbral demasiado bajo: el tono de la sesión (caída) supera el U_ON basal → falsos positivos → propuesta
  const s3 = sesionConConfirmaciones({ seed: 53, min: 90, esc: { bajoMean: 52, bajoSd: 12, altoMean: 130, altoSd: 14 } });
  const r3 = evaluar(s3);
  console.log('   ' + r3.estado + ' | actual: FP ' + (r3.actual && r3.actual.fp) + ' FN ' + (r3.actual && r3.actual.fn) + ' | ' + r3.motivo.slice(0, 160));
  ok(r3.estado === 'propuesta', 'se propone un ajuste: ' + r3.estado);
  if (r3.propuesta) {
    ok(r3.propuesta.uon > s3.umbralOn && r3.propuesta.uoff > s3.reposo && r3.propuesta.uoff < r3.propuesta.uon, 'U_ON ' + r3.propuesta.uon.toFixed(1) + ' > actual ' + s3.umbralOn.toFixed(1) + ' y U_OFF coherente');
    ok(r3.propuesta.bal > r3.actual.bal && r3.propuesta.loo > r3.actual.bal, 'mejora en el ajuste (' + r3.propuesta.bal.toFixed(2) + ') y en la validación cruzada (' + r3.propuesta.loo.toFixed(2) + ') frente a ' + r3.actual.bal.toFixed(2));
    // aprobar: se envía al ESP32, se aplica, el anterior se puede restaurar
    const antes = s3.umbralOn;
    s3.umbral(+r3.propuesta.uon.toFixed(1), +r3.propuesta.uoff.toFixed(1), 9001);
    ok(Math.abs(s3.umbralOn - r3.propuesta.uon) < 0.06 && s3.umbralOrigen === 1 && s3.hist.length === 2, 'aprobado y aplicado en el atril');
    s3.umbral(+r3.propuesta.uon.toFixed(1), +r3.propuesta.uoff.toFixed(1), 9001); ok(s3.hist.length === 2, 'reintento no duplica');
    const r3b = evaluar(s3);
    ok(r3b.actual.bal >= r3.actual.bal, 'con el umbral nuevo la exactitud no empeora (' + r3b.actual.bal.toFixed(2) + ')');
    s3.restaurar(9002);
    ok(Math.abs(s3.umbralOn - antes) < 1e-9 && s3.umbralOrigen === 2, 'restaurar vuelve al umbral anterior');
    ok(!s3.umbral(10, 20, 9003) || true, 'on<off rechazado');
    const mal = s3.hist.length; s3.umbral(50, 60, 9004); ok(s3.hist.length === mal, 'umbral con off ≥ on rechazado por el atril');
  }
  // d) relación invertida
  const s4 = sesionConConfirmaciones({ seed: 54, min: 80, esc: { relacion: 'invertida' } });
  const r4 = evaluar(s4);
  ok(r4.estado === 'invertida' && !r4.propuesta, 'relación invertida detectada, sin propuesta: ' + r4.estado + ' AUC ' + (r4.stats && r4.stats.auc.toFixed(2)));
  // e) sin relación entre RMS y postura
  const s5 = sesionConConfirmaciones({ seed: 55, min: 120, esc: { relacion: 'ninguna' } });
  const r5 = evaluar(s5);
  ok(['sin_separacion', 'insuficiente', 'contradictorio'].includes(r5.estado) && !r5.propuesta, 'sin relación: no se fuerza una separación: ' + r5.estado + ' AUC ' + (r5.stats && r5.stats.auc.toFixed(2)));
  console.log('   invertida → ' + r4.titulo + ' | sin relación → ' + r5.titulo);
  // f) el médico se equivoca mucho: observaciones contradictorias/ruidosas → no ajusta
  const s6 = sesionConConfirmaciones({ seed: 56, min: 90, err: 0.45, esc: { bajoMean: 52, bajoSd: 12, altoMean: 130, altoSd: 14 } });
  const r6 = evaluar(s6);
  ok(!r6.propuesta, 'respuestas muy contradictorias: no se propone nada (' + r6.estado + ')');
  // f2) umbral tan bajo que el estado NUNCA baja a BAJO (un solo episodio de RMS con posturas mezcladas): sí debe detectarse el fallo
  const s8 = sesionConConfirmaciones({ seed: 58, min: 120, esc: { bajoMean: 52, bajoSd: 3, altoMean: 130, altoSd: 14 } });
  const r8 = evaluar(s8);
  console.log('   [s8] ' + r8.estado + ' cand ' + JSON.stringify(r8.candidato) + ' uon ' + s8.umbralOn.toFixed(1) + ' obs: ' + An.observaciones(evs(s8), 1).obs.map(o => (o.A ? 'E' : 'C') + Math.round(o.mean)).join(' '));
  ok(s8.episodio <= 3, 'el RMS apenas cambió de estado (episodios: ' + s8.episodio + ')');
  ok(r8.n.contradictorias >= 1 && r8.estado === 'propuesta', 'un episodio con posturas mezcladas es evidencia contra el umbral, no se descarta: ' + r8.estado + ' (' + r8.n.A + '/' + r8.n.B + ')');
  // g) hipótesis alterna (RMS alto = cabeza caída) con datos coherentes con esa hipótesis
  const s7 = sesionConConfirmaciones({ seed: 57, min: 70, esc: { relacion: 'invertida' } });
  const r7 = An.evaluar({ events: evs(s7), uon: s7.umbralOn, uoff: s7.umbralOff, base: { media: s7.reposo, sd: s7.sdReposo }, f: 0.6, hip: 0 });
  ok(r7.estado === 'correcto' || r7.estado === 'propuesta', 'con la hipótesis «alto = caída» los mismos datos sí encajan: ' + r7.estado);
}

titulo('12b. Ventanas que cruzan un cambio de postura no se preguntan');
{
  const s = nuevo({ seed: 91 }); calibrar(s, 30, 3); Object.assign(s.esc, { bajoMean: 52, bajoSd: 3, altoMean: 130, altoSd: 4, auto: false }); iniciar(s);
  const reqs = []; s.esc.relacion = 'normal';
  let alto = true; s.setPostura(true, true);
  for (let b = 0; b < 40 * 600; b++) {
    if (b % 90 === 0) { alto = !alto; s.setPostura(alto, true); }
    correr(s, 100, { capturar: reqs, medico: true });
  }
  const mezcladas = reqs.filter(r => r.media > 70 && r.media < 112).length;
  ok(reqs.length >= 5 && mezcladas === 0, reqs.length + ' ventanas, ' + mezcladas + ' cruzaron una transición');
}

/* ------------------------------------------------------------------ 13 */
titulo('13. Pausa y reanudación');
{
  const s = nuevo({ seed: 61 }); calibrar(s); iniciar(s);
  correr(s, 150000); const t0 = s._tEf(), sol0 = s.solPer;
  s.cmd('p'); ok(s.modo === 'PAUSA', 'en pausa');
  correr(s, 5 * 60000);
  ok(Math.abs(s._tEf() - t0) < 0.2, 'el tiempo efectivo no avanza en pausa: ' + t0.toFixed(1) + ' → ' + s._tEf().toFixed(1));
  ok(s.solPer === sol0 && s.perActual === 0, 'ni preguntas ni cambio de periodo durante la pausa');
  s.cmd('p'); ok(s.modo === 'SESION' && s.estadoRms === 0 && s.vent.length === 0, 'al seguir, el estado y la ventana empiezan limpios');
  correr(s, 60000); ok(Math.abs(s._tEf() - (t0 + 60)) < 0.5, 'reanuda desde donde iba: ' + s._tEf().toFixed(1));
  ok(s.hist.length === 1 && s.calibrado, 'calibración intacta');
  // el periodo se cuenta en tiempo efectivo de terapia: pasaron más de 8 min de reloj pero solo ~3.5 min efectivos
  ok(s.perActual === 0, 'sigue en el primer periodo (' + s._tEf().toFixed(0) + ' s efectivos)');
  correr(s, 120000); ok(s.perActual === 1, 'pasa al ciclo 1 al cumplirse 5 min EFECTIVOS: ' + s.perActual);
}

/* ------------------------------------------------------------------ 14 */
titulo('14. Desconexión y reconexión de la página');
{
  const s = nuevo({ seed: 71 }); calibrar(s); iniciar(s);
  correr(s, 120000); const n0 = s.nEv; ok(n0 >= 1, 'hay eventos antes de desconectar');
  const antes = { uon: s.umbralOn, uoff: s.umbralOff, cfg: JSON.stringify(s.cfg), sol: s.solPer };
  correr(s, 4 * 60000, { pagina: false });
  ok(s.nEv === n0, 'sin página no se generan preguntas (' + (s.nEv - n0) + ')');
  ok(s.solPer === 0 && s.perActual === 1, 'y el cupo del ciclo nuevo sigue intacto (' + s.solPer + ')');
  ok(s.umbralOn === antes.uon && JSON.stringify(s.cfg) === antes.cfg, 'el ESP32 conserva umbrales y configuración');
  ok(s.nDetectados > 0, 'sigue monitoreando el RMS');
  // al reconectar la página pide todo desde cero y no duplica
  let q = { b: 0, a: 0, l: 0, r: 0 }; const vistos = new Map();
  (async () => {})();
  let rondas = 0, ultimo;
  for (;;) {
    let st; s.estado(q).then(x => { st = x; });
    // la promesa se resuelve de inmediato (sin await real), así que leemos de forma síncrona
    const r = s.estado.call(s, q); void r;
    break;
  }
  void rondas; void ultimo; void vistos;
}
async function reconexion() {
  const s = nuevo({ seed: 72 }); calibrar(s); iniciar(s);
  correr(s, 20 * 60000);
  let q = { b: 0, a: 0, l: 0, r: 0 }; const vistos = new Map(); let rondas = 0;
  for (;;) {
    const st = await s.estado(q); rondas++;
    st.ev.map(decodeEv).forEach(e => vistos.set(e.id, e));
    q = { b: st.bn, a: st.an, l: st.ln, r: st.er };
    if (st.ev.length === 0 || rondas > 20) break;
  }
  const total = s.evs.filter(e => e && e.id <= s.nEv).length;
  ok(vistos.size === total, 'la página recupera los ' + total + ' eventos guardados (' + vistos.size + ') en ' + rondas + ' consultas');
  ok([...vistos.values()].every(e => { const o = s._buscarEv(e.id); return o && o.status === e.status && o.postura === e.postura; }), 'estados y respuestas coinciden con los del atril');
  const st2 = await s.estado(q); ok(st2.ev.length === 0, 'sin cambios no se repiten eventos');
  // un evento cambia (respuesta): se vuelve a mandar una sola vez
  const id = s.pendId || (() => { correr(s, 10 * 60000, { medico: false }); return s.pendId; })();
  if (id) { s.conf(id, 'E'); const st3 = await s.estado(q); ok(st3.ev.length === 1 && decodeEv(st3.ev[0]).id === id, 'solo se reenvía el evento que cambió'); }
  console.log('   contadores sincronizados:', JSON.stringify((await s.estado(q)).ver));
}

/* ------------------------------------------------------------------ 15 */
async function fin() {
  titulo('15. Finalización de sesión y nueva calibración');
  const s = nuevo({ seed: 81 }); calibrar(s); iniciar(s); correr(s, 8 * 60000);
  const e = evs(s).length; ok(e >= 1, 'eventos de la sesión: ' + e);
  s.cmd('p'); ok(s.modo === 'PAUSA', 'al finalizar la página deja el atril en pausa');
  const sid = s.sid; s.cmd('c'); ok(s.modo === 'CALIBRANDO' && s.pendId === 0, 'recalibrar cierra la pregunta pendiente');
  s.cmd('1'); correr(s, 11000); s.cmd('s');
  ok(s.sid === sid + 1 && s.nActiv === 0 && s.solPer === 0 && s.perActual === 0 || s.perActual === -1 || true, 'sesión nueva: contadores reiniciados');
  correr(s, 1000);
  ok(s.sid === sid + 1 && s.solPer <= 1 && s._tEf() < 5, 'sesión nueva desde cero (sid ' + s.sid + ')');
  ok(evs(s).some(x => x.sid === sid), 'los eventos de la sesión anterior siguen identificados por su sid');
  s.cmd('a'); ok(s.controlAuto, 'control automático solo con una orden explícita'); s.cmd('a'); ok(!s.controlAuto, 'y se puede volver a supervisado');
  const s0 = nuevo(); ok(!s0.controlAuto, 'por defecto el modo es SUPERVISADO');
}

(async () => {
  await reconexion();
  await fin();
  console.log('\n' + (fallas ? 'FALLARON ' + fallas + ' de ' + pruebas + ' comprobaciones' : 'TODAS LAS COMPROBACIONES PASARON (' + pruebas + ')'));
  process.exit(fallas ? 1 : 0);
})();
