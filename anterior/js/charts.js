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

  /* Señal EMG + envolvente en tiempo real (buffer circular, mín/máx por columna de píxeles). */
  class LiveChart {
    constructor(canvas, fs) { this.c = canvas; this.fs = fs; this.ymax = 0.2; this.thr = null; this.setWindow(15); }
    setWindow(s) {
      this.win = s; this.N = Math.round(s * this.fs);
      this.sig = new Float32Array(this.N); this.env = new Float32Array(this.N);
      this.flag = new Uint8Array(this.N); this.w = 0;
    }
    push(x, env, flag) {
      this.sig[this.w] = x; this.env[this.w] = env; this.flag[this.w] = flag;
      if (++this.w >= this.N) this.w = 0;
    }
    draw() {
      const { ctx, w, h } = fit(this.c);
      if (!w || !h) return;
      const L = 52, R = 10, T = 8, B = 24, W = w - L - R, H = h - T - B;
      ctx.clearRect(0, 0, w, h);
      const cols = Math.max(1, Math.floor(W)), N = this.N;
      const mn = new Float32Array(cols), mx = new Float32Array(cols), ev = new Float32Array(cols), fl = new Uint8Array(cols);
      let target = 0.02;
      for (let c = 0; c < cols; c++) {
        const i0 = Math.floor(c * N / cols), i1 = Math.max(i0 + 1, Math.floor((c + 1) * N / cols));
        let lo = 0, hi = 0, f = 0, e = 0;
        for (let i = i0; i < i1; i++) {
          const j = (this.w + i) % N, v = this.sig[j];
          if (v < lo) lo = v; if (v > hi) hi = v;
          if (this.flag[j] > f) f = this.flag[j];
          e = this.env[j];
        }
        mn[c] = lo; mx[c] = hi; ev[c] = e; fl[c] = f;
        target = Math.max(target, -lo, hi, e);
      }
      target *= 1.15;
      this.ymax = target > this.ymax ? target : this.ymax - (this.ymax - target) * 0.03;
      const Y = v => T + H / 2 - (v / this.ymax) * (H / 2);

      // franjas de activación (verde = válida, ámbar = candidata)
      for (let c = 0; c < cols; c++) {
        if (!fl[c]) continue;
        ctx.fillStyle = fl[c] === 2 ? css('--green-bg') : css('--amber-bg');
        ctx.fillRect(L + c, T, 1, H);
      }
      // rejilla y ejes
      ctx.strokeStyle = css('--grid'); ctx.fillStyle = css('--muted'); ctx.font = '11px system-ui';
      ctx.textAlign = 'right'; ctx.textBaseline = 'middle'; ctx.lineWidth = 1;
      for (const f of [-1, -0.5, 0, 0.5, 1]) {
        const y = Y(f * this.ymax); ctx.beginPath(); ctx.moveTo(L, y); ctx.lineTo(L + W, y); ctx.stroke();
        ctx.fillText((f * this.ymax).toFixed(this.ymax < 1 ? 2 : 1), L - 6, y);
      }
      ctx.save(); ctx.translate(12, T + H / 2); ctx.rotate(-Math.PI / 2); ctx.textAlign = 'center';
      ctx.fillText('mV', 0, 0); ctx.restore();
      ctx.textAlign = 'center'; ctx.textBaseline = 'top';
      for (let s = 0; s <= this.win; s += this.win > 20 ? 10 : 5) {
        const x = L + W - (s / this.win) * W;
        ctx.fillText(s === 0 ? '0 s' : '-' + s + ' s', x, T + H + 6);
      }
      // señal filtrada
      ctx.strokeStyle = css('--signal'); ctx.beginPath();
      for (let c = 0; c < cols; c++) { ctx.moveTo(L + c + 0.5, Y(mn[c])); ctx.lineTo(L + c + 0.5, Y(mx[c]) - 0.5); }
      ctx.stroke();
      // umbral
      if (this.thr) {
        ctx.setLineDash([6, 4]); ctx.strokeStyle = css('--amber'); ctx.beginPath();
        for (const s of [1, -1]) { const y = Y(s * this.thr); ctx.moveTo(L, y); ctx.lineTo(L + W, y); }
        ctx.stroke(); ctx.setLineDash([]);
      }
      // envolvente (espejada)
      ctx.strokeStyle = css('--primary'); ctx.lineWidth = 2.5;
      for (const s of [1, -1]) {
        ctx.beginPath();
        for (let c = 0; c < cols; c++) { const y = Y(s * ev[c]); c ? ctx.lineTo(L + c, y) : ctx.moveTo(L + c, y); }
        ctx.stroke();
      }
      ctx.lineWidth = 1;
    }
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

  EMG.LiveChart = LiveChart; EMG.ProgressChart = ProgressChart;
})();
