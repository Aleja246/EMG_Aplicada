
// =====================================================================
//  ASTRA - Firmware del atril (ESP32) v4.0
//  Calibracion BASAL individualizada + verificacion supervisada.
// ---------------------------------------------------------------------
//  Entrada: el Arduino UNO R4 WiFi (arduino_emg.ino) manda por el opto,
//  cada 100 ms, una linea con el RMS en milivoltios ("640\n"). El Arduino
//  sigue siendo quien procesa la senal y calcula el RMS; el opto sigue
//  siendo el unico enlace entre los dos lados.
//
//  Que hace:
//   1. ARRANQUE (igual que antes): sube las estrellas hasta el microswitch.
//      El lazy susan empieza a girar desde el encendido.
//   2. CALIBRANDO - calibracion BASAL: solo RMS en reposo. NO se piden
//      movimientos pasivos, contracciones voluntarias ni MVC.
//        "Comenzar calibracion" -> espera
//        "Registrar basal"      -> ~10 s de RMS con el nino comodo y la cabeza
//                                  apoyada (se descarta el 1.er segundo)
//      Se calcula la media y la SD del RMS basal y se evalua la calidad
//      (una senal inestable se RECHAZA). Si es aceptable:
//        U_ON  = media + K x SD                   (K = 3)
//        U_OFF = media + F x (U_ON - media)       (F = 0.6)
//      Ese umbral es PRELIMINAR: se comprueba durante la terapia con las
//      confirmaciones de la fisioterapeuta.
//   3. SESION (supervisada):
//      - Estado sostenido: el RMS suavizado cruza U_ON (ALTO) o U_OFF (BAJO)
//        y se mantiene T_SOST (3 s) con histeresis. Eso solo lo hace
//        "candidato a confirmacion"; no genera un aviso cada 3 s.
//      - Verificacion: la pagina pregunta a la fisioterapeuta si la cabeza y el
//        cuello estan erguidos. Maximo 4 preguntas automaticas en los primeros
//        5 min y 3 en cada ciclo de 10 min, con intervalo minimo entre ellas
//        (45 s / 120 s). El ESP32 lleva los contadores y los temporizadores
//        (sin bloquear nada) con el tiempo EFECTIVO de terapia (la pausa no cuenta).
//      - Cada pregunta guarda la ventana de RMS del EVENTO (media, SD, min, max),
//        no la del momento en que se responde.
//      - Ajuste del umbral: lo propone la pagina; solo se aplica aqui si la
//        fisioterapeuta lo aprueba (/umbral). Se puede restaurar el anterior.
//      - MODO SUPERVISADO: mientras no haya evidencia suficiente, el umbral NO
//        controla los actuadores (estrellas lentas, luz azul tenue, sin sonido).
//        La pagina puede habilitar el control automatico (tecla a) cuando el
//        analisis lo respalda. CONTROL_AUTOMATICO_INICIAL = true lo habilita desde
//        el arranque.
//      - Estrellas, planetas, luz y sonido: mismas reglas de siempre.
//   4. Seguridad: sin datos validos por 500 ms las estrellas pasan a lento y
//      la luz a ambar. PARO (IO34, con USAR_PARO) y la pausa p detienen todo.
//   5. WiFi: se une a la red "ASTRA" que crea el Arduino y atiende a la
//      pagina en http://192.168.4.200:
//        /estado?b=&a=&l=&r=   estado, RMS, eventos de verificacion, contadores
//        /cmd?c=<letra>        comandos simples
//        /conf?id=&p=E|C|D     postura confirmada para un evento
//        /manual?p=E|C|D&n=    confirmacion manual
//        /umbral?on=&off=&n=   umbral aprobado por la fisioterapeuta
//        /restaurar?n=         vuelve al umbral anterior
//        /cfg?k=&v=            parametros (factores, tiempos, limites)
//      Los comandos con identificador (id / n) son idempotentes: si la pagina
//      reintenta, no se duplica nada. La red corre en el nucleo 0; motores, luz y
//      deteccion siguen en el nucleo 1. La configuracion vigente (umbrales,
//      parametros, eventos, contadores) vive en el ESP32: si la pagina se
//      desconecta, el atril sigue igual y al volver se resincroniza.
//
//  Comandos (monitor serie a 115200, "Sin ajuste de linea", o la pagina):
//   c = comenzar (o repetir) la calibracion: queda esperando el registro
//   1 = registrar el RMS basal (10 s)       x = cancelar el registro
//   f = aceptar una calibracion RECHAZADA de todas formas (decision clinica)
//   s = iniciar la sesion (con la calibracion LISTA)
//   n = siguiente paso (lo mismo que el boton BOOT)
//   p = pausa / seguir   r = reanudar tras el paro
//   a = habilitar / deshabilitar el control automatico de los actuadores
//   h = volver a buscar el tope   i = info de calibracion
//  Boton BOOT de la placa: siguiente paso de la calibracion; en sesion,
//  recalibrar; tras el paro, reanudar.
//
//  Placa: "ESP32 Dev Module". Bibliotecas: AccelStepper. WiFi y WebServer
//  vienen con el paquete de placas ESP32 (el DFPlayer se maneja con
//  comandos directos, no necesita su biblioteca).
//
//  Cambios v4.0 respecto a v3.0 (motores, luz, DFPlayer, tope y paro NO cambian):
//   - Calibracion basal (10 s de RMS en reposo) en lugar de reposo + pasiva +
//     5 repeticiones activas. Calidad de la senal antes de aceptarla.
//   - Estados sostenidos, verificaciones con ventana de RMS por evento,
//     periodos de 5 y 10 min, contadores y temporizadores no bloqueantes.
//   - Umbral ajustable solo con aprobacion; historial y restauracion.
//   - Modo supervisado para los actuadores.
//   - Parametros nuevos configurables desde la pagina (/cfg).
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
// (La deteccion de "activacion" de 300 ms se conserva: alimenta el conteo de
//  activaciones y, con control automatico, los actuadores.)
const unsigned long T_CONFIRMAR = 300;  // ms sobre/bajo el umbral para cambiar de estado
const unsigned long HOLGURA_MS  = 50;   // tolera que un bloque llegue unos ms antes (300 ms = 4 bloques seguidos)
const unsigned long T_PREMIO    = 1000; // ms sostenidos para el sonido de activacion
const unsigned long WATCHDOG_MS = 500;  // sin datos por mas de esto -> todo se detiene
const float SUAVIZADO           = 0.5;  // 0..1 (mas alto = responde mas rapido)

// ------------------------- Calibracion basal ---------------------------
// Solo RMS en reposo. Los valores de esta seccion y de las dos siguientes son
// los INICIALES: la pagina puede cambiarlos con /cfg y el ESP32 los conserva
// (en RAM) aunque la pagina se desconecte.
const int   BLOQUES_POR_S      = 10;    // el Arduino manda un bloque cada 100 ms
const float CAL_BASAL_S        = 10;    // duracion del registro basal
const float CAL_DESC_BASAL_S   = 1.0;   // se descartan los primeros segundos (filtros)
const float UMBRAL_K_SD        = 3.0;   // U_ON  = media + K x SD
const float UMBRAL_OFF_FACTOR  = 0.6;   // U_OFF = media + F x (U_ON - media)
// Calidad de la calibracion basal (advertencia / rechazo). Valores en mV de RMS.
const float Q_MEDIA_MIN     = 2.0;      // por debajo: senal casi nula (sensor o electrodos)
const float Q_MEDIA_MAX     = 1500.0;   // por encima: ruido o mal contacto
const float Q_SATURADO      = 2400.0;   // un bloque asi de alto: senal saturada
const float Q_CV_AVISO      = 0.30;     // SD / media: advertencia
const float Q_CV_RECHAZO    = 0.60;     //             rechazo
const float Q_DERIVA_AVISO  = 0.25;     // |media 2.a mitad - 1.a mitad| / media: advertencia
const float Q_DERIVA_RECHAZO = 0.50;    //                                         rechazo
const float Q_PICO_SD       = 6.0;      // bloque > media + 6 SD: pico aislado (advertencia)

// ------------------------- Estados sostenidos y verificacion -----------
const float T_SOSTENIDO_S      = 3.0;   // estado estable = candidato a confirmacion (y ventana de RMS)
const float VENT_DERIVA_MAX    = 0.35;  // las dos mitades de la ventana no pueden diferir mas de 35 %: si cruza un cambio de postura, no se pregunta
const float VER_INI_S          = 300;   // primer periodo de verificacion: 5 min
const int   VER_INI_MAX        = 4;     //   maximo de preguntas automaticas
const float VER_INI_MIN_ENTRE_S = 45;   //   intervalo minimo entre preguntas
const float VER_CICLO_S        = 600;   // ciclos siguientes: 10 min (se repiten hasta el final)
const int   VER_CICLO_MAX      = 3;
const float VER_CICLO_MIN_ENTRE_S = 120;
const float T_REVERIFICAR_S    = 120;   // un estado que dura mucho se vuelve a comprobar tras esto
const float T_RESPUESTA_S      = 120;   // tiempo efectivo para responder una pregunta
const bool  HIPOTESIS_ALTO_ES_ERGUIDO = true;  // interpretacion a comprobar: RMS alto = cabeza erguida
const bool  CONTROL_AUTOMATICO_INICIAL = false; // false = SUPERVISADO (actuadores no usan el umbral)

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
const char *VERSION_ATRIL = "4.0";

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

// --- Parametros (iniciales = constantes de arriba; /cfg los cambia) ---
struct Cfg {
  float basalS, descBasalS, k, f, tSost;
  float verIniS, verIniMin; int verIniMax;
  float verCicloS, verCicloMin; int verCicloMax;
  float tRever, tResp;
  int hip;                          // 1 = RMS alto <-> cabeza erguida; 0 = RMS alto <-> cabeza caida
} cfg = {CAL_BASAL_S, CAL_DESC_BASAL_S, UMBRAL_K_SD, UMBRAL_OFF_FACTOR, T_SOSTENIDO_S,
         VER_INI_S, VER_INI_MIN_ENTRE_S, VER_INI_MAX, VER_CICLO_S, VER_CICLO_MIN_ENTRE_S, VER_CICLO_MAX,
         T_REVERIFICAR_S, T_RESPUESTA_S, HIPOTESIS_ALTO_ES_ERGUIDO ? 1 : 0};

// --- Calibracion basal ---
enum FaseCal { CAL_ESPERA, CAL_BASAL, CAL_LISTA, CAL_RECHAZADA };
const char *NOMBRE_FASE[] = {"ESPERA", "BASAL", "LISTA", "RECHAZADA"};
FaseCal faseCal = CAL_ESPERA;
int bloquesFase = 0;                // bloques recibidos en el registro (reloj de las muestras)
double bSum = 0, bSum2 = 0, bSumA = 0, bSumB = 0;
int bN = 0, bNa = 0, bNb = 0;
float bMin = 1e9, bMax = 0;
bool bCorte = false;                // se cortaron los datos durante el registro
// Banderas de calidad
const uint16_t F_CORTE = 1 << 0, F_NULA = 1 << 1, F_RUIDO = 1 << 2, F_SATURA = 1 << 3, F_PLANA = 1 << 4,
               F_CV_EXC = 1 << 5, F_CV = 1 << 6, F_DER_EXC = 1 << 7, F_DER = 1 << 8, F_PICOS = 1 << 9, F_POCOS = 1 << 10;
const uint16_t F_RECHAZO = F_CORTE | F_NULA | F_RUIDO | F_SATURA | F_PLANA | F_CV_EXC | F_DER_EXC | F_POCOS;
const char *TXT_FLAG[] = {"datos interrumpidos durante el registro", "senal casi nula (sensor o electrodos)",
                          "RMS basal muy alto (ruido o mal contacto)", "senal saturada", "senal plana",
                          "variabilidad excesiva", "variabilidad alta", "deriva excesiva", "deriva notable",
                          "picos aislados (movimiento?)", "muy pocos bloques"};
// nivel: 0 aceptada, 1 con advertencias, 2 rechazada, 3 aceptada a la fuerza (decision de la fisioterapeuta)
struct CalBasal { float media, sd, cv, deriva, minimo, maximo; int n; uint16_t flags; uint8_t nivel; } cb = {0, 0, 0, 0, 0, 0, 0, 0, 0};
// Calibracion aplicada (la que usa la sesion)
bool calibrado = false;
float reposo = 100, sdReposo = 10;
float umbralOn = 300, umbralOff = 200;
uint8_t umbralOrigen = 0;           // 0 basal, 1 ajustado (aprobado en la pagina), 2 restaurado
struct UmbralHist { float on, off; uint8_t origen; float tSes; };
UmbralHist hist[6]; int histN = 0;
bool controlAuto = CONTROL_AUTOMATICO_INICIAL;   // false = modo supervisado

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

// --- Estados sostenidos y verificacion ---
enum EstadoRms { RMS_NINGUNO = 0, RMS_ALTO = 1, RMS_BAJO = 2 };
uint8_t estadoRms = RMS_NINGUNO;    // estado con histeresis (ALTO entra en U_ON; BAJO entra en U_OFF)
uint32_t episodio = 0;              // sube cada vez que cambia el estado
int bloquesEnEstado = 0;
bool sostenidoMarcado = false;      // este episodio ya se conto como "sostenido detectado"
uint32_t nDetectados = 0;
const int N_VENT = 64;              // ventana de RMS: ultimos 6.4 s (se limpia con cada pausa o corte de datos)
float vRms[N_VENT]; int vN = 0, vW = 0;
// Eventos de verificacion
enum StatusEv { EV_PENDIENTE = 1, EV_CONFIRMADO = 2, EV_DESCARTADO = 3, EV_EXPIRADO = 4 };
enum Postura { POST_NINGUNA = 0, POST_ERGUIDA = 1, POST_CAIDA = 2, POST_INDETERMINADA = 3 };
struct Evento {
  uint32_t id, rev, ep, tok;
  uint8_t origen, estado, status, postura, calidad, periodo, nSol;
  int sid;
  float tIni, dur, media, sd, minimo, maximo, uon, uoff, tSolic;
};
const int N_EV = 48;
Evento evs[N_EV];
uint32_t nEv = 0, revGlobal = 0;
int perActual = -1, solPer = 0;     // periodo de verificacion en curso y preguntas automaticas hechas en el
float tSolUlt = -1e9, finVentUlt = -1e9, tEpUlt = -1e9;
uint32_t epUltSol = 0, pendId = 0;
uint32_t nResp[3] = {0, 0, 0};      // confirmaciones E/C por estado RMS (1 = ALTO, 2 = BAJO)
uint32_t nErr[3] = {0, 0, 0};       // las que no coincidieron con la hipotesis
uint32_t tokManual = 0, tokUmbral = 0, tokRestaurar = 0;
volatile uint32_t tUltimaConsulta = 0;   // ultima vez que la pagina pidio /estado

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
// En modo SUPERVISADO el umbral aun no controla los actuadores: nunca hay "premio".
bool premiado() { return controlAuto && (PREMIO_AL_RELAJAR ? !activo : activo); }

// El registro basal esta en curso
bool midiendo() { return faseCal == CAL_BASAL; }

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

float tEfS() { return msSesion / 1000.0f; }   // tiempo EFECTIVO de terapia (la pausa no cuenta)

const char *txtOrigen() { return umbralOrigen == 0 ? "basal (preliminar)" : umbralOrigen == 1 ? "ajustado y aprobado" : "restaurado"; }

// Umbrales de la calibracion basal: U_ON = media + K SD; U_OFF = media + F (U_ON - media)
void umbralesBasal(float &on, float &off) {
  on  = reposo + cfg.k * sdReposo;
  off = reposo + cfg.f * (on - reposo);
}

// Cambia el umbral vigente y lo guarda en el historial (para poder restaurarlo)
void fijarUmbral(float on, float off, uint8_t origen) {
  umbralOn = on; umbralOff = off; umbralOrigen = origen;
  if (histN == 6) { for (int i = 1; i < 6; i++) hist[i - 1] = hist[i]; histN = 5; }
  hist[histN++] = {on, off, origen, tEfS()};
}

void imprimirCalibracion() {
  msg("---- Calibracion basal ----");
  msg("RMS basal: media %.1f mV, SD %.1f mV (CV %.2f, deriva %+.0f %%, %d bloques)", cb.media, cb.sd, cb.cv, cb.deriva * 100, cb.n);
  const char *niv[] = {"ACEPTADA", "ACEPTADA CON ADVERTENCIAS", "RECHAZADA", "ACEPTADA A LA FUERZA"};
  msg("Calidad: %s", niv[cb.nivel]);
  for (int i = 0; i < 11; i++) if (cb.flags & (1 << i)) msg("   - %s", TXT_FLAG[i]);
  msg("U_ON %.1f mV (media + %.1f SD) | U_OFF %.1f mV (media + %.2f x (U_ON - media))", umbralOn, cfg.k, umbralOff, cfg.f);
  msg("Umbral %s. Modo %s.", txtOrigen(), controlAuto ? "CONTROL AUTOMATICO" : "SUPERVISADO");
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
    int pct = (int)lroundf(100.0f * (rmsMedio - reposo) / max(umbralOn - reposo, 1.0f));   // % del umbral sobre el reposo
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

// ---------------------------------------------------------------------
//  Eventos de verificacion (ring de N_EV). Solo el loop escribe; la red lee con candado.
// ---------------------------------------------------------------------
Evento *buscarEv(uint32_t id) {
  if (id == 0 || id > nEv || id + N_EV <= nEv) return NULL;
  return &evs[(id - 1) % N_EV];
}
// Guarda (o actualiza) un evento y le pone una revision nueva: la pagina pide lo que cambio
void guardarEv(Evento &e) {
  portENTER_CRITICAL(&candado);
  e.rev = ++revGlobal;
  evs[(e.id - 1) % N_EV] = e;
  if (e.id > nEv) nEv = e.id;
  portEXIT_CRITICAL(&candado);
}

// Ventana de RMS: los ultimos n bloques (todos de este mismo tramo de sesion)
void ventanaAgregar(float mv) { vRms[vW] = mv; vW = (vW + 1) % N_VENT; if (vN < N_VENT) vN++; }
void ventanaLimpiar() { vN = 0; vW = 0; }
int nVentana() { int n = (int)lroundf(cfg.tSost * BLOQUES_POR_S); return constrain(n, 10, N_VENT); }
struct Ventana { float media, sd, minimo, maximo, deriva; uint8_t calidad; };
bool statsVentana(int n, Ventana &v) {
  if (vN < n) return false;
  double s = 0, s2 = 0, sNuevo = 0, sViejo = 0; float mn = 1e9, mx = 0; uint8_t cal = 0;
  int mitad = n / 2;
  for (int i = 0; i < n; i++) {
    float x = vRms[(vW - 1 - i + N_VENT) % N_VENT];
    s += x; s2 += (double)x * x;
    if (i < mitad) sNuevo += x; else if (i >= n - mitad) sViejo += x;
    if (x < mn) mn = x;
    if (x > mx) mx = x;
    if (x >= Q_SATURADO) cal = 1;                  // 1 = ventana dudosa (saturacion)
  }
  double m = s / n, var = n > 1 ? (s2 - n * m * m) / (n - 1) : 0;
  float der = (mitad > 0 && m > 0) ? fabsf((float)((sNuevo - sViejo) / mitad)) / (float)m : 0;   // |mitad nueva - mitad vieja| / media
  v = {(float)m, sqrtf(max((float)var, 0.0f)), mn, mx, der, cal};
  return true;
}

struct Periodo { int idx; float ini, dur, minEntre; int max; };
Periodo periodoDe(float t) {
  if (t < cfg.verIniS) return {0, 0, cfg.verIniS, cfg.verIniMin, cfg.verIniMax};
  int n = (int)((t - cfg.verIniS) / cfg.verCicloS);
  return {1 + n, cfg.verIniS + n * cfg.verCicloS, cfg.verCicloS, cfg.verCicloMin, cfg.verCicloMax};
}

void expirarPendiente() {
  if (!pendId) return;
  Evento *p = buscarEv(pendId);
  if (p && p->status == EV_PENDIENTE) { Evento e = *p; e.status = EV_EXPIRADO; guardarEv(e); }
  pendId = 0;
}

void reiniciarVerificacion() {
  expirarPendiente();
  perActual = -1; solPer = 0;
  tSolUlt = finVentUlt = tEpUlt = -1e9;
  epUltSol = 0;
  for (int i = 0; i < 3; i++) nResp[i] = nErr[i] = 0;
  nDetectados = 0;
}

void reiniciarEstadoRms() {
  estadoRms = RMS_NINGUNO; bloquesEnEstado = 0; sostenidoMarcado = false; ventanaLimpiar();
}

// Hay un estado sostenido ahora mismo? (histeresis + ventana coherente con el estado)
bool sostenidoVigente(Ventana &v) {
  int nv = nVentana();
  if (estadoRms == RMS_NINGUNO || bloquesEnEstado < nv || !statsVentana(nv, v)) return false;
  if (v.deriva > VENT_DERIVA_MAX) return false;        // ventana que cruza una transicion: todavia no es estable
  return estadoRms == RMS_ALTO ? v.media >= umbralOn : v.media < umbralOff;
}

// Sigue el estado del RMS suavizado con histeresis y cuenta los estados sostenidos.
// NO genera avisos: solo marca que existe un estado estable candidato a confirmacion.
void actualizarEstadoRms() {
  uint8_t nuevo = estadoRms;
  if (estadoRms == RMS_NINGUNO) nuevo = rmsSuave >= umbralOn ? RMS_ALTO : RMS_BAJO;
  else if (estadoRms == RMS_BAJO && rmsSuave >= umbralOn) nuevo = RMS_ALTO;
  else if (estadoRms == RMS_ALTO && rmsSuave < umbralOff) nuevo = RMS_BAJO;
  if (nuevo != estadoRms) { estadoRms = nuevo; episodio++; bloquesEnEstado = 0; sostenidoMarcado = false; }
  bloquesEnEstado++;
  Ventana v;
  if (!sostenidoMarcado && sostenidoVigente(v)) {
    sostenidoMarcado = true; nDetectados++;
    msg("   Estado sostenido %s (RMS medio %.0f mV en %.0f s). Candidato a confirmacion.",
        estadoRms == RMS_ALTO ? "ALTO" : "BAJO", v.media, cfg.tSost);
  }
}

bool paginaViva() { return tUltimaConsulta && (millis() - tUltimaConsulta) < 3000; }

// Arma el evento con la ventana de RMS de AHORA (la del estado que se esta viendo)
Evento armarEvento(const Ventana &v, uint8_t origen, const Periodo &p) {
  Evento e;
  memset(&e, 0, sizeof e);
  e.id = nEv + 1; e.ep = episodio; e.origen = origen; e.estado = estadoRms;
  e.calidad = v.calidad; e.periodo = p.idx; e.sid = sesionId;
  e.tIni = tEfS() - nVentana() / (float)BLOQUES_POR_S; e.dur = nVentana() / (float)BLOQUES_POR_S;
  e.media = v.media; e.sd = v.sd; e.minimo = v.minimo; e.maximo = v.maximo;
  e.uon = umbralOn; e.uoff = umbralOff; e.tSolic = tEfS();
  return e;
}

// Cuenta la respuesta y marca si coincide con la hipotesis (RMS alto <-> erguida, segun cfg.hip)
void contarRespuesta(const Evento &e) {
  if (e.postura != POST_ERGUIDA && e.postura != POST_CAIDA) return;
  nResp[e.estado]++;
  bool altoEsErguido = cfg.hip == 1;
  bool esperadaErguida = (e.estado == RMS_ALTO) == altoEsErguido;
  if (esperadaErguida != (e.postura == POST_ERGUIDA)) nErr[e.estado]++;
}

// Planificador de verificaciones: sin bloqueos, con el tiempo efectivo de terapia.
void planificarVerificacion() {
  if (modo != SESION) return;
  float t = tEfS();
  Periodo p = periodoDe(t);
  if (p.idx != perActual) {                         // periodo nuevo: solo se reinicia el contador automatico
    perActual = p.idx; solPer = 0;
    if (p.idx == 0) msg(">> Verificacion: primeros %.0f min, maximo %d confirmaciones automaticas.", p.dur / 60, p.max);
    else msg(">> Verificacion: ciclo %d (min %.0f-%.0f), maximo %d confirmaciones automaticas.", p.idx, p.ini / 60, (p.ini + p.dur) / 60, p.max);
  }
  if (pendId) {                                     // la pregunta caduca si no se responde a tiempo
    Evento *pe = buscarEv(pendId);
    if (!pe || pe->status != EV_PENDIENTE) pendId = 0;
    else if (t - pe->tSolic > cfg.tResp) {
      Evento e = *pe; e.status = EV_EXPIRADO; guardarEv(e); pendId = 0;
      msg("   Pregunta #%lu sin respuesta: expirada.", (unsigned long)e.id);
    }
  }
  if (pendId || !datosOK() || !paginaViva()) return;   // sin pagina no se pregunta (no se gasta el cupo)
  if (solPer >= p.max) return;
  if (t < tSolUlt + p.minEntre) return;                // intervalo minimo entre preguntas
  if (t < p.ini + solPer * p.dur / (p.max + 1)) return; // reparto en el periodo: no todo al inicio
  Ventana v;
  if (!sostenidoVigente(v)) return;                    // hace falta un estado sostenido ahora
  if (tEfS() - nVentana() / (float)BLOQUES_POR_S < finVentUlt) return;   // nunca dos preguntas sobre la misma ventana
  // Prioridad: estado nuevo > observaciones poco representadas > estados que ya fallaron
  int pts;
  if (episodio != epUltSol) pts = 3;                   // episodio nuevo
  else {                                               // el mismo estado sigue: comprobacion periodica, sin saturar
    float desde = t - tEpUlt;
    if (desde < cfg.tRever) return;
    pts = (int)(desde / cfg.tRever);                   // 1 tras T_REVER, 2 tras el doble...
  }
  uint8_t otro = estadoRms == RMS_ALTO ? RMS_BAJO : RMS_ALTO;
  if (nResp[estadoRms] < nResp[otro]) pts += 2;        // poco representado
  if (nErr[estadoRms] > 0) pts += 2;                   // ya genero errores
  if (pts < 2) return;
  Evento e = armarEvento(v, 0, p);
  e.status = EV_PENDIENTE; e.nSol = solPer + 1;
  guardarEv(e);
  pendId = e.id; solPer++; tSolUlt = t; finVentUlt = t; epUltSol = episodio; tEpUlt = t;
  msg(">> Verificacion %d/%d (periodo %d): pregunta #%lu, ventana de %.0f s.", solPer, p.max, p.idx, (unsigned long)e.id, cfg.tSost);
}

// Respuesta de la fisioterapeuta a una pregunta pendiente. Idempotente.
void responderEvento(uint32_t id, uint8_t postura) {
  Evento *pe = buscarEv(id);
  if (!pe || pe->status != EV_PENDIENTE || postura < 1 || postura > 3) return;   // duplicado o desconocido: se ignora
  Evento e = *pe;
  e.postura = postura;
  e.status = postura == POST_INDETERMINADA ? EV_DESCARTADO : EV_CONFIRMADO;
  guardarEv(e);
  if (pendId == id) pendId = 0;
  contarRespuesta(e);
  msg("   Pregunta #%lu respondida: %s.", (unsigned long)id,
      postura == POST_ERGUIDA ? "erguida" : postura == POST_CAIDA ? "caida" : "no se puede determinar");
}

// Confirmacion manual: usa la ventana de RMS de este momento. No gasta el cupo automatico.
void confirmarManual(uint8_t postura, uint32_t token) {
  if (token && token == tokManual) return;             // reintento de la pagina
  if (modo != SESION || postura < 1 || postura > 3) { msg("!! La confirmacion manual necesita la sesion en marcha."); return; }
  Ventana v;
  int nv = min(nVentana(), vN);
  if (estadoRms == RMS_NINGUNO || nv < 10 || !statsVentana(nv, v)) { msg("!! Aun no hay RMS suficiente para una confirmacion manual."); return; }
  tokManual = token;
  Periodo p = periodoDe(tEfS());
  Evento e = armarEvento(v, 1, p);
  e.dur = nv / (float)BLOQUES_POR_S; e.tIni = tEfS() - e.dur; e.tok = token;
  e.postura = postura; e.status = postura == POST_INDETERMINADA ? EV_DESCARTADO : EV_CONFIRMADO;
  guardarEv(e);
  contarRespuesta(e);
  finVentUlt = max(finVentUlt, tEfS());
  msg("   Confirmacion manual #%lu: %s (RMS %.0f mV, estado %s).", (unsigned long)e.id,
      postura == POST_ERGUIDA ? "erguida" : postura == POST_CAIDA ? "caida" : "no se puede determinar", v.media,
      estadoRms == RMS_ALTO ? "ALTO" : "BAJO");
}

// ---------------------------------------------------------------------
//  Parametros
// ---------------------------------------------------------------------
bool ajustarCfg(const char *k, float v, bool aplicar) {
#define CF(nombre, campo, lo, hi) if (!strcmp(k, nombre)) { if (!(v >= (lo) && v <= (hi))) return false; if (aplicar) campo = (decltype(campo))v; return true; }
  CF("basal", cfg.basalS, 5, 60)
  CF("desc", cfg.descBasalS, 0, 5)
  CF("k", cfg.k, 0.5, 10)
  CF("f", cfg.f, 0.1, 0.95)
  CF("tSost", cfg.tSost, 1, 6)
  CF("v1s", cfg.verIniS, 30, 3600)
  CF("v1max", cfg.verIniMax, 0, 20)
  CF("v1min", cfg.verIniMin, 5, 3600)
  CF("vcs", cfg.verCicloS, 60, 7200)
  CF("vcmax", cfg.verCicloMax, 0, 20)
  CF("vcmin", cfg.verCicloMin, 5, 7200)
  CF("rever", cfg.tRever, 10, 3600)
  CF("resp", cfg.tResp, 10, 600)
  CF("hip", cfg.hip, 0, 1)
#undef CF
  return false;
}

// ---------------------------------------------------------------------
//  Calibracion basal
// ---------------------------------------------------------------------
void reiniciarBasal() {
  bSum = bSum2 = bSumA = bSumB = 0; bN = bNa = bNb = 0; bMin = 1e9; bMax = 0; bCorte = false;
}

// Deja la calibracion lista para empezar: espera el registro basal.
void iniciarCalibracion() {
  cerrarActivacion();
  expirarPendiente();
  modo = CALIBRANDO;
  faseCal = CAL_ESPERA;
  bloquesFase = 0;
  reiniciarBasal();
  msg(">> CALIBRACION BASAL (%.0f s): solo RMS en reposo; no se piden movimientos ni contracciones.", cfg.basalS);
  msg(">> Esperando: pon al nino comodo y estable, con la cabeza apoyada, y registra el basal desde la pagina, con BOOT o con la tecla 1.");
}

void iniciarSesion(bool nueva) {
  if (nueva) {                     // tras calibrar: sesion nueva desde cero
    sesionId++;
    nActivaciones = 0;
    tiempoActivoTotal = 0;
    msSesion = 0;
    reiniciarVerificacion();
  }
  modo = SESION;
  bajando = false;                 // el vaiven sigue donde iba
  activo = false;
  tSobre = tBajo = 0;
  tInicioPremio = tFinPremio = millis();   // tras una pausa esperan 1.5 s antes de bajar
  premio1 = false;
  rmsSuave = reposo;               // no arrastra la contraccion de la calibracion
  reiniciarEstadoRms();            // el RMS de antes de la pausa no se mezcla con el de ahora
  if (nueva) msg(controlAuto ? ">> SESION: control automatico de los actuadores." :
                 ">> SESION SUPERVISADA: el umbral aun no controla los actuadores (estrellas lentas, luz azul tenue, sin sonido).");
  else msg(">> SESION: continua.");
}

// Pone en uso la calibracion basal (aceptada, con advertencias o a la fuerza)
void aplicarBasal(uint8_t nivel) {
  cb.nivel = nivel;
  reposo = cb.media; sdReposo = cb.sd;
  float on, off; umbralesBasal(on, off);
  histN = 0;
  fijarUmbral(on, off, 0);
  calibrado = true;
  faseCal = CAL_LISTA;
  imprimirCalibracion();
  msg(">> Calibracion LISTA. Inicia la sesion desde la pagina, con BOOT o con la tecla s. El umbral es preliminar.");
}

void terminarBasal() {
  int total = (int)lroundf(cfg.basalS * BLOQUES_POR_S), desc = (int)lroundf(cfg.descBasalS * BLOQUES_POR_S);
  int esperados = max(total - desc, 1);
  cb.n = bN;
  cb.media = bN ? bSum / bN : 0;
  double var = bN > 1 ? (bSum2 - bN * (double)cb.media * cb.media) / (bN - 1) : 0;
  cb.sd = sqrtf(max((float)var, 0.0f));
  cb.cv = cb.media > 0 ? cb.sd / cb.media : 0;
  float mA = bNa ? bSumA / bNa : cb.media, mB = bNb ? bSumB / bNb : cb.media;
  cb.deriva = cb.media > 0 ? (mB - mA) / cb.media : 0;
  cb.minimo = bN ? bMin : 0; cb.maximo = bMax;
  uint16_t f = 0;
  if (bCorte) f |= F_CORTE;
  if (bN < 0.8f * esperados) f |= F_POCOS;
  if (cb.media < Q_MEDIA_MIN) f |= F_NULA;
  if (cb.media > Q_MEDIA_MAX) f |= F_RUIDO;
  if (cb.maximo >= Q_SATURADO) f |= F_SATURA;
  if (bN > 1 && cb.sd < 0.01f) f |= F_PLANA;
  if (cb.cv > Q_CV_RECHAZO) f |= F_CV_EXC; else if (cb.cv > Q_CV_AVISO) f |= F_CV;
  if (fabsf(cb.deriva) > Q_DERIVA_RECHAZO) f |= F_DER_EXC; else if (fabsf(cb.deriva) > Q_DERIVA_AVISO) f |= F_DER;
  if (cb.sd > 0 && cb.maximo > cb.media + Q_PICO_SD * cb.sd) f |= F_PICOS;
  cb.flags = f;
  bloquesFase = 0;
  if (f & F_RECHAZO) {
    cb.nivel = 2;
    faseCal = CAL_RECHAZADA;
    imprimirCalibracion();
    msg("!! Calibracion RECHAZADA: la senal basal no es suficientemente estable. Revisa electrodos, postura y cables y repite (1).");
    msg("!! Si la fisioterapeuta decide usarla igual: tecla f (queda marcada como aceptada a la fuerza).");
    return;
  }
  aplicarBasal(f ? 1 : 0);
}

// Empieza el registro basal. Se puede repetir las veces que haga falta.
void empezarBasal() {
  if (modo != CALIBRANDO) { msg("!! Primero comienza la calibracion (c)."); return; }
  if (midiendo()) { msg("!! Ya se esta registrando el basal. Espera a que termine o cancelalo (x)."); return; }
  reiniciarBasal();
  faseCal = CAL_BASAL;
  bloquesFase = 0;
  msg(">> REGISTRO BASAL (%.0f s): cabeza apoyada y relajada. No mover al nino. Se descarta el primer %.1f s.", cfg.basalS, cfg.descBasalS);
}

void cancelarBasal() {
  if (modo != CALIBRANDO || !midiendo()) return;
  reiniciarBasal();
  faseCal = CAL_ESPERA;
  bloquesFase = 0;
  msg(">> Registro basal cancelado. Puedes volver a empezarlo. La calibracion anterior (si habia) sigue vigente.");
}

// Devuelve true si el bloque se uso para la calibracion (no descartado)
bool calibrarBloque(int mv) {
  if (faseCal != CAL_BASAL) return false;
  float env = mv;                     // envolvente = RMS del bloque de 100 ms
  bool usado = false;
  bloquesFase++;
  int total = (int)lroundf(cfg.basalS * BLOQUES_POR_S), desc = (int)lroundf(cfg.descBasalS * BLOQUES_POR_S);
  if (bloquesFase > desc) {
    int esperados = max(total - desc, 1), mitad = esperados / 2;
    bN++; bSum += env; bSum2 += (double)env * env;
    if (bN <= mitad) { bSumA += env; bNa++; }
    else if (bN > esperados - mitad) { bSumB += env; bNb++; }
    if (env < bMin) bMin = env;
    if (env > bMax) bMax = env;
    usado = true;
  }
  if (bloquesFase >= total) terminarBasal();
  return usado;
}

// Aprueba un umbral nuevo propuesto por la pagina (ya aprobado por la fisioterapeuta)
void aplicarUmbralAprobado(float on, float off, uint32_t token) {
  if (token && token == tokUmbral) return;             // reintento
  if (!calibrado || !(off > 0 && on > off && on <= 5000)) { msg("!! Umbral rechazado por el atril (valores no validos)."); return; }
  tokUmbral = token;
  float antes = umbralOn;
  fijarUmbral(on, off, 1);
  msg(">> Umbral AJUSTADO (aprobado): U_ON %.1f -> %.1f mV, U_OFF %.1f mV.", antes, on, off);
}

void restaurarUmbral(uint32_t token) {
  if (token && token == tokRestaurar) return;
  if (histN < 2) { msg("!! No hay umbral anterior que restaurar."); return; }
  tokRestaurar = token;
  float antes = umbralOn;
  histN--;                                              // descarta el vigente
  UmbralHist h = hist[histN - 1];
  histN--;                                              // fijarUmbral lo vuelve a anotar como restaurado
  fijarUmbral(h.on, h.off, 2);
  msg(">> Umbral RESTAURADO: U_ON %.1f -> %.1f mV, U_OFF %.1f mV.", antes, umbralOn, umbralOff);
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
  empezarBasal();                  // esperando o rechazada: (re)registrar el basal
}

// Comandos del monitor serie y de la pagina (todos pasan por aqui, en el loop)
void ejecutarComando(char c) {
  switch (c) {
    case 'c':
      if (modo == ARRANQUE) msg("!! Espera a que las estrellas encuentren el tope.");
      else if (modo == PARO) msg("!! Primero suelta el paro y reanuda (r).");
      else iniciarCalibracion();
      break;
    case '1': empezarBasal(); break;
    case 'x': cancelarBasal(); break;
    case 'f':
      if (modo == CALIBRANDO && faseCal == CAL_RECHAZADA) { msg("!! Calibracion aceptada A LA FUERZA por decision de la fisioterapeuta."); aplicarBasal(3); }
      break;
    case 's':
      if (modo == CALIBRANDO && faseCal == CAL_LISTA) iniciarSesion(true);
      else msg("!! La sesion empieza cuando la calibracion este LISTA.");
      break;
    case 'n': siguientePaso(); break;
    case 'p':
      if (modo == SESION) { cerrarActivacion(); modo = PAUSA; msg(">> PAUSA (p para seguir). El tiempo de terapia se detiene."); }
      else if (modo == PAUSA) iniciarSesion(false);
      break;
    case 'r': if (modo == PARO) reanudarTrasParo(); break;
    case 'h': if (modo != PARO) iniciarArranque(); break;
    case 'a':
      controlAuto = !controlAuto;
      msg(controlAuto ? ">> CONTROL AUTOMATICO habilitado: el umbral ahora controla los actuadores."
                      : ">> Modo SUPERVISADO: el umbral ya no controla los actuadores.");
      break;
    case 'i': imprimirCalibracion(); break;
    default: break;
  }
}

enum TipoCmd : uint8_t { C_CONF = 1, C_MANUAL, C_UMBRAL, C_RESTAURAR, C_CFG };
struct CmdWeb { uint8_t tipo; uint8_t p; uint32_t id; uint32_t token; float a, b; char k[8]; };
QueueHandle_t colaWeb = NULL;       // comandos con datos de la pagina -> loop

void ejecutarWeb(const CmdWeb &c) {
  switch (c.tipo) {
    case C_CONF:      responderEvento(c.id, c.p); break;
    case C_MANUAL:    confirmarManual(c.p, c.token); break;
    case C_UMBRAL:    aplicarUmbralAprobado(c.a, c.b, c.token); break;
    case C_RESTAURAR: restaurarUmbral(c.token); break;
    case C_CFG:
      if (ajustarCfg(c.k, c.a, true)) {
        msg(">> Parametro %s = %g", c.k, c.a);
        // Si el umbral vigente es el basal sin ajustes, se recalcula con los factores nuevos
        if ((!strcmp(c.k, "k") || !strcmp(c.k, "f")) && calibrado && umbralOrigen == 0) {
          float on, off; umbralesBasal(on, off); fijarUmbral(on, off, 0);
          msg(">> U_ON %.1f mV, U_OFF %.1f mV (factores nuevos).", umbralOn, umbralOff);
        }
      } else msg("!! Parametro no valido: %s", c.k);
      break;
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

  nivel = constrain((rmsSuave - reposo) / (2 * max(umbralOn - reposo, 1.0f)), 0.0f, 1.0f);

  // Estados sostenidos con histeresis (no avisan: solo marcan candidatos a confirmacion)
  ventanaAgregar(mv);
  actualizarEstadoRms();

  // Activacion de 300 ms (conteo y, con control automatico, actuadores): igual que antes
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

  // Banderas del bloque para la pagina: 1 ALTO, 4 sostenido, 8 activacion
  uint8_t fl = (estadoRms == RMS_ALTO ? 1 : 0) | ((bloquesEnEstado >= nVentana() && vN >= nVentana()) ? 4 : 0) | (activo ? 8 : 0);
  guardarBloque(mv, fl);
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
  CmdWeb w;
  while (colaWeb && xQueueReceive(colaWeb, &w, 0) == pdTRUE) ejecutarWeb(w);
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
      reiniciarEstadoRms();                          // no se mezclan datos de antes y despues del corte
      if (modo == CALIBRANDO && faseCal == CAL_BASAL) bCorte = true;   // el registro basal ya no es continuo
      msg("!! Sin datos del Arduino: estrellas lentas y luz ambar hasta que vuelvan.");
    } else {
      if (!activo) { tInicioPremio = tFinPremio = ahora; premio1 = false; }  // 1.5 s para reconfirmar
      msg(">> Llegan datos del Arduino.");
    }
  }

  // El registro basal avanza con cada bloque recibido (calibrarBloque): sin datos, espera.
  planificarVerificacion();                      // temporizadores de verificacion (no bloquea)

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
      if (!midiendo()) { r = 15; g = 15; b = 15; }                  // blanco tenue: espera el registro
      else b = 70;                                                  // azul: registrando el basal
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
    Serial.printf("[SESION] RMS %4d mV (U_ON %.0f) | nivel %3d %% | %s | estrellas %s | activaciones %d%s\n",
                  rmsUltimo, umbralOn, (int)(nivel * 100), activo ? "ACTIVO" : "reposo",
                  est, nActivaciones, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else if (modo == CALIBRANDO) {
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
static char js[16384];              // respuesta de /estado
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

const int EV_POR_RESPUESTA = 10;

void manejarEstado() {
  static int32_t bv[N_BLQ];
  static uint8_t bf[N_BLQ];
  static Activacion av[N_ACT];
  static char lt[N_LOG][LARGO_LOG];
  static Evento ev[EV_POR_RESPUESTA];

  tUltimaConsulta = millis();                         // la pagina esta viva: ya se pueden hacer preguntas

  // Copia de lo que cambia en el otro nucleo
  uint32_t nb, na, nl, b0, a0, l0, rev, nev;
  portENTER_CRITICAL(&candado);
  nb = nBlq; na = nActLista; nl = nLog; rev = revGlobal; nev = nEv;
  portEXIT_CRITICAL(&candado);
  b0 = desde("b", nb, N_BLQ);
  a0 = desde("a", na, N_ACT);
  l0 = desde("l", nl, N_LOG);
  uint32_t r0 = web.hasArg("r") ? (uint32_t)web.arg("r").toInt() : 0;
  int nEvSal = 0;
  uint32_t er = rev;
  portENTER_CRITICAL(&candado);
  for (uint32_t i = b0; i < nb; i++) { bv[i - b0] = blqVal[i % N_BLQ]; bf[i - b0] = blqFlag[i % N_BLQ]; }
  for (uint32_t i = a0; i < na; i++) av[i - a0] = actLista[i % N_ACT];
  for (uint32_t i = l0; i < nl; i++) memcpy(lt[i - l0], logTxt[i % N_LOG], LARGO_LOG);
  // Eventos con revision nueva, de la mas antigua a la mas reciente (hasta EV_POR_RESPUESTA por vez)
  uint32_t ultima = r0;
  while (nEvSal < EV_POR_RESPUESTA) {
    int mejor = -1; uint32_t rm = 0xFFFFFFFF;
    for (int i = 0; i < N_EV; i++) {
      if (evs[i].id == 0 || evs[i].id > nev || evs[i].rev <= ultima) continue;
      if (evs[i].rev < rm) { rm = evs[i].rev; mejor = i; }
    }
    if (mejor < 0) break;
    ev[nEvSal++] = evs[mejor];
    ultima = rm;
  }
  portEXIT_CRITICAL(&candado);
  // Si quedaron eventos sin mandar, el cursor se queda en el ultimo mandado
  bool hayMas = false;
  for (int i = 0; i < N_EV && !hayMas; i++) hayMas = evs[i].id != 0 && evs[i].id <= nev && evs[i].rev > ultima;
  if (hayMas) er = ultima;

  float t = tEfS();
  Periodo per = periodoDe(t);
  float espera = -1;
  if (modo == SESION && solPer < per.max)
    espera = max(max(tSolUlt + per.minEntre - t, per.ini + solPer * per.dur / (per.max + 1) - t), 0.0f);
  int total = faseCal == CAL_BASAL ? (int)lroundf(cfg.basalS * BLOQUES_POR_S) : 0;
  unsigned long ahora = millis();
  bool act = activo;
  Ventana vv;
  bool sost = modo == SESION && sostenidoVigente(vv);

  jl = 0; js[0] = 0;
  ja("{\"v\":\"%s\",\"boot\":%lu,\"modo\":\"%s\",\"fase\":\"%s\"", VERSION_ATRIL, (unsigned long)idArranque, NOMBRE_MODO[modo], NOMBRE_FASE[faseCal]);
  ja(",\"bloq\":%d,\"total\":%d,\"datos\":%d,\"rms\":%d,\"suave\":%.1f,\"calibrado\":%d,\"uon\":%.1f,\"uoff\":%.1f,\"uorig\":%d,\"ctl\":%d",
     bloquesFase, total, datosOK() ? 1 : 0, rmsUltimo, rmsSuave, calibrado ? 1 : 0, umbralOn, umbralOff, umbralOrigen, controlAuto ? 1 : 0);
  ja(",\"activo\":%d,\"nivel\":%d,\"durAct\":%.1f,\"sid\":%d,\"nAct\":%d,\"tAct\":%.1f,\"tSes\":%.1f",
     act ? 1 : 0, (int)(nivel * 100), act ? (ahora - tInicioActivo) / 1000.0f : 0.0f, sesionId,
     nActivaciones, tiempoActivoTotal / 1000.0f, msSesion / 1000.0f);
  ja(",\"alt\":%d,\"estOK\":%d,\"malas\":%lu,\"rssi\":%d",
     (int)(altura() * 100), modoE != E_SIN_TOPE ? 1 : 0, lineasMalas, (int)WiFi.RSSI());
  ja(",\"est\":%d,\"bEst\":%.1f,\"sost\":%d,\"ep\":%lu,\"nDet\":%lu",
     estadoRms, bloquesEnEstado / (float)BLOQUES_POR_S, sost ? 1 : 0, (unsigned long)episodio, (unsigned long)nDetectados);
  ja(",\"cb\":{\"media\":%.2f,\"sd\":%.2f,\"cv\":%.3f,\"der\":%.3f,\"mn\":%.1f,\"mx\":%.1f,\"n\":%d,\"fl\":%u,\"niv\":%d}",
     cb.media, cb.sd, cb.cv, cb.deriva, cb.minimo, cb.maximo, cb.n, (unsigned)cb.flags, cb.nivel);
  ja(",\"ver\":{\"per\":%d,\"max\":%d,\"sol\":%d,\"tPer\":%.1f,\"dur\":%.0f,\"esp\":%.1f,\"pend\":%lu,\"nr\":[%lu,%lu],\"ne\":[%lu,%lu]}",
     modo == SESION ? per.idx : -1, per.max, solPer, max(t - per.ini, 0.0f), per.dur, espera, (unsigned long)pendId,
     (unsigned long)nResp[1], (unsigned long)nResp[2], (unsigned long)nErr[1], (unsigned long)nErr[2]);
  ja(",\"cfg\":{\"basal\":%.1f,\"desc\":%.1f,\"k\":%.2f,\"f\":%.2f,\"tSost\":%.1f,\"v1s\":%.0f,\"v1max\":%d,\"v1min\":%.0f,\"vcs\":%.0f,\"vcmax\":%d,\"vcmin\":%.0f,\"rever\":%.0f,\"resp\":%.0f,\"hip\":%d,\"bps\":%d}",
     cfg.basalS, cfg.descBasalS, cfg.k, cfg.f, cfg.tSost, cfg.verIniS, cfg.verIniMax, cfg.verIniMin,
     cfg.verCicloS, cfg.verCicloMax, cfg.verCicloMin, cfg.tRever, cfg.tResp, cfg.hip, BLOQUES_POR_S);
  ja(",\"hist\":[");
  for (int i = 0; i < histN; i++) ja(i ? ",[%.1f,%.1f,%d,%.0f]" : "[%.1f,%.1f,%d,%.0f]", hist[i].on, hist[i].off, hist[i].origen, hist[i].tSes);
  ja("],\"rev\":%lu,\"er\":%lu,\"ev\":[", (unsigned long)rev, (unsigned long)er);
  for (int i = 0; i < nEvSal; i++) {
    const Evento &e = ev[i];
    ja(i ? ",[%lu,%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]" : "[%lu,%lu,%lu,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f]",
       (unsigned long)e.id, (unsigned long)e.rev, (unsigned long)e.ep, e.origen, e.estado, e.status, e.postura, e.sid, e.periodo, e.nSol,
       e.calidad, (unsigned long)e.tok, e.tIni, e.dur, e.media, e.sd, e.minimo, e.maximo, e.uon, e.uoff);
  }
  ja("],\"bn\":%lu,\"b\":[", (unsigned long)nb);
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

void responder(int codigo, bool ok, const char *err) {
  cabeceras();
  char t[96];
  if (ok) snprintf(t, sizeof t, "{\"ok\":true}");
  else snprintf(t, sizeof t, "{\"ok\":false,\"e\":\"%s\"}", err ? err : "error");
  web.send(codigo, "application/json", t);
}

bool encolarWeb(const CmdWeb &c) { return colaWeb && xQueueSend(colaWeb, &c, 0) == pdTRUE; }

uint8_t posturaDe(const String &p) {
  if (p.length() != 1) return 0;
  char c = p[0];
  return c == 'E' ? POST_ERGUIDA : c == 'C' ? POST_CAIDA : c == 'D' ? POST_INDETERMINADA : 0;
}

void manejarCmd() {
  String c = web.arg("c");
  const char *validos = "c1xfsnprhai";
  if (c.length() != 1 || !strchr(validos, c[0])) { responder(400, false, "comando"); return; }
  char ch = c[0];
  bool ok = colaCmd && xQueueSend(colaCmd, &ch, 0) == pdTRUE;
  responder(ok ? 200 : 503, ok, "cola llena");
}

// Postura confirmada para un evento pendiente (idempotente: un reintento no duplica)
void manejarConf() {
  uint8_t p = posturaDe(web.arg("p"));
  long id = web.arg("id").toInt();
  if (!p || id <= 0) { responder(400, false, "datos"); return; }
  CmdWeb c = {C_CONF, p, (uint32_t)id, 0, 0, 0, ""};
  bool ok = encolarWeb(c);
  responder(ok ? 200 : 503, ok, "cola llena");
}

void manejarManual() {
  uint8_t p = posturaDe(web.arg("p"));
  long n = web.arg("n").toInt();
  if (!p || n <= 0) { responder(400, false, "datos"); return; }
  CmdWeb c = {C_MANUAL, p, 0, (uint32_t)n, 0, 0, ""};
  bool ok = encolarWeb(c);
  responder(ok ? 200 : 503, ok, "cola llena");
}

void manejarUmbral() {
  float on = web.arg("on").toFloat(), off = web.arg("off").toFloat();
  long n = web.arg("n").toInt();
  if (!(off > 0 && on > off && on <= 5000) || n <= 0) { responder(400, false, "umbral"); return; }
  CmdWeb c = {C_UMBRAL, 0, 0, (uint32_t)n, on, off, ""};
  bool ok = encolarWeb(c);
  responder(ok ? 200 : 503, ok, "cola llena");
}

void manejarRestaurar() {
  long n = web.arg("n").toInt();
  if (n <= 0) { responder(400, false, "datos"); return; }
  CmdWeb c = {C_RESTAURAR, 0, 0, (uint32_t)n, 0, 0, ""};
  bool ok = encolarWeb(c);
  responder(ok ? 200 : 503, ok, "cola llena");
}

void manejarCfg() {
  String k = web.arg("k");
  float v = web.arg("v").toFloat();
  if (k.length() == 0 || k.length() > 7 || !ajustarCfg(k.c_str(), v, false)) { responder(400, false, "parametro"); return; }
  CmdWeb c = {C_CFG, 0, 0, 0, v, 0, ""};
  strncpy(c.k, k.c_str(), sizeof(c.k) - 1);
  bool ok = encolarWeb(c);
  responder(ok ? 200 : 503, ok, "cola llena");
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
                              "\nLa pagina usa /estado y /cmd, /conf, /manual, /umbral, /restaurar, /cfg\n");
}

void tareaRed(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // respuesta rapida a la pagina
  WiFi.config(IP_ATRIL, IP_PUERTA, MASCARA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(RED_SSID, RED_CLAVE);
  web.on("/estado", HTTP_GET, manejarEstado);
  web.on("/cmd", HTTP_GET, manejarCmd);
  web.on("/conf", HTTP_GET, manejarConf);
  web.on("/manual", HTTP_GET, manejarManual);
  web.on("/umbral", HTTP_GET, manejarUmbral);
  web.on("/restaurar", HTTP_GET, manejarRestaurar);
  web.on("/cfg", HTTP_GET, manejarCfg);
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
  colaWeb = xQueueCreate(16, sizeof(CmdWeb));
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
