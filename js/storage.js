/* Registros de ASTRA en ESTE navegador (IndexedDB), respaldos y exportación.
 *
 * - Pacientes, sesiones y la señal de cada sesión se guardan en IndexedDB del navegador
 *   (base "astra"). No se mandan a ningún servidor ni a internet; el ESP32 no guarda pacientes.
 * - Los datos pertenecen a ESTE dispositivo y ESTE navegador: otro celular no los ve
 *   (para pasarlos, se usa un respaldo .json).
 * - La interfaz lee de una copia en memoria (rápido y síncrono); cada cambio se escribe en
 *   IndexedDB y, si falla, se avisa (nunca se pierde un registro en silencio).
 * - Los ajustes (música elegida, fisioterapeuta) siguen en localStorage: son de comodidad.
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  function load(key, def) {
    try { const v = localStorage.getItem(key); return v ? JSON.parse(v) : def; } catch (e) { return def; }
  }
  function save(key, val) {
    try { localStorage.setItem(key, JSON.stringify(val)); return true; } catch (e) { return false; }
  }
  const uid = () => Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
  const numCodigo = c => parseInt(String(c || '').replace(/\D/g, ''), 10) || 0;
  const codigoDe = n => 'AST-' + String(n).padStart(4, '0');

  /* ---------- IndexedDB ---------- */
  const DB_NOMBRE = 'astra', DB_VERSION = 1;
  let db = null;                                   // null = sin IndexedDB (solo memoria)
  const mem = { pacientes: [], sesiones: [] };
  const estado = { modo: 'iniciando', error: '', persistente: null };

  function abrir() {
    return new Promise((ok, mal) => {
      if (!window.indexedDB) { mal(new Error('Este navegador no tiene IndexedDB')); return; }
      let r;
      try { r = indexedDB.open(DB_NOMBRE, DB_VERSION); } catch (e) { mal(e); return; }
      r.onupgradeneeded = () => {
        const d = r.result;
        if (!d.objectStoreNames.contains('pacientes')) {
          const p = d.createObjectStore('pacientes', { keyPath: 'id' });
          p.createIndex('codigo', 'codigo', { unique: true });      // nunca dos pacientes con el mismo ID
        }
        if (!d.objectStoreNames.contains('sesiones')) {
          const s = d.createObjectStore('sesiones', { keyPath: 'id' });
          s.createIndex('patientId', 'patientId', { unique: false });
        }
        if (!d.objectStoreNames.contains('senales')) d.createObjectStore('senales', { keyPath: 'id' });
      };
      r.onsuccess = () => ok(r.result);
      r.onerror = () => mal(r.error || new Error('No se pudo abrir la base de datos'));
      r.onblocked = () => mal(new Error('La base de datos está abierta en otra pestaña con otra versión'));
    });
  }
  // Una transacción: f(stores) hace los cambios; la promesa termina cuando quedaron ESCRITOS
  function tx(tablas, modo, f) {
    return new Promise((ok, mal) => {
      if (!db) { mal(new Error('No hay almacenamiento en este navegador')); return; }
      let t, res;
      try {
        t = db.transaction(tablas, modo);
        const st = {}; [].concat(tablas).forEach(n => { st[n] = t.objectStore(n); });
        res = f(st);
      } catch (e) { mal(e); return; }
      t.oncomplete = () => ok(res instanceof IDBRequest ? res.result : res);
      t.onerror = () => mal(t.error || new Error('Error al guardar'));
      t.onabort = () => mal(t.error || new Error('Se canceló al guardar (¿memoria llena?)'));
    });
  }
  const todos = tabla => tx(tabla, 'readonly', st => st[tabla].getAll());
  function mensajeError(e) {
    const n = e && e.name;
    if (n === 'QuotaExceededError') return 'el navegador no tiene más espacio. Descarga un respaldo y borra sesiones viejas.';
    if (n === 'ConstraintError') return 'ya existe un registro con ese ID.';
    return (e && e.message) || String(e);
  }
  function avisar(texto) { estado.error = texto; if (Store.onError) Store.onError(texto); }

  const Store = {
    onError: null,
    get remoto() { return false; },
    get estado() { return Object.assign({}, estado, { pacientes: mem.pacientes.length, sesiones: mem.sesiones.length }); },

    /** Abre la base de datos y carga los registros. Pasa una sola vez los datos viejos de localStorage. */
    async conectar() {
      try {
        db = await abrir();
        db.onversionchange = () => { db.close(); db = null; avisar('Se actualizó la base de datos en otra pestaña: recarga la página.'); };
        mem.pacientes = await todos('pacientes');
        mem.sesiones = await todos('sesiones');
        estado.modo = 'indexeddb';
        await migrarLocalStorage();
      } catch (e) {
        db = null; estado.modo = 'memoria';
        avisar('Este navegador no permite guardar registros (' + mensajeError(e) + '). Lo que registres se perderá al cerrar la página: usa Chrome o Safari normal (no privado).');
      }
      try { if (navigator.storage && navigator.storage.persist) estado.persistente = await navigator.storage.persist(); } catch (e) { /* no disponible */ }
      return estado.modo === 'indexeddb';
    },

    settings() {
      const saved = load('emg.settings', {});
      if (saved.cfgVer === EMG.DEFAULTS.cfgVer) return Object.assign({}, EMG.DEFAULTS, saved);
      const keep = {};
      ['sound', 'winS', 'mus_abajo', 'mus_arriba', 'volumen'].forEach(k => { if (k in saved) keep[k] = saved[k]; });
      return Object.assign({}, EMG.DEFAULTS, keep);
    },
    saveSettings(s) { return save('emg.settings', s); },

    patients() { return mem.pacientes.slice(); },
    sessions() { return mem.sesiones.slice(); },
    sessionsFor(pid) {
      return mem.sesiones.filter(s => s.patientId === pid).sort((a, b) => String(a.fecha).localeCompare(String(b.fecha)));
    },
    /** Siguiente ID de paciente: AST-0001, AST-0002… */
    nextCodigo() { return codigoDe(mem.pacientes.reduce((m, x) => Math.max(m, numCodigo(x.codigo)), 0) + 1); },

    /** Guarda (crea o actualiza) un paciente. Devuelve una promesa con el paciente ya ESCRITO. */
    async savePatient(p) {
      const nuevo = !p.id;
      if (nuevo) { p.id = uid(); p.creado = new Date().toISOString(); }
      if (!p.codigo) p.codigo = Store.nextCodigo();
      for (let intento = 0; ; intento++) {
        try {
          if (db) await tx('pacientes', 'readwrite', st => st.pacientes.put(p));
          break;
        } catch (e) {
          // Otro registro (p. ej. en otra pestaña) tomó ese ID: se usa el siguiente libre
          if (e && e.name === 'ConstraintError' && nuevo && intento < 5) { mem.pacientes = await todos('pacientes'); p.codigo = Store.nextCodigo(); continue; }
          if (nuevo) { delete p.id; delete p.creado; }
          avisar('No se pudo guardar el paciente: ' + mensajeError(e));
          throw e;
        }
      }
      const i = mem.pacientes.findIndex(x => x.id === p.id);
      if (i >= 0) mem.pacientes[i] = p; else mem.pacientes.push(p);
      return p;
    },

    /** Guarda una sesión y su señal [[t, rms, med, abajo], ...]. Promesa: termina cuando quedó escrita. */
    async saveSession(s, senal) {
      if (!s.id) s.id = uid();
      if (mem.sesiones.some(x => x.id === s.id)) return s;           // ya estaba: no se duplica
      s.conSenal = !!(senal && senal.length);
      try {
        if (db) await tx(['sesiones', 'senales'], 'readwrite', st => {
          st.sesiones.put(s);
          if (s.conSenal) st.senales.put({ id: s.id, datos: senal });
        });
      } catch (e) {
        avisar('No se pudo guardar la sesión: ' + mensajeError(e) + ' Descarga el Excel antes de salir.');
        throw e;
      }
      mem.sesiones.push(s);
      return s;
    },
    async deleteSession(id) {
      try { if (db) await tx(['sesiones', 'senales'], 'readwrite', st => { st.sesiones.delete(id); st.senales.delete(id); }); }
      catch (e) { avisar('No se pudo eliminar la sesión: ' + mensajeError(e)); throw e; }
      mem.sesiones = mem.sesiones.filter(s => s.id !== id);
    },
    /** Señal guardada de una sesión: [{t, rms, med, act}] */
    async senal(id) {
      if (!db) throw new Error('sin almacenamiento');
      const r = await tx('senales', 'readonly', st => st.senales.get(id));
      if (!r || !r.datos) throw new Error('Esta sesión no tiene señal guardada');
      return r.datos.map(([t, rms, med, act]) => ({ t, rms, med, act }));
    },

    /** Respaldo completo (pacientes, sesiones y señales) como texto JSON */
    async backup() {
      const senales = db ? await todos('senales') : [];
      return JSON.stringify({ app: 'ASTRA', version: 2, exportado: new Date().toISOString(),
        pacientes: mem.pacientes, sesiones: mem.sesiones, senales }, null, 1);
    },
    /** Importa un respaldo sin duplicar. Si un ID AST-xxxx ya lo usa OTRO paciente, el importado recibe uno nuevo. */
    async restore(text) {
      const d = typeof text === 'string' ? JSON.parse(text.replace(/^\uFEFF/, '')) : text;
      const P = d.pacientes || d.patients, S = d.sesiones || d.sessions, X = d.senales || [];
      if (!Array.isArray(P) || !Array.isArray(S)) throw new Error('El archivo no es un respaldo de ASTRA');
      const r = { patients: 0, sessions: 0, renombrados: 0, repetidos: 0 };
      const nuevosP = [], nuevasS = [], nuevasX = [];
      const ids = new Set(mem.pacientes.map(p => p.id)), codigos = new Map(mem.pacientes.map(p => [p.codigo, p.id]));
      let siguiente = mem.pacientes.reduce((m, x) => Math.max(m, numCodigo(x.codigo)), 0);
      P.forEach(p0 => {
        if (!p0 || !p0.id) return;
        if (ids.has(p0.id)) { r.repetidos++; return; }
        const p = Object.assign({}, p0);
        siguiente = Math.max(siguiente, numCodigo(p.codigo));
        if (!p.codigo || (codigos.has(p.codigo) && codigos.get(p.codigo) !== p.id)) {
          if (p.codigo) { p.codigoOriginal = p.codigo; r.renombrados++; }
          p.codigo = codigoDe(++siguiente);
        }
        ids.add(p.id); codigos.set(p.codigo, p.id); nuevosP.push(p);
      });
      const sesIds = new Set(mem.sesiones.map(s => s.id));
      S.forEach(s => {
        if (!s || !s.id || !s.patientId || !ids.has(s.patientId)) return;
        if (sesIds.has(s.id)) { r.repetidos++; return; }
        sesIds.add(s.id); nuevasS.push(s);
      });
      const nuevasIds = new Set(nuevasS.map(s => s.id));
      X.forEach(x => { if (x && nuevasIds.has(x.id) && Array.isArray(x.datos)) nuevasX.push(x); });
      try {
        if (db) await tx(['pacientes', 'sesiones', 'senales'], 'readwrite', st => {
          nuevosP.forEach(p => st.pacientes.put(p)); nuevasS.forEach(s => st.sesiones.put(s)); nuevasX.forEach(x => st.senales.put(x));
        });
      } catch (e) { avisar('No se pudo importar: ' + mensajeError(e)); throw e; }
      mem.pacientes.push(...nuevosP); mem.sesiones.push(...nuevasS);
      r.patients = nuevosP.length; r.sessions = nuevasS.length;
      return r;
    }
  };

  // Una sola vez: lo que guardaban las versiones anteriores en localStorage pasa a IndexedDB
  async function migrarLocalStorage() {
    if (load('emg.migrado', false)) return;
    const P = load('emg.patients', []), S = load('emg.sessions', []);
    if (P.length || S.length) {
      const r = await Store.restore({ pacientes: P, sesiones: S });
      if (r.patients || r.sessions) avisar('Se pasaron ' + r.patients + ' paciente(s) y ' + r.sessions + ' sesión(es) guardados antes en este navegador.');
    }
    save('emg.migrado', true);
  }

  function csvCell(v) {
    if (v === null || v === undefined || (typeof v === 'number' && !isFinite(v))) return '';
    if (typeof v === 'number') return String(Math.round(v * 10000) / 10000);
    const s = String(v);
    return /[",\n;]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
  }
  function toCSV(headers, rows) {
    return [headers.join(',')].concat(rows.map(r => r.map(csvCell).join(','))).join('\r\n');
  }
  // Descarga un archivo. En iPhone (Safari) se abre "Descargas"/"Guardar en Archivos".
  function descargarBlob(filename, blob) {
    const url = URL.createObjectURL(blob), a = document.createElement('a');
    a.href = url; a.download = filename; a.rel = 'noopener';
    document.body.appendChild(a); a.click(); a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 60000);
  }
  function download(filename, text, mime) {
    mime = mime || 'text/csv';
    const bom = mime === 'text/csv' ? '\uFEFF' : '';   // BOM solo para CSV (Excel lee los acentos); el JSON va sin BOM
    descargarBlob(filename, new Blob([bom + text], { type: mime + ';charset=utf-8' }));
  }
  const slug = s => s.normalize('NFD').replace(/[̀-ͯ]/g, '').replace(/[^a-zA-Z0-9]+/g, '_').replace(/^_|_$/g, '');

  EMG.Store = Store; EMG.toCSV = toCSV; EMG.download = download; EMG.descargarBlob = descargarBlob; EMG.slug = slug;
})();
