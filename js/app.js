/* ASTRA — interfaz para el fisioterapeuta (versión sin laptop: la sirve el ESP32 en http://192.168.4.1).
 * Quién hace qué:
 *   Sensor (Arduino)  -> señal EMG (solo se dibuja) y el RMS que va al atril
 *   Atril (ESP32)     -> calibración, detección de la postura, estrellas, luz y música
 *   Esta página       -> muestra la terapia, guarda el expediente y manda la música/volumen
 * En pantalla nunca se nombran las placas ni los umbrales: todo en palabras del consultorio.
 */
(function () {
  'use strict';
  const E = window.EMG;
  const $ = id => document.getElementById(id);
  const pad = n => String(n).padStart(2, '0');
  const clock = s => { s = Math.max(0, Math.floor(s)); return pad(Math.floor(s / 60)) + ':' + pad(s % 60); };
  const fmtDate = iso => new Date(iso).toLocaleDateString('es-MX', { day: '2-digit', month: 'short', year: 'numeric' });
  const fmtCorta = iso => new Date(iso).toLocaleDateString('es-MX', { day: '2-digit', month: '2-digit', year: '2-digit' }) + ' ' + pad(new Date(iso).getHours()) + ':' + pad(new Date(iso).getMinutes());
  const fmtDateTime = iso => fmtDate(iso) + ' ' + pad(new Date(iso).getHours()) + ':' + pad(new Date(iso).getMinutes());
  const f1 = v => (isFinite(v) && v !== null ? (+v).toFixed(1) : '—');
  const pct = v => (isFinite(v) && v !== null ? Math.round(v) + ' %' : '—');
  const edadTexto = p => {
    if (p.edadAnios === undefined || p.edadAnios === '') return p.edad || '';
    const a = +p.edadAnios;
    return a + (a === 1 ? ' año' : ' años');
  };
  const idDe = p => p.codigo || ('AST-' + String(p.id || '').slice(-4));
  let toastTimer;
  function toast(msg) {
    const t = $('toast'); t.textContent = msg; t.hidden = false;
    clearTimeout(toastTimer); toastTimer = setTimeout(() => { t.hidden = true; }, 4000);
  }

  /* ---------- estado ---------- */
  let S = E.Store.settings();
  let proc, quality, chart, progChart;
  let view = 'home', patient = null;
  let emg = null, emgConn = { status: 'disconnected', msg: '' };
  let atril = null, atrilConn = { status: 'disconnected', msg: '' };
  let link = null, srv = null, sim = null, st = null;
  let rec = null;        // sesión en curso
  let fin = null;        // sesión terminada, esperando que la guarden
  const rx = { lastData: 0, samples: 0, lastCount: 0, lastT: performance.now(), fs: 0, apartadas: 0 };
  let senalVivo = true;               // Ajustes > Diagnóstico: apagar la señal en vivo para comparar el ruido
  let prevCrit = new Set();

  function rebuildDsp() {
    proc = new E.Processor(S);
    quality = new E.SignalQuality(S.adcMax);
  }

  /* ---------- navegación ---------- */
  function show(name) {
    view = name;
    document.querySelectorAll('.view').forEach(v => { v.hidden = v.id !== 'view-' + name; });
    window.scrollTo(0, 0);
    if (name === 'home') renderHome();
    if (name === 'patients') renderPatients();
    if (name === 'patient') renderPatient();
    if (name === 'live') chart.setWindow(S.winS);
  }
  function salirDeSesion(destino) {
    if (rec && !confirm('La terapia sigue en curso. ¿Salir sin guardarla? Se perderán los datos de esta sesión.')) return;
    rec = null; show(destino);
  }
  document.querySelectorAll('[data-nav]').forEach(b => b.addEventListener('click', () => salirDeSesion(b.dataset.nav)));
  $('btn-home').addEventListener('click', () => salirDeSesion('home'));
  $('go-register').addEventListener('click', () => {
    $('form-register').reset(); $('reg-id').textContent = E.Store.nextCodigo(); show('register');
  });
  $('go-patients').addEventListener('click', () => show('patients'));

  function renderHome() {
    const p = E.Store.patients().length, s = E.Store.sessions().length;
    $('home-stats').textContent = p
      ? p + (p === 1 ? ' paciente' : ' pacientes') + ' · ' + s + (s === 1 ? ' sesión guardada' : ' sesiones guardadas')
      : 'Todavía no hay pacientes registrados.';
  }

  /* ---------- registro ---------- */
  $('form-register').addEventListener('submit', async e => {
    e.preventDefault();
    const boton = e.target.querySelector('[type=submit]');
    if (boton.disabled) return;                       // evita registrar dos veces con doble toque
    const fd = new FormData(e.target), p = {};
    fd.forEach((v, k) => { p[k] = typeof v === 'string' ? v.trim() : v; });
    p.edadAnios = String(parseInt(p.edadAnios, 10));
    p.fechaRegistro = new Date().toISOString();
    boton.disabled = true;
    try {
      patient = await E.Store.savePatient(p);
      toast('Paciente guardado con el ID ' + idDe(patient));
      show('patient');
    } catch (err) { /* el aviso ya salió (Store.onError) */ }
    finally { boton.disabled = false; }
  });

  /* ---------- buscar paciente por ID ---------- */
  function renderPatients() {
    const q = $('search-input').value.replace(/\D/g, '');
    const list = E.Store.patients()
      .filter(p => !q || idDe(p).replace(/\D/g, '').includes(q))
      .sort((a, b) => idDe(a).localeCompare(idDe(b)));
    const tb = $('pac-body'); tb.innerHTML = '';
    list.forEach(p => {
      const ses = E.Store.sessionsFor(p.id), ult = ses.length ? fmtDate(ses[ses.length - 1].fecha) : '—';
      const tr = document.createElement('tr');
      [idDe(p), p.sexo || '—', edadTexto(p) || '—', p.gmfcs || '—', String(ses.length), ult].forEach(v => {
        const td = document.createElement('td'); td.textContent = v; tr.appendChild(td);
      });
      const td = document.createElement('td'), b = document.createElement('button');
      b.className = 'btn small'; b.textContent = 'Abrir';
      td.appendChild(b); tr.appendChild(td);
      tr.addEventListener('click', () => { patient = p; show('patient'); });
      tb.appendChild(tr);
    });
    $('pac-vacio').hidden = list.length > 0;
    $('pac-vacio').textContent = E.Store.patients().length ? 'No hay pacientes con ese ID.' : 'Todavía no hay pacientes registrados.';
  }
  $('search-input').addEventListener('input', renderPatients);

  /* ===================================================================
   * Conexión con el equipo
   * =================================================================== */
  function onEmgStatus(status, msg) {
    emgConn = { status, msg: msg || '' };
    if (status === 'connected') { rx.lastData = performance.now(); }
    renderSys();
  }
  function onAtrilStatus(status, msg) {
    atrilConn = { status, msg: msg || '' };
    if (status !== 'connected') st = null;
    renderSys();
  }
  // Muestras crudas del Arduino. Bit 15 = tomada mientras el WiFi del Arduino transmitía (v5.1-AP):
  // los filtros de la pantalla la reciben igual (no se inventa nada), pero no se dibuja.
  function onSamples(arr) {
    rx.lastData = performance.now(); rx.samples += arr.length;
    const flag = st && st.modo === 'SESION' ? (st.abajo ? 1 : 2) : 0;
    for (let i = 0; i < arr.length; i++) {
      const tx = arr[i] & 0x8000, raw = arr[i] & 0x3FFF, x = proc.process(raw);
      if (tx) rx.apartadas++;
      else quality.feed(raw, proc.hpOut, x);
      chart.push(tx ? NaN : x, flag);
    }
  }
  // El atril (ESP32) sirve la página: la conexión en vivo va al mismo lugar de donde vino la página
  const servidorHost = () => (/^https?:$/.test(location.protocol) ? location.host : (S.servidor || '192.168.4.1'));
  let rtt = null;                     // retraso de ida y vuelta con el atril (ms)

  async function disconnectAll() {
    const e = emg, a = atril; emg = null; atril = null; sim = null; st = null; link = null; srv = null;
    if (a) a.stop();
    if (e) { if (e.stop) e.stop(); else await e.disconnect(); }
    emgConn = { status: 'disconnected', msg: '' }; atrilConn = { status: 'disconnected', msg: '' };
    renderSys();
  }
  async function conectar() {
    await disconnectAll();
    rebuildDsp();
    link = new E.ServidorLink(servidorHost(), {
      onSamples,
      onEstado,
      onServidor,
      onRtt: v => { rtt = v; },
      onStatus: (s, msg) => {
        if (s === 'open' && link) setTimeout(() => link && link.atril.senal(senalVivo), 200);   // el atril sabe si esta pagina quiere la señal
        if (s === 'connecting' || s === 'open') {
          if (atrilConn.status !== 'connecting') onAtrilStatus('connecting');
          if (emgConn.status !== 'connecting') onEmgStatus('connecting');
        } else if (s === 'error') { srv = null; onAtrilStatus('error', msg); onEmgStatus('error', msg); }
      }
    });
    atril = link.atril; emg = link.emg;
  }
  function onServidor(info) {
    srv = info;
    const a = info.atril.conectado;
    if (a && atrilConn.status !== 'connected') onAtrilStatus('connected');
    if (!a && atrilConn.msg !== 'atril') onAtrilStatus('error', 'atril');
    const d = info.arduino.conectado;
    if (d && emgConn.status !== 'connected') onEmgStatus('connected');
    if (!d && emgConn.msg !== 'arduino') onEmgStatus('error', 'arduino');
  }
  function startSim() {
    disconnectAll();
    rebuildDsp();
    const asim = new E.AtrilSim(), blk = new E.BlockRms(S);
    sim = { atril: asim };
    atril = new E.AtrilLink(asim, onEstado, onAtrilStatus);
    emg = new E.SimSource(arr => {
      onSamples(arr);
      for (let i = 0; i < arr.length; i++) { const mv = blk.feed(arr[i]); if (mv !== null) asim.feedBlock(mv); }
    }, onEmgStatus, () => asim.simCtx(), S.fs);
    emg.start();
    toast('Modo demostración: el equipo está simulado, sirve para practicar.');
  }
  const equipoListo = () => atrilConn.status === 'connected' && !!st;
  function sendCmd(c) {
    if (!equipoListo()) { toast('El atril no responde. Revisa que esté encendido.'); return; }
    atril.cmd(c).then(ok => { if (!ok) toast('El atril no recibió la orden. Inténtalo otra vez.'); });
  }

  /* ---------- estado del sistema, en palabras sencillas ---------- */
  function sysInfo() {
    if (sim) return ['demo', 'Modo demostración'];
    if (!link) return ['bad', 'Sin conexión con el atril'];
    if (atrilConn.status === 'error' && atrilConn.msg !== 'atril') return ['bad', 'Sin conexión con el atril'];
    if (atrilConn.status === 'connecting' || (atrilConn.status === 'connected' && !st)) return ['warn', 'Conectando con el equipo…'];
    if (atrilConn.status !== 'connected') return ['bad', 'Atril apagado'];
    if (st && st.modo === 'PARO') return ['bad', 'Paro de emergencia activado'];
    if (emgConn.status !== 'connected') return ['warn', 'Sensor sin conexión'];
    if (st && !st.datos) return ['warn', 'El atril no recibe la señal'];
    if (st && st.modo === 'CALIBRANDO') return ['warn', 'Calibrando (' + Math.ceil(st.cal.restante) + ' s)'];
    if (st && st.cal && st.cal.ok) return ['ok', 'Equipo listo'];
    return ['warn', 'Falta calibrar'];
  }
  function renderSys() {
    const [cls, txt] = sysInfo();
    $('sys').className = 'sys ' + cls;
    $('sys-text').textContent = txt;
  }

  /* ---------- lo que manda el atril ---------- */
  function onEstado(s) {
    const prev = st; st = s;
    syncMusica(s);
    if (rec) recordUpdate(prev, s);
    if (!rec && view === 'live' && patient && prev && prev.modo === 'CALIBRANDO' && s.modo === 'SESION') startRecording(s);
    renderSys();
    if (view === 'live') renderLive();
    if (view === 'start') renderStart();
  }

  /* ---------- música y volumen (lo único técnico que ve el fisio) ---------- */
  // Cabeza ARRIBA: pistas 001-003 del DFPlayer; cabeza ABAJO: 004-006
  const MUS_OPC = {
    abajo: [['4,4,4', 'Canción 1'], ['5,5,5', 'Canción 2'], ['6,6,6', 'Canción 3']],
    arriba: [['1,1,1', 'Canción 1'], ['2,2,2', 'Canción 2'], ['3,3,3', 'Canción 3']]
  };
  const musValida = (l, v) => MUS_OPC[l].some(o => o[0] === v);
  // Lo elegido en esta página (o Canción 1 si no hay elección válida: p. ej. la lista vieja "las tres")
  function musElegida(l) {
    if (!musValida(l, S['mus_' + l])) { S['mus_' + l] = MUS_OPC[l][0][0]; E.Store.saveSettings(S); }
    return S['mus_' + l];
  }
  const musEnviado = { abajo: 0, arriba: 0 };
  function initMusica() {
    for (const l of ['abajo', 'arriba']) {
      const sel = $('mus-' + l);
      sel.innerHTML = MUS_OPC[l].map(() => '<option></option>').join('');
      sel.querySelectorAll('option').forEach((o, i) => { o.value = MUS_OPC[l][i][0]; o.textContent = MUS_OPC[l][i][1]; });
      sel.addEventListener('change', () => { S['mus_' + l] = sel.value; E.Store.saveSettings(S); enviarMusica(l, sel.value, true); });
    }
    const v = $('vol');
    v.addEventListener('input', () => { $('vol-val').textContent = v.value; });
    v.addEventListener('change', () => {
      S.volumen = +v.value; E.Store.saveSettings(S);
      if (!equipoListo()) { toast('Se ajustará en cuanto el atril esté encendido.'); return; }
      atril.volumen(+v.value).then(ok => toast(ok ? 'Volumen ajustado a ' + v.value + ' de 30.' : 'El atril no recibió el volumen.'));
    });
  }
  function enviarMusica(l, v, avisar) {
    if (!equipoListo()) { if (avisar) toast('Se mandará al atril en cuanto esté encendido.'); return; }
    musEnviado[l] = performance.now();
    atril.musica(l, v.split(',').map(Number)).then(ok => {
      if (avisar) toast(ok ? 'Música de la cabeza ' + l + ' guardada.' : 'El atril no recibió la música.');
    });
  }
  function syncMusica(s) {
    if (!s.musica) return;
    for (const l of ['abajo', 'arriba']) {
      const q = musElegida(l);
      if (q !== s.musica[l].join(',') && performance.now() - musEnviado[l] > 4000) enviarMusica(l, q, false);
    }
    if (S.volumen !== undefined && s.musica.vol !== undefined && s.musica.vol !== S.volumen &&
        performance.now() - (musEnviado.vol || 0) > 4000) { musEnviado.vol = performance.now(); atril.volumen(S.volumen); }
  }
  function renderAjustes() {
    const m = st && st.musica, on = !!m && equipoListo();
    for (const l of ['abajo', 'arriba']) {
      const sel = $('mus-' + l);
      if (document.activeElement !== sel) sel.value = musElegida(l);
    }
    const vol = S.volumen !== undefined ? S.volumen : (m && m.vol !== undefined ? m.vol : 15);
    if (document.activeElement !== $('vol')) $('vol').value = vol;
    $('vol-val').textContent = vol;
    const sens = st && st.cal && st.cal.sens ? st.cal.sens : patient && patient.sensibilidad || 2;
    if (document.activeElement !== $('aj-sens')) $('aj-sens').value = String(sens);
    $('aj-sens').disabled = !(equipoListo() || sim);
    $('mus-estado').textContent = on
      ? 'Guardado en el atril. Suena una canción cuando el niño sostiene la cabeza y otra cuando se le cae.'
      : 'El atril no está encendido: los cambios se mandarán en cuanto lo esté.';
    const ed = E.Store.estado;
    $('datos-estado').textContent = ed.modo === 'indexeddb'
      ? 'Se guardan solo en este dispositivo y este navegador: ' + ed.pacientes + ' paciente(s), ' + ed.sesiones + ' sesión(es). ' +
        'Otro celular no los ve. Descarga un respaldo con frecuencia.' + (ed.persistente === false ? ' El navegador puede borrarlos si se queda sin espacio.' : '')
      : 'Este navegador NO está guardando los registros (' + (ed.error || 'sin IndexedDB') + ').';
    if ($('diag-box').open) renderDiag();
  }
  // Diagnóstico de la conexión (para revisar el equipo; no hace falta durante la terapia)
  function renderDiag() {
    const a = srv && srv.arduino, d = a && a.diag, c = st && st.cal;
    const si = (v, ok, no) => [v ? ok : no, v ? 'ok' : 'mal'];
    const filas = [
      ['Página ↔ atril', si(atrilConn.status === 'connected', 'conectada', sim ? 'modo demostración' : 'sin conexión')],
      ['Retraso (ida y vuelta)', [rtt != null && link ? Math.round(rtt) + ' ms' : '—', rtt != null && rtt > 500 ? 'mal' : '']],
      ['Sensor (Arduino) por WiFi', a ? si(a.conectado, 'conectado' + (a.ip ? ' (' + a.ip + ')' : ''), 'no conectado') : ['—', '']],
      ['Muestras/s que llegan a esta página', [rx.fs ? Math.round(rx.fs) + ' (se miden 1000)' : '—', rx.fs && Math.abs(rx.fs - 1000) > 100 ? 'mal' : '']],
      ['Muestras/s que recibe el atril', [a && a.fs ? Math.round(a.fs) : '—', '']],
      ['Paquetes perdidos (sensor → atril)', [a ? String(a.perdidos) : '—', a && a.perdidos ? 'mal' : '']],
      ['Paquetes perdidos (atril → página)', [emg && emg.lost != null ? String(emg.lost) : '—', emg && emg.lost ? 'mal' : '']],
      ['Frecuencia medida en el Arduino', [d && d.fs ? d.fs + ' Hz' : '—', d && d.fs && Math.abs(d.fs - 1000) > 20 ? 'mal' : '']],
      ['Señal en vivo del sensor', [d && d.sen != null ? (d.sen ? 'se está enviando' : 'apagada') : '—', '']],
      ['Muestras apartadas por el WiFi (último s)', [d && d.tx != null ? d.tx + ' de 1000 (no cuentan para el RMS)' : '—', '']],
      ['Ruido mientras transmite / sin transmitir', [d && d.rOk != null ? f1(d.rTx) + ' / ' + f1(d.rOk) + ' mV' : '—', d && d.rTx > 2 * d.rOk && d.rOk > 0 ? 'mal' : '']],
      ['Envío WiFi más lento (último s)', [d && d.wMax != null ? d.wMax + ' ms' : '—', d && d.wMax > 60 ? 'mal' : '']],
      ['Pausas de la señal por WiFi lento', [d && d.lentos != null ? String(d.lentos) : '—', d && d.lentos ? 'mal' : '']],
      ['Mayor espera entre datos al atril (10 s)', [d && d.opto ? d.opto + ' ms (normal: 100)' : '—', d && d.opto > 250 ? 'mal' : '']],
      ['Rango de la señal en A0', [d && d.max != null ? (d.min * 5 / 16383).toFixed(2) + ' a ' + (d.max * 5 / 16383).toFixed(2) + ' V' : '—', '']],
      ['Estado del atril', [st ? st.modo + (st.datos ? '' : ' · sin RMS del opto') : '—', st && !st.datos ? 'mal' : '']],
      ['RMS actual (opto)', [st ? st.rms + ' mV (mediana ' + f1(st.med) + ')' : '—', '']],
      ['Reposo de la calibración', [c && c.ok ? f1(c.reposo) + ' ± ' + f1(c.sd) + ' mV' : '—', c && c.inestable ? 'mal' : '']],
      ['Umbral de activación / relajación', [c && c.ok ? f1(c.uon) + ' / ' + f1(c.uoff) + ' mV' : '—', '']],
      ['Sensibilidad', [c && c.sens ? ['', 'más sensible', 'normal', 'menos sensible'][c.sens] : '—', '']],
      ['Nivel con la cabeza arriba (se ajusta solo)', [st && st.arriba ? f1(st.arriba) + ' mV' : '—', '']],
      ['Umbral ahora: caída / subida', [st && st.uAhora ? f1(st.uAhora) + (st.uSuelta ? ' / ' + f1(st.uSuelta) : '') + ' mV' : '—', '']],
      ['Equipos en la red / páginas abiertas', [srv ? (srv.equipos != null ? srv.equipos : '—') + ' / ' + (srv.paginas != null ? srv.paginas : '—') : '—', '']],
      ['Memoria libre del atril', [srv && srv.memoria ? Math.round(srv.memoria / 1024) + ' KB' : '—', srv && srv.memoria && srv.memoria < 30000 ? 'mal' : '']],
      ['Versión del atril', [st && st.v ? st.v : '—', '']]
    ];
    $('diag').innerHTML = filas.map(() => '<dt></dt><dd></dd>').join('');
    const dts = $('diag').querySelectorAll('dt'), dds = $('diag').querySelectorAll('dd');
    filas.forEach((f, i) => { dts[i].textContent = f[0]; dds[i].textContent = f[1][0]; if (f[1][1]) dds[i].className = f[1][1]; });
  }
  $('diag-box').addEventListener('toggle', () => { if ($('diag-box').open) renderDiag(); });
  $('aj-sens').addEventListener('change', () => {
    const n = +$('aj-sens').value;
    if (!atril || !atril.sensibilidad) return;
    atril.sensibilidad(n).then(ok => toast(ok ? 'Sensibilidad cambiada: el atril recalculó el umbral.' : 'El atril no recibió la sensibilidad.'));
    if (patient && (patient.sensibilidad || 2) !== n) { patient.sensibilidad = n; E.Store.savePatient(patient).catch(() => {}); }
  });
  $('btn-senal').addEventListener('click', () => {
    senalVivo = !senalVivo;
    if (atril && atril.senal) atril.senal(senalVivo);
    if (senalVivo && link) link.parser.lastSeq = -1;   // la señal vuelve a empezar: ese salto no es pérdida
    $('btn-senal').textContent = senalVivo ? 'Apagar la señal en vivo' : 'Encender la señal en vivo';
    toast(senalVivo ? 'Señal en vivo encendida.' : 'Señal en vivo apagada: el sensor deja de transmitirla (el atril sigue funcionando).');
  });

  /* ---------- registro de la sesión ---------- */
  const keyOf = (s, sid) => s.boot + ':' + (sid === undefined ? s.sid : sid);
  function startRecording(s) {
    rec = {
      start: new Date().toISOString(), key: keyOf(s), t0: s.tSes, base: 0, last: s.tSes, minId: s.nAct,
      abajoS: 0, caidas: 0, maxAbajoS: 0, curAbajoS: 0, eraAbajo: false,
      acts: [], seen: new Set(), rms: [], umbral: s.cal.uon, cal: fotoCal(s), recal: 0, pausaS: 0
    };
    toast('La terapia empezó: ASTRA ya está midiendo.');
  }
  // Lo que dejó la calibración del atril (se guarda con la sesión y va al Excel)
  const fotoCal = s => ({ reposoMV: s.cal.reposo, sdMV: s.cal.sd, umbralOnMV: s.cal.uon, umbralOffMV: s.cal.uoff, bloques: s.cal.n,
    factorK: s.cal.k, inestable: !!s.cal.inestable, pocosDatos: !!s.cal.pocos, abajoEsActividad: !!s.abajoAct, version: s.v });
  const recTime = () => (rec ? rec.base + rec.last - rec.t0 : 0);
  function recordUpdate(prev, s) {
    const take = () => {
      for (const [sid, id, ini, dur, rms] of s.acts) {
        const k = keyOf(s, sid), uid = s.boot + '#' + id;
        if (k !== rec.key || id <= rec.minId || rec.seen.has(uid)) continue;
        rec.seen.add(uid);
        rec.acts.push({ n: rec.acts.length + 1, inicioS: rec.base + ini - rec.t0, durS: dur, rms });
      }
    };
    take();
    const enSesion = s.modo === 'SESION' || s.modo === 'PAUSA';
    if (enSesion && keyOf(s) !== rec.key) {   // recalibraron: la misma terapia continúa
      rec.base += rec.last - rec.t0; rec.key = keyOf(s); rec.t0 = 0; rec.last = s.tSes; rec.minId = 0;
      rec.umbral = s.cal.uon; rec.cal = fotoCal(s); rec.recal++; take();
    }
    if (enSesion && keyOf(s) === rec.key) rec.last = s.tSes;
    if (s.modo === 'SESION' && keyOf(s) === rec.key) {
      // tiempo con la cabeza caída y número de caídas, medidos con el reloj del atril
      const dt = prev && prev.modo === 'SESION' && keyOf(prev) === rec.key ? Math.max(0, Math.min(1, s.tSes - prev.tSes)) : 0;
      if (s.abajo) {
        rec.abajoS += dt; rec.curAbajoS += dt;
        if (!rec.eraAbajo) { rec.caidas++; rec.curAbajoS = dt; }
        rec.maxAbajoS = Math.max(rec.maxAbajoS, rec.curAbajoS);
      } else rec.curAbajoS = 0;
      rec.eraAbajo = !!s.abajo;
      const t = recTime(), n = s.b.length;
      s.b.forEach(([v, m, f], i) => rec.rms.push([+(t - (n - 1 - i) * 0.1).toFixed(1), v, m, f === 1 ? 1 : 0]));
    }
  }

  /* ---------- avisos (en palabras del consultorio) ---------- */
  function beep() {
    if (!S.sound) return;
    try {
      const a = new (window.AudioContext || window.webkitAudioContext)(), o = a.createOscillator(), g = a.createGain();
      o.frequency.value = 880; g.gain.value = 0.15; o.connect(g); g.connect(a.destination); o.start(); o.stop(a.currentTime + 0.35);
      setTimeout(() => a.close(), 600);
    } catch (e) { /* sin audio */ }
  }
  function evalAlarms(now) {
    const A = {}, enTerapia = view === 'live' || view === 'start';
    if (!sim) {
      if (atrilConn.status === 'error' && atrilConn.msg === 'atril') A.atril = ['crit', 'El atril no responde. Revisa que esté encendido y con batería.'];
      else if (atrilConn.status === 'error') A.red = ['crit', 'No hay conexión con el atril. Revisa que esté encendido y que este celular esté unido a la red WiFi «' + (srv && srv.red || 'ASTRA') + '»; luego abre http://192.168.4.1'];
      else if (atrilConn.status === 'disconnected' && enTerapia) A.atril = ['crit', 'Sin conexión con el atril.'];
      if (emgConn.status === 'error' && enTerapia) A.emg = ['warn', 'El sensor no está enviando la señal. Revisa que esté encendido y con batería.'];
    }
    if (st) {
      if (st.modo === 'PARO') A.paro = ['crit', 'Paro de emergencia activado en el atril. Suéltalo para continuar.'];
      else if (!st.datos && st.modo !== 'ARRANQUE') A.opto = ['crit', 'El atril no está recibiendo la señal del sensor. Revisa el cable entre el sensor y el atril.'];
      else if (st.cal && st.cal.inestable && enTerapia) A.cal = ['warn', 'La medición salió inestable. Revisa los electrodos y vuelve a calibrar.'];
    }
    if (enTerapia && emgConn.status === 'connected' && now - rx.lastData > 2000) A.nodata = ['warn', 'Hace unos segundos que no llega la señal del sensor.'];
    if (enTerapia && emgConn.status === 'connected' && now - rx.lastData < 2000) {
      const q = quality.evaluate();
      if (q) {
        if (q.saturated) A.sat = ['crit', 'La señal está en el límite: puede haber un electrodo o un cable suelto.'];
        else if (q.flat) A.flat = ['crit', 'No se detecta actividad del músculo. Revisa que los electrodos estén bien pegados.'];
        else if (q.noisy60) A.noisy = ['warn', 'Hay mucha interferencia eléctrica. Separa los cables del cargador o de la corriente.'];
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
   * Antes de comenzar
   * =================================================================== */
  $('btn-p-start').addEventListener('click', () => {
    if (!patient) return;
    $('st-id').textContent = idDe(patient);
    const fisios = [...new Set(E.Store.sessions().map(s => s.fisio).filter(Boolean))];
    $('st-fisios').innerHTML = fisios.map(() => '<option></option>').join('');
    $('st-fisios').querySelectorAll('option').forEach((o, i) => { o.value = fisios[i]; });
    $('st-fisio').value = localStorage.getItem('emg.fisio') || '';
    $('st-sens').value = String(patient.sensibilidad || 2);
    show('start'); renderStart();
  });
  $('st-atras').addEventListener('click', () => show('patient'));
  function renderStart() {
    const [cls, txt] = sysInfo(), box = $('st-estado');
    box.className = 'estado-grande ' + (cls === 'demo' ? 'ok' : cls);
    let extra = '';
    if (cls === 'ok') extra = ' El equipo está listo para empezar.';
    else if (cls === 'bad') extra = ' Enciende el sensor y el atril para poder comenzar.';
    else if (cls === 'demo') extra = ' Puedes practicar con el equipo simulado.';
    box.textContent = txt + '.' + extra;
    $('st-comenzar').disabled = !(equipoListo() || sim);
  }
  $('st-comenzar').addEventListener('click', () => {
    const fisio = $('st-fisio').value.trim();
    if (!fisio) { toast('Escribe quién dirige la sesión.'); $('st-fisio').focus(); return; }
    localStorage.setItem('emg.fisio', fisio);
    rec = null; fin = null;
    const sens = +$('st-sens').value || 2;          // se guarda con el paciente y se manda al atril antes de calibrar
    if ((patient.sensibilidad || 2) !== sens) { patient.sensibilidad = sens; E.Store.savePatient(patient).catch(() => {}); }
    if (atril && atril.sensibilidad) atril.sensibilidad(sens);
    if (st && (st.modo === 'SESION' || st.modo === 'PAUSA')) sendCmd('c');   // calibra de nuevo para esta sesión
    show('live'); renderLive();
  });

  /* ===================================================================
   * Terapia en curso
   * =================================================================== */
  $('lv-pausa').addEventListener('click', () => sendCmd('p'));
  // Zoom de la gráfica: Auto (×1) y de ×2 en ×2 (se recuerda en este celular)
  const ZOOMS = [0.25, 0.5, 1, 2, 4, 8, 16];
  function ponerZoom(z) {
    chart.zoom = z; S.zoom = z; E.Store.saveSettings(S);
    $('zoom-auto').textContent = z === 1 ? 'Auto' : '×' + (z >= 1 ? z : '1/' + (1 / z));
    $('zoom-auto').classList.toggle('on', z === 1);
  }
  $('zoom-mas').addEventListener('click', () => ponerZoom(ZOOMS[Math.min(ZOOMS.length - 1, ZOOMS.indexOf(chart.zoom) + 1)]));
  $('zoom-menos').addEventListener('click', () => ponerZoom(ZOOMS[Math.max(0, ZOOMS.indexOf(chart.zoom) - 1)]));
  $('zoom-auto').addEventListener('click', () => ponerZoom(1));   // el atril alterna pausa / seguir con 'p'
  $('lv-recal').addEventListener('click', () => {
    if (confirm('¿Volver a calibrar? Deja la cabeza del niño arriba, en la postura de la terapia, y quieta 10 segundos. El tiempo de la terapia continúa después.')) sendCmd('c');
  });
  $('lv-fin').addEventListener('click', () => {
    if (!rec) { if (confirm('La terapia todavía no empezó a medir. ¿Salir?')) { rec = null; show('patient'); } return; }
    if (!confirm('¿Finalizar la sesión?')) return;
    fin = buildRecord();
    const senal = rec.rms; rec = null;
    fin.senal = senal;
    if (st && st.modo === 'SESION') sendCmd('p');
    renderEnd(); show('end');
  });

  function renderLive() {
    const p = patient || {};
    $('lv-paciente').textContent = 'Paciente ' + idDe(p) + (p.gmfcs ? ' · GMFCS ' + p.gmfcs : '');
    const [cls, txt] = sysInfo();
    const pill = $('lv-sistema');
    pill.className = 'pill ' + (cls === 'ok' || cls === 'demo' ? 'ok' : cls === 'warn' ? 'warn' : 'bad');
    pill.textContent = cls === 'ok' ? 'Conectado' : cls === 'demo' ? 'Demostración' : txt;

    const calibrando = st && st.modo === 'CALIBRANDO';
    $('lv-cal').hidden = !(calibrando || !rec);
    if (calibrando) {
      const c = st.cal;
      $('lv-cal').className = 'calibrando';
      $('lv-cal-titulo').textContent = 'Calibrando · ' + Math.ceil(c.restante) + ' s';
      $('lv-cal-txt').textContent = 'Mantén la cabeza del niño arriba, en la postura de la terapia, y quieta.';
      $('lv-cal-bar').style.width = (100 * (1 - c.restante / c.total)) + '%';
    } else if (!rec) {
      const esperando = equipoListo() || sim;
      $('lv-cal').className = 'calibrando' + (esperando ? '' : ' bad');
      $('lv-cal-titulo').textContent = esperando ? 'Preparando la sesión…' : 'Esperando el equipo';
      $('lv-cal-txt').textContent = esperando
        ? 'En cuanto termine la calibración, ASTRA empieza a medir solo.'
        : 'Enciende el sensor y el atril.';
      $('lv-cal-bar').style.width = '0';
    }

    const abajo = !!(st && st.modo === 'SESION' && st.abajo);
    const post = $('lv-postura');
    post.className = 'postura ' + (st && st.modo === 'SESION' ? (abajo ? 'abajo' : 'arriba') : '');
    post.textContent = st && st.modo === 'SESION' ? (abajo ? 'Cabeza abajo' : 'Cabeza arriba')
      : st && st.modo === 'PAUSA' ? 'En pausa' : '—';
    $('lv-tiempo').textContent = clock(recTime());
    $('lv-abajo').textContent = clock(rec ? rec.abajoS : 0);
    $('lv-caidas').textContent = rec ? rec.caidas : 0;
    const cal = st && st.cal && st.cal.ok;
    $('lv-pie').textContent = (cal ? 'Calibración completada' : 'Falta calibrar') + ' · ' +
      (!senalVivo ? 'Señal en vivo apagada (Ajustes)' : emgConn.status === 'connected' || sim ? 'Señal disponible' : 'Sin señal del sensor');
    $('lv-pausa').textContent = st && st.modo === 'PAUSA' ? '▶ Continuar' : '❚❚ Pausar';
  }

  /* ===================================================================
   * Fin de la sesión
   * =================================================================== */
  function buildRecord() {
    const t = recTime(), abajo = Math.min(t, rec.abajoS), arriba = Math.max(0, t - abajo);
    return {
      patientId: patient.id, codigo: idDe(patient), fecha: rec.start, fisio: localStorage.getItem('emg.fisio') || '',
      duracionS: t, umbralMV: rec.umbral, calibracion: rec.cal, recalibraciones: rec.recal, observaciones: '',
      resumen: {
        tiempoArribaS: arriba, tiempoAbajoS: abajo, caidas: rec.caidas, caidaMasLargaS: rec.maxAbajoS,
        pctArriba: t > 0 ? 100 * arriba / t : null, caidasMin: t > 0 ? rec.caidas / (t / 60) : null,
        duracionMin: t / 60
      },
      episodios: rec.acts.map(a => ({ n: a.n, inicioS: a.inicioS, durS: a.durS, rms: a.rms }))
    };
  }
  function kpisSesion(r) {
    const u = r.resumen || {};
    return [
      ['Duración de la terapia', clock(r.duracionS)],
      ['Tiempo con la cabeza erguida', clock(u.tiempoArribaS) + (u.pctArriba != null ? ' · ' + pct(u.pctArriba) : '')],
      ['Tiempo con la cabeza caída', clock(u.tiempoAbajoS)],
      ['Caídas de cabeza', String(u.caidas != null ? u.caidas : '—')],
      ['Caída más larga', f1(u.caidaMasLargaS) + ' s'],
      ['Umbral de activación', f1(r.umbralMV) + ' mV']
    ];
  }
  function pintarKpis(el, items) {
    el.innerHTML = items.map(() => '<div class="kpi"><span></span><b></b></div>').join('');
    el.querySelectorAll('.kpi').forEach((d, i) => { d.firstChild.textContent = items[i][0]; d.lastChild.textContent = items[i][1]; });
  }
  function renderEnd() {
    $('en-id').textContent = idDe(patient);
    $('en-sub').textContent = fmtDateTime(fin.fecha) + ' · ' + (fin.fisio || 'sin responsable anotado');
    pintarKpis($('en-kpis'), kpisSesion(fin));
    $('en-obs').value = '';
  }
  $('en-guardar').addEventListener('click', async () => {
    if (!fin) return;
    fin.observaciones = $('en-obs').value.trim();
    const senal = fin.senal; delete fin.senal;
    try { await E.Store.saveSession(fin, senal); }
    catch (e) { fin.senal = senal; toast('No se pudo guardar: ' + e.message + '. Descarga el Excel antes de salir.'); return; }
    toast('Sesión guardada en el expediente.');
    fin = null; show('patient');
  });
  $('en-descartar').addEventListener('click', () => {
    if (!confirm('¿Descartar esta sesión? No quedará registrada.')) return;
    fin = null; show('patient');
  });
  $('en-excel').addEventListener('click', () => {
    if (!fin) return;
    fin.observaciones = $('en-obs').value.trim();
    excelSesion(fin, patient, fin.senal || []);
  });

  /* ===================================================================
   * Expediente del paciente
   * =================================================================== */
  function renderPatient() {
    const p = patient; if (!p) return show('patients');
    $('p-id').textContent = idDe(p);
    const datos = [p.sexo, edadTexto(p), p.gmfcs ? 'GMFCS ' + p.gmfcs : '', p.diagnostico]
      .filter(Boolean).join(' · ');
    $('p-datos').textContent = datos + (p.fechaRegistro ? ' · registrado el ' + fmtDate(p.fechaRegistro) : '');
    const ses = E.Store.sessionsFor(p.id);
    renderResumen(ses);
    renderProgreso(ses);
    renderSesiones(ses, p);
    $('h-notas').value = p.notas || ''; $('h-notas-ok').textContent = '';
    $('ses-card').hidden = true;
  }
  const res = s => s.resumen || {};
  function renderResumen(ses) {
    if (!ses.length) {
      pintarKpis($('p-resumen'), [['Sesiones', '0'], ['Siguiente paso', 'Iniciar la primera terapia']]);
      $('p-tendencia').textContent = '';
      return;
    }
    const tot = ses.reduce((a, s) => a + (s.duracionS || 0), 0);
    const prom = k => { const v = ses.map(s => res(s)[k]).filter(x => x != null && isFinite(x)); return v.length ? v.reduce((a, b) => a + b, 0) / v.length : null; };
    const mejor = ses.reduce((a, s) => (res(s).pctArriba > res(a).pctArriba ? s : a), ses[0]);
    pintarKpis($('p-resumen'), [
      ['Sesiones realizadas', String(ses.length)],
      ['Primera sesión', fmtDate(ses[0].fecha)],
      ['Última sesión', fmtDate(ses[ses.length - 1].fecha)],
      ['Tiempo total de terapia', clock(tot)],
      ['Cabeza erguida (promedio)', pct(prom('pctArriba'))],
      ['Caídas por minuto (promedio)', f1(prom('caidasMin'))],
      ['Caída más larga (promedio)', f1(prom('caidaMasLargaS')) + ' s'],
      ['Mejor sesión', pct(res(mejor).pctArriba) + ' · ' + fmtDate(mejor.fecha)]
    ]);
    let msg = '';
    const pts = ses.map(s => res(s).pctArriba).filter(v => v != null && isFinite(v));
    if (pts.length >= 2) {
      const k = pts.length >= 6 ? 3 : 1, avg = a => a.reduce((x, y) => x + y, 0) / a.length;
      const a = avg(pts.slice(0, k)), b = avg(pts.slice(-k)), d = b - a;
      msg = (k > 1 ? 'Últimas 3 sesiones frente a las primeras 3: ' : 'Última sesión frente a la primera: ') +
        Math.round(a) + ' % → ' + Math.round(b) + ' % del tiempo con la cabeza erguida (' +
        (d >= 0 ? '+' : '') + Math.round(d) + ' puntos' + (Math.abs(d) < 2 ? ', sin cambio claro' : d > 0 ? ', va mejorando' : ', ha bajado') + ').';
    }
    $('p-tendencia').textContent = msg;
  }
  const METRICS = { pctArriba: '%', caidasMin: 'caídas/min', caidaMasLargaS: 's', duracionMin: 'min' };
  function renderProgreso(ses) {
    const key = $('h-metric').value;
    const pts = ses.filter(s => res(s)[key] != null && isFinite(res(s)[key])).map(s => ({ y: res(s)[key], label: fmtDate(s.fecha) }));
    progChart.set(pts, METRICS[key]);
  }
  $('h-metric').addEventListener('change', () => renderProgreso(E.Store.sessionsFor(patient.id)));
  function renderSesiones(ses, p) {
    const tb = $('h-body'); tb.innerHTML = '';
    ses.slice().reverse().forEach(s => {
      const u = res(s), tr = document.createElement('tr');
      [fmtCorta(s.fecha), s.fisio || '—', clock(s.duracionS), f1(s.umbralMV),
        clock(u.tiempoArribaS) + (u.pctArriba != null ? ' (' + pct(u.pctArriba) + ')' : ''),
        clock(u.tiempoAbajoS), String(u.caidas != null ? u.caidas : '—')]
        .forEach(v => { const td = document.createElement('td'); td.textContent = v; tr.appendChild(td); });
      const td = document.createElement('td');
      const ver = document.createElement('button'); ver.className = 'btn small ghost'; ver.textContent = 'Ver';
      ver.addEventListener('click', () => verSesion(s));
      const xls = document.createElement('button'); xls.className = 'btn small ghost'; xls.textContent = '⬇';
      xls.title = 'Descargar esta sesión en Excel';
      xls.addEventListener('click', () => descargarSesion(s, p));
      const del = document.createElement('button'); del.className = 'btn small ghost'; del.textContent = '🗑';
      del.title = 'Eliminar esta sesión';
      del.addEventListener('click', () => {
        if (confirm('¿Eliminar esta sesión del expediente?')) E.Store.deleteSession(s.id).then(renderPatient, () => {});
      });
      td.append(ver, ' ', xls, ' ', del); tr.appendChild(td); tb.appendChild(tr);
    });
    $('h-vacio').hidden = ses.length > 0;
  }
  let sesVista = null;
  async function verSesion(s) {
    sesVista = s;
    $('ses-card').hidden = false;
    $('ses-titulo').textContent = 'Sesión del ' + fmtDateTime(s.fecha);
    pintarKpis($('ses-kpis'), kpisSesion(s));
    $('ses-obs').textContent = s.observaciones ? 'Observaciones: ' + s.observaciones : '';
    $('ses-txt').textContent = 'Cargando la señal…';
    $('ses-card').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    try {
      const datos = await E.Store.senal(s.id);
      $('ses-txt').textContent = 'Señal del músculo durante la sesión. Lo sombreado es el tiempo con la cabeza caída.';
      requestAnimationFrame(() => E.dibujarSesion($('ses-senal'), datos, { uon: s.umbralMV, uoff: null }));
    } catch (e) {
      $('ses-txt').textContent = 'La señal de esta sesión no está disponible.';
      const c = $('ses-senal'), x = c.getContext('2d'); x.clearRect(0, 0, c.width, c.height);
    }
  }
  $('ses-cerrar').addEventListener('click', () => { $('ses-card').hidden = true; });
  $('ses-excel').addEventListener('click', () => { if (sesVista) descargarSesion(sesVista, patient); });
  $('btn-h-notas').addEventListener('click', async () => {
    if (!patient) return;
    patient.notas = $('h-notas').value;
    try { await E.Store.savePatient(patient); $('h-notas-ok').textContent = 'Guardado ✓'; }
    catch (e) { $('h-notas-ok').textContent = 'No se guardó'; }
  });

  /* ---------- Excel ---------- */
  const archivo = (p, extra) => 'ASTRA_' + idDe(p).replace(/[^\w-]/g, '') + '_' + extra + '.xlsx';
  function datosPaciente(p) {
    return [['Paciente', idDe(p)], ['Sexo', p.sexo || ''], ['Edad', edadTexto(p)], ['Nivel funcional (GMFCS)', p.gmfcs || ''],
      ['Diagnóstico', p.diagnostico || ''], ['Fecha de registro', p.fechaRegistro ? fmtDate(p.fechaRegistro) : '']];
  }
  function hojaSesion(r) {
    const u = res(r);
    return [['Fecha', fmtDateTime(r.fecha)], ['Fisioterapeuta responsable', r.fisio || ''],
      ['Duración de la terapia', clock(r.duracionS)], ['Duración (minutos)', +(r.duracionS / 60).toFixed(2)],
      ['Umbral de activación (mV)', r.umbralMV != null ? +(+r.umbralMV).toFixed(2) : ''],
      ['Tiempo con la cabeza erguida', clock(u.tiempoArribaS)], ['Tiempo cabeza erguida (s)', u.tiempoArribaS != null ? +u.tiempoArribaS.toFixed(1) : ''],
      ['% del tiempo con la cabeza erguida', u.pctArriba != null ? +u.pctArriba.toFixed(1) : ''],
      ['Tiempo con la cabeza caída', clock(u.tiempoAbajoS)], ['Tiempo cabeza caída (s)', u.tiempoAbajoS != null ? +u.tiempoAbajoS.toFixed(1) : ''],
      ['Número de caídas de cabeza', u.caidas != null ? u.caidas : ''], ['Caída más larga (s)', u.caidaMasLargaS != null ? +u.caidaMasLargaS.toFixed(1) : ''],
      ['Caídas por minuto', u.caidasMin != null ? +u.caidasMin.toFixed(2) : ''],
      ['Veces que se volvió a calibrar', r.recalibraciones || 0],
      ['Calibración: reposo medio (mV)', r.calibracion && r.calibracion.reposoMV != null ? +(+r.calibracion.reposoMV).toFixed(1) : ''],
      ['Calibración: desviación (mV)', r.calibracion && r.calibracion.sdMV != null ? +(+r.calibracion.sdMV).toFixed(2) : ''],
      ['Umbral de relajación (mV)', r.calibracion && r.calibracion.umbralOffMV != null ? +(+r.calibracion.umbralOffMV).toFixed(1) : ''],
      ['Calibración inestable', r.calibracion ? (r.calibracion.inestable ? 'Sí' : 'No') : ''],
      ['Observaciones', r.observaciones || '']];
  }
  function excelSesion(r, p, senal) {
    const hojas = [
      { nombre: 'Sesión', anchos: [34, 26], negritas: true,
        filas: [['Sesión de terapia ASTRA', '']].concat(datosPaciente(p), [['', '']], hojaSesion(r)), titulos: 1 },
      { nombre: 'Episodios', anchos: [10, 16, 16, 16],
        filas: [['N.º', 'Inicio (s)', 'Duración (s)', 'Intensidad media (mV)']].concat(
          (r.episodios || []).map(a => [a.n, +a.inicioS.toFixed(1), +a.durS.toFixed(1), Math.round(a.rms)])), titulos: 1 }
    ];
    if (senal && senal.length) hojas.push({ nombre: 'Señal', anchos: [12, 14, 14, 12],
      filas: [['Tiempo (s)', 'Señal (mV)', 'Promedio (mV)', 'Cabeza caída']].concat(senal.map(f => [f[0], f[1], f[2], f[3] ? 1 : 0])), titulos: 1 });
    E.xlsx(archivo(p, r.fecha.slice(0, 16).replace(/[:T]/g, '-')), hojas);
  }
  async function descargarSesion(s, p) {
    let senal = [];
    try { senal = await E.Store.senal(s.id); } catch (e) { /* la señal solo existe si se guardó en el servidor */ }
    excelSesion(s, p, senal.map ? senal.map(d => (Array.isArray(d) ? d : [d.t, d.rms, d.med, d.act])) : []);
    toast('Excel de la sesión descargado.');
  }
  $('btn-p-excel').addEventListener('click', () => {
    const p = patient, ses = E.Store.sessionsFor(p.id);
    if (!ses.length) { toast('Este paciente todavía no tiene sesiones.'); return; }
    const H = ['Fecha', 'Fisioterapeuta', 'Duración (min)', 'Umbral de activación (mV)', 'Tiempo cabeza erguida (s)',
      '% cabeza erguida', 'Tiempo cabeza caída (s)', 'Caídas de cabeza', 'Caída más larga (s)', 'Caídas por minuto', 'Observaciones'];
    const filas = ses.map(s => {
      const u = res(s);
      return [fmtDateTime(s.fecha), s.fisio || '', +(s.duracionS / 60).toFixed(2), s.umbralMV != null ? +(+s.umbralMV).toFixed(2) : '',
        u.tiempoArribaS != null ? +u.tiempoArribaS.toFixed(1) : '', u.pctArriba != null ? +u.pctArriba.toFixed(1) : '',
        u.tiempoAbajoS != null ? +u.tiempoAbajoS.toFixed(1) : '', u.caidas != null ? u.caidas : '',
        u.caidaMasLargaS != null ? +u.caidaMasLargaS.toFixed(1) : '', u.caidasMin != null ? +u.caidasMin.toFixed(2) : '', s.observaciones || ''];
    });
    E.xlsx(archivo(p, 'expediente'), [
      { nombre: 'Paciente', anchos: [30, 30], negritas: true, filas: datosPaciente(p).concat([['', ''], ['Notas', p.notas || '']]) },
      { nombre: 'Sesiones', anchos: [20, 22, 14, 18, 20, 16, 20, 16, 18, 16, 40], filas: [H].concat(filas), titulos: 1 }
    ]);
    toast('Excel del expediente descargado.');
  });

  /* ---------- ajustes ---------- */
  $('btn-settings').addEventListener('click', () => { renderAjustes(); $('dlg-settings').showModal(); });
  $('btn-sim').addEventListener('click', () => { if (sim) { disconnectAll(); toast('Modo demostración apagado.'); } else startSim(); $('dlg-settings').close(); });
  $('btn-backup').addEventListener('click', async () => {
    try {
      E.download('ASTRA-respaldo-' + new Date().toISOString().slice(0, 10) + '.json', await E.Store.backup(), 'application/json');
      toast('Respaldo descargado. Guárdalo en un lugar seguro (contiene datos clínicos).');
    } catch (e) { toast('No se pudo crear el respaldo: ' + e.message); }
  });
  $('btn-restore').addEventListener('click', () => $('file-restore').click());
  $('file-restore').addEventListener('change', async e => {
    try {
      const r = await E.Store.restore(await e.target.files[0].text());
      toast('Importados: ' + r.patients + ' pacientes y ' + r.sessions + ' sesiones' +
        (r.repetidos ? ' (' + r.repetidos + ' ya estaban)' : '') + (r.renombrados ? '. ' + r.renombrados + ' recibieron un ID nuevo porque el suyo ya existía' : '') + '.');
      renderHome(); renderAjustes();
    } catch (err) { toast('No se pudo importar: ' + err.message); }
    e.target.value = '';
  });

  /* ---------- arranque ---------- */
  chart = new E.LiveChart($('live'), S.fs);
  chart.zoom = [0.25, 0.5, 1, 2, 4, 8, 16].includes(S.zoom) ? S.zoom : 1;
  ponerZoom(chart.zoom);
  progChart = new E.ProgressChart($('prog'), $('prog-tip'));
  initMusica();
  rebuildDsp();
  (function frame() { if (view === 'live') chart.draw(); requestAnimationFrame(frame); })();
  let tick = 0;
  setInterval(() => {
    const now = performance.now();
    if (now - rx.lastT >= 1000) {
      const inst = (rx.samples - rx.lastCount) * 1000 / (now - rx.lastT);
      rx.fs = rx.fs ? 0.75 * rx.fs + 0.25 * inst : inst; rx.lastCount = rx.samples; rx.lastT = now;
    }
    renderSys();
    if (view === 'live') renderLive();
    if (view === 'start') renderStart();
    if ($('dlg-settings').open) renderAjustes();
    if (++tick % 2 === 0) evalAlarms(now);
  }, 250);
  window.addEventListener('resize', () => { if (view === 'patient') progChart.draw(); });
  window.addEventListener('beforeunload', e => { if (rec || fin) { e.preventDefault(); e.returnValue = ''; } });
  E.Store.onError = toast;
  show('home');
  // Registros de este navegador (IndexedDB) y, si la página vino del atril, conexión en vivo
  E.Store.conectar().then(() => {
    renderHome(); if (view === 'patients') renderPatients();
    if (/^https?:$/.test(location.protocol)) conectar();
  });
})();
