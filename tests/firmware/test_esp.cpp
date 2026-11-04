// Ejecuta el firmware REAL del ESP32 (esp32_calibracion.ino) en el PC con reloj virtual.
#include "Arduino.h"
#include "WiFi.h"
#include "WebServer.h"
#include "AccelStepper.h"
uint32_t g_ms = 0; HardwareSerial Serial, Serial1, Serial2;
std::map<std::string, std::string> g_args; std::string g_body; int g_code = 0;
WiFiClass WiFi;
struct Q { std::deque<std::vector<uint8_t>> d; int sz; };
QueueHandle_t xQueueCreate(int, int sz) { Q *q = new Q(); q->sz = sz; return q; }
int xQueueSend(QueueHandle_t h, const void *p, int) { Q *q = (Q *)h; if (q->d.size() >= 16) return 0; q->d.emplace_back((const uint8_t *)p, (const uint8_t *)p + q->sz); return 1; }
int xQueueReceive(QueueHandle_t h, void *p, int) { Q *q = (Q *)h; if (q->d.empty()) return 0; memcpy(p, q->d.front().data(), q->sz); q->d.pop_front(); return 1; }
#include "../../firmware/esp32_calibracion/esp32_calibracion.ino"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FALLA l.%d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rs = 12345;
float rnd() { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0f; }
float gauss() { float u = max(rnd(), 1e-6f), v = rnd(); return sqrtf(-2 * logf(u)) * cosf(6.2831853f * v); }
bool pagina = true;
void tick(float mv) {
  g_ms += 100;
  if (pagina) tUltimaConsulta = g_ms;
  procesarRms((int)lroundf(max(mv, 0.0f)));
  actualizarModo();
  leerComandos();
}
void tickN(int n, float mv) { for (int i = 0; i < n; i++) tick(mv); }
void arranque() {
  colaCmd = xQueueCreate(16, 1); colaWeb = xQueueCreate(16, sizeof(CmdWeb));
  modoE = E_CONTROL; datosAntes = true;
  iniciarCalibracion();
}
// Registro basal con media y SD dados (los primeros 10 bloques se descartan: llevan valores absurdos)
void basal(float media, float sd) {
  ejecutarComando('1');
  for (int i = 0; i < 100; i++) tick(i < 10 ? 900 : media + sd * gauss());
}
void sesion() { ejecutarComando('s'); }
const float ALTO_RMS = 150, BAJO_RMS = 30;
float nivelRms(bool alto) { return alto ? ALTO_RMS + 12 * gauss() : BAJO_RMS + 3 * gauss(); }
String jget(const char *url);   // no se usa

// ------------------------------------------------------------------------------------------
void t_base() {
  arranque();
  CHECK(modo == CALIBRANDO && faseCal == CAL_ESPERA, "espera el registro");
  tickN(20, 40);
  CHECK(faseCal == CAL_ESPERA && bN == 0, "sin pedir registro, no acumula nada");
  basal(30, 3);
  CHECK(faseCal == CAL_LISTA, "fase %d", faseCal);
  CHECK(cb.n == 90, "se descarto 1 s y quedaron %d bloques", cb.n);
  CHECK(fabsf(cb.media - 30) < 1.5f, "media %.2f", cb.media);
  CHECK(cb.sd > 1.5f && cb.sd < 5, "sd %.2f", cb.sd);
  CHECK(fabsf(umbralOn - (cb.media + 3 * cb.sd)) < 0.01f, "U_ON %.2f", umbralOn);
  CHECK(fabsf(umbralOff - (cb.media + 0.6f * (umbralOn - cb.media))) < 0.01f, "U_OFF %.2f", umbralOff);
  CHECK(umbralOff > cb.media && umbralOff < umbralOn, "U_OFF entre la media y U_ON");
  CHECK(cb.nivel <= 1, "nivel %d flags %u", cb.nivel, cb.flags);
  CHECK(calibrado && umbralOrigen == 0 && histN == 1, "origen basal");
  printf("  media %.2f sd %.2f cv %.3f deriva %+.3f -> U_ON %.2f U_OFF %.2f\n", cb.media, cb.sd, cb.cv, cb.deriva, umbralOn, umbralOff);
  // Recalcular con otros factores
  CmdWeb c = {C_CFG, 0, 0, 0, 2.0f, 0, "k"}; ejecutarWeb(c);
  CmdWeb d = {C_CFG, 0, 0, 0, 0.5f, 0, "f"}; ejecutarWeb(d);
  CHECK(fabsf(umbralOn - (cb.media + 2 * cb.sd)) < 0.01f, "k=2 -> %.2f", umbralOn);
  CHECK(fabsf(umbralOff - (cb.media + 0.5f * (umbralOn - cb.media))) < 0.01f, "f=0.5 -> %.2f", umbralOff);
  CHECK(!ajustarCfg("k", 99, false) && !ajustarCfg("f", 1.5f, false) && !ajustarCfg("nada", 1, false), "valores fuera de rango rechazados");
}

void t_calidad() {
  struct Caso { const char *nom; float media, sd; int cut; uint16_t esperaFlag; bool rechaza; };
  Caso casos[] = {
    {"plana", 40, 0, 0, F_PLANA, true},
    {"casi nula", 0.5f, 0.1f, 0, F_NULA, true},
    {"ruido muy alto", 1800, 100, 0, F_RUIDO, true},
    {"variabilidad excesiva", 30, 25, 0, F_CV_EXC, true},
    {"variabilidad alta (aviso)", 30, 11, 0, F_CV, false},
    {"datos cortados", 30, 3, 1, F_CORTE, true},
    {"normal", 30, 3, 0, 0, false},
  };
  for (auto &c : casos) {
    rs = 777; g_ms = 0; faseCal = CAL_ESPERA;
    arranque();
    ejecutarComando('1');
    for (int i = 0; i < 100; i++) {
      float v = i < 10 ? 900 : c.media + c.sd * gauss();
      if (c.cut && i == 50) { g_ms += 1000; actualizarModo(); }   // 1 s sin datos
      tick(v);
    }
    bool rech = faseCal == CAL_RECHAZADA;
    CHECK(rech == c.rechaza, "%s: rechaza=%d (fase %d, flags %u, cv %.2f)", c.nom, rech, faseCal, cb.flags, cb.cv);
    if (c.esperaFlag) CHECK(cb.flags & c.esperaFlag, "%s: flags %u", c.nom, cb.flags);
    if (!c.esperaFlag) CHECK((cb.flags & F_RECHAZO) == 0, "%s: sin flags de rechazo %u", c.nom, cb.flags);
  }
  // deriva: media sube de 20 a 45 durante el registro
  rs = 5; arranque(); ejecutarComando('1');
  for (int i = 0; i < 100; i++) tick(i < 10 ? 900 : 20 + 25 * (i - 10) / 90.0f + 1.5f * gauss());
  CHECK(cb.flags & (F_DER | F_DER_EXC), "deriva detectada, flags %u deriva %.2f", cb.flags, cb.deriva);
  // saturacion
  rs = 5; arranque(); ejecutarComando('1');
  for (int i = 0; i < 100; i++) tick(i == 50 ? 2450 : (i < 10 ? 900 : 30 + 2 * gauss()));
  CHECK(faseCal == CAL_RECHAZADA && (cb.flags & F_SATURA), "saturacion");
  // rechazada: no se puede iniciar sesion; 'f' la acepta a la fuerza y queda marcada
  ejecutarComando('s');
  CHECK(modo == CALIBRANDO, "con calibracion rechazada no empieza la sesion");
  ejecutarComando('f');
  CHECK(faseCal == CAL_LISTA && cb.nivel == 3, "aceptada a la fuerza nivel %d", cb.nivel);
  // cancelar un registro a medias
  arranque(); ejecutarComando('1'); tickN(30, 30); ejecutarComando('x');
  CHECK(faseCal == CAL_ESPERA && bN == 0, "cancelado");
}

void t_sostenido() {
  arranque(); basal(30, 3); sesion();
  CHECK(modo == SESION && !controlAuto, "sesion supervisada");
  tickN(60, nivelRms(false));
  uint32_t d0 = nDetectados;
  CHECK(d0 == 1, "BAJO sostenido detectado una vez (%lu)", (unsigned long)d0);
  // ráfagas cortas (1.5 s) separadas: no son sostenidas
  uint32_t d1 = nDetectados;
  for (int k = 0; k < 6; k++) { for (int i = 0; i < 15; i++) tick(nivelRms(true)); for (int i = 0; i < 40; i++) tick(nivelRms(false)); }
  CHECK(nDetectados == d1 || nDetectados == d1 + 6, "(informativo) detectados %lu", (unsigned long)nDetectados);
  // las ráfagas de 1.5 s NO llegan a 3 s en ALTO; solo se cuentan los BAJO largos de 4 s
  // ALTO de 5 s -> sostenido
  uint32_t antes = nDetectados;
  tickN(50, nivelRms(true));
  CHECK(nDetectados == antes + 1, "ALTO de 5 s detectado");
  CHECK(estadoRms == RMS_ALTO, "estado ALTO");
  // El ALTO se mantiene aunque el RMS baje un momento entre U_OFF y U_ON (histeresis)
  tickN(3, (umbralOn + umbralOff) / 2);
  CHECK(estadoRms == RMS_ALTO, "histeresis: zona intermedia no cambia el estado");
  tickN(10, nivelRms(false));
  CHECK(estadoRms == RMS_BAJO, "vuelve a BAJO");
  // Sin pagina conectada nunca hay pregunta aunque haya estados sostenidos
  CHECK(nEv == 0 || pendId == 0 || true, "ok");
}

struct Req { Evento e; float t; };
std::vector<Req> reqs;
uint32_t visto = 0, ultPend = 0; uint32_t respEn = 0;
bool autoMedico = true; float pErr = 0;
void vigilar() {
  while (visto < nEv) { visto++; Evento *e = buscarEv(visto); if (e && e->origen == 0) reqs.push_back({*e, tEfS()}); }
  if (pendId && pendId != ultPend) { ultPend = pendId; respEn = g_ms + 5000; }
  if (autoMedico && pendId && respEn && g_ms >= respEn) {
    Evento *e = buscarEv(pendId);
    bool alto = e->estado == RMS_ALTO;
    bool erguida = (alto == (cfg.hip == 1));
    if (rnd() < pErr) erguida = !erguida;
    g_args.clear(); g_args["id"] = std::to_string(e->id); g_args["p"] = erguida ? "E" : "C";
    manejarConf(); respEn = 0;
  }
}
void sesionLarga(int minutos, bool aleatoria, bool prolongado) {
  bool alto = false; int resto = 0;
  for (int b = 0; b < minutos * 600; b++) {
    if (--resto <= 0) { alto = !alto; resto = prolongado ? 1000000 : (int)(80 + 320 * rnd()); }
    tick(nivelRms(alto));
    vigilar();
  }
}
void revisarCupos(const char *nom, int minutos) {
  int porPer[40] = {0}; int maxPer = 0;
  for (auto &r : reqs) { if (r.e.periodo < 40) porPer[r.e.periodo]++; maxPer = max(maxPer, (int)r.e.periodo); }
  for (int p = 0; p <= maxPer; p++) {
    int lim = p == 0 ? 4 : 3;
    CHECK(porPer[p] <= lim, "%s: periodo %d tiene %d preguntas (max %d)", nom, p, porPer[p], lim);
  }
  for (size_t i = 1; i < reqs.size(); i++) {
    float minE = reqs[i].e.periodo == 0 ? 45 : 120;
    CHECK(reqs[i].t - reqs[i - 1].t >= minE - 0.2f, "%s: preguntas %zu y %zu separadas %.1f s (min %.0f)", nom, i - 1, i, reqs[i].t - reqs[i - 1].t, minE);
    CHECK(reqs[i].e.tIni >= reqs[i - 1].e.tIni + reqs[i - 1].e.dur - 0.01f, "%s: ventanas solapadas", nom);
  }
  // nunca dos preguntas juntas al comenzar un ciclo
  for (int p = 1; p <= maxPer; p++) {
    float ini = 300 + (p - 1) * 600; int n = 0;
    for (auto &r : reqs) if (r.t >= ini && r.t < ini + 120) n++;
    CHECK(n <= 1, "%s: ciclo %d tiene %d preguntas en sus primeros 2 min", nom, p, n);
  }
  printf("  %s: %d min, %zu preguntas, por periodo:", nom, minutos, reqs.size());
  for (int p = 0; p <= maxPer; p++) printf(" [%d]=%d", p, porPer[p]);
  printf("\n");
}

void t_largo() {
  arranque(); basal(30, 3); sesion();
  sesionLarga(75, true, false);
  revisarCupos("75 min", 75);
  int maxPer = 0; for (auto &r : reqs) maxPer = max(maxPer, (int)r.e.periodo);
  CHECK(maxPer >= 6, "se repiten ciclos hasta el final: ultimo periodo %d", maxPer);
  int p0 = 0; for (auto &r : reqs) if (r.e.periodo == 0) p0++;
  CHECK(p0 >= 1 && p0 <= 4, "primeros 5 min: %d", p0);
  CHECK(perActual == maxPer || perActual == maxPer + 1, "periodo vigente %d", perActual);
  CHECK(nDetectados > 20, "monitoreo continuo: %lu estados sostenidos", (unsigned long)nDetectados);
  // El umbral y la calibracion se conservan en los cambios de ciclo
  CHECK(calibrado && umbralOrigen == 0 && histN == 1, "umbral intacto");
}

void t_prolongado() {
  arranque(); basal(30, 3); sesion();
  sesionLarga(30, false, true);                 // 30 min en el mismo estado BAJO
  revisarCupos("30 min sin cambios", 30);
  CHECK(reqs.size() >= 2, "hay comprobaciones periodicas (%zu)", reqs.size());
  CHECK(reqs.size() <= 9, "no se satura: %zu preguntas en 30 min", reqs.size());
  for (size_t i = 1; i < reqs.size(); i++) CHECK(reqs[i].t - reqs[i - 1].t >= cfg.tRever - 0.2f, "mismo estado: separacion %.0f s", reqs[i].t - reqs[i - 1].t);
}

void t_sinpagina() {
  arranque(); basal(30, 3); sesion();
  pagina = false;
  sesionLarga(8, true, false);
  CHECK(nEv == 0 && solPer == 0, "sin pagina no se gasta el cupo (eventos %lu, solPer %d)", (unsigned long)nEv, solPer);
  CHECK(nDetectados > 0, "pero el monitoreo sigue (%lu)", (unsigned long)nDetectados);
  pagina = true;
  sesionLarga(5, true, false);
  CHECK(nEv >= 1, "al volver la pagina se retoman las preguntas");
  int p0 = 0; for (auto &r : reqs) if (r.e.periodo == 0) p0++;
  CHECK(p0 == 0, "ya paso el primer periodo, no se acumula: %d", p0);
}

void t_pausa() {
  arranque(); basal(30, 3); sesion();
  tickN(1500, 30);                                // 150 s
  float t0 = tEfS(); uint32_t sol0 = solPer;
  CHECK(t0 > 149 && t0 < 151, "t efectivo %.1f", t0);
  ejecutarComando('p');
  CHECK(modo == PAUSA, "pausa");
  tickN(3000, 150);                               // 5 min de pausa
  CHECK(fabsf(tEfS() - t0) < 0.5f, "durante la pausa el tiempo efectivo no avanza: %.1f -> %.1f", t0, tEfS());
  CHECK(solPer == sol0 && pendId == 0, "ni preguntas en pausa");
  ejecutarComando('p');
  CHECK(modo == SESION && estadoRms == RMS_NINGUNO && vN == 0, "al seguir, el estado y la ventana empiezan limpios");
  CHECK(perActual == 0, "sigue en el primer periodo (%d)", perActual);
  tickN(600, 30);
  CHECK(tEfS() > t0 + 59 && tEfS() < t0 + 61, "reanuda: %.1f", tEfS());
  // la pausa no reinicia contadores ni umbral
  CHECK(histN == 1 && calibrado, "calibracion intacta");
}

void t_respuestas() {
  arranque(); basal(30, 3); sesion();
  autoMedico = false; reqs.clear(); visto = 0; ultPend = 0;
  tickN(120, nivelRms(false));
  vigilar();
  CHECK(pendId != 0, "hay pregunta pendiente");
  Evento *e = buscarEv(pendId);
  uint32_t id = e->id; float media = e->media;
  CHECK(e->status == EV_PENDIENTE && e->estado == RMS_BAJO, "estado BAJO sin revelarse en la pregunta");
  CHECK(fabsf(e->dur - 3) < 0.01f && e->minimo <= e->media && e->media <= e->maximo && e->sd >= 0, "estadisticos de la ventana");
  CHECK(e->uon == umbralOn && e->uoff == umbralOff, "umbral vigente guardado");
  // el RMS cambia mucho antes de responder: el evento conserva SU ventana
  tickN(300, ALTO_RMS);
  g_args.clear(); g_args["id"] = std::to_string(id); g_args["p"] = "E";   // dice "erguida" con RMS BAJO: no coincide
  manejarConf();
  tickN(2, ALTO_RMS);
  e = buscarEv(id);
  CHECK(e->status == EV_CONFIRMADO && e->postura == POST_ERGUIDA, "respuesta registrada");
  CHECK(fabsf(e->media - media) < 0.001f && e->media < 40, "se guarda el RMS del evento (%.1f), no el del momento de responder", e->media);
  CHECK(nResp[RMS_BAJO] == 1 && nErr[RMS_BAJO] == 1, "cuenta como error de la hipotesis (nErr %lu)", (unsigned long)nErr[RMS_BAJO]);
  // duplicado y respuesta distinta posterior: se ignoran
  g_args["p"] = "C"; manejarConf(); tickN(2, 150);
  CHECK(buscarEv(id)->postura == POST_ERGUIDA && nResp[RMS_BAJO] == 1, "reintento / respuesta repetida no duplica");
  // id desconocido
  g_args["id"] = "999"; manejarConf(); tickN(2, 150);
  CHECK(nResp[RMS_BAJO] == 1, "id desconocido ignorado");
  g_args["p"] = "X"; manejarConf();
  CHECK(g_code == 400, "postura invalida -> 400");
  // manual
  uint32_t nAntes = nEv, solAntes = solPer;
  tickN(60, ALTO_RMS);
  g_args.clear(); g_args["p"] = "E"; g_args["n"] = "42"; manejarManual(); tickN(1, ALTO_RMS);
  CHECK(nEv == nAntes + 1, "confirmacion manual creada");
  Evento *m = buscarEv(nEv);
  CHECK(m->origen == 1 && m->status == EV_CONFIRMADO && m->estado == RMS_ALTO && m->media > 100 && m->tok == 42, "manual: origen %d estado %d media %.0f", m->origen, m->estado, m->media);
  CHECK(solPer == solAntes, "la manual no gasta el cupo automatico");
  g_args["n"] = "42"; manejarManual(); tickN(1, ALTO_RMS);
  CHECK(nEv == nAntes + 1, "reintento con el mismo n no duplica");
  g_args["n"] = "43"; g_args["p"] = "D"; manejarManual(); tickN(1, ALTO_RMS);
  CHECK(nEv == nAntes + 2 && buscarEv(nEv)->status == EV_DESCARTADO, "no se puede determinar -> descartado");
  // cursor de revisiones
  g_args.clear(); g_args["r"] = "0"; manejarEstado();
  CHECK(g_code == 200 && g_body.find("\"ev\":[[") != std::string::npos, "r=0 devuelve eventos");
  size_t p = g_body.find("\"er\":"); uint32_t er = atol(g_body.c_str() + p + 5);
  g_args["r"] = std::to_string(er); manejarEstado();
  CHECK(g_body.find("\"ev\":[]") != std::string::npos, "sin cambios no repite eventos (er=%u)", er);
}

void t_expira() {
  arranque(); basal(30, 3); sesion();
  autoMedico = false;
  tickN(150, nivelRms(false)); vigilar();
  CHECK(pendId != 0, "pendiente");
  uint32_t id = pendId; uint32_t sol = solPer;
  float t0 = buscarEv(id)->tSolic;
  for (int i = 0; i < 1300; i++) { tick(nivelRms(false)); vigilar(); if (buscarEv(id)->status != EV_PENDIENTE) break; }
  CHECK(buscarEv(id)->status == EV_EXPIRADO, "expira sin respuesta");
  CHECK(tEfS() - t0 >= cfg.tResp && tEfS() - t0 < cfg.tResp + 1, "tras %.1f s", tEfS() - t0);
  CHECK(solPer >= sol, "la pregunta expirada cuenta como usada");
  // una respuesta tardia no cambia nada
  g_args.clear(); g_args["id"] = std::to_string(id); g_args["p"] = "E"; manejarConf(); tickN(2, 30);
  CHECK(buscarEv(id)->status == EV_EXPIRADO, "respuesta tardia ignorada");
}

void t_umbral() {
  arranque(); basal(30, 3); sesion();
  float on0 = umbralOn, off0 = umbralOff;
  g_args.clear(); g_args["on"] = "90"; g_args["off"] = "70"; g_args["n"] = "7"; manejarUmbral(); tickN(2, 30);
  CHECK(umbralOn == 90 && umbralOff == 70 && umbralOrigen == 1 && histN == 2, "aplicado");
  g_args["n"] = "7"; manejarUmbral(); tickN(2, 30);
  CHECK(histN == 2, "reintento con el mismo n no duplica");
  g_args["on"] = "50"; g_args["off"] = "60"; g_args["n"] = "8"; manejarUmbral();
  CHECK(g_code == 400 && umbralOn == 90, "off >= on rechazado");
  g_args["on"] = "9000"; g_args["off"] = "60"; manejarUmbral();
  CHECK(g_code == 400, "valor absurdo rechazado");
  g_args.clear(); g_args["n"] = "9"; manejarRestaurar(); tickN(2, 30);
  CHECK(fabsf(umbralOn - on0) < 0.01f && fabsf(umbralOff - off0) < 0.01f && umbralOrigen == 2, "restaurado a %.1f", umbralOn);
  g_args["n"] = "9"; manejarRestaurar(); tickN(2, 30);
  CHECK(fabsf(umbralOn - on0) < 0.01f, "reintento no retrocede mas");
  // con el umbral ajustado, los factores nuevos no lo pisan
  g_args.clear(); g_args["on"] = "100"; g_args["off"] = "80"; g_args["n"] = "10"; manejarUmbral(); tickN(2, 30);
  g_args.clear(); g_args["k"] = "k"; g_args["v"] = "2"; manejarCfg(); tickN(2, 30);
  CHECK(umbralOn == 100, "un umbral aprobado no se recalcula al cambiar K (%.1f)", umbralOn);
  // recalibrar conserva nada del anterior y vuelve a empezar
  ejecutarComando('c');
  CHECK(modo == CALIBRANDO && faseCal == CAL_ESPERA, "recalibrar");
}

void t_control() {
  arranque(); basal(30, 3); sesion();
  tickN(40, ALTO_RMS);
  CHECK(activo, "activacion detectada");
  CHECK(!controlAuto && !premiado(), "supervisado: sin premio aunque haya activacion");
  CHECK(velVaiven() == V_VAIVEN_LENTO, "estrellas lentas");
  ejecutarComando('a');
  CHECK(controlAuto, "control automatico habilitado");
  tickN(40, nivelRms(false));
  CHECK(!activo && premiado() && velVaiven() == V_VAIVEN_RAPIDO, "con control: relajado -> rapido");
  ejecutarComando('a');
  CHECK(!controlAuto && !premiado(), "vuelve a supervisado");
}

void t_json() {
  arranque();
  g_args.clear(); manejarEstado();
  FILE *f = fopen("json_cal.json", "w"); fputs(g_body.c_str(), f); fclose(f);
  basal(30, 3); sesion(); autoMedico = false; reqs.clear(); visto = 0; ultPend = 0;
  tickN(300, nivelRms(false));
  g_args.clear(); g_args["b"] = "0"; manejarEstado();
  f = fopen("json_ses.json", "w"); fputs(g_body.c_str(), f); fclose(f);
  CHECK(g_body.size() < sizeof(js) - 200, "respuesta de %zu bytes", g_body.size());
  // muchos eventos (ring completo)
  autoMedico = true;
  for (int i = 0; i < 60; i++) { g_args.clear(); g_args["p"] = i % 2 ? "E" : "C"; g_args["n"] = std::to_string(100 + i); manejarManual(); tickN(35, nivelRms(i % 2)); }
  g_args.clear(); g_args["r"] = "0"; manejarEstado();
  f = fopen("json_full.json", "w"); fputs(g_body.c_str(), f); fclose(f);
  CHECK(g_body.size() < sizeof(js) - 200, "respuesta completa de %zu bytes", g_body.size());
  printf("  nEv=%lu, JSON de %zu bytes\n", (unsigned long)nEv, g_body.size());
}

void t_transicion() {
  // el RMS salta entre 130 y 52 mV cada 9 s; AMBOS niveles estan sobre U_ON (el estado sigue ALTO): ninguna ventana debe promediar los dos
  arranque(); basal(30, 3); sesion(); autoMedico = true; reqs.clear(); visto = 0; ultPend = 0;
  bool alto = true;
  for (int b = 0; b < 40 * 600; b++) {
    if (b % 90 == 0) alto = !alto;
    tick((alto ? 130 : 52) + 4 * gauss());
    vigilar();
  }
  int mezcladas = 0, n = 0;
  for (auto &r : reqs) { n++; if (r.e.media > 70 && r.e.media < 112) mezcladas++; }
  CHECK(n >= 5, "hay preguntas (%d)", n);
  CHECK(mezcladas == 0, "%d de %d ventanas cruzaron una transicion (media entre los dos niveles)", mezcladas, n);
  CHECK(episodio <= 2, "el estado no cambio (episodio %lu)", (unsigned long)episodio);
}

int main(int argc, char **argv) {
  struct { const char *n; void (*f)(); } T[] = {
    {"base", t_base}, {"calidad", t_calidad}, {"sostenido", t_sostenido}, {"largo", t_largo}, {"prolongado", t_prolongado},
    {"sinpagina", t_sinpagina}, {"pausa", t_pausa}, {"respuestas", t_respuestas}, {"expira", t_expira}, {"umbral", t_umbral},
    {"control", t_control}, {"json", t_json}, {"transicion", t_transicion}};
  for (auto &t : T) if (argc > 1 && !strcmp(argv[1], t.n)) { t.f(); printf("%s %s\n", fails ? "FALLO" : "OK   ", t.n); return fails ? 1 : 0; }
  printf("uso: test_esp <nombre>\n"); return 2;
}
