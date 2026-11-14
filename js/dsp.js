/* Procesamiento digital de la señal sEMG que se DIBUJA en la página.
 * La calibración y la detección las hace el ESP32 del atril (ver atril.js);
 * aquí solo se filtra la señal cruda del Arduino para verla.
 * Cadena: cuentas ADC -> quitar DC -> pasa altas 4º orden (latido/movimiento)
 *         -> notch 60 Hz y armónicos (como el Arduino) -> señal filtrada (mV)
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  EMG.DEFAULTS = {
    cfgVer: 2,       // sube cuando cambian los valores por defecto (descarta ajustes viejos de la v1)
    servidor: '192.168.4.1',   // el atril (ESP32); solo se usa si abres index.html como archivo
    emgUsb: false,   // true: la señal EMG llega por USB (Web Serial) en vez de por el servidor
    fs: 1000,        // Hz, igual que FS en arduino_emg.ino
    adcMax: 16383,   // 14 bits
    vref: 5000,      // mV, igual que V_REF en arduino_emg.ino
    gain: 1,         // 1 = mV a la salida de Nova (igual que el RMS del atril). ~1000 = µV en la piel
    notchHz: 60, notchQ: 20,
    nArm: 7,         // notch en 60, 120 ... 420 Hz, como el Arduino
    hpHz: 20,        // pasa altas: elimina latido cardiaco y movimiento (solo para ver)
    winS: 15,        // ventana visible de las gráficas (s)
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
  }

  // Notch en f0, 2·f0 ... n·f0 (solo los que quedan debajo de fs/2)
  function notchChain(f0, Q, n, fs) {
    const out = [];
    for (let k = 1; k <= n; k++) if (k * f0 < fs / 2) out.push(Biquad.notch(k * f0, Q, fs));
    return out;
  }

  class Processor {
    constructor(opts) {
      const o = (this.o = Object.assign({}, EMG.DEFAULTS, opts));
      const fs = o.fs;
      // Butterworth 4º orden = 2 biquads con Q 0.5412 y 1.3066
      this.hp = [Biquad.highpass(o.hpHz, 0.5412, fs), Biquad.highpass(o.hpHz, 1.3066, fs)];
      this.notches = notchChain(o.notchHz, o.notchQ, Math.max(1, o.nArm), fs);
      this.scale = o.vref / o.adcMax / o.gain; // cuentas -> mV
      this.dc = null;
      this.x = 0; this.hpOut = 0;
    }
    process(raw) {
      if (this.dc === null) this.dc = raw;        // evita transitorio inicial
      this.dc += 0.001 * (raw - this.dc);
      let v = (raw - this.dc) * this.scale;
      v = this.hp[0].process(v);
      v = this.hp[1].process(v);
      this.hpOut = v;
      for (const n of this.notches) v = n.process(v);
      this.x = v;
      return v;
    }
  }

  /* El mismo cálculo que hace el Arduino para el opto (arduino_emg.ino):
   * quitar DC (α = 0.003) -> notch 60..420 Hz -> RMS de 100 muestras, en mV.
   * Lo usa el simulador para alimentar al atril simulado. */
  class BlockRms {
    constructor(o) {
      o = Object.assign({}, EMG.DEFAULTS, o);
      this.fs = o.fs; this.n = Math.round(o.fs / 10);
      this.vPerCount = o.vref / 1000 / o.adcMax;            // V por cuenta
      this.arm = notchChain(60, 20, 7, o.fs);
      this.dc = null; this.s = 0; this.k = 0;
    }
    /** @returns RMS en mV cuando se completa un bloque, o null */
    feed(raw) {
      const v = raw * this.vPerCount;
      if (this.dc === null) this.dc = v;
      this.dc += 0.003 * (v - this.dc);
      let y = v - this.dc;
      for (const n of this.arm) y = n.process(y);
      this.s += y * y;
      if (++this.k < this.n) return null;
      const mv = Math.round(Math.sqrt(this.s / this.n) * 1000);
      this.s = 0; this.k = 0;
      return mv;
    }
  }

  // Calidad de señal / conexión: se evalúa cada ~500 ms
  class SignalQuality {
    constructor(adcMax) { this.adcMax = adcMax; this.reset(); }
    reset() { this.n = 0; this.sum = 0; this.sumsq = 0; this.sat = 0; this.hpE = 0; this.nE = 0; this.min = Infinity; this.max = -Infinity; }
    feed(raw, hp, xn) {
      this.n++; this.sum += raw; this.sumsq += raw * raw;
      if (raw < this.min) this.min = raw; if (raw > this.max) this.max = raw;
      if (raw <= this.adcMax * 0.01 || raw >= this.adcMax * 0.99) this.sat++;
      this.hpE += hp * hp; this.nE += xn * xn;
    }
    evaluate() {
      if (this.n < 100) return null;
      const mean = this.sum / this.n;
      const sd = Math.sqrt(Math.max(0, this.sumsq / this.n - mean * mean));
      const satFrac = this.sat / this.n;
      const ratio60 = this.hpE > 0 ? 1 - this.nE / this.hpE : 0; // energía que quitaron los notch
      const r = {
        sd, satFrac, ratio60, mean, min: this.min, max: this.max,
        saturated: satFrac > 0.2,
        flat: sd < 1.5,                       // cuentas ADC
        noisy60: ratio60 > 0.7 && sd >= 1.5
      };
      this.reset();
      return r;
    }
  }

  Object.assign(EMG, { Biquad, Processor, BlockRms, SignalQuality });
})();
