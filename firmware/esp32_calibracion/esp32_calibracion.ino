
// =====================================================================
//  ASTRA - Firmware del atril (ESP32) v3.0
//  La senal sEMG controla los actuadores.
// ---------------------------------------------------------------------
//  Entrada: el Arduino UNO R4 WiFi (arduino_emg.ino) manda por el opto,
//  cada 100 ms, una linea con el RMS en milivoltios ("640\n").
//
//  Que hace:
//   1. ARRANQUE: sube las estrellas hasta el microswitch (posicion 0).
//      El lazy susan empieza a girar desde el encendido.
//   2. CALIBRANDO (segun "Calibracion en la pagina sEMG cervical", 60 s),
//      PASO A PASO desde la pagina web (o con el boton BOOT):
//        "Comenzar calibracion" -> espera
//        "Comenzar reposo"  -> REPOSO 10 s               -> espera
//        "Comenzar pasiva"  -> PASIVA 10 s               -> espera
//        "Comenzar activa"  -> [ARRIBA 4 s + ABAJO 4 s] x 5 -> resultados
//        "Iniciar sesion"   -> SESION
//      U_on = mayor de (media_reposo + 3 SD_reposo, 25 % envolvente_activa)
//      U_off = 0.6 x U_on
//      Las estrellas ya van y vienen, lento.
//   3. SESION:
//      - Activacion: RMS suave sobre U_on por 300 ms. Termina: bajo U_off
//        por 300 ms.
//      - Estrellas: suben y bajan sin parar entre fondo y tope. Musculo
//        RELAJADO: rapido (700 pasos/s). Musculo ACTIVO: lento (250 pasos/s).
//      - Planetas (lazy susan): giran siempre, sin importar el esfuerzo.
//      - Luz: relajado = lila que crece; activo = azul tenue; arriba y
//        relajado = verde azulado. Cambios lentos (>= 1 s, sin parpadeo).
//      - Sonido: 0001.mp3 tras 1 s relajado; 0002.mp3 al llegar arriba relajado.
//   4. Seguridad: sin datos validos por 500 ms las estrellas pasan a lento y
//      la luz a ambar. PARO (IO34, con USAR_PARO) y la pausa p detienen todo.
//   5. WiFi: se une a la red "ASTRA" que crea el Arduino y atiende a la
//      pagina web en http://192.168.4.200 (/estado y /cmd?c=...). La red
//      corre en el nucleo 0; motores, luz y deteccion siguen en el nucleo 1,
//      asi que la pagina nunca frena a los motores. Sin pagina, el atril
//      funciona igual con el boton BOOT y el monitor serie.
//
//  Comandos (monitor serie a 115200, "Sin ajuste de linea", o la pagina):
//   c = comenzar (o repetir) la calibracion: queda esperando el primer paso
//   1 = comenzar REPOSO   2 = comenzar PASIVA   3 = comenzar ACTIVA
//   x = cancelar la fase que se esta midiendo
//   s = iniciar la sesion (con la calibracion LISTA)
//   n = siguiente paso (lo mismo que el boton BOOT)
//   p = pausa / seguir   r = reanudar tras el paro
//   h = volver a buscar el tope   + / - = umbral   i = info de calibracion
//  Boton BOOT de la placa: siguiente paso de la calibracion; en sesion,
//  recalibrar; tras el paro, reanudar.
//
//  Placa: "ESP32 Dev Module". Bibliotecas: AccelStepper. WiFi y WebServer
//  vienen con el paquete de placas ESP32 (el DFPlayer se maneja con
//  comandos directos, no necesita su biblioteca).
//
//  Cambios v3.0 respecto a v2.3 (mismas reglas de calibracion y deteccion):
//   - Calibracion paso a paso: tras "c" el atril ESPERA; cada fase (reposo,
//     pasiva, activa) empieza solo cuando se pide (pagina, BOOT o 1/2/3) y
//     al terminar vuelve a esperar. Cada fase se puede repetir o cancelar.
//     Los umbrales de la sesion solo cambian cuando las tres fases estan
//     hechas y la calibracion es valida. CALIBRACION_AUTOMATICA = true
//     vuelve a la calibracion seguida de v2.3.
//   - Al terminar, la calibracion queda LISTA y la sesion empieza con s,
//     BOOT o el boton "Iniciar sesion" de la pagina.
//   - WiFi + servidor web para la pagina (estado en vivo, RMS de cada
//     bloque, activaciones y mensajes; comandos).
//   - Cada sesion nueva (tras calibrar) reinicia el conteo de activaciones
//     y el tiempo de sesion (sin contar las pausas).
//   - La luz queda blanca tenue mientras la calibracion espera el siguiente
//     paso.
//  Para probar motores, luz, microswitch y opto por separado: prueba_atril.ino
// =====================================================================

#include <AccelStepper.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdarg.h>

// ------------------------- Pines (esquematico) -------------------------
// Motor de giro (J4):      IN1..IN4 = IO23, IO22, IO21, IO19
// Motor de estrellas (J3): IN1..IN4 = IO18, IO17, IO16, IO4
// El 28BYJ-48 necesita el orden IN1, IN3, IN2, IN4 en AccelStepper.
// El "false" final: los pines se configuran en setup() con las bobinas apagadas.
// Si un motor VIBRA en vez de girar parejo, cambia su ORDEN_B a true
// (prueba primero con la tecla o de prueba_atril.ino).
const bool ORDEN_B_PLANETAS  = false;
const bool ORDEN_B_ESTRELLAS = false;
AccelStepper planetas(AccelStepper::HALF4WIRE, 23, ORDEN_B_PLANETAS ? 22 : 21, ORDEN_B_PLANETAS ? 21 : 22, 19, false);
AccelStepper estrellas(AccelStepper::HALF4WIRE, 18, ORDEN_B_ESTRELLAS ? 17 : 16, ORDEN_B_ESTRELLAS ? 16 : 17, 4, false);

const int PIN_ROJO  = 25;   // LEDR -> ULN2803 I1 + I2
const int PIN_VERDE = 33;   // LEDG -> ULN2803 I3 + I4
const int PIN_AZUL  = 32;   // LEDB -> ULN2803 I5 + I6
const int PIN_TOPE  = 13;   // microswitch (C a GND, NO a IO13, pull-up R8)
const int PIN_PARO  = 34;   // divisor 10k/15k desde 5VA (alto = paro suelto)
const int PIN_BOOT  = 0;    // boton BOOT de la placa
const int EMG_RX    = 35;   // datos del opto (pull-up R4 de 2.2k)
const int EMG_TX    = 14;   // Serial2 necesita un TX: no conectes nada en IO14
const int DF_RX     = 27;   // TX del DFPlayer
const int DF_TX     = 26;   // RX del DFPlayer (por R5 de 1k)

// ------------------------- Ajustes mecanicos --------------------------
const int  DIR_SUBIR     = 1;      // si las estrellas BAJAN al buscar el tope, cambia a -1
const long RECORRIDO     = 17400;  // pasos del fondo al tope (40 cm, carrete de 3 cm)
const int  V_HOMING      = 400;    // pasos/s al buscar el tope
const int  V_VAIVEN_RAPIDO = 700;  // pasos/s subiendo y bajando con el musculo RELAJADO
const int  V_VAIVEN_LENTO  = 250;  // pasos/s con el musculo ACTIVO, calibrando o sin datos
const int  ACELERACION   = 800;    // pasos/s^2
const int  V_PLANETAS    = 120;    // pasos/s (~1.8 rpm, una vuelta del eje en ~34 s). Hasta ~400 si quieres mas rapido
const int  ACEL_PLANETAS = 200;    // pasos/s^2: arranque suave del lazy susan
const long TOPE_SUELTA   = 1000;   // pasos: al bajar del tope, el microswitch debe soltarse antes de esto (~2 cm)

// ------------------------- Ajustes de deteccion -----------------------
const float UMBRAL_PCT_INICIAL = 0.25;  // umbral minimo = 25 % de la envolvente activa (+ / - lo cambian)
const unsigned long T_CONFIRMAR = 300;  // ms sobre/bajo el umbral para cambiar de estado
const unsigned long HOLGURA_MS  = 50;   // tolera que un bloque llegue unos ms antes (300 ms = 4 bloques seguidos)
const unsigned long T_PREMIO    = 1000; // ms sostenidos para el sonido de activacion
const unsigned long WATCHDOG_MS = 500;  // sin datos por mas de esto -> todo se detiene
const float SUAVIZADO           = 0.5;  // 0..1 (mas alto = responde mas rapido)
// ------------------------- Calibracion (documento) ---------------------
// REPOSO 10 s -> PASIVA 10 s -> [ARRIBA 4 s + ABAJO 4 s] x 5 repeticiones = 60 s.
// El tiempo se mide con el "reloj de las muestras": cada bloque que llega del
// Arduino son 100 ms (si se cortan los datos, la calibracion espera).
// La "envolvente" es el RMS de cada bloque de 100 ms que manda el Arduino.
const int   BLOQUES_POR_S      = 10;    // el Arduino manda un bloque cada 100 ms
const float CAL_REPOSO_S       = 10;    // fase 1
const float CAL_PASIVA_S       = 10;    // fase 2
const int   CAL_REPS           = 5;     // fase 3: repeticiones
const float CAL_ARRIBA_S       = 4;     //   segundos arriba
const float CAL_ABAJO_S        = 4;     //   segundos abajo
const float CAL_DESC_REPOSO_S  = 1.0;   // se descarta el primer segundo del reposo
const float CAL_DESC_PASIVA_S  = 0.5;   // se descarta el primer 0.5 s de la pasiva
const float CAL_DESC_INI_S     = 0.7;   // ARRIBA: se omiten los 0.7 s iniciales
const float CAL_DESC_FIN_S     = 0.3;   //         y los 0.3 s finales
const float UMBRAL_K_SD        = 3.0;   // umbral estadistico: media + 3 SD del reposo
const float UMBRAL_OFF_FACTOR  = 0.6;   // umbral_OFF = 0.6 x umbral_ON
const float SNR_MINIMA         = 3.0;   // aviso si la contraccion apenas se distingue
const float ARTEFACTO_MAX_PCT  = 80.0;  // aviso si el movimiento pasivo parece contraccion
// true: las 3 fases corren seguidas y la sesion empieza sola (como v2.3).
// false: cada fase espera a que la pidas (pagina, BOOT o teclas 1 / 2 / 3).
const bool CALIBRACION_AUTOMATICA = false;

// ------------------------- Luz y sonido -------------------------------
const int BRILLO_MAX = 128;        // tope del 50 %
const int PASO_LUZ   = 2;          // por cada 20 ms -> 0 a 128 en ~1.3 s
const int VOLUMEN    = 15;         // 0 a 30
const unsigned long PAUSA_SONIDO = 3000;  // ms minimos entre sonidos

// ------------------------- Enlace y paro ------------------------------
// Que premia el atril:
//   true  = RELAJAR: sin contraccion las estrellas suben rapido y la luz crece
//           en color lila; al contraer esperan 1.5 s y bajan rapido.
//   false = CONTRAER: la logica original del documento.
const bool PREMIO_AL_RELAJAR = true;
const int  LILA_R = 80, LILA_G = 0, LILA_B = 128;   // color del premio al relajar (0..128)
const unsigned long T_BRILLO_RELAJADO = 5000;      // ms relajado hasta llegar al brillo maximo

const bool USAR_TOPE = true;       // false si no hay microswitch: enciende con las estrellas ABAJO
const bool USAR_PARO = false;      // pon true cuando el divisor del paro (R6, R7) este conectado
const bool EMG_INVERTIDO = false;  // pon true si el opto invierte (aviso "!! Llegan bytes ...")
const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta

// ------------------------- WiFi (pagina web) --------------------------
// El Arduino UNO R4 WiFi crea la red; el atril se une con una IP fija.
// Deben coincidir con RED_SSID / RED_CLAVE de arduino_emg.ino y con
// "IP del atril" en los Ajustes de la pagina.
const bool USAR_WIFI = true;
const char *RED_SSID  = "ASTRA";
const char *RED_CLAVE = "astra2026";
const IPAddress IP_ATRIL(192, 168, 4, 200);
const IPAddress IP_PUERTA(192, 168, 4, 1);    // el Arduino
const IPAddress MASCARA(255, 255, 255, 0);
const char *VERSION_ATRIL = "3.0";

// ======================================================================
enum Modo { ARRANQUE, CALIBRANDO, SESION, PAUSA, PARO };
enum ModoEstrellas { E_HOMING, E_CONTROL, E_SIN_TOPE };
const char *NOMBRE_MODO[] = {"ARRANQUE", "CALIBRANDO", "SESION", "PAUSA", "PARO"};

Modo modo = ARRANQUE;
ModoEstrellas modoE = E_HOMING;
const long POS_TOPE  = 0;
const long POS_FONDO = -DIR_SUBIR * RECORRIDO;

// --- Sonido ---
unsigned long ultimoSonido = 0;
int pistaPendiente = 0;
unsigned long dfVolumenEn = 0;

// --- Datos del Arduino ---
char linea[8];
int lineaLen = 0;
bool lineaMala = false;
bool sincronizado = false;          // la primera linea se descarta (puede llegar a medias)
unsigned long tUltimoByte = 0;
unsigned long tUltimoDato = 0;
unsigned long lineasMalas = 0;
unsigned long bytesSinLinea = 0;    // bytes recibidos desde la ultima linea valida
bool avisoBytes = false;
int rmsUltimo = 0;
float rmsSuave = 0;
bool hayDatos = false;
bool datosAntes = false;

// --- Calibracion ---
// ESPERA: espera el siguiente paso. LISTA: las 3 fases hechas y validas.
enum FaseCal { CAL_ESPERA, CAL_REPOSO, CAL_PASIVA, CAL_ARRIBA, CAL_ABAJO, CAL_LISTA, CAL_FALLIDA };
const char *NOMBRE_FASE[] = {"ESPERA", "REPOSO", "PASIVA", "ARRIBA", "ABAJO", "LISTA", "FALLIDA"};
const char *NOMBRE_PASO[] = {"REPOSO", "PASIVA", "ACTIVA"};
FaseCal faseCal = CAL_ESPERA;
bool hecho[3] = {false, false, false};   // reposo, pasiva y activa terminadas
bool faseContrae = false;           // true durante ARRIBA (la luz se pone verde)
int bloquesFase = 0;                // bloques recibidos en la fase actual (reloj de las muestras)
int repCal = 0;                     // repeticion activa en curso (1..5)
double sumaR = 0, suma2R = 0;       // reposo: envolvente y su cuadrado
int nR = 0;
double sumaP = 0;                   // pasiva: envolvente
int nP = 0;
float picoP = 0;
double sumaA = 0, suma2A = 0;       // repeticion actual: envolvente y cuadrado (para el RMS)
int nA = 0;
double sumaRepRms = 0, sumaRepEnv = 0;
int nRepsOK = 0;
// Resultados de cada fase de la calibracion en curso (se ven en la pagina)
float cReposo = 0, cSd = 0, cRestRms = 0, cPasMedia = 0, cPasPico = 0;
float cActRms = 0, cActEnv = 0, cSnr = 0, cArt = 0;
float cRepRms[CAL_REPS];
// Calibracion aplicada (la que usa la sesion)
float restRms = 0, pasivaMedia = 0, pasivaPico = 0;
float activeRms = 0, activeEnv = 0, snr = 0, artefactoPct = 0;
bool calibrado = false;
float reposo = 100, sdReposo = 10, maximo = 1000;
float umbralPct = UMBRAL_PCT_INICIAL;
float umbralOn = 300, umbralOff = 200;

// --- Deteccion ---
bool activo = false;
unsigned long tSobre = 0, tBajo = 0, tInicioActivo = 0;
unsigned long tInicioPremio = 0, tFinPremio = 0;   // cuando empezo / termino el estado premiado
bool premio1 = false, premioCima = false;
float nivel = 0;
int nActivaciones = 0;
float sumaRmsAct = 0;               // RMS de la activacion, desde que paso U_on
int nRmsAct = 0;
float sumaCola = 0;                 // RMS bajo U_off aun sin confirmar (si se relajo, no cuenta)
int nCola = 0;
unsigned long tiempoActivoTotal = 0;
int sesionId = 0;                   // sube con cada sesion nueva (tras calibrar)
unsigned long msSesion = 0;         // tiempo en SESION (sin pausas)
unsigned long msSesionIni = 0;      // msSesion al empezar la activacion en curso

// --- Motores ---
bool subiendo = false, bajando = false, frenando = false;
bool bobinasEstrellas = false, bobinasPlanetas = false;
long pasosHoming = 0;
float velPlanetas = 0;
unsigned long tRampaP = 0;
float velE = 0;                     // velocidad actual del vaiven

// --- Paro y boton ---
bool paroActivo = false;
int paroLectura = HIGH;
unsigned long paroCambio = 0;
int bootAnterior = HIGH;
unsigned long bootCambio = 0;

// --- Luz ---
float luzR = 0, luzG = 0, luzB = 0;
unsigned long ultimaLuz = 0, ultimoEstado = 0;

// --- Pagina web ---
// Lo escribe el nucleo 1 (loop) y lo lee el nucleo 0 (red): se copia con candado.
portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;
const int N_LOG = 24, LARGO_LOG = 160;
char logTxt[N_LOG][LARGO_LOG];      // ultimos mensajes (los mismos del monitor serie)
uint32_t nLog = 0;
const int N_BLQ = 64;               // ultimos 6.4 s de RMS recibidos
int32_t blqVal[N_BLQ];
uint8_t blqFlag[N_BLQ];             // 1 = activacion, 2 = bloque usado en la calibracion
uint32_t nBlq = 0;
struct Activacion { int sid, id; float ini, dur; int rms, pct; };
const int N_ACT = 32;
Activacion actLista[N_ACT];
uint32_t nActLista = 0;
QueueHandle_t colaCmd = NULL;       // comandos de la pagina -> loop
volatile int8_t avisoWifi = 0;      // 1 = conectado, -1 = se perdio (lo imprime el loop)
uint32_t idArranque = 0;            // cambia en cada encendido (la pagina lo detecta)

// ======================================================================
//  Utilidades
// ======================================================================
// Imprime en el monitor serie y guarda el mensaje para la pagina.
void msg(const char *fmt, ...) {
  char t[LARGO_LOG];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(t, sizeof t, fmt, ap);
  va_end(ap);
  Serial.println(t);
  portENTER_CRITICAL(&candado);
  memcpy(logTxt[nLog % N_LOG], t, LARGO_LOG);
  nLog++;
  portEXIT_CRITICAL(&candado);
}

bool topePresionado() {
  if (digitalRead(PIN_TOPE) == HIGH) return false;
  delayMicroseconds(300);
  return digitalRead(PIN_TOPE) == LOW;
}

bool datosOK() {
  return hayDatos && (millis() - tUltimoDato) < WATCHDOG_MS;
}

// Estado que se premia: relajado (PREMIO_AL_RELAJAR) o contrayendo (original)
bool premiado() { return PREMIO_AL_RELAJAR ? !activo : activo; }

// La calibracion esta midiendo una fase (no esperando)
bool midiendo() {
  return faseCal == CAL_REPOSO || faseCal == CAL_PASIVA || faseCal == CAL_ARRIBA || faseCal == CAL_ABAJO;
}

float altura() {  // 0 = fondo, 1 = tope
  float a = 1.0f - (float)labs(estrellas.currentPosition()) / RECORRIDO;
  return constrain(a, 0.0f, 1.0f);
}

// Comando directo al DFPlayer (10 bytes). No espera respuesta, asi los
// motores nunca se detienen esperando al reproductor.
void dfEnviar(uint8_t cmd, uint16_t param) {
  uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, (uint8_t)(param >> 8), (uint8_t)(param & 0xFF), 0, 0, 0xEF};
  uint16_t suma = 0;
  for (int i = 1; i < 7; i++) suma += f[i];
  uint16_t chk = (uint16_t)(0 - suma);
  f[7] = chk >> 8;
  f[8] = chk & 0xFF;
  Serial1.write(f, 10);
}

// Pista N = orden en que se copiaron a la microSD. Si no han pasado 3 s
// desde el ultimo sonido, queda pendiente (actualizarModo la toca despues).
void sonar(int pista) {
  if (millis() - ultimoSonido < PAUSA_SONIDO) { pistaPendiente = pista; return; }
  dfEnviar(0x03, pista);
  ultimoSonido = millis();
  pistaPendiente = 0;
}

void bobinasE(bool on) {
  if (on == bobinasEstrellas) return;
  if (on) estrellas.enableOutputs(); else estrellas.disableOutputs();
  bobinasEstrellas = on;
}

void bobinasP(bool on) {
  if (on == bobinasPlanetas) return;
  if (on) planetas.enableOutputs(); else planetas.disableOutputs();
  bobinasPlanetas = on;
}

// El microswitch marca el tope. La posicion pasa a ~0 conservando la fase de
// las bobinas (0 a 7 medios pasos del lado de arriba, < 0.2 mm), asi el
// siguiente paso sigue la secuencia: sin tiron y sin empujar el microswitch.
void fijarTope() {
  long fase = estrellas.currentPosition() & 7;
  long p = (DIR_SUBIR > 0) ? fase : (fase ? fase - 8 : 0);
  estrellas.setCurrentPosition(POS_TOPE + p);   // tambien detiene el motor ahi mismo
}

void detenerEstrellasYa() {
  estrellas.setCurrentPosition(estrellas.currentPosition());  // alto inmediato, conserva la posicion
  subiendo = bajando = false;
  bobinasE(false);
}

// Frenado suave (800 pasos/s^2). Si ya estan quietas, cancela cualquier destino.
void frenarEstrellas() {
  if (estrellas.speed() == 0) estrellas.moveTo(estrellas.currentPosition());
  else estrellas.stop();
}

// Umbral del documento (6.1): el mayor entre el estadistico y el 25 % de la activa
void calcularUmbrales() {
  umbralOn  = max(reposo + UMBRAL_K_SD * sdReposo, umbralPct * activeEnv);
  umbralOff = UMBRAL_OFF_FACTOR * umbralOn;
}

void imprimirCalibracion() {
  msg("---- Calibracion ----");
  msg("Reposo (envolvente): %.1f mV", reposo);
  msg("Ruido en reposo (SD): %.1f mV", sdReposo);
  msg("Pasiva: media %.1f mV, pico %.1f mV", pasivaMedia, pasivaPico);
  msg("Activa: RMS de referencia %.1f mV (envolvente %.1f mV, %d repeticiones)",
      activeRms, activeEnv, nRepsOK);
  msg("Relacion senal/reposo (SNR): %.1f", snr);
  msg("Artefacto pasivo / activa: %.0f %%", artefactoPct);
  msg("Umbral ON: %.1f mV (mayor de media + %.0f SD = %.1f, y %.0f %% activa = %.1f)",
      umbralOn, UMBRAL_K_SD, reposo + UMBRAL_K_SD * sdReposo,
      umbralPct * 100, umbralPct * activeEnv);
  msg("Umbral OFF: %.1f mV (%.1f x umbral ON)", umbralOff, UMBRAL_OFF_FACTOR);
  msg("Activaciones: %d | tiempo activo total: %.1f s | lineas danadas: %lu",
      nActivaciones, tiempoActivoTotal / 1000.0f, lineasMalas);
  msg("---------------------");
}

// Termina la activacion en curso. 'fin' es cuando el musculo realmente se
// relajo (para la tabla). La espera de 1.5 s antes de bajar cuenta desde ahora.
// Imprime una linea por activacion: sirve para la tabla de resultados.
void terminarActivacion(unsigned long fin) {
  if (activo) {
    activo = false;
    if (PREMIO_AL_RELAJAR) { tInicioPremio = millis(); premio1 = false; }   // empieza a relajar
    else tFinPremio = millis();                                            // termina el premio
    unsigned long dur = fin - tInicioActivo;
    tiempoActivoTotal += dur;
    float rmsMedio = nRmsAct ? sumaRmsAct / nRmsAct : 0;
    int pct = (int)lroundf(100.0f * (rmsMedio - reposo) / max(maximo - reposo, 1.0f));
    msg("   Activacion #%d: %.1f s, RMS medio %.0f mV (%d %% de la referencia)",
        nActivaciones, dur / 1000.0f, rmsMedio, pct);
    Activacion a = {sesionId, nActivaciones, msSesionIni / 1000.0f, dur / 1000.0f, (int)lroundf(rmsMedio), pct};
    portENTER_CRITICAL(&candado);
    actLista[nActLista % N_ACT] = a;
    nActLista++;
    portEXIT_CRITICAL(&candado);
  }
  tSobre = tBajo = 0;
}

// Por cambio de modo o falta de datos. Si ya se estaba relajando, termina ahi.
void cerrarActivacion(unsigned long fin) { terminarActivacion(tBajo ? tBajo : fin); }
void cerrarActivacion() { cerrarActivacion(millis()); }

// ======================================================================
//  Cambios de modo
// ======================================================================
void empezarFase(int f);            // mas abajo

void iniciarArranque() {
  cerrarActivacion();
  modo = ARRANQUE;
  modoE = E_HOMING;
  pasosHoming = 0;
  subiendo = bajando = frenando = false;
  premioCima = false;
  pistaPendiente = 0;
  bobinasP(false);
  bobinasE(true);
  estrellas.setMaxSpeed(V_HOMING > V_VAIVEN_RAPIDO ? V_HOMING : V_VAIVEN_RAPIDO);  // si no, setSpeed lo recorta
  estrellas.setSpeed(DIR_SUBIR * V_HOMING);
  dfVolumenEn = millis() + 2000;     // el DFPlayer pudo reiniciarse (paro): reenvia el volumen
  msg(">> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).");
}

void reiniciarFase(int f) {
  if (f == 0) { sumaR = suma2R = 0; nR = 0; }
  else if (f == 1) { sumaP = 0; nP = 0; picoP = 0; }
  else {
    sumaA = suma2A = 0; nA = 0;
    sumaRepRms = sumaRepEnv = 0; nRepsOK = 0;
    for (int i = 0; i < CAL_REPS; i++) cRepRms[i] = 0;
  }
  hecho[f] = false;
}

// Deja la calibracion lista para empezar: espera el primer paso.
void iniciarCalibracion() {
  cerrarActivacion();
  modo = CALIBRANDO;
  faseCal = CAL_ESPERA;
  faseContrae = false;
  bloquesFase = 0; repCal = 0;
  for (int f = 0; f < 3; f++) reiniciarFase(f);
  cReposo = cSd = cRestRms = cPasMedia = cPasPico = cActRms = cActEnv = cSnr = cArt = 0;
  pistaPendiente = 0;
  msg(">> CALIBRACION (60 s): REPOSO 10 s -> PASIVA 10 s -> 5 x (ARRIBA 4 s + ABAJO 4 s)");
  if (CALIBRACION_AUTOMATICA) empezarFase(0);
  else msg(">> Esperando: comienza el REPOSO desde la pagina, con BOOT o con la tecla 1.");
}

void iniciarSesion(bool nueva) {
  if (nueva) {                     // tras calibrar: sesion nueva desde cero
    sesionId++;
    nActivaciones = 0;
    tiempoActivoTotal = 0;
    msSesion = 0;
  }
  modo = SESION;
  bajando = false;                 // el vaiven sigue donde iba
  activo = false;
  tSobre = tBajo = 0;
  tInicioPremio = tFinPremio = millis();   // tras una pausa esperan 1.5 s antes de bajar
  premio1 = false;
  rmsSuave = reposo;               // no arrastra la contraccion de la calibracion
  msg(PREMIO_AL_RELAJAR ? ">> SESION: relaja para subir las estrellas."
                        : ">> SESION: contrae para subir las estrellas.");
}

// Calcula los resultados con las 3 fases hechas y, si es valida, la aplica.
void terminarCalibracion() {
  faseContrae = false;
  bloquesFase = 0;
  cSnr = cRestRms > 0 ? cActRms / cRestRms : 0;
  cArt = cActEnv > 0 ? 100.0f * cPasPico / cActEnv : 0;

  // Advertencias automaticas
  if (cActRms <= 0) {
    msg("!! No se detecto contraccion activa. Revisa electrodos y repite la fase ACTIVA (3) o toda la calibracion (c).");
    msg("!! La terapia no puede comenzar sin una calibracion valida.");
    faseCal = CAL_FALLIDA;
    return;                           // no se aplica
  }

  // Aplica la calibracion nueva
  reposo = cReposo;
  sdReposo = cSd;
  restRms = cRestRms;
  pasivaMedia = cPasMedia;
  pasivaPico = cPasPico;
  activeRms = cActRms;
  activeEnv = cActEnv;
  snr = cSnr;
  artefactoPct = cArt;
  maximo = activeEnv;                 // el 100 % del esfuerzo de ese nino hoy
  calcularUmbrales();
  imprimirCalibracion();

  if (snr < SNR_MINIMA)
    msg("!! SNR < 3: la contraccion apenas se distingue del reposo (mal contacto o electrodo mal ubicado).");
  if (artefactoPct > ARTEFACTO_MAX_PCT)
    msg("!! Artefacto > 80 %%: las transiciones podrian contarse como activaciones (sube el umbral con + o mejora la fijacion de cables).");

  calibrado = true;
  faseCal = CAL_LISTA;
  if (CALIBRACION_AUTOMATICA) iniciarSesion(true);
  else msg(">> Calibracion LISTA. Inicia la sesion desde la pagina, con BOOT o con la tecla s.");
}

// Una fase termino: si ya estan las 3, resultados; si no, espera el siguiente paso.
void finFase() {
  if (hecho[0] && hecho[1] && hecho[2]) { terminarCalibracion(); return; }
  faseCal = CAL_ESPERA;
  faseContrae = false;
  bloquesFase = 0;
  int sig = !hecho[0] ? 0 : !hecho[1] ? 1 : 2;
  if (CALIBRACION_AUTOMATICA) empezarFase(sig);
  else msg(">> Esperando: comienza la fase %s desde la pagina, con BOOT o con la tecla %d.", NOMBRE_PASO[sig], sig + 1);
}

// Avanza la calibracion con cada bloque que llega (reloj de las muestras)
void cambiarFaseCal(FaseCal f) {
  faseCal = f;
  bloquesFase = 0;
  faseContrae = (f == CAL_ARRIBA);
  if (f == CAL_REPOSO)
    msg(">> FASE 1 - REPOSO (%.0f s): cabeza apoyada y relajada. No mover al nino.", CAL_REPOSO_S);
  else if (f == CAL_PASIVA)
    msg(">> FASE 2 - PASIVA (%.0f s): el terapeuta mueve la cabeza de abajo hacia arriba y viceversa. El nino NO hace fuerza.", CAL_PASIVA_S);
  else if (f == CAL_ARRIBA)
    msg(">> FASE 3 - ACTIVA, repeticion %d/%d: ARRIBA! El nino levanta la cabeza y la sostiene (%.0f s).",
        repCal, CAL_REPS, CAL_ARRIBA_S);
  else if (f == CAL_ABAJO)
    msg("   ABAJO: baja la cabeza y relaja (%.0f s).", CAL_ABAJO_S);
}

// Empieza a medir una fase (0 reposo, 1 pasiva, 2 activa). Se puede repetir
// una fase ya hecha; para avanzar hay que seguir el orden.
void empezarFase(int f) {
  if (modo != CALIBRANDO) { msg("!! Primero comienza la calibracion (c)."); return; }
  if (midiendo()) { msg("!! Ya se esta midiendo la fase %s. Espera a que termine o cancelala (x).", NOMBRE_FASE[faseCal]); return; }
  if (f == 1 && !hecho[0]) { msg("!! Primero haz la fase de REPOSO (1)."); return; }
  if (f == 2 && !hecho[1]) { msg("!! Primero haz la fase PASIVA (2)."); return; }
  reiniciarFase(f);
  if (f == 0) cambiarFaseCal(CAL_REPOSO);
  else if (f == 1) cambiarFaseCal(CAL_PASIVA);
  else { repCal = 1; cambiarFaseCal(CAL_ARRIBA); }
}

void cancelarFase() {
  if (modo != CALIBRANDO || !midiendo()) return;
  int f = faseCal == CAL_REPOSO ? 0 : faseCal == CAL_PASIVA ? 1 : 2;
  reiniciarFase(f);
  faseCal = CAL_ESPERA;
  faseContrae = false;
  bloquesFase = 0;
  msg(">> Fase %s cancelada. Puedes volver a empezarla.", NOMBRE_PASO[f]);
}

// Devuelve true si el bloque se uso para la calibracion (no descartado)
bool calibrarBloque(int mv) {
  float env = mv;                     // envolvente = RMS del bloque de 100 ms
  bool usado = false;
  if (!midiendo()) return false;
  bloquesFase++;
  switch (faseCal) {
    case CAL_REPOSO:
      if (bloquesFase > (int)lroundf(CAL_DESC_REPOSO_S * BLOQUES_POR_S)) {
        sumaR += env; suma2R += (double)env * env; nR++;
        usado = true;
      }
      if (bloquesFase >= (int)lroundf(CAL_REPOSO_S * BLOQUES_POR_S)) {
        cReposo  = nR ? sumaR / nR : 0;
        float var = nR ? suma2R / nR - (double)cReposo * cReposo : 0;
        cSd      = sqrtf(max(var, 0.0f));
        cRestRms = nR ? sqrt(suma2R / nR) : 0;
        hecho[0] = true;
        msg("   Reposo listo: media %.1f mV, SD %.1f mV.", cReposo, cSd);
        finFase();
      }
      break;

    case CAL_PASIVA:
      if (bloquesFase > (int)lroundf(CAL_DESC_PASIVA_S * BLOQUES_POR_S)) {
        sumaP += env; nP++;
        if (env > picoP) picoP = env;
        usado = true;
      }
      if (bloquesFase >= (int)lroundf(CAL_PASIVA_S * BLOQUES_POR_S)) {
        cPasMedia = nP ? sumaP / nP : 0;
        cPasPico  = picoP;
        hecho[1] = true;
        msg("   Pasiva lista: media %.1f mV, pico %.1f mV.", cPasMedia, cPasPico);
        finFase();
      }
      break;

    case CAL_ARRIBA: {
      int total = (int)lroundf(CAL_ARRIBA_S * BLOQUES_POR_S);
      int ini = (int)lroundf(CAL_DESC_INI_S * BLOQUES_POR_S);
      int fin = (int)lroundf(CAL_DESC_FIN_S * BLOQUES_POR_S);
      if (bloquesFase > ini && bloquesFase <= total - fin) {     // solo la ventana estable
        sumaA += env; suma2A += (double)env * env; nA++;
        usado = true;
      }
      if (bloquesFase >= total) {
        if (nA > 0) {
          float rms = sqrt(suma2A / nA), media = sumaA / nA;
          sumaRepRms += rms; sumaRepEnv += media; nRepsOK++;
          cRepRms[repCal - 1] = rms;
          msg("   Repeticion %d: RMS %.1f mV, envolvente %.1f mV", repCal, rms, media);
        }
        sumaA = suma2A = 0; nA = 0;
        cambiarFaseCal(CAL_ABAJO);
      }
      break;
    }

    case CAL_ABAJO:
      if (bloquesFase >= (int)lroundf(CAL_ABAJO_S * BLOQUES_POR_S)) {
        if (repCal < CAL_REPS) { repCal++; cambiarFaseCal(CAL_ARRIBA); }
        else {
          cActRms = nRepsOK ? sumaRepRms / nRepsOK : 0;
          cActEnv = nRepsOK ? sumaRepEnv / nRepsOK : 0;
          hecho[2] = true;
          msg("   Activa lista: RMS de referencia %.1f mV, envolvente %.1f mV (%d repeticiones).", cActRms, cActEnv, nRepsOK);
          finFase();
        }
      }
      break;

    default:
      break;
  }
  return usado;
}

void entrarParo() {
  modo = PARO;
  cerrarActivacion();
  detenerEstrellasYa();
  bobinasP(false);
  pistaPendiente = 0;
  msg("!! PARO DE EMERGENCIA. Al soltarlo, presiona BOOT o escribe r para reanudar.");
}

void reanudarTrasParo() {
  if (paroActivo) { msg("El paro sigue presionado: giralo para soltarlo."); return; }
  iniciarArranque();                 // vuelve a buscar el tope antes de moverse (y reenvia el volumen)
}

// Boton BOOT o tecla n: el siguiente paso que toca
void siguientePaso() {
  if (modo == PARO) { reanudarTrasParo(); return; }
  if (modo == SESION || modo == PAUSA) { iniciarCalibracion(); return; }
  if (modo != CALIBRANDO || midiendo()) return;
  if (faseCal == CAL_LISTA) { iniciarSesion(true); return; }
  if (faseCal == CAL_FALLIDA) { empezarFase(2); return; }
  empezarFase(!hecho[0] ? 0 : !hecho[1] ? 1 : 2);
}

// Comandos del monitor serie y de la pagina (todos pasan por aqui, en el loop)
void ejecutarComando(char c) {
  switch (c) {
    case 'c':
      if (modo == ARRANQUE) msg("!! Espera a que las estrellas encuentren el tope.");
      else if (modo == PARO) msg("!! Primero suelta el paro y reanuda (r).");
      else iniciarCalibracion();
      break;
    case '1': empezarFase(0); break;
    case '2': empezarFase(1); break;
    case '3': empezarFase(2); break;
    case 'x': cancelarFase(); break;
    case 's':
      if (modo == CALIBRANDO && faseCal == CAL_LISTA) iniciarSesion(true);
      else msg("!! La sesion empieza cuando la calibracion este LISTA (reposo, pasiva y activa).");
      break;
    case 'n': siguientePaso(); break;
    case 'p':
      if (modo == SESION) { cerrarActivacion(); modo = PAUSA; msg(">> PAUSA (p para seguir)"); }
      else if (modo == PAUSA) iniciarSesion(false);
      break;
    case 'r': if (modo == PARO) reanudarTrasParo(); break;
    case 'h': if (modo != PARO) iniciarArranque(); break;
    case '+': umbralPct = min(umbralPct + 0.05f, 0.80f); calcularUmbrales(); imprimirCalibracion(); break;
    case '-': umbralPct = max(umbralPct - 0.05f, 0.05f); calcularUmbrales(); imprimirCalibracion(); break;
    case 'i': imprimirCalibracion(); break;
    default: break;
  }
}

// ======================================================================
//  Entradas
// ======================================================================
void guardarBloque(int mv, uint8_t flag) {
  portENTER_CRITICAL(&candado);
  blqVal[nBlq % N_BLQ] = mv;
  blqFlag[nBlq % N_BLQ] = flag;
  nBlq++;
  portEXIT_CRITICAL(&candado);
}

void procesarRms(int mv) {
  unsigned long ahora = millis();
  rmsUltimo = mv;
  rmsSuave = hayDatos ? SUAVIZADO * mv + (1 - SUAVIZADO) * rmsSuave : mv;
  hayDatos = true;
  tUltimoDato = ahora;

  if (modo == CALIBRANDO) {
    guardarBloque(mv, calibrarBloque(mv) ? 2 : 0);
    return;
  }
  if (modo != SESION) { guardarBloque(mv, 0); return; }

  nivel = constrain((rmsSuave - reposo) / max(maximo - reposo, 1.0f), 0.0f, 1.0f);

  // Histeresis + confirmacion en el tiempo
  if (!activo) {
    if (rmsSuave >= umbralOn) {
      if (!tSobre) { tSobre = ahora; sumaRmsAct = 0; nRmsAct = 0; msSesionIni = msSesion; }
      sumaRmsAct += mv; nRmsAct++;
      if (ahora - tSobre + HOLGURA_MS >= T_CONFIRMAR) {
        activo = true;
        tInicioActivo = tSobre;
        if (PREMIO_AL_RELAJAR) tFinPremio = ahora;            // contraer termina el premio
        else { tInicioPremio = tSobre; premio1 = false; }
        nActivaciones++;
        tBajo = 0;
      }
    } else tSobre = 0;
  } else {
    if (rmsSuave < umbralOff) {
      if (!tBajo) { tBajo = ahora; sumaCola = 0; nCola = 0; }
      sumaCola += mv; nCola++;
      if (ahora - tBajo + HOLGURA_MS >= T_CONFIRMAR) terminarActivacion(tBajo);
    } else {
      if (tBajo) { sumaRmsAct += sumaCola; nRmsAct += nCola; tBajo = 0; }  // fue una baja corta
      sumaRmsAct += mv; nRmsAct++;
    }
  }

  guardarBloque(mv, activo ? 1 : 0);
}

void leerArduino() {
  while (Serial2.available()) {
    char c = Serial2.read();
    unsigned long ahora = millis();
    bytesSinLinea++;
    if (lineaLen > 0 && ahora - tUltimoByte > CORTE_LINEA_MS) lineaMala = true;  // linea cortada
    tUltimoByte = ahora;

    if (c >= '0' && c <= '9') {
      if (lineaLen < 6) linea[lineaLen++] = c; else lineaMala = true;
    } else if (c == '\n') {
      if (!sincronizado) sincronizado = true;     // la primera linea puede venir a medias
      else if (lineaLen > 0 && !lineaMala) {
        linea[lineaLen] = 0;
        bytesSinLinea = 0;
        procesarRms(atoi(linea));
      }
      else if (lineaLen > 0 || lineaMala) lineasMalas++;
      lineaLen = 0; lineaMala = false;
    } else if (c != '\r') {
      lineaMala = true;
    }
  }

  // ~4 s de bytes sin ninguna linea valida: casi siempre es polaridad o baudios
  if (!avisoBytes && bytesSinLinea > 200) {
    avisoBytes = true;
    msg("!! Llegan bytes del Arduino pero ninguna linea valida.");
    msg("!! Revisa que ambos esten a 9600 baud, o cambia EMG_INVERTIDO y vuelve a cargar.");
  }
}

void leerComandos() {
  while (Serial.available()) ejecutarComando(Serial.read());
  char c;
  while (colaCmd && xQueueReceive(colaCmd, &c, 0) == pdTRUE) ejecutarComando(c);
}

void revisarBoot() {
  int v = digitalRead(PIN_BOOT);
  if (v != bootAnterior && millis() - bootCambio > 50) {
    bootCambio = millis();
    bootAnterior = v;
    if (v == LOW) siguientePaso();  // al presionar
  }
}

void revisarParo() {
  if (!USAR_PARO) return;
  int v = digitalRead(PIN_PARO);
  if (v != paroLectura) { paroLectura = v; paroCambio = millis(); }
  if (millis() - paroCambio < 30) return;          // antirrebote
  bool presionado = (paroLectura == LOW);
  if (presionado && !paroActivo) { paroActivo = true; entrarParo(); }
  else if (!presionado && paroActivo) {
    paroActivo = false;
    msg(">> Paro liberado. Presiona BOOT o escribe r para reanudar.");
  }
}

// ======================================================================
//  Logica principal
// ======================================================================
void actualizarModo() {
  unsigned long ahora = millis();

  // Tiempo de sesion (sin pausas)
  static unsigned long tPrevio = 0;
  if (modo == SESION && tPrevio) msSesion += ahora - tPrevio;
  tPrevio = ahora;

  // Avisos de la red (los pone la tarea de WiFi; se imprimen aqui)
  int8_t aw = avisoWifi;
  if (aw) {
    avisoWifi = 0;
    if (aw > 0) msg(">> WiFi conectado a la red %s. Pagina: http://%s", RED_SSID, WiFi.localIP().toString().c_str());
    else msg("!! Se perdio la red WiFi %s (el atril sigue funcionando; reintentando).", RED_SSID);
  }

  // Watchdog: avisa una vez al perder / recuperar los datos
  bool ok = datosOK();
  if (ok != datosAntes) {
    datosAntes = ok;
    if (!ok) {
      cerrarActivacion(tUltimoDato);                 // no cuenta el tiempo sin senal
      msg("!! Sin datos del Arduino: estrellas lentas y luz ambar hasta que vuelvan.");
    } else {
      if (!activo) { tInicioPremio = tFinPremio = ahora; premio1 = false; }  // 1.5 s para reconfirmar
      msg(">> Llegan datos del Arduino.");
    }
  }

  // La calibracion avanza con cada bloque recibido (calibrarBloque): sin datos, espera.

  // Sonido: estado premiado sostenido 1 s
  if (modo == SESION && premiado() && ok && !premio1 && ahora - tInicioPremio >= T_PREMIO) {
    premio1 = true;
    sonar(1);
  }

  // Pista pendiente: suena al cumplirse la pausa, si su motivo sigue vigente
  if (pistaPendiente) {
    bool vigente = (modo == SESION) && ok && premiado() && (pistaPendiente == 1 || premioCima);
    if (!vigente) pistaPendiente = 0;
    else if (ahora - ultimoSonido >= PAUSA_SONIDO) sonar(pistaPendiente);
  }

  // Reenvio del volumen (el DFPlayer pierde la configuracion si se queda sin energia)
  if (dfVolumenEn && (long)(ahora - dfVolumenEn) >= 0) {
    dfVolumenEn = 0;
    dfEnviar(0x06, VOLUMEN);
  }

  // Descarta lo que manda el DFPlayer (avisos de fin de pista, etc.)
  while (Serial1.available()) Serial1.read();
}

// Vaiven: velocidad segun el estado del musculo
float velVaiven() {
  if (modo == SESION && datosOK() && premiado()) return V_VAIVEN_RAPIDO;   // relajado: rapido
  return V_VAIVEN_LENTO;          // contrayendo, calibrando o sin datos: lento
}

void empezarVaiven() {
  modoE = E_CONTROL;
  subiendo = frenando = false;
  bobinasE(true);
  velE = V_VAIVEN_LENTO;
  estrellas.setMaxSpeed(velE);
  estrellas.setAcceleration(ACELERACION);
  estrellas.moveTo(POS_FONDO);       // primero bajan, luego van y vienen
}

void moverEstrellas() {
  switch (modoE) {
    case E_HOMING:
      if (modo == PARO) return;
      if (!USAR_TOPE) {
        estrellas.setCurrentPosition(POS_FONDO);
        msg(">> USAR_TOPE = false: cuento desde el FONDO (las estrellas deben arrancar abajo).");
        empezarVaiven();
        if (calibrado) iniciarSesion(false); else iniciarCalibracion();
      } else if (topePresionado()) {
        fijarTope();
        msg(">> Tope encontrado. Las estrellas empiezan a subir y bajar.");
        empezarVaiven();
        if (calibrado) iniciarSesion(false); else iniciarCalibracion();
      } else if (estrellas.runSpeed()) {
        if (++pasosHoming > RECORRIDO * 13 / 10) {
          bobinasE(false);
          modoE = E_SIN_TOPE;
          msg("!! No encontre el tope. Las estrellas quedan desactivadas (revisa microswitch y DIR_SUBIR,");
          msg("!! o pon USAR_TOPE = false y enciende con las estrellas abajo).");
          if (calibrado) iniciarSesion(false); else iniciarCalibracion();
        }
      }
      return;
    case E_SIN_TOPE:
      return;
    case E_CONTROL:
      break;
  }

  // ---- Vaiven continuo entre el fondo y el tope ----
  if (modo == PARO) return;
  if (modo == PAUSA) {
    if (!frenando) { frenarEstrellas(); frenando = true; }
  } else {
    if (frenando) { frenando = false; estrellas.moveTo(subiendo ? POS_TOPE : POS_FONDO); }
    float v = velVaiven();
    if (v != velE) { velE = v; estrellas.setMaxSpeed(v); }
    if (estrellas.distanceToGo() == 0) {               // llego a un extremo: cambia de sentido
      subiendo = !subiendo;
      estrellas.moveTo(subiendo ? POS_TOPE : POS_FONDO);
    }
  }

  if (USAR_TOPE) {
    // Microswitch: tope real. Si se presiona subiendo, resincroniza y da la vuelta.
    if ((subiendo || estrellas.speed() * DIR_SUBIR > 0) && topePresionado()) fijarTope();
    // Bajando lejos del tope no puede estar presionado: cableado en NC o atorado.
    else if (!subiendo && labs(estrellas.currentPosition() - POS_TOPE) > TOPE_SUELTA && topePresionado()) {
      detenerEstrellasYa();
      modoE = E_SIN_TOPE;
      msg("!! El microswitch sigue presionado lejos del tope. Revisa que este en NO (no en NC).");
      msg("!! Las estrellas quedan desactivadas; usa h al arreglarlo.");
      return;
    }
  }

  if (estrellas.distanceToGo() != 0) { bobinasE(true); estrellas.run(); }
  else bobinasE(false);

  // Premio al llegar arriba relajado
  float a = altura();
  if (!premioCima && premiado() && a >= 0.99f && modo == SESION && datosOK()) {
    premioCima = true;
    sonar(2);
    msg("   *** Estrellas arriba ***");
  } else if (premioCima && a < 0.80f) {
    premioCima = false;
  }
}

// El lazy susan gira siempre (desde el encendido). Solo para con PARO o la pausa p.
void moverPlanetas() {
  if (modo != PARO && modo != PAUSA) {
    bobinasP(true);
    unsigned long ahora = millis();
    if (velPlanetas < V_PLANETAS && ahora - tRampaP >= 20) {   // arranque suave
      tRampaP = ahora;
      velPlanetas = min(velPlanetas + ACEL_PLANETAS * 0.02f, (float)V_PLANETAS);
      planetas.setSpeed(velPlanetas);
    }
    planetas.runSpeed();
  } else {
    bobinasP(false);
    velPlanetas = 0;                 // al volver, arranca suave otra vez
  }
}

void actualizarLuz() {
  unsigned long ahora = millis();
  if (ahora - ultimaLuz < 20) return;
  ultimaLuz = ahora;

  float r = 0, g = 0, b = 0;
  switch (modo) {
    case ARRANQUE:   r = 15; g = 15; b = 15; break;                 // blanco tenue
    case CALIBRANDO:
      if (!midiendo()) { r = 15; g = 15; b = 15; }                  // blanco tenue: espera el siguiente paso
      else if (faseContrae) g = 90; else b = 70;                    // verde / azul
      break;
    case PAUSA:      r = 15; g = 15; b = 15; break;
    case PARO:       break;                                         // apagada
    case SESION:
      if (!datosOK())               { r = 40; g = 10; }             // ambar: sin senal
      else if (premioCima && premiado()) { g = 110; b = 50; }       // verde azulado: arriba
      else if (premiado() && PREMIO_AL_RELAJAR) {                    // lila que crece al relajar
        float k = min((float)(ahora - tInicioPremio) / T_BRILLO_RELAJADO, 1.0f);
        float f = 0.25f + 0.75f * k;
        r = LILA_R * f; g = LILA_G * f; b = LILA_B * f;
      }
      else if (premiado())          { r = 40 + 88 * nivel; g = 15 + 45 * nivel; }  // dorado
      else                          { b = 25; }                      // azul tenue
      break;
  }
  r = min(r, (float)BRILLO_MAX); g = min(g, (float)BRILLO_MAX); b = min(b, (float)BRILLO_MAX);

  // Cambio gradual: nunca de golpe ni parpadeando
  luzR += constrain(r - luzR, (float)-PASO_LUZ, (float)PASO_LUZ);
  luzG += constrain(g - luzG, (float)-PASO_LUZ, (float)PASO_LUZ);
  luzB += constrain(b - luzB, (float)-PASO_LUZ, (float)PASO_LUZ);
  analogWrite(PIN_ROJO,  (int)luzR);
  analogWrite(PIN_VERDE, (int)luzG);
  analogWrite(PIN_AZUL,  (int)luzB);
}

void imprimirEstado() {
  unsigned long ahora = millis();
  if (ahora - ultimoEstado < 500) return;
  ultimoEstado = ahora;
  if (modo == SESION) {
    char est[16];
    if (modoE == E_SIN_TOPE) snprintf(est, sizeof est, "DESACTIVADAS");
    else snprintf(est, sizeof est, "%3d %%", (int)(altura() * 100));
    Serial.printf("[SESION] RMS %4d mV (umbral %.0f) | nivel %3d %% | %s | estrellas %s | activaciones %d%s\n",
                  rmsUltimo, umbralOn, (int)(nivel * 100), activo ? "ACTIVO" : "reposo",
                  est, nActivaciones, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else if (modo == CALIBRANDO) {
    if (faseCal == CAL_ARRIBA || faseCal == CAL_ABAJO)
      Serial.printf("[CALIBRANDO %s %d/%d] RMS %4d mV%s\n", NOMBRE_FASE[faseCal], repCal, CAL_REPS,
                    rmsUltimo, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
    else
      Serial.printf("[CALIBRANDO %s] RMS %4d mV%s\n", NOMBRE_FASE[faseCal],
                    rmsUltimo, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else {
    Serial.printf("[%s]%s\n", NOMBRE_MODO[modo], datosOK() ? "" : " sin datos del Arduino");
  }
}

// ======================================================================
//  Pagina web (nucleo 0). Solo lee el estado y encola comandos: todo lo
//  que cambia el atril lo hace el loop con ejecutarComando().
// ======================================================================
WebServer web(80);
static char js[8192];               // respuesta de /estado
static int jl = 0;

void ja(const char *fmt, ...) {
  if (jl >= (int)sizeof(js) - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(js + jl, sizeof(js) - jl, fmt, ap);
  va_end(ap);
  if (n > 0) jl = min(jl + n, (int)sizeof(js) - 1);
}

// Texto entre comillas, escapando lo necesario para JSON
void jaTexto(const char *t) {
  ja("\"");
  for (; *t && jl < (int)sizeof(js) - 8; t++) {
    unsigned char c = *t;
    if (c == '"' || c == '\\') ja("\\%c", c);
    else if (c < 0x20) ja(" ");
    else { js[jl++] = c; js[jl] = 0; }
  }
  ja("\"");
}

void cabeceras() {
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.sendHeader("Access-Control-Allow-Private-Network", "true");
  web.sendHeader("Cache-Control", "no-store");
}

// Primer indice a mandar: lo que pide la pagina, dentro de lo que se guarda
uint32_t desde(const char *arg, uint32_t total, uint32_t guardados) {
  long q = web.hasArg(arg) ? web.arg(arg).toInt() : 0;
  uint32_t ini = total > guardados ? total - guardados : 0;
  if (q < (long)ini) q = ini;
  if (q > (long)total) q = total;
  return (uint32_t)q;
}

void manejarEstado() {
  static int32_t bv[N_BLQ];
  static uint8_t bf[N_BLQ];
  static Activacion av[N_ACT];
  static char lt[N_LOG][LARGO_LOG];

  // Copia de lo que cambia en el otro nucleo
  uint32_t nb, na, nl, b0, a0, l0;
  portENTER_CRITICAL(&candado);
  nb = nBlq; na = nActLista; nl = nLog;
  portEXIT_CRITICAL(&candado);
  b0 = desde("b", nb, N_BLQ);
  a0 = desde("a", na, N_ACT);
  l0 = desde("l", nl, N_LOG);
  portENTER_CRITICAL(&candado);
  for (uint32_t i = b0; i < nb; i++) { bv[i - b0] = blqVal[i % N_BLQ]; bf[i - b0] = blqFlag[i % N_BLQ]; }
  for (uint32_t i = a0; i < na; i++) av[i - a0] = actLista[i % N_ACT];
  for (uint32_t i = l0; i < nl; i++) memcpy(lt[i - l0], logTxt[i % N_LOG], LARGO_LOG);
  portEXIT_CRITICAL(&candado);

  int total = faseCal == CAL_REPOSO ? (int)lroundf(CAL_REPOSO_S * BLOQUES_POR_S)
            : faseCal == CAL_PASIVA ? (int)lroundf(CAL_PASIVA_S * BLOQUES_POR_S)
            : faseCal == CAL_ARRIBA ? (int)lroundf(CAL_ARRIBA_S * BLOQUES_POR_S)
            : faseCal == CAL_ABAJO  ? (int)lroundf(CAL_ABAJO_S * BLOQUES_POR_S) : 0;
  unsigned long ahora = millis();
  bool act = activo;

  jl = 0; js[0] = 0;
  ja("{\"v\":\"%s\",\"boot\":%lu,\"modo\":\"%s\",\"fase\":\"%s\"", VERSION_ATRIL, (unsigned long)idArranque, NOMBRE_MODO[modo], NOMBRE_FASE[faseCal]);
  ja(",\"hechos\":[%d,%d,%d],\"rep\":%d,\"reps\":%d,\"bloq\":%d,\"total\":%d,\"auto\":%d",
     hecho[0], hecho[1], hecho[2], repCal, CAL_REPS, bloquesFase, total, CALIBRACION_AUTOMATICA ? 1 : 0);
  ja(",\"datos\":%d,\"rms\":%d,\"suave\":%.1f,\"calibrado\":%d,\"uon\":%.1f,\"uoff\":%.1f,\"pct\":%.0f",
     datosOK() ? 1 : 0, rmsUltimo, rmsSuave, calibrado ? 1 : 0, umbralOn, umbralOff, umbralPct * 100);
  ja(",\"activo\":%d,\"nivel\":%d,\"durAct\":%.1f,\"sid\":%d,\"nAct\":%d,\"tAct\":%.1f,\"tSes\":%.1f",
     act ? 1 : 0, (int)(nivel * 100), act ? (ahora - tInicioActivo) / 1000.0f : 0.0f, sesionId,
     nActivaciones, tiempoActivoTotal / 1000.0f, msSesion / 1000.0f);
  ja(",\"alt\":%d,\"estOK\":%d,\"malas\":%lu,\"rssi\":%d",
     (int)(altura() * 100), modoE != E_SIN_TOPE ? 1 : 0, lineasMalas, (int)WiFi.RSSI());
  ja(",\"cfg\":{\"reposo\":%.1f,\"pasiva\":%.1f,\"reps\":%d,\"arriba\":%.1f,\"abajo\":%.1f,\"descR\":%.1f,\"descP\":%.1f,\"descIni\":%.1f,\"descFin\":%.1f,\"bps\":%d}",
     CAL_REPOSO_S, CAL_PASIVA_S, CAL_REPS, CAL_ARRIBA_S, CAL_ABAJO_S,
     CAL_DESC_REPOSO_S, CAL_DESC_PASIVA_S, CAL_DESC_INI_S, CAL_DESC_FIN_S, BLOQUES_POR_S);
  ja(",\"cal\":{\"reposo\":%.1f,\"sd\":%.1f,\"restRms\":%.1f,\"pasMedia\":%.1f,\"pasPico\":%.1f,\"actRms\":%.1f,\"actEnv\":%.1f,\"snr\":%.2f,\"art\":%.1f,\"repRms\":[",
     cReposo, cSd, cRestRms, cPasMedia, cPasPico, cActRms, cActEnv, cSnr, cArt);
  for (int i = 0; i < CAL_REPS; i++) ja(i ? ",%.1f" : "%.1f", cRepRms[i]);
  ja("]},\"apl\":{\"reposo\":%.1f,\"sd\":%.1f,\"restRms\":%.1f,\"pasMedia\":%.1f,\"pasPico\":%.1f,\"actRms\":%.1f,\"actEnv\":%.1f,\"snr\":%.2f,\"art\":%.1f,\"maximo\":%.1f}",
     reposo, sdReposo, restRms, pasivaMedia, pasivaPico, activeRms, activeEnv, snr, artefactoPct, maximo);
  ja(",\"bn\":%lu,\"b\":[", (unsigned long)nb);
  for (uint32_t i = 0; i < nb - b0; i++) ja(i ? ",[%ld,%d]" : "[%ld,%d]", (long)bv[i], bf[i]);
  ja("],\"an\":%lu,\"acts\":[", (unsigned long)na);
  for (uint32_t i = 0; i < na - a0; i++)
    ja(i ? ",[%d,%d,%.1f,%.1f,%d,%d]" : "[%d,%d,%.1f,%.1f,%d,%d]", av[i].sid, av[i].id, av[i].ini, av[i].dur, av[i].rms, av[i].pct);
  ja("],\"ln\":%lu,\"logs\":[", (unsigned long)nl);
  for (uint32_t i = 0; i < nl - l0; i++) { if (i) ja(","); jaTexto(lt[i]); }
  ja("]}");

  cabeceras();
  web.send(200, "application/json", String(js));
}

void manejarCmd() {
  cabeceras();
  String c = web.arg("c");
  const char *validos = "c123xsnprh+-i";
  if (c.length() != 1 || !strchr(validos, c[0])) {
    web.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  char ch = c[0];
  bool ok = colaCmd && xQueueSend(colaCmd, &ch, 0) == pdTRUE;
  web.send(ok ? 200 : 503, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

void manejarOtro() {
  cabeceras();
  if (web.method() == HTTP_OPTIONS) {             // permiso previo del navegador
    web.sendHeader("Access-Control-Allow-Methods", "GET");
    web.sendHeader("Access-Control-Allow-Headers", "*");
    web.send(204);
    return;
  }
  web.send(200, "text/plain", String("ASTRA - atril ESP32 v") + VERSION_ATRIL + "\nModo: " + NOMBRE_MODO[modo] +
                              "\nLa pagina usa /estado y /cmd?c=...\n");
}

void tareaRed(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // respuesta rapida a la pagina
  WiFi.config(IP_ATRIL, IP_PUERTA, MASCARA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(RED_SSID, RED_CLAVE);
  web.on("/estado", HTTP_GET, manejarEstado);
  web.on("/cmd", HTTP_GET, manejarCmd);
  web.onNotFound(manejarOtro);
  web.begin();
  bool conectado = false;
  unsigned long tIntento = millis();
  for (;;) {
    bool c = WiFi.status() == WL_CONNECTED;
    if (c != conectado) { conectado = c; avisoWifi = c ? 1 : -1; tIntento = millis(); }
    if (!c && millis() - tIntento > 15000) {     // sin red por 15 s: vuelve a intentar
      tIntento = millis();
      WiFi.disconnect();
      WiFi.begin(RED_SSID, RED_CLAVE);
    }
    web.handleClient();
    vTaskDelay(2);
  }
}

// ======================================================================
void setup() {
  // Bobinas apagadas desde el primer instante
  estrellas.enableOutputs(); estrellas.disableOutputs();
  planetas.enableOutputs();  planetas.disableOutputs();

  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("=== ASTRA - atril v%s ===\n", VERSION_ATRIL);

  analogWrite(PIN_ROJO, 0);
  analogWrite(PIN_VERDE, 0);
  analogWrite(PIN_AZUL, 0);
  pinMode(PIN_TOPE, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_PARO, INPUT);

  estrellas.setMaxSpeed(V_VAIVEN_RAPIDO);
  estrellas.setAcceleration(ACELERACION);
  planetas.setMaxSpeed(V_PLANETAS * 2);

  Serial2.begin(9600, SERIAL_8N1, EMG_RX, EMG_TX, EMG_INVERTIDO);   // enlace con el Arduino

  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque

  idArranque = (uint32_t)random(1, 2000000000);
  colaCmd = xQueueCreate(16, sizeof(char));
  if (USAR_WIFI) xTaskCreatePinnedToCore(tareaRed, "red", 8192, NULL, 1, NULL, 0);  // nucleo 0

  if (USAR_PARO && digitalRead(PIN_PARO) == LOW) {
    paroActivo = true;
    paroLectura = LOW;
    entrarParo();
  } else {
    iniciarArranque();
  }
}

void loop() {
  leerArduino();
  leerComandos();
  revisarBoot();
  revisarParo();
  actualizarModo();
  moverEstrellas();
  moverPlanetas();
  actualizarLuz();
  imprimirEstado();
}
