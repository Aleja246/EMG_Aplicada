/* Fuentes de la señal EMG cruda del Arduino: WiFi (recomendado), USB (Web Serial) y simulador.
 * Protocolo (arduino_emg.ino, igual por WiFi y por USB), paquetes de 20 muestras:
 *   0xAA 0x55 | seq (1B) | n (1B) | n x uint16 little-endian | checksum (XOR de seq, n y payload)
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});
  const sleep = ms => new Promise(r => setTimeout(r, ms));

  /* Opciones de fetch hacia un aparato de la red local.
   * Desde una página HTTPS (p. ej. GitHub Pages) Chrome solo deja pedir http://192.168.x.x si se
   * declara targetAddressSpace: 'local' (y pregunta al usuario). Desde un archivo local o http no
   * hace falta, y declararlo con un destino que no es de red local hace fallar la petición. */
  EMG.fetchOpts = function (url, extra) {
    const o = Object.assign({ cache: 'no-store' }, extra);
    const host = new URL(url).hostname;
    const privada = /^(10\.|192\.168\.|172\.(1[6-9]|2\d|3[01])\.)/.test(host);
    if (location.protocol === 'https:' && privada) o.targetAddressSpace = 'local';
    return o;
  };

  /* Separa los paquetes de un flujo de bytes (tolera bytes basura y cortes). */
  class PacketParser {
    constructor(onSamples) { this.onSamples = onSamples; this.buf = new Uint8Array(0); this.reset(); }
    reset() { this.lastSeq = -1; this.packets = 0; this.lost = 0; this.badBytes = 0; this.buf = new Uint8Array(0); }
    push(chunk) {
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
  }

  /* WiFi: GET http://<ip del Arduino>/stream, que nunca termina. */
  class WifiSource {
    constructor(onSamples, onStatus, ip) {
      this.onStatus = onStatus; this.url = 'http://' + ip + '/stream';
      this.parser = new PacketParser(onSamples);
      this.kind = 'wifi'; this.closing = false; this.ctrl = null;
    }
    get lost() { return this.parser.lost; }
    connect() { this.closing = false; this._run(); }

    async _run() {
      while (!this.closing) {
        this.onStatus('connecting');
        this.parser.reset();
        const ctrl = (this.ctrl = new AbortController());
        let last = performance.now(), watch = null, msg = '';
        try {
          watch = setInterval(() => { if (performance.now() - last > 4000) ctrl.abort(); }, 1000);
          const r = await fetch(this.url, EMG.fetchOpts(this.url, { signal: ctrl.signal }));
          if (!r.ok || !r.body) throw new Error('HTTP ' + r.status);
          const reader = r.body.getReader();
          last = performance.now();
          this.onStatus('connected');
          for (;;) {
            const { value, done } = await reader.read();
            if (done) break;
            last = performance.now();
            if (value && value.length) this.parser.push(value);
          }
          msg = 'el Arduino cerró la conexión';
        } catch (e) {
          msg = e.name === 'AbortError' ? 'sin datos del Arduino' : 'no se pudo conectar (' + (e.message || e.name) + ')';
        } finally { clearInterval(watch); }
        if (this.closing) break;
        this.onStatus('error', msg);
        await sleep(2000);       // reintenta solo
      }
    }

    disconnect() {
      this.closing = true;
      try { if (this.ctrl) this.ctrl.abort(); } catch (e) { /* ya cerrado */ }
      this.onStatus('disconnected');
    }
  }

  /* USB (Web Serial): arduino_emg.ino con USB_PARA_PAGINA = true. */
  class SerialSource {
    constructor(onSamples, onStatus) {
      this.onStatus = onStatus; this.parser = new PacketParser(onSamples);
      this.port = null; this.reader = null; this.kind = 'usb'; this.closing = false;
    }
    get lost() { return this.parser.lost; }
    static supported() { return 'serial' in navigator; }

    async connect() {
      this.onStatus('connecting');
      this.port = await navigator.serial.requestPort(); // requiere clic del usuario
      await this.port.open({ baudRate: 250000, bufferSize: 65536 });
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
              if (value) this.parser.push(value);
            }
          } finally { this.reader.releaseLock(); }
        }
        if (!this.closing) this.onStatus('error', 'El puerto se cerró');
      } catch (e) {
        if (!this.closing) this.onStatus('error', 'Se perdió la conexión USB (' + (e.message || e.name) + ')');
      }
    }

    async disconnect() {
      this.closing = true;
      try { if (this.reader) await this.reader.cancel(); } catch (e) { /* ya cerrado */ }
      try { if (this.port) await this.port.close(); } catch (e) { /* ya cerrado */ }
      this.port = null; this.onStatus('disconnected');
    }
  }

  /* Simulador de la señal cruda (cuentas del ADC de 14 bits) + 60 Hz + latido.
   * getCtx() devuelve {rmsMv}: el nivel de RMS que el atril simulado considera ahora (así la señal que se
   * dibuja y el RMS que "recibe" el atril cuentan la misma historia). */
  class SimSource {
    constructor(onSamples, onStatus, getCtx, fs) {
      this.onSamples = onSamples; this.onStatus = onStatus; this.getCtx = getCtx; this.fs = fs || 1000;
      this.kind = 'sim'; this.fault = null; this.timer = null; this.lost = 0;
      this.n = 0; this.last = 0; this.lvl = 30;
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
      if (k > this.fs) { this.last = now - 1000; k = this.fs; }   // pestaña en segundo plano: no generar de golpe
      this.last += k * 1000 / this.fs;
      const out = new Uint16Array(k);
      const ctx = this.getCtx() || {}, target = ctx.rmsMv != null ? ctx.rmsMv : 30;
      for (let i = 0; i < k; i++) {
        const t = this.n++ / this.fs;
        this.lvl += (target - this.lvl) * 0.02;
        let v = 8192 + 30 * Math.sin(2 * Math.PI * 0.2 * t);              // deriva lenta
        v += 40 * Math.sin(2 * Math.PI * 60 * t) + 12 * Math.sin(2 * Math.PI * 180 * t); // red
        const ph = (t * 1.3) % 1; if (ph < 0.04) v += 25 * Math.sin(Math.PI * ph / 0.04); // latido
        v += this.lvl * (16383 / 5000) * this._randn();                   // EMG: RMS en mV a la salida de Nova → cuentas
        if (this.fault === 'disconnect') v = 16383 - Math.random() * 20;
        else if (this.fault === 'flat') v = 8192;
        else if (this.fault === 'noise60') v += 2500 * Math.sin(2 * Math.PI * 60 * t);
        out[i] = Math.max(0, Math.min(16383, Math.round(v)));
      }
      if (this.fault === 'nodata') return;
      this.onSamples(out);
    }
  }

  EMG.PacketParser = PacketParser;
  EMG.WifiSource = WifiSource;
  EMG.SerialSource = SerialSource;
  EMG.SimSource = SimSource;
})();
