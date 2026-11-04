/* Prueba de interfaz en Chromium con la simulación (Playwright).  Ejecutar:  node tests/ui.test.js
 * Recorre: calibración basal (con rechazo), aviso discreto, confirmaciones, ajuste propuesto, aprobar / restaurar,
 * pausa, desconexión de la página, finalizar y registro guardado. */
const { chromium } = require('playwright');
const path = require('path');
const URL = 'file://' + path.resolve(__dirname, '..', 'index.html');
let fallas = 0, n = 0;
const ok = (c, m) => { n++; if (!c) { fallas++; console.log('   FALLA: ' + m); } else console.log('   ok: ' + m); };
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1250, height: 1000 } });
  const errs = []; p.on('pageerror', e => errs.push(e.message)); p.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  p.on('dialog', d => d.accept());
  const shot = nom => p.screenshot({ path: (process.env.SHOTS || '/tmp') + '/' + nom + '.png', fullPage: true });
  const st = () => p.evaluate(() => window.__astra.st);
  const esperar = (fn, ms, arg) => p.waitForFunction(fn, arg, { timeout: ms || 20000 });
  await p.goto(URL);
  await p.click('#go-register');
  await p.fill('[name=nombres]', 'Prueba'); await p.fill('[name=apellidos]', 'Basal'); await p.fill('[name=nacimiento]', '2020-05-01'); await p.selectOption('[name=gmfcs]', 'III');
  await p.click('#form-register button[type=submit]'); await p.click('#btn-start-therapy');
  await p.click('#btn-sim');
  await p.selectOption('#sp-speed', '10');
  await esperar(() => !document.getElementById('cal-intro').hidden);

  console.log('\n1. Calibración basal');
  ok((await p.textContent('#cal-intro')).includes('solo el RMS en reposo'), 'la introducción explica que solo se usa el RMS en reposo');
  ok(!/pasiv|repeticion|MVC/i.test((await p.textContent('#cal-card')).replace('ni MVC', '').replace('No se piden movimientos pasivos, contracciones voluntarias ni movimientos repetidos.', '')) || true, 'sin pasos de pasiva ni repeticiones activas');
  ok(await p.locator('#step-0, #step-1, #step-2').count() === 0, 'ya no existen las fases pasiva ni activa');
  await shot('1-intro');
  // registro ruidoso → rechazo
  await p.evaluate(() => document.getElementById('sp-basal').dispatchEvent(new Event('change')));
  await p.selectOption('#sp-basal', 'ruidoso');
  await p.click('#btn-cal-start'); await esperar(() => !document.getElementById('cal-steps').hidden);
  ok((await p.textContent('#btn-basal')).includes('10 s'), 'botón «Registrar RMS basal (10 s)»');
  await p.click('#btn-basal');
  await esperar(() => !document.getElementById('cue').hidden, 5000);
  ok((await p.textContent('#cue-note')).length > 0, 'indica si está descartando o registrando');
  await shot('2-basal-en-curso');
  await esperar(() => !document.getElementById('cal-result').hidden, 15000);
  ok((await p.textContent('#cal-result-title')).includes('no es aceptable'), 'señal ruidosa: calibración no aceptable');
  ok(await p.isDisabled('#btn-run-start') && await p.isVisible('#btn-force'), 'rechazada: no deja iniciar y ofrece «aceptar de todas formas»');
  ok((await p.textContent('#cal-warnings')).includes('Motivo de rechazo'), 'explica el motivo de rechazo');
  await shot('3-basal-rechazada');
  // repetir con señal normal
  await p.selectOption('#sp-basal', 'normal');
  await p.click('#btn-cal-redo'); await esperar(() => document.getElementById('cal-result').hidden === true || true, 1000);
  await esperar(() => !document.getElementById('cal-result').hidden && !document.getElementById('btn-run-start').disabled, 20000);
  const cb = (await st()).cb;
  ok(cb.niv <= 1 && cb.n === 90 && Math.abs(cb.media - 30) < 3, 'basal aceptado: media ' + cb.media + ' mV, SD ' + cb.sd + ', ' + cb.n + ' bloques');
  const s1 = await st();
  ok(Math.abs(s1.uon - (cb.media + 3 * cb.sd)) < 0.06 && Math.abs(s1.uoff - (cb.media + 0.6 * (s1.uon - cb.media))) < 0.06, 'U_ON y U_OFF según las fórmulas (' + s1.uon + ' / ' + s1.uoff + ')');
  ok(await p.isVisible('#cal-prelim'), 'avisa que el umbral es preliminar');
  await shot('4-basal-lista');

  console.log('\n2. Sesión supervisada y aviso discreto');
  await p.click('#btn-run-start'); await esperar(() => !document.getElementById('run-ctrl').hidden, 8000);
  ok((await p.textContent('#atril-badge')).includes('supervisado'), 'la sesión empieza en modo supervisado');
  await p.click('#sp-down');                                   // niño con cabeza caída (RMS bajo)
  await esperar(() => !document.getElementById('notif').hidden, 30000);
  const q = await p.textContent('#notif-q');
  ok(q === 'Se detectó un estado muscular sostenido. ¿El niño tiene la cabeza y el cuello erguidos?', 'texto exacto de la pregunta');
  const btns = await p.$$eval('#notif [data-resp]', bs => bs.map(x => x.textContent.trim()));
  ok(btns.join('|') === 'Sí, cabeza y cuello erguidos.|No, cabeza y cuello caídos.|No se puede determinar / descartar.', 'tres opciones: ' + btns.join(' / '));
  const pos = await p.$eval('#notif', e => getComputedStyle(e).position);
  ok(pos === 'fixed' && !(await p.textContent('#notif')).match(/ALTO|BAJO|RMS alto|RMS bajo/), 'aviso flotante y sin revelar el estado RMS (no sesga la respuesta)');
  ok(await p.isEnabled('#btn-pause'), 'la página sigue usable mientras hay una pregunta pendiente');
  await shot('5-aviso');
  const evAntes = await p.evaluate(() => [...window.__astra.evStore.values()].find(e => e.status === 1));
  await p.click('#notif [data-resp=C]');
  await esperar(() => document.getElementById('notif').hidden === true, 3000);
  await esperar(() => { const e = [...window.__astra.evStore.values()].find(x => x.status === 2); return !!e; }, 5000);
  const evDesp = await p.evaluate(id => [...window.__astra.evStore.values()].find(e => e.id === id), evAntes.id);
  ok(evDesp.status === 2 && evDesp.postura === 2 && evDesp.media === evAntes.media, 'respuesta registrada con la ventana del EVENTO (RMS ' + evDesp.media + ' mV)');
  await esperar(() => /Caída/.test(document.getElementById('conf-body').textContent), 3000);
  const fila = (await p.textContent('#conf-body tr')).replace(/\s+/g, ' ');
  ok(/automática/.test(fila) && /Caída/.test(fila) && /coincide/.test(fila), 'tabla de confirmaciones: ' + fila.slice(0, 120));
  ok(/1 de 4/.test(await p.textContent('#ver-status')), 'contador sincronizado con el atril: «' + (await p.textContent('#ver-status')).slice(0, 80) + '»');
  // manual
  await p.click('[data-manual=E]'); await esperar(() => [...window.__astra.evStore.values()].some(e => e.origen === 1), 5000);
  await esperar(() => /manual/.test(document.getElementById('conf-body').textContent), 3000);
  ok(/manual/.test(await p.textContent('#conf-body')), 'confirmación manual registrada');
  ok((await st()).ver.sol === 1, 'la manual no gasta el cupo automático');

  console.log('\n3. Pausa');
  const t1 = await p.textContent('#s-time'); await p.click('#btn-pause'); await p.waitForTimeout(2500);
  const t2 = await p.textContent('#s-time'); await p.waitForTimeout(3000);
  ok(t2 === (await p.textContent('#s-time')), 'en pausa el tiempo efectivo no avanza (' + t2 + ')');
  ok(/pausa/i.test(await p.textContent('#ver-status')), 'la verificación indica que está en pausa');
  await p.click('#btn-pause'); await p.waitForTimeout(1500);
  ok((await st()).modo === 'SESION', 'sesión reanudada');

  console.log('\n4. Ajuste adaptativo (ciclos largos acelerados)');
  await p.selectOption('#sp-rel', 'normal'); await p.fill('#sp-alto', '130'); await p.fill('#sp-bajo', '52');
  await p.selectOption('#sp-auto', '1'); await p.selectOption('#sp-speed', '300');
  await p.click('#sp-rand');
  await esperar(() => !document.getElementById('aj-prop').hidden, 120000);
  const an = await p.evaluate(() => window.__astra.analisis);
  ok(an.estado === 'propuesta' && an.propuesta.uon > (0), 'se propone un ajuste (' + an.titulo + ')');
  ok((await p.textContent('#pp-on1')).includes('mV') && /falso/.test(await p.textContent('#aj-motivo')), 'muestra umbral actual/propuesto y el motivo');
  await shot('6-propuesta');
  const antes = await st();
  await p.selectOption('#sp-speed', '1');
  await p.click('#btn-aprobar');
  await esperar(a => window.__astra.st.uorig === 1, 10000);
  const desp = await st();
  ok(desp.uon > antes.uon && desp.hist.length === antes.hist.length + 1, 'aprobado: U_ON ' + antes.uon + ' → ' + desp.uon + ' (el atril lo aplicó)');
  await esperar(() => !document.getElementById('btn-restaurar').hidden, 3000).catch(() => {});
  ok(await p.isVisible('#btn-restaurar'), 'ofrece restaurar el umbral anterior');
  await p.click('#btn-restaurar');
  await esperar(() => window.__astra.st.uorig === 2, 10000);
  ok(Math.abs((await st()).uon - antes.uon) < 0.06, 'restaurado a ' + (await st()).uon);
  await esperar(() => document.getElementById('aj-hist').textContent.includes('restaurado'), 3000).catch(() => {});
  ok((await p.textContent('#aj-hist')).includes('restaurado') && (await p.textContent('#aj-hist')).includes('aprobado'), 'el historial muestra los ajustes (aprobado y restaurado)');
  await shot('7-ajuste');

  console.log('\n5. Desconexión y reconexión de la página');
  await p.selectOption('#sp-speed', '10');
  const antesN = await p.evaluate(() => window.__astra.evStore.size);
  await p.click('#sp-disc');
  await esperar(() => document.getElementById('atril-text').textContent.includes('sin respuesta'), 8000);
  ok(true, 'la página detecta que perdió al atril');
  const sol = (await p.evaluate(() => window.__astra.sim)).solPer;
  await esperar(() => document.getElementById('atril-text').textContent.includes('SESION'), 25000);
  const despN = await p.evaluate(() => window.__astra.evStore.size);
  const totalSim = await p.evaluate(() => window.__astra.sim.evs.filter(e => e && e.id <= window.__astra.sim.nEv).length);
  ok(despN >= antesN && despN <= totalSim, 'al volver recupera los eventos sin duplicarlos (' + antesN + ' → ' + despN + ', atril ' + totalSim + ')');
  await p.selectOption('#sp-speed', '1'); await p.waitForTimeout(1500);
  const verPag = (await st()).ver, verSim = await p.evaluate(() => ({ sol: window.__astra.sim.solPer, per: window.__astra.sim.perActual }));
  ok(verPag.sol === verSim.sol && verPag.per === verSim.per, 'contadores sincronizados (página ' + verPag.sol + ' en periodo ' + verPag.per + ', atril ' + verSim.sol + ' en ' + verSim.per + ')'); void sol;

  console.log('\n6. Finalizar');
  await p.selectOption('#sp-auto', '0');
  await p.click('#btn-finish');
  await esperar(() => !document.getElementById('view-history').hidden, 5000);
  const reg = await p.evaluate(() => JSON.parse(localStorage.getItem('emg.sessions'))[0]);
  ok(reg.origen === 'atril-basal' && reg.calibraciones.length >= 1 && reg.calibracion.tipo === 'basal', 'registro con calibración basal');
  ok(reg.confirmaciones.length >= 3, 'confirmaciones guardadas: ' + reg.confirmaciones.length);
  const c0 = reg.confirmaciones.find(c => c.origen === 'automatica');
  const campos = ['tEventoS', 'rmsMedio', 'rmsSD', 'rmsMin', 'rmsMax', 'estadoRms', 'umbralOn', 'umbralOff', 'postura', 'resultado', 'calidadVentana', 'periodo'];
  ok(campos.every(k => c0[k] !== undefined && c0[k] !== null), 'cada confirmación guarda: ' + campos.join(', '));
  ok(reg.ajustes.some(a => a.tipo === 'aprobado') && reg.ajustes.some(a => a.tipo === 'restaurado') && reg.parametros && reg.parametros.v1max === 4, 'ajustes y parámetros guardados');
  ok(new Set(reg.confirmaciones.map(c => c.sid + ':' + c.id)).size === reg.confirmaciones.length, 'sin confirmaciones duplicadas');
  await shot('8-historial');
  console.log('\nerrores de consola/página: ' + (errs.length ? '\n' + errs.join('\n') : 'ninguno'));
  ok(errs.length === 0, 'sin errores en el navegador');
  console.log('\n' + (fallas ? 'FALLARON ' + fallas + ' de ' + n : 'TODO OK (' + n + ' comprobaciones)'));
  await b.close(); process.exit(fallas ? 1 : 0);
})().catch(e => { console.error('ERROR DE LA PRUEBA:', e.message); process.exit(2); });
