/* Interfaz: navegación, pacientes, sesión en vivo (Arduino + atril), historial, ajustes.
 * Quién hace qué:
 *   Arduino UNO R4 WiFi -> señal EMG cruda (solo se dibuja aquí)
 *   Atril ESP32         -> calibración paso a paso, detección, motores, luz y sonido
 *   Página              -> manda los pasos al atril, muestra todo y guarda la sesión
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

  /* ---------- estado global ---------- */
  let S = E.Store.settings();
  let proc, quality, chart, rmsChart, progChart;
  let view = 'home', patient = null;
  let emg = null, emgConn = { status: 'disconnected', msg: '' };       // señal del Arduino
  let atril = null, atrilConn = { status: 'disconnected', msg: '' };   // ESP32 (o simulado)
  let sim = null;                    // { atril: AtrilSim, blk: BlockRms } en modo simulación
  let st = null;                     // último estado del atril (/estado)
  let calPreparada = false;          // se pulsó «Comenzar calibración» en esta sesión
  let rec = null;                    // registro de la sesión en curso (lo que se guarda al finalizar)
  let logLines = [], logDirty = false;
  const rx = { lastData: 0, samples: 0, lastCount: 0, lastT: performance.now(), fs: 0, lostSeen: 0, lostAt: 0, connectedAt: 0, pulse: false };
  const qCount = { sat: 0, flat: 0, noisy: 0 }; let qLast = null;
  let prevCrit = new Set();
  const MIDE = ['REPOSO', 'PASIVA', 'ARRIBA', 'ABAJO'];
  const midiendo = s => !!s && s.modo === 'CALIBRANDO' && MIDE.includes(s.fase);

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
    if (st.modo === 'SESION' && st.activo) return 2;
    if (st.modo === 'CALIBRANDO') return st.fase === 'ARRIBA' ? 2 : MIDE.includes(st.fase) ? 1 : 0;
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
    const e = emg, a = atril; emg = null; atril = null; sim = null; st = null;
    if (a) a.stop();
    if (e) { if (e.stop) e.stop(); else await e.disconnect(); }
    emgConn = { status: 'disconnected', msg: '' }; atrilConn = { status: 'disconnected', msg: '' };
    renderConn(); if (view === 'session') renderCal();
  }
  async function connectAstra() {
    if (S.emgUsb) {                    // hay que pedir el puerto mientras dura el clic
      if (!E.SerialSource.supported()) { alert('Este navegador no soporta Web Serial. Usa Google Chrome o Microsoft Edge en una computadora, o desactiva «Señal EMG por USB» en Ajustes.'); return; }
    }
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
    const asim = new E.AtrilSim(), blk = new E.BlockRms(S);
    sim = { atril: asim, blk };
    atril = new E.AtrilLink(asim, onEstado, onAtrilStatus);
    emg = new E.SimSource(arr => {
      onSamples(arr);
      for (let i = 0; i < arr.length; i++) { const mv = blk.feed(arr[i]); if (mv !== null) asim.feedBlock(mv); }  // el "opto"
    }, onEmgStatus, () => asim.simCtx(), S.fs);
    emg.start();
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
  function onEstado(s) {
    const prev = st; st = s;
    if (s.reinicio) { addLog('--- el atril se reinició ---'); toast('El atril (ESP32) se reinició.'); }
    for (const [v, f] of s.b) rmsChart.push(v, f);
    const cal = s.cal, apl = s.apl;
    rmsChart.lines = {
      reposo: s.modo === 'CALIBRANDO' && s.hechos[0] ? cal.reposo : s.calibrado ? apl.reposo : 0,
      uon: s.calibrado && s.modo !== 'CALIBRANDO' ? s.uon : 0,
      uoff: s.calibrado && s.modo !== 'CALIBRANDO' ? s.uoff : 0
    };
    for (const t of s.logs) addLog(t);
    if (rec) recordUpdate(s);
    // La sesión del atril empezó (botón «Iniciar sesión», BOOT o tecla s): se registra
    if (!rec && view === 'session' && patient && prev && prev.modo === 'CALIBRANDO' && s.modo === 'SESION') startRecording(s);
    renderConn();
    if (view === 'session') renderCal();
  }

  /* ---------- registro de la sesión ---------- */
  const keyOf = (s, sid) => s.boot + ':' + (sid === undefined ? s.sid : sid);
  function snapshotCal(s) {
    const a = s.apl;
    return { origen: 'atril', reposo: a.reposo, sd: a.sd, restRms: a.restRms, pasMedia: a.pasMedia, pasPico: a.pasPico,
      actRms: a.actRms, actEnv: a.actEnv, snr: a.snr, art: a.art, maximo: a.maximo, uon: s.uon, uoff: s.uoff, umbralPct: s.pct,
      repRms: s.cal.repRms, fecha: new Date().toISOString() };
  }
  function startRecording(s) {
    rec = { start: new Date().toISOString(), key: keyOf(s), t0: s.tSes, base: 0, last: s.tSes, minId: s.nAct,
      acts: [], seen: new Set(), rms: [], cal: snapshotCal(s), recal: 0 };
    localStorage.setItem('emg.fisio', $('m-fisio').value);
    toast('Sesión en curso: se registran las activaciones del atril.');
  }
  const recTime = () => (rec ? rec.base + rec.last - rec.t0 : 0);
  function recordUpdate(s) {
    const take = () => {
      for (const [sid, id, ini, dur, rms, pct] of s.acts) {
        const k = keyOf(s, sid), uid = k + '#' + id;
        if (k !== rec.key || id <= rec.minId || rec.seen.has(uid)) continue;
        rec.seen.add(uid);
        rec.acts.push({ n: rec.acts.length + 1, inicioS: rec.base + ini - rec.t0, durS: dur, rms, pct });
      }
    };
    take();
    const enSesion = s.modo === 'SESION' || s.modo === 'PAUSA';
    if (enSesion && keyOf(s) !== rec.key) {            // recalibraron: sigue la misma terapia con la sesión nueva del atril
      rec.base += rec.last - rec.t0; rec.key = keyOf(s); rec.t0 = 0; rec.last = s.tSes; rec.minId = 0;
      rec.cal = snapshotCal(s); rec.recal++;
      take();
    }
    if (enSesion && keyOf(s) === rec.key) rec.last = s.tSes;
    if (s.modo === 'SESION' && keyOf(s) === rec.key) {
      const t = recTime(), n = s.b.length;
      s.b.forEach(([v, f], i) => rec.rms.push([+(t - (n - 1 - i) * 0.1).toFixed(1), v, f === 1 ? 1 : 0]));
    }
  }
  function liveAct() {
    return rec && st && st.modo === 'SESION' && st.activo && keyOf(st) === rec.key ? st.durAct : 0;
  }

  /* ---------- alarmas ---------- */
  function beep() {
    if (!S.sound) return;
    try {
      const a = new (window.AudioContext || window.webkitAudioContext)(), o = a.createOscillator(), g = a.createGain();
      o.frequency.value = 880; g.gain.value = 0.15; o.connect(g); g.connect(a.destination); o.start(); o.stop(a.currentTime + 0.35);
      setTimeout(() => a.close(), 600);
    } catch (e) { /* sin audio */ }
  }
  function evalAlarms(now) {
    const A = {}, inSes = view === 'session';
    // Atril
    const ac = atrilConn.status;
    if (ac === 'error') A.atril = ['crit', 'El atril (ESP32) no responde en ' + S.ipAtril + ': ' + atrilConn.msg + '. ¿Está encendido y la laptop conectada a la red WiFi «ASTRA»?'];
    else if (ac === 'disconnected' && inSes) A.atril = ['crit', 'Atril sin conexión. Pulsa «Conectar ASTRA» (o «Simulación» para probar).'];
    if (st) {
      if (st.modo === 'PARO') A.paro = ['crit', 'PARO DE EMERGENCIA en el atril.'];
      else if (!st.datos && st.modo !== 'ARRANQUE') A.opto = ['crit', 'El atril no recibe el RMS del Arduino (opto). Revisa que el Arduino esté encendido y el cable del opto.' + (midiendo(st) ? ' La calibración espera hasta que vuelvan los datos.' : '')];
      if (!st.estOK) A.tope = ['warn', 'Estrellas desactivadas: el atril no encontró el tope (microswitch).'];
    }
    // Señal EMG (solo la gráfica)
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
    for (const k of crit) if (!prevCrit.has(k)) { beep(); break; }
    prevCrit = crit;
  }

  /* ===================================================================
   * Sesión
   * =================================================================== */
  function startSession() {
    rec = null; calPreparada = false;
    $('s-name').textContent = fullName(patient);
    $('s-dob').textContent = 'Nacimiento: ' + fmtDate(patient.nacimiento) + ' (' + ageText(patient.nacimiento) + ') · GMFCS ' + patient.gmfcs;
    ['m-electrodo', 'm-posicion', 'm-notas'].forEach(i => { $(i).value = ''; });
    $('m-fisio').value = localStorage.getItem('emg.fisio') || '';
    show('session'); renderSession();
    if (!atril) toast('Conecta el sistema ASTRA (o activa la simulación) para empezar.');
  }

  // Botones de la calibración (cada uno manda un comando al atril)
  $('btn-cal-start').addEventListener('click', () => { calPreparada = true; sendCmd('c'); renderCal(); });
  $('cal-steps').querySelectorAll('[data-cmd]').forEach(b => b.addEventListener('click', () => sendCmd(b.dataset.cmd)));
  $('btn-phase-cancel').addEventListener('click', () => sendCmd('x'));
  $('btn-run-start').addEventListener('click', () => sendCmd('s'));
  $('btn-cal-redo').addEventListener('click', () => { if (confirm('¿Borrar las tres fases y calibrar de nuevo?')) { calPreparada = true; sendCmd('c'); } });
  $('btn-attach').addEventListener('click', () => { if (st) { startRecording(st); renderCal(); } });
  $('btn-attach-cal').addEventListener('click', () => { calPreparada = true; sendCmd('c'); });
  $('btn-pause').addEventListener('click', () => sendCmd('p'));
  $('btn-thr-down').addEventListener('click', () => sendCmd('-'));
  $('btn-thr-up').addEventListener('click', () => sendCmd('+'));
  $('btn-recal').addEventListener('click', () => {
    if (confirm('¿Recalibrar? El atril deja la sesión y repite reposo, pasiva y activa. El tiempo de la terapia se detiene mientras tanto y luego continúa.')) { calPreparada = true; sendCmd('c'); }
  });
  $('btn-resume').addEventListener('click', () => sendCmd('r'));

  const CUE = {
    REPOSO: ['REPOSO', 'Cabeza apoyada y relajada. No mover al niño.', 'rest'],
    PASIVA: ['PASIVA', 'El terapeuta mueve la cabeza de abajo hacia arriba y viceversa. El niño NO hace fuerza.', 'passive'],
    ARRIBA: ['¡ARRIBA!', 'El niño levanta la cabeza y la sostiene.', 'up'],
    ABAJO: ['ABAJO', 'Baja la cabeza y relaja.', 'down']
  };
  function setMsg(text, cls, showResume) {
    $('cal-msg').hidden = !text; $('cal-msg').textContent = text || ''; $('cal-msg').className = 'callout ' + (cls || '');
    $('cal-msg-actions').hidden = !showResume;
  }
  function renderCal() {
    const on = atrilConn.status === 'connected' && st;
    let intro = false, steps = false, result = false, run = false, attach = false;
    const badge = $('atril-badge');
    badge.className = 'badge'; badge.textContent = on ? st.modo + (st.modo === 'CALIBRANDO' ? ' · ' + st.fase : '') : 'sin conexión';
    const cls = on ? { PARO: 'bad', SESION: 'ok', CALIBRANDO: 'warn' }[st.modo] : null;
    if (cls) badge.classList.add(cls);
    setMsg('');
    if (!on) setMsg(atrilConn.status === 'connecting' || atrilConn.status === 'connected' ? 'Conectando con el atril…' : 'Conecta el atril («Conectar ASTRA» o «Simulación») para calibrar.', 'warn');
    else if (st.modo === 'ARRANQUE') setMsg('El atril está buscando el tope de las estrellas. La calibración se habilita al terminar.');
    else if (st.modo === 'PARO') setMsg('Paro de emergencia. Suelta el hongo y pulsa «Reanudar el atril».', 'bad', true);
    else if (st.modo === 'CALIBRANDO') {
      const started = calPreparada || st.hechos.some(Boolean) || midiendo(st) || st.fase === 'LISTA' || st.fase === 'FALLIDA';
      intro = !started; steps = started; result = started && (st.fase === 'LISTA' || st.fase === 'FALLIDA');
    } else if (rec) run = true;
    else attach = true;

    $('cal-intro').hidden = !intro; $('cal-steps').hidden = !steps; $('cal-result').hidden = !result;
    $('run-ctrl').hidden = !run; $('attach').hidden = !attach;
    $('cal-title').textContent = run ? 'Sesión en el atril' : 'Calibración en el atril';
    $('meta-box').open = !rec;
    if (steps) renderSteps();
    if (result) renderResult();
    if (run) {
      const a = st.apl;
      $('cal-compact').textContent = 'U_on ' + f1(st.uon) + ' mV · U_off ' + f1(st.uoff) + ' mV · umbral mínimo ' + st.pct + ' % de la activa · reposo ' + f1(a.reposo) +
        ' mV · referencia ' + f1(a.actRms) + ' mV' + (rec.recal ? ' · recalibrado ' + rec.recal + ' vez/veces' : '');
      $('btn-pause').textContent = st.modo === 'PAUSA' ? '▶ Seguir' : '❚❚ Pausa';
    }
    if (attach) $('attach-thr').textContent = 'U_on ' + f1(st.uon) + ' mV, sesión ' + st.sid + ' del atril';
    $('btn-csv').disabled = !rec; $('btn-finish').disabled = !rec;
  }
  function renderSteps() {
    const s = st, c = s.cal, cfg = s.cfg, bps = cfg.bps || 10, mid = midiendo(s);
    const cur = s.fase === 'REPOSO' ? 0 : s.fase === 'PASIVA' ? 1 : (s.fase === 'ARRIBA' || s.fase === 'ABAJO') ? 2 : -1;
    const next = !s.hechos[0] ? 0 : !s.hechos[1] ? 1 : (!s.hechos[2] || s.fase === 'FALLIDA') ? 2 : -1;
    const durs = [cfg.reposo + ' s', cfg.pasiva + ' s', cfg.reps + ' × (' + cfg.arriba + ' s + ' + cfg.abajo + ' s)'];
    const labels = ['Comenzar reposo', 'Comenzar calibración pasiva', 'Comenzar calibración activa'];
    const activeTotal = cfg.reps * (cfg.arriba + cfg.abajo) * bps;
    const activeDone = s.fase === 'ARRIBA' || s.fase === 'ABAJO' ? ((s.rep - 1) * (cfg.arriba + cfg.abajo) * bps + (s.fase === 'ABAJO' ? cfg.arriba * bps : 0) + s.bloq) : 0;
    const reps = (c.repRms || []).filter(v => v > 0);
    const res = [
      s.hechos[0] ? 'Media ' + f1(c.reposo) + ' mV · SD ' + f1(c.sd) + ' mV' : '',
      s.hechos[1] ? 'Media ' + f1(c.pasMedia) + ' mV · pico ' + f1(c.pasPico) + ' mV' : '',
      s.hechos[2] ? 'RMS de referencia ' + f1(c.actRms) + ' mV · envolvente ' + f1(c.actEnv) + ' mV' : (reps.length ? 'Repeticiones: ' + reps.map(f0).join(', ') + ' mV' : '')
    ];
    for (let i = 0; i < 3; i++) {
      const li = $('step-' + i), btn = li.querySelector('button');
      li.className = cur === i ? 'active' : (s.hechos[i] && !(i === 2 && s.fase === 'FALLIDA')) ? 'done' : i === next ? 'next' : 'pending';
      let time = durs[i];
      if (cur === i) time = i < 2 ? Math.ceil((s.total - s.bloq) / bps) + ' s' : Math.ceil((activeTotal - activeDone) / bps) + ' s · rep ' + s.rep + '/' + s.reps;
      li.querySelector('.st-time').textContent = time;
      li.querySelector('.step-res').textContent = res[i];
      btn.hidden = mid;
      btn.disabled = !(i === next || s.hechos[i]);
      btn.textContent = i === next ? labels[i] : s.hechos[i] ? 'Repetir' : labels[i];
      btn.className = 'btn' + (i === next ? '' : ' ghost');
    }
    // Indicación grande mientras se mide
    $('cue').hidden = !mid;
    if (mid) {
      const [title, text, cls] = CUE[s.fase];
      $('cue').className = 'cue ' + cls;
      $('cue-title').textContent = title + (cur === 2 ? ' (' + s.rep + '/' + s.reps + ')' : '');
      $('cue-text').textContent = text;
      $('cue-count').textContent = Math.max(0, Math.ceil((s.total - s.bloq) / bps)) + ' s';
      $('cue-bar').style.width = (100 * (cur === 2 ? activeDone / activeTotal : s.bloq / Math.max(1, s.total))) + '%';
      let note;
      if (s.fase === 'REPOSO') note = s.bloq <= cfg.descR * bps ? 'Descartando el primer ' + cfg.descR + ' s (los filtros se asientan)…' : 'Midiendo el reposo…';
      else if (s.fase === 'PASIVA') note = s.bloq <= cfg.descP * bps ? 'Descartando el primer ' + cfg.descP + ' s…' : 'Midiendo el artefacto del movimiento pasivo…';
      else if (s.fase === 'ARRIBA') note = s.bloq <= cfg.descIni * bps ? 'Transición abajo → arriba: todavía no se mide (' + cfg.descIni + ' s)…'
        : s.bloq > (cfg.arriba - cfg.descFin) * bps ? 'Últimos ' + cfg.descFin + ' s: ya no se mide…' : 'Midiendo la contracción sostenida…';
      else note = 'Descanso: no se mide.';
      if (!s.datos) note = 'El atril no recibe datos del Arduino: la fase está detenida hasta que vuelvan.';
      $('cue-note').textContent = note + ' · RMS ahora: ' + s.rms + ' mV';
    }
  }
  function renderResult() {
    const s = st, c = s.cal, ok = s.fase === 'LISTA';
    $('cal-result-title').textContent = ok ? 'Calibración lista.' : 'La calibración no es válida.';
    $('cal-result-sub').textContent = ok ? 'Revisa los valores y pulsa «Iniciar sesión».' : 'Repite la fase activa o toda la calibración.';
    const items = [['Reposo (media)', f1(c.reposo) + ' mV'], ['Ruido en reposo (SD)', f1(c.sd) + ' mV'], ['Pasiva: pico', f1(c.pasPico) + ' mV'],
      ['Activa: RMS de referencia', f1(c.actRms) + ' mV'], ['Activa: envolvente', f1(c.actEnv) + ' mV'], ['Relación señal/reposo', f1(c.snr) + '×'],
      ['Artefacto pasivo / activa', f0(c.art) + ' %']];
    if (ok) items.push(['U_on (inicio de activación)', f1(s.uon) + ' mV'], ['U_off (fin de activación)', f1(s.uoff) + ' mV']);
    $('cal-values').innerHTML = items.map(() => '<div><span></span><b></b></div>').join('');
    $('cal-values').querySelectorAll('div').forEach((d, i) => { d.firstChild.textContent = items[i][0]; d.lastChild.textContent = items[i][1]; });
    const w = [];
    if (!ok) w.push('No se detectó contracción activa. Revisa electrodos y repite.');
    else {
      if (c.snr < 3) w.push('La contracción apenas se distingue del reposo (SNR ' + f1(c.snr) + '). Revisa electrodos.');
      if (c.art > 80) w.push('El movimiento pasivo produce una señal casi tan grande como la activa (' + f0(c.art) + ' %). Las transiciones podrían contarse como activaciones; sube el umbral o mejora la fijación de cables.');
      if (s.uoff < c.reposo) w.push('U_off (' + f1(s.uoff) + ' mV) quedó por debajo del reposo (' + f1(c.reposo) + ' mV): una activación podría no terminar nunca. Prueba «Umbral +» o revisa el ruido de los electrodos.');
    }
    $('cal-warnings').innerHTML = w.map(() => '<li></li>').join('');
    $('cal-warnings').querySelectorAll('li').forEach((li, i) => { li.textContent = w[i]; });
    $('btn-run-start').disabled = !ok;
  }

  /* ---------- UI de sesión (4 Hz; las gráficas van a 60 fps aparte) ---------- */
  function renderSession() {
    const t = recTime(), live = liveAct();
    $('s-time').textContent = $('k-time').textContent = clock(t);
    const acts = rec ? rec.acts : [];
    const totalActive = acts.reduce((a, s) => a + s.durS, 0) + live;
    $('k-count').textContent = acts.length + (live ? 1 : 0);
    $('k-disc').textContent = live ? 'Una en curso' : '';
    $('k-active').textContent = f1(totalActive) + ' s';
    $('k-pct').textContent = t > 1 ? Math.round(100 * totalActive / t) + ' % del tiempo de sesión' : '';
    $('k-max').textContent = f1(Math.max(0, live, ...acts.map(a => a.durS))) + ' s';
    $('k-cur').textContent = live ? 'En curso: ' + f1(live) + ' s' : (acts.length ? 'Última: ' + f1(acts[acts.length - 1].durS) + ' s' : '');
    $('k-rms').textContent = acts.length ? Math.round(acts.reduce((a, s) => a + s.pct, 0) / acts.length) + ' %' : '—';
    $('k-cal').textContent = rec ? 'sin contar pausas ni recalibraciones' : 'empieza con «Iniciar sesión»';
    // Lecturas del atril
    if (st) {
      const a = st.apl, span = Math.max(a.maximo - a.reposo, 1);
      const items = [['RMS', st.rms + ' mV'], ['Suavizado', f1(st.suave) + ' mV'], ['Estado', st.modo === 'SESION' ? (st.activo ? 'ACTIVACIÓN' : 'reposo') : st.modo],
        ['Estrellas', st.estOK ? st.alt + ' %' : 'desactivadas'], ['WiFi del atril', st.rssi + ' dBm']];
      $('readouts').innerHTML = items.map(() => '<span><span></span>: <b></b></span>').join('');
      $('readouts').querySelectorAll(':scope > span').forEach((el, i) => {
        el.firstChild.textContent = items[i][0]; el.lastChild.textContent = items[i][1];
        if (i === 2 && st.activo) el.lastChild.className = 'on';
      });
      const sesion = st.modo === 'SESION' && st.calibrado;
      $('gauge-fill').style.width = sesion ? st.nivel + '%' : '0';
      $('gauge-val').textContent = sesion ? st.nivel + ' %' : '—';
      $('gauge-mark').style.left = Math.min(100, Math.max(0, 100 * (st.uon - a.reposo) / span)) + '%';
    } else {
      $('readouts').textContent = ''; $('gauge-fill').style.width = '0'; $('gauge-val').textContent = '—';
    }
    renderTable(acts, live);
    if (logDirty && $('log-card').open) {
      logDirty = false;
      const el = $('log'), atEnd = el.scrollTop + el.clientHeight >= el.scrollHeight - 20;
      el.textContent = logLines.join('\n');
      if (atEnd) el.scrollTop = el.scrollHeight;
    }
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
    const t = recTime(), acts = rec.acts, tot = acts.reduce((a, s) => a + s.durS, 0);
    return {
      patientId: patient.id, fecha: rec.start, duracionS: t, origen: 'atril',
      electrodo: $('m-electrodo').value.trim(), posicion: $('m-posicion').value.trim(), fisio: $('m-fisio').value.trim(), notas: $('m-notas').value.trim(),
      calibracion: rec.cal, umbralMV: rec.cal.uon, recalibraciones: rec.recal,
      resumen: { activaciones: acts.length, tiempoActivoS: tot, pctActivo: t > 0 ? 100 * tot / t : 0, maxContrS: Math.max(0, ...acts.map(a => a.durS)),
        medContrS: acts.length ? tot / acts.length : 0, rmsNormMedio: acts.length ? acts.reduce((a, s) => a + s.pct, 0) / acts.length : null, descartadas: 0 },
      activaciones: acts.map(a => ({ n: a.n, inicioS: a.inicioS, finS: a.inicioS + a.durS, durS: a.durS, rms: a.rms, mav: null, rmsNorm: a.pct }))
    };
  }
  function activationsCSV(rec, p) {
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha_sesion', 'activacion', 'inicio_s', 'fin_s', 'duracion_s', 'rms_medio_mV', 'pct_referencia', 'reposo_mV', 'referencia_mV', 'umbral_on_mV'];
    const c = rec.calibracion || {};
    const rows = rec.activaciones.map(a => [fullName(p), p.expediente || '', p.gmfcs, rec.fecha, a.n, a.inicioS, a.finS, a.durS, a.rms, a.rmsNorm,
      c.reposo !== undefined ? c.reposo : c.restMean, c.actRms !== undefined ? c.actRms : c.activeRms, rec.umbralMV]);
    E.download(E.slug(fullName(p)) + '_' + rec.fecha.slice(0, 16).replace(/[:T]/g, '-') + '_activaciones.csv', E.toCSV(H, rows));
  }
  $('btn-csv').addEventListener('click', () => {
    if (!rec) return;
    const r = buildRecord(), stamp = r.fecha.slice(0, 16).replace(/[:T]/g, '-');
    activationsCSV(r, patient);
    E.download(E.slug(fullName(patient)) + '_' + stamp + '_rms_atril.csv', E.toCSV(['tiempo_s', 'rms_mV', 'activacion'], rec.rms));
    toast('CSV descargados (activaciones + RMS del atril)');
  });
  $('btn-finish').addEventListener('click', async () => {
    if (!rec || !confirm('¿Finalizar la terapia y guardar la sesión en el historial? El atril queda en pausa.')) return;
    const r = buildRecord();
    if (!E.Store.saveSession(r)) { toast('No se pudo guardar en el navegador. Descarga el CSV.'); return; }
    if (st && st.modo === 'SESION') sendCmd('p');
    if (S.sheetsUrl) {
      try {
        await E.sendToSheets(S.sheetsUrl, { tipo: 'sesion', paciente: S.sheetsName ? fullName(patient) : (patient.expediente || patient.id), gmfcs: patient.gmfcs,
          fecha: r.fecha, duracionS: r.duracionS, electrodo: r.electrodo, posicion: r.posicion, fisio: r.fisio, notas: r.notas, resumen: r.resumen, activaciones: r.activaciones });
      } catch (e) { toast('Sesión guardada, pero no se pudo enviar a Google Sheets.'); }
    }
    rec = null; calPreparada = false; toast('Sesión guardada en el historial'); show('history');
  });

  /* ---------- historial ---------- */
  const METRICS = { pctActivo: ['% del tiempo', '%'], maxContrS: ['Contracción más larga', 's'], medContrS: ['Duración media', 's'], activaciones: ['Activaciones', ''],
    tiempoActivoS: ['Tiempo activo', 's'], rmsNormMedio: ['Esfuerzo medio', '%'] };
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
      b.className = 'btn small ghost'; b.textContent = 'CSV'; b.addEventListener('click', () => activationsCSV(s, p));
      d.className = 'btn small ghost'; d.textContent = '🗑'; d.title = 'Eliminar sesión';
      d.addEventListener('click', () => { if (confirm('¿Eliminar esta sesión del historial?')) { E.Store.deleteSession(s.id); renderHistory(); } });
      td.append(b, ' ', d); tr.appendChild(td); tb.appendChild(tr);
    });
  }
  $('h-metric').addEventListener('change', renderHistory);
  $('btn-hist-csv').addEventListener('click', () => {
    const ses = E.Store.sessionsFor(patient.id);
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha', 'duracion_s', 'activaciones', 'tiempo_activo_s', 'pct_activo', 'max_contraccion_s', 'media_contraccion_s', 'esfuerzo_medio_pct', 'electrodo', 'posicion', 'fisio', 'notas'];
    E.download(E.slug(fullName(patient)) + '_historial.csv', E.toCSV(H, ses.map(s => [fullName(patient), patient.expediente || '', patient.gmfcs, s.fecha, s.duracionS, s.resumen.activaciones, s.resumen.tiempoActivoS, s.resumen.pctActivo, s.resumen.maxContrS, s.resumen.medContrS, s.resumen.rmsNormMedio, s.electrodo, s.posicion, s.fisio, s.notas])));
  });

  /* ---------- ajustes ---------- */
  const FIELDS = [
    ['ipArduino', 'IP del Arduino (red WiFi «ASTRA»)', 'text'], ['ipAtril', 'IP del atril (ESP32)', 'text'],
    ['emgUsb', 'Señal EMG por USB (Web Serial) en vez de WiFi', 'checkbox'],
    ['fs', 'Frecuencia de muestreo (Hz) — igual que FS del Arduino'], ['adcMax', 'Máximo del ADC (14 bits = 16383)'], ['vref', 'Referencia ADC (mV)'],
    ['gain', 'Ganancia (1 = mV a la salida de Nova)'], ['hpHz', 'Pasa altas para ver la señal (Hz)'], ['notchHz', 'Frecuencia de la red (Hz)'], ['nArm', 'Notch: n.º de armónicos (7, como el Arduino)'],
    ['sheetsUrl', 'URL de Google Sheets (Apps Script)', 'text'], ['sheetsName', 'Enviar nombre del paciente a Sheets', 'checkbox'], ['sound', 'Sonido en alarmas críticas', 'checkbox']
  ];
  $('btn-settings').addEventListener('click', () => {
    $('settings-fields').innerHTML = FIELDS.map(([k, label, type]) => {
      const t = type || 'number';
      return t === 'checkbox' ? '<label class="check"><input type="checkbox" id="set-' + k + '"> ' + label + '</label>'
        : '<label>' + label + '<input id="set-' + k + '" type="' + t + '" step="any"></label>';
    }).join('');
    FIELDS.forEach(([k, , type]) => { const el = $('set-' + k); if (type === 'checkbox') el.checked = !!S[k]; else el.value = S[k]; });
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
    const reconnect = next.ipArduino !== S.ipArduino || next.ipAtril !== S.ipAtril || next.emgUsb !== S.emgUsb;
    S = next; E.Store.saveSettings(S); rebuildDsp(); chart.setWindow(S.winS);
    $('dlg-settings').close();
    toast(reconnect && realOn() ? 'Ajustes guardados. Vuelve a pulsar «Conectar ASTRA» para usar la nueva conexión.' : 'Ajustes guardados');
  });
  $('btn-backup').addEventListener('click', () => E.download('backup-emg-' + new Date().toISOString().slice(0, 10) + '.json', E.Store.backup(), 'application/json'));
  $('btn-restore').addEventListener('click', () => $('file-restore').click());
  $('file-restore').addEventListener('change', async e => {
    try { const r = E.Store.restore(await e.target.files[0].text()); toast('Importados: ' + r.patients + ' pacientes, ' + r.sessions + ' sesiones'); renderHome(); }
    catch (err) { toast('No se pudo importar: ' + err.message); }
    e.target.value = '';
  });

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
    if (view === 'session') renderSession();
    if (++tick % 2 === 0) evalAlarms(now);
  }, 250);
  window.addEventListener('resize', () => { if (view === 'history') progChart.draw(); });
  window.addEventListener('beforeunload', e => { if (rec) { e.preventDefault(); e.returnValue = ''; } });
  show('home');
})();
