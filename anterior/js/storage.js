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

  const Store = {
    settings() { return Object.assign({}, EMG.DEFAULTS, load('emg.settings', {})); },
    saveSettings(s) { return save('emg.settings', s); },
    patients() { return load('emg.patients', []); },
    sessions() { return load('emg.sessions', []); },
    sessionsFor(pid) {
      return Store.sessions().filter(s => s.patientId === pid).sort((a, b) => a.fecha.localeCompare(b.fecha));
    },
    savePatient(p) {
      const list = Store.patients();
      if (!p.id) { p.id = uid(); p.creado = new Date().toISOString(); list.push(p); }
      else { const i = list.findIndex(x => x.id === p.id); if (i >= 0) list[i] = p; else list.push(p); }
      return save('emg.patients', list) ? p : null;
    },
    saveSession(s) {
      const list = Store.sessions();
      s.id = s.id || uid(); list.push(s);
      return save('emg.sessions', list) ? s : null;
    },
    deleteSession(id) { save('emg.sessions', Store.sessions().filter(s => s.id !== id)); },
    backup() {
      return JSON.stringify({ version: 1, exportado: new Date().toISOString(),
        patients: Store.patients(), sessions: Store.sessions() }, null, 1);
    },
    restore(text) {
      const d = JSON.parse(text);
      if (!Array.isArray(d.patients) || !Array.isArray(d.sessions)) throw new Error('Archivo no válido');
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
