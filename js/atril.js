/* Conexión con el atril (ESP32, esp32_calibracion.ino v6).
 * El atril calibra (RMS basal automático de 10 s) y detecta; la página solo muestra su estado y le manda comandos:
 *   GET http://<ip del atril>/estado?b=&a=&l=   -> estado + bloques (RMS y mediana), activaciones y mensajes nuevos
 *   GET http://<ip del atril>/cmd?c=<letra>     -> c p r h + - x i o (los mismos del monitor serie)
 *   GET http://<ip del atril>/musica?l=abajo|arriba&p=1,2,3 -> las 3 pistas (carpeta /01) de una lista
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
    musica(lista, p) { return this._get('/musica?l=' + lista + '&p=' + p.join(','), 2500); }
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
    async musica(lista, p) {
      try { await this.be.musica(lista, p); return true; } catch (e) { return false; }
    }
    async volumen(v) {
      try { await this.be.volumen(v); return true; } catch (e) { return false; }
    }
    stop() { this.run = false; this.be.stop(); this._set('disconnected'); }
  }

  /* ===================================================================
   * Atril simulado: misma lógica que esp32_calibracion.ino v6.
   * Recibe un RMS (mV) cada 100 ms con feedBlock(), como el ESP32 por el opto.
   * =================================================================== */
  const C = {
    CAL_MS: 10000, DESC_MS: 1000, MIN_MUESTRAS: 20, CV_MAX: 0.5, K_INICIAL: 2, MARGEN: 2, OFF: 0.6,
    N_MED: 5, CONFIRMAR: 300, HOLGURA: 50, WATCHDOG: 500, V_LENTO: 600, V_RAPIDO: 1000, T_ACEL: 2000,
    N_CANCIONES: 3, ABAJO_MIN: 1000, ENTRE_CANCIONES: 5000
  };

  class AtrilSim {
    constructor() {
      this.kind = 'sim';
      this.boot = Math.floor(Math.random() * 1e9);
      this.blq = []; this.nBlq = 0; this.acts = []; this.nAct = 0; this.logs = []; this.nLog = 0;
      this.modo = 'ARRANQUE'; this.tArr = performance.now();
      this.calibrado = false; this.reposo = 100; this.sd = 10; this.k = C.K_INICIAL; this.uon = 300; this.uoff = 200;
      this.tCal = 0; this.nRep = 0; this.sR = 0; this.s2R = 0; this.calN = 0; this.pocos = false; this.inestable = false;
      this.ventana = []; this.rms = 0; this.med = 0; this.hayDatos = false; this.tDato = -1e9; this.datosAntes = false;
      this.activo = false; this.tSobre = 0; this.tBajo = 0; this.tIni = 0; this.nivel = 0; this.abajoAct = true;
      this.sumaAct = 0; this.nRmsAct = 0; this.sumaCola = 0; this.nCola = 0;
      this.nActiv = 0; this.tActMs = 0; this.sid = 0; this.msSes = 0; this.msSesIni = 0;
      this.tCambio = 0; this.abajoAntes = false; this.tIniAbajo = 0; this.tUltCancion = -1e9; this.cancion = 4; this.orden = 0;
      this.mus = { abajo: [1, 2, 3], arriba: [4, 5, 6], idx: { abajo: -1, arriba: -1 }, lista: 'arriba', sonando: false, aviso: false,
        tAviso: 0, tPista: 0, subio: false, reiniciar: false, tLista: performance.now() + 2300, tCmd: 0, vol: 15 };
      this.tPrev = performance.now();
      this._msg('=== ASTRA - atril v5.1 (simulado) ===');
      this._msg('>> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).');
      this.timer = setInterval(() => this._loop(), 50);
    }
    stop() { clearInterval(this.timer); }
    simCtx() { return this.modo === 'SESION' ? { phase: 'running' } : { phase: 'rest' }; }

    _msg(t) { this.logs.push(t); if (this.logs.length > 24) this.logs.shift(); this.nLog++; }
    _ms() { return performance.now(); }
    _datosOK() { return this.hayDatos && this._ms() - this.tDato < C.WATCHDOG; }
    _abajo() { return this.abajoAct ? this.activo : !this.activo; }
    _umbrales() { this.uon = this.reposo + Math.max(this.k * this.sd, C.MARGEN); this.uoff = this.reposo + C.OFF * (this.uon - this.reposo); }
    _imprimir() {
      this._msg('---- Calibracion ----');
      this._msg('Reposo: ' + Math.round(this.reposo) + ' mV (SD ' + this.sd.toFixed(1) + ')');
      this._msg('Umbral de activacion: ' + this.uon.toFixed(1) + ' mV (media + ' + this.k.toFixed(1) + ' SD)');
      this._msg('Umbral de relajacion: ' + Math.round(this.uoff) + ' mV');
      this._msg('Activaciones: ' + this.nActiv + ' | tiempo activo total: ' + (this.tActMs / 1000).toFixed(1) + ' s');
      this._msg('---------------------');
    }
    _terminarActivacion(fin) {
      if (this.activo) {
        this.activo = false; this.tCambio = this._ms();
        const dur = fin - this.tIni; this.tActMs += dur;
        const rmsMedio = this.nRmsAct ? this.sumaAct / this.nRmsAct : 0;
        this._msg('   Activacion #' + this.nActiv + ': ' + (dur / 1000).toFixed(1) + ' s, RMS medio ' + Math.round(rmsMedio) + ' mV');
        this.acts.push([this.sid, this.nActiv, +(this.msSesIni / 1000).toFixed(1), +(dur / 1000).toFixed(1), Math.round(rmsMedio)]);
        if (this.acts.length > 32) this.acts.shift();
        this.nAct++;
      }
      this.tSobre = this.tBajo = 0;
    }
    _cerrar() { this._terminarActivacion(this.tBajo || this._ms()); }
    _iniciarCalibracion() {
      this._cerrar();
      this.modo = 'CALIBRANDO'; this.tCal = this._ms(); this.nRep = 0; this.sR = this.s2R = 0;
      this._msg('>> CALIBRACION BASAL: cabeza apoyada y estable durante 10 s (luz azul).');
      this._msg('>> Se descarta el primer segundo; no hagas contracciones voluntarias.');
    }
    _iniciarSesion() {
      this.modo = 'SESION'; this.activo = false; this.tSobre = this.tBajo = 0; this.tCambio = this._ms(); this.abajoAntes = this._abajo();
      this._msg('>> SESION: cabeza abajo = estrellas rapidas, luz brillante y colores rapidos.');
    }
    _terminarCalibracion() {
      this.calN = this.nRep; this.pocos = this.nRep < C.MIN_MUESTRAS; this.inestable = false;
      this.sid++; this.msSes = 0;
      if (this.pocos) this._msg('!! Calibracion basal con solo ' + this.nRep + ' bloques: se conservan los umbrales anteriores. Repite con c o BOOT.');
      else {
        const media = this.sR / this.nRep, sd = Math.sqrt(Math.max(this.s2R / this.nRep - media * media, 0));
        this.reposo = media; this.sd = Math.max(sd, 0.5);
        this.inestable = media > 0 && sd / media > C.CV_MAX;
        if (this.inestable) this._msg('!! RMS basal inestable (variacion > 50 % de la media). Revisa electrodos;');
      }
      this.calibrado = true; this._umbrales(); this._imprimir(); this._iniciarSesion();
    }

    cmd(c) {
      switch (c) {
        case 'c': if (this.modo === 'SESION' || this.modo === 'PAUSA') this._iniciarCalibracion(); break;
        case 'p':
          if (this.modo === 'SESION') { this._cerrar(); this.modo = 'PAUSA'; this._msg('>> PAUSA (p para seguir)'); }
          else if (this.modo === 'PAUSA') this._iniciarSesion();
          break;
        case 'h': this._cerrar(); this.modo = 'ARRANQUE'; this.tArr = this._ms(); this._msg('>> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).'); break;
        case 'x':
          this.abajoAct = !this.abajoAct; this.tCambio = this._ms(); this.abajoAntes = this._abajo();
          this._msg('>> Ahora CABEZA ABAJO = RMS ' + (this.abajoAct ? 'SOBRE' : 'BAJO') + ' el umbral. Si queda bien, pon ABAJO_ES_ACTIVIDAD = ' + this.abajoAct + '.');
          break;
        case '+': if (this.calibrado) { this.k = Math.min(this.k + 0.5, 8); this._umbrales(); this._imprimir(); } break;
        case '-': if (this.calibrado) { this.k = Math.max(this.k - 0.5, 1); this._umbrales(); this._imprimir(); } break;
        case 'i': this._imprimir(); break;
        case 'o': this.orden = (this.orden + 1) % 6; this._msg('>> Lazy susan: combinacion ' + this.orden + '.'); break;
      }
      return Promise.resolve({ ok: true });
    }

    feedBlock(mv) {
      const ahora = this._ms();
      this.rms = mv;
      this.ventana.push(mv); if (this.ventana.length > C.N_MED) this.ventana.shift();
      const ord = this.ventana.slice().sort((a, b) => a - b);
      this.med = ord[Math.floor(ord.length / 2)];
      this.hayDatos = true; this.tDato = ahora;
      let flag = 0;
      if (this.modo === 'CALIBRANDO') {
        if (ahora - this.tCal >= C.DESC_MS && this.ventana.length === C.N_MED) {
          this.sR += this.med; this.s2R += this.med * this.med; this.nRep++; flag = 2;
        }
      } else if (this.modo === 'SESION') {
        this.nivel = Math.min(1, Math.max(0, (this.med - this.reposo) / Math.max(this.uon - this.reposo, 1)));
        if (!this.activo) {
          if (this.med >= this.uon) {
            if (!this.tSobre) { this.tSobre = ahora; this.sumaAct = 0; this.nRmsAct = 0; this.msSesIni = this.msSes; }
            this.sumaAct += mv; this.nRmsAct++;
            if (ahora - this.tSobre + C.HOLGURA >= C.CONFIRMAR) { this.activo = true; this.tIni = this.tSobre; this.tCambio = ahora; this.nActiv++; this.tBajo = 0; }
          } else this.tSobre = 0;
        } else if (this.med < this.uoff) {
          if (!this.tBajo) { this.tBajo = ahora; this.sumaCola = 0; this.nCola = 0; }
          this.sumaCola += mv; this.nCola++;
          if (ahora - this.tBajo + C.HOLGURA >= C.CONFIRMAR) this._terminarActivacion(this.tBajo);
        } else {
          if (this.tBajo) { this.sumaAct += this.sumaCola; this.nRmsAct += this.nCola; this.tBajo = 0; }
          this.sumaAct += mv; this.nRmsAct++;
        }
        flag = this.activo ? 1 : 0;
      }
      this.blq.push([mv, this.med, flag]); if (this.blq.length > 64) this.blq.shift(); this.nBlq++;
    }

    _loop() {
      const ahora = this._ms(), dt = ahora - this.tPrev; this.tPrev = ahora;
      if (this.modo === 'SESION') this.msSes += dt;
      const ok = this._datosOK();
      if (ok !== this.datosAntes) {
        this.datosAntes = ok;
        if (!ok) { this._terminarActivacion(this.tDato); this.ventana = []; this._msg('!! Sin datos del Arduino: estrellas y planetas quietos hasta que vuelvan.'); }
        else { this.tCambio = ahora; this._msg('>> Llegan datos del Arduino.'); }
      }
      if (this.modo === 'ARRANQUE' && ahora - this.tArr > 2000) {    // el real tarda lo que tarde en pisar el microswitch
        this._msg('>> Tope encontrado. Las estrellas empiezan a subir y bajar.');
        if (this.calibrado) this._iniciarSesion(); else this._iniciarCalibracion();
      }
      if (this.modo === 'CALIBRANDO' && ahora - this.tCal >= C.CAL_MS) this._terminarCalibracion();
      this._musica(ahora, ok);
    }
    // Igual que la musica del firmware v6: dos listas (cabeza abajo / arriba) y el aviso al subir la cabeza.
    // Una pista simulada dura 40 s; el aviso, 3 s.
    _tocar(abajo, avanzar) {
      const m = this.mus, l = abajo ? 'abajo' : 'arriba';
      if (avanzar || m.idx[l] < 0) m.idx[l] = (m.idx[l] + 1) % 3;
      this.cancion = m[l][m.idx[l]]; m.lista = l; m.sonando = true; m.aviso = false; m.tPista = this._ms(); m.tCmd = this._ms();
      this._msg('   >> Musica (cabeza ' + (abajo ? 'ABAJO' : 'arriba') + '): /01/' + String(this.cancion).padStart(3, '0') + '.mp3');
    }
    _musica(ahora, ok) {
      const m = this.mus;
      if (this.modo === 'SESION' && ok) {
        const abajo = this._abajo();
        if (!this.abajoAntes && abajo) { this.tIniAbajo = ahora; m.subio = false; }
        if (this.abajoAntes && !abajo && ahora - this.tIniAbajo >= C.ABAJO_MIN) m.subio = true;
        this.abajoAntes = abajo;
      }
      if (m.aviso && ahora - m.tAviso > 3000) { m.aviso = false; m.tPista = ahora; this._msg('   >> Fin del aviso: sigue la cancion'); }
      if (ahora - m.tCmd < 300 || ahora < m.tLista) return;
      const quiereAbajo = this.modo === 'SESION' && ok && this._abajo();
      if (this.modo === 'PARO') { m.sonando = m.aviso = m.subio = m.reiniciar = false; return; }
      if (!m.sonando) this._tocar(quiereAbajo, false);
      else if (this.modo === 'SESION' && ok && quiereAbajo !== (m.lista === 'abajo') && (ahora - this.tUltCancion >= C.ENTRE_CANCIONES || m.subio)) {
        this.tUltCancion = ahora; this._tocar(quiereAbajo, true);
      } else if (m.reiniciar && !m.aviso) { m.reiniciar = false; m.idx[m.lista] = -1; this._tocar(m.lista === 'abajo', false); }
      else if (!m.aviso && ahora - m.tPista > 40000) this._tocar(m.lista === 'abajo', true);
      else if (m.subio && ahora - m.tCmd >= 600) {
        m.subio = false; m.aviso = true; m.tAviso = ahora; m.tCmd = ahora;
        this._msg('   >> Cabeza arriba: aviso /ADVERT/0001.mp3');
      }
    }
    musica(lista, p) {
      if ((lista !== 'abajo' && lista !== 'arriba') || p.length !== 3 || p.some(v => !(v >= 1 && v <= 255))) return Promise.reject(new Error('HTTP 400'));
      this.mus[lista] = p.slice();
      this._msg('>> Lista CABEZA ' + lista.toUpperCase() + ': ' + p.map(v => String(v).padStart(3, '0')).join(', '));
      if (this.mus.sonando && this.mus.lista === lista) this.mus.reiniciar = true;
      return Promise.resolve({ ok: true });
    }
    volumen(v) { this.mus.vol = Math.max(0, Math.min(30, Math.round(v))); this._msg('>> Volumen: ' + this.mus.vol + ' de 30'); return Promise.resolve({ ok: true }); }
    _velE() {
      if (this.modo === 'SESION' && this._datosOK() && this._abajo()) {
        const k = Math.min((this._ms() - this.tCambio) / C.T_ACEL, 1);
        return 50 * Math.floor((C.V_LENTO + (C.V_RAPIDO - C.V_LENTO) * k) / 50);
      }
      return C.V_LENTO;
    }

    /* mismo JSON que /estado */
    estado(q) {
      const pick = (arr, n, from) => { const ini = Math.max(from, n - arr.length, 0); return arr.slice(arr.length - (n - Math.min(ini, n))); };
      const r1 = v => Math.round(v * 10) / 10, ahora = this._ms(), tc = ahora - this.tCal;
      return Promise.resolve({
        v: '6-sim', boot: this.boot, modo: this.modo, datos: this._datosOK() ? 1 : 0, rms: this.rms, med: r1(this.med),
        activo: this.activo ? 1 : 0, abajo: this._abajo() ? 1 : 0, abajoAct: this.abajoAct ? 1 : 0, nivel: Math.round(this.nivel * 100),
        durAct: this.activo ? r1((ahora - this.tIni) / 1000) : 0,
        cal: { ok: this.calibrado ? 1 : 0, restante: this.modo === 'CALIBRANDO' ? r1(Math.max(0, C.CAL_MS - tc) / 1000) : 0, total: C.CAL_MS / 1000, desc: C.DESC_MS / 1000,
          n: this.modo === 'CALIBRANDO' ? this.nRep : this.calN, nMin: C.MIN_MUESTRAS, reposo: r1(this.reposo), sd: Math.round(this.sd * 100) / 100,
          cv: this.reposo > 0 ? Math.round(1000 * this.sd / this.reposo) / 1000 : 0, cvMax: C.CV_MAX, k: this.k, margen: C.MARGEN, koff: C.OFF,
          uon: r1(this.uon), uoff: r1(this.uoff), pocos: this.pocos ? 1 : 0, inestable: this.inestable ? 1 : 0 },
        sid: this.sid, nAct: this.nActiv, tAct: r1(this.tActMs / 1000), tSes: r1(this.msSes / 1000), velE: this._velE(),
        cancion: this.cancion, orden: this.orden, malas: 0, rssi: -40,
        musica: { carpeta: 1, abajo: this.mus.abajo.slice(), arriba: this.mus.arriba.slice(), sonando: this.mus.sonando ? 1 : 0,
          lista: this.mus.lista, pista: this.cancion, aviso: this.mus.aviso ? 1 : 0, avisoPista: 1, vol: this.mus.vol },
        bn: this.nBlq, b: pick(this.blq, this.nBlq, q.b),
        an: this.nAct, acts: pick(this.acts, this.nAct, q.a),
        ln: this.nLog, logs: pick(this.logs, this.nLog, q.l)
      });
    }
  }

  /* ===================================================================
   * Servidor de la página (servidor/servidor.js): UN WebSocket /ws trae
   *   - binario: paquetes 0xAA 0x55 de la señal cruda del Arduino
   *   - {type:'atril', st}: estado del ESP32 (lo mismo que antes daba /estado)
   *   - {type:'servidor', arduino, atril}: quién está conectado
   * y lleva las órdenes: {type:'cmd', c} y {type:'musica', l, p}.
   * Se presenta a la app como dos fuentes: emg (señal) y atril (estado).
   * =================================================================== */
  class ServidorLink {
    constructor(host, h) {
      this.host = host; this.h = h; this.run = true; this.ws = null; this.boot = null; this.info = null;
      this.parser = new EMG.PacketParser(arr => h.onSamples(arr));
      const link = this;
      this.emg = { kind: 'servidor', get lost() { return link.parser.lost; }, stop() { link.stop(); } };
      this.atril = { kind: 'servidor', cmd: c => link._enviar({ type: 'cmd', c }), musica: (l, p) => link._enviar({ type: 'musica', l, p }),
        volumen: v => link._enviar({ type: 'volumen', v }), stop() { link.stop(); } };
      this._conectar();
    }
    _conectar() {
      if (!this.run) return;
      this.h.onStatus('connecting');
      const proto = location.protocol === 'https:' ? 'wss' : 'ws';
      let ws;
      try { ws = this.ws = new WebSocket(proto + '://' + this.host + '/ws?role=monitor'); }
      catch (e) { this.h.onStatus('error', 'dirección del servidor no válida'); return; }
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => { this.parser.reset(); this.h.onStatus('open'); };
      ws.onmessage = ev => {
        if (typeof ev.data !== 'string') { this.parser.push(new Uint8Array(ev.data)); return; }
        let m; try { m = JSON.parse(ev.data); } catch (e) { return; }
        if (m.type === 'servidor') { this.info = m; this.h.onServidor(m); }
        else if (m.type === 'atril' && m.st) {
          if (this.boot !== null && m.st.boot !== this.boot) m.st.reinicio = true;
          this.boot = m.st.boot;
          try { this.h.onEstado(m.st); } catch (e) { console.error('Error al mostrar el estado del atril:', e); }
        }
      };
      ws.onclose = () => {
        if (!this.run) return;
        this.h.onStatus('error', 'sin conexión con el servidor de la página (' + this.host + ')');
        setTimeout(() => this._conectar(), 1500);
      };
    }
    async _enviar(obj) {
      if (!this.ws || this.ws.readyState !== 1) return false;
      this.ws.send(JSON.stringify(obj)); return true;
    }
    stop() { if (!this.run) return; this.run = false; try { this.ws && this.ws.close(); } catch (e) { /* ya cerrado */ } this.h.onStatus('disconnected'); }
  }

  EMG.ServidorLink = ServidorLink;
  EMG.AtrilHttp = AtrilHttp;
  EMG.AtrilLink = AtrilLink;
  EMG.AtrilSim = AtrilSim;
})();
