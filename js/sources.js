/* Fuentes de datos: Arduino por USB (Web Serial) y simulador.
 * Protocolo del Arduino (ver arduino/emg_stream/emg_stream.ino):
 *   0xAA 0x55 | seq (1B) | n (1B) | n x uint16 little-endian | checksum (XOR de seq, n y payload)
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
        if (this.lastSeq >= 0) this.lost += (seq - this.lastSeq - 1) & 255;
        this.lastSeq = seq;
        const s = new Uint16Array(n);
        for (let k = 0; k < n; k++) s[k] = b[i + 4 + 2 * k] | (b[i + 5 + 2 * k] << 8);
        this.packets++;
        this.onSamples(s);
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
      this.kind = 'simulación'; this.fault = null; this.timer = null;
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
      this.onSamples(out);
    }
  }

  EMG.SerialSource = SerialSource;
  EMG.SimSource = SimSource;
})();
