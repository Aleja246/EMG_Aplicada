/* Interfaz: navegación, pacientes, sesión en vivo, historial, ajustes. */
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
  let proc, quality, chart, progChart;
  let view = 'home', patient = null;
  let mode = 'idle';                 // idle | calibrating | calibrated | running
  let cal = null, calResult = null, thr = null, det = null, calSource = '';
  let runSamples = 0, acts = [], discarded = 0, envT = [], envV = [], sessionStart = null;
  let source = null, conn = { status: 'disconnected', msg: '' };
  const rx = { lastData: 0, samples: 0, lastCount: 0, lastT: performance.now(), fs: 0, lostSeen: 0, lostAt: 0, connectedAt: 0, pulse: false };
  const qCount = { sat: 0, flat: 0, noisy: 0 }; let qLast = null;
  let prevCrit = new Set();

  function rebuildDsp() {
    proc = new E.Processor(S);
    quality = new E.SignalQuality(S.adcMax);
    if (chart) chart.fs = S.fs;
  }

  /* ---------- navegación ---------- */
  function show(name) {
    view = name;
    document.querySelectorAll('.view').forEach(v => { v.hidden = v.id !== 'view-' + name; });
    window.scrollTo(0, 0);
    if (name === 'home') renderHome();
    if (name === 'search') renderSearch();
    if (name === 'history') renderHistory();
    if (name === 'session') chart.setWindow(S.winS);
  }
  function goHome() {
    if (view === 'session' && mode === 'running' && !confirm('La terapia sigue en curso. ¿Salir sin finalizar? Los datos de esta sesión se perderán.')) return;
    if (view === 'session') resetSession();
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
  let selected = null, sugIdx = -1;
  function renderSearch() {
    const q = norm($('search-input').value).split(/\s+/).filter(Boolean);
    const list = E.Store.patients().filter(p => q.every(t => norm(fullName(p)).includes(t)))
      .sort((a, b) => fullName(a).localeCompare(fullName(b))).slice(0, 8);
    const ul = $('suggestions'); ul.innerHTML = '';
    list.forEach((p, i) => {
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
        ['Sesiones', ses.length ? ses.length + ' (última: ' + fmtDate(ses[ses.length - 1].fecha) + ')' : 'Ninguna']];
      $('pd-body').innerHTML = rows.filter(r => r[1]).map(r => '<dt>' + r[0] + '</dt><dd></dd>').join('');
      $('pd-body').querySelectorAll('dd').forEach((dd, i) => { dd.textContent = rows.filter(r => r[1])[i][1]; });
    }
  }
  $('search-input').addEventListener('input', () => { selected = null; renderSearch(); });
  $('search-input').addEventListener('keydown', e => {
    if (e.key === 'Enter') { const li = $('suggestions').querySelector('li:not(.muted)'); if (li) li.click(); }
  });
  $('btn-history').addEventListener('click', () => { patient = selected; show('history'); });
  $('btn-start-therapy').addEventListener('click', () => { patient = selected; startSession(); });

  /* ---------- conexión ---------- */
  function onStatus(status, msg) {
    conn = { status, msg: msg || '' };
    if (status === 'connected') { rx.connectedAt = performance.now(); rx.lastData = performance.now(); rx.lostSeen = 0; qCount.sat = qCount.flat = qCount.noisy = 0; }
    if (status === 'error') toast('Conexión perdida');
    renderConn();
  }
  function onSamples(arr) {
    rx.lastData = performance.now(); rx.samples += arr.length; rx.pulse = true;
    const fs = S.fs;
    for (let i = 0; i < arr.length; i++) {
      const raw = arr[i], x = proc.process(raw);
      quality.feed(raw, proc.hpOut, x);
      let flag = 0;
      if (mode === 'calibrating') {
        cal.feed(x, proc.env);
        if (cal.done) onCalibrationDone();
      } else if (mode === 'running') {
        const t = runSamples++ / fs;
        const seg = det.feed(x, proc.env, t);
        if (seg) onSegment(seg);
        if (det.active) flag = (t - det.startT) >= S.minDur ? 2 : 1;
        if (runSamples % 40 === 0) { envT.push(t); envV.push(proc.env); }
      }
      chart.push(x, proc.env, flag);
    }
  }
  async function connectArduino() {
    if (source) await disconnect();
    if (!E.SerialSource.supported()) { alert('Este navegador no soporta Web Serial. Usa Google Chrome o Microsoft Edge en una computadora.'); return; }
    rebuildDsp();
    source = new E.SerialSource(onSamples, onStatus);
    try { await source.connect(); }
    catch (e) { source = null; onStatus('disconnected'); if (e.name !== 'NotFoundError') toast('No se pudo abrir el puerto: ' + (e.message || e.name) + '. ¿Está abierto el Monitor Serie de Arduino?'); }
  }
  function startSim() {
    if (source) disconnect();
    rebuildDsp();
    source = new E.SimSource(onSamples, onStatus, () => {
      if (mode === 'calibrating' && cal) { const i = cal.info(); return { phase: i.phase, cueUp: i.cue === 'up' }; }
      return { phase: mode === 'running' ? 'running' : 'idle' };
    }, S.fs);
    source.start();
  }
  async function disconnect() {
    const s = source; source = null;
    if (s) { if (s.stop) s.stop(); else await s.disconnect(); }
    conn = { status: 'disconnected', msg: '' }; renderConn();
  }
  $('btn-connect').addEventListener('click', () => (conn.status === 'connected' && source && source.kind === 'arduino') ? disconnect() : connectArduino());
  $('btn-sim').addEventListener('click', () => (conn.status === 'connected' && source && source.kind !== 'arduino') ? disconnect() : startSim());
  $('sim-fault').addEventListener('change', e => { if (source && source.setFault) source.setFault(e.target.value || null); });
  if (navigator.serial) navigator.serial.addEventListener('disconnect', () => { if (source && source.kind === 'arduino') onStatus('error', 'Se desconectó el cable USB'); });

  function renderConn() {
    const chip = $('conn-chip'), t = $('conn-text'), c = conn.status;
    const live = source && source.kind === 'arduino' ? 'Arduino' : 'Simulación';
    chip.className = 'chip';
    if (c === 'connected') {
      const stale = performance.now() - rx.lastData > 1000;
      chip.classList.add(stale ? 'bad' : (rx.fs && Math.abs(rx.fs - S.fs) > S.fs * 0.1 ? 'warn' : 'ok'));
      t.textContent = live + (stale ? ': SIN DATOS' : ': conectado · ' + Math.round(rx.fs) + ' Hz');
    } else if (c === 'connecting') { chip.classList.add('warn'); t.textContent = 'Conectando…'; }
    else if (c === 'error') { chip.classList.add('bad'); t.textContent = 'Arduino: error de conexión'; }
    else t.textContent = 'Arduino: sin conexión';
    const ard = source && source.kind === 'arduino' && c === 'connected', sim = source && source.kind !== 'arduino' && c === 'connected';
    $('btn-connect').textContent = ard ? 'Desconectar' : 'Conectar Arduino';
    $('btn-sim').textContent = sim ? 'Detener simulación' : 'Simulación';
    $('btn-sim').classList.toggle('on', !!sim);
    $('sim-fault').hidden = !sim; if (!sim) $('sim-fault').value = '';
    if (rx.pulse) { rx.pulse = false; const d = $('conn-dot'); d.classList.remove('pulse'); void d.offsetWidth; d.classList.add('pulse'); }
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
    const A = {};
    const c = conn.status;
    if (c === 'error') A.conn = ['crit', 'Conexión perdida: ' + conn.msg + '. Revisa el cable USB y vuelve a conectar.'];
    else if (c === 'disconnected' && view === 'session') A.conn = ['crit', 'Arduino no conectado. Pulsa «Conectar Arduino» (o «Simulación» para probar).'];
    if (c === 'connected') {
      const silent = now - rx.lastData;
      if (silent > 1000) A.nodata = ['crit', 'Sin datos desde hace ' + Math.round(silent / 1000) + ' s. Revisa el cable USB, que la placa esté encendida y que el Monitor Serie esté cerrado.'];
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
        else if (qCount.noisy >= 2) A.noisy = ['warn', 'Mucho ruido de 60 Hz (' + Math.round(qLast.ratio60 * 100) + '% de la energía). Revisa contacto de electrodos, referencia y cables.'];
        if (source && source.lost > rx.lostSeen) { rx.lostSeen = source.lost; rx.lostAt = now; }
        if (now - rx.lostAt < 5000 && rx.lostAt) A.lost = ['warn', 'Se están perdiendo paquetes de datos (' + (source ? source.lost : 0) + ' en total). Evita hubs USB y cierra otros programas.'];
        if (now - rx.connectedAt > 4000 && rx.fs && Math.abs(rx.fs - S.fs) > S.fs * 0.1) A.rate = ['warn', 'Frecuencia de muestreo medida ' + Math.round(rx.fs) + ' Hz (esperada ' + S.fs + ' Hz). Revisa que el firmware coincida con Ajustes.'];
      }
    }
    if (mode === 'running' && cal && !A.conn && c !== 'connected') A.run = ['crit', 'La sesión sigue abierta pero no llegan datos.'];
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

  /* ---------- sesión ---------- */
  function resetSession() {
    mode = 'idle'; cal = null; calResult = null; thr = null; det = null; calSource = '';
    runSamples = 0; acts = []; discarded = 0; envT = []; envV = []; sessionStart = null;
    chart.thr = null;
  }
  function startSession() {
    resetSession();
    $('s-name').textContent = fullName(patient);
    $('s-dob').textContent = 'Nacimiento: ' + fmtDate(patient.nacimiento) + ' (' + ageText(patient.nacimiento) + ') · GMFCS ' + patient.gmfcs;
    ['m-electrodo', 'm-posicion', 'm-notas'].forEach(i => { $(i).value = ''; });
    $('m-fisio').value = localStorage.getItem('emg.fisio') || '';
    const prev = E.Store.sessionsFor(patient.id).filter(s => s.calibracion).pop();
    $('btn-cal-reuse').hidden = !prev;
    $('cal-plan').textContent = 'Plan: Reposo ' + S.restS + ' s · Pasiva ' + S.passiveS + ' s · Activa ' + S.reps + ' repeticiones (' + S.holdS + ' s arriba / ' + S.relaxS + ' s abajo).';
    show('session'); applyMode(); renderSession();
    if (conn.status !== 'connected') toast('Conecta el Arduino (o activa la simulación) para empezar.');
  }
  function applyMode() {
    $('cal-intro').hidden = mode !== 'idle';
    $('cal-progress').hidden = mode !== 'calibrating';
    $('cal-result').hidden = mode !== 'calibrated';
    $('cal-compact').hidden = mode !== 'running';
    $('btn-csv').disabled = mode !== 'running';
    $('btn-finish').disabled = mode !== 'running';
    $('cal-card').querySelector('h3').textContent = mode === 'running' ? 'Calibración (completada)' : 'Calibración';
  }
  $('btn-cal-start').addEventListener('click', () => {
    if (conn.status !== 'connected') { toast('Primero conecta el Arduino o activa la simulación.'); return; }
    if (qCount.sat >= 2 || qCount.flat >= 2) { if (!confirm('Hay una alarma de señal activa. ¿Calibrar de todas formas?')) return; }
    rebuildDsp();  // reinicia filtros para empezar limpio
    cal = new E.Calibrator(S, S.fs); mode = 'calibrating'; applyMode();
  });
  $('btn-cal-cancel').addEventListener('click', () => { mode = 'idle'; cal = null; applyMode(); });
  $('btn-cal-redo').addEventListener('click', () => { mode = 'idle'; calResult = null; applyMode(); });
  $('btn-cal-reuse').addEventListener('click', () => {
    const prev = E.Store.sessionsFor(patient.id).filter(s => s.calibracion).pop();
    if (!prev) return;
    calResult = prev.calibracion; calSource = 'reutilizada de ' + fmtDate(prev.fecha);
    mode = 'calibrated'; showCalResult(); applyMode();
    toast('Calibración anterior cargada. Recuerda que depende de la colocación de los electrodos.');
  });
  function onCalibrationDone() {
    calResult = cal.result; calSource = ''; mode = 'calibrated';
    showCalResult(); applyMode();
  }
  function showCalResult() {
    const c = calResult, f = (v, d) => (isFinite(v) ? v.toFixed(d) : '—');
    $('cal-result-date').textContent = calSource;
    const items = [['Reposo (envolvente)', f(c.restMean, 3) + ' mV'], ['Ruido en reposo (SD)', f(c.restSD, 3) + ' mV'],
      ['Activa: RMS de referencia', f(c.activeRms, 3) + ' mV'], ['Relación señal/reposo', f(c.snr, 1) + '×'],
      ['Artefacto pasivo / activa', f(c.artifactPct, 0) + ' %']];
    $('cal-values').innerHTML = items.map(i => '<div><span>' + i[0] + '</span><b>' + i[1] + '</b></div>').join('');
    $('cal-warnings').innerHTML = (c.warnings || []).map(w => '<li></li>').join('');
    $('cal-warnings').querySelectorAll('li').forEach((li, i) => { li.textContent = c.warnings[i]; });
    $('btn-run-start').disabled = !(c.activeRms > 0);
  }
  $('btn-run-start').addEventListener('click', () => {
    if (conn.status !== 'connected') { toast('Conecta el Arduino antes de comenzar.'); return; }
    thr = E.deriveThresholds(calResult, S);
    det = new E.ActivationDetector({ fs: S.fs, onThr: thr.onThr, offThr: thr.offThr, minDur: S.minDur, maxGap: S.maxGap, trimStart: S.trimStart });
    acts = []; discarded = 0; runSamples = 0; envT = []; envV = []; sessionStart = new Date().toISOString();
    chart.thr = thr.onThr; mode = 'running'; applyMode();
    $('cal-compact').textContent = 'Umbral de activación: ' + thr.onThr.toFixed(3) + ' mV · Referencia activa (RMS): ' + calResult.activeRms.toFixed(3) + ' mV · Duración mínima válida: ' + S.minDur + ' s';
    localStorage.setItem('emg.fisio', $('m-fisio').value);
  });
  function onSegment(seg) {
    if (!seg.valid) { discarded++; return; }
    seg.rmsNorm = 100 * seg.rms / calResult.activeRms;
    acts.push(seg);
  }

  /* ---------- UI de sesión (4 Hz; la gráfica va a 60 fps aparte) ---------- */
  const f1 = v => (isFinite(v) ? v.toFixed(1) : '—'), f2 = v => (isFinite(v) ? v.toFixed(2) : '—');
  function renderSession() {
    const t = runSamples / S.fs;
    $('s-time').textContent = $('k-time').textContent = clock(t);
    if (mode === 'calibrating' && cal) {
      const i = cal.info();
      for (const p of ['rest', 'passive', 'active']) {
        const li = $('st-' + p), ph = i.phases[p];
        li.className = ph.state; li.querySelector('.st-time').textContent = ph.state === 'done' ? '✓' : Math.ceil(ph.left) + ' s' + (p === 'active' ? ' · rep ' + Math.max(1, i.rep) + '/' + i.reps : '');
      }
      $('cue').className = 'cue ' + i.cue; $('cue-title').textContent = i.cueTitle + (i.phase === 'active' ? ' (' + i.rep + '/' + i.reps + ')' : '');
      $('cue-text').textContent = i.cueText; $('cue-count').textContent = Math.ceil(i.stepLeft) + ' s';
      $('cal-bar').style.width = (i.progress * 100) + '%';
    }
    if (mode === 'running') {
      const live = det.live(t), liveValid = live && live.dur >= S.minDur;
      const totalActive = acts.reduce((a, s) => a + s.dur, 0) + (liveValid ? live.dur : 0);
      const maxDur = Math.max(0, ...acts.map(s => s.dur), liveValid ? live.dur : 0);
      $('k-count').textContent = acts.length + (liveValid ? 1 : 0);
      $('k-disc').textContent = discarded ? discarded + ' transición(es) < ' + S.minDur + ' s descartada(s)' : '';
      $('k-active').textContent = f1(totalActive) + ' s';
      $('k-pct').textContent = t > 1 ? Math.round(100 * totalActive / t) + ' % del tiempo de sesión' : '';
      $('k-max').textContent = f1(maxDur) + ' s';
      $('k-cur').textContent = live ? (liveValid ? 'En curso: ' : 'Posible activación: ') + f1(live.dur) + ' s' : (acts.length ? 'Última: ' + f1(acts[acts.length - 1].dur) + ' s' : '');
      $('k-rms').textContent = acts.length ? Math.round(acts.reduce((a, s) => a + s.rmsNorm, 0) / acts.length) + ' %' : '—';
      $('k-cal').textContent = 'desde «Comenzar monitoreo»';
      const g = proc.env / calResult.activeEnv * 100;
      $('gauge-fill').style.width = Math.min(150, g) / 1.5 + '%'; $('gauge-val').textContent = Math.round(g) + ' %';
      $('gauge-mark').style.left = Math.min(150, 100 * thr.onThr / calResult.activeEnv) / 1.5 + '%';
      renderTable(live, liveValid);
    } else {
      $('gauge-fill').style.width = '0'; $('gauge-val').textContent = '—';
    }
  }
  function renderTable(live, liveValid) {
    const tb = $('act-body'); tb.innerHTML = '';
    const rows = acts.map((s, i) => [i + 1, clock(s.tStart), f1(s.dur), Math.round(s.rmsNorm), f2(s.rms), f2(s.mav), f1(s.mdf), f1(s.mnf)]);
    if (liveValid) rows.push([acts.length + 1 + ' (en curso)', clock(live.tStart || 0), f1(live.dur), Math.round(100 * live.rms / calResult.activeRms), f2(live.rms), f2(live.mav), '…', '…']);
    if (!rows.length) { tb.innerHTML = '<tr><td colspan="8" class="muted">Sin activaciones todavía.</td></tr>'; return; }
    rows.slice().reverse().forEach((r, k) => {
      const tr = document.createElement('tr'); if (liveValid && k === 0) tr.className = 'live-row';
      r.forEach((v, c) => { const td = document.createElement('td'); td.textContent = v; if (c > 3) td.className = 'tech'; tr.appendChild(td); });
      tb.appendChild(tr);
    });
  }
  $('tech-toggle').addEventListener('change', e => { $('act-table').classList.toggle('show-tech', e.target.checked); S.techView = e.target.checked; E.Store.saveSettings(S); });
  $('win-sel').addEventListener('change', e => { S.winS = +e.target.value; E.Store.saveSettings(S); chart.setWindow(S.winS); });

  /* ---------- guardar / finalizar ---------- */
  function buildRecord() {
    const seg = det && det.active ? det.flush() : null; if (seg) onSegment(seg);
    const t = runSamples / S.fs, tot = acts.reduce((a, s) => a + s.dur, 0), mean = k => acts.length ? acts.reduce((a, s) => a + s[k], 0) / acts.length : null;
    return {
      patientId: patient.id, fecha: sessionStart || new Date().toISOString(), duracionS: t,
      electrodo: $('m-electrodo').value.trim(), posicion: $('m-posicion').value.trim(), fisio: $('m-fisio').value.trim(), notas: $('m-notas').value.trim(),
      calibracion: calResult, umbralMV: thr ? thr.onThr : null,
      resumen: { activaciones: acts.length, tiempoActivoS: tot, pctActivo: t > 0 ? 100 * tot / t : 0, maxContrS: Math.max(0, ...acts.map(s => s.dur)),
        medContrS: acts.length ? tot / acts.length : 0, rmsNormMedio: mean('rmsNorm'), mdfMedio: mean('mdf'), mnfMedio: mean('mnf'), descartadas: discarded },
      activaciones: acts.map((s, i) => ({ n: i + 1, inicioS: s.tStart, finS: s.tEnd, durS: s.dur, rms: s.rms, mav: s.mav, mdf: s.mdf, mnf: s.mnf, rmsNorm: s.rmsNorm }))
    };
  }
  function activationsCSV(rec, p) {
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha_sesion', 'activacion', 'inicio_s', 'fin_s', 'duracion_s', 'rms_mV', 'mav_mV', 'mdf_Hz', 'mnf_Hz', 'rms_norm_pct', 'rms_referencia_mV'];
    const rows = rec.activaciones.map(a => [fullName(p), p.expediente || '', p.gmfcs, rec.fecha, a.n, a.inicioS, a.finS, a.durS, a.rms, a.mav, a.mdf, a.mnf, a.rmsNorm, rec.calibracion ? rec.calibracion.activeRms : '']);
    E.download(E.slug(fullName(p)) + '_' + rec.fecha.slice(0, 16).replace(/[:T]/g, '-') + '_activaciones.csv', E.toCSV(H, rows));
  }
  $('btn-csv').addEventListener('click', () => {
    if (mode !== 'running') return;
    const rec = buildRecord(), stamp = rec.fecha.slice(0, 16).replace(/[:T]/g, '-');
    // buildRecord() cierra una activación en curso; el detector ya quedó reiniciado, es lo esperado.
    activationsCSV(rec, patient);
    E.download(E.slug(fullName(patient)) + '_' + stamp + '_envolvente.csv', E.toCSV(['tiempo_s', 'envolvente_mV'], envT.map((t, i) => [t, envV[i]])));
    toast('CSV descargados (activaciones + envolvente)');
  });
  $('btn-finish').addEventListener('click', async () => {
    if (!confirm('¿Finalizar la terapia y guardar la sesión en el historial?')) return;
    const rec = buildRecord();
    if (!E.Store.saveSession(rec)) { toast('No se pudo guardar en el navegador. Descarga el CSV.'); return; }
    if (S.sheetsUrl) {
      try {
        await E.sendToSheets(S.sheetsUrl, { tipo: 'sesion', paciente: S.sheetsName ? fullName(patient) : (patient.expediente || patient.id), gmfcs: patient.gmfcs,
          fecha: rec.fecha, duracionS: rec.duracionS, electrodo: rec.electrodo, posicion: rec.posicion, fisio: rec.fisio, notas: rec.notas, resumen: rec.resumen, activaciones: rec.activaciones });
      } catch (e) { toast('Sesión guardada, pero no se pudo enviar a Google Sheets.'); }
    }
    resetSession(); toast('Sesión guardada en el historial'); show('history');
  });

  /* ---------- historial ---------- */
  const METRICS = { pctActivo: ['% del tiempo', '%'], maxContrS: ['Contracción más larga', 's'], medContrS: ['Duración media', 's'], activaciones: ['Activaciones', ''],
    tiempoActivoS: ['Tiempo activo', 's'], rmsNormMedio: ['RMS normalizado', '%'], mdfMedio: ['MDF', 'Hz'] };
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
    const H = ['paciente', 'expediente', 'gmfcs', 'fecha', 'duracion_s', 'activaciones', 'tiempo_activo_s', 'pct_activo', 'max_contraccion_s', 'media_contraccion_s', 'rms_norm_medio_pct', 'mdf_medio_Hz', 'mnf_medio_Hz', 'descartadas', 'electrodo', 'posicion', 'fisio', 'notas'];
    E.download(E.slug(fullName(patient)) + '_historial.csv', E.toCSV(H, ses.map(s => [fullName(patient), patient.expediente || '', patient.gmfcs, s.fecha, s.duracionS, s.resumen.activaciones, s.resumen.tiempoActivoS, s.resumen.pctActivo, s.resumen.maxContrS, s.resumen.medContrS, s.resumen.rmsNormMedio, s.resumen.mdfMedio, s.resumen.mnfMedio, s.resumen.descartadas, s.electrodo, s.posicion, s.fisio, s.notas])));
  });

  /* ---------- ajustes ---------- */
  const FIELDS = [
    ['fs', 'Frecuencia de muestreo (Hz) — igual al firmware'], ['adcMax', 'Máximo del ADC (14 bits = 16383)'], ['vref', 'Referencia ADC (mV)'], ['gain', 'Ganancia del amplificador'],
    ['notchHz', 'Notch (Hz)'], ['hpHz', 'Pasa altas (Hz)'], ['lpHz', 'Envolvente: pasa bajas (Hz)'],
    ['minDur', 'Duración mínima de activación válida (s)'], ['maxGap', 'Fusionar huecos menores a (s)'], ['thrSD', 'Umbral: desviaciones estándar sobre reposo'],
    ['thrPct', 'Umbral mínimo (% de envolvente activa)'], ['trimStart', 'Ignorar primeros (s) de cada activación'],
    ['restS', 'Calibración: reposo (s)'], ['passiveS', 'Calibración: pasiva (s)'], ['reps', 'Calibración activa: repeticiones'], ['holdS', 'Activa: segundos arriba'], ['relaxS', 'Activa: segundos abajo'],
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
      else { const v = parseFloat(el.value); if (!isFinite(v) || v <= 0) { if (k !== 'trimStart' || v < 0) { toast('Valor no válido en: ' + k); return; } } next[k] = v; }
    }
    S = next; E.Store.saveSettings(S); rebuildDsp(); chart.setWindow(S.winS);
    $('dlg-settings').close(); toast('Ajustes guardados (se aplican desde la próxima calibración)');
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
  progChart = new E.ProgressChart($('prog'), $('prog-tip'));
  rebuildDsp();
  $('tech-toggle').checked = S.techView; $('act-table').classList.toggle('show-tech', S.techView);
  $('win-sel').value = String(S.winS);
  (function frame() { if (view === 'session') chart.draw(); requestAnimationFrame(frame); })();
  let tick = 0;
  setInterval(() => {
    const now = performance.now();
    if (now - rx.lastT >= 1000) { rx.fs = (rx.samples - rx.lastCount) * 1000 / (now - rx.lastT); rx.lastCount = rx.samples; rx.lastT = now; }
    renderConn();
    if (view === 'session') renderSession();
    if (++tick % 2 === 0) evalAlarms(now);
  }, 250);
  window.addEventListener('resize', () => { if (view === 'history') progChart.draw(); });
  window.addEventListener('beforeunload', e => { if (mode === 'running') { e.preventDefault(); e.returnValue = ''; } });
  show('home');
})();
