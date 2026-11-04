/* Conexión con el atril (ESP32, esp32_calibracion.ino v3).
 * El atril calibra y detecta; la página solo muestra su estado y le manda comandos:
 *   GET http://<ip del atril>/estado?b=&a=&l=   -> estado + bloques RMS, activaciones y mensajes nuevos
 *   GET http://<ip del atril>/cmd?c=<letra>     -> c 1 2 3 x s n p r h + - i
 * AtrilSim reproduce la misma lógica del firmware para probar sin hardware.
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  class AtrilHttp {
    constructor(ip) { this.base = 'http://' + ip; this.kind = 'wifi'; }
    async _get(path, ms) {
      const ctrl = new AbortController(), to = setTimeout(() => ctrl.abort(), ms);
      try {
        const r = await fetch(this.base + path, EMG.fetchOpts(this.base + path, { signal: ctrl.signal }));
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return await r.json();
      } catch (e) {
        throw new Error(e.name === 'AbortError' ? 'no responde' : (e.message || e.name));
      } finally { clearTimeout(to); }
    }
    estado(q) { return this._get('/estado?b=' + q.b + '&a=' + q.a + '&l=' + q.l, 2500); }
    cmd(c) { return this._get('/cmd?c=' + encodeURIComponent(c), 2500); }
    stop() { }
  }

  /* Pide el estado ~6 veces por segundo y reparte lo nuevo. */
  class AtrilLink {
    constructor(backend, onEstado, onStatus) {
      this.be = backend; this.onEstado = onEstado; this.onStatus = onStatus;
      this.q = { b: 0, a: 0, l: 0 }; this.boot = null; this.run = true; this.status = '';
      this._set('connecting');
      this._loop();
    }
    get kind() { return this.be.kind; }
    _set(s, msg) { if (s !== this.status || msg) { this.status = s; this.onStatus(s, msg || ''); } }
    async _loop() {
      let fails = 0;
      while (this.run) {
        const t0 = performance.now();
        try {
          const st = await this.be.estado(this.q);
          if (!this.run) break;
          if (this.boot !== null && st.boot !== this.boot) st.reinicio = true;   // el ESP32 se reinició
          this.boot = st.boot;
          this.q = { b: st.bn, a: st.an, l: st.ln };
          fails = 0;
          this._set('connected');
          try { this.onEstado(st); } catch (e) { console.error('Error al mostrar el estado del atril:', e); }
        } catch (e) {
          if (!this.run) break;
          if (++fails >= 2) this._set('error', e.message);
        }
        const wait = (this.status === 'connected' ? 160 : 1500) - (performance.now() - t0);
        await sleep(Math.max(20, wait));
      }
    }
    async cmd(c) {
      try { await this.be.cmd(c); return true; } catch (e) { return false; }
    }
    stop() { this.run = false; this.be.stop(); this._set('disconnected'); }
  }

  /* ===================================================================
   * Atril simulado: misma máquina de estados que esp32_calibracion.ino.
   * Recibe un RMS (mV) cada 100 ms con feedBlock(), como el ESP32 por el opto.
   * =================================================================== */
  const FASES = ['ESPERA', 'REPOSO', 'PASIVA', 'ARRIBA', 'ABAJO', 'LISTA', 'FALLIDA'];
  const PASOS = ['REPOSO', 'PASIVA', 'ACTIVA'];

  class AtrilSim {
    constructor() {
      this.kind = 'sim';
      this.cfg = { reposo: 10, pasiva: 10, reps: 5, arriba: 4, abajo: 4, descR: 1, descP: 0.5, descIni: 0.7, descFin: 0.3, bps: 10 };
      this.boot = Math.floor(Math.random() * 1e9);
      this.blq = []; this.nBlq = 0; this.acts = []; this.nAct = 0; this.logs = []; this.nLog = 0;
      this.modo = 'ARRANQUE'; this.fase = 'ESPERA'; this.hecho = [false, false, false];
      this.bloq = 0; this.rep = 0;
      this.c = {}; this._resetCal();
      this.apl = { reposo: 100, sd: 10, restRms: 0, pasMedia: 0, pasPico: 0, actRms: 0, actEnv: 0, snr: 0, art: 0, maximo: 1000 };
      this.calibrado = false; this.umbralPct = 0.25; this.uon = 300; this.uoff = 200;
      this.rms = 0; this.suave = 0; this.hayDatos = false; this.tDato = -1e9; this.datosAntes = false;
      this.activo = false; this.tSobre = 0; this.tBajo = 0; this.tIni = 0; this.nivel = 0;
      this.sumaAct = 0; this.nRmsAct = 0; this.sumaCola = 0; this.nCola = 0;
      this.sid = 0; this.nActiv = 0; this.tActMs = 0; this.msSes = 0; this.msSesIni = 0;
      this.pos = -9000; this.dir = 1;                                  // estrellas: 0 = tope, -17400 = fondo
      this.tPrev = performance.now();
      this._msg('=== ASTRA - atril v3.0 (simulado) ===');
      this._msg('>> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).');
      this.timer = setInterval(() => this._loop(), 50);
    }
    stop() { clearInterval(this.timer); }

    /* ---------- lo que el generador de señal necesita saber ---------- */
    simCtx() {
      if (this.modo === 'SESION') return { phase: 'running' };
      if (this.modo === 'CALIBRANDO') {
        if (this.fase === 'PASIVA') return { phase: 'passive' };
        if (this.fase === 'ARRIBA' || this.fase === 'ABAJO') return { phase: 'active', cueUp: this.fase === 'ARRIBA' };
      }
      return { phase: 'rest' };
    }

    _msg(t) { this.logs.push(t); if (this.logs.length > 24) this.logs.shift(); this.nLog++; }
    _ms() { return performance.now(); }
    _datosOK() { return this.hayDatos && this._ms() - this.tDato < 500; }
    _midiendo() { return ['REPOSO', 'PASIVA', 'ARRIBA', 'ABAJO'].includes(this.fase); }
    _resetCal() {
      this.c = { reposo: 0, sd: 0, restRms: 0, pasMedia: 0, pasPico: 0, actRms: 0, actEnv: 0, snr: 0, art: 0, repRms: [0, 0, 0, 0, 0] };
      this.acc = { sR: 0, s2R: 0, nR: 0, sP: 0, nP: 0, picoP: 0, sA: 0, s2A: 0, nA: 0, sRepRms: 0, sRepEnv: 0, nReps: 0 };
    }
    _reiniciarFase(f) {
      const a = this.acc;
      if (f === 0) { a.sR = a.s2R = 0; a.nR = 0; }
      else if (f === 1) { a.sP = 0; a.nP = 0; a.picoP = 0; }
      else { a.sA = a.s2A = 0; a.nA = 0; a.sRepRms = a.sRepEnv = 0; a.nReps = 0; this.c.repRms = [0, 0, 0, 0, 0]; }
      this.hecho[f] = false;
    }
    _umbrales() {
      this.uon = Math.max(this.apl.reposo + 3 * this.apl.sd, this.umbralPct * this.apl.actEnv);
      this.uoff = 0.6 * this.uon;
    }

    /* ---------- modos ---------- */
    _cerrarActivacion() { this._terminarActivacion(this.tBajo || this._ms()); }
    _terminarActivacion(fin) {
      if (this.activo) {
        this.activo = false;
        const dur = fin - this.tIni; this.tActMs += dur;
        const rmsMedio = this.nRmsAct ? this.sumaAct / this.nRmsAct : 0;
        const pct = Math.round(100 * (rmsMedio - this.apl.reposo) / Math.max(this.apl.maximo - this.apl.reposo, 1));
        this._msg('   Activacion #' + this.nActiv + ': ' + (dur / 1000).toFixed(1) + ' s, RMS medio ' + Math.round(rmsMedio) + ' mV (' + pct + ' % de la referencia)');
        this.acts.push([this.sid, this.nActiv, +(this.msSesIni / 1000).toFixed(1), +(dur / 1000).toFixed(1), Math.round(rmsMedio), pct]);
        if (this.acts.length > 32) this.acts.shift();
        this.nAct++;
      }
      this.tSobre = this.tBajo = 0;
    }
    _iniciarCalibracion() {
      this._cerrarActivacion();
      this.modo = 'CALIBRANDO'; this.fase = 'ESPERA'; this.bloq = 0; this.rep = 0;
      for (let f = 0; f < 3; f++) this._reiniciarFase(f);
      this._resetCal();
      this._msg('>> CALIBRACION (60 s): REPOSO 10 s -> PASIVA 10 s -> 5 x (ARRIBA 4 s + ABAJO 4 s)');
      this._msg('>> Esperando: comienza el REPOSO desde la pagina, con BOOT o con la tecla 1.');
    }
    _iniciarSesion(nueva) {
      if (nueva) { this.sid++; this.nActiv = 0; this.tActMs = 0; this.msSes = 0; }
      this.modo = 'SESION'; this.activo = false; this.tSobre = this.tBajo = 0; this.suave = this.apl.reposo;
      this._msg('>> SESION: relaja para subir las estrellas.');
    }
    _imprimir() {
      const a = this.apl, f1 = v => v.toFixed(1);
      this._msg('---- Calibracion ----');
      this._msg('Reposo (envolvente): ' + f1(a.reposo) + ' mV');
      this._msg('Ruido en reposo (SD): ' + f1(a.sd) + ' mV');
      this._msg('Pasiva: media ' + f1(a.pasMedia) + ' mV, pico ' + f1(a.pasPico) + ' mV');
      this._msg('Activa: RMS de referencia ' + f1(a.actRms) + ' mV (envolvente ' + f1(a.actEnv) + ' mV, ' + this.acc.nReps + ' repeticiones)');
      this._msg('Relacion senal/reposo (SNR): ' + f1(a.snr));
      this._msg('Artefacto pasivo / activa: ' + Math.round(a.art) + ' %');
      this._msg('Umbral ON: ' + f1(this.uon) + ' mV | Umbral OFF: ' + f1(this.uoff) + ' mV');
      this._msg('---------------------');
    }
    _terminarCalibracion() {
      const c = this.c;
      c.snr = c.restRms > 0 ? c.actRms / c.restRms : 0;
      c.art = c.actEnv > 0 ? 100 * c.pasPico / c.actEnv : 0;
      if (c.actRms <= 0) {
        this._msg('!! No se detecto contraccion activa. Revisa electrodos y repite la fase ACTIVA (3) o toda la calibracion (c).');
        this.fase = 'FALLIDA'; return;
      }
      Object.assign(this.apl, { reposo: c.reposo, sd: c.sd, restRms: c.restRms, pasMedia: c.pasMedia, pasPico: c.pasPico,
        actRms: c.actRms, actEnv: c.actEnv, snr: c.snr, art: c.art, maximo: c.actEnv });
      this._umbrales(); this._imprimir();
      if (c.snr < 3) this._msg('!! SNR < 3: la contraccion apenas se distingue del reposo (mal contacto o electrodo mal ubicado).');
      if (c.art > 80) this._msg('!! Artefacto > 80 %: las transiciones podrian contarse como activaciones (sube el umbral con + o mejora la fijacion de cables).');
      this.calibrado = true; this.fase = 'LISTA';
      this._msg('>> Calibracion LISTA. Inicia la sesion desde la pagina, con BOOT o con la tecla s.');
    }
    _finFase() {
      if (this.hecho.every(Boolean)) { this._terminarCalibracion(); return; }
      this.fase = 'ESPERA'; this.bloq = 0;
      const sig = !this.hecho[0] ? 0 : !this.hecho[1] ? 1 : 2;
      this._msg('>> Esperando: comienza la fase ' + PASOS[sig] + ' desde la pagina, con BOOT o con la tecla ' + (sig + 1) + '.');
    }
    _cambiarFase(f) {
      this.fase = f; this.bloq = 0;
      if (f === 'REPOSO') this._msg('>> FASE 1 - REPOSO (10 s): cabeza apoyada y relajada. No mover al nino.');
      else if (f === 'PASIVA') this._msg('>> FASE 2 - PASIVA (10 s): el terapeuta mueve la cabeza de abajo hacia arriba y viceversa. El nino NO hace fuerza.');
      else if (f === 'ARRIBA') this._msg('>> FASE 3 - ACTIVA, repeticion ' + this.rep + '/5: ARRIBA! El nino levanta la cabeza y la sostiene (4 s).');
      else if (f === 'ABAJO') this._msg('   ABAJO: baja la cabeza y relaja (4 s).');
    }
    _empezarFase(f) {
      if (this.modo !== 'CALIBRANDO') { this._msg('!! Primero comienza la calibracion (c).'); return; }
      if (this._midiendo()) { this._msg('!! Ya se esta midiendo la fase ' + this.fase + '. Espera a que termine o cancelala (x).'); return; }
      if (f === 1 && !this.hecho[0]) { this._msg('!! Primero haz la fase de REPOSO (1).'); return; }
      if (f === 2 && !this.hecho[1]) { this._msg('!! Primero haz la fase PASIVA (2).'); return; }
      this._reiniciarFase(f);
      if (f === 0) this._cambiarFase('REPOSO');
      else if (f === 1) this._cambiarFase('PASIVA');
      else { this.rep = 1; this._cambiarFase('ARRIBA'); }
    }
    _siguiente() {
      if (this.modo === 'SESION' || this.modo === 'PAUSA') { this._iniciarCalibracion(); return; }
      if (this.modo !== 'CALIBRANDO' || this._midiendo()) return;
      if (this.fase === 'LISTA') { this._iniciarSesion(true); return; }
      if (this.fase === 'FALLIDA') { this._empezarFase(2); return; }
      this._empezarFase(!this.hecho[0] ? 0 : !this.hecho[1] ? 1 : 2);
    }

    cmd(c) {
      switch (c) {
        case 'c':
          if (this.modo === 'ARRANQUE') this._msg('!! Espera a que las estrellas encuentren el tope.');
          else this._iniciarCalibracion();
          break;
        case '1': this._empezarFase(0); break;
        case '2': this._empezarFase(1); break;
        case '3': this._empezarFase(2); break;
        case 'x':
          if (this.modo === 'CALIBRANDO' && this._midiendo()) {
            const f = this.fase === 'REPOSO' ? 0 : this.fase === 'PASIVA' ? 1 : 2;
            this._reiniciarFase(f); this.fase = 'ESPERA'; this.bloq = 0;
            this._msg('>> Fase ' + PASOS[f] + ' cancelada. Puedes volver a empezarla.');
          }
          break;
        case 's':
          if (this.modo === 'CALIBRANDO' && this.fase === 'LISTA') this._iniciarSesion(true);
          else this._msg('!! La sesion empieza cuando la calibracion este LISTA (reposo, pasiva y activa).');
          break;
        case 'n': this._siguiente(); break;
        case 'p':
          if (this.modo === 'SESION') { this._cerrarActivacion(); this.modo = 'PAUSA'; this._msg('>> PAUSA (p para seguir)'); }
          else if (this.modo === 'PAUSA') this._iniciarSesion(false);
          break;
        case 'h': this._cerrarActivacion(); this.modo = 'ARRANQUE'; this._msg('>> Buscando el tope de las estrellas.'); break;
        case '+': this.umbralPct = Math.min(this.umbralPct + 0.05, 0.8); this._umbrales(); this._imprimir(); break;
        case '-': this.umbralPct = Math.max(this.umbralPct - 0.05, 0.05); this._umbrales(); this._imprimir(); break;
        case 'i': this._imprimir(); break;
      }
      return Promise.resolve({ ok: true });
    }

    /* ---------- un bloque RMS del "opto" ---------- */
    feedBlock(mv) {
      const ahora = this._ms();
      this.rms = mv;
      this.suave = this.hayDatos ? 0.5 * mv + 0.5 * this.suave : mv;
      this.hayDatos = true; this.tDato = ahora;
      let flag = 0;
      if (this.modo === 'CALIBRANDO') flag = this._calibrarBloque(mv) ? 2 : 0;
      else if (this.modo === 'SESION') {
        const a = this.apl;
        this.nivel = Math.min(1, Math.max(0, (this.suave - a.reposo) / Math.max(a.maximo - a.reposo, 1)));
        if (!this.activo) {
          if (this.suave >= this.uon) {
            if (!this.tSobre) { this.tSobre = ahora; this.sumaAct = 0; this.nRmsAct = 0; this.msSesIni = this.msSes; }
            this.sumaAct += mv; this.nRmsAct++;
            if (ahora - this.tSobre + 50 >= 300) { this.activo = true; this.tIni = this.tSobre; this.nActiv++; this.tBajo = 0; }
          } else this.tSobre = 0;
        } else if (this.suave < this.uoff) {
          if (!this.tBajo) { this.tBajo = ahora; this.sumaCola = 0; this.nCola = 0; }
          this.sumaCola += mv; this.nCola++;
          if (ahora - this.tBajo + 50 >= 300) this._terminarActivacion(this.tBajo);
        } else {
          if (this.tBajo) { this.sumaAct += this.sumaCola; this.nRmsAct += this.nCola; this.tBajo = 0; }
          this.sumaAct += mv; this.nRmsAct++;
        }
        flag = this.activo ? 1 : 0;
      }
      this.blq.push([mv, flag]); if (this.blq.length > 64) this.blq.shift(); this.nBlq++;
    }
    _calibrarBloque(env) {
      if (!this._midiendo()) return false;
      const a = this.acc, c = this.c; let usado = false;
      this.bloq++;
      if (this.fase === 'REPOSO') {
        if (this.bloq > 10) { a.sR += env; a.s2R += env * env; a.nR++; usado = true; }
        if (this.bloq >= 100) {
          c.reposo = a.nR ? a.sR / a.nR : 0;
          c.sd = Math.sqrt(Math.max(a.nR ? a.s2R / a.nR - c.reposo * c.reposo : 0, 0));
          c.restRms = a.nR ? Math.sqrt(a.s2R / a.nR) : 0;
          this.hecho[0] = true;
          this._msg('   Reposo listo: media ' + c.reposo.toFixed(1) + ' mV, SD ' + c.sd.toFixed(1) + ' mV.');
          this._finFase();
        }
      } else if (this.fase === 'PASIVA') {
        if (this.bloq > 5) { a.sP += env; a.nP++; a.picoP = Math.max(a.picoP, env); usado = true; }
        if (this.bloq >= 100) {
          c.pasMedia = a.nP ? a.sP / a.nP : 0; c.pasPico = a.picoP; this.hecho[1] = true;
          this._msg('   Pasiva lista: media ' + c.pasMedia.toFixed(1) + ' mV, pico ' + c.pasPico.toFixed(1) + ' mV.');
          this._finFase();
        }
      } else if (this.fase === 'ARRIBA') {
        if (this.bloq > 7 && this.bloq <= 37) { a.sA += env; a.s2A += env * env; a.nA++; usado = true; }
        if (this.bloq >= 40) {
          if (a.nA > 0) {
            const rms = Math.sqrt(a.s2A / a.nA), media = a.sA / a.nA;
            a.sRepRms += rms; a.sRepEnv += media; a.nReps++; c.repRms[this.rep - 1] = +rms.toFixed(1);
            this._msg('   Repeticion ' + this.rep + ': RMS ' + rms.toFixed(1) + ' mV, envolvente ' + media.toFixed(1) + ' mV');
          }
          a.sA = a.s2A = 0; a.nA = 0;
          this._cambiarFase('ABAJO');
        }
      } else if (this.fase === 'ABAJO' && this.bloq >= 40) {
        if (this.rep < 5) { this.rep++; this._cambiarFase('ARRIBA'); }
        else {
          c.actRms = a.nReps ? a.sRepRms / a.nReps : 0; c.actEnv = a.nReps ? a.sRepEnv / a.nReps : 0; this.hecho[2] = true;
          this._msg('   Activa lista: RMS de referencia ' + c.actRms.toFixed(1) + ' mV, envolvente ' + c.actEnv.toFixed(1) + ' mV (' + a.nReps + ' repeticiones).');
          this._finFase();
        }
      }
      return usado;
    }

    /* ---------- lo que hace el loop() del firmware: tiempo, tope, watchdog, estrellas ---------- */
    _loop() {
      const ahora = this._ms(), dt = ahora - this.tPrev; this.tPrev = ahora;
      if (this.modo === 'SESION') this.msSes += dt;
      const ok = this._datosOK();
      if (ok !== this.datosAntes) {
        this.datosAntes = ok;
        if (!ok) { this._terminarActivacion(this.tDato); this._msg('!! Sin datos del Arduino: estrellas lentas y luz ambar hasta que vuelvan.'); }
        else this._msg('>> Llegan datos del Arduino.');
      }
      if (this.modo === 'ARRANQUE') {
        this.pos = Math.min(0, this.pos + 4000 * dt / 1000);   // más rápido que el real (400 pasos/s) para no esperar
        if (this.pos >= 0) {
          this._msg('>> Tope encontrado. Las estrellas empiezan a subir y bajar.');
          this.dir = -1;
          if (this.calibrado) this._iniciarSesion(false); else this._iniciarCalibracion();
        }
        return;
      }
      if (this.modo === 'PAUSA' || this.modo === 'PARO') return;
      const v = this.modo === 'SESION' && ok && !this.activo ? 700 : 250;
      this.pos += this.dir * v * dt / 1000;
      if (this.pos >= 0) { this.pos = 0; this.dir = -1; }
      if (this.pos <= -17400) { this.pos = -17400; this.dir = 1; }
    }

    /* ---------- mismo JSON que /estado ---------- */
    estado(q) {
      const pick = (arr, n, from) => { const ini = Math.max(from, n - arr.length, 0); return arr.slice(arr.length - (n - Math.min(ini, n))); };
      const total = { REPOSO: 100, PASIVA: 100, ARRIBA: 40, ABAJO: 40 }[this.fase] || 0;
      const r1 = v => Math.round(v * 10) / 10;
      const c = this.c, a = this.apl;
      return Promise.resolve({
        v: '3.0-sim', boot: this.boot, modo: this.modo, fase: this.fase, hechos: this.hecho.map(Number), rep: this.rep, reps: 5,
        bloq: this.bloq, total, auto: 0, datos: this._datosOK() ? 1 : 0, rms: this.rms, suave: r1(this.suave),
        calibrado: this.calibrado ? 1 : 0, uon: r1(this.uon), uoff: r1(this.uoff), pct: Math.round(this.umbralPct * 100),
        activo: this.activo ? 1 : 0, nivel: Math.round(this.nivel * 100), durAct: this.activo ? r1((this._ms() - this.tIni) / 1000) : 0,
        sid: this.sid, nAct: this.nActiv, tAct: r1(this.tActMs / 1000), tSes: r1(this.msSes / 1000),
        alt: Math.round(100 * (1 - Math.abs(this.pos) / 17400)), estOK: 1, malas: 0, rssi: -40, cfg: this.cfg,
        cal: { reposo: r1(c.reposo), sd: r1(c.sd), restRms: r1(c.restRms), pasMedia: r1(c.pasMedia), pasPico: r1(c.pasPico),
          actRms: r1(c.actRms), actEnv: r1(c.actEnv), snr: Math.round(c.snr * 100) / 100, art: r1(c.art), repRms: c.repRms.slice() },
        apl: Object.fromEntries(Object.entries(a).map(([k, v]) => [k, r1(v)])),
        bn: this.nBlq, b: pick(this.blq, this.nBlq, q.b),
        an: this.nAct, acts: pick(this.acts, this.nAct, q.a),
        ln: this.nLog, logs: pick(this.logs, this.nLog, q.l)
      });
    }
  }

  EMG.AtrilHttp = AtrilHttp;
  EMG.AtrilLink = AtrilLink;
  EMG.AtrilSim = AtrilSim;
  EMG.FASES = FASES;
})();
