/**
 * Recibe las sesiones desde la página y las agrega a una hoja de Google Sheets.
 * Instalación: ver docs/GUIA.md (sección "Google Sheets").
 */
function doPost(e) {
  var d = JSON.parse(e.postData.contents);
  var ss = SpreadsheetApp.getActiveSpreadsheet();

  var ses = ss.getSheetByName('Sesiones') || ss.insertSheet('Sesiones');
  if (ses.getLastRow() === 0) {
    ses.appendRow(['paciente', 'gmfcs', 'fecha', 'duracion_s', 'activaciones', 'tiempo_activo_s', 'pct_activo',
      'max_contraccion_s', 'media_contraccion_s', 'rms_norm_medio_pct', 'mdf_medio_Hz', 'mnf_medio_Hz',
      'descartadas', 'electrodo', 'posicion', 'fisio', 'notas']);
  }
  var r = d.resumen;
  ses.appendRow([d.paciente, d.gmfcs, d.fecha, d.duracionS, r.activaciones, r.tiempoActivoS, r.pctActivo,
    r.maxContrS, r.medContrS, r.rmsNormMedio, r.mdfMedio, r.mnfMedio, r.descartadas,
    d.electrodo, d.posicion, d.fisio, d.notas]);

  var act = ss.getSheetByName('Activaciones') || ss.insertSheet('Activaciones');
  if (act.getLastRow() === 0) {
    act.appendRow(['paciente', 'fecha_sesion', 'activacion', 'inicio_s', 'fin_s', 'duracion_s',
      'rms_mV', 'mav_mV', 'mdf_Hz', 'mnf_Hz', 'rms_norm_pct']);
  }
  (d.activaciones || []).forEach(function (a) {
    act.appendRow([d.paciente, d.fecha, a.n, a.inicioS, a.finS, a.durS, a.rms, a.mav, a.mdf, a.mnf, a.rmsNorm]);
  });

  return ContentService.createTextOutput('ok');
}
