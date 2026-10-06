/* Calibración en 3 fases: reposo, pasiva y activa (con repeticiones).
 * Usa el reloj de las muestras (n/fs), no el del reloj de pared. */
(function () {
  'use strict';
  const EMG = (window.EMG = window.EMG || {});

  const CUES = {
    rest:    { title: 'REPOSO', text: 'Cabeza apoyada y relajada. No mover al niño.' },
    passive: { title: 'PASIVA', text: 'El terapeuta mueve la cabeza de abajo hacia arriba y viceversa. El niño NO hace fuerza.' },
    up:      { title: '¡ARRIBA!', text: 'El niño levanta la cabeza y la sostiene.' },
    down:    { title: 'ABAJO', text: 'Baja la cabeza y relaja.' }
  };

  class Calibrator {
    constructor(cfg, fs) {
      this.fs = fs; this.cfg = cfg;
      this.steps = [
        { phase: 'rest', cue: 'rest', dur: cfg.restS },
        { phase: 'passive', cue: 'passive', dur: cfg.passiveS }
      ];
      for (let r = 1; r <= cfg.reps; r++) {
        this.steps.push({ phase: 'active', cue: 'up', dur: cfg.holdS, rep: r });
        this.steps.push({ phase: 'active', cue: 'down', dur: cfg.relaxS, rep: r });
      }
      this.total = this.steps.reduce((a, s) => a + s.dur, 0);
      this.idx = 0; this.tIn = 0; this.elapsed = 0; this.done = false; this.result = null;
      this.rest = { n: 0, mean: 0, m2: 0, sumsq: 0 };
      this.pas = { n: 0, sum: 0, max: 0 };
      this.act = { sumsq: 0, n: 0, envSum: 0 };
      this.repRms = []; this.repEnv = [];
    }

    feed(x, env) {
      if (this.done) return;
      const st = this.steps[this.idx], dt = 1 / this.fs;
      this.tIn += dt; this.elapsed += dt;
      if (st.phase === 'rest' && this.tIn >= 1) {            // descarta 1 s de transitorio del filtro
        const r = this.rest; r.n++;
        const d = env - r.mean; r.mean += d / r.n; r.m2 += d * (env - r.mean);
        r.sumsq += x * x;
      } else if (st.phase === 'passive' && this.tIn >= 0.5) {
        this.pas.n++; this.pas.sum += env; if (env > this.pas.max) this.pas.max = env;
      } else if (st.cue === 'up' && this.tIn >= 0.7 && this.tIn <= st.dur - 0.3) {
        // ventana estable: evita el artefacto de la transición abajo->arriba
        this.act.n++; this.act.sumsq += x * x; this.act.envSum += env;
      }
      if (this.tIn >= st.dur) {
        if (st.cue === 'up' && this.act.n > 0) {
          this.repRms.push(Math.sqrt(this.act.sumsq / this.act.n));
          this.repEnv.push(this.act.envSum / this.act.n);
          this.act = { sumsq: 0, n: 0, envSum: 0 };
        }
        this.idx++; this.tIn = 0;
        if (this.idx >= this.steps.length) this._finish();
      }
    }

    _finish() {
      this.done = true;
      const r = this.rest, mean = a => a.reduce((s, v) => s + v, 0) / (a.length || 1);
      const res = {
        restMean: r.mean,
        restSD: r.n > 1 ? Math.sqrt(r.m2 / (r.n - 1)) : 0,
        restRms: r.n ? Math.sqrt(r.sumsq / r.n) : 0,
        passiveMean: this.pas.n ? this.pas.sum / this.pas.n : 0,
        passivePeak: this.pas.max,
        activeRms: mean(this.repRms),
        activeEnv: mean(this.repEnv),
        repRms: this.repRms.slice(),
        fecha: new Date().toISOString()
      };
      res.snr = res.restRms > 0 ? res.activeRms / res.restRms : 0;
      res.artifactPct = res.activeEnv > 0 ? 100 * res.passivePeak / res.activeEnv : 0;
      res.warnings = [];
      if (!this.repRms.length || res.activeRms <= 0) res.warnings.push('No se detectó contracción activa. Revisa electrodos y repite.');
      else if (res.snr < 3) res.warnings.push('La contracción activa apenas se distingue del reposo (SNR ' + res.snr.toFixed(1) + '). Revisa electrodos.');
      if (res.artifactPct > 80) res.warnings.push('El movimiento pasivo produce una señal casi tan grande como la activa (' + res.artifactPct.toFixed(0) + '%). Las transiciones podrían contarse como activaciones; sube el umbral o la duración mínima.');
      this.result = res;
    }

    info() {
      const st = this.steps[Math.min(this.idx, this.steps.length - 1)];
      const phases = {};
      for (const p of ['rest', 'passive', 'active']) {
        const list = this.steps.map((s, i) => ({ s, i })).filter(o => o.s.phase === p);
        const dur = list.reduce((a, o) => a + o.s.dur, 0);
        let left = 0, state = 'pending';
        for (const o of list) {
          if (o.i > this.idx) left += o.s.dur;
          else if (o.i === this.idx) left += o.s.dur - this.tIn;
        }
        if (this.done || list[list.length - 1].i < this.idx) state = 'done';
        else if (list[0].i <= this.idx) state = 'active';
        phases[p] = { dur, left: state === 'done' ? 0 : left, state };
      }
      return {
        done: this.done, phase: st.phase, cue: st.cue, rep: st.rep || 0, reps: this.cfg.reps,
        cueTitle: CUES[st.cue].title, cueText: CUES[st.cue].text,
        stepLeft: st.dur - this.tIn, progress: Math.min(1, this.elapsed / this.total), phases
      };
    }
  }

  function deriveThresholds(cal, S) {
    const onThr = Math.max(cal.restMean + S.thrSD * cal.restSD, (S.thrPct / 100) * cal.activeEnv);
    return { onThr, offThr: 0.6 * onThr };
  }

  EMG.Calibrator = Calibrator;
  EMG.deriveThresholds = deriveThresholds;
})();
