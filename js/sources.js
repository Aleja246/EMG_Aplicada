/* Fuentes de datos: Arduino por USB (Web Serial), Arduino UNO R4 por WiFi (vía el
 * puente local) y simulador. Todas llaman onSamples(muestras, { start }) donde
 * start = número absoluto de la primera muestra (sirve para el eje de tiempo y
 * para detectar huecos) y exponen fs (Hz) y units ('cuentas' o 'mV').
 *
 * USB (arduino/emg_stream/emg_stream.ino):
 *   0xAA 0x55 | seq (1B) | n (1B) | n x uint16 little-endian | checksum (XOR de seq, n y payload)
 * WiFi (arduino/diagnostico_emg_enlace_wifi, ver emg_flujo.h):
 *   'E' 'M' | ver=1 | n | fs uint16 | índice uint32 | µV por unidad uint16 | n x int16 | CRC-16/CCITT-FALSE
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  class SerialSource {
    constructor(onSamples, onStatus) {
      this.onSamples = onSamples; this.onStatus = onStatus;
      this.port = null; this.reader = null; this.buf = new Uint8Array(0);
      this.lastSeq = -1; this.packets = 0; this.lost = 0; this.badBytes = 0;
      this.kind = 'arduino'; this.closing = false;
      this.fs = 2000;                 // la app lo ajusta a Ajustes → fs (debe coincidir con el firmware)
      this.units = 'cuentas'; this.index = 0; this.lostSamples = 0;
    }
    static supported() { return 'serial' in navigator; }

    async connect() {
      this.onStatus('connecting');
      this.port = await navigator.serial.requestPort(); // requiere clic del usuario
      await this.port.open({ baudRate: 500000, bufferSize: 65536 });
      this.closing = false;
      this.onStatus('connected');
      this._loop();
    }

    async _loop() {
      try {
        while (this.port && this.port.readable && !this.closing) {
          this.reader = this.port.readable.getReader();
          try {
            for (;;) {
              const { value, done } = await this.reader.read();
              if (done) break;
              if (value) this._parse(value);
            }
          } finally { this.reader.releaseLock(); }
        }
        if (!this.closing) this.onStatus('error', 'El puerto se cerró');
      } catch (e) {
        if (!this.closing) this.onStatus('error', 'Se perdió la conexión USB (' + (e.message || e.name) + ')');
      }
    }

    _parse(chunk) {
      const b = new Uint8Array(this.buf.length + chunk.length);
      b.set(this.buf); b.set(chunk, this.buf.length);
      let i = 0;
      while (i + 5 <= b.length) {
        if (b[i] !== 0xAA || b[i + 1] !== 0x55) { i++; this.badBytes++; continue; }
        const n = b[i + 3];
        if (n === 0 || n > 64) { i++; this.badBytes++; continue; }
        const len = 4 + 2 * n + 1;
        if (i + len > b.length) break; // paquete incompleto
        let chk = 0;
        for (let k = i + 2; k < i + len - 1; k++) chk ^= b[k];
        if (chk !== b[i + len - 1]) { i++; this.badBytes++; continue; }
        const seq = b[i + 2];
        if (this.lastSeq >= 0) {
          const perdidos = (seq - this.lastSeq - 1) & 255;
          this.lost += perdidos; this.lostSamples += perdidos * n; this.index += perdidos * n;
        }
        this.lastSeq = seq;
        const s = new Uint16Array(n);
        for (let k = 0; k < n; k++) s[k] = b[i + 4 + 2 * k] | (b[i + 5 + 2 * k] << 8);
        this.packets++;
        const start = this.index; this.index += n;
        this.onSamples(s, { start });
        i += len;
      }
      this.buf = b.slice(i);
    }

    async disconnect() {
      this.closing = true;
      try { if (this.reader) await this.reader.cancel(); } catch (e) { /* ya cerrado */ }
      try { if (this.port) await this.port.close(); } catch (e) { /* ya cerrado */ }
      this.port = null; this.onStatus('disconnected');
    }
  }

  /* Simulador: EMG sintético + 60 Hz + latido + artefactos de transición.
   * getCtx() devuelve la fase de la app para que la simulación "obedezca" la calibración. */
  class SimSource {
    constructor(onSamples, onStatus, getCtx, fs) {
      this.onSamples = onSamples; this.onStatus = onStatus; this.getCtx = getCtx; this.fs = fs || 2000;
      this.kind = 'simulación'; this.fault = null; this.timer = null; this.units = 'cuentas';
      this.packets = 0; this.lost = 0; this.badBytes = 0;
      this.n = 0; this.last = 0; this.amp = 0; this.burst = 0; this.prevUp = false;
      this.autoUp = false; this.autoLeft = 3; this.tickS = 0;
    }
    start() {
      this.last = performance.now();
      this.timer = setInterval(() => this._gen(), 20);
      this.onStatus('connected');
    }
    stop() { clearInterval(this.timer); this.timer = null; this.onStatus('disconnected'); }
    setFault(f) { this.fault = f; }
    _randn() { return Math.sqrt(-2 * Math.log(1 - Math.random())) * Math.cos(2 * Math.PI * Math.random()); }

    _gen() {
      const now = performance.now();
      let k = Math.round((now - this.last) * this.fs / 1000);
      if (k < 1) return;
      this.last += k * 1000 / this.fs;
      const out = new Uint16Array(k), dt = 1 / this.fs;
      const ctx = this.getCtx() || {};
      for (let i = 0; i < k; i++) {
        const t = this.n++ / this.fs;
        // ¿hay contracción objetivo ahora?
        let up = false, extraBurst = false;
        if (ctx.phase === 'active') up = !!ctx.cueUp;
        else if (ctx.phase === 'passive') { const c = t % 2.5; extraBurst = c < 0.5; }
        else if (ctx.phase === 'running') {
          this.autoLeft -= dt;
          if (this.autoLeft <= 0) {
            this.autoUp = !this.autoUp;
            this.autoLeft = this.autoUp ? 1.5 + Math.random() * 6 : 2 + Math.random() * 3;
            if (!this.autoUp && Math.random() < 0.4) { this.autoUp = true; this.autoLeft = 0.35; } // micro-activación
          }
          up = this.autoUp;
        }
        if (up !== this.prevUp) { this.burst = 0.4; this.prevUp = up; } // artefacto de transición
        this.burst = Math.max(0, this.burst - dt);
        const target = (up ? 1 : 0) + (this.burst > 0 ? 1.4 : 0) + (extraBurst ? 1.1 : 0);
        this.amp += (target - this.amp) * 0.02;
        let v = 8192 + 20 * Math.sin(2 * Math.PI * 0.2 * t);               // deriva lenta
        v += 2.5 * this._randn();                                          // ruido de reposo
        v += 4 * Math.sin(2 * Math.PI * 60 * t);                           // red 60 Hz
        const ph = (t * 1.3) % 1; if (ph < 0.04) v += 40 * Math.sin(Math.PI * ph / 0.04); // latido
        v += this.amp * 70 * this._randn();                                // EMG
        if (this.fault === 'disconnect') v = 16383 - Math.random() * 20;
        else if (this.fault === 'flat') v = 8192;
        else if (this.fault === 'noise60') v += 2500 * Math.sin(2 * Math.PI * 60 * t);
        out[i] = Math.max(0, Math.min(16383, Math.round(v)));
      }
      if (this.fault === 'nodata') return;
      this.packets += Math.ceil(k / 20);
      this.onSamples(out, { start: this.n - k });
    }
  }

  /* Arduino UNO R4 WiFi a través del puente local (puente/puente_emg.py).
   * La página (HTTPS en GitHub Pages) no puede abrir ws:// hacia la IP del Arduino
   * (contenido mixto), pero sí hacia ws://127.0.0.1, que el navegador trata como seguro. */
  function crc16(u8, n) {
    let c = 0xFFFF;
    for (let i = 0; i < n; i++) {
      c ^= u8[i] << 8;
      for (let b = 0; b < 8; b++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    return c;
  }

  class WifiSource {
    constructor(onSamples, onStatus, url) {
      this.onSamples = onSamples; this.onStatus = onStatus; this.url = url;
      this.kind = 'wifi'; this.fs = 1000; this.units = 'mV';
      this.ws = null; this.wanted = false; this.retry = null; this.attempts = 0;
      this.packets = 0; this.lost = 0; this.lostSamples = 0; this.badPackets = 0; this.resets = 0;
      this.bridge = null;            // último estado que mandó el puente
      this.next = null;              // siguiente índice absoluto esperado
      this.offset = 0;               // corrige el índice si el Arduino se reinicia
      this.lastArrival = 0;
    }
    start() { this.wanted = true; this._open(); }
    stop() {
      this.wanted = false; clearTimeout(this.retry);
      if (this.ws) { try { this.ws.close(); } catch (e) { /* ya cerrado */ } }
      this.ws = null; this.bridge = null; this.onStatus('disconnected');
    }
    _open() {
      this.attempts++;
      this.onStatus('connecting', 'Buscando el puente en ' + this.url);
      let ws;
      try { ws = new WebSocket(this.url); }
      catch (e) { this._retry('Dirección del puente no válida: ' + this.url); return; }
      ws.binaryType = 'arraybuffer';
      this.ws = ws;
      ws.onopen = () => { this.attempts = 0; this.onStatus('connected'); };
      ws.onmessage = ev => {
        if (typeof ev.data === 'string') { try { this.bridge = JSON.parse(ev.data); } catch (e) { /* ignorar */ } return; }
        this._packet(ev.data);
      };
      ws.onclose = () => {
        if (this.ws !== ws) return;
        this.ws = null; this.bridge = null;
        if (this.wanted) this._retry('Sin conexión con el puente (¿está abierto puente_emg.py?)');
      };
      ws.onerror = () => { /* onclose decide */ };
    }
    _retry(msg) {
      this.onStatus('connecting', msg);
      clearTimeout(this.retry);
      this.retry = setTimeout(() => { if (this.wanted) this._open(); }, 2000);
    }
    _packet(buf) {
      const u8 = new Uint8Array(buf), dv = new DataView(buf);
      if (u8.length < 14 || u8[0] !== 0x45 || u8[1] !== 0x4D || u8[2] !== 1) { this.badPackets++; return; }
      const n = u8[3], len = 12 + 2 * n + 2;
      if (n === 0 || u8.length !== len || crc16(u8, len - 2) !== dv.getUint16(len - 2, true)) { this.badPackets++; return; }
      const fs = dv.getUint16(4, true), idx = dv.getUint32(6, true), uv = dv.getUint16(10, true);
      if (fs !== this.fs) { this.fs = fs; this.next = null; }
      const now = performance.now();
      let abs = idx + this.offset, gap = 0;
      if (this.next !== null) {
        if (abs < this.next) {
          // El índice volvió atrás: el Arduino se reinició. Se sigue el tiempo con lo que pasó de verdad.
          const est = Math.max(0, Math.round((now - this.lastArrival) / 1000 * fs) - n);
          this.offset = this.next + est - idx; abs = idx + this.offset; this.resets++;
        }
        gap = abs - this.next;
        if (gap > 0) { this.lost++; this.lostSamples += gap; }
      }
      this.next = abs + n; this.lastArrival = now; this.packets++;
      const mv = new Float32Array(n), k = uv / 1000;
      for (let i = 0; i < n; i++) mv[i] = dv.getInt16(12 + 2 * i, true) * k;
      this.onSamples(mv, { start: abs, gap });
    }
    // Texto corto para el indicador y las alarmas
    detail() {
      if (!this.ws || this.ws.readyState !== 1) return { ok: false, level: 'bad', text: 'sin puente' };
      if (!this.bridge || !this.bridge.arduino) return { ok: false, level: 'warn', text: 'esperando al Arduino' };
      return { ok: true, level: 'ok', text: 'conectado' };
    }
  }
  EMG.WifiSource = WifiSource;
  EMG.crc16 = crc16;

  EMG.SerialSource = SerialSource;
  EMG.SimSource = SimSource;
})();
