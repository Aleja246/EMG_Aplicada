/* Análisis BILATERAL (v8.3-BI): un semiespinoso en cada lado del cuello.
 *   Lado DERECHO   = A0 (la placa principal: es la que controla el atril, como siempre)
 *   Lado IZQUIERDO = A1 (solo se mide y se analiza aquí; el atril no lo usa)
 * Recibe, cada 100 ms, el RMS (SinArm, mV) de los dos lados calculado por el Arduino con la
 * misma cadena de filtros (paquete 0xAA 0x58).
 *
 * 1) Reposo de cada lado: durante la calibración del atril (cabeza apoyada, 10 s), igual que
 *    el atril: descarta el primer segundo, mediana de las medianas de 0.5 s, SD robusta =
 *    1.4826 x MAD, umbral = R + max(k x SD, 2 mV, margen x R) con la sensibilidad elegida
 *    (k 2/3/4, margen 10/20/35 %). Cada lado con SU reposo y SU umbral.
 * 2) Intento de levantar la cabeza: la mediana de 0.5 s de cualquiera de los dos lados pasa
 *    su umbral durante 0.3 s seguidos. Termina cuando los dos lados quedan bajo su umbral de
 *    relajación (R + 0.6 (U - R)) durante 0.5 s.
 * 3) En cada intento:
 *    - un lado está ACTIVO si pasó su umbral al menos 0.3 s seguidos (no fue un pico de ruido);
 *    - BILATERAL = los dos lados activos;
 *    - activación de cada lado = (RMS medio del intento - reposo) / reposo  (veces sobre su reposo).
 *      Se normaliza con el reposo de cada lado porque la amplitud en mV depende de la piel,
 *      los electrodos y la placa de cada canal: comparar mV directos no sería justo;
 *    - índice de asimetría IA = (D - I) / (D + I) x 100 %: positivo = predomina el derecho.
 *      |IA| < 10 % = sin predominio claro.
 * 4) Lado predominante de la sesión: el IA con todos los intentos (cada uno pesa su duración).
 * Limitación: normalizar con el reposo no es lo mismo que con una contracción máxima (%MVC).
 */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});
  const KS = [0, 2, 3, 4], MARGEN = [0, 0.10, 0.20, 0.35];
  const IA_CLARO = 10;                       // % de asimetría a partir del cual un lado "predomina"
  const B_INICIO = 3, B_FIN = 5, B_ACTIVO = 3, B_MAX = 1200;   // bloques de 100 ms
  const mediana = a => {
    const b = a.filter(v => isFinite(v)).sort((x, y) => x - y), n = b.length;
    return n ? (n % 2 ? b[(n - 1) / 2] : (b[n / 2 - 1] + b[n / 2]) / 2) : NaN;
  };
  const LADO_TXT = { D: 'Derecho', I: 'Izquierdo', S: 'Sin predominio claro' };

  class Bilateral {
    constructor() { this.cal = null; this.calBuf = []; this.nuevaSesion(); }
    nuevaSesion() { this.intentos = []; this.descartados = 0; this.recal = this.cal ? [this.cal] : []; this._limpiar(); }
    _limpiar() { this.hist = []; this.prev = []; this.it = null; this.sobre = 0; this.bajo = 0; this.ahora = null; }

    /* ---- calibración (la marca el atril) ---- */
    iniciarCal() { this.calBuf = []; }
    calibrar(d, i) { if (this.calBuf.length < 2000) this.calBuf.push([d, i]); }
    cerrarCal(sens) {
      sens = KS[sens] ? sens : 2;
      const b = this.calBuf.slice(10);       // sin el primer segundo
      this.calBuf = [];
      if (b.length < 50) return false;       // menos de 5 s con datos de los dos lados (p. ej. la página se abrió
                                             // a media calibración): no se cambia el reposo
      const lado = k => {
        const v = b.map(p => p[k]), meds = [];
        for (let j = 0; j + 5 <= v.length; j += 5) meds.push(mediana(v.slice(j, j + 5)));
        const R = mediana(meds), sd = 1.4826 * mediana(meds.map(x => Math.abs(x - R)));
        const on = R + Math.max(KS[sens] * sd, 2, MARGEN[sens] * R);
        return { reposo: R, sd, on, off: R + 0.6 * (on - R) };
      };
      if (this.it) this._cerrar(0);
      this.cal = { D: lado(0), I: lado(1), sens };
      this.recal.push(this.cal);
      this._limpiar();
      return true;
    }

    /* ---- un bloque de 100 ms durante la terapia. t = segundos de terapia ---- */
    bloque(t, d, i) {
      if (!this.cal || !isFinite(d) || !isFinite(i)) return null;
      const c = this.cal;
      this.hist.push([d, i]); if (this.hist.length > 5) this.hist.shift();
      const mD = mediana(this.hist.map(p => p[0])), mI = mediana(this.hist.map(p => p[1]));
      const aD = mD >= c.D.on, aI = mI >= c.I.on;
      this.ahora = { mD, mI, aD, aI };
      let it = this.it;
      if (!it) {
        this.prev.push([d, i, aD, aI]); if (this.prev.length > B_INICIO) this.prev.shift();
        this.sobre = aD || aI ? this.sobre + 1 : 0;
        if (this.sobre < B_INICIO) return null;
        it = this.it = { ini: t - 0.1 * (this.prev.length - 1), d: [], i: [], cD: 0, cI: 0, bestD: 0, bestI: 0, pD: 0, pI: 0 };
        for (const [pd, pi, pa, pb] of this.prev) this._sumar(pd, pi, pa, pb);
        this.prev = []; this.bajo = 0;
        return null;
      }
      this._sumar(d, i, aD, aI);
      this.bajo = mD < c.D.off && mI < c.I.off ? this.bajo + 1 : 0;
      if (this.bajo >= B_FIN) return this._cerrar(B_FIN);
      if (it.d.length >= B_MAX) return this._cerrar(0);   // 2 min: se cierra y empieza otro
      return null;
    }
    _sumar(d, i, aD, aI) {
      const it = this.it;
      it.d.push(d); it.i.push(i);
      it.cD = aD ? it.cD + 1 : 0; it.cI = aI ? it.cI + 1 : 0;
      it.bestD = Math.max(it.bestD, it.cD); it.bestI = Math.max(it.bestI, it.cI);
      if (d > it.pD) it.pD = d; if (i > it.pI) it.pI = i;
    }
    /** Cierra el intento en curso (p. ej. al pausar o terminar). */
    cortar() { const r = this.it ? this._cerrar(0) : null; this._limpiar(); return r; }
    _cerrar(quitar) {
      const it = this.it, c = this.cal; this.it = null; this.sobre = 0; this.bajo = 0; this.prev = [];
      const n = Math.max(1, it.d.length - quitar);          // sin la relajación del final
      const prom = a => a.slice(0, n).reduce((x, y) => x + y, 0) / n;
      const rmsD = prom(it.d), rmsI = prom(it.i);
      const actD = it.bestD >= B_ACTIVO, actI = it.bestI >= B_ACTIVO;
      if (!actD && !actI) { this.descartados++; return null; }   // ningún lado sostuvo la activación: ruido
      const inc = (v, l) => Math.max(0, (v - l.reposo) / Math.max(l.reposo, 0.5));
      const xD = inc(rmsD, c.D), xI = inc(rmsI, c.I);
      const ia = xD + xI > 0 ? 100 * (xD - xI) / (xD + xI) : 0;
      const r = {
        n: this.intentos.length + 1, inicioS: Math.max(0, it.ini), durS: 0.1 * n,
        rmsD, rmsI, picoD: it.pD, picoI: it.pI, activD: xD, activI: xI,
        derechoActivo: actD, izquierdoActivo: actI, bilateral: actD && actI,
        ia, lado: ia >= IA_CLARO ? 'D' : ia <= -IA_CLARO ? 'I' : 'S'
      };
      this.intentos.push(r);
      return r;
    }

    /** Resumen para guardar con la sesión (null si nunca hubo datos de los dos lados). */
    resumen() {
      if (!this.cal) return null;
      const L = this.intentos, c = this.cal;
      let sD = 0, sI = 0;
      for (const x of L) { sD += x.activD * x.durS; sI += x.activI * x.durS; }
      const ia = sD + sI > 0 ? 100 * (sD - sI) / (sD + sI) : null;
      const lado = !L.length || ia === null ? null : ia >= IA_CLARO ? 'D' : ia <= -IA_CLARO ? 'I' : 'S';
      const cuenta = f => L.filter(f).length;
      return {
        intentos: L.length,
        bilaterales: cuenta(x => x.bilateral),
        soloDerecho: cuenta(x => x.derechoActivo && !x.izquierdoActivo),
        soloIzquierdo: cuenta(x => x.izquierdoActivo && !x.derechoActivo),
        pctBilateral: L.length ? 100 * cuenta(x => x.bilateral) / L.length : null,
        predominaDerecho: cuenta(x => x.lado === 'D'), predominaIzquierdo: cuenta(x => x.lado === 'I'), sinPredominio: cuenta(x => x.lado === 'S'),
        indiceAsimetria: ia, lado, ladoTexto: lado ? LADO_TXT[lado] : 'Sin intentos detectados',
        descartadosRuido: this.descartados,
        reposoDMV: c.D.reposo, reposoIMV: c.I.reposo, umbralDMV: c.D.on, umbralIMV: c.I.on,
        sdDMV: c.D.sd, sdIMV: c.I.sd, calibraciones: this.recal.length,
        intentosDetalle: L.map(x => Object.assign({}, x))
      };
    }
  }
  Bilateral.LADO_TXT = LADO_TXT;
  Bilateral.IA_CLARO = IA_CLARO;
  EMG.Bilateral = Bilateral;
})();
