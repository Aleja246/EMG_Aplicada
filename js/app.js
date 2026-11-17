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
      acts: [], seen: new Set(), rms: [], umbral: s.cal.uon, cal: fotoCal(s), recal: 0, pausaS: 0, premioS: 0
    };
    toast('La terapia empezó: ASTRA ya está midiendo.');
  }
  // Lo que dejó la calibración del atril (se guarda con la sesión y va al Excel)
  const fotoCal = s => ({ reposoMV: s.cal.reposo, sdMV: s.cal.sd, umbralOnMV: s.cal.uon, umbralOffMV: s.cal.uoff, bloques: s.cal.n,
    factorK: s.cal.k, inestable: !!s.cal.inestable, pocosDatos: !!s.cal.pocos, abajoEsActividad: !!s.abajoAct, version: s.v,
    sensibilidad: s.cal.sens, margenMinMV: s.cal.margen, margenRel: s.cal.rel, factorOff: s.cal.koff });
  // El atril da el premio (estrellas, luz y música) cuando NO marca "abajo" (o al revés si abajoAct)
  const premioActivo = s => !!(s && (s.abajoAct ? s.abajo : !s.abajo));
  // Umbral con el que el atril compara la mediana en ese bloque: el de activación en reposo, el de soltar durante un intento
  const umbralVigente = (s, f) => { const u = f === 1 ? s.uSuelta : s.uAhora; return u > 0 ? +(+u).toFixed(1) : null; };
  const recTime = () => (rec ? rec.base + rec.last - rec.t0 : 0);
  function recordUpdate(prev, s) {
    const take = () => {
      for (const [sid, id, ini, dur, rms] of s.acts) {
        const k = keyOf(s, sid), uid = s.boot + '#' + id;
        if (k !== rec.key || id <= rec.minId || rec.seen.has(uid)) continue;
        rec.seen.add(uid);
        rec.acts.push({ n: rec.acts.length + 1, inicioS: rec.base + ini - rec.t0, durS: dur, rms, reposo: rec.cal.reposoMV });
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
      // tiempo con el premio activo, medido con el reloj del atril (lo que el atril mostró, no la postura)
      const dt = prev && prev.modo === 'SESION' && keyOf(prev) === rec.key ? Math.max(0, Math.min(1, s.tSes - prev.tSes)) : 0;
      if (premioActivo(s)) rec.premioS += dt;
      const t = recTime(), n = s.b.length;
      s.b.forEach(([v, m, f], i) => rec.rms.push([+(t - (n - 1 - i) * 0.1).toFixed(1), v, m, f === 1 ? 1 : 0, umbralVigente(s, f)]));
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

    const enSes = !!(st && st.modo === 'SESION'), premio = enSes && premioActivo(st);
    const post = $('lv-postura');
    post.className = 'postura ' + (enSes ? (premio ? 'arriba' : 'abajo') : '');
    post.textContent = enSes ? (st.activo ? 'Intentando levantar' : premio ? 'Premio' : 'En reposo')
      : st && st.modo === 'PAUSA' ? 'En pausa' : '—';
    $('lv-tiempo').textContent = clock(recTime());
    const mv = metricas(rec ? rec.acts : [], recTime());
    $('lv-abajo').textContent = String(mv.intentos);
    $('lv-caidas').textContent = mv.intentosMin != null && recTime() >= 30 ? f1(mv.intentosMin) : '—';
    const cal = st && st.cal && st.cal.ok;
    $('lv-pie').textContent = (cal ? 'Calibración completada' : 'Falta calibrar') + ' · ' +
      (!senalVivo ? 'Señal en vivo apagada (Ajustes)' : emgConn.status === 'connected' || sim ? 'Señal disponible' : 'Sin señal del sensor');
    $('lv-pausa').textContent = st && st.modo === 'PAUSA' ? '▶ Continuar' : '❚❚ Pausar';
  }

  /* ---------- métricas: solo lo que el EMG mide de verdad ----------
   * El EMG ve el ESFUERZO de levantar la cabeza, no la postura. Por eso ya no hay
   * "tiempo con la cabeza erguida" ni "caídas": eran una suposición del atril. */
  const media = v => (v.length ? v.reduce((a, b) => a + b, 0) / v.length : null);
  const mediana = v => {
    if (!v.length) return null;
    const a = v.slice().sort((x, y) => x - y), k = a.length >> 1;
    return a.length % 2 ? a[k] : (a[k - 1] + a[k]) / 2;
  };
  const desv = v => { if (v.length < 2) return null; const m = media(v); return Math.sqrt(v.reduce((a, x) => a + (x - m) * (x - m), 0) / (v.length - 1)); };
  function metricas(acts, t) {
    const n = acts.length, min = t / 60;
    const durs = acts.map(a => a.durS);
    const rel = acts.filter(a => a.reposo > 0).map(a => a.rms / a.reposo);
    const huecos = [];
    for (let i = 1; i < n; i++) huecos.push(Math.max(0, acts[i].inicioS - (acts[i - 1].inicioS + acts[i - 1].durS)));
    return {
      intentos: n,
      intentosMin: min > 0 ? n / min : null,
      esfuerzoTotalS: durs.reduce((a, b) => a + b, 0),
      esfuerzoMedioS: media(durs),
      esfuerzoMedianaS: mediana(durs),
      esfuerzoDesvS: desv(durs),
      esfuerzoMaxS: n ? Math.max(...durs) : null,
      rmsMedioMV: media(acts.map(a => a.rms)),
      intensidadRel: media(rel),            // veces el nivel de reposo de la calibración
      entreIntentosS: media(huecos),
      primerIntentoS: n ? acts[0].inicioS : null,
      duracionMin: min
    };
  }
  // Las sesiones guardadas con la página anterior tienen las métricas viejas (cabeza erguida / caídas)
  const esVieja = u => u && u.intentos == null && (u.pctArriba != null || u.caidas != null);

  /* ---------- cada intento contra la señal guardada ----------
   * senal: filas [t, rms, mediana, detectorActivo, umbralVigente] cada 0.1 s.
   * Los tiempos del atril y de la señal se redondean a 0.1 s: por eso la ventana del intento se abre VENT_S de cada lado.
   * Reposo previo: mediana del RMS en los 2 s que terminan 0.5 s antes del intento, sin bloques con el detector activo. */
  const VENT_S = 0.2, REPOSO_PREV_S = 2, REPOSO_HUECO_S = 0.5;
  function analizarIntentos(eps, senal) {
    const S2 = senal || [];
    return (eps || []).map((a, i) => {
      const ini = a.inicioS, fin = a.inicioS + a.durS, sig = eps[i + 1];
      const dentro = S2.filter(f => f[0] >= ini - VENT_S && f[0] <= fin + VENT_S);
      const antes = S2.filter(f => f[0] >= ini - REPOSO_HUECO_S - REPOSO_PREV_S && f[0] < ini - REPOSO_HUECO_S && !f[3]);
      const alInicio = S2.find(f => f[0] >= ini - VENT_S && f[4] != null);
      const pico = dentro.length ? Math.max(...dentro.map(f => f[1])) : null;
      const reposoPrev = antes.length >= 5 ? mediana(antes.map(f => f[1])) : null;
      const razon = pico != null && reposoPrev > 0 ? pico / reposoPrev : null;
      return {
        n: a.n, inicioS: ini, durS: a.durS, rms: a.rms, pico, reposoPrev, razon,
        db: razon > 0 ? 20 * Math.log10(razon) : null,
        relCal: a.reposo > 0 ? a.rms / a.reposo : null,
        umbralIni: alInicio && alInicio[0] <= ini + VENT_S ? alInicio[4] : null,
        hastaSig: sig ? Math.max(0, sig.inicioS - fin) : null
      };
    });
  }

  /* ===================================================================
   * Fin de la sesión
   * =================================================================== */
  function buildRecord() {
    const t = recTime();
    return {
      patientId: patient.id, codigo: idDe(patient), fecha: rec.start, fisio: localStorage.getItem('emg.fisio') || '',
      duracionS: t, umbralMV: rec.umbral, calibracion: rec.cal, recalibraciones: rec.recal, observaciones: '',
      resumen: Object.assign(metricas(rec.acts, t), {
        tiempoPremioS: rec.premioS, pctPremio: t > 0 ? 100 * Math.min(t, rec.premioS) / t : null
      }),
      episodios: rec.acts.map(a => ({ n: a.n, inicioS: a.inicioS, durS: a.durS, rms: a.rms, reposo: a.reposo }))
    };
  }
  const seg = v => (v != null && isFinite(v) ? f1(v) + ' s' : '—');
  const veces = v => (v != null && isFinite(v) ? f1(v) + ' × reposo' : '—');
  function kpisSesion(r) {
    const u = r.resumen || {};
    if (esVieja(u)) return [['Duración de la terapia', clock(r.duracionS)], ['Umbral de activación', f1(r.umbralMV) + ' mV'],
      ['Métricas', 'Sesión guardada con la página anterior (medía una postura supuesta)']];
    return [
      ['Duración de la terapia', clock(r.duracionS)],
      ['Intentos de levantar la cabeza', String(u.intentos != null ? u.intentos : '—')],
      ['Intentos por minuto', u.intentosMin != null ? f1(u.intentosMin) : '—'],
      ['Esfuerzo medio por intento', seg(u.esfuerzoMedioS)],
      ['Esfuerzo más largo', seg(u.esfuerzoMaxS)],
      ['Intensidad media', veces(u.intensidadRel)],
      ['Reposo medio entre intentos', seg(u.entreIntentosS)],
      ['Tiempo con el premio activo', u.tiempoPremioS != null ? clock(u.tiempoPremioS) + (u.pctPremio != null ? ' · ' + pct(u.pctPremio) : '') : '—'],
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
    const nuevas = ses.filter(s => res(s).esfuerzoMedioS != null && isFinite(res(s).esfuerzoMedioS));
    const mejor = nuevas.length ? nuevas.reduce((a, s) => (res(s).esfuerzoMedioS > res(a).esfuerzoMedioS ? s : a), nuevas[0]) : null;
    pintarKpis($('p-resumen'), [
      ['Sesiones realizadas', String(ses.length)],
      ['Primera sesión', fmtDate(ses[0].fecha)],
      ['Última sesión', fmtDate(ses[ses.length - 1].fecha)],
      ['Tiempo total de terapia', clock(tot)],
      ['Intentos por minuto (promedio)', prom('intentosMin') != null ? f1(prom('intentosMin')) : '—'],
      ['Esfuerzo medio por intento (promedio)', seg(prom('esfuerzoMedioS'))],
      ['Esfuerzo más largo (promedio)', seg(prom('esfuerzoMaxS'))],
      ['Mejor sesión (esfuerzo medio)', mejor ? seg(res(mejor).esfuerzoMedioS) + ' · ' + fmtDate(mejor.fecha) : '—']
    ]);
    // Tendencia: cuánto sostiene cada esfuerzo (el objetivo es sostener la cabeza más tiempo)
    let msg = '';
    const pts = nuevas.map(s => res(s).esfuerzoMedioS);
    if (pts.length >= 2) {
      const k = pts.length >= 6 ? 3 : 1, avg = a => a.reduce((x, y) => x + y, 0) / a.length;
      const a = avg(pts.slice(0, k)), b = avg(pts.slice(-k)), d = b - a, rel = a > 0 ? d / a : 0;
      msg = (k > 1 ? 'Últimas 3 sesiones frente a las primeras 3: ' : 'Última sesión frente a la primera: ') +
        'esfuerzo medio de ' + f1(a) + ' s → ' + f1(b) + ' s por intento (' +
        (Math.abs(rel) < 0.1 ? 'sin cambio claro' : d > 0 ? 'sostiene más el esfuerzo' : 'sostiene menos el esfuerzo') + ').';
    }
    if (ses.some(s => esVieja(res(s)))) msg += (msg ? ' ' : '') + 'Las sesiones guardadas con la página anterior no entran en estas métricas.';
    $('p-tendencia').textContent = msg;
  }
  const METRICS = { intentosMin: 'intentos/min', esfuerzoMedioS: 's', esfuerzoMaxS: 's', intensidadRel: '× reposo', entreIntentosS: 's', duracionMin: 'min' };
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
        u.intentos != null ? String(u.intentos) : '—', u.intentosMin != null ? f1(u.intentosMin) : '—', seg(u.esfuerzoMedioS)]
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
      $('ses-txt').textContent = 'Señal del músculo durante la sesión. Lo sombreado es el tiempo con el detector activo (intento de levantar la cabeza).';
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
  // Número redondeado para el Excel ('' si no hay dato: la celda queda vacía)
  const nx = (v, d) => (v != null && v !== '' && isFinite(v) ? +(+v).toFixed(d) : '');
  const SENS = { 1: '1 · Más sensible', 2: '2 · Normal', 3: '3 · Menos sensible' };
  // Recalcula todo desde los intentos guardados: así también sirven las sesiones viejas
  function analisisSesion(r, senal) {
    const cal = r.calibracion || {};
    const eps = (r.episodios || []).map(a => Object.assign({}, a, { reposo: a.reposo > 0 ? a.reposo : cal.reposoMV }));
    const m = metricas(eps, r.duracionS || 0), u = res(r);
    const fil = analizarIntentos(eps, senal);
    const picos = fil.map(f => f.pico).filter(v => v != null), dbs = fil.map(f => f.db).filter(v => v != null);
    return { cal, m, u, fil, picoMedio: media(picos), dbMedio: media(dbs), dbDesv: desv(dbs), conSenal: !!(senal && senal.length) };
  }
  function hojaSesion(r, A) {
    const { cal, m, u } = A;
    return [['Fecha', fmtDateTime(r.fecha)], ['Fisioterapeuta responsable', r.fisio || ''],
      ['Duración de la terapia', clock(r.duracionS)], ['Duración (minutos)', nx(r.duracionS / 60, 2)],
      ['Versión del atril', cal.version || ''], ['Sensibilidad usada', SENS[cal.sensibilidad] || ''],
      ['', ''], ['CALIBRACIÓN (10 s en reposo)', ''],
      ['Reposo medio (mV)', nx(cal.reposoMV, 1)], ['Desviación del reposo (mV)', nx(cal.sdMV, 2)],
      ['Bloques de 0.1 s usados', nx(cal.bloques, 0)], ['Factor k (veces la desviación)', nx(cal.factorK, 1)],
      ['Margen mínimo (mV)', nx(cal.margenMinMV, 1)], ['Margen relativo (fracción del reposo)', nx(cal.margenRel, 2)],
      ['Umbral de activación de la calibración (mV)', nx(r.umbralMV, 1)], ['Umbral de relajación de la calibración (mV)', nx(cal.umbralOffMV, 1)],
      ['Calibración inestable', r.calibracion ? (cal.inestable ? 'Sí' : 'No') : ''], ['Veces que se volvió a calibrar', r.recalibraciones || 0],
      ['', ''], ['INTENTOS DE LEVANTAR LA CABEZA', ''],
      ['Intentos', m.intentos], ['Intentos por minuto', nx(m.intentosMin, 2)],
      ['Tiempo total de esfuerzo (s)', nx(m.esfuerzoTotalS, 1)],
      ['Duración media por intento (s)', nx(m.esfuerzoMedioS, 2)], ['Desviación estándar de la duración (s)', nx(m.esfuerzoDesvS, 2)],
      ['Mediana de la duración (s)', nx(m.esfuerzoMedianaS, 2)], ['Duración máxima (s)', nx(m.esfuerzoMaxS, 1)],
      ['RMS medio de los intentos (mV)', nx(m.rmsMedioMV, 1)], ['Intensidad media (veces el reposo de calibración)', nx(m.intensidadRel, 2)],
      ['Pico medio (mV)', nx(A.picoMedio, 1)], ['Pico / reposo previo, media (dB)', nx(A.dbMedio, 1)],
      ['Pico / reposo previo, desviación (dB)', nx(A.dbDesv, 1)],
      ['Reposo medio entre intentos (s)', nx(m.entreIntentosS, 1)], ['Primer intento (s desde el inicio)', nx(m.primerIntentoS, 1)],
      ['', ''], ['RETROALIMENTACIÓN DEL ATRIL', ''],
      ['Tiempo con el premio activo (s)', nx(u.tiempoPremioS, 1)], ['Tiempo con el premio activo (% de la sesión)', nx(u.pctPremio, 1)],
      ['', ''], ['Señal guardada', A.conSenal ? 'Sí' : 'No'],
      ['Observaciones', r.observaciones || '']];
  }
  // Una fila por intento. Las dos últimas columnas se llenan a mano viendo el video.
  const H_INTENTOS = ['N.º', 'Inicio (s)', 'Duración (s)', 'RMS medio (mV)', 'Pico RMS (mV)', 'Reposo previo (mV)',
    'Pico / reposo previo (veces)', 'Pico / reposo previo (dB)', 'RMS medio / reposo de calibración (veces)',
    'Umbral vigente al iniciar (mV)', 'Reposo hasta el siguiente intento (s)',
    'Latencia medida en video (s) — a mano', 'Activación falsa (S/N) — a mano'];
  const filaIntento = f => [f.n, nx(f.inicioS, 1), nx(f.durS, 1), nx(f.rms, 0), nx(f.pico, 0), nx(f.reposoPrev, 1),
    nx(f.razon, 2), nx(f.db, 1), nx(f.relCal, 2), nx(f.umbralIni, 1), nx(f.hastaSig, 1), '', ''];
  const ANCHOS_INTENTOS = [7, 10, 11, 13, 13, 15, 16, 16, 20, 17, 18, 20, 18];
  // Qué es cada dato (va como hoja en los dos Excel)
  const DEFINICIONES = [['Dato', 'Qué es y cómo se obtiene'],
    ['Intento', 'Un periodo en que el atril detecta esfuerzo del músculo: la mediana de 0.5 s del RMS supera el umbral vigente al menos 0.3 s y termina cuando baja del umbral de soltar 0.3 s. Mide el esfuerzo de levantar la cabeza, no la postura.'],
    ['Inicio (s)', 'Momento en que la señal cruzó el umbral, en segundos de terapia (sin pausas). Lo marca el atril.'],
    ['Duración (s)', 'Tiempo con el detector activo en ese intento. Lo mide el atril con resolución de 0.1 s.'],
    ['RMS medio (mV)', 'Promedio del RMS de los bloques de 0.1 s durante el intento. Lo calcula el atril (entero, en mV).'],
    ['Pico RMS (mV)', 'Mayor RMS de bloque de 0.1 s dentro del intento. Se calcula de la señal guardada con una ventana de ±0.2 s alrededor del intento.'],
    ['Reposo previo (mV)', 'Mediana del RMS en los 2 s que terminan 0.5 s antes del intento, sin bloques con el detector activo. Vacío si hay menos de 5 bloques.'],
    ['Pico / reposo previo', 'Pico RMS entre reposo previo, en veces y en dB (20·log10). Indica cuánto se separa el esfuerzo del reposo inmediato.'],
    ['RMS medio / reposo de calibración', 'RMS medio del intento entre el reposo medio de la calibración de 10 s.'],
    ['Umbral vigente (mV)', 'Umbral con el que el atril compara la mediana en ese momento: el de activación mientras no hay intento (se ajusta solo al nivel de reposo de los últimos 3 s y nunca baja del de la calibración) y el de soltar durante un intento. Sale del estado que el atril manda 5 veces por segundo.'],
    ['Reposo hasta el siguiente intento (s)', 'Tiempo entre el final de un intento y el inicio del siguiente.'],
    ['Tiempo con el premio activo', 'Tiempo en que el atril mostró el premio (estrellas, luz y música): se enciende al detectar un intento y sigue mientras dura el esfuerzo y al menos 8 s desde que se detectó el último. Es lo que el niño vio, no su postura. Solo existe en sesiones grabadas con esta versión de la página.'],
    ['Señal (RMS del bloque)', 'RMS de cada bloque de 0.1 s que manda el sensor al atril, en mV.'],
    ['Mediana de 0.5 s', 'Mediana de los últimos 5 bloques: es la que el atril compara con el umbral.'],
    ['Detector activo', '1 cuando el atril marca ese bloque dentro de un intento.'],
    ['Calibración', '10 s en reposo al inicio. Reposo medio y desviación (1.4826·MAD). Umbral de activación = reposo + máx(k·desviación, margen mínimo, margen relativo·reposo).'],
    ['Latencia y activación falsa', 'Columnas vacías para llenarlas a mano viendo el video de la sesión.'],
    ['Sesiones anteriores', 'Las sesiones guardadas con la página anterior no tienen tiempo de premio, sensibilidad, margen ni umbral vigente; sus intentos, duraciones y RMS medio sí son válidos.']];
  async function senalDe(s) {
    try {
      const d = await E.Store.senal(s.id);
      return d.map(x => (Array.isArray(x) ? x : [x.t, x.rms, x.med, x.act, x.u == null ? null : x.u]));
    } catch (e) { return []; }   // la sesión no tiene señal guardada
  }
  function excelSesion(r, p, senal) {
    const A = analisisSesion(r, senal);
    const hojas = [
      { nombre: 'Sesión', anchos: [44, 28], negritas: true,
        filas: [['Sesión de terapia ASTRA', '']].concat(datosPaciente(p), [['', '']], hojaSesion(r, A)), titulos: 1 },
      { nombre: 'Intentos', anchos: ANCHOS_INTENTOS, filas: [H_INTENTOS].concat(A.fil.map(filaIntento)), titulos: 1 }
    ];
    if (senal && senal.length) hojas.push({ nombre: 'Señal', anchos: [11, 22, 26, 22, 20],
      filas: [['Tiempo (s)', 'RMS del bloque de 0.1 s (mV)', 'Mediana de 0.5 s (mV) — la que compara el atril', 'Detector activo (1 = sí)', 'Umbral vigente (mV)']]
        .concat(senal.map(f => [f[0], f[1], f[2], f[3] ? 1 : 0, f[4] != null ? f[4] : ''])), titulos: 1 });
    hojas.push({ nombre: 'Definiciones', anchos: [34, 110], negritas: true, filas: DEFINICIONES, titulos: 1 });
    E.xlsx(archivo(p, r.fecha.slice(0, 16).replace(/[:T]/g, '-')), hojas);
  }
  async function descargarSesion(s, p) {
    excelSesion(s, p, await senalDe(s));
    toast('Excel de la sesión descargado.');
  }
  $('btn-p-excel').addEventListener('click', async () => {
    const p = patient, ses = E.Store.sessionsFor(p.id);
    if (!ses.length) { toast('Este paciente todavía no tiene sesiones.'); return; }
    const H = ['Fecha', 'Fisioterapeuta', 'Versión del atril', 'Sensibilidad', 'Duración (min)',
      'Reposo de calibración (mV)', 'Desviación de calibración (mV)', 'Umbral de activación de la calibración (mV)', 'Recalibraciones',
      'Intentos', 'Intentos por minuto', 'Duración media (s)', 'Desviación de la duración (s)', 'Mediana de la duración (s)', 'Duración máxima (s)',
      'RMS medio (mV)', 'RMS medio / reposo de calibración (veces)', 'Pico medio (mV)', 'Pico / reposo previo, media (dB)',
      'Reposo medio entre intentos (s)', 'Tiempo con el premio activo (s)', 'Premio activo (% de la sesión)', 'Señal guardada', 'Observaciones'];
    const filas = [], todos = [];
    for (const s of ses) {
      const A = analisisSesion(s, await senalDe(s)), { cal, m, u } = A;
      filas.push([fmtDateTime(s.fecha), s.fisio || '', cal.version || '', SENS[cal.sensibilidad] || '', nx(s.duracionS / 60, 2),
        nx(cal.reposoMV, 1), nx(cal.sdMV, 2), nx(s.umbralMV, 1), s.recalibraciones || 0,
        m.intentos, nx(m.intentosMin, 2), nx(m.esfuerzoMedioS, 2), nx(m.esfuerzoDesvS, 2), nx(m.esfuerzoMedianaS, 2), nx(m.esfuerzoMaxS, 1),
        nx(m.rmsMedioMV, 1), nx(m.intensidadRel, 2), nx(A.picoMedio, 1), nx(A.dbMedio, 1),
        nx(m.entreIntentosS, 1), nx(u.tiempoPremioS, 1), nx(u.pctPremio, 1), A.conSenal ? 'Sí' : 'No', s.observaciones || '']);
      A.fil.forEach(f => todos.push([fmtDateTime(s.fecha)].concat(filaIntento(f))));
    }
    E.xlsx(archivo(p, 'expediente'), [
      { nombre: 'Paciente', anchos: [30, 30], negritas: true, filas: datosPaciente(p).concat([['', ''], ['Notas', p.notas || '']]) },
      { nombre: 'Sesiones', anchos: [18, 20, 9, 18, 10, 14, 14, 18, 10, 9, 11, 12, 13, 13, 12, 11, 18, 11, 16, 15, 15, 13, 9, 40],
        filas: [H].concat(filas), titulos: 1 },
      { nombre: 'Todos los intentos', anchos: [18].concat(ANCHOS_INTENTOS), filas: [['Sesión'].concat(H_INTENTOS)].concat(todos), titulos: 1 },
      { nombre: 'Definiciones', anchos: [34, 110], negritas: true, filas: DEFINICIONES, titulos: 1 }
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
