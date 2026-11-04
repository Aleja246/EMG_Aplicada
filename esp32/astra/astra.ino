// =====================================================================
//  ASTRA - ESP32: el cerebro (v3.0)
//  Un solo cerebro: el Arduino solo mide; este ESP32 decide todo
//  (calibración, detección, actuadores, métricas y página); la página solo
//  muestra datos y manda botones. Si la página se cierra, la terapia sigue.
// ---------------------------------------------------------------------
//  Entra : Arduino -> opto -> IO35, 9600 baud, cada 50 ms:   R,0.052
//  Sale  : página (ws://192.168.4.1/ws), con cada muestra:
//          {"rms":0.052,"alto":0.07,"bajo":0.055,"estado":"BASAL","n":6,"nivel":3,"t":42}
//  Recibe: {"cmd":"cal_reposo"} {"cmd":"iniciar"} {"cmd":"terminar"} {"cmd":"sens","k":3} ...
//          (lista completa en docs/PROTOCOLO.md)
//
//  Bloques del proyecto:
//   1. Detección sin actuadores: pon USAR_LUZ/SONIDO/ESTRELLAS/PLANETAS en false
//      y mira el conteo en el monitor serie (115200). Comandos: c i t + - ?
//   2. Actuadores de uno en uno: luz -> sonido -> estrellas -> planetas.
//
//  Placa "ESP32 Dev Module". Bibliotecas (Herramientas > Gestionar bibliotecas):
//   AccelStepper (Mike McCauley), ESP Async WebServer (ESP32Async), Async TCP (ESP32Async).
//  De v2.3 se conserva lo que ya funcionaba: pines, motores, microswitch, DFPlayer
//  con comandos directos, paro y botón BOOT.
// =====================================================================
#include <stdarg.h>
#include "nucleo.h"
using namespace nucleo;

// ----------------- Qué actuadores usar (bloque 2: de uno en uno) -----------------
const bool USAR_LUZ       = true;
const bool USAR_SONIDO    = true;
const bool USAR_ESTRELLAS = true;
const bool USAR_PLANETAS  = true;

// ----------------- WiFi propio del ESP32 (punto de acceso) -----------------
const char *WIFI_NOMBRE = "ASTRA";
const char *WIFI_CLAVE  = "astra2030";            // mínimo 8 caracteres

// ----------------- Pines (esquemático, igual que v2.3) -----------------
const int PIN_ROJO = 25, PIN_VERDE = 33, PIN_AZUL = 32;   // tira RGB por el ULN2803
const int PIN_TOPE = 13;    // microswitch (C a GND, NO a IO13)
const int PIN_PARO = 34;    // divisor desde 5VA (alto = paro suelto)
const int PIN_BOOT = 0;     // botón BOOT de la placa
const int EMG_RX = 35;      // datos del opto
const int EMG_TX = 14;      // Serial2 necesita un TX: no conectes nada en IO14
const int DF_RX = 27, DF_TX = 26;                 // DFPlayer

// ----------------- Enlace y seguridad -----------------
const bool USAR_PARO     = false;   // true cuando el divisor del paro (R6, R7) esté conectado
const bool EMG_INVERTIDO = false;   // true si el opto invierte (aviso "Llegan bytes...")
const unsigned long SIN_SENAL_MS = 500;           // más de esto sin datos: todo se detiene
const bool PLOTTER = false;         // true: el monitor serie imprime rms/alto/bajo para graficar

// ----------------- Calibración (solo con el reposo, Hodges y Bui) -----------------
const unsigned long T_REPOSO_MS = 15000;          // obligatoria
const unsigned long T_PASIVA_MS = 10000;          // opcional
const unsigned long T_REF_MS    = 45000;          // opcional (30-60 s)
const unsigned long T_DESCARTE_MS = 1000;         // el primer segundo del reposo no cuenta
const float K_INICIAL = 3.0f, K_MIN = 1.5f, K_MAX = 6.0f;

// ----------------- Sesión -----------------
const int N_ESCALONES = 5;                        // 5 x 8 cm = 40 cm
const float INV_INICIAL_S = 8;                    // invitación tras 8 s sin activación
const unsigned long T_ANUNCIO_INV_MS = 15000;     // "¡Arriba!" cada 15 s
const unsigned long T_BAJAR_INV_MS   = 10000;     // las estrellas bajan un escalón cada 10 s
const unsigned long T_LUZ_MIN_MS     = 1000;      // máximo un cambio de color por segundo
const unsigned long T_COLOR_CIMA_MS  = 1000;      // en la cima: un color por segundo

enum Modo { ESPERA, CAL_REPOSO, CAL_PASIVA, CAL_REF, SESION, PARO };
const char *NOMBRE_MODO[] = {"ESPERA", "CAL_REPOSO", "CAL_PASIVA", "CAL_REF", "SESION", "PARO"};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;  // protege lo que lee la tarea de la red

#include "luz.h"
#include "sonido.h"
#include "motores.h"

// ======================================================================
//  Estado
// ======================================================================
Modo modo = ESPERA, modoAntesParo = ESPERA;

// Enlace con el Arduino
LectorLineas lector;
float rmsUltimo = 0;
unsigned long tUltimoDato = 0;
bool hayDatos = false, senalOK = false;
unsigned long bytesSinLinea = 0;
bool avisoBytes = false;
unsigned lineasSeg = 0, lineasCuenta = 0;

// Calibración
Umbrales umb;
Estad estReposo;
MaxSostenido maxCal;
unsigned long tCal = 0, durCal = 0, proxAnuncioCal = 0;
bool hayPasiva = false, iniciarAlCalibrar = false;
float refCal = 0, cvReposo = 0;

// Sesión
Detector det;
Metricas met;
bool haySesion = false;
unsigned long tInicioSesion = 0, tFinSesion = 0;
int escalones = 0, colorIdx = -1, colorCima = 0;
bool celebrando = false, felicitacionPendiente = false;
unsigned long tCima = 0;
bool enInvitacion = false;
unsigned long tInvitacion = 0, proxAnuncioInv = 0, tUltimaBajada = 0;
unsigned long tUltimaActividad = 0, reaccionLibreEn = 0;
float respPendiente = -1;

// Ajustes que cambia la página
float invS = INV_INICIAL_S;
bool bajarEnInv = true, usarFamilia = false;

// Paro y botón BOOT
bool paroActivo = false;
int paroLectura = HIGH, bootAnterior = HIGH;
unsigned long paroCambio = 0, bootCambio = 0;

// Envío a la página
bool infoPendiente = true;
unsigned long tUltimaInfo = 0, tUltimoLatido = 0, tUltimoEstado = 0;

#include "web.h"

// Funciones que se usan antes de definirse
void iniciarSesion();
void enviarDato(bool conValor);
void imprimirResumen();

// ======================================================================
//  Utilidades y mensajes
// ======================================================================
bool enSesion()   { return modo == SESION || (modo == PARO && modoAntesParo == SESION); }
bool calibrando() { return modo == CAL_REPOSO || modo == CAL_PASIVA || modo == CAL_REF; }

// 100 % = RMS más alto de la referencia; si no la hubo, el más alto de la sesión.
float referencia() { return refCal > 0 ? refCal : met.pico.maximo; }

float duracionSesionS() {
  if (!haySesion) return 0;
  unsigned long fin = enSesion() ? millis() : tFinSesion;
  return (fin - tInicioSesion) / 1000.0f;
}

// Mensaje para la terapeuta: va al monitor serie y a la página.
void avisar(const char *fmt, ...) {
  char txt[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(txt, sizeof txt, fmt, args);
  va_end(args);
  for (char *p = txt; *p; p++) if (*p == '"' || *p == '\\') *p = '\'';
  Serial.print("   ");
  Serial.println(txt);
  char json[200];
  int n = snprintf(json, sizeof json, "{\"tipo\":\"msg\",\"txt\":\"%s\"}", txt);
  webEnviar(json, n < (int)sizeof json ? n : sizeof json - 1);
  infoPendiente = true;
}

// ======================================================================
//  Calibración (no se le pide al niño que suba la cabeza)
// ======================================================================
void empezarCal(Modo m) {
  modo = m;
  tCal = millis();
  estReposo.reiniciar();
  maxCal.reiniciar();
  durCal = m == CAL_REPOSO ? T_REPOSO_MS : m == CAL_PASIVA ? T_PASIVA_MS : T_REF_MS;
  luzFija(BLANCO_TENUE);
  if (m == CAL_REF) {                            // el móvil hace su espectáculo
    musicaIniciar();
    colorIdx = -1;
    proxAnuncioCal = tCal + 3000;
  }
  if (m == CAL_REPOSO) avisar("REPOSO %lu s: cabeza apoyada en la mano de la terapeuta, sin moverla.", T_REPOSO_MS / 1000);
  else if (m == CAL_PASIVA) avisar("PASIVA %lu s: la terapeuta mueve la cabeza; el niño no hace fuerza.", T_PASIVA_MS / 1000);
  else avisar("REFERENCIA %lu s: enséñale un juguete arriba o llámalo por su nombre.", T_REF_MS / 1000);
}

void terminarCal() {
  Modo m = modo;
  modo = ESPERA;
  luzFija(BLANCO_TENUE);
  if (m == CAL_REPOSO) {
    if (estReposo.n < 100) {
      iniciarAlCalibrar = false;
      avisar("Reposo: llegaron muy pocos datos (%lu). Revisa el enlace y repite.", (unsigned long)estReposo.n);
      return;
    }
    umb.media = estReposo.media;
    umb.sd = estReposo.sd();
    umb.piso = 0;
    umb.listo = true;
    hayPasiva = false;                           // nueva colocación: pasiva y referencia de nuevo
    refCal = 0;
    cvReposo = umb.media > 0 ? 100 * umb.sd / umb.media : 0;
    avisar("Reposo listo: media %.4f, SD %.4f. Umbral alto %.4f, bajo %.4f (k = %.1f).",
           umb.media, umb.sd, umb.alto(), umb.bajo(), umb.k);
    if (umb.media < 0.0001f) avisar("Ojo: la señal está casi en cero. Revisa el sensor y los electrodos.");
    else if (cvReposo > 50) avisar("Ojo: el reposo varió mucho (%.0f %%). Si se movió, repite el reposo.", cvReposo);
    if (iniciarAlCalibrar) { iniciarAlCalibrar = false; iniciarSesion(); }
  } else if (m == CAL_PASIVA) {
    float sinPiso = umb.media + umb.k * umb.sdEfectiva();
    umb.piso = maxCal.maximo * MARGEN_PASIVA;    // el umbral nunca queda bajo el artefacto
    hayPasiva = true;
    if (maxCal.maximo > sinPiso) avisar("Pasiva: el movimiento llegó a %.4f; el umbral alto sube a %.4f.", maxCal.maximo, umb.alto());
    else avisar("Pasiva: el movimiento (máx. %.4f) no cruza el umbral alto (%.4f). Sin cambios.", maxCal.maximo, umb.alto());
  } else {
    musicaDetener();
    if (maxCal.maximo > umb.alto()) {
      refCal = maxCal.maximo;
      avisar("Referencia lista: 100 %% = %.4f.", refCal);
    } else {
      refCal = 0;
      avisar("No hubo activación clara: el 100 %% será el RMS más alto de la sesión.");
    }
  }
}

void actualizarCal() {
  if (!calibrando()) return;
  unsigned long ahora = millis();
  if (modo == CAL_REF && senalOK) {              // espectáculo: un color cada 2 s
    int c = ((ahora - tCal) / 2000) % N_COLORES;
    if (c != colorIdx) { colorIdx = c; luzFija(COLORES[c]); }
    if ((long)(ahora - proxAnuncioCal) >= 0 && anunciar(INVITACION)) proxAnuncioCal = ahora + T_ANUNCIO_INV_MS;
  }
  if (senalOK && ahora - tCal >= durCal) terminarCal();
}

// ======================================================================
//  Sesión
// ======================================================================
void iniciarSesion() {
  portENTER_CRITICAL(&mux);
  met.reiniciar();
  portEXIT_CRITICAL(&mux);
  haySesion = true;
  modo = SESION;
  tInicioSesion = tUltimaActividad = reaccionLibreEn = millis();
  tFinSesion = 0;
  det.reiniciar();
  escalones = 0;
  colorIdx = -1;
  respPendiente = -1;
  celebrando = felicitacionPendiente = enInvitacion = false;
  estrellasIrA(0);                               // vuelven a cero (el tope se buscó al encender)
  musicaIniciar();                               // canción de fondo en bucle, volumen bajo
  luzFija(AZUL_TENUE);
  avisar("Sesión iniciada: umbral alto %.4f, bajo %.4f (k = %.1f).", umb.alto(), umb.bajo(), umb.k);
}

void alFinActivacion() {
  Act a;
  a.t0 = (det.tInicio - tInicioSesion) / 1000.0f;
  a.dur = det.duracionS();
  a.media = det.media();
  a.pico = det.pico;
  a.resp = respPendiente;
  portENTER_CRITICAL(&mux);
  met.agregar(a);
  portEXIT_CRITICAL(&mux);
  respPendiente = -1;
  tUltimaActividad = det.tFin;
  float ref = referencia();
  Serial.printf("   fin: %.2f s sobre el umbral, RMS medio %.4f (%.0f %% de la referencia), pico %.4f\n",
                a.dur, a.media, ref > 0 ? 100 * a.media / ref : 0.0f, a.pico);
  infoPendiente = true;
}

void terminarSesion() {
  unsigned long ahora = millis();
  if (det.cerrar(senalOK ? ahora : tUltimoDato)) alFinActivacion();
  enInvitacion = celebrando = felicitacionPendiente = false;
  tFinSesion = ahora;
  escalones = 0;
  estrellasIrA(0);
  if (modo == PARO) {
    modoAntesParo = ESPERA;                      // sigue en paro hasta reanudar
  } else {
    modo = ESPERA;
    musicaDetener();
    luzFija(BLANCO_TENUE);
    estrellasSeguir();
  }
  avisar("Sesión terminada: %lu levantamientos en %.0f s, %u rondas. Ya puedes descargar el resumen.",
         (unsigned long)met.n, duracionSesionS(), (unsigned)met.rondas);
}

void alLevantamiento(unsigned long t) {
  float resp = -1;
  if (enInvitacion) {                            // respondió a la invitación
    enInvitacion = false;
    resp = (det.tInicio > tInvitacion ? det.tInicio - tInvitacion : 0) / 1000.0f;
  }
  portENTER_CRITICAL(&mux);
  met.n++;
  if (resp >= 0) { met.respondidas++; met.sumaResp += resp; }
  portEXIT_CRITICAL(&mux);
  respPendiente = resp;

  if (!celebrando && escalones < N_ESCALONES) estrellasIrA(++escalones);   // un escalón más

  // Contar no es lo mismo que reaccionar: si la luz o el sonido del anterior no
  // terminó, este levantamiento se cuenta pero no dispara luz ni sonido.
  bool reacciona = !celebrando && (long)(t - reaccionLibreEn) >= 0;
  if (reacciona) {
    colorIdx = (colorIdx + 1) % N_COLORES;
    luzFija(COLORES[colorIdx]);
    unsigned long ocupado = T_LUZ_MIN_MS;
    if (anunciar(DING)) ocupado = max(ocupado, DURACION_ANUNCIO_MS[DING] + MARGEN_ANUNCIO_MS);
    reaccionLibreEn = t + ocupado;
  } else if (luzRespirando) {
    luzFija(colorIdx >= 0 ? COLORES[colorIdx] : AZUL_TENUE);
  }
  Serial.printf("#%lu LEVANTAMIENTO  t=%.1f s  rms %.4f > alto %.4f  nivel %d/%d",
                (unsigned long)met.n, (t - tInicioSesion) / 1000.0f, rmsUltimo, umb.alto(), escalones, N_ESCALONES);
  if (resp >= 0) Serial.printf("  (respondió a la invitación en %.1f s)", resp);
  if (!reacciona) Serial.print("  (sin luz ni sonido: el anterior no terminó)");
  Serial.println();
  infoPendiente = true;
}

void actualizarSesion() {
  unsigned long ahora = millis();

  // Llegan arriba (5 escalones = 40 cm): celebración; luego bajan a cero y otra ronda.
  if (!celebrando && escalones >= N_ESCALONES && estrellasEnObjetivo()) {
    celebrando = felicitacionPendiente = true;
    enInvitacion = false;
    tCima = ahora;
    colorCima = 0;
    luzFija(COLORES[0]);
    Serial.println("   *** Estrellas arriba: ronda completa ***");
    infoPendiente = true;
  }
  if (celebrando) {
    if (felicitacionPendiente && anunciar(usarFamilia ? FAMILIA : FELICITACION)) felicitacionPendiente = false;
    int c = (ahora - tCima) / T_COLOR_CIMA_MS;   // recorre los colores, uno por segundo
    if (c < N_COLORES) {
      if (c != colorCima) { colorCima = c; luzFija(COLORES[c]); }
      return;
    }
    celebrando = felicitacionPendiente = false;
    portENTER_CRITICAL(&mux);
    met.rondas++;
    portEXIT_CRITICAL(&mux);
    escalones = 0;
    estrellasIrA(0);
    tUltimaActividad = ahora;
    colorIdx = -1;
    luzFija(AZUL_TENUE);
    musicaSiguiente();
    avisar("Ronda %u completa: las estrellas bajan a cero.", (unsigned)met.rondas);
    return;
  }

  // Invitación: unos segundos sin activación (lo más probable: la cabeza cayó).
  if (!enInvitacion && !det.activo && escalones < N_ESCALONES &&
      ahora - tUltimaActividad >= (unsigned long)(invS * 1000)) {
    enInvitacion = true;
    tInvitacion = proxAnuncioInv = tUltimaBajada = ahora;
    portENTER_CRITICAL(&mux);
    met.invitaciones++;
    portEXIT_CRITICAL(&mux);
    luzRespira();
    Serial.printf("   Invitación: %.0f s sin activación.\n", invS);
    infoPendiente = true;
  }
  if (enInvitacion) {
    if ((long)(ahora - proxAnuncioInv) >= 0 && anunciar(INVITACION)) proxAnuncioInv = ahora + T_ANUNCIO_INV_MS;
    if (bajarEnInv && escalones > 0 && ahora - tUltimaBajada >= T_BAJAR_INV_MS) {
      tUltimaBajada = ahora;
      estrellasIrA(--escalones);
      infoPendiente = true;
    }
  }
}

// ======================================================================
//  Señal del Arduino y paro de emergencia
// ======================================================================
void revisarSenal() {
  bool ok = hayDatos && millis() - tUltimoDato < SIN_SENAL_MS;
  if (ok == senalOK) return;
  senalOK = ok;
  infoPendiente = true;
  if (!ok) {                                     // más de 500 ms sin señal: todo se detiene
    maxCal.corte();
    portENTER_CRITICAL(&mux);
    met.pico.corte();
    if (modo == SESION && celebrando) met.rondas++;
    portEXIT_CRITICAL(&mux);
    if (modo == SESION) {
      if (det.cerrar(tUltimoDato)) alFinActivacion();
      celebrando = felicitacionPendiente = enInvitacion = false;
      estrellasPausar();
      musicaPausar();
      luzFija(APAGADA);
    } else if (modo == CAL_REF) {
      musicaPausar();
      luzFija(APAGADA);
    }
    avisar("Sin señal del Arduino: todo se detiene hasta que vuelva.");
    return;
  }
  if (modo == SESION) {                          // vuelve: estrellas a cero y sigue
    escalones = 0;
    estrellasIrA(0);
    estrellasSeguir();
    musicaReanudar();
    luzFija(AZUL_TENUE);
    tUltimaActividad = reaccionLibreEn = millis();
    avisar("Volvió la señal: las estrellas vuelven a cero y la terapia sigue.");
  } else if (calibrando()) {
    tCal = millis();
    estReposo.reiniciar();
    maxCal.reiniciar();
    if (modo == CAL_REF) { musicaReanudar(); proxAnuncioCal = tCal + 3000; }
    avisar("Volvió la señal: la calibración empieza de nuevo.");
  } else {
    avisar("Llegan datos del Arduino.");
  }
}

void entrarParo() {
  if (modo != PARO) modoAntesParo = calibrando() ? ESPERA : modo;
  if (modo == SESION) {
    if (det.cerrar(millis())) alFinActivacion();
    if (celebrando) { portENTER_CRITICAL(&mux); met.rondas++; portEXIT_CRITICAL(&mux); }
    celebrando = felicitacionPendiente = enInvitacion = false;
  }
  iniciarAlCalibrar = false;
  modo = PARO;
  estrellasDetenerYa();
  musicaPausar();
  luzFija(APAGADA);
  avisar("PARO DE EMERGENCIA. Al soltarlo, pulsa Reanudar (o el botón BOOT).");
}

void reanudarTrasParo() {
  if (paroActivo) { avisar("El paro sigue presionado: gíralo para soltarlo."); return; }
  modo = modoAntesParo;
  escalones = 0;
  estrellasIrA(0);
  estrellasBuscarCero();                         // las bobinas estuvieron apagadas: buscar el tope
  if (modo == SESION) {
    musicaReanudar();
    luzFija(AZUL_TENUE);
    tUltimaActividad = reaccionLibreEn = millis();
  } else {
    musicaDetener();
    luzFija(BLANCO_TENUE);
  }
  avisar("Reanudado: las estrellas buscan el tope y vuelven a cero.");
}

void revisarParo() {
  if (!USAR_PARO) return;
  int v = digitalRead(PIN_PARO);
  if (v != paroLectura) { paroLectura = v; paroCambio = millis(); }
  if (millis() - paroCambio < 30) return;        // antirrebote
  bool presionado = paroLectura == LOW;
  if (presionado && !paroActivo) { paroActivo = true; entrarParo(); }
  else if (!presionado && paroActivo) { paroActivo = false; avisar("Paro liberado. Pulsa Reanudar (o BOOT)."); }
}

// ======================================================================
//  Comandos (página, monitor serie y botón BOOT llegan aquí)
// ======================================================================
void ejecutar(const char *cmd, float v, bool hayV) {
  Modo cal = !strcmp(cmd, "cal_reposo") ? CAL_REPOSO
           : !strcmp(cmd, "cal_pasiva") ? CAL_PASIVA
           : !strcmp(cmd, "cal_ref")    ? CAL_REF : ESPERA;
  if (cal != ESPERA) {
    if (modo != ESPERA) avisar("Para calibrar, primero termina la sesión.");
    else if (!senalOK) avisar("No llegan datos del Arduino: revisa el enlace.");
    else if (cal != CAL_REPOSO && !umb.listo) avisar("Primero calibra el REPOSO.");
    else empezarCal(cal);
  } else if (!strcmp(cmd, "cancelar")) {
    if (!calibrando()) return;
    if (modo == CAL_REF) musicaDetener();
    iniciarAlCalibrar = false;
    modo = ESPERA;
    luzFija(BLANCO_TENUE);
    avisar("Calibración cancelada.");
  } else if (!strcmp(cmd, "iniciar")) {
    if (modo != ESPERA) return;
    if (!umb.listo) avisar("Primero calibra el REPOSO.");
    else if (!senalOK) avisar("No llegan datos del Arduino: revisa el enlace.");
    else iniciarSesion();
  } else if (!strcmp(cmd, "terminar")) {
    if (enSesion()) terminarSesion();
  } else if (!strcmp(cmd, "sens") && hayV) {
    umb.k = constrain(v, K_MIN, K_MAX);
    Serial.printf("   k = %.1f -> umbral alto %.4f, bajo %.4f\n", umb.k, umb.alto(), umb.bajo());
  } else if (!strcmp(cmd, "inv") && hayV) {
    invS = constrain(v, 3.0f, 120.0f);
  } else if (!strcmp(cmd, "bajar") && hayV) {
    bajarEnInv = v != 0;
  } else if (!strcmp(cmd, "familia") && hayV) {
    usarFamilia = v != 0;
  } else if (!strcmp(cmd, "vol") && hayV) {
    sonidoVolumen((uint8_t)constrain((int)v, 0, 30));
  } else if (!strcmp(cmd, "reanudar")) {
    if (modo == PARO) reanudarTrasParo();
  } else if (!strcmp(cmd, "buscar_cero")) {
    if (modo != PARO) estrellasBuscarCero();
  } else {
    return;
  }
  infoPendiente = true;
}

void revisarBoot() {
  int v = digitalRead(PIN_BOOT);
  if (v == bootAnterior || millis() - bootCambio < 50) return;
  bootCambio = millis();
  bootAnterior = v;
  if (v != LOW) return;                          // solo al presionar
  if (modo == PARO) reanudarTrasParo();
  else if (modo == ESPERA && senalOK) {          // sin página: calibra y empieza solo
    iniciarAlCalibrar = true;
    empezarCal(CAL_REPOSO);
  }
}

// ======================================================================
//  Entradas
// ======================================================================
void procesarMuestra(float v, unsigned long t) {
  rmsUltimo = v;
  tUltimoDato = t;
  hayDatos = true;
  lineasCuenta++;
  if (!senalOK) revisarSenal();
  switch (modo) {
    case CAL_REPOSO: if (t - tCal >= T_DESCARTE_MS) estReposo.agregar(v); break;
    case CAL_PASIVA: if (t - tCal >= 500) maxCal.agregar(v); break;
    case CAL_REF:    maxCal.agregar(v); break;
    case SESION: {
      portENTER_CRITICAL(&mux);
      met.pico.agregar(v);
      portEXIT_CRITICAL(&mux);
      Evento e = det.procesar(v, t, umb.alto(), umb.bajo());
      if (e == INICIO) alLevantamiento(t);
      else if (e == FIN) alFinActivacion();
      break;
    }
    default: break;
  }
  enviarDato(true);
  if (PLOTTER) Serial.printf("rms:%.4f alto:%.4f bajo:%.4f\n", v, umb.listo ? umb.alto() : 0.0f, umb.listo ? umb.bajo() : 0.0f);
}

void leerArduino() {
  while (Serial2.available()) {
    char c = Serial2.read();
    unsigned long ahora = millis();
    float v;
    bytesSinLinea++;
    if (lector.procesar(c, ahora, v)) { bytesSinLinea = 0; procesarMuestra(v, ahora); }
  }
  if (!avisoBytes && bytesSinLinea > 200) {      // casi siempre: polaridad o baudios
    avisoBytes = true;
    avisar("Llegan bytes del Arduino pero ninguna línea válida: revisa 9600 baud o EMG_INVERTIDO.");
  }
}

void leerMonitor() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'c': ejecutar("cal_reposo", 0, false); break;
      case 'p': ejecutar("cal_pasiva", 0, false); break;
      case 'f': ejecutar("cal_ref", 0, false); break;
      case 'x': ejecutar("cancelar", 0, false); break;
      case 'i': ejecutar("iniciar", 0, false); break;
      case 't': ejecutar("terminar", 0, false); break;
      case '+': ejecutar("sens", umb.k + 0.5f, true); break;
      case '-': ejecutar("sens", umb.k - 0.5f, true); break;
      case 'r': ejecutar("reanudar", 0, false); break;
      case 'h': ejecutar("buscar_cero", 0, false); break;
      case '?': imprimirResumen(); break;
      default: break;
    }
  }
}

// ======================================================================
//  Salidas: página (WebSocket) y monitor serie
// ======================================================================
const char *estadoTexto() {
  if (modo == PARO) return "PARO";
  if (!senalOK) return "SIN_SENAL";
  if (modo != SESION) return NOMBRE_MODO[modo];
  if (celebrando) return "CELEBRA";
  if (det.activo) return "ACTIVO";
  if (enInvitacion) return "INVITACION";
  return "BASAL";
}

// El mensaje acordado, con cada muestra (cada 50 ms). Sin señal: rms null cada 250 ms.
void enviarDato(bool conValor) {
  char rms[16] = "null";
  if (conValor) snprintf(rms, sizeof rms, "%.4f", rmsUltimo);
  char b[200];
  int n = snprintf(b, sizeof b, "{\"rms\":%s,\"alto\":%.4f,\"bajo\":%.4f,\"estado\":\"%s\",\"n\":%lu,\"nivel\":%d,\"t\":%lu}",
                   rms, umb.listo ? umb.alto() : 0.0f, umb.listo ? umb.bajo() : 0.0f, estadoTexto(),
                   (unsigned long)met.n, escalones, (unsigned long)duracionSesionS());
  webEnviar(b, n);
  tUltimoLatido = millis();
}

unsigned long calRestanS() {
  if (!calibrando()) return 0;
  unsigned long pasado = millis() - tCal;
  return pasado >= durCal ? 0 : (durCal - pasado + 999) / 1000;
}

// Estado completo y métricas: cada segundo y después de cada cambio.
void enviarInfo() {
  float dur = duracionSesionS(), ref = referencia();
  char b[1024];
  int n = snprintf(b, sizeof b,
    "{\"tipo\":\"info\",\"modo\":\"%s\",\"senal\":%d,\"est\":\"%s\",\"t\":%lu,\"n\":%lu,\"nivel\":%d,\"rondas\":%u,"
    "\"k\":%.1f,\"cal\":%d,\"media\":%.4f,\"sd\":%.4f,\"cv\":%.0f,\"pasiva\":%d,\"piso\":%.4f,"
    "\"alto\":%.4f,\"bajo\":%.4f,\"ref\":%.4f,\"refFuente\":\"%s\","
    "\"lpm\":%.1f,\"amp\":%.0f,\"dur\":%.2f,\"resp\":%.1f,\"nInv\":%u,\"nResp\":%u,"
    "\"inv\":%.0f,\"bajar\":%d,\"familia\":%d,\"vol\":%u,\"calRestan\":%lu,\"calTotal\":%lu,"
    "\"lps\":%u,\"malas\":%lu,\"csv\":%d}",
    NOMBRE_MODO[modo], senalOK, NOMBRE_EST[estE], (unsigned long)dur, (unsigned long)met.n, escalones, (unsigned)met.rondas,
    umb.k, umb.listo, umb.media, umb.sd, cvReposo, hayPasiva, umb.piso,
    umb.listo ? umb.alto() : 0.0f, umb.listo ? umb.bajo() : 0.0f, ref, refCal > 0 ? "calibracion" : "sesion",
    met.porMinuto(dur), met.ampMedia(ref), met.durMedia(), met.respMedia(), (unsigned)met.invitaciones, (unsigned)met.respondidas,
    invS, bajarEnInv, usarFamilia, (unsigned)volumen, calRestanS(), calibrando() ? durCal / 1000 : 0UL,
    lineasSeg, (unsigned long)lector.malas, haySesion);
  webEnviar(b, n < (int)sizeof b ? n : sizeof b - 1);
  tUltimaInfo = millis();
  infoPendiente = false;
}

void imprimirEstado() {
  if (modo == SESION) {
    Serial.printf("[SESION %4lu s] rms %.4f | alto %.4f bajo %.4f | %-10s | n=%lu nivel %d/%d rondas %u | %u líneas/s\n",
                  (unsigned long)duracionSesionS(), rmsUltimo, umb.alto(), umb.bajo(), estadoTexto(),
                  (unsigned long)met.n, escalones, N_ESCALONES, (unsigned)met.rondas, lineasSeg);
  } else if (calibrando()) {
    Serial.printf("[%s] rms %.4f | faltan %lu s%s\n", NOMBRE_MODO[modo], rmsUltimo, calRestanS(), senalOK ? "" : " | SIN SEÑAL");
  } else {
    Serial.printf("[%s] rms %.4f | %s | %u líneas/s | estrellas %s\n", NOMBRE_MODO[modo], rmsUltimo,
                  senalOK ? "señal OK" : "SIN SEÑAL", lineasSeg, NOMBRE_EST[estE]);
  }
}

void imprimirResumen() {
  float dur = duracionSesionS(), ref = referencia(), resp = met.respMedia();
  Serial.println();
  Serial.println("---- Calibración ----");
  if (umb.listo) Serial.printf("Reposo: media %.4f, SD %.4f (variación %.0f %%)\n", umb.media, umb.sd, cvReposo);
  else Serial.println("Reposo: sin calibrar (escribe c)");
  Serial.printf("k = %.1f -> umbral alto %.4f, bajo %.4f%s\n", umb.k, umb.alto(), umb.bajo(), hayPasiva ? " (con pasiva)" : "");
  Serial.printf("Referencia 100 %%: %.4f (%s)\n", ref, refCal > 0 ? "calibración" : "máximo de la sesión");
  Serial.println("---- Sesión ----");
  Serial.printf("Levantamientos: %lu en %.0f s (%.1f por minuto), rondas: %u\n",
                (unsigned long)met.n, dur, met.porMinuto(dur), (unsigned)met.rondas);
  Serial.printf("Duración media: %.2f s | amplitud media: %.0f %% de la referencia\n", met.durMedia(), met.ampMedia(ref));
  Serial.printf("Invitaciones: %u, respondidas: %u", (unsigned)met.invitaciones, (unsigned)met.respondidas);
  if (resp >= 0) Serial.printf(" (respuesta media %.1f s)", resp);
  Serial.printf("\nLíneas dañadas del enlace: %lu\n----------------\n", (unsigned long)lector.malas);
}

void enviarPeriodicos() {
  unsigned long ahora = millis();
  if (clienteNuevo) { clienteNuevo = false; infoPendiente = true; }
  if (ahora - tUltimaInfo >= 1000 || (infoPendiente && ahora - tUltimaInfo >= 100)) enviarInfo();
  if (!senalOK && ahora - tUltimoLatido >= 250) enviarDato(false);   // la página ve "SIN_SENAL"
  if (ahora - tUltimoEstado >= 1000) {
    tUltimoEstado = ahora;
    lineasSeg = lineasCuenta;
    lineasCuenta = 0;
    if (!PLOTTER) imprimirEstado();
  }
}

// ======================================================================
//  Resumen CSV (lo descarga la página: http://192.168.4.1/resumen.csv)
//  Corre en la tarea de la red: copia las métricas con el candado y escribe la copia.
// ======================================================================
Metricas copiaCsv;                               // fuera de la pila (~8 KB)

String limpiarCsv(String s) {
  s.replace(',', ' ');
  s.replace('"', ' ');
  s.replace('\n', ' ');
  s.replace('\r', ' ');
  return s;
}

void escribirCsv(Print &o, const String &paciente, const String &fecha) {
  portENTER_CRITICAL(&mux);
  copiaCsv = met;
  portEXIT_CRITICAL(&mux);
  const Metricas &m = copiaCsv;
  float dur = duracionSesionS();
  float ref = refCal > 0 ? refCal : m.pico.maximo;
  float resp = m.respMedia();
  o.print("\xEF\xBB\xBF");                       // BOM: Excel lee los acentos
  o.print("ASTRA - Resumen de la sesión\r\n");
  o.printf("Paciente,%s\r\n", limpiarCsv(paciente).c_str());
  o.printf("Fecha,%s\r\n", limpiarCsv(fecha).c_str());
  o.printf("Duración de la sesión (s),%.0f\r\n", dur);
  o.printf("Levantamientos,%lu\r\n", (unsigned long)m.n);
  o.printf("Levantamientos por minuto,%.2f\r\n", m.porMinuto(dur));
  o.printf("Rondas completas,%u\r\n", (unsigned)m.rondas);
  o.printf("Amplitud media (%% de la referencia),%.0f\r\n", m.ampMedia(ref));
  o.printf("Duración media de la activación (s),%.2f\r\n", m.durMedia());
  o.printf("Invitaciones,%u\r\n", (unsigned)m.invitaciones);
  o.printf("Invitaciones respondidas,%u\r\n", (unsigned)m.respondidas);
  o.print("Tiempo de respuesta a la invitación promedio (s),");
  if (resp >= 0) o.printf("%.1f", resp);
  o.printf("\r\nReferencia 100%% (RMS),%.4f\r\n", ref);
  o.printf("Fuente de la referencia,%s\r\n", refCal > 0 ? "calibración" : "máximo de la sesión");
  o.printf("Reposo: media,%.4f\r\n", umb.media);
  o.printf("Reposo: desviación estándar,%.4f\r\n", umb.sd);
  o.printf("Sensibilidad k,%.1f\r\n", umb.k);
  o.printf("Umbral alto,%.4f\r\n", umb.alto());
  o.printf("Umbral bajo,%.4f\r\n", umb.bajo());
  o.printf("Piso por movimiento pasivo,%.4f\r\n", umb.piso);
  o.print("Nota,La duración es el tiempo sobre el umbral (no el tiempo con la cabeza arriba)\r\n");
  o.print("\r\nActivación,Inicio (s),Duración (s),RMS medio,RMS pico,Amplitud (% de la referencia),Respuesta a la invitación (s)\r\n");
  for (int i = 0; i < m.guardadas; i++) {
    const Act &a = m.acts[i];
    o.printf("%d,%.2f,%.2f,%.4f,%.4f,%.0f,", i + 1, a.t0, a.dur, a.media, a.pico, ref > 0 ? 100 * a.media / ref : 0.0f);
    if (a.resp >= 0) o.printf("%.1f", a.resp);
    o.print("\r\n");
  }
  if (m.terminadas > m.guardadas) o.printf("(%lu activaciones más no caben en la memoria)\r\n", (unsigned long)(m.terminadas - m.guardadas));
}

// ======================================================================
void setup() {
  motoresIniciar();                              // bobinas apagadas desde el primer instante
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== ASTRA v3: el Arduino mide, el ESP32 decide, la página muestra ===");
  luzIniciar();
  pinMode(PIN_TOPE, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_PARO, INPUT);
  Serial2.begin(9600, SERIAL_8N1, EMG_RX, EMG_TX, EMG_INVERTIDO);   // enlace con el Arduino
  sonidoIniciar();
  umb.k = K_INICIAL;
  webIniciar();
  luzFija(BLANCO_TENUE);
  if (USAR_PARO && digitalRead(PIN_PARO) == LOW) { paroActivo = true; paroLectura = LOW; entrarParo(); }
  else estrellasBuscarCero();
  Serial.println("Monitor serie: c=reposo p=pasiva f=referencia x=cancelar i=iniciar t=terminar");
  Serial.println("               +/- = sensibilidad k   r=reanudar tras paro   h=buscar cero   ?=resumen");
}

void loop() {                                    // nada bloquea: ningún delay()
  leerArduino();
  leerMonitor();
  CmdWeb c;
  while (sacarComando(c)) ejecutar(c.nombre, c.valor, c.hayValor);
  revisarBoot();
  revisarParo();
  revisarSenal();
  actualizarCal();
  if (modo == SESION && senalOK) actualizarSesion();
  estrellasActualizar();
  planetasActualizar(senalOK && (modo == SESION || modo == CAL_REF));
  luzActualizar();
  sonidoActualizar();
  enviarPeriodicos();
  webMantener();
}
