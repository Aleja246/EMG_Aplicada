/* Análisis de las confirmaciones de la fisioterapeuta y propuesta (supervisada) de un umbral nuevo.
 * Código puro (sin DOM) para poder probarlo.
 *
 * Idea: cada confirmación trae la ventana de RMS de un estado sostenido (media, SD...) y la postura que vio
 * la fisioterapeuta. Se comprueba si el RMS separa de verdad cabeza erguida y caída con ese niño, sin ASUMIR
 * que RMS alto = erguida: la «hipótesis» (RMS alto ↔ erguida, por defecto) es lo que se verifica.
 *
 * Reglas para proponer un umbral (todas deben cumplirse; si no, NO se propone nada):
 *  1. Observaciones INDEPENDIENTES: una por ventana, y las ventanas de la misma postura que estén a menos de
 *     gapIndepS segundos una de otra (o cuyo RMS sea el mismo episodio y esté pegado) se fusionan en una sola.
 *     Las muestras dentro de una ventana NUNCA son observaciones independientes. Se excluyen «no se puede
 *     determinar» y las ventanas de calidad dudosa. Si en un mismo episodio de RMS la postura cambió (el RMS no
 *     cambió de estado aunque la cabeza sí), no se descarta: es justo la evidencia de que el umbral no separa.
 *  2. Mínimo de observaciones independientes en CADA postura (nMinClase).
 *  3. Separación real: AUC ≥ aucMin en el sentido de la hipótesis. Si es ≤ 1 − aucMin la relación está INVERTIDA.
 *  4. El umbral actual se mide con las mismas observaciones (falsos positivos y falsos negativos).
 *  5. Candidato = el punto medio entre observaciones que mejor separa (exactitud balanceada).
 *  6. Validación con observaciones independientes: validación cruzada dejando una observación fuera (el
 *     candidato se vuelve a buscar sin ella y se prueba en ella).
 *  7. Solo se propone si mejora al umbral actual en el ajuste y en la validación, el cambio es relevante
 *     (≥ cambioMin) y razonable (dentro de [rangoMin, rangoMax] × el actual y por encima del ruido basal;
 *     si el salto supera 2× se avisa de que conviene revisar electrodos y calibración basal).
 *  8. Nunca se aplica solo: la página pide aprobación y el ESP32 lo recibe solo si se aprueba.
 */
(function (root) {
  'use strict';
  const A = {};

  A.CFG = {
    nMinClase: 4,       // observaciones independientes mínimas por postura
    gapIndepS: 60,      // ventanas de la misma postura más cercanas que esto cuentan como una sola observación
    aucMin: 0.75,       // separación mínima (AUC) para confiar en el RMS
    mejoraBal: 0.10,    // mejora mínima de exactitud balanceada (ajuste)
    mejoraLoo: 0.05,    // mejora mínima en la validación cruzada
    looMin: 0.75,       // exactitud balanceada mínima en la validación cruzada
    cambioMin: 0.10,    // cambio relativo mínimo del umbral para que valga la pena
    rangoMin: 1 / 3, rangoMax: 3,   // el umbral nuevo debe estar entre 1/3× y 3× el actual (si no, se pide revisar la calibración)
    avisoMin: 0.5, avisoMax: 2,     //   fuera de 0.5×–2× se propone, pero con una advertencia explícita
    ruidoSD: 2,         // y al menos media_basal + 2 SD
    funcionaBal: 0.9,   // por encima de esta exactitud el umbral «funciona»
    aptoClase: 5,       // observaciones por postura para considerar apto el control automático
    aptoBal: 0.85       //   y exactitud balanceada mínima
  };

  // Postura a la que, según la hipótesis, corresponde el RMS ALTO
  const posturaAlta = hip => (hip === 1 ? 1 : 2);   // 1 erguida, 2 caída

  /** Resultado de comparar el estado RMS del evento con la postura confirmada. */
  A.resultado = function (e, hip) {
    if (e.status === 1) return 'pendiente';
    if (e.status === 4) return 'sin respuesta';
    if (e.status === 3 || e.postura === 3) return 'sin determinar';
    if (e.status !== 2) return '—';
    const esperada = e.estado === 1 ? posturaAlta(hip) : (3 - posturaAlta(hip));
    return e.postura === esperada ? 'coincide' : 'no coincide';
  };

  /** Resumen de todas las confirmaciones (no solo las independientes). */
  A.resumen = function (events, hip) {
    const r = { total: events.length, auto: 0, manual: 0, confirmadas: 0, descartadas: 0, expiradas: 0, pendientes: 0,
      coinciden: 0, noCoinciden: 0, falsosPositivos: 0, falsosNegativos: 0 };
    for (const e of events) {
      if (e.origen === 1) r.manual++; else r.auto++;
      if (e.status === 1) r.pendientes++;
      else if (e.status === 3) r.descartadas++;
      else if (e.status === 4) r.expiradas++;
      else if (e.status === 2) {
        r.confirmadas++;
        const res = A.resultado(e, hip);
        if (res === 'coincide') r.coinciden++;
        else {
          r.noCoinciden++;
          // positivo = la postura a la que corresponde el RMS alto
          if (e.estado === 1) r.falsosPositivos++; else r.falsosNegativos++;
        }
      }
    }
    return r;
  };

  /** Observaciones independientes y lo que se descarta. */
  A.observaciones = function (events, hip, gapS) {
    gapS = gapS === undefined ? A.CFG.gapIndepS : gapS;
    const g = new Map(), eps = new Map();
    let descartadas = 0, dudosas = 0;
    for (const e of events) {
      if (e.status === 3) { descartadas++; continue; }
      if (e.status !== 2 || (e.postura !== 1 && e.postura !== 2)) continue;
      if (e.calidad !== 0) { dudosas++; continue; }
      const k = e.sid + ':' + e.postura;
      if (!g.has(k)) g.set(k, []);
      g.get(k).push(e);
      const ek = e.sid + ':' + e.ep;
      if (!eps.has(ek)) eps.set(ek, new Set());
      eps.get(ek).add(e.postura);
    }
    const obs = [];
    for (const [k, arr] of g) {
      arr.sort((a, b) => a.tIni - b.tIni);
      let grupo = [arr[0]];
      const cerrar = () => {
        const post = grupo[0].postura;
        obs.push({ k: k + '@' + grupo[0].id, mean: media(grupo.map(e => e.media)), post, A: post === posturaAlta(hip), n: grupo.length });
      };
      for (let i = 1; i < arr.length; i++) {
        const prev = grupo[grupo.length - 1];
        if (arr[i].tIni - (prev.tIni + prev.dur) < gapS) grupo.push(arr[i]);   // pegada a la anterior: misma observación
        else { cerrar(); grupo = [arr[i]]; }
      }
      cerrar();
    }
    const contradictorias = [...eps.values()].filter(x => x.size > 1).length;   // episodios de RMS con posturas distintas
    return { obs, descartadas, dudosas, contradictorias };
  };

  const media = a => a.reduce((s, v) => s + v, 0) / (a.length || 1);
  const sdev = a => { if (a.length < 2) return 0; const m = media(a); return Math.sqrt(a.reduce((s, v) => s + (v - m) ** 2, 0) / (a.length - 1)); };
  const mediana = a => { const b = a.slice().sort((x, y) => x - y), n = b.length; return n ? (n % 2 ? b[(n - 1) / 2] : (b[n / 2 - 1] + b[n / 2]) / 2) : 0; };

  /** Matriz de confusión: se predice «A» (la postura del RMS alto) si la media ≥ umbral. */
  A.matriz = function (obs, uon) {
    let tp = 0, fn = 0, fp = 0, tn = 0;
    for (const o of obs) {
      const pred = o.mean >= uon;
      if (o.A) { if (pred) tp++; else fn++; } else { if (pred) fp++; else tn++; }
    }
    const pos = tp + fn, neg = fp + tn;
    const tpr = pos ? tp / pos : 0, tnr = neg ? tn / neg : 0;
    return { tp, fn, fp, tn, bal: (tpr + tnr) / 2, acc: (tp + tn) / Math.max(1, obs.length), tpr, tnr };
  };

  /** AUC: probabilidad de que una observación de la postura «A» tenga más RMS que una de la otra. */
  A.auc = function (obs) {
    const a = obs.filter(o => o.A).map(o => o.mean), b = obs.filter(o => !o.A).map(o => o.mean);
    if (!a.length || !b.length) return 0.5;
    let s = 0;
    for (const x of a) for (const y of b) s += x > y ? 1 : x === y ? 0.5 : 0;
    return s / (a.length * b.length);
  };

  /** Mejor umbral: punto medio entre observaciones vecinas con la mayor exactitud balanceada. */
  A.mejorUmbral = function (obs, preferido) {
    const v = Array.from(new Set(obs.map(o => o.mean))).sort((x, y) => x - y);
    if (v.length < 2) return null;
    let best = null;
    for (let i = 0; i < v.length - 1; i++) {
      const c = (v[i] + v[i + 1]) / 2, m = A.matriz(obs, c);
      if (!best || m.bal > best.bal + 1e-9 || (Math.abs(m.bal - best.bal) <= 1e-9 && Math.abs(c - preferido) < Math.abs(best.c - preferido))) best = { c, bal: m.bal, m };
    }
    return best;
  };

  /** Validación cruzada dejando una observación fuera. */
  A.loo = function (obs, preferido) {
    let tp = 0, fn = 0, fp = 0, tn = 0;
    for (let i = 0; i < obs.length; i++) {
      const rest = obs.filter((_, j) => j !== i), b = A.mejorUmbral(rest, preferido);
      if (!b) return null;
      const pred = obs[i].mean >= b.c;
      if (obs[i].A) { if (pred) tp++; else fn++; } else { if (pred) fp++; else tn++; }
    }
    const pos = tp + fn, neg = fp + tn;
    return { tp, fn, fp, tn, bal: ((pos ? tp / pos : 0) + (neg ? tn / neg : 0)) / 2 };
  };

  const f1 = v => (Math.round(v * 10) / 10).toString();
  const f2 = v => (Math.round(v * 100) / 100).toFixed(2);
  const nombre = (hip, esA) => ((hip === 1) === esA ? 'erguida' : 'caída');

  /**
   * @param {object} p {events, uon, uoff, base:{media, sd}, f, hip, cfg?}
   * @returns {object} {estado, titulo, motivo, n, actual, stats, propuesta?, apto}
   */
  A.evaluar = function (p) {
    const C = Object.assign({}, A.CFG, p.cfg), hip = p.hip === 0 ? 0 : 1;
    const { obs, descartadas, dudosas, contradictorias } = A.observaciones(p.events, hip, C.gapIndepS);
    const nA = obs.filter(o => o.A).length, nB = obs.length - nA;
    const res = {
      estado: 'insuficiente', apto: false, hip,
      n: { A: nA, B: nB, total: obs.length, descartadas, dudosas, contradictorias, confirmadas: p.events.filter(e => e.status === 2).length },
      nombres: { A: nombre(hip, true), B: nombre(hip, false) }, actual: null, stats: null, propuesta: null, detalles: []
    };
    const dA = obs.filter(o => o.A).map(o => o.mean), dB = obs.filter(o => !o.A).map(o => o.mean);
    if (obs.length) res.stats = { medA: mediana(dA), medB: mediana(dB), sdA: sdev(dA), sdB: sdev(dB), auc: A.auc(obs) };
    if (obs.length) res.actual = A.matriz(obs, p.uon);
    if (contradictorias) res.detalles.push(contradictorias + ' episodio(s) en los que la postura cambió sin que el RMS cambiara de estado: cuentan como observaciones separadas (señal de que el umbral no separa bien).');
    if (dudosas) res.detalles.push(dudosas + ' ventana(s) de calidad dudosa no se usan.');
    if (descartadas) res.detalles.push(descartadas + ' respuesta(s) «no se puede determinar» no se usan.');

    if (!res.n.confirmadas) {
      res.estado = 'sin_datos'; res.titulo = 'Sin confirmaciones todavía';
      res.motivo = 'Aún no hay confirmaciones de la fisioterapeuta. El umbral es preliminar y se conserva el funcionamiento supervisado.';
      return res;
    }
    if (nA < C.nMinClase || nB < C.nMinClase) {
      res.titulo = 'Datos insuficientes para evaluar el umbral';
      res.motivo = 'Hay ' + nA + ' observación(es) independiente(s) con cabeza ' + res.nombres.A + ' y ' + nB + ' con cabeza ' + res.nombres.B +
        '; se necesitan al menos ' + C.nMinClase + ' de cada una (una por episodio, no por ventana). No se modifica el umbral.';
      return res;
    }
    const auc = res.stats.auc, act = res.actual;
    if (auc <= 1 - C.aucMin) {
      res.estado = 'invertida'; res.titulo = 'Relación invertida: no se ajusta';
      res.motivo = 'Con este niño el RMS es MÁS ALTO con la cabeza ' + res.nombres.B + ' que con la ' + res.nombres.A + ' (AUC ' + f2(auc) +
        '). Eso contradice la hipótesis actual. No se propone ningún umbral: conviene revisar la colocación de electrodos y la interpretación antes de usar el RMS para controlar nada.';
      return res;
    }
    if (auc < C.aucMin) {
      res.estado = 'sin_separacion'; res.titulo = 'El RMS no separa de forma confiable ambas posturas';
      res.motivo = 'Las observaciones con cabeza ' + res.nombres.A + ' y ' + res.nombres.B + ' se solapan demasiado (AUC ' + f2(auc) + ', se pide ≥ ' + C.aucMin +
        '). No se fuerza una separación que los datos no respaldan: se conserva el funcionamiento supervisado y el umbral actual.';
      return res;
    }
    const mejor = A.mejorUmbral(obs, p.uon), loo = A.loo(obs, p.uon);
    const apto = nA >= C.aptoClase && nB >= C.aptoClase && act.bal >= C.aptoBal;
    if (act.bal >= C.funcionaBal) {
      res.estado = 'correcto'; res.apto = apto; res.titulo = 'El umbral actual funciona bien';
      res.motivo = 'El umbral actual (U_ON ' + f1(p.uon) + ' mV) clasificó bien ' + (act.tp + act.tn) + ' de ' + obs.length + ' observaciones independientes (exactitud balanceada ' +
        f2(act.bal) + ', AUC ' + f2(auc) + '). No hace falta ajustarlo.';
      res.detalles.push(apto ? 'Hay evidencia suficiente para considerar el control automático de los actuadores.' : 'Aún faltan observaciones para considerar el control automático (mínimo ' + C.aptoClase + ' por postura).');
      return res;
    }
    if (!mejor || !loo) { res.estado = 'insuficiente'; res.titulo = 'Datos insuficientes'; res.motivo = 'No hay valores distintos suficientes para buscar un umbral.'; return res; }
    res.candidato = { c: mejor.c, bal: mejor.bal, loo: loo.bal };
    if (loo.bal < C.looMin || mejor.bal < C.looMin) {
      res.estado = 'contradictorio'; res.titulo = 'Observaciones contradictorias: no se ajusta';
      res.motivo = 'Aunque hay cierta diferencia entre posturas (AUC ' + f2(auc) + '), ningún umbral las separa de manera consistente (mejor exactitud balanceada ' + f2(mejor.bal) +
        ', validación cruzada ' + f2(loo.bal) + '; se pide ≥ ' + C.looMin + '). Se conserva el umbral actual y el funcionamiento supervisado.';
      return res;
    }
    const nuevo = mejor.c, base = p.base || { media: 0, sd: 0 }, f = p.f || 0.6;
    if (!(mejor.bal >= act.bal + C.mejoraBal && loo.bal >= act.bal + C.mejoraLoo)) {
      res.estado = 'sin_mejora'; res.titulo = 'Ningún umbral nuevo mejora al actual de forma clara';
      res.motivo = 'El mejor candidato (U_ON ' + f1(nuevo) + ' mV) logra exactitud balanceada ' + f2(mejor.bal) + ' (validación cruzada ' + f2(loo.bal) + ') frente a ' + f2(act.bal) +
        ' del actual; la mejora no alcanza el mínimo exigido. No se modifica el umbral.';
      return res;
    }
    if (nuevo < base.media + C.ruidoSD * base.sd) {
      res.estado = 'ruido'; res.titulo = 'Umbral propuesto dentro del ruido basal: no se aplica';
      res.motivo = 'El candidato (U_ON ' + f1(nuevo) + ' mV) quedaría a menos de ' + C.ruidoSD + ' SD del reposo (' + f1(base.media) + ' ± ' + f1(base.sd) + ' mV) y se dispararía con el ruido. Conviene repetir la calibración basal.';
      return res;
    }
    if (nuevo < p.uon * C.rangoMin || nuevo > p.uon * C.rangoMax) {
      res.estado = 'fuera_de_rango'; res.titulo = 'Cambio demasiado grande: revisar la calibración';
      res.motivo = 'El candidato (U_ON ' + f1(nuevo) + ' mV) es más de ' + f1(C.rangoMax) + ' veces mayor o menor que el actual (' + f1(p.uon) + ' mV). Un salto así sugiere un problema de electrodos o de la calibración basal; no se aplica sin repetirla.';
      return res;
    }
    if (Math.abs(nuevo - p.uon) / p.uon < C.cambioMin) {
      res.estado = 'sin_mejora'; res.titulo = 'El cambio sería demasiado pequeño';
      res.motivo = 'El candidato (U_ON ' + f1(nuevo) + ' mV) difiere menos del ' + Math.round(C.cambioMin * 100) + ' % del actual (' + f1(p.uon) + ' mV). No vale la pena modificarlo.';
      return res;
    }
    const off = base.media + f * (nuevo - base.media);
    res.estado = 'propuesta'; res.titulo = 'Se propone un ajuste del umbral';
    if (nuevo > p.uon * C.avisoMax || nuevo < p.uon * C.avisoMin) res.detalles.push('Atención: el cambio es grande (' + f1(nuevo / p.uon) + '× el umbral actual). Antes de aprobarlo, comprueba la colocación de los electrodos y considera repetir la calibración basal.');
    res.propuesta = { uon: nuevo, uoff: off, bal: mejor.bal, loo: loo.bal, m: mejor.m, actual: act, auc };
    res.motivo = 'El umbral actual (U_ON ' + f1(p.uon) + ' mV) clasificó mal ' + (act.fp + act.fn) + ' de ' + obs.length + ' observaciones independientes (' + act.fp + ' falso(s) positivo(s), ' +
      act.fn + ' falso(s) negativo(s)). Un U_ON de ' + f1(nuevo) + ' mV separa mejor: exactitud balanceada ' + f2(mejor.bal) + ' frente a ' + f2(act.bal) +
      ' y ' + f2(loo.bal) + ' en la validación cruzada (cada observación probada sin haberla usado para elegir el umbral).';
    return res;
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = A;
  else { root.EMG = root.EMG || {}; root.EMG.Analisis = A; }
})(typeof window !== 'undefined' ? window : globalThis);
