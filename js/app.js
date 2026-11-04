/* Interfaz: navegación, pacientes, sesión en vivo (Arduino + atril), verificación, historial, ajustes.
 * Quién hace qué:
 *   Arduino UNO R4 WiFi -> procesa la señal y calcula el RMS (la señal cruda solo se dibuja aquí)
 *   Atril ESP32         -> calibración basal, umbrales, estados sostenidos, contadores y temporizadores de
 *                          verificación, y los actuadores. Conserva todo aunque la página se desconecte.
 *   Página              -> muestra todo, pregunta a la fisioterapeuta, analiza las confirmaciones, propone
 *                          ajustes y manda al atril solo lo que la fisioterapeuta aprueba.
 */
(function () {
  'use strict';
  const E = window.EMG;
  const $ = id => document.getElementById(id);
  const norm = s => (s || '').normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase();
  const fullName = p => (p.nombres + ' ' + p.apellidos).trim();
  const pad = n => String(n).padStart(2, '0');
  const clock = s => { s = Math.max(0, Math.floor(s)); return pad(Math.floor(s / 60)) + ':' + pad(s % 60); };
  const fmtDate = iso => { const d = new Date(iso); return d.toLocaleDateString('es-MX', { day: '2-digit', month: 'short', year: 'numeric' }); };
  const fmtDateTime = iso => fmtDate(iso) + ' ' + pad(new Date(iso).getHours()) + ':' + pad(new Date(iso).getMinutes());
  const f0 = v => (isFinite(v) ? Math.round(v).toString() : '—');
  const f1 = v => (isFinite(v) ? (+v).toFixed(1) : '—');
  const f2 = v => (isFinite(v) ? (+v).toFixed(2) : '—');
  function ageText(iso) {
    const b = new Date(iso + 'T00:00:00'), n = new Date();
    let m = (n.getFullYear() - b.getFullYear()) * 12 + n.getMonth() - b.getMonth();
    if (n.getDate() < b.getDate()) m--;
    if (m < 0 || isNaN(m)) return '';
    return m >= 24 ? Math.floor(m / 12) + ' años' + (m % 12 ? ' ' + (m % 12) + ' m' : '') : m + ' meses';
  }
  let toastTimer;
  function toast(msg) {
    const t = $('toast'); t.textContent = msg; t.hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, 3500);
  }
  const setText = (id, t) => { const el = $(id); if (el.textContent !== t) el.textContent = t; };

  /* ---------- estado global ---------- */
  let S = E.Store.settings();
  let proc, quality, chart, rmsChart, progChart;
  let view = 'home', patient = null;
  let emg = null, emgConn = { status: 'disconnected', msg: '' };       // señal del Arduino
  let atril = null, atrilConn = { status: 'disconnected', msg: '' };   // ESP32 (o simulado)
  let sim = null;                    // AtrilSim cuando se usa la simulación
  let st = null;                     // último estado del atril (/estado)
  let calPreparada = false;          // se pulsó «Comenzar calibración» en esta sesión
  let rec = null;                    // registro de la terapia en curso (lo que se guarda al finalizar)
  let logLines = [], logDirty = false;
  const evStore = new Map();         // eventos de verificación: clave boot:id -> evento (+ visto)
  const tareas = [];                 // comandos con reintento (idempotentes en el atril)
  const respondidos = new Set();     // preguntas que ya se contestaron (el aviso se oculta enseguida)
  let propuesta = null, rechazadas = [], ajustesLog = [];
  let analisis = null, analisisSig = '';
  const rx = { lastData: 0, samples: 0, lastCount: 0, lastT: performance.now(), fs: 0, lostSeen: 0, lostAt: 0, connectedAt: 0, pulse: false };
  const qCount = { sat: 0, flat: 0, noisy: 0 }; let qLast = null;
  let prevCrit = new Set();
  const auto = { on: false, err: 0, demora: 4, visto: 0 };      // fisioterapeuta simulada (solo en simulación)

  const evKey = (boot, id) => boot + ':' + id;
  const midiendo = s => !!s && s.modo === 'CALIBRANDO' && s.fase === 'BASAL';
  const nextTok = (() => { let n = (Date.now() % 1000000000) + 1; return () => ++n; })();

  function rebuildDsp() {
    proc = new E.Processor(S);
    quality = new E.SignalQuality(S.adcMax);
    if (chart && chart.fs !== S.fs) { chart.fs = S.fs; chart.setWindow(S.winS); }
  }

  /* ---------- navegación ---------- */
  function show(name) {
    view = name;
    document.querySelectorAll('.view').forEach(v => { v.hidden = v.id !== 'view-' + name; });
    window.scrollTo(0, 0);
    if (name === 'home') renderHome();
    if (name === 'search') renderSearch();
    if (name === 'history') renderHistory();
    if (name === 'session') { chart.setWindow(S.winS); rmsChart.setWindow(S.winS); renderCal(); }
    if (name !== 'session') $('notif').hidden = true;
  }
  function goHome() {
    if (view === 'session' && rec && !confirm('La terapia sigue en curso. ¿Salir sin finalizar? Los datos de esta sesión se perderán.')) return;
    if (view === 'session') { rec = null; calPreparada = false; }
    show('home');
  }
  document.querySelectorAll('[data-nav]').forEach(b => b.addEventListener('click', () => show(b.dataset.nav)));
  $('btn-home').addEventListener('click', goHome);
  $('go-register').addEventListener('click', () => { $('form-register').reset(); show('register'); });
  $('go-search').addEventListener('click', () => show('search'));

  function renderHome() {
    const p = E.Store.patients().length, s = E.Store.sessions().length;
    $('home-stats').textContent = p + ' paciente(s) registrado(s) · ' + s + ' sesión(es) guardada(s)';
  }

  /* ---------- registro ---------- */
  $('form-register').addEventListener('submit', e => {
    e.preventDefault();
    const fd = new FormData(e.target), p = {};
    fd.forEach((v, k) => { p[k] = typeof v === 'string' ? v.trim() : v; });
    p.consentimiento = fd.has('consentimiento');
    const dup = E.Store.patients().find(x => norm(fullName(x)) === norm(fullName(p)) && x.nacimiento === p.nacimiento);
    if (dup && !confirm('Ya existe un paciente con el mismo nombre y fecha de nacimiento. ¿Registrar de todas formas?')) return;
    if (!E.Store.savePatient(p)) { toast('No se pudo guardar (almacenamiento lleno o bloqueado).'); return; }
    toast('Paciente guardado'); selected = p; show('search');
  });

  /* ---------- búsqueda ---------- */
  let selected = null;
  function renderSearch() {
    const q = norm($('search-input').value).split(/\s+/).filter(Boolean);
    const list = E.Store.patients().filter(p => q.every(t => norm(fullName(p)).includes(t)))
      .sort((a, b) => fullName(a).localeCompare(fullName(b))).slice(0, 8);
    const ul = $('suggestions'); ul.innerHTML = '';
    list.forEach(p => {
      const li = document.createElement('li');
      li.textContent = fullName(p) + '  ·  ' + fmtDate(p.nacimiento);
      if (selected && selected.id === p.id) li.className = 'sel';
      li.addEventListener('click', () => { selected = p; renderSearch(); });
      ul.appendChild(li);
    });
    if (!list.length && q.length) { const li = document.createElement('li'); li.className = 'muted'; li.textContent = 'Sin resultados'; ul.appendChild(li); }
    const has = !!selected;
    $('pd-empty').hidden = has; $('pd-body').hidden = !has;
    $('btn-start-therapy').disabled = !has; $('btn-history').disabled = !has;
    if (has) {
      const ses = E.Store.sessionsFor(selected.id);
      const rows = [['Nombre completo', fullName(selected)], ['Fecha de nacimiento', fmtDate(selected.nacimiento) + ' (' + ageText(selected.nacimiento) + ')'],
        ['GMFCS', selected.gmfcs], ['Expediente', selected.expediente], ['Diagnóstico', selected.diagnostico],
        ['Sesiones', ses.length ? ses.length + ' (última: ' + fmtDate(ses[ses.length - 1].fecha) + ')' : 'Ninguna']].filter(r => r[1]);
      $('pd-body').innerHTML = rows.map(r => '<dt>' + r[0] + '</dt><dd></dd>').join('');
      $('pd-body').querySelectorAll('dd').forEach((dd, i) => { dd.textContent = rows[i][1]; });
    }
  }
  $('search-input').addEventListener('input', () => { selected = null; renderSearch(); });
  $('search-input').addEventListener('keydown', e => {
    if (e.key === 'Enter') { const li = $('suggestions').querySelector('li:not(.muted)'); if (li) li.click(); }
  });
  $('btn-history').addEventListener('click', () => { patient = selected; show('history'); });
  $('btn-start-therapy').addEventListener('click', () => { patient = selected; startSession(); });

  /* ===================================================================
   * Conexión: Arduino (señal) + atril (estado y comandos)
   * =================================================================== */
  function onEmgStatus(status, msg) {
    emgConn = { status, msg: msg || '' };
    if (status === 'connected') { rx.connectedAt = performance.now(); rx.lastData = performance.now(); rx.lostSeen = 0; qCount.sat = qCount.flat = qCount.noisy = 0; }
    renderConn();
  }
  function onAtrilStatus(status, msg) {
    atrilConn = { status, msg: msg || '' };
    if (status !== 'connected') st = null;
    renderConn(); if (view === 'session') renderCal();
  }
  function emgFlag() {
    if (!st) return 0;
    if (st.modo === 'CALIBRANDO') return st.fase === 'BASAL' ? 1 : 0;
    if (st.modo === 'SESION' && st.sost) return st.est === 1 ? 2 : 3;      // 2 = ALTO sostenido, 3 = BAJO sostenido
    return 0;
  }
  function onSamples(arr) {
    rx.lastData = performance.now(); rx.samples += arr.length; rx.pulse = true;
    const flag = emgFlag();
    for (let i = 0; i < arr.length; i++) {
      const raw = arr[i], x = proc.process(raw);
      quality.feed(raw, proc.hpOut, x);
      chart.push(x, flag);
    }
  }

  async function disconnectAll() {
    const e = emg, a = atril; emg = null; atril = null; sim = null; st = null; tareas.length = 0;
    if (a) a.stop();
    if (e) { if (e.stop) e.stop(); else await e.disconnect(); }
    emgConn = { status: 'disconnected', msg: '' }; atrilConn = { status: 'disconnected', msg: '' };
    $('sim-panel').hidden = true;
    renderConn(); if (view === 'session') renderCal();
  }
  async function connectAstra() {
    if (S.emgUsb && !E.SerialSource.supported()) { alert('Este navegador no soporta Web Serial. Usa Google Chrome o Microsoft Edge en una computadora, o desactiva «Señal EMG por USB» en Ajustes.'); return; }
    await disconnectAll();
    rebuildDsp();
    atril = new E.AtrilLink(new E.AtrilHttp(S.ipAtril), onEstado, onAtrilStatus);
    if (S.emgUsb) {
      emg = new E.SerialSource(onSamples, onEmgStatus);
      try { await emg.connect(); }
      catch (e) { emg = null; onEmgStatus('disconnected'); if (e.name !== 'NotFoundError') toast('No se pudo abrir el puerto: ' + (e.message || e.name) + '. ¿Está abierto el Monitor Serie de Arduino?'); }
    } else {
      emg = new E.WifiSource(onSamples, onEmgStatus, S.ipArduino);
      emg.connect();
    }
  }
  function startSim() {
    disconnectAll();
    rebuildDsp();
    sim = new E.AtrilSim({ speed: +$('sp-speed').value || 1 });
    simPanelSync();
    atril = new E.AtrilLink(sim, onEstado, onAtrilStatus);
    emg = new E.SimSource(arr => onSamples(arr), onEmgStatus, () => sim.simCtx(), S.fs);
    emg.start();
    $('sim-panel').hidden = false;
  }
  const realOn = () => (atril && atril.kind !== 'sim') || (emg && emg.kind !== 'sim');
  $('btn-connect').addEventListener('click', () => (realOn() ? disconnectAll() : connectAstra()));
  $('btn-sim').addEventListener('click', () => (sim ? disconnectAll() : startSim()));
  $('sim-fault').addEventListener('change', e => { if (emg && emg.setFault) emg.setFault(e.target.value || null); });
  if (navigator.serial) navigator.serial.addEventListener('disconnect', () => { if (emg && emg.kind === 'usb') onEmgStatus('error', 'Se desconectó el cable USB'); });

  function sendCmd(c) {
    if (!atril || atrilConn.status !== 'connected') { toast('El atril no está conectado.'); return; }
    atril.cmd(c).then(ok => { if (!ok) toast('El atril no recibió el comando. Revisa la conexión WiFi.'); });
  }
  /* Comando con datos: se reintenta hasta que el estado del atril muestra que se aplicó. Los comandos llevan un
   * identificador, así que un reintento nunca duplica nada en el atril. */
  function tarea(nombre, enviar, hecho, ok) {
    tareas.push({ nombre, enviar, hecho, ok, intentos: 0, t: 0 });
    revisarTareas(true);
  }
  function revisarTareas(ya) {
    if (!tareas.length) return;
    const now = performance.now();
    for (const t of tareas.slice()) {
      if (st && t.hecho(st)) { tareas.splice(tareas.indexOf(t), 1); if (t.ok) t.ok(); continue; }
      if (!atril || atrilConn.status !== 'connected') continue;
      if (ya && t.intentos === 0 || now - t.t > 1500) {
        if (t.intentos >= 8) { tareas.splice(tareas.indexOf(t), 1); toast('No se pudo confirmar en el atril: ' + t.nombre + '.'); continue; }
        t.t = now; t.intentos++; t.enviar();
      }
    }
  }

  function renderConn() {
    const kindName = k => ({ wifi: 'WiFi', usb: 'USB', sim: 'simulación' }[k] || '');
    // EMG
    let chip = $('emg-chip'), t = $('emg-text'), c = emgConn.status;
    chip.className = 'chip';
    if (c === 'connected') {
      const stale = performance.now() - rx.lastData > 1000;
      chip.classList.add(stale ? 'bad' : (rx.fs && Math.abs(rx.fs - S.fs) > S.fs * 0.1 ? 'warn' : 'ok'));
      t.textContent = 'EMG' + (emg ? ' (' + kindName(emg.kind) + ')' : '') + (stale ? ': SIN DATOS' : ': ' + Math.round(rx.fs) + ' Hz');
    } else if (c === 'connecting') { chip.classList.add('warn'); t.textContent = 'EMG: conectando…'; }
    else if (c === 'error') { chip.classList.add('bad'); t.textContent = 'EMG: ' + (emgConn.msg || 'error'); }
    else t.textContent = 'EMG: sin conexión';
    chip.title = 'Señal EMG cruda del Arduino' + (S.emgUsb ? ' (USB)' : ' en http://' + S.ipArduino + '/stream');
    // Atril
    chip = $('atril-chip'); t = $('atril-text'); c = atrilConn.status;
    chip.className = 'chip';
    if (c === 'connected' && st) {
      chip.classList.add(st.modo === 'PARO' ? 'bad' : st.datos ? 'ok' : 'warn');
      t.textContent = 'Atril: ' + st.modo + (st.modo === 'CALIBRANDO' ? ' · ' + st.fase : '') + (st.datos ? '' : ' · sin datos del Arduino');
    } else if (c === 'connecting' || c === 'connected') { chip.classList.add('warn'); t.textContent = 'Atril: conectando…'; }
    else if (c === 'error') { chip.classList.add('bad'); t.textContent = 'Atril: sin respuesta'; }
    else t.textContent = 'Atril: sin conexión';
    chip.title = 'Atril (ESP32) en http://' + S.ipAtril + (atrilConn.msg ? ' — ' + atrilConn.msg : '');
    // Botones
    $('btn-connect').textContent = realOn() ? 'Desconectar' : 'Conectar ASTRA';
    $('btn-sim').textContent = sim ? 'Detener simulación' : 'Simulación';
    $('btn-sim').classList.toggle('on', !!sim);
    $('sim-fault').hidden = !sim; if (!sim) $('sim-fault').value = '';
    if (rx.pulse) { rx.pulse = false; const d = $('emg-dot'); d.classList.remove('pulse'); void d.offsetWidth; d.classList.add('pulse'); }
  }

  /* ---------- lo que llega del atril ---------- */
  function addLog(t) { logLines.push(t); if (logLines.length > 300) logLines.splice(0, logLines.length - 300); logDirty = true; }
  let ultEv = 0;       // para detectar si cambió algo que haya que dibujar
  function onEstado(s) {
    const prev = st; st = s;
    if (s.reinicio) { addLog('--- el atril se reinició ---'); toast('El atril (ESP32) se reinició.'); evStore.clear(); respondidos.clear(); }
    for (const [v, f] of s.b) rmsChart.push(v, f);
    const aplicado = s.calibrado && (s.modo === 'SESION' || s.modo === 'PAUSA' || s.fase === 'LISTA');
    rmsChart.lines = { reposo: aplicado ? s.cb.media : 0, uon: aplicado ? s.uon : 0, uoff: aplicado ? s.uoff : 0 };
    for (const t of s.logs) addLog(t);
    for (const e of s.ev) {
      const k = evKey(s.boot, e.id), old = evStore.get(k);
      evStore.set(k, Object.assign(e, { boot: s.boot, key: k, visto: old ? old.visto : new Date().toISOString() }));
      ultEv++;
    }
    if (rec) recordUpdate(s);
    // La sesión del atril empezó (botón «Iniciar sesión», BOOT o tecla s): se registra
    if (!rec && view === 'session' && patient && prev && prev.modo === 'CALIBRANDO' && s.modo === 'SESION') startRecording(s);
    revisarTareas();
    simAuto(s);
    renderConn();
    if (view === 'session') { renderCal(); renderNotif(); }
  }

  /* ---------- registro de la terapia ---------- */
  function snapshotCal(s) {
    return { origen: 'atril', tipo: 'basal', reposo: s.cb.media, sd: s.cb.sd, cv: s.cb.cv, deriva: s.cb.der, minimo: s.cb.mn, maximo: s.cb.mx, bloques: s.cb.n,
      calidadNivel: s.cb.niv, calidadBanderas: s.cb.fl, k: s.cfg.k, f: s.cfg.f, uon: s.uon, uoff: s.uoff, umbralOrigen: s.uorig, fecha: new Date().toISOString() };
  }
  function addSid(s, primera) {
    if (rec.offs[s.sid] !== undefined) return;
    // tiempo de terapia ANTES de que empiece este tramo del atril; al unirse a mitad de sesión el cero es «ahora»
    rec.offs[s.sid] = primera ? -s.tSes : rec.total;
    rec.minAct[s.sid] = primera ? s.nAct : 0;
    rec.cals.push(snapshotCal(s));
  }
  function startRecording(s) {
    rec = { start: new Date().toISOString(), boot: s.boot, offs: {}, minAct: {}, total: 0, acts: [], seen: new Set(), rms: [], cals: [], recal: 0, cfg: Object.assign({}, s.cfg) };
    addSid(s, true);
    localStorage.setItem('emg.fisio', $('m-fisio').value);
    propuesta = null; rechazadas = []; ajustesLog = []; analisis = null; analisisSig = '';
    toast('Sesión en curso: se registran las confirmaciones y las activaciones del atril.');
  }
  const recTime = () => (rec && st && rec.offs[st.sid] !== undefined && st.boot === rec.boot ? rec.offs[st.sid] + st.tSes : rec ? rec.total : 0);
  function recordUpdate(s) {
    if (s.boot !== rec.boot) return;
    if ((s.modo === 'SESION' || s.modo === 'PAUSA') && rec.offs[s.sid] === undefined) {   // recalibraron: la misma terapia continúa
      addSid(s, false); rec.recal++;
    }
    if (rec.offs[s.sid] !== undefined) rec.total = Math.max(rec.total, rec.offs[s.sid] + s.tSes);
    for (const [sid, id, ini, dur, rms, pct] of s.acts) {
      const uid = s.boot + ':' + sid + ':' + id;
      if (rec.offs[sid] === undefined || id <= (rec.minAct[sid] || 0) || rec.seen.has(uid)) continue;
      rec.seen.add(uid);
      rec.acts.push({ n: rec.acts.length + 1, inicioS: rec.offs[sid] + ini, durS: dur, rms, pct });
    }
    if (s.modo === 'SESION' && rec.offs[s.sid] !== undefined) {
      const t = recTime(), n = s.b.length;
      s.b.forEach(([v, f], i) => rec.rms.push([+(t - (n - 1 - i) * 0.1).toFixed(1), v, f & 1 ? 'ALTO' : 'BAJO', f & 4 ? 1 : 0]));
    }
  }
  const liveAct = () => (rec && st && st.modo === 'SESION' && st.activo && rec.offs[st.sid] !== undefined ? st.durAct : 0);
  /** Eventos de la terapia en curso (todos los tramos) y del tramo vigente del atril */
  const eventosRec = () => (rec ? [...evStore.values()].filter(e => e.boot === rec.boot && rec.offs[e.sid] !== undefined).sort((a, b) => a.sid - b.sid || a.id - b.id) : []);
  const eventosVigentes = () => (st ? [...evStore.values()].filter(e => e.boot === st.boot && e.sid === st.sid).sort((a, b) => a.id - b.id) : []);
  const tEvento = e => (rec && rec.offs[e.sid] !== undefined ? rec.offs[e.sid] + e.tIni : e.tIni);

  /* ---------- alarmas ---------- */
  function beep(f, vol) {
    try {
      const a = new (window.AudioContext || window.webkitAudioContext)(), o = a.createOscillator(), g = a.createGain();
      o.frequency.value = f || 880; g.gain.value = vol || 0.15; o.connect(g); g.connect(a.destination); o.start(); o.stop(a.currentTime + 0.35);
      setTimeout(() => a.close(), 600);
    } catch (e) { /* sin audio */ }
  }
  function evalAlarms(now) {
    const A = {}, inSes = view === 'session';
    const ac = atrilConn.status;
    if (ac === 'error') A.atril = ['crit', 'El atril (ESP32) no responde en ' + S.ipAtril + ': ' + atrilConn.msg + '. ¿Está encendido y la laptop conectada a la red WiFi «ASTRA»? El atril sigue funcionando y conserva sus umbrales; se vuelve a sincronizar solo.'];
    else if (ac === 'disconnected' && inSes) A.atril = ['crit', 'Atril sin conexión. Pulsa «Conectar ASTRA» (o «Simulación» para probar).'];
    if (st) {
      if (st.modo === 'PARO') A.paro = ['crit', 'PARO DE EMERGENCIA en el atril.'];
      else if (!st.datos && st.modo !== 'ARRANQUE') A.opto = ['crit', 'El atril no recibe el RMS del Arduino (opto). Revisa que el Arduino esté encendido y el cable del opto.' + (midiendo(st) ? ' El registro basal espera hasta que vuelvan los datos y, si se cortan, se rechaza.' : '')];
      if (!st.estOK) A.tope = ['warn', 'Estrellas desactivadas: el atril no encontró el tope (microswitch).'];
    }
    const c = emgConn.status;
    if (c === 'error' && inSes) A.emg = ['warn', 'Señal EMG del Arduino: ' + emgConn.msg + '. Solo afecta a la gráfica; el atril sigue funcionando. Reintentando…'];
    else if (c === 'disconnected' && inSes && atril) A.emg = ['warn', 'La señal EMG del Arduino no está conectada (solo afecta a la gráfica).'];
    if (c === 'connected') {
      const silent = now - rx.lastData;
      if (silent > 1000) A.nodata = ['warn', 'Sin señal EMG desde hace ' + Math.round(silent / 1000) + ' s. Revisa el WiFi del Arduino.'];
      else {
        const q = quality.evaluate();
        if (q) {
          qLast = q;
          qCount.sat = q.saturated ? qCount.sat + 1 : 0;
          qCount.flat = q.flat && !q.saturated ? qCount.flat + 1 : 0;
          qCount.noisy = q.noisy60 ? qCount.noisy + 1 : 0;
        }
        if (qCount.sat >= 2) A.sat = ['crit', 'Señal saturada (' + Math.round(qLast.satFrac * 100) + '% de las muestras en el límite del ADC). Electrodo o cable desconectado, o falta el electrodo de referencia.'];
        else if (qCount.flat >= 2) A.flat = ['crit', 'Señal plana, sin ruido. El sensor puede no estar alimentado o el cable de señal está desconectado.'];
        else if (qCount.noisy >= 2) A.noisy = ['warn', 'Mucho ruido de la red (' + Math.round(qLast.ratio60 * 100) + '% de la energía en 60 Hz y armónicos). Revisa contacto de electrodos, referencia y cables.'];
        if (emg && emg.lost > rx.lostSeen) { rx.lostSeen = emg.lost; rx.lostAt = now; }
        if (now - rx.lostAt < 5000 && rx.lostAt) A.lost = ['warn', 'Se pierden paquetes de la señal EMG (' + (emg ? emg.lost : 0) + ' en total). Acerca la laptop al Arduino.'];
        if (now - rx.connectedAt > 4000 && rx.fs && Math.abs(rx.fs - S.fs) > S.fs * 0.1) A.rate = ['warn', 'Frecuencia medida ' + Math.round(rx.fs) + ' Hz (esperada ' + S.fs + ' Hz). Revisa que FS del Arduino coincida con Ajustes.'];
      }
    }
    const el = $('alarms'); el.innerHTML = '';
    const crit = new Set();
    Object.entries(A).forEach(([k, [lvl, msg]]) => {
      const d = document.createElement('div'); d.className = 'alarm ' + lvl;
      d.textContent = (lvl === 'crit' ? '⚠ ' : '! ') + msg; el.appendChild(d);
      if (lvl === 'crit') crit.add(k);
    });
    if (S.sound) for (const k of crit) if (!prevCrit.has(k)) { beep(); break; }
    prevCrit = crit;
  }

  /* ===================================================================
   * Sesión
   * =================================================================== */
  function startSession() {
    rec = null; calPreparada = false; propuesta = null; rechazadas = []; ajustesLog = [];
    $('s-name').textContent = fullName(patient);
    $('s-dob').textContent = 'Nacimiento: ' + fmtDate(patient.nacimiento) + ' (' + ageText(patient.nacimiento) + ') · GMFCS ' + patient.gmfcs;
    ['m-electrodo', 'm-posicion', 'm-notas'].forEach(i => { $(i).value = ''; });
    $('m-fisio').value = localStorage.getItem('emg.fisio') || '';
    show('session'); renderSession();
    if (!atril) toast('Conecta el sistema ASTRA (o activa la simulación) para empezar.');
  }

  // Botones de la calibración basal (cada uno manda un comando al atril)
  $('btn-cal-start').addEventListener('click', () => { calPreparada = true; sendCmd('c'); renderCal(); });
  $('btn-basal').addEventListener('click', () => sendCmd('1'));
  $('btn-phase-cancel').addEventListener('click', () => sendCmd('x'));
  $('btn-run-start').addEventListener('click', () => sendCmd('s'));
  $('btn-cal-redo').addEventListener('click', () => { calPreparada = true; sendCmd('1'); });
  $('btn-force').addEventListener('click', () => {
    if (confirm('La señal basal no cumplió los criterios de calidad. Usarla de todas formas es una decisión clínica: el umbral podría ser poco fiable. ¿Aceptarla de todas formas?')) sendCmd('f');
  });
  $('btn-attach').addEventListener('click', () => { if (st) { startRecording(st); renderCal(); } });
  $('btn-attach-cal').addEventListener('click', () => { calPreparada = true; sendCmd('c'); });
  $('btn-pause').addEventListener('click', () => sendCmd('p'));
  $('btn-recal').addEventListener('click', () => {
    if (confirm('¿Recalibrar? El atril deja la sesión y vuelve a registrar el RMS basal. El tiempo de terapia se detiene mientras tanto y luego continúa; las confirmaciones anteriores se conservan.')) { calPreparada = true; sendCmd('c'); }
  });
  $('btn-resume').addEventListener('click', () => sendCmd('r'));

  function setMsg(text, cls, showResume) {
    $('cal-msg').hidden = !text; $('cal-msg').textContent = text || ''; $('cal-msg').className = 'callout ' + (cls || '');
    $('cal-msg-actions').hidden = !showResume;
  }
  const TXT_FLAGS = [
    [1, 'Se interrumpieron los datos durante el registro.', 1], [2, 'Señal casi nula: revisa electrodos y sensor.', 1], [4, 'RMS basal muy alto: ruido o mal contacto.', 1],
    [8, 'Señal saturada.', 1], [16, 'Señal plana (sin variación): ¿sensor sin alimentación o cable suelto?', 1], [32, 'Variabilidad excesiva del RMS en reposo.', 1],
    [64, 'Variabilidad alta del RMS en reposo.', 0], [128, 'Deriva excesiva entre la primera y la segunda mitad del registro.', 1], [256, 'Deriva notable entre la primera y la segunda mitad.', 0],
    [512, 'Picos aislados (¿el niño se movió?).', 0], [1024, 'Muy pocos bloques válidos.', 1]];
  function renderCal() {
    const on = atrilConn.status === 'connected' && st;
    let intro = false, steps = false, result = false, run = false, attach = false;
    const badge = $('atril-badge');
    badge.className = 'badge'; badge.textContent = on ? st.modo + (st.modo === 'CALIBRANDO' ? ' · ' + st.fase : '') + (st.modo === 'SESION' || st.modo === 'PAUSA' ? (st.ctl ? ' · control automático' : ' · supervisado') : '') : 'sin conexión';
    const cls = on ? { PARO: 'bad', SESION: 'ok', CALIBRANDO: 'warn' }[st.modo] : null;
    if (cls) badge.classList.add(cls);
    setMsg('');
    if (!on) setMsg(atrilConn.status === 'connecting' || atrilConn.status === 'connected' ? 'Conectando con el atril…' : 'Conecta el atril («Conectar ASTRA» o «Simulación») para calibrar.', 'warn');
    else if (st.modo === 'ARRANQUE') setMsg('El atril está buscando el tope de las estrellas. La calibración se habilita al terminar.');
    else if (st.modo === 'PARO') setMsg('Paro de emergencia. Suelta el hongo y pulsa «Reanudar el atril».', 'bad', true);
    else if (st.modo === 'CALIBRANDO') {
      const started = calPreparada || st.fase !== 'ESPERA';
      intro = !started; steps = started && (st.fase === 'ESPERA' || st.fase === 'BASAL'); result = st.fase === 'LISTA' || st.fase === 'RECHAZADA';
    } else if (rec) run = true;
    else attach = true;

    $('cal-intro').hidden = !intro; $('cal-steps').hidden = !steps; $('cal-result').hidden = !result;
    $('run-ctrl').hidden = !run; $('attach').hidden = !attach;
    $('cal-title').textContent = run ? 'Sesión en el atril' : 'Calibración basal';
    $('meta-box').open = !rec;
    if (steps) renderBasal();
    if (result) renderResult();
    if (run) {
      $('cal-compact').textContent = 'U_ON ' + f1(st.uon) + ' mV · U_OFF ' + f1(st.uoff) + ' mV · umbral ' + ['basal (preliminar)', 'ajustado y aprobado', 'restaurado'][st.uorig] +
        ' · media basal ' + f1(st.cb.media) + ' ± ' + f1(st.cb.sd) + ' mV' + (rec.recal ? ' · recalibrado ' + rec.recal + ' vez/veces' : '');
      $('btn-pause').textContent = st.modo === 'PAUSA' ? '▶ Seguir' : '❚❚ Pausa';
    }
    if (attach) $('attach-thr').textContent = 'U_ON ' + f1(st.uon) + ' mV, sesión ' + st.sid + ' del atril';
    $('btn-csv').disabled = !rec; $('btn-finish').disabled = !rec;
  }
  function renderBasal() {
    const s = st, mid = midiendo(s), cfg = s.cfg, bps = cfg.bps || 10;
    $('basal-step').hidden = mid;
    $('btn-basal').textContent = 'Registrar RMS basal (' + f0(cfg.basal) + ' s)';
    $('cue').hidden = !mid;
    if (!mid) return;
    const left = Math.max(0, Math.ceil((s.total - s.bloq) / bps));
    setText('cue-count', left + ' s');
    $('cue-bar').style.width = (100 * s.bloq / Math.max(1, s.total)) + '%';
    let note = s.bloq <= cfg.desc * bps ? 'Descartando el primer ' + cfg.desc + ' s (los filtros se estabilizan)…' : 'Registrando el RMS basal…';
    if (!s.datos) note = 'El atril no recibe datos del Arduino. Si se cortan, el registro se rechaza.';
    setText('cue-note', note + ' · RMS ahora: ' + s.rms + ' mV');
  }
  function renderResult() {
    const s = st, c = s.cb, rech = s.fase === 'RECHAZADA';
    const niv = ['aceptada', 'aceptada con advertencias', 'RECHAZADA', 'aceptada a la fuerza'][c.niv];
    $('cal-result-title').textContent = rech ? 'La calibración basal no es aceptable.' : 'Calibración basal lista.';
    $('cal-result-sub').textContent = 'Calidad de la señal: ' + niv + '.';
    const items = [['RMS basal (media)', f2(c.media) + ' mV'], ['Desviación estándar', f2(c.sd) + ' mV'], ['Variabilidad (SD / media)', f2(c.cv)],
      ['Deriva (2.ª mitad vs 1.ª)', (c.der >= 0 ? '+' : '') + Math.round(c.der * 100) + ' %'], ['Mínimo – máximo', f1(c.mn) + ' – ' + f1(c.mx) + ' mV'], ['Bloques usados', String(c.n)]];
    if (!rech) items.push(['U_ON (media + ' + s.cfg.k + ' × SD)', f1(s.uon) + ' mV'], ['U_OFF (media + ' + s.cfg.f + ' × (U_ON − media))', f1(s.uoff) + ' mV']);
    $('cal-values').innerHTML = items.map(() => '<div><span></span><b></b></div>').join('');
    $('cal-values').querySelectorAll('div').forEach((d, i) => { d.firstChild.textContent = items[i][0]; d.lastChild.textContent = items[i][1]; });
    const w = TXT_FLAGS.filter(([bit]) => c.fl & bit).map(([, t, rechazo]) => (rechazo ? 'Motivo de rechazo: ' : 'Advertencia: ') + t);
    if (rech) w.push('Revisa electrodos, postura y cables y repite el registro. Si decides usarla de todas formas, queda marcada en el registro.');
    $('cal-warnings').innerHTML = w.map(() => '<li></li>').join('');
    $('cal-warnings').querySelectorAll('li').forEach((li, i) => { li.textContent = w[i]; });
    $('cal-prelim').hidden = rech;
    $('btn-run-start').disabled = rech; $('btn-force').hidden = !rech;
  }

  /* ---------- aviso de verificación (discreto) ---------- */
  let notifId = 0;
  function renderNotif() {
    const box = $('notif');
    const ev = st && st.ver.pend ? evStore.get(evKey(st.boot, st.ver.pend)) : null;
    const activo = ev && ev.status === 1 && !respondidos.has(ev.key) && view === 'session' && st.modo === 'SESION';
    if (!activo) { box.hidden = true; notifId = 0; return; }
    const restante = Math.max(0, st.cfg.resp - (st.tSes - ev.tIni - ev.dur));
    const cambio = st.ep !== ev.ep;
    setText('notif-meta', 'Aviso ' + ev.nSol + ' de ' + st.ver.max + ' · ventana de ' + f0(ev.dur) + ' s ya registrada · caduca en ' + clock(restante) +
      (cambio ? ' · El estado del RMS cambió desde entonces: responde solo si recuerdas cómo estaba cuando apareció el aviso, o elige «No se puede determinar».' : ''));
    if (notifId !== ev.id) { notifId = ev.id; if (S.soundVerif) beep(660, 0.05); }
    box.hidden = false;
  }
  document.querySelectorAll('[data-resp]').forEach(b => b.addEventListener('click', () => {
    const ev = st && st.ver.pend ? evStore.get(evKey(st.boot, st.ver.pend)) : null;
    if (ev) responder(ev, b.dataset.resp);
  }));
  function responder(ev, p) {
    respondidos.add(ev.key); $('notif').hidden = true;
    tarea('respuesta de verificación', () => atril.conf(ev.id, p), s => { const e = evStore.get(evKey(s.boot, ev.id)); return !e || e.status !== 1; });
    renderVerif();
  }
  document.querySelectorAll('[data-manual]').forEach(b => b.addEventListener('click', () => {
    if (!st || st.modo !== 'SESION') { toast('La confirmación manual necesita la sesión en marcha.'); return; }
    const p = b.dataset.manual, tok = nextTok(), boot = st.boot;
    tarea('confirmación manual', () => atril.manual(p, tok), s => [...evStore.values()].some(e => e.boot === boot && e.tok === tok), () => toast('Confirmación manual registrada.'));
  }));

  /* ---------- verificación, análisis y ajuste ---------- */
  const NOM_POST = { 1: 'Erguida', 2: 'Caída', 3: 'No se puede determinar' };
  const NOM_STATUS = { 1: 'pendiente', 2: 'confirmada', 3: 'descartada', 4: 'sin respuesta' };
  const cfgAn = () => ({ nMinClase: S.nMinClase, aucMin: S.aucMin });
  function calidadTxt(e) {
    return (e.calidad === 0 ? 'buena' : 'dudosa') + (st && st.sid === e.sid && st.cb.niv >= 1 ? ' · basal ' + (st.cb.niv === 1 ? 'con avisos' : st.cb.niv === 3 ? 'forzado' : '') : '');
  }
  function renderVerif() {
    if (!st) { setText('ver-status', 'Conecta el atril para ver la verificación.'); return; }
    const v = st.ver, enSes = st.modo === 'SESION', hip = st.cfg.hip;
    const badge = $('ver-badge'); badge.className = 'badge';
    let txt;
    if (st.modo === 'SESION' || st.modo === 'PAUSA') {
      const min = x => f0(x / 60);
      const per = v.per === 0 ? 'Primeros ' + min(v.dur) + ' min' : 'Ciclo ' + v.per + ' (min ' + min(v.dur * (v.per - 1) + (st.cfg.v1s)) + '–' + min(v.dur * v.per + st.cfg.v1s) + ')';
      badge.textContent = per; badge.classList.add(st.modo === 'PAUSA' ? 'warn' : 'ok');
      let sig;
      if (st.modo === 'PAUSA') sig = 'En pausa: el tiempo de verificación está detenido.';
      else if (v.pend) sig = 'Hay una pregunta pendiente.';
      else if (v.esp < 0 || v.sol >= v.max) sig = 'Cupo completo en este periodo; se reinicia en ' + clock(v.dur - v.tPer) + '.';
      else if (v.esp > 0.5) sig = 'Próxima pregunta posible en ' + clock(v.esp) + ' (si hay un estado sostenido).';
      else sig = 'Se preguntará cuando haya un estado sostenido.';
      txt = per + ': ' + v.sol + ' de ' + v.max + ' confirmaciones automáticas. ' + sig;
      $('ver-bar').style.width = Math.min(100, 100 * v.tPer / Math.max(1, v.dur)) + '%';
    } else { badge.textContent = '—'; txt = 'La verificación comienza al iniciar la sesión (primeros 5 min: máx. 4 confirmaciones; luego ciclos de 10 min: máx. 3).'; $('ver-bar').style.width = '0'; }
    setText('ver-status', txt);
    document.querySelectorAll('[data-manual]').forEach(b => { b.disabled = !enSes; });

    const evs = eventosRec();
    const r = E.Analisis.resumen(evs, hip);
    const chips = [['Estados sostenidos detectados: ' + st.nDet, ''], ['Automáticas: ' + r.auto, ''], ['Manuales: ' + r.manual, ''], ['Confirmadas: ' + r.confirmadas, ''],
      ['Coinciden con el umbral: ' + r.coinciden, r.coinciden ? 'ok' : ''], ['No coinciden: ' + r.noCoinciden, r.noCoinciden ? 'bad' : ''],
      ['Falsos positivos: ' + r.falsosPositivos, r.falsosPositivos ? 'bad' : ''], ['Falsos negativos: ' + r.falsosNegativos, r.falsosNegativos ? 'bad' : ''],
      ['Descartadas: ' + r.descartadas, ''], ['Sin respuesta: ' + r.expiradas, '']];
    const sig = chips.map(c => c[0]).join('|');
    if ($('ver-chips').dataset.sig !== sig) {
      $('ver-chips').dataset.sig = sig; $('ver-chips').innerHTML = chips.map(() => '<span></span>').join('');
      $('ver-chips').querySelectorAll('span').forEach((el, i) => { el.textContent = chips[i][0]; if (chips[i][1]) el.className = chips[i][1]; });
    }
    renderConfTable(evs, hip);
    renderAjuste();
  }
  function renderConfTable(evs, hip) {
    const tb = $('conf-body'), key = evs.map(e => e.key + e.status + e.postura).join(',') + hip;
    if (tb.dataset.key === key) return;
    tb.dataset.key = key; tb.innerHTML = '';
    if (!evs.length) { tb.innerHTML = '<tr><td colspan="10" class="muted">Sin confirmaciones todavía.</td></tr>'; return; }
    evs.slice().reverse().forEach((e, i) => {
      const res = E.Analisis.resultado(e, hip);
      const cells = [evs.length - i, clock(tEvento(e)), e.origen ? 'manual' : 'automática', e.periodo === 0 ? '0–5 min' : 'ciclo ' + e.periodo,
        f1(e.media) + ' ± ' + f1(e.sd) + ' (' + f1(e.minimo) + '–' + f1(e.maximo) + ') mV', e.estado === 1 ? 'ALTO (≥ U_ON)' : 'BAJO (< U_OFF)',
        f1(e.uon) + ' / ' + f1(e.uoff), e.postura ? NOM_POST[e.postura] : NOM_STATUS[e.status], res, calidadTxt(e)];
      const tr = document.createElement('tr');
      cells.forEach((v, c) => { const td = document.createElement('td'); td.textContent = v; if (c === 8) td.className = res === 'coincide' ? 'res-coincide' : res === 'no coincide' ? 'res-no' : ''; tr.appendChild(td); });
      tb.appendChild(tr);
    });
  }
  function renderAjuste() {
    const evs = eventosVigentes(), hip = st.cfg.hip;
    const sig = evs.map(e => e.key + e.status + e.postura).join(',') + '|' + st.uon + '|' + st.uoff + '|' + hip + '|' + st.cb.media + '|' + S.nMinClase + '|' + S.aucMin + '|' + rechazadas.length;
    if (sig !== analisisSig) {
      analisisSig = sig;
      analisis = E.Analisis.evaluar({ events: evs, uon: st.uon, uoff: st.uoff, base: { media: st.cb.media, sd: st.cb.sd }, f: st.cfg.f, hip, cfg: cfgAn() });
      propuesta = null;
      if (analisis.estado === 'propuesta') {
        const p = analisis.propuesta, rej = rechazadas.find(x => Math.abs(x.uon - p.uon) / x.uon < 0.05 && analisis.n.total < x.n + 3);
        if (rej) { analisis = Object.assign({}, analisis, { estado: 'rechazada_antes', titulo: 'Propuesta rechazada anteriormente',
          motivo: 'Rechazaste un ajuste parecido (U_ON ' + f1(rej.uon) + ' mV). No se vuelve a proponer hasta tener al menos 3 observaciones independientes nuevas. ' + analisis.motivo }); }
        else { propuesta = p; const k = f0(p.uon * 10) + ':' + analisis.n.total; if (!ajustesLog.some(a => a.k === k)) ajustesLog.push({ k, tipo: 'propuesta', t: new Date().toISOString(), tSes: recTime(), uonAntes: st.uon, uoffAntes: st.uoff, uonNuevo: p.uon, uoffNuevo: p.uoff, motivo: analisis.motivo, balActual: p.actual.bal, balNuevo: p.bal, loo: p.loo }); }
      }
      pintarAjuste();
    }
    // botones que dependen del estado del atril (no del análisis)
    const enSes = st.modo === 'SESION' || st.modo === 'PAUSA';
    $('btn-restaurar').hidden = !(enSes && st.hist.length >= 2);
    const bc = $('btn-control');
    bc.hidden = !(enSes && (st.ctl || (analisis && analisis.apto)));
    bc.textContent = st.ctl ? 'Volver al modo supervisado (quitar el control automático)' : 'Habilitar el control automático de los actuadores (hay evidencia suficiente)';
    $('aj-badge').className = 'badge ' + (st.ctl ? 'warn' : 'ok'); setText('aj-badge', st.ctl ? 'Control automático' : 'Modo supervisado');
    const h = st.hist.map((x, i) => 'Atril · U_ON ' + f1(x[0]) + ' mV / U_OFF ' + f1(x[1]) + ' mV · ' + ['basal', 'ajustado (aprobado)', 'restaurado'][x[2]] + ' · a los ' + clock(x[3]) + ' de terapia' + (i === st.hist.length - 1 ? ' ← vigente' : ''))
      .concat(ajustesLog.filter(a => a.tipo !== 'propuesta').map(a => 'Página · ' + a.tipo + ' · U_ON ' + f1(a.uonAntes) + (a.uonNuevo !== undefined ? ' → ' + f1(a.uonNuevo) : '') + ' mV · ' + fmtDateTime(a.t)));
    const hs = h.join('|');
    if ($('aj-hist').dataset.sig !== hs) { $('aj-hist').dataset.sig = hs; $('aj-hist').innerHTML = h.map(() => '<li></li>').join(''); $('aj-hist').querySelectorAll('li').forEach((li, i) => { li.textContent = h[i]; }); }
  }
  function pintarAjuste() {
    const a = analisis;
    const cls = { correcto: 'ok', propuesta: 'warn', invertida: 'bad', contradictorio: 'bad', ruido: 'bad', fuera_de_rango: 'bad' }[a.estado] || '';
    setText('aj-titulo', a.titulo); setText('aj-motivo', a.motivo);
    $('aj-titulo').className = cls === 'bad' ? 'danger-text' : '';
    const det = a.detalles.slice(); $('aj-detalles').innerHTML = det.map(() => '<li></li>').join(''); $('aj-detalles').querySelectorAll('li').forEach((li, i) => { li.textContent = det[i]; });
    const items = [['Confirmadas', String(a.n.confirmadas)], ['Observaciones independientes', a.n.A + ' (' + a.nombres.A + ') + ' + a.n.B + ' (' + a.nombres.B + ')']];
    if (a.actual) items.push(['Umbral actual: exactitud balanceada', f2(a.actual.bal)], ['Falsos positivos / negativos', a.actual.fp + ' / ' + a.actual.fn]);
    if (a.stats) items.push(['Separación entre posturas (AUC)', f2(a.stats.auc)], ['RMS mediano ' + a.nombres.A + ' / ' + a.nombres.B, f0(a.stats.medA) + ' / ' + f0(a.stats.medB) + ' mV']);
    $('aj-nums').innerHTML = items.map(() => '<div><span></span><b></b></div>').join('');
    $('aj-nums').querySelectorAll('div').forEach((d, i) => { d.firstChild.textContent = items[i][0]; d.lastChild.textContent = items[i][1]; });
    const p = propuesta; $('aj-prop').hidden = !p;
    if (p) {
      setText('pp-on0', f1(st.uon) + ' mV'); setText('pp-on1', f1(p.uon) + ' mV'); setText('pp-off0', f1(st.uoff) + ' mV'); setText('pp-off1', f1(p.uoff) + ' mV');
      setText('pp-bal0', f2(p.actual.bal)); setText('pp-bal1', f2(p.bal) + ' (validación cruzada ' + f2(p.loo) + ')');
      setText('pp-err0', p.actual.fp + ' / ' + p.actual.fn); setText('pp-err1', p.m.fp + ' / ' + p.m.fn);
    }
  }
  $('btn-aprobar').addEventListener('click', () => {
    const p = propuesta; if (!p || !st) return;
    const on = +p.uon.toFixed(1), off = +p.uoff.toFixed(1), tok = nextTok(), antes = { on: st.uon, off: st.uoff };
    propuesta = null; $('aj-prop').hidden = true;
    tarea('aplicar el umbral aprobado', () => atril.umbral(on, off, tok), s => Math.abs(s.uon - on) < 0.06 && s.uorig === 1, () => {
      ajustesLog.push({ k: 'ap' + tok, tipo: 'aprobado', t: new Date().toISOString(), tSes: recTime(), uonAntes: antes.on, uoffAntes: antes.off, uonNuevo: on, uoffNuevo: off, motivo: analisis ? analisis.motivo : '' });
      analisisSig = ''; toast('Umbral nuevo aplicado en el atril: U_ON ' + f1(on) + ' mV.');
    });
  });
  $('btn-rechazar').addEventListener('click', () => {
    const p = propuesta; if (!p || !st) return;
    rechazadas.push({ uon: p.uon, n: analisis.n.total });
    ajustesLog.push({ k: 'rj' + Date.now(), tipo: 'rechazado', t: new Date().toISOString(), tSes: recTime(), uonAntes: st.uon, uoffAntes: st.uoff, uonNuevo: p.uon, uoffNuevo: p.uoff, motivo: analisis.motivo });
    propuesta = null; analisisSig = ''; toast('Propuesta rechazada: se conserva el umbral actual.');
  });
  $('btn-restaurar').addEventListener('click', () => {
    if (!st || st.hist.length < 2) return;
    const prev = st.hist[st.hist.length - 2], antes = { on: st.uon, off: st.uoff }, tok = nextTok();
    if (!confirm('¿Restaurar el umbral anterior (U_ON ' + f1(prev[0]) + ' mV)?')) return;
    tarea('restaurar el umbral anterior', () => atril.restaurar(tok), s => s.uorig === 2 && Math.abs(s.uon - prev[0]) < 0.06, () => {
      ajustesLog.push({ k: 'rs' + tok, tipo: 'restaurado', t: new Date().toISOString(), tSes: recTime(), uonAntes: antes.on, uoffAntes: antes.off, uonNuevo: prev[0], uoffNuevo: prev[1], motivo: 'Restaurado por la fisioterapeuta.' });
      analisisSig = ''; toast('Umbral anterior restaurado.');
    });
  });
  $('btn-control').addEventListener('click', () => {
    if (!st) return;
    const quiere = !st.ctl;
    if (quiere && !confirm('¿Habilitar el control automático? A partir de ahora el umbral moverá estrellas, luz y sonido. El análisis indica que el RMS separa bien las posturas de este niño en esta sesión, pero sigue siendo tu decisión.')) return;
    sendCmd('a');
  });

  /* ---------- UI de sesión (4 Hz; las gráficas van a 60 fps aparte) ---------- */
  function renderSession() {
    const t = recTime(), live = liveAct();
    $('s-time').textContent = $('k-time').textContent = clock(t);
    const acts = rec ? rec.acts : [];
    const totalActive = acts.reduce((a, s) => a + s.durS, 0) + live;
    $('k-count').textContent = acts.length + (live ? 1 : 0);
    $('k-active').textContent = f1(totalActive) + ' s con RMS sobre U_ON';
    $('k-cal').textContent = rec ? 'sin contar pausas ni recalibraciones' : 'empieza con «Iniciar sesión»';
    if (st) {
      const evs = eventosRec(), r = E.Analisis.resumen(evs, st.cfg.hip);
      $('k-det').textContent = st.nDet; $('k-est').textContent = st.modo === 'SESION' ? (st.est === 1 ? 'ahora: ALTO · ' + f0(st.bEst) + ' s' : st.est === 2 ? 'ahora: BAJO · ' + f0(st.bEst) + ' s' : '') : '';
      $('k-conf').textContent = r.confirmadas;
      $('k-coin').textContent = r.confirmadas ? Math.round(100 * r.coinciden / r.confirmadas) + ' % coincide con el umbral' : 'sin confirmaciones';
      $('k-per').textContent = st.modo === 'SESION' || st.modo === 'PAUSA' ? st.ver.sol + ' / ' + st.ver.max : '—';
      $('k-perdet').textContent = st.modo === 'SESION' || st.modo === 'PAUSA' ? (st.ver.per === 0 ? 'primeros 5 min' : 'ciclo ' + st.ver.per) : '';
      const items = [['RMS', st.rms + ' mV'], ['Suavizado', f1(st.suave) + ' mV'], ['Estado', st.modo === 'SESION' ? (st.est === 1 ? 'ALTO' : st.est === 2 ? 'BAJO' : '—') + (st.sost ? ' (sostenido)' : '') : st.modo],
        ['Estrellas', st.estOK ? st.alt + ' %' : 'desactivadas'], ['WiFi del atril', st.rssi + ' dBm']];
      const html = items.map(i => i[0] + ': ' + i[1]).join('|');
      if ($('readouts').dataset.sig !== html) {
        $('readouts').dataset.sig = html; $('readouts').innerHTML = items.map(() => '<span><span></span>: <b></b></span>').join('');
        $('readouts').querySelectorAll(':scope > span').forEach((el, i) => { el.firstChild.textContent = items[i][0]; el.lastChild.textContent = items[i][1]; });
      }
    } else { $('readouts').textContent = ''; $('readouts').dataset.sig = ''; }
    renderVerif();
    renderTable(acts, live);
    if (logDirty && $('log-card').open) {
      logDirty = false;
      const el = $('log'), atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
      el.textContent = logLines.join('\n');
      if (atEnd) el.scrollTop = el.scrollHeight;
    }
    if (sim) setText('sp-real', sim.posturaReal() ? 'erguido' : 'caído');
  }
  function renderTable(acts, live) {
    const tb = $('act-body'); tb.innerHTML = '';
    const rows = acts.map(a => [a.n, clock(a.inicioS), f1(a.durS), f0(a.rms), f0(a.pct)]);
    if (live) rows.push([acts.length + 1 + ' (en curso)', '—', f1(live), '—', '—']);
    if (!rows.length) { tb.innerHTML = '<tr><td colspan="5" class="muted">Sin activaciones todavía.</td></tr>'; return; }
    rows.slice().reverse().forEach((r, k) => {
      const tr = document.createElement('tr'); if (live && k === 0) tr.className = 'live-row';
      r.forEach(v => { const td = document.createElement('td'); td.textContent = v; tr.appendChild(td); });
      tb.appendChild(tr);
    });
  }
  $('win-sel').addEventListener('change', e => { S.winS = +e.target.value; E.Store.saveSettings(S); chart.setWindow(S.winS); rmsChart.setWindow(S.winS); });

  /* ---------- guardar / finalizar ---------- */
  function buildRecord() {
    const t = recTime(), acts = rec.acts, tot = acts.reduce((a, s) => a + s.durS, 0), hip = rec.cfg.hip;
    const evs = eventosRec(), resumenV = E.Analisis.resumen(evs, hip);
    const conf = evs.map(e => ({
      id: e.id, sid: e.sid, origen: e.origen ? 'manual' : 'automatica', periodo: e.periodo === 0 ? 'primeros 5 min' : 'ciclo ' + e.periodo, solicitud: e.nSol,
      tEventoS: tEvento(e), horaVista: e.visto, rmsMedio: e.media, rmsSD: e.sd, rmsMin: e.minimo, rmsMax: e.maximo, ventanaS: e.dur,
      estadoRms: e.estado === 1 ? 'ALTO' : 'BAJO', umbralOn: e.uon, umbralOff: e.uoff,
      postura: e.postura ? ['', 'erguida', 'caida', 'no determinada'][e.postura] : '', estado: NOM_STATUS[e.status], resultado: E.Analisis.resultado(e, hip),
      calidadVentana: e.calidad === 0 ? 'buena' : 'dudosa', episodio: e.ep }));
    return {
      patientId: patient.id, fecha: rec.start, duracionS: t, origen: 'atril-basal',
      electrodo: $('m-electrodo').value.trim(), posicion: $('m-posicion').value.trim(), fisio: $('m-fisio').value.trim(), notas: $('m-notas').value.trim(),
      calibracion: rec.cals[0], calibraciones: rec.cals, umbralMV: rec.cals[0].uon, recalibraciones: rec.recal, parametros: rec.cfg,
      confirmaciones: conf, ajustes: ajustesLog.filter(a => a.tipo !== 'propuesta' || true).map(a => Object.assign({}, a)),
      analisisFinal: analisis ? { estado: analisis.estado, titulo: analisis.titulo, motivo: analisis.motivo, n: analisis.n, actual: analisis.actual, stats: analisis.stats } : null,
      resumen: { activaciones: acts.length, tiempoActivoS: tot, pctActivo: t > 0 ? 100 * tot / t : 0, maxContrS: Math.max(0, ...acts.map(a => a.durS)),
        medContrS: acts.length ? tot / acts.length : 0, rmsNormMedio: acts.length ? acts.reduce((a, s) => a + s.pct, 0) / acts.length : null, descartadas: 0,
        confirmaciones: resumenV.confirmadas, coinciden: resumenV.coinciden, noCoinciden: resumenV.noCoinciden },
      activaciones: acts.map(a => ({ n: a.n, inicioS: a.inicioS, finS: a.inicioS + a.durS, durS: a.durS, rms: a.rms, mav: null, rmsNorm: a.pct }))
    };
  }
  function activationsCSV(r, p) {
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha_sesion', 'activacion', 'inicio_s', 'fin_s', 'duracion_s', 'rms_medio_mV', 'pct_del_umbral', 'media_basal_mV', 'umbral_on_mV'];
    const c = r.calibracion || {};
    const rows = r.activaciones.map(a => [fullName(p), p.expediente || '', p.gmfcs, r.fecha, a.n, a.inicioS, a.finS, a.durS, a.rms, a.rmsNorm, c.reposo !== undefined ? c.reposo : c.restMean, r.umbralMV]);
    E.download(E.slug(fullName(p)) + '_' + r.fecha.slice(0, 16).replace(/[:T]/g, '-') + '_activaciones.csv', E.toCSV(H, rows));
  }
  function confirmacionesCSV(r, p) {
    const H = ['paciente', 'expediente', 'fecha_sesion', 'evento', 'origen', 'periodo', 'solicitud', 'tiempo_terapia_s', 'hora_aprox', 'ventana_s', 'rms_medio_mV', 'rms_sd_mV', 'rms_min_mV', 'rms_max_mV',
      'estado_rms', 'umbral_on_mV', 'umbral_off_mV', 'postura_confirmada', 'estado_confirmacion', 'resultado', 'calidad_ventana', 'calidad_basal'];
    const niv = ['aceptada', 'con advertencias', 'rechazada', 'forzada'];
    const rows = (r.confirmaciones || []).map(c => [fullName(p), p.expediente || '', r.fecha, c.id, c.origen, c.periodo, c.solicitud, c.tEventoS, c.horaVista, c.ventanaS, c.rmsMedio, c.rmsSD, c.rmsMin, c.rmsMax,
      c.estadoRms, c.umbralOn, c.umbralOff, c.postura, c.estado, c.resultado, c.calidadVentana, niv[(r.calibraciones && r.calibraciones[0] ? r.calibraciones[0].calidadNivel : 0)] || '']);
    E.download(E.slug(fullName(p)) + '_' + r.fecha.slice(0, 16).replace(/[:T]/g, '-') + '_confirmaciones.csv', E.toCSV(H, rows));
  }
  $('btn-csv').addEventListener('click', () => {
    if (!rec) return;
    const r = buildRecord(), stamp = r.fecha.slice(0, 16).replace(/[:T]/g, '-');
    confirmacionesCSV(r, patient); activationsCSV(r, patient);
    E.download(E.slug(fullName(patient)) + '_' + stamp + '_rms_atril.csv', E.toCSV(['tiempo_s', 'rms_mV', 'estado', 'sostenido'], rec.rms));
    toast('CSV descargados (confirmaciones, activaciones y RMS del atril)');
  });
  $('btn-finish').addEventListener('click', async () => {
    if (!rec || !confirm('¿Finalizar la terapia y guardar la sesión en el historial? El atril queda en pausa.')) return;
    const r = buildRecord();
    if (!E.Store.saveSession(r)) { toast('No se pudo guardar en el navegador. Descarga el CSV.'); return; }
    if (st && st.modo === 'SESION') sendCmd('p');
    if (S.sheetsUrl) {
      try {
        await E.sendToSheets(S.sheetsUrl, { tipo: 'sesion', paciente: S.sheetsName ? fullName(patient) : (patient.expediente || patient.id), gmfcs: patient.gmfcs,
          fecha: r.fecha, duracionS: r.duracionS, electrodo: r.electrodo, posicion: r.posicion, fisio: r.fisio, notas: r.notas, resumen: r.resumen, activaciones: r.activaciones, confirmaciones: r.confirmaciones });
      } catch (e) { toast('Sesión guardada, pero no se pudo enviar a Google Sheets.'); }
    }
    rec = null; calPreparada = false; propuesta = null; toast('Sesión guardada en el historial'); show('history');
  });

  /* ---------- historial ---------- */
  const METRICS = { pctActivo: ['% del tiempo', '%'], maxContrS: ['Contracción más larga', 's'], medContrS: ['Duración media', 's'], activaciones: ['Activaciones', ''],
    tiempoActivoS: ['Tiempo activo', 's'], rmsNormMedio: ['% del umbral', '%'], confirmaciones: ['Confirmaciones', ''] };
  function renderHistory() {
    const p = patient; if (!p) return show('search');
    const ses = E.Store.sessionsFor(p.id);
    $('h-name').textContent = fullName(p);
    $('h-sub').textContent = 'GMFCS ' + p.gmfcs + ' · ' + ageText(p.nacimiento) + ' · ' + ses.length + ' sesión(es)';
    const key = $('h-metric').value, pts = ses.filter(s => s.resumen && isFinite(s.resumen[key]) && s.resumen[key] !== null)
      .map(s => ({ y: s.resumen[key], label: fmtDate(s.fecha) }));
    progChart.set(pts, METRICS[key][1]);
    let msg = '';
    if (pts.length >= 2) {
      const k = pts.length >= 6 ? 3 : 1, avg = a => a.reduce((s, q) => s + q.y, 0) / a.length;
      const first = avg(pts.slice(0, k)), last = avg(pts.slice(-k));
      const d = last - first, rel = first > 0 ? ' (' + (d >= 0 ? '+' : '') + (100 * d / first).toFixed(0) + ' %)' : '';
      msg = (k > 1 ? 'Últimas 3 sesiones vs. primeras 3: ' : 'Última sesión vs. primera: ') + first.toFixed(1) + ' → ' + last.toFixed(1) + ' ' + METRICS[key][1] + rel;
    }
    $('h-summary').textContent = msg;
    const tb = $('h-body'); tb.innerHTML = '';
    ses.slice().reverse().forEach(s => {
      const r = s.resumen, tr = document.createElement('tr');
      [fmtDateTime(s.fecha), clock(s.duracionS), r.activaciones, f1(r.tiempoActivoS), Math.round(r.pctActivo), f1(r.maxContrS), r.rmsNormMedio == null ? '—' : Math.round(r.rmsNormMedio)]
        .forEach(v => { const td = document.createElement('td'); td.textContent = v; tr.appendChild(td); });
      const td = document.createElement('td'), b = document.createElement('button'), d = document.createElement('button');
      b.className = 'btn small ghost'; b.textContent = 'CSV'; b.addEventListener('click', () => { if (s.confirmaciones) confirmacionesCSV(s, p); activationsCSV(s, p); });
      d.className = 'btn small ghost'; d.textContent = '🗑'; d.title = 'Eliminar sesión';
      d.addEventListener('click', () => { if (confirm('¿Eliminar esta sesión del historial?')) { E.Store.deleteSession(s.id); renderHistory(); } });
      td.append(b, ' ', d); tr.appendChild(td); tb.appendChild(tr);
    });
  }
  $('h-metric').addEventListener('change', renderHistory);
  $('btn-hist-csv').addEventListener('click', () => {
    const ses = E.Store.sessionsFor(patient.id);
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha', 'duracion_s', 'activaciones', 'tiempo_activo_s', 'pct_activo', 'max_contraccion_s', 'media_contraccion_s', 'pct_del_umbral_medio', 'confirmaciones', 'coinciden', 'no_coinciden', 'electrodo', 'posicion', 'fisio', 'notas'];
    E.download(E.slug(fullName(patient)) + '_historial.csv', E.toCSV(H, ses.map(s => [fullName(patient), patient.expediente || '', patient.gmfcs, s.fecha, s.duracionS, s.resumen.activaciones, s.resumen.tiempoActivoS, s.resumen.pctActivo, s.resumen.maxContrS, s.resumen.medContrS, s.resumen.rmsNormMedio,
      s.resumen.confirmaciones, s.resumen.coinciden, s.resumen.noCoinciden, s.electrodo, s.posicion, s.fisio, s.notas])));
  });

  /* ---------- ajustes ---------- */
  const FIELDS = [
    ['ipArduino', 'IP del Arduino (red WiFi «ASTRA»)', 'text'], ['ipAtril', 'IP del atril (ESP32)', 'text'],
    ['emgUsb', 'Señal EMG por USB (Web Serial) en vez de WiFi', 'checkbox'],
    ['fs', 'Frecuencia de muestreo (Hz) — igual que FS del Arduino'], ['adcMax', 'Máximo del ADC (14 bits = 16383)'], ['vref', 'Referencia ADC (mV)'],
    ['gain', 'Ganancia (1 = mV a la salida de Nova)'], ['hpHz', 'Pasa altas para ver la señal (Hz)'], ['notchHz', 'Frecuencia de la red (Hz)'], ['nArm', 'Notch: n.º de armónicos (7, como el Arduino)'],
    ['nMinClase', 'Análisis: observaciones independientes mínimas por postura'], ['aucMin', 'Análisis: separación mínima (AUC, 0.5–1)'],
    ['soundVerif', 'Pitido suave al aparecer una pregunta de verificación', 'checkbox'],
    ['sheetsUrl', 'URL de Google Sheets (Apps Script)', 'text'], ['sheetsName', 'Enviar nombre del paciente a Sheets', 'checkbox'], ['sound', 'Sonido en alarmas críticas', 'checkbox']
  ];
  // Parámetros que viven en el ESP32 (se guardan allí, no en la página)
  const FIELDS_ATRIL = [
    ['basal', 'Calibración basal: duración (s)'], ['desc', 'Calibración basal: segundos que se descartan'], ['k', 'Umbral: K (U_ON = media + K × SD)'], ['f', 'Umbral: F (U_OFF = media + F × (U_ON − media))'],
    ['tSost', 'Estado sostenido: segundos (y ventana de RMS)'], ['v1s', 'Primer periodo de verificación (s)'], ['v1max', 'Primer periodo: máximo de confirmaciones'], ['v1min', 'Primer periodo: intervalo mínimo (s)'],
    ['vcs', 'Ciclos siguientes (s)'], ['vcmax', 'Cada ciclo: máximo de confirmaciones'], ['vcmin', 'Cada ciclo: intervalo mínimo (s)'], ['rever', 'Mismo estado prolongado: volver a comprobar tras (s)'],
    ['resp', 'Tiempo para responder una pregunta (s)'], ['hip', 'Hipótesis a verificar: RMS alto ↔ cabeza erguida (1) o caída (0)']
  ];
  $('btn-settings').addEventListener('click', () => {
    const conectado = atrilConn.status === 'connected' && st;
    $('settings-fields').innerHTML = FIELDS.map(([k, label, type]) => {
      const t = type || 'number';
      return t === 'checkbox' ? '<label class="check"><input type="checkbox" id="set-' + k + '"> ' + label + '</label>'
        : '<label>' + label + '<input id="set-' + k + '" type="' + t + '" step="any"></label>';
    }).join('') + '<h3 style="grid-column:1/-1;margin-top:.8rem">Parámetros del atril ' + (conectado ? '(se guardan en el ESP32)' : '(conecta el atril para cambiarlos)') + '</h3>' +
      FIELDS_ATRIL.map(([k, label]) => '<label>' + label + '<input id="seta-' + k + '" type="number" step="any"' + (conectado ? '' : ' disabled') + '></label>').join('');
    FIELDS.forEach(([k, , type]) => { const el = $('set-' + k); if (type === 'checkbox') el.checked = !!S[k]; else el.value = S[k]; });
    if (conectado) FIELDS_ATRIL.forEach(([k]) => { $('seta-' + k).value = st.cfg[k]; });
    $('dlg-settings').showModal();
  });
  $('btn-save-settings').addEventListener('click', () => {
    const next = Object.assign({}, S);
    for (const [k, , type] of FIELDS) {
      const el = $('set-' + k);
      if (type === 'checkbox') next[k] = el.checked;
      else if (type === 'text') next[k] = el.value.trim();
      else { const v = parseFloat(el.value); if (!isFinite(v) || v <= 0) { toast('Valor no válido en: ' + k); return; } next[k] = v; }
    }
    const cambios = [];
    if (atrilConn.status === 'connected' && st) {
      for (const [k] of FIELDS_ATRIL) {
        const v = parseFloat($('seta-' + k).value);
        if (!isFinite(v)) { toast('Valor no válido en el parámetro del atril: ' + k); return; }
        if (Math.abs(v - st.cfg[k]) > 1e-9) cambios.push([k, v]);
      }
    }
    const reconnect = next.ipArduino !== S.ipArduino || next.ipAtril !== S.ipAtril || next.emgUsb !== S.emgUsb;
    S = next; E.Store.saveSettings(S); rebuildDsp(); chart.setWindow(S.winS); analisisSig = '';
    cambios.forEach(([k, v]) => tarea('parámetro ' + k, () => atril.setCfg(k, v), s => Math.abs(s.cfg[k] - v) < 0.06));
    $('dlg-settings').close();
    toast(reconnect && realOn() ? 'Ajustes guardados. Vuelve a pulsar «Conectar ASTRA» para usar la nueva conexión.' : cambios.length ? 'Ajustes guardados; parámetros del atril enviados.' : 'Ajustes guardados');
  });
  $('btn-backup').addEventListener('click', () => E.download('backup-emg-' + new Date().toISOString().slice(0, 10) + '.json', E.Store.backup(), 'application/json'));
  $('btn-restore').addEventListener('click', () => $('file-restore').click());
  $('file-restore').addEventListener('change', async e => {
    try { const r = E.Store.restore(await e.target.files[0].text()); toast('Importados: ' + r.patients + ' pacientes, ' + r.sessions + ' sesiones'); renderHome(); }
    catch (err) { toast('No se pudo importar: ' + err.message); }
    e.target.value = '';
  });

  /* ===================================================================
   * Panel de pruebas de la simulación
   * =================================================================== */
  function simPanelSync() {
    if (!sim) return;
    sim.speed = +$('sp-speed').value || 1;
    sim.esc.relacion = $('sp-rel').value; sim.esc.basal = $('sp-basal').value;
    sim.esc.altoMean = +$('sp-alto').value || 150; sim.esc.bajoMean = +$('sp-bajo').value || 30;
    auto.on = $('sp-auto').value === '1'; auto.err = Math.max(0, Math.min(100, +$('sp-err').value || 0)) / 100; auto.demora = Math.max(0, +$('sp-demora').value || 0);
  }
  ['sp-speed', 'sp-rel', 'sp-basal', 'sp-alto', 'sp-bajo', 'sp-auto', 'sp-err', 'sp-demora'].forEach(id => $(id).addEventListener('change', simPanelSync));
  $('sp-up').addEventListener('click', () => { if (sim) sim.setPostura(true, true); });
  $('sp-down').addEventListener('click', () => { if (sim) sim.setPostura(false, true); });
  $('sp-rand').addEventListener('click', () => { if (sim) { sim.esc.prolongado = false; sim.esc.auto = true; sim.esc.resto = 1000; } });
  $('sp-nodata').addEventListener('click', () => { if (sim) { sim.esc.sinDatos = true; setTimeout(() => { if (sim) sim.esc.sinDatos = false; }, 5000); } });
  $('sp-disc').addEventListener('click', () => {
    if (!atril) return;
    const a = atril; a.pausar(true); toast('Página desconectada del atril 15 s (el atril sigue funcionando).');
    setTimeout(() => { if (atril === a) { a.pausar(false); toast('Página reconectada: se resincroniza todo.'); } }, 15000);
  });
  /* La fisioterapeuta simulada responde con lo que «vio» cuando apareció la pregunta (con un % de error) */
  function simAuto(s) {
    if (!sim || !auto.on || !s.ver.pend) return;
    const ev = evStore.get(evKey(s.boot, s.ver.pend));
    if (!ev || ev.status !== 1 || respondidos.has(ev.key) || auto.visto === ev.key) return;
    auto.visto = ev.key;
    const vio = sim.verdadDe(ev.id), err = Math.random() < auto.err;   // lo que vio cuando apareció la pregunta
    const p = (vio !== err) ? 'E' : 'C';
    setTimeout(() => { const e = evStore.get(ev.key); if (e && e.status === 1 && !respondidos.has(e.key)) responder(e, p); }, auto.demora * 1000 / Math.max(1, sim.speed));
  }

  /* ---------- arranque ---------- */
  chart = new E.LiveChart($('live'), S.fs);
  rmsChart = new E.RmsChart($('rms'));
  progChart = new E.ProgressChart($('prog'), $('prog-tip'));
  rebuildDsp();
  $('win-sel').value = String(S.winS);
  (function frame() { if (view === 'session') { chart.draw(); rmsChart.draw(); } requestAnimationFrame(frame); })();
  let tick = 0;
  setInterval(() => {
    const now = performance.now();
    if (now - rx.lastT >= 1000) { rx.fs = (rx.samples - rx.lastCount) * 1000 / (now - rx.lastT); rx.lastCount = rx.samples; rx.lastT = now; }
    renderConn();
    revisarTareas();
    if (view === 'session') { renderSession(); renderNotif(); }
    if (++tick % 2 === 0) evalAlarms(now);
  }, 250);
  window.addEventListener('resize', () => { if (view === 'history') progChart.draw(); });
  window.addEventListener('beforeunload', e => { if (rec) { e.preventDefault(); e.returnValue = ''; } });
  show('home');
  window.__astra = { get st() { return st; }, get sim() { return sim; }, get rec() { return rec; }, evStore, get analisis() { return analisis; }, get propuesta() { return propuesta; }, get ajustes() { return ajustesLog; }, buildRecord: () => buildRecord() };
})();
