/* Procesamiento digital de la señal sEMG (corre en el navegador).
 * Cadena: raw ADC -> quitar DC -> pasa altas 4º orden (latido/movimiento)
 *         -> notch 60 Hz -> señal filtrada (mV)
 *         -> |x| -> pasa bajas 5 Hz -> envolvente (mV)
 * Métricas por contracción: RMS, MAV, MNF, MDF (Welch 1024 pts).
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  EMG.DEFAULTS = {
    fs: 2000,        // Hz, debe coincidir con el firmware de Arduino
    adcMax: 16383,   // 14 bits (usa 1023 si tu placa es de 10 bits)
    vref: 5000,      // mV, igual que 'escala' en DIGITALIZARR.ino
    gain: 1,         // ganancia del amplificador (para expresar mV en el músculo)
    notchHz: 60, notchQ: 20,
    hpHz: 20,        // pasa altas: elimina latido cardiaco y movimiento
    lpHz: 5,         // pasa bajas de la envolvente
    minDur: 1.0,     // s, una activación menor a esto se descarta (transición/artefacto)
    maxGap: 0.3,     // s, huecos menores se fusionan en una sola activación
    thrSD: 3,        // umbral = media_reposo + thrSD * SD_reposo
    thrPct: 25,      // ... o este % de la envolvente activa (el mayor de los dos)
    trimStart: 0,    // s ignorados al inicio de cada activación para RMS/MAV
    restS: 10, passiveS: 10, reps: 5, holdS: 4, relaxS: 4,
    winS: 15,        // ventana visible de la gráfica (s)
    techView: false, // mostrar RMS/MAV/MNF/MDF crudos en la tabla
    sound: true,
    sheetsUrl: '', sheetsName: false
  };

  // Filtro biquad (forma transpuesta II), coeficientes RBJ
  class Biquad {
    constructor(b0, b1, b2, a1, a2) {
      this.b0 = b0; this.b1 = b1; this.b2 = b2; this.a1 = a1; this.a2 = a2;
      this.z1 = 0; this.z2 = 0;
    }
    process(x) {
      const y = this.b0 * x + this.z1;
      this.z1 = this.b1 * x - this.a1 * y + this.z2;
      this.z2 = this.b2 * x - this.a2 * y;
      return y;
    }
    static _n(b0, b1, b2, a0, a1, a2) { return new Biquad(b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0); }
    static notch(f0, Q, fs) {
      const w = 2 * Math.PI * f0 / fs, c = Math.cos(w), al = Math.sin(w) / (2 * Q);
      return Biquad._n(1, -2 * c, 1, 1 + al, -2 * c, 1 - al);
    }
    static highpass(fc, Q, fs) {
      const w = 2 * Math.PI * fc / fs, c = Math.cos(w), al = Math.sin(w) / (2 * Q);
      return Biquad._n((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }
    static lowpass(fc, Q, fs) {
      const w = 2 * Math.PI * fc / fs, c = Math.cos(w), al = Math.sin(w) / (2 * Q);
      return Biquad._n((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
    }
  }

  class Processor {
    constructor(opts) {
      const o = (this.o = Object.assign({}, EMG.DEFAULTS, opts));
      const fs = o.fs;
      // Butterworth 4º orden = 2 biquads con Q 0.5412 y 1.3066
      this.hp = [Biquad.highpass(o.hpHz, 0.5412, fs), Biquad.highpass(o.hpHz, 1.3066, fs)];
      this.notch = Biquad.notch(o.notchHz, o.notchQ, fs);
      this.lp = Biquad.lowpass(o.lpHz, 0.7071, fs);
      this.scale = o.vref / o.adcMax / o.gain; // cuentas -> mV
      this.dc = null;
      this.x = 0; this.env = 0; this.hpOut = 0;
    }
    process(raw) {
      if (this.dc === null) this.dc = raw;        // evita transitorio inicial
      this.dc += 0.001 * (raw - this.dc);
      let v = (raw - this.dc) * this.scale;
      v = this.hp[0].process(v);
      v = this.hp[1].process(v);
      this.hpOut = v;
      v = this.notch.process(v);
      this.x = v;
      this.env = this.lp.process(Math.abs(v));
      if (this.env < 0) this.env = 0;
      return v;
    }
  }

  function fft(re, im) {
    const n = re.length;
    for (let i = 1, j = 0; i < n; i++) {
      let bit = n >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) { let t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (let len = 2; len <= n; len <<= 1) {
      const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
      for (let i = 0; i < n; i += len) {
        let cr = 1, ci = 0;
        for (let k = 0; k < len / 2; k++) {
          const a = i + k, b = a + len / 2;
          const tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
          re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
          const ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr;
        }
      }
    }
  }

  // Frecuencia media (MNF) y mediana (MDF) con periodograma de Welch, 20-450 Hz
  function spectralFreqs(x, fs, nfft) {
    nfft = nfft || 1024;
    const psd = new Float64Array(nfft / 2 + 1);
    const re = new Float64Array(nfft), im = new Float64Array(nfft);
    const win = new Float64Array(nfft);
    for (let i = 0; i < nfft; i++) win[i] = 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (nfft - 1));
    let mean = 0;
    for (let i = 0; i < x.length; i++) mean += x[i];
    mean /= x.length;
    const step = nfft / 2;
    const starts = [];
    for (let s = 0; s + nfft <= x.length; s += step) starts.push(s);
    if (!starts.length) starts.push(0); // segmentos cortos: relleno con ceros
    for (const s of starts) {
      for (let i = 0; i < nfft; i++) {
        const v = s + i < x.length ? x[s + i] - mean : 0;
        re[i] = v * win[i]; im[i] = 0;
      }
      fft(re, im);
      for (let k = 0; k <= nfft / 2; k++) psd[k] += re[k] * re[k] + im[k] * im[k];
    }
    const df = fs / nfft;
    let tot = 0, wsum = 0;
    const lo = Math.ceil(20 / df), hi = Math.floor(450 / df);
    for (let k = lo; k <= hi; k++) { tot += psd[k]; wsum += psd[k] * k * df; }
    if (tot <= 0) return { mnf: NaN, mdf: NaN };
    let acc = 0, mdf = lo * df;
    for (let k = lo; k <= hi; k++) { acc += psd[k]; if (acc >= tot / 2) { mdf = k * df; break; } }
    return { mnf: wsum / tot, mdf };
  }

  // Detecta activaciones sobre la envolvente con histéresis.
  class ActivationDetector {
    constructor(o) {
      this.fs = o.fs; this.onThr = o.onThr; this.offThr = o.offThr;
      this.minDur = o.minDur; this.maxGap = o.maxGap; this.trimStart = o.trimStart || 0;
      this.reset();
    }
    reset() {
      this.active = false; this.startT = 0; this.lastAboveT = 0;
      this.buf = []; this.sumsq = 0; this.sumabs = 0; this.peak = 0;
    }
    /** @returns segmento terminado ({valid, dur, rms, mav, ...}) o null */
    feed(x, env, t) {
      if (!this.active) {
        if (env > this.onThr) {
          this.active = true; this.startT = t; this.lastAboveT = t;
          this.buf = [x]; this.sumsq = x * x; this.sumabs = Math.abs(x); this.peak = env;
        }
        return null;
      }
      this.buf.push(x);
      this.sumsq += x * x; this.sumabs += Math.abs(x);
      if (env > this.peak) this.peak = env;
      if (env > this.offThr) this.lastAboveT = t;
      if (t - this.lastAboveT > this.maxGap) return this.flush();
      return null;
    }
    /** duración en curso y RMS provisional (para mostrar en vivo) */
    live(t) {
      if (!this.active) return null;
      const n = this.buf.length || 1;
      return { tStart: this.startT, dur: t - this.startT, rms: Math.sqrt(this.sumsq / n), mav: this.sumabs / n };
    }
    flush() {
      if (!this.active) return null;
      const dur = this.lastAboveT - this.startT;
      const n = Math.min(this.buf.length, Math.round(dur * this.fs) + 1);
      const seg = { tStart: this.startT, tEnd: this.lastAboveT, dur, peakEnv: this.peak, valid: dur >= this.minDur };
      if (seg.valid) {
        let skip = Math.round(this.trimStart * this.fs);
        if (n - skip < this.fs * 0.25) skip = 0;
        const x = Float64Array.from(this.buf.slice(skip, n));
        let sq = 0, ab = 0;
        for (let i = 0; i < x.length; i++) { sq += x[i] * x[i]; ab += Math.abs(x[i]); }
        seg.rms = Math.sqrt(sq / x.length);
        seg.mav = ab / x.length;
        const f = spectralFreqs(x, this.fs, 1024);
        seg.mnf = f.mnf; seg.mdf = f.mdf;
      }
      this.reset();
      return seg;
    }
  }

  // Calidad de señal / conexión: se evalúa cada ~500 ms
  class SignalQuality {
    constructor(adcMax) { this.adcMax = adcMax; this.reset(); }
    reset() { this.n = 0; this.sum = 0; this.sumsq = 0; this.sat = 0; this.hpE = 0; this.nE = 0; }
    feed(raw, hp, xn) {
      this.n++; this.sum += raw; this.sumsq += raw * raw;
      if (raw <= this.adcMax * 0.01 || raw >= this.adcMax * 0.99) this.sat++;
      this.hpE += hp * hp; this.nE += xn * xn;
    }
    evaluate() {
      if (this.n < 100) return null;
      const mean = this.sum / this.n;
      const sd = Math.sqrt(Math.max(0, this.sumsq / this.n - mean * mean));
      const satFrac = this.sat / this.n;
      const ratio60 = this.hpE > 0 ? 1 - this.nE / this.hpE : 0; // energía que quitó el notch
      const r = {
        sd, satFrac, ratio60,
        saturated: satFrac > 0.2,
        flat: sd < 1.5,                       // cuentas ADC
        noisy60: ratio60 > 0.7 && sd >= 1.5
      };
      this.reset();
      return r;
    }
  }

  Object.assign(EMG, { Biquad, Processor, ActivationDetector, SignalQuality, spectralFreqs, fft });
})();
