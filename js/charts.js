/* Gráficas en canvas (sin librerías externas, funciona sin internet). */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});
  const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();

  function fit(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth, h = canvas.clientHeight;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { ctx, w, h };
  }

  /* Señal EMG cruda del Arduino (filtrada) en tiempo real: buffer circular, mín/máx por columna de píxeles.
   * flag por muestra: 2 = activación (o ARRIBA en calibración), 1 = fase de calibración midiendo. */
  class LiveChart {
    constructor(canvas, fs) { this.c = canvas; this.fs = fs; this.ymax = 0.2; this.zoom = 1; this.setWindow(15); }
    setWindow(s) {
      this.win = s; this.N = Math.round(s * this.fs);
      this.sig = new Float32Array(this.N); this.flag = new Uint8Array(this.N); this.w = 0;
    }
    push(x, flag) {
      this.sig[this.w] = x; this.flag[this.w] = flag;
      if (++this.w >= this.N) this.w = 0;
    }
    draw() {
      const { ctx, w, h } = fit(this.c);
      if (!w || !h) return;
      const L = 52, R = 10, T = 8, B = 24, W = w - L - R, H = h - T - B;
      ctx.clearRect(0, 0, w, h);
      const cols = Math.max(1, Math.floor(W)), N = this.N;
      const mn = new Float32Array(cols), mx = new Float32Array(cols), fl = new Uint8Array(cols);
      let target = 0.02;
      for (let c = 0; c < cols; c++) {
        const i0 = Math.floor(c * N / cols), i1 = Math.max(i0 + 1, Math.floor((c + 1) * N / cols));
        let lo = Infinity, hi = -Infinity, f = 0;
        for (let i = i0; i < i1; i++) {
          const j = (this.w + i) % N, v = this.sig[j];
          if (v < lo) lo = v; if (v > hi) hi = v;               // NaN (muestra apartada) no cuenta
          if (this.flag[j] > f) f = this.flag[j];
        }
        if (lo === Infinity) { lo = c ? mn[c - 1] : 0; hi = c ? mx[c - 1] : 0; }   // columna sin muestras validas: como la anterior
        mn[c] = lo; mx[c] = hi; fl[c] = f;
      }
      // Escala ROBUSTA: el 98 % de las columnas cabe; un pico suelto (movimiento, cable) no aplasta la señal.
      // Lo que se pasa del borde se recorta. El zoom (botones − / +) multiplica sobre esto.
      const picos = new Float32Array(cols);
      for (let c = 0; c < cols; c++) picos[c] = Math.max(-mn[c], mx[c]);
      picos.sort();
      target = Math.max(target, picos[Math.min(cols - 1, Math.floor(cols * 0.98))] * 1.25) / this.zoom;
      this.ymax = target > this.ymax ? this.ymax + (target - this.ymax) * 0.3 : this.ymax - (this.ymax - target) * 0.05;
      const Y = v => T + H / 2 - (Math.max(-this.ymax, Math.min(this.ymax, v)) / this.ymax) * (H / 2);
      // Línea de ACTIVACIÓN: RMS de ~100 ms de la señal (como el Serial Plotter)
      const porCol = N / cols, env = new Float32Array(cols), anchoEnv = Math.max(1, Math.round(0.1 * this.fs / porCol));
      const e2 = new Float32Array(cols), en = new Float32Array(cols);
      for (let c = 0; c < cols; c++) {
        const i0 = Math.floor(c * N / cols), i1 = Math.max(i0 + 1, Math.floor((c + 1) * N / cols));
        for (let i = i0; i < i1; i++) { const v = this.sig[(this.w + i) % N]; if (v === v) { e2[c] += v * v; en[c]++; } }
      }
      for (let c = 0; c < cols; c++) {
        let s = 0, n = 0;
        for (let k = Math.max(0, c - anchoEnv + 1); k <= c; k++) { s += e2[k]; n += en[k]; }
        env[c] = n ? Math.sqrt(s / n) : (c ? env[c - 1] : 0);
      }

      for (let c = 0; c < cols; c++) {
        if (!fl[c]) continue;
        ctx.fillStyle = fl[c] === 2 ? css('--green-bg') : css('--amber-bg');
        ctx.fillRect(L + c, T, 1, H);
      }
      axes(ctx, L, T, W, H, this.win, [-1, -0.5, 0, 0.5, 1].map(f => [Y(f * this.ymax), (f * this.ymax).toFixed(this.ymax < 10 ? 2 : 0)]), 'mV');
      ctx.strokeStyle = css('--signal'); ctx.globalAlpha = 0.75; ctx.beginPath();
      for (let c = 0; c < cols; c++) { ctx.moveTo(L + c + 0.5, Y(mn[c])); ctx.lineTo(L + c + 0.5, Y(mx[c]) - 0.5); }
      ctx.stroke(); ctx.globalAlpha = 1;
      ctx.strokeStyle = css('--star'); ctx.lineWidth = 2.5; ctx.lineJoin = 'round'; ctx.beginPath();
      for (let c = 0; c < cols; c++) { const y = Y(env[c]); if (c) ctx.lineTo(L + c + 0.5, y); else ctx.moveTo(L + 0.5, y); }
      ctx.stroke(); ctx.lineWidth = 1;
    }
  }

  function axes(ctx, L, T, W, H, win, ticks, unit) {
    ctx.strokeStyle = css('--grid'); ctx.fillStyle = css('--muted'); ctx.font = '11px system-ui';
    ctx.textAlign = 'right'; ctx.textBaseline = 'middle'; ctx.lineWidth = 1;
    for (const [y, label] of ticks) { ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + W, y); ctx.stroke(); ctx.fillText(label, L - 6, y); }
    ctx.save(); ctx.translate(12, T + H / 2); ctx.rotate(-Math.PI / 2); ctx.textAlign = 'center';
    ctx.fillText(unit, 0, 0); ctx.restore();
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    for (let s = 0; s <= win; s += win > 20 ? 10 : 5) {
      const x = L + W - (s / win) * W;
      ctx.fillText(s === 0 ? '0 s' : '-' + s + ' s', x, T + H + 6);
    }
  }

  /* RMS que recibe el atril (un valor cada 100 ms) y su mediana de 0.5 s (la que se compara con los umbrales).
   * flag por bloque: 1 = sobre el umbral (activación), 2 = bloque usado por la calibración basal. */
  class RmsChart {
    constructor(canvas) { this.c = canvas; this.ymax = 50; this.lines = {}; this.setWindow(15); }
    setWindow(s) { this.win = s; this.N = Math.round(s * 10); this.v = new Float32Array(this.N).fill(NaN); this.m = new Float32Array(this.N).fill(NaN); this.f = new Uint8Array(this.N); this.w = 0; }
    push(v, flag, med) { this.v[this.w] = v; this.m[this.w] = med === undefined ? NaN : med; this.f[this.w] = flag; if (++this.w >= this.N) this.w = 0; }
    draw() {
      const { ctx, w, h } = fit(this.c);
      if (!w || !h) return;
      const L = 52, R = 10, T = 8, B = 24, W = w - L - R, H = h - T - B, N = this.N;
      ctx.clearRect(0, 0, w, h);
      let target = 20;
      for (let i = 0; i < N; i++) if (this.v[i] > target) target = this.v[i];
      for (const k in this.lines) if (this.lines[k] > target) target = this.lines[k];
      target *= 1.15;
      this.ymax = target > this.ymax ? target : this.ymax - (this.ymax - target) * 0.05;
      const X = i => L + (i + 0.5) * W / N, Y = v => T + H - (v / this.ymax) * H, bw = Math.max(1, W / N);
      for (let i = 0; i < N; i++) {
        const f = this.f[(this.w + i) % N];
        if (!f) continue;
        ctx.fillStyle = f === 1 ? css('--green-bg') : css('--blue-bg');
        ctx.fillRect(L + i * W / N, T, bw + 0.5, H);
      }
      axes(ctx, L, T, W, H, this.win, [0, 0.25, 0.5, 0.75, 1].map(f => [Y(f * this.ymax), Math.round(f * this.ymax)]), 'mV');
      const dash = (v, color, d) => {
        if (!(v > 0)) return;
        ctx.setLineDash(d); ctx.strokeStyle = color; ctx.lineWidth = 1.5;
        ctx.beginPath(); ctx.moveTo(L, Y(v)); ctx.lineTo(L + W, Y(v)); ctx.stroke(); ctx.setLineDash([]);
      };
      dash(this.lines.reposo, css('--muted'), [2, 3]);
      dash(this.lines.uoff, css('--amber'), [3, 3]);
      dash(this.lines.uon, css('--amber'), [8, 4]);
      const linea = (arr, color, width) => {
        ctx.strokeStyle = color; ctx.lineWidth = width; ctx.beginPath();
        let pen = false;
        for (let i = 0; i < N; i++) {
          const v = arr[(this.w + i) % N];
          if (!isFinite(v)) { pen = false; continue; }
          pen ? ctx.lineTo(X(i), Y(v)) : ctx.moveTo(X(i), Y(v)); pen = true;
        }
        ctx.stroke();
      };
      linea(this.v, css('--signal'), 1.2);       // RMS de cada bloque
      linea(this.m, css('--primary'), 2.4);      // mediana de 0.5 s
      ctx.lineWidth = 1;
    }
  }

  /* Señal guardada de una sesión (RMS y mediana cada 100 ms) con sus umbrales y las activaciones sombreadas. */
  function dibujarSesion(canvas, datos, umbrales) {
    const { ctx, w, h } = fit(canvas);
    if (!w || !h) return;
    const L = 52, R = 10, T = 8, B = 24, W = w - L - R, H = h - T - B;
    ctx.clearRect(0, 0, w, h);
    if (!datos.length) return;
    const t0 = datos[0].t, t1 = Math.max(t0 + 1, datos[datos.length - 1].t);
    let ymax = Math.max(20, umbrales.uon || 0, ...datos.map(d => d.rms)) * 1.1;
    const X = t => L + (t - t0) / (t1 - t0) * W, Y = v => T + H - (v / ymax) * H;
    ctx.fillStyle = css('--green-bg');
    for (let i = 0; i < datos.length; i++) if (datos[i].act) ctx.fillRect(X(datos[i].t), T, Math.max(1, W / datos.length + 0.5), H);
    ctx.strokeStyle = css('--grid'); ctx.fillStyle = css('--muted'); ctx.font = '11px system-ui'; ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
    for (const f of [0, 0.25, 0.5, 0.75, 1]) { const y = Y(f * ymax); ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + W, y); ctx.stroke(); ctx.fillText(Math.round(f * ymax), L - 6, y); }
    ctx.textAlign = 'center'; ctx.textBaseline = 'top';
    const paso = [10, 30, 60, 120, 300, 600].find(p => (t1 - t0) / p <= 8) || 600;
    for (let s = Math.ceil(t0 / paso) * paso; s <= t1; s += paso) ctx.fillText(Math.floor(s / 60) + ':' + String(Math.round(s % 60)).padStart(2, '0'), X(s), T + H + 6);
    const linea = (k, color, ancho) => { ctx.strokeStyle = color; ctx.lineWidth = ancho; ctx.beginPath(); datos.forEach((d, i) => i ? ctx.lineTo(X(d.t), Y(d[k])) : ctx.moveTo(X(d.t), Y(d[k]))); ctx.stroke(); };
    linea('rms', css('--signal'), 1); linea('med', css('--primary'), 2);
    ctx.setLineDash([8, 4]); ctx.strokeStyle = css('--amber'); ctx.lineWidth = 1.5;
    for (const v of [umbrales.uon, umbrales.uoff]) if (v > 0) { ctx.beginPath(); ctx.moveTo(L, Y(v)); ctx.lineTo(L + W, Y(v)); ctx.stroke(); }
    ctx.setLineDash([]); ctx.lineWidth = 1;
  }

  /* Progreso entre sesiones: puntos + línea de tendencia (regresión lineal). */
  class ProgressChart {
    constructor(canvas, tip) {
      this.c = canvas; this.tip = tip; this.pts = []; this.unit = ''; this.pos = [];
      canvas.addEventListener('mousemove', e => this._hover(e));
      canvas.addEventListener('mouseleave', () => { tip.hidden = true; });
    }
    set(points, unit) { this.pts = points; this.unit = unit; this.draw(); }
    draw() {
      const { ctx, w, h } = fit(this.c);
      if (!w || !h) return;
      ctx.clearRect(0, 0, w, h);
      const L = 48, R = 16, T = 16, B = 34, W = w - L - R, H = h - T - B, p = this.pts;
      ctx.font = '12px system-ui'; ctx.fillStyle = css('--muted'); ctx.textAlign = 'center';
      if (p.length < 1) { ctx.fillText('Aún no hay sesiones guardadas para este paciente', w / 2, h / 2); return; }
      let lo = Math.min(0, ...p.map(q => q.y)), hi = Math.max(...p.map(q => q.y));
      if (hi <= lo) hi = lo + 1; hi *= 1.1;
      const X = i => L + (p.length === 1 ? W / 2 : (i / (p.length - 1)) * W), Y = v => T + H - ((v - lo) / (hi - lo)) * H;
      ctx.strokeStyle = css('--grid'); ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
      for (let k = 0; k <= 4; k++) {
        const v = lo + (hi - lo) * k / 4, y = Y(v);
        ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + W, y); ctx.stroke();
        ctx.fillText(v.toFixed(hi < 10 ? 1 : 0), L - 6, y);
      }
      ctx.textAlign = 'center'; ctx.textBaseline = 'top';
      p.forEach((q, i) => { if (p.length <= 8 || i % Math.ceil(p.length / 8) === 0) ctx.fillText(q.label, X(i), T + H + 8); });
      if (p.length >= 3) { // tendencia
        const n = p.length, mx = (n - 1) / 2, my = p.reduce((a, q) => a + q.y, 0) / n;
        let sxy = 0, sxx = 0; p.forEach((q, i) => { sxy += (i - mx) * (q.y - my); sxx += (i - mx) ** 2; });
        const m = sxy / sxx, b = my - m * mx;
        ctx.setLineDash([6, 5]); ctx.strokeStyle = css('--amber'); ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(X(0), Y(b)); ctx.lineTo(X(n - 1), Y(b + m * (n - 1))); ctx.stroke(); ctx.setLineDash([]);
      }
      ctx.strokeStyle = css('--primary'); ctx.lineWidth = 2.5; ctx.beginPath();
      p.forEach((q, i) => i ? ctx.lineTo(X(i), Y(q.y)) : ctx.moveTo(X(i), Y(q.y))); ctx.stroke();
      ctx.fillStyle = css('--primary'); this.pos = [];
      p.forEach((q, i) => { ctx.beginPath(); ctx.arc(X(i), Y(q.y), 5, 0, 7); ctx.fill(); this.pos.push({ x: X(i), y: Y(q.y), q }); });
      ctx.lineWidth = 1;
    }
    _hover(e) {
      const r = this.c.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      const near = this.pos.find(o => Math.hypot(o.x - x, o.y - y) < 12);
      if (!near) { this.tip.hidden = true; return; }
      this.tip.hidden = false; this.tip.textContent = near.q.label + ': ' + near.q.y.toFixed(1) + ' ' + this.unit;
      this.tip.style.left = near.x + 'px'; this.tip.style.top = (near.y - 34) + 'px';
    }
  }

  /* ACTIVACIÓN, como el Serial Plotter: el RMS que el Arduino manda al atril (SinArm, 1 cada 100 ms),
   * el nivel que compara el atril (mediana de 1.3 s) y el umbral vigente (de caída o de subida).
   * Sombreado: cuando el atril dice "cabeza abajo". Escala automática del mínimo al máximo, como el plotter. */
  class ActChart {
    constructor(canvas) { this.c = canvas; this.lo = 0; this.hi = 0; this.setWindow(15); }
    setWindow(s) { this.win = s; this.N = Math.round(s * 10); this.v = []; this.m = []; this.u = []; this.ab = []; }
    push(v, m, u, abajo) {
      this.v.push(v); this.m.push(m); this.u.push(u > 0 ? u : NaN); this.ab.push(abajo ? 1 : 0);
      if (this.v.length > this.N) { this.v.shift(); this.m.shift(); this.u.shift(); this.ab.shift(); }
    }
    draw() {
      const { ctx, w, h } = fit(this.c);
      if (!w || !h) return;
      const L = 52, R = 10, T = 8, B = 24, W = w - L - R, H = h - T - B, n = this.v.length;
      ctx.clearRect(0, 0, w, h);
      // escala: del mínimo al máximo (sin los picos sueltos), con el umbral siempre a la vista
      let lo = Infinity, hi = -Infinity;
      if (n) {
        const ord = this.v.slice().sort((a, b) => a - b), p98 = ord[Math.min(n - 1, Math.floor(n * 0.98))];
        lo = ord[0]; hi = p98;
        for (let i = 0; i < n; i++) { if (this.m[i] < lo) lo = this.m[i]; if (this.m[i] > hi) hi = this.m[i]; if (this.u[i] > hi) hi = this.u[i]; }
      }
      if (!(hi > lo)) { lo = 0; hi = 10; }
      const pad = Math.max(1, (hi - lo) * 0.15); lo = Math.max(0, lo - pad); hi = hi + pad;
      if (!this.hi) { this.lo = lo; this.hi = hi; }
      this.lo += (lo - this.lo) * 0.2; this.hi += (hi - this.hi) * 0.2;
      const Y = v => T + H - ((Math.max(this.lo, Math.min(this.hi, v)) - this.lo) / (this.hi - this.lo)) * H;
      const X = i => L + (i + this.N - n) * W / Math.max(1, this.N - 1);
      ctx.fillStyle = css('--amber-bg');
      const paso1 = W / Math.max(1, this.N - 1);
      for (let i = 0; i < n; i++) {                    // un rectángulo por cada tramo con la cabeza abajo
        if (!this.ab[i] || (i && this.ab[i - 1])) continue;
        let j = i; while (j + 1 < n && this.ab[j + 1]) j++;
        const x0 = Math.max(L, X(i) - paso1 / 2), x1 = Math.min(L + W, X(j) + paso1 / 2);
        ctx.fillRect(x0, T, x1 - x0, H);
      }
      const paso = (this.hi - this.lo) / 4, dec = this.hi - this.lo < 10 ? 1 : 0;
      axes(ctx, L, T, W, H, this.win, [0, 1, 2, 3, 4].map(k => [Y(this.lo + k * paso), (this.lo + k * paso).toFixed(dec)]), 'mV');
      const linea = (arr, color, ancho, guiones) => {
        ctx.strokeStyle = color; ctx.lineWidth = ancho; ctx.setLineDash(guiones || []); ctx.lineJoin = 'round'; ctx.beginPath();
        let dentro = false;
        for (let i = 0; i < n; i++) {
          const v = arr[i];
          if (!(v === v)) { dentro = false; continue; }
          if (dentro) ctx.lineTo(X(i), Y(v)); else { ctx.moveTo(X(i), Y(v)); dentro = true; }
        }
        ctx.stroke(); ctx.setLineDash([]); ctx.lineWidth = 1;
      };
      linea(this.u, css('--amber'), 1.5, [6, 4]);      // umbral vigente
      linea(this.v, css('--signal'), 1.6);             // RMS (como el Serial Plotter)
      linea(this.m, css('--star'), 2.6);               // lo que compara el atril
    }
  }

  EMG.ActChart = ActChart;
  EMG.LiveChart = LiveChart; EMG.RmsChart = RmsChart; EMG.ProgressChart = ProgressChart; EMG.dibujarSesion = dibujarSesion;
})();
