/* Almacenamiento local (localStorage), exportación CSV/JSON y envío opcional a Google Sheets. */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  function load(key, def) {
    try { const v = localStorage.getItem(key); return v ? JSON.parse(v) : def; } catch (e) { return def; }
  }
  function save(key, val) {
    try { localStorage.setItem(key, JSON.stringify(val)); return true; } catch (e) { return false; }
  }
  const uid = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 6);

  /* Dos modos con la misma interfaz:
   *  - SERVIDOR (la página se abrió desde servidor/servidor.js): pacientes y sesiones se guardan en la
   *    laptop (servidor/datos/). Se cargan una vez al abrir y cada cambio se manda a /api/... al momento.
   *  - NAVEGADOR (index.html abierto como archivo): como antes, en localStorage. */
  const remoto = { activo: false, pacientes: [], sesiones: [] };
  async function pedir(metodo, url, cuerpo) {
    const r = await fetch(url, { method: metodo, headers: cuerpo ? { 'Content-Type': 'application/json' } : undefined,
      body: cuerpo ? JSON.stringify(cuerpo) : undefined, cache: 'no-store' });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.error || 'HTTP ' + r.status);
    return j;
  }
  function avisarError(e) { if (Store.onError) Store.onError('No se pudo guardar en el servidor: ' + e.message); }

  const Store = {
    onError: null,
    get remoto() { return remoto.activo; },
    /** Usa el servidor si la página vino de él. Devuelve true si quedó en modo servidor. */
    async conectar() {
      if (!/^https?:$/.test(location.protocol)) return false;
      try {
        const d = await pedir('GET', '/api/datos');
        remoto.pacientes = d.pacientes || []; remoto.sesiones = d.sesiones || []; remoto.activo = true;
      } catch (e) { remoto.activo = false; }
      return remoto.activo;
    },
    // Lo que quedó guardado en este navegador antes de usar el servidor (para subirlo)
    datosLocales() { return { patients: load('emg.patients', []), sessions: load('emg.sessions', []) }; },
    settings() {
      const saved = load('emg.settings', {});
      if (saved.cfgVer === EMG.DEFAULTS.cfgVer) return Object.assign({}, EMG.DEFAULTS, saved);
      // Ajustes de una versión anterior (p. ej. 2000 Hz): solo se conservan los que no dependen del hardware
      const keep = {};
      ['sheetsUrl', 'sheetsName', 'sound', 'winS'].forEach(k => { if (k in saved) keep[k] = saved[k]; });
      return Object.assign({}, EMG.DEFAULTS, keep);
    },
    saveSettings(s) { return save('emg.settings', s); },
    patients() { return remoto.activo ? remoto.pacientes.slice() : load('emg.patients', []); },
    sessions() { return remoto.activo ? remoto.sesiones.slice() : load('emg.sessions', []); },
    sessionsFor(pid) {
      return Store.sessions().filter(s => s.patientId === pid).sort((a, b) => a.fecha.localeCompare(b.fecha));
    },
    /** Siguiente ID de paciente: AST-0001, AST-0002… */
    nextCodigo() {
      const n = Store.patients().reduce((m, x) => Math.max(m, parseInt(String(x.codigo || '').replace(/\D/g, ''), 10) || 0), 0) + 1;
      return 'AST-' + String(n).padStart(4, '0');
    },
    savePatient(p) {
      if (!p.id) { p.id = uid(); p.creado = new Date().toISOString(); }
      if (!p.codigo) p.codigo = Store.nextCodigo();   // ID con el que se identifica al paciente (sin nombre)
      if (remoto.activo) {
        const i = remoto.pacientes.findIndex(x => x.id === p.id); if (i >= 0) remoto.pacientes[i] = p; else remoto.pacientes.push(p);
        pedir('POST', '/api/pacientes', p).catch(avisarError);
        return p;
      }
      const list = Store.patients();
      const i = list.findIndex(x => x.id === p.id); if (i >= 0) list[i] = p; else list.push(p);
      return save('emg.patients', list) ? p : null;
    },
    /** senal (opcional): [[tiempo_s, rms_mV, mediana_mV, activacion], ...] — en modo servidor se guarda como CSV */
    saveSession(s, senal) {
      s.id = s.id || uid();
      if (remoto.activo) {
        remoto.sesiones.push(s);
        return pedir('POST', '/api/sesiones', { sesion: s, senal: senal || [] })
          .then(r => { Object.assign(s, r.sesion); return s; })
          .catch(e => { remoto.sesiones = remoto.sesiones.filter(x => x.id !== s.id); throw e; });
      }
      const list = Store.sessions(); list.push(s);
      return save('emg.sessions', list) ? Promise.resolve(s) : Promise.reject(new Error('el navegador no tiene espacio'));
    },
    deleteSession(id) {
      if (remoto.activo) { remoto.sesiones = remoto.sesiones.filter(s => s.id !== id); pedir('DELETE', '/api/sesiones/' + id).catch(avisarError); return; }
      save('emg.sessions', Store.sessions().filter(s => s.id !== id));
    },
    /** Señal guardada de una sesión (solo modo servidor): [{t, rms, med, act}] */
    async senal(id) {
      if (!remoto.activo) throw new Error('La señal de cada sesión se guarda cuando usas la página desde el servidor');
      const r = await fetch('/api/sesiones/' + id + '/senal', { cache: 'no-store' });
      if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || 'HTTP ' + r.status);
      return (await r.text()).trim().split(/\r?\n/).slice(1).map(l => { const [t, rms, med, act] = l.split(',').map(Number); return { t, rms, med, act }; });
    },
    backup() {
      return JSON.stringify({ version: 1, exportado: new Date().toISOString(),
        patients: Store.patients(), sessions: Store.sessions() }, null, 1);
    },
    async restore(text) {
      const d = typeof text === 'string' ? JSON.parse(text) : text;
      if (!Array.isArray(d.patients) || !Array.isArray(d.sessions)) throw new Error('Archivo no válido');
      if (remoto.activo) {
        const r = await pedir('POST', '/api/restaurar', d);
        await Store.conectar();
        return { patients: r.pacientes, sessions: r.sesiones };
      }
      const P = Store.patients(), S = Store.sessions();
      d.patients.forEach(p => { if (!P.some(x => x.id === p.id)) P.push(p); });
      d.sessions.forEach(s => { if (!S.some(x => x.id === s.id)) S.push(s); });
      save('emg.patients', P); save('emg.sessions', S);
      return { patients: d.patients.length, sessions: d.sessions.length };
    }
  };

  function csvCell(v) {
    if (v === null || v === undefined || (typeof v === 'number' && !isFinite(v))) return '';
    if (typeof v === 'number') return String(Math.round(v * 10000) / 10000);
    const s = String(v);
    return /[",\n;]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }
  function toCSV(headers, rows) {
    return [headers.join(',')].concat(rows.map(r => r.map(csvCell).join(','))).join('\r\n');
  }
  function download(filename, text, mime) {
    const blob = new Blob(['﻿' + text], { type: (mime || 'text/csv') + ';charset=utf-8' }); // BOM: Excel lee acentos
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob); a.download = filename;
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  }
  const slug = s => s.normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-zA-Z0-9]+/g, '_').replace(/^_|_$/g, '');

  async function sendToSheets(url, payload) {
    // Apps Script no devuelve CORS, por eso 'no-cors': se envía pero no se puede leer la respuesta.
    await fetch(url, { method: 'POST', mode: 'no-cors', headers: { 'Content-Type': 'text/plain;charset=utf-8' },
      body: JSON.stringify(payload) });
  }

  EMG.Store = Store; EMG.toCSV = toCSV; EMG.download = download; EMG.slug = slug; EMG.sendToSheets = sendToSheets;
})();
