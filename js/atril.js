/* Conexión con el atril (ESP32, esp32_calibracion.ino v4).
 * El ESP32 calibra, detecta estados sostenidos, lleva los contadores de verificación y guarda los umbrales.
 * La página muestra su estado, pregunta a la fisioterapeuta y le manda lo que ella decide:
 *   GET /estado?b=&a=&l=&r=  estado + RMS + eventos de verificación + mensajes (solo lo nuevo)
 *   GET /cmd?c=<letra>       c 1 x f s n p r h a i
 *   GET /conf?id=&p=E|C|D    postura confirmada para el evento pendiente (idempotente)
 *   GET /manual?p=&n=        confirmación manual (n = identificador: un reintento no duplica)
 *   GET /umbral?on=&off=&n=  umbral aprobado por la fisioterapeuta
 *   GET /restaurar?n=        vuelve al umbral anterior
 *   GET /cfg?k=&v=           parámetros del atril
 * AtrilSim reproduce la misma lógica del firmware (con reloj virtual) para probar sin hardware.
 */
(function (root) {
  'use strict';
  const EMG = (root.EMG = root.EMG || {});
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  /* Campos de un evento (el ESP32 los manda como arreglo para ahorrar bytes) */
  const EV_CAMPOS = ['id', 'rev', 'ep', 'origen', 'estado', 'status', 'postura', 'sid', 'periodo', 'nSol', 'calidad', 'tok',
    'tIni', 'dur', 'media', 'sd', 'minimo', 'maximo', 'uon', 'uoff'];
  const decodeEv = a => { const e = {}; EV_CAMPOS.forEach((k, i) => { e[k] = a[i]; }); return e; };

  class AtrilHttp {
    constructor(ip) { this.base = 'http://' + ip; this.kind = 'wifi'; }
    async _get(path, ms) {
      const url = this.base + path;
      const ctrl = new AbortController(), to = setTimeout(() => ctrl.abort(), ms || 2500);
      try {
        const r = await fetch(url, EMG.fetchOpts(url, { signal: ctrl.signal }));
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return await r.json();
      } catch (e) {
        throw new Error(e.name === 'AbortError' ? 'no responde' : (e.message || e.name));
      } finally { clearTimeout(to); }
    }
    estado(q) { return this._get('/estado?b=' + q.b + '&a=' + q.a + '&l=' + q.l + '&r=' + q.r); }
    cmd(c) { return this._get('/cmd?c=' + encodeURIComponent(c)); }
    conf(id, p) { return this._get('/conf?id=' + id + '&p=' + p); }
    manual(p, n) { return this._get('/manual?p=' + p + '&n=' + n); }
    umbral(on, off, n) { return this._get('/umbral?on=' + on + '&off=' + off + '&n=' + n); }
    restaurar(n) { return this._get('/restaurar?n=' + n); }
    setCfg(k, v) { return this._get('/cfg?k=' + encodeURIComponent(k) + '&v=' + v); }
    stop() { }
  }

  /* Pide el estado ~6 veces por segundo y reparte lo nuevo. Si la conexión se corta, reintenta sola y, al volver,
   * vuelve a pedir todo desde cero (los eventos ya conocidos se reconocen por su identificador). */
  class AtrilLink {
    constructor(backend, onEstado, onStatus) {
      this.be = backend; this.onEstado = onEstado; this.onStatus = onStatus;
      this.q = { b: 0, a: 0, l: 0, r: 0 }; this.boot = null; this.run = true; this.status = ''; this.pausado = false;
      this._set('connecting');
      this._loop();
    }
    get kind() { return this.be.kind; }
    _set(s, msg) { if (s !== this.status || msg) { this.status = s; this.onStatus(s, msg || ''); } }
    async _loop() {
      let fails = 0;
      while (this.run) {
        const t0 = performance.now();
        if (!this.pausado) {
          try {
            const st = await this.be.estado(this.q);
            if (!this.run) break;
            if (this.pausado) continue;                        // la prueba de «desconexión» cortó la conexión a mitad
            st.ev = (st.ev || []).map(decodeEv);
            if (this.boot !== null && st.boot !== this.boot) {   // el ESP32 se reinició: se pide todo otra vez
              st.reinicio = true; this.q = { b: 0, a: 0, l: 0, r: 0 };
            } else this.q = { b: st.bn, a: st.an, l: st.ln, r: st.er };
            this.boot = st.boot;
            fails = 0;
            this._set('connected');
            try { this.onEstado(st); } catch (e) { console.error('Error al mostrar el estado del atril:', e); }
          } catch (e) {
            if (!this.run) break;
            if (++fails >= 2) this._set('error', e.message);
          }
        }
        const wait = (this.status === 'connected' ? 160 : 1500) - (performance.now() - t0);
        await sleep(Math.max(20, wait));
      }
    }
    /** Corta la conexión sin destruirla (prueba de «desconexión web»). Al reanudar se resincroniza todo. */
    pausar(p) {
      this.pausado = p;
      if (p) this._set('error', 'conexión interrumpida');
      else { this.q = { b: 0, a: 0, l: 0, r: 0 }; this._set('connecting'); }
    }
    async _ok(fn) { try { const r = await fn(); return !!(r && r.ok); } catch (e) { return false; } }
    cmd(c) { return this._ok(() => this.be.cmd(c)); }
    conf(id, p) { return this._ok(() => this.be.conf(id, p)); }
    manual(p, n) { return this._ok(() => this.be.manual(p, n)); }
    umbral(on, off, n) { return this._ok(() => this.be.umbral(on, off, n)); }
    restaurar(n) { return this._ok(() => this.be.restaurar(n)); }
    setCfg(k, v) { return this._ok(() => this.be.setCfg(k, v)); }
    stop() { this.run = false; this.be.stop(); this._set('disconnected'); }
  }

  /* ===================================================================
   * Atril simulado: misma máquina de estados que esp32_calibracion.ino v4.
   *  - Reloj virtual: avanzar(ms) hace pasar el tiempo (en la página, 50 ms reales × velocidad).
   *  - Genera él mismo los bloques de RMS (cada 100 ms virtuales) según un escenario.
   *  - Escenario: la postura REAL del niño y cómo se relaciona con el RMS ('normal': erguido = RMS alto,
   *    'invertida', 'ninguna'), para probar que el análisis no asume la relación.
   * =================================================================== */
  const NOM_MODO = ['ARRANQUE', 'CALIBRANDO', 'SESION', 'PAUSA', 'PARO'];
  const F_CORTE = 1 << 0, F_NULA = 1 << 1, F_RUIDO = 1 << 2, F_SATURA = 1 << 3, F_PLANA = 1 << 4, F_CV_EXC = 1 << 5, F_CV = 1 << 6,
    F_DER_EXC = 1 << 7, F_DER = 1 << 8, F_PICOS = 1 << 9, F_POCOS = 1 << 10;
  const F_RECHAZO = F_CORTE | F_NULA | F_RUIDO | F_SATURA | F_PLANA | F_CV_EXC | F_DER_EXC | F_POCOS;
  const Q = { VENT_DERIVA: 0.35, MEDIA_MIN: 2, MEDIA_MAX: 1500, SAT: 2400, CV_AVISO: 0.30, CV_RECHAZO: 0.60, DER_AVISO: 0.25, DER_RECHAZO: 0.50, PICO_SD: 6 };

  class AtrilSim {
    constructor(opts) {
      opts = opts || {};
      this.kind = 'sim';
      this.speed = opts.speed || 1;
      this.modoManual = !!opts.manual;                // true: sin temporizador (lo avanzan las pruebas)
      this._seed = opts.seed || 12345;
      this.boot = 1 + Math.floor(this._rnd() * 1e9);
      this.t = 0;                                     // millis() virtual
      this.cfg = { basal: 10, desc: 1, k: 3, f: 0.6, tSost: 3, v1s: 300, v1max: 4, v1min: 45, vcs: 600, vcmax: 3, vcmin: 120, rever: 120, resp: 120, hip: 1, bps: 10 };
      // escenario simulado (no existe en el firmware)
      this.esc = { erguido: false, relacion: 'normal', baseMean: 30, baseSd: 3, bajoMean: 30, bajoSd: 3, altoMean: 150, altoSd: 12, auto: true, resto: 0,
        prolongado: false, altoIndep: false, basal: 'normal', sinDatos: false };
      this.esc.resto = this._durPostura();
      this.logs = []; this.nLog = 0; this.blq = []; this.nBlq = 0; this.acts = []; this.nAct = 0;
      this.evs = new Array(48); this.nEv = 0; this.revGlobal = 0;
      this.modo = 'ARRANQUE'; this.fase = 'ESPERA'; this.bloquesFase = 0;
      this._reiniciarBasal();
      this.cb = { media: 0, sd: 0, cv: 0, deriva: 0, minimo: 0, maximo: 0, n: 0, flags: 0, nivel: 0 };
      this.calibrado = false; this.reposo = 100; this.sdReposo = 10; this.umbralOn = 300; this.umbralOff = 200; this.umbralOrigen = 0;
      this.hist = []; this.controlAuto = !!opts.controlAuto;
      this.rms = 0; this.suave = 0; this.hayDatos = false; this.tDato = -1e9; this.datosAntes = false;
      this.activo = false; this.tSobre = 0; this.tBajo = 0; this.tIniAct = 0; this.nivel = 0;
      this.sumaAct = 0; this.nRmsAct = 0; this.sumaCola = 0; this.nCola = 0;
      this.sid = 0; this.nActiv = 0; this.tActMs = 0; this.msSesion = 0; this.msSesionIni = 0; this.tPrev = 0;
      this._reiniciarEstadoRms(); this.episodio = 0; this.nDetectados = 0;
      this.perActual = -1; this.solPer = 0; this.tSolUlt = -1e9; this.finVentUlt = -1e9; this.tEpUlt = -1e9; this.epUltSol = 0; this.pendId = 0;
      this.nResp = [0, 0, 0]; this.nErr = [0, 0, 0]; this.tokManual = 0; this.tokUmbral = 0; this.tokRestaurar = 0;
      this.tUltimaConsulta = 0;
      this.pos = -9000; this.dir = 1;
      this.acum = 0; this.bloqueMs = 0;
      this._msg('=== ASTRA - atril v4.0 (simulado) ===');
      this._msg('>> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).');
      if (!this.modoManual) { this._last = performance.now(); this.timer = setInterval(() => { const n = performance.now(); this.avanzar((n - this._last) * this.speed); this._last = n; }, 50); }
    }
    stop() { if (this.timer) clearInterval(this.timer); }
    _rnd() { let t = (this._seed += 0x6D2B79F5); t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }
    _gauss() { return Math.sqrt(-2 * Math.log(Math.max(this._rnd(), 1e-9))) * Math.cos(2 * Math.PI * this._rnd()); }
    _msg(t) { this.logs.push(t); if (this.logs.length > 24) this.logs.shift(); this.nLog++; }

    /* ---------- escenario ---------- */
    setPostura(erguido, mantener) { this.esc.erguido = !!erguido; this.esc.prolongado = !!mantener; this.esc.resto = mantener ? 1e12 : this._durPostura(); }
    _durPostura() { return 10000 + 35000 * this._rnd(); }
    posturaReal() { return this.esc.erguido; }
    /** Postura real del niño simulado en el instante del evento (para la fisioterapeuta simulada) */
    verdadDe(id) { const e = this._buscarEv(id); return e ? !!e.verdad : this.esc.erguido; }
    /** Nivel medio de RMS que corresponde ahora (en reposo o sesión). */
    nivelObjetivo() {
      const e = this.esc;
      if (this.modo === 'SESION') {
        const alto = e.relacion === 'normal' ? e.erguido : e.relacion === 'invertida' ? !e.erguido : e.altoIndep;
        return { mean: alto ? e.altoMean : e.bajoMean, sd: alto ? e.altoSd : e.bajoSd };   // bajo = tono de la sesión (puede diferir del basal)
      }
      return { mean: e.baseMean, sd: e.baseSd };
    }
    /** Para el generador de señal cruda de la página */
    simCtx() { return { phase: 'running', rmsMv: this.nivelObjetivo().mean }; }

    /* ---------- tiempo ---------- */
    avanzar(ms) {
      this.acum += ms;
      while (this.acum >= 100) {
        this.acum -= 100; this.t += 100;
        if (this.modo !== 'ARRANQUE' && this.modo !== 'PARO') {
          const e = this.esc;
          if (e.auto && !e.prolongado) { e.resto -= 100; if (e.resto <= 0) { e.erguido = !e.erguido; e.altoIndep = this._rnd() < 0.5; e.resto = this._durPostura(); } }
          else if (e.auto && e.prolongado) { /* sigue como está */ }
          if (!e.sinDatos) this.feedBlock(this._valorBloque());
        }
        this._loop(100);
      }
    }
    _valorBloque() {
      const e = this.esc, n = this.nivelObjetivo();
      if (this.modo === 'CALIBRANDO' && this.fase === 'BASAL') {
        const i = this.bloquesFase;
        if (i < 10) return 900;                                                    // transitorio que se descarta
        if (e.basal === 'plano') return 40;
        if (e.basal === 'nulo') return 0;
        if (e.basal === 'ruidoso') return Math.max(0, e.baseMean + 25 * this._gauss());
        if (e.basal === 'deriva') return Math.max(0, e.baseMean * (1 + 1.2 * (i - 10) / 90) + e.baseSd * this._gauss());       // deriva moderada (advertencia)
        if (e.basal === 'deriva2') return Math.max(0, e.baseMean * (1 + 4 * (i - 10) / 90) + e.baseSd * this._gauss());       // deriva fuerte (rechazo)
        if (e.basal === 'saturado' && i === 50) return 2450;
      }
      return Math.max(0, Math.round(n.mean + n.sd * this._gauss()));
    }
    _datosOK() { return this.hayDatos && (this.t - this.tDato) < 500; }
    _midiendo() { return this.fase === 'BASAL'; }
    _tEf() { return this.msSesion / 1000; }
    // La página está "viva" si consultó /estado hace menos de 3 s (en la simulación acelerada, 3 s REALES)
    _paginaViva() { return this.tUltimaConsulta && (this.modoManual ? this.t - this.tUltimaConsulta : performance.now() - this.tUltimaConsulta) < 3000; }

    _loop(dt) {                                      // lo que hace actualizarModo() + moverEstrellas()
      if (this.modo === 'SESION') this.msSesion += dt;
      const ok = this._datosOK();
      if (ok !== this.datosAntes) {
        this.datosAntes = ok;
        if (!ok) {
          this._cerrarActivacion(this.tDato); this._reiniciarEstadoRms();
          if (this.modo === 'CALIBRANDO' && this.fase === 'BASAL') this.bCorte = true;
          this._msg('!! Sin datos del Arduino: estrellas lentas y luz ambar hasta que vuelvan.');
        } else this._msg('>> Llegan datos del Arduino.');
      }
      this._planificar();
      if (this.modo === 'ARRANQUE') {
        this.pos = Math.min(0, this.pos + 4000 * dt / 1000);   // más rápido que el real (400 pasos/s)
        if (this.pos >= 0) {
          this._msg('>> Tope encontrado. Las estrellas empiezan a subir y bajar.'); this.dir = -1;
          if (this.calibrado) this._iniciarSesion(false); else this._iniciarCalibracion();
        }
        return;
      }
      if (this.modo === 'PAUSA' || this.modo === 'PARO') return;
      const v = this.modo === 'SESION' && ok && this._premiado() ? 700 : 250;
      this.pos += this.dir * v * dt / 1000;
      if (this.pos >= 0) { this.pos = 0; this.dir = -1; }
      if (this.pos <= -17400) { this.pos = -17400; this.dir = 1; }
    }
    _premiado() { return this.controlAuto && !this.activo; }

    /* ---------- calibración basal ---------- */
    _reiniciarBasal() { this.b = { sum: 0, sum2: 0, sumA: 0, sumB: 0, n: 0, na: 0, nb: 0, min: 1e9, max: 0 }; this.bCorte = false; }
    _iniciarCalibracion() {
      this._cerrarActivacion(this.t); this._expirarPendiente();
      this.modo = 'CALIBRANDO'; this.fase = 'ESPERA'; this.bloquesFase = 0; this._reiniciarBasal();
      this._msg('>> CALIBRACION BASAL (' + this.cfg.basal + ' s): solo RMS en reposo; no se piden movimientos ni contracciones.');
      this._msg('>> Esperando: pon al nino comodo y estable, con la cabeza apoyada, y registra el basal desde la pagina, con BOOT o con la tecla 1.');
    }
    _empezarBasal() {
      if (this.modo !== 'CALIBRANDO') { this._msg('!! Primero comienza la calibracion (c).'); return; }
      if (this._midiendo()) { this._msg('!! Ya se esta registrando el basal. Espera a que termine o cancelalo (x).'); return; }
      this._reiniciarBasal(); this.fase = 'BASAL'; this.bloquesFase = 0;
      this._msg('>> REGISTRO BASAL (' + this.cfg.basal + ' s): cabeza apoyada y relajada. No mover al nino. Se descarta el primer ' + this.cfg.desc + ' s.');
    }
    _cancelarBasal() {
      if (this.modo !== 'CALIBRANDO' || !this._midiendo()) return;
      this._reiniciarBasal(); this.fase = 'ESPERA'; this.bloquesFase = 0;
      this._msg('>> Registro basal cancelado. Puedes volver a empezarlo. La calibracion anterior (si habia) sigue vigente.');
    }
    _calibrarBloque(mv) {
      if (this.fase !== 'BASAL') return false;
      const b = this.b, bps = this.cfg.bps; let usado = false;
      this.bloquesFase++;
      const total = Math.round(this.cfg.basal * bps), desc = Math.round(this.cfg.desc * bps);
      if (this.bloquesFase > desc) {
        const esperados = Math.max(total - desc, 1), mitad = Math.floor(esperados / 2);
        b.n++; b.sum += mv; b.sum2 += mv * mv;
        if (b.n <= mitad) { b.sumA += mv; b.na++; } else if (b.n > esperados - mitad) { b.sumB += mv; b.nb++; }
        if (mv < b.min) b.min = mv;
        if (mv > b.max) b.max = mv;
        usado = true;
      }
      if (this.bloquesFase >= total) this._terminarBasal();
      return usado;
    }
    _terminarBasal() {
      const b = this.b, c = this.cb, bps = this.cfg.bps;
      const total = Math.round(this.cfg.basal * bps), desc = Math.round(this.cfg.desc * bps), esperados = Math.max(total - desc, 1);
      c.n = b.n; c.media = b.n ? b.sum / b.n : 0;
      const varz = b.n > 1 ? (b.sum2 - b.n * c.media * c.media) / (b.n - 1) : 0;
      c.sd = Math.sqrt(Math.max(varz, 0)); c.cv = c.media > 0 ? c.sd / c.media : 0;
      const mA = b.na ? b.sumA / b.na : c.media, mB = b.nb ? b.sumB / b.nb : c.media;
      c.deriva = c.media > 0 ? (mB - mA) / c.media : 0; c.minimo = b.n ? b.min : 0; c.maximo = b.max;
      let f = 0;
      if (this.bCorte) f |= F_CORTE;
      if (b.n < 0.8 * esperados) f |= F_POCOS;
      if (c.media < Q.MEDIA_MIN) f |= F_NULA;
      if (c.media > Q.MEDIA_MAX) f |= F_RUIDO;
      if (c.maximo >= Q.SAT) f |= F_SATURA;
      if (b.n > 1 && c.sd < 0.01) f |= F_PLANA;
      if (c.cv > Q.CV_RECHAZO) f |= F_CV_EXC; else if (c.cv > Q.CV_AVISO) f |= F_CV;
      if (Math.abs(c.deriva) > Q.DER_RECHAZO) f |= F_DER_EXC; else if (Math.abs(c.deriva) > Q.DER_AVISO) f |= F_DER;
      if (c.sd > 0 && c.maximo > c.media + Q.PICO_SD * c.sd) f |= F_PICOS;
      c.flags = f; this.bloquesFase = 0;
      if (f & F_RECHAZO) {
        c.nivel = 2; this.fase = 'RECHAZADA';
        this._msg('!! Calibracion RECHAZADA: la senal basal no es suficientemente estable. Revisa electrodos, postura y cables y repite (1).');
        this._msg('!! Si la fisioterapeuta decide usarla igual: tecla f (queda marcada como aceptada a la fuerza).');
        return;
      }
      this._aplicarBasal(f ? 1 : 0);
    }
    _umbralesBasal() { const on = this.reposo + this.cfg.k * this.sdReposo; return [on, this.reposo + this.cfg.f * (on - this.reposo)]; }
    _fijarUmbral(on, off, origen) {
      this.umbralOn = on; this.umbralOff = off; this.umbralOrigen = origen;
      if (this.hist.length === 6) this.hist.shift();
      this.hist.push([on, off, origen, this._tEf()]);
    }
    _aplicarBasal(nivel) {
      this.cb.nivel = nivel; this.reposo = this.cb.media; this.sdReposo = this.cb.sd;
      const [on, off] = this._umbralesBasal(); this.hist = []; this._fijarUmbral(on, off, 0);
      this.calibrado = true; this.fase = 'LISTA';
      this._msg('---- Calibracion basal ----');
      this._msg('RMS basal: media ' + this.cb.media.toFixed(1) + ' mV, SD ' + this.cb.sd.toFixed(1) + ' mV (CV ' + this.cb.cv.toFixed(2) + ', ' + this.cb.n + ' bloques)');
      this._msg('U_ON ' + on.toFixed(1) + ' mV | U_OFF ' + off.toFixed(1) + ' mV. Umbral basal (preliminar).');
      this._msg('>> Calibracion LISTA. Inicia la sesion desde la pagina, con BOOT o con la tecla s. El umbral es preliminar.');
    }
    _iniciarSesion(nueva) {
      if (nueva) { this.sid++; this.nActiv = 0; this.tActMs = 0; this.msSesion = 0; this._reiniciarVerificacion(); }
      this.modo = 'SESION'; this.activo = false; this.tSobre = this.tBajo = 0; this.suave = this.reposo; this._reiniciarEstadoRms();
      if (nueva) this._msg(this.controlAuto ? '>> SESION: control automatico de los actuadores.' : '>> SESION SUPERVISADA: el umbral aun no controla los actuadores (estrellas lentas, luz azul tenue, sin sonido).');
      else this._msg('>> SESION: continua.');
    }

    /* ---------- comandos ---------- */
    cmd(c) {
      switch (c) {
        case 'c': if (this.modo === 'ARRANQUE') this._msg('!! Espera a que las estrellas encuentren el tope.'); else this._iniciarCalibracion(); break;
        case '1': this._empezarBasal(); break;
        case 'x': this._cancelarBasal(); break;
        case 'f': if (this.modo === 'CALIBRANDO' && this.fase === 'RECHAZADA') { this._msg('!! Calibracion aceptada A LA FUERZA por decision de la fisioterapeuta.'); this._aplicarBasal(3); } break;
        case 's': if (this.modo === 'CALIBRANDO' && this.fase === 'LISTA') this._iniciarSesion(true); else this._msg('!! La sesion empieza cuando la calibracion este LISTA.'); break;
        case 'n': this._siguiente(); break;
        case 'p':
          if (this.modo === 'SESION') { this._cerrarActivacion(this.t); this.modo = 'PAUSA'; this._msg('>> PAUSA (p para seguir). El tiempo de terapia se detiene.'); }
          else if (this.modo === 'PAUSA') this._iniciarSesion(false);
          break;
        case 'h': this._cerrarActivacion(this.t); this.modo = 'ARRANQUE'; this._msg('>> Buscando el tope de las estrellas.'); break;
        case 'a': this.controlAuto = !this.controlAuto;
          this._msg(this.controlAuto ? '>> CONTROL AUTOMATICO habilitado: el umbral ahora controla los actuadores.' : '>> Modo SUPERVISADO: el umbral ya no controla los actuadores.'); break;
        case 'i': break;
      }
      return Promise.resolve({ ok: true });
    }
    _siguiente() {
      if (this.modo === 'SESION' || this.modo === 'PAUSA') { this._iniciarCalibracion(); return; }
      if (this.modo !== 'CALIBRANDO' || this._midiendo()) return;
      if (this.fase === 'LISTA') { this._iniciarSesion(true); return; }
      this._empezarBasal();
    }
    cfgSet(k, v) {
      const lim = { basal: [5, 60], desc: [0, 5], k: [0.5, 10], f: [0.1, 0.95], tSost: [1, 6], v1s: [30, 3600], v1max: [0, 20], v1min: [5, 3600], vcs: [60, 7200],
        vcmax: [0, 20], vcmin: [5, 7200], rever: [10, 3600], resp: [10, 600], hip: [0, 1] }[k];
      if (!lim || !(v >= lim[0] && v <= lim[1])) return false;
      this.cfg[k] = (k === 'v1max' || k === 'vcmax' || k === 'hip') ? Math.trunc(v) : v;
      this._msg('>> Parametro ' + k + ' = ' + v);
      if ((k === 'k' || k === 'f') && this.calibrado && this.umbralOrigen === 0) { const [on, off] = this._umbralesBasal(); this._fijarUmbral(on, off, 0); this._msg('>> U_ON ' + on.toFixed(1) + ' mV, U_OFF ' + off.toFixed(1) + ' mV (factores nuevos).'); }
      return true;
    }
    setCfg(k, v) { return Promise.resolve({ ok: this.cfgSet(k, +v) }); }

    /* ---------- eventos ---------- */
    _buscarEv(id) { return (id > 0 && id <= this.nEv && id + 48 > this.nEv) ? this.evs[(id - 1) % 48] : null; }
    _guardarEv(e) { e.rev = ++this.revGlobal; this.evs[(e.id - 1) % 48] = e; if (e.id > this.nEv) this.nEv = e.id; }
    _vAgregar(mv) { this.vent.push(mv); if (this.vent.length > 64) this.vent.shift(); }
    _nVent() { return Math.max(10, Math.min(64, Math.round(this.cfg.tSost * this.cfg.bps))); }
    _stats(n) {
      if (this.vent.length < n) return null;
      const w = this.vent.slice(-n); let s = 0, s2 = 0, mn = 1e9, mx = 0, cal = 0;
      for (const x of w) { s += x; s2 += x * x; if (x < mn) mn = x; if (x > mx) mx = x; if (x >= Q.SAT) cal = 1; }
      const m = s / n, v = n > 1 ? (s2 - n * m * m) / (n - 1) : 0, mitad = Math.floor(n / 2);
      const mV = w.slice(0, mitad).reduce((a, x) => a + x, 0) / Math.max(mitad, 1), mN = w.slice(n - mitad).reduce((a, x) => a + x, 0) / Math.max(mitad, 1);
      return { media: m, sd: Math.sqrt(Math.max(v, 0)), minimo: mn, maximo: mx, deriva: mitad > 0 && m > 0 ? Math.abs(mN - mV) / m : 0, calidad: cal };
    }
    _periodo(t) {
      const c = this.cfg;
      if (t < c.v1s) return { idx: 0, ini: 0, dur: c.v1s, minEntre: c.v1min, max: c.v1max };
      const n = Math.floor((t - c.v1s) / c.vcs);
      return { idx: 1 + n, ini: c.v1s + n * c.vcs, dur: c.vcs, minEntre: c.vcmin, max: c.vcmax };
    }
    _reiniciarEstadoRms() { this.estadoRms = 0; this.bloquesEnEstado = 0; this.sostenidoMarcado = false; this.vent = []; }
    _expirarPendiente() {
      if (!this.pendId) return;
      const p = this._buscarEv(this.pendId);
      if (p && p.status === 1) this._guardarEv(Object.assign({}, p, { status: 4 }));
      this.pendId = 0;
    }
    _reiniciarVerificacion() {
      this._expirarPendiente(); this.perActual = -1; this.solPer = 0; this.tSolUlt = this.finVentUlt = this.tEpUlt = -1e9; this.epUltSol = 0;
      this.nResp = [0, 0, 0]; this.nErr = [0, 0, 0]; this.nDetectados = 0;
    }
    _sostenidoVigente() {
      const nv = this._nVent();
      if (!this.estadoRms || this.bloquesEnEstado < nv) return null;
      const v = this._stats(nv);
      if (!v || v.deriva > Q.VENT_DERIVA) return null;      // ventana que cruza una transición: todavía no es estable
      return (this.estadoRms === 1 ? v.media >= this.umbralOn : v.media < this.umbralOff) ? v : null;
    }
    _actualizarEstadoRms() {
      let nuevo = this.estadoRms;
      if (!this.estadoRms) nuevo = this.suave >= this.umbralOn ? 1 : 2;
      else if (this.estadoRms === 2 && this.suave >= this.umbralOn) nuevo = 1;
      else if (this.estadoRms === 1 && this.suave < this.umbralOff) nuevo = 2;
      if (nuevo !== this.estadoRms) { this.estadoRms = nuevo; this.episodio++; this.bloquesEnEstado = 0; this.sostenidoMarcado = false; }
      this.bloquesEnEstado++;
      const v = this._sostenidoVigente();
      if (!this.sostenidoMarcado && v) {
        this.sostenidoMarcado = true; this.nDetectados++;
        this._msg('   Estado sostenido ' + (this.estadoRms === 1 ? 'ALTO' : 'BAJO') + ' (RMS medio ' + Math.round(v.media) + ' mV en ' + this.cfg.tSost + ' s). Candidato a confirmacion.');
      }
    }
    _armarEv(v, origen, p) {
      const nv = this._nVent(), t = this._tEf();
      return { verdad: this.esc.erguido, id: this.nEv + 1, rev: 0, ep: this.episodio, tok: 0, origen, estado: this.estadoRms, status: 0, postura: 0, calidad: v.calidad, periodo: p.idx, nSol: 0,
        sid: this.sid, tIni: t - nv / this.cfg.bps, dur: nv / this.cfg.bps, media: v.media, sd: v.sd, minimo: v.minimo, maximo: v.maximo, uon: this.umbralOn, uoff: this.umbralOff, tSolic: t };
    }
    _contar(e) {
      if (e.postura !== 1 && e.postura !== 2) return;
      this.nResp[e.estado]++;
      const esperadaErguida = (e.estado === 1) === (this.cfg.hip === 1);
      if (esperadaErguida !== (e.postura === 1)) this.nErr[e.estado]++;
    }
    _planificar() {
      if (this.modo !== 'SESION') return;
      const t = this._tEf(), p = this._periodo(t);
      if (p.idx !== this.perActual) {
        this.perActual = p.idx; this.solPer = 0;
        if (p.idx === 0) this._msg('>> Verificacion: primeros ' + (p.dur / 60) + ' min, maximo ' + p.max + ' confirmaciones automaticas.');
        else this._msg('>> Verificacion: ciclo ' + p.idx + ' (min ' + (p.ini / 60) + '-' + ((p.ini + p.dur) / 60) + '), maximo ' + p.max + ' confirmaciones automaticas.');
      }
      if (this.pendId) {
        const pe = this._buscarEv(this.pendId);
        if (!pe || pe.status !== 1) this.pendId = 0;
        else if (t - pe.tSolic > this.cfg.resp) { this._guardarEv(Object.assign({}, pe, { status: 4 })); this.pendId = 0; this._msg('   Pregunta #' + pe.id + ' sin respuesta: expirada.'); }
      }
      if (this.pendId || !this._datosOK() || !this._paginaViva()) return;
      if (this.solPer >= p.max) return;
      if (t < this.tSolUlt + p.minEntre) return;
      if (t < p.ini + this.solPer * p.dur / (p.max + 1)) return;
      const v = this._sostenidoVigente();
      if (!v) return;
      if (t - this._nVent() / this.cfg.bps < this.finVentUlt) return;
      let pts;
      if (this.episodio !== this.epUltSol) pts = 3;
      else { const desde = t - this.tEpUlt; if (desde < this.cfg.rever) return; pts = Math.floor(desde / this.cfg.rever); }
      const otro = this.estadoRms === 1 ? 2 : 1;
      if (this.nResp[this.estadoRms] < this.nResp[otro]) pts += 2;
      if (this.nErr[this.estadoRms] > 0) pts += 2;
      if (pts < 2) return;
      const e = this._armarEv(v, 0, p); e.status = 1; e.nSol = this.solPer + 1;
      this._guardarEv(e);
      this.pendId = e.id; this.solPer++; this.tSolUlt = t; this.finVentUlt = t; this.epUltSol = this.episodio; this.tEpUlt = t;
      this._msg('>> Verificacion ' + this.solPer + '/' + p.max + ' (periodo ' + p.idx + '): pregunta #' + e.id + ', ventana de ' + this.cfg.tSost + ' s.');
    }
    conf(id, p) {
      const post = { E: 1, C: 2, D: 3 }[p];
      const pe = this._buscarEv(id);
      if (post && pe && pe.status === 1) {
        const e = Object.assign({}, pe, { postura: post, status: post === 3 ? 3 : 2 });
        this._guardarEv(e); if (this.pendId === id) this.pendId = 0; this._contar(e);
        this._msg('   Pregunta #' + id + ' respondida: ' + (post === 1 ? 'erguida' : post === 2 ? 'caida' : 'no se puede determinar') + '.');
      }
      return Promise.resolve({ ok: !!post && id > 0 });
    }
    manual(p, n) {
      const post = { E: 1, C: 2, D: 3 }[p];
      if (!post || !(n > 0)) return Promise.resolve({ ok: false });
      if (n === this.tokManual) return Promise.resolve({ ok: true });
      const nv = Math.min(this._nVent(), this.vent.length);
      const v = nv >= 10 ? this._stats(nv) : null;
      if (this.modo !== 'SESION' || !this.estadoRms || !v) { this._msg('!! La confirmacion manual necesita la sesion en marcha y RMS suficiente.'); return Promise.resolve({ ok: true }); }
      this.tokManual = n;
      const e = this._armarEv(v, 1, this._periodo(this._tEf()));
      e.dur = nv / this.cfg.bps; e.tIni = this._tEf() - e.dur; e.tok = n; e.postura = post; e.status = post === 3 ? 3 : 2;
      this._guardarEv(e); this._contar(e); this.finVentUlt = Math.max(this.finVentUlt, this._tEf());
      this._msg('   Confirmacion manual #' + e.id + ': ' + (post === 1 ? 'erguida' : post === 2 ? 'caida' : 'no se puede determinar') + ' (RMS ' + Math.round(v.media) + ' mV, estado ' + (this.estadoRms === 1 ? 'ALTO' : 'BAJO') + ').');
      return Promise.resolve({ ok: true });
    }
    umbral(on, off, n) {
      if (!(off > 0 && on > off && on <= 5000) || !(n > 0)) return Promise.resolve({ ok: false });
      if (n === this.tokUmbral) return Promise.resolve({ ok: true });
      if (!this.calibrado) return Promise.resolve({ ok: true });
      this.tokUmbral = n; const antes = this.umbralOn; this._fijarUmbral(on, off, 1);
      this._msg('>> Umbral AJUSTADO (aprobado): U_ON ' + antes.toFixed(1) + ' -> ' + on.toFixed(1) + ' mV, U_OFF ' + off.toFixed(1) + ' mV.');
      return Promise.resolve({ ok: true });
    }
    restaurar(n) {
      if (!(n > 0)) return Promise.resolve({ ok: false });
      if (n === this.tokRestaurar) return Promise.resolve({ ok: true });
      if (this.hist.length < 2) { this._msg('!! No hay umbral anterior que restaurar.'); return Promise.resolve({ ok: true }); }
      this.tokRestaurar = n; const antes = this.umbralOn;
      this.hist.pop(); const h = this.hist.pop(); this._fijarUmbral(h[0], h[1], 2);
      this._msg('>> Umbral RESTAURADO: U_ON ' + antes.toFixed(1) + ' -> ' + this.umbralOn.toFixed(1) + ' mV, U_OFF ' + this.umbralOff.toFixed(1) + ' mV.');
      return Promise.resolve({ ok: true });
    }

    /* ---------- un bloque de RMS (lo que llega por el opto) ---------- */
    _cerrarActivacion(fin) { this._terminarActivacion(this.tBajo || fin); }
    _terminarActivacion(fin) {
      if (this.activo) {
        this.activo = false;
        const dur = fin - this.tIniAct; this.tActMs += dur;
        const rmsMedio = this.nRmsAct ? this.sumaAct / this.nRmsAct : 0;
        const pct = Math.round(100 * (rmsMedio - this.reposo) / Math.max(this.umbralOn - this.reposo, 1));
        this._msg('   Activacion #' + this.nActiv + ': ' + (dur / 1000).toFixed(1) + ' s, RMS medio ' + Math.round(rmsMedio) + ' mV (' + pct + ' % del umbral)');
        this.acts.push([this.sid, this.nActiv, +(this.msSesionIni / 1000).toFixed(1), +(dur / 1000).toFixed(1), Math.round(rmsMedio), pct]);
        if (this.acts.length > 32) this.acts.shift();
        this.nAct++;
      }
      this.tSobre = this.tBajo = 0;
    }
    feedBlock(mv) {
      const ahora = this.t;
      this.rms = mv; this.suave = this.hayDatos ? 0.5 * mv + 0.5 * this.suave : mv; this.hayDatos = true; this.tDato = ahora;
      let flag = 0;
      if (this.modo === 'CALIBRANDO') flag = this._calibrarBloque(mv) ? 2 : 0;
      else if (this.modo === 'SESION') {
        this.nivel = Math.min(1, Math.max(0, (this.suave - this.reposo) / (2 * Math.max(this.umbralOn - this.reposo, 1))));
        this._vAgregar(mv); this._actualizarEstadoRms();
        if (!this.activo) {
          if (this.suave >= this.umbralOn) {
            if (!this.tSobre) { this.tSobre = ahora; this.sumaAct = 0; this.nRmsAct = 0; this.msSesionIni = this.msSesion; }
            this.sumaAct += mv; this.nRmsAct++;
            if (ahora - this.tSobre + 50 >= 300) { this.activo = true; this.tIniAct = this.tSobre; this.nActiv++; this.tBajo = 0; }
          } else this.tSobre = 0;
        } else if (this.suave < this.umbralOff) {
          if (!this.tBajo) { this.tBajo = ahora; this.sumaCola = 0; this.nCola = 0; }
          this.sumaCola += mv; this.nCola++;
          if (ahora - this.tBajo + 50 >= 300) this._terminarActivacion(this.tBajo);
        } else {
          if (this.tBajo) { this.sumaAct += this.sumaCola; this.nRmsAct += this.nCola; this.tBajo = 0; }
          this.sumaAct += mv; this.nRmsAct++;
        }
        const nv = this._nVent();
        flag = (this.estadoRms === 1 ? 1 : 0) | ((this.bloquesEnEstado >= nv && this.vent.length >= nv) ? 4 : 0) | (this.activo ? 8 : 0);
      }
      this.blq.push([mv, flag]); if (this.blq.length > 64) this.blq.shift(); this.nBlq++;
    }

    /* ---------- mismo JSON que /estado ---------- */
    estado(q) {
      this.tUltimaConsulta = this.modoManual ? this.t : performance.now();
      q = q || { b: 0, a: 0, l: 0, r: 0 };
      const tail = (arr, n, from) => arr.slice(arr.length - (n - Math.max(from, n - arr.length, 0)));
      const t = this._tEf(), per = this._periodo(t), r1 = v => Math.round(v * 10) / 10;
      let espera = -1;
      if (this.modo === 'SESION' && this.solPer < per.max) espera = Math.max(this.tSolUlt + per.minEntre - t, per.ini + this.solPer * per.dur / (per.max + 1) - t, 0);
      // eventos con revisión nueva, de la más antigua a la más reciente (hasta 10)
      const todos = this.evs.filter(e => e && e.id <= this.nEv && e.rev > q.r).sort((a, b) => a.rev - b.rev);
      const sal = todos.slice(0, 10), hayMas = todos.length > sal.length;
      const er = hayMas ? sal[sal.length - 1].rev : this.revGlobal;
      const cb = this.cb, v = this._sostenidoVigente();
      return Promise.resolve({
        v: '4.0-sim', boot: this.boot, modo: this.modo, fase: this.fase, bloq: this.bloquesFase,
        total: this.fase === 'BASAL' ? Math.round(this.cfg.basal * this.cfg.bps) : 0, datos: this._datosOK() ? 1 : 0, rms: this.rms, suave: r1(this.suave),
        calibrado: this.calibrado ? 1 : 0, uon: r1(this.umbralOn), uoff: r1(this.umbralOff), uorig: this.umbralOrigen, ctl: this.controlAuto ? 1 : 0,
        activo: this.activo ? 1 : 0, nivel: Math.round(this.nivel * 100), durAct: this.activo ? r1((this.t - this.tIniAct) / 1000) : 0, sid: this.sid, nAct: this.nActiv,
        tAct: r1(this.tActMs / 1000), tSes: r1(this.msSesion / 1000), alt: Math.round(100 * (1 - Math.abs(this.pos) / 17400)), estOK: 1, malas: 0, rssi: -40,
        est: this.estadoRms, bEst: r1(this.bloquesEnEstado / this.cfg.bps), sost: this.modo === 'SESION' && v ? 1 : 0, ep: this.episodio, nDet: this.nDetectados,
        cb: { media: Math.round(cb.media * 100) / 100, sd: Math.round(cb.sd * 100) / 100, cv: Math.round(cb.cv * 1000) / 1000, der: Math.round(cb.deriva * 1000) / 1000,
          mn: r1(cb.minimo), mx: r1(cb.maximo), n: cb.n, fl: cb.flags, niv: cb.nivel },
        ver: { per: this.modo === 'SESION' ? per.idx : -1, max: per.max, sol: this.solPer, tPer: r1(Math.max(t - per.ini, 0)), dur: per.dur, esp: r1(espera), pend: this.pendId,
          nr: [this.nResp[1], this.nResp[2]], ne: [this.nErr[1], this.nErr[2]] },
        cfg: Object.assign({}, this.cfg),
        hist: this.hist.map(h => [r1(h[0]), r1(h[1]), h[2], Math.round(h[3])]),
        rev: this.revGlobal, er,
        ev: sal.map(e => [e.id, e.rev, e.ep, e.origen, e.estado, e.status, e.postura, e.sid, e.periodo, e.nSol, e.calidad, e.tok, r1(e.tIni), r1(e.dur), r1(e.media), r1(e.sd),
          r1(e.minimo), r1(e.maximo), r1(e.uon), r1(e.uoff)]),
        bn: this.nBlq, b: tail(this.blq, this.nBlq, q.b),
        an: this.nAct, acts: tail(this.acts, this.nAct, q.a),
        ln: this.nLog, logs: tail(this.logs, this.nLog, q.l)
      });
    }
  }
  EMG.AtrilHttp = AtrilHttp;
  EMG.AtrilLink = AtrilLink;
  EMG.AtrilSim = AtrilSim;
  EMG.decodeEv = decodeEv;
  if (typeof module !== 'undefined' && module.exports) module.exports = EMG;
})(typeof window !== 'undefined' ? window : globalThis);
