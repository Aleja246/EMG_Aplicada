
// =====================================================================
//  ASTRA - Firmware del atril (ESP32) v8 SIN LAPTOP (sonido + red ASTRA + pagina web)
//  La senal sEMG controla los actuadores.
// ---------------------------------------------------------------------
//  Entrada: el Arduino UNO R4 (diagnostico_emg_enlace.ino) manda por el
//  opto, cada 100 ms, una linea con el RMS en milivoltios ("640\n").
//
//  Que hace:
//   1. ARRANQUE: las estrellas buscan el microswitch; ahi queda la posicion 0
//      y empieza su ciclo (como motor_endstop.ino). El lazy susan gira desde el encendido.
//   2. CALIBRANDO: 10 s de RMS basal con la cabeza apoyada (luz azul).
//      Descarta el primer segundo. U_on = R + 2 SD (minimo +2 mV), sobre la
//      MEDIANA de 0.5 s (quita saltos sueltos: el umbral queda mas cerca del basal);
//      U_off = R + 0.6 (U_on - R). Sin contracciones solicitadas.
//      Termina a los 10 s por reloj; si la senal es inestable solo avisa.
//      Las estrellas ya van y vienen, lento.
//   3. SESION:
//      - Activacion: RMS suave sobre U_on por 300 ms. Termina: bajo U_off
//        por 300 ms.
//      - CABEZA ABAJO = RMS SOBRE el umbral (ABAJO_ES_ACTIVIDAD; x lo invierte).
//      - Estrellas (paso completo, como motor_endstop.ino): giran hasta pisar
//        el endstop, regresan a 0, giran al otro lado, pisan, regresan... sin
//        parar. CABEZA ABAJO: aceleran de 200 a 600 pasos/s en 2 s. ARRIBA: 200.
//      - Lazy susan: gira siempre desde el encendido; solo el PARO lo detiene.
//      - Luz: CABEZA ABAJO = brillo maximo y cambia de color rapido (vuelta en
//        4 s). CABEZA ARRIBA = tenue (15 %) y casi quieta (vuelta en 40 s).
//      - Musica: siempre sonando desde el encendido, desde la carpeta /01 de la
//        microSD con DOS listas de 3 pistas: CABEZA ABAJO (001-003) y CABEZA
//        ARRIBA (004-006). Al cambiar la cabeza cambia de lista (siguiente pista);
//        al terminar una pista sigue la siguiente de la misma lista. Cada vez que
//        SUBE la cabeza suena el aviso /ADVERT/0001.mp3 y luego sigue la pista.
//   4. Seguridad: sin datos validos por 500 ms las estrellas pasan a lento y
//      la luz a ambar. PARO (IO34, con USAR_PARO) y la pausa p detienen todo.
//
//  Comandos (monitor serie a 115200, "Sin ajuste de linea"):
//   c = recalibrar   p = pausa / seguir   r = reanudar tras el paro
//   h = volver a buscar el microswitch   + / - = factor SD   i = info
//   x = invertir que estado del EMG es "cabeza abajo"
//   o = probar la siguiente combinacion del lazy susan (6: orden x medio/completo)
//  Boton BOOT de la placa: recalibrar (o reanudar tras el paro).
//
//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca). WiFi, LittleFS,
//  Preferences y el servidor web (esp_http_server, con WebSocket) vienen con
//  el paquete de placas "esp32" de Espressif 3.x: no hace falta otra biblioteca.
//  Esquema de particiones: "No OTA (2MB APP/2MB SPIFFS)" (o el "Default 4MB
//  with spiffs"); la carpeta data/ se sube con "Upload LittleFS".
//
//  v8 = tu firmware con sonido + RED ASTRA + PAGINA WEB. La deteccion, la
//  calibracion, los motores, el microswitch, la luz, la musica, el PARO y los
//  comandos del monitor serie NO cambian:
//   - Crea la red WiFi "ASTRA" (clave en credenciales.h). El celular se une
//     a esa red y abre http://192.168.4.1: el ESP32 le sirve la pagina desde
//     LittleFS (copiada a la RAM al encender, para no leer la flash durante
//     la terapia).
//   - El Arduino se une a la misma red y manda la senal EMG cruda al puerto
//     9001 (paquetes 0xAA 0x55 de 20 muestras con numero de secuencia). El
//     ESP32 la revisa y la reenvia a la pagina por WebSocket (/ws). Los
//     motores siguen usando SOLO el RMS que llega por el opto (Serial2).
//   - Cada 200 ms manda a la pagina su estado (modo, calibracion, umbrales,
//     RMS, activaciones, musica, mensajes) y cada segundo el diagnostico
//     (Arduino conectado, muestras/s, paquetes perdidos, paginas abiertas).
//   - De la pagina acepta solo: recalibrar (c), pausa/seguir (p), la musica
//     de cada lista y el volumen. Con el PARO activo ignora toda orden de la
//     pagina: el paro solo se libera con el boton fisico (y BOOT o "r").
//   - La red y la pagina corren en el nucleo 0; motores, luz, musica y
//     deteccion en el nucleo 1 (loop): la pagina nunca los frena. Sin WiFi,
//     sin Arduino por WiFi o sin celular, el atril funciona igual.
//   - Los mensajes (Log.print...) salen por el monitor serie Y a la pagina.
//
//  Cambios v2 respecto a v1 (mismas reglas del documento):
//   - Buscar el tope siempre va a 400 pasos/s (en v1, despues de una bajada,
//     AccelStepper lo recortaba a 300).
//   - La espera de 1.5 s cuenta desde que se CONFIRMA la relajacion
//     (confirmacion 0.3 s + espera 1.5 s, como el documento; en v1 la espera
//     real era 1.2 s). La duracion se sigue midiendo hasta que la senal bajo
//     del umbral, y el RMS medio usa justo esas muestras (en v1 empezaba
//     despues de la confirmacion).
//   - Confirmacion de 300 ms = 4 bloques seguidos, aunque un bloque llegue
//     unos ms antes (en v1 a veces hacian falta 5 bloques).
//   - Watchdog: al cortarse los datos se cierra la activacion en curso (no
//     suma tiempo sin senal) y se avisa una sola vez en el monitor. Al volver,
//     las estrellas esperan 1.5 s antes de bajar (dan tiempo a reconfirmar).
//   - El frenado por pausa / calibracion / sin datos ya no puede quedar
//     "olvidado" de una sesion anterior.
//   - Microswitch: tambien detiene si lo tocan por inercia al frenar, y fija
//     el tope sin romper la secuencia de las bobinas (sin tiron hacia arriba).
//   - Sonido: si una pista no puede sonar por la pausa de 3 s, suena en
//     cuanto se pueda si su motivo sigue vigente (antes la pista 2 se perdia).
//     Los comandos se mandan aunque el DFPlayer no conteste al arrancar
//     (p. ej. si se enciende con el paro presionado), y el volumen se
//     reenvia cada vez que se busca el tope (tambien con h).
//   - Lectura: la primera linea (puede llegar a medias) y una linea cortada
//     a la mitad se descartan. EMG_INVERTIDO por si el opto invierte.
//   - Las bobinas quedan apagadas desde el primer instante del arranque.
//  v2.1 (diagnostico en el atril):
//   - La linea [SESION] muestra el umbral y dice si las estrellas estan
//     DESACTIVADAS (no se encontro el tope).
//   - Si al bajar del tope el microswitch sigue presionado (cableado en NC o
//     atorado), las estrellas se desactivan y lo avisa, en vez de seguir
//     desenrollando el hilo.
//   - Los planetas arrancan con rampa suave (permite subir V_PLANETAS).
//  v2.2: PREMIO_AL_RELAJAR. Sin contraccion las estrellas suben rapido
//   (700 pasos/s) y la luz lila crece; al contraer esperan 1.5 s y bajan
//   rapido (500 pasos/s). PREMIO_AL_RELAJAR = false vuelve a la logica original.
//  v2.3: el lazy susan gira siempre (salvo PARO o pausa). Las estrellas suben
//   y bajan sin parar entre fondo y tope: rapido (700) con el musculo relajado,
//   lento (250) activo. Orden de bobinas configurable, USAR_TOPE, y el arranque
//   ya no espera al DFPlayer.
//  Para probar motores, luz, microswitch y opto por separado: prueba_atril.ino
// =====================================================================

#include <AccelStepper.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
// v8.3: la pagina va DENTRO del firmware si pagina_web.h esta junto a este .ino (lo genera
// herramientas/preparar_littlefs.py). Asi la pagina siempre es la de esta version y no hace falta
// subir LittleFS. Sin pagina_web.h se usa la carpeta data/ de LittleFS, como antes.
#if __has_include("pagina_web.h")
#include "pagina_web.h"
#define HAY_PAGINA_INTERNA 1
#else
#define HAY_PAGINA_INTERNA 0
#endif

// ------------------------- Pines (esquematico) -------------------------
// Motor de giro (J4):      IN1..IN4 = IO23, IO22, IO21, IO19
// Motor de estrellas (J3): IN1..IN4 = IO18, IO17, IO16, IO4
// El 28BYJ-48 necesita el orden IN1, IN3, IN2, IN4 en AccelStepper.
// El "false" final: los pines se configuran en setup() con las bobinas apagadas.
// Si un motor VIBRA en vez de girar parejo, cambia su ORDEN_B a true
// (prueba primero con la tecla o de prueba_atril.ino).
// LAZY SUSAN: si VIBRA y no gira, escribe o en el monitor serie para probar la
// siguiente combinacion en vivo (6: 3 ordenes de bobinas x medio paso / paso
// completo). El paso completo da mas fuerza si el motor no alcanza a mover la
// carga. Cuando gire parejo, pon ese numero en ORDEN_PLANETAS y vuelve a cargar.
const int  N_ORDENES_P       = 6;
const int  ORDEN_PLANETAS    = 0;  // 0 a 5
const bool ORDEN_B_ESTRELLAS = false;
AccelStepper planetasOrden[N_ORDENES_P] = {
  AccelStepper(AccelStepper::HALF4WIRE, 23, 21, 22, 19, false),   // 0: medio paso, IN1 IN3 IN2 IN4 (normal)
  AccelStepper(AccelStepper::HALF4WIRE, 23, 22, 21, 19, false),   // 1: medio paso, IN1 IN2 IN3 IN4
  AccelStepper(AccelStepper::HALF4WIRE, 23, 19, 22, 21, false),   // 2: medio paso, IN1 IN4 IN2 IN3
  AccelStepper(AccelStepper::FULL4WIRE, 23, 21, 22, 19, false),   // 3: paso completo (mas fuerza), IN1 IN3 IN2 IN4
  AccelStepper(AccelStepper::FULL4WIRE, 23, 22, 21, 19, false),   // 4: paso completo, IN1 IN2 IN3 IN4
  AccelStepper(AccelStepper::FULL4WIRE, 23, 19, 22, 21, false)};  // 5: paso completo, IN1 IN4 IN2 IN3
const char *NOMBRE_ORDEN_P[N_ORDENES_P] = {"medio paso, IN1 IN3 IN2 IN4", "medio paso, IN1 IN2 IN3 IN4",
  "medio paso, IN1 IN4 IN2 IN3", "paso completo, IN1 IN3 IN2 IN4", "paso completo, IN1 IN2 IN3 IN4",
  "paso completo, IN1 IN4 IN2 IN3"};
int ordenP = ORDEN_PLANETAS;
AccelStepper *planetas = &planetasOrden[ORDEN_PLANETAS];
// Estrellas: paso completo, orden IN1, IN3, IN2, IN4 (igual que motor_endstop.ino)
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
// ---- Estrellas (como motor_endstop.ino) ----
// Al encender, la posicion actual es 0. Gira hacia SENTIDO_INICIAL hasta pisar el
// endstop, regresa a 0 (todo lo que avanzo), gira hacia el otro lado hasta pisar
// el endstop, regresa a 0... y asi sin parar. El endstop SOLO actua sobre las estrellas.
const int  SENTIDO_INICIAL = 1;      // +1 = hacia un lado, -1 = hacia el otro
const long MUY_LEJOS       = 1000000L;   // objetivo "infinito": gira hasta pisar el endstop
// Arranque (como antes): al encender buscan el microswitch y ahi queda la posicion 0
const int  V_HOMING_E      = 200;        // pasos completos/s al buscar el microswitch
                                         // (= los 400 medios pasos/s del arranque de la V3)
const long HOMING_MAX_PASOS = 20000;     // si no lo encuentra en estos pasos, sigue sin el
// Velocidades en pasos completos por segundo (el 28BYJ-48 aguanta hasta ~600-700)
const int  V_ESTRELLAS_RAPIDO = 1000;  // con la CABEZA ABAJO
const int  V_ESTRELLAS_LENTO  = 600;  // con la CABEZA ARRIBA, calibrando o sin datos
const int  ACELERACION   = 1500;    // pasos/s^2: que tan rapido arranca, frena y cambia de sentido
const int  V_PLANETAS    = 400;    // pasos/s (~1.8 rpm, una vuelta del eje en ~34 s). Hasta ~400 si quieres mas rapido
const int  ACEL_PLANETAS = 200;    // pasos/s^2: arranque suave del lazy susan

// ------------------------- Ajustes de deteccion -----------------------
const float UMBRAL_K_INICIAL = 3.0f; // factor sobre la SD basal ROBUSTA (v8.1: antes 2.0; +/- de 0.5 en 0.5)
const float MARGEN_REL_INICIAL = 0.20f; // v8.1: el umbral queda al menos 20 % sobre el reposo
// v8.1: sensibilidad elegida en la pagina -> {k, margen relativo}
const float SENS_K[3]   = {2.0f, 3.0f, 4.0f};
const float SENS_REL[3] = {0.10f, 0.20f, 0.35f};
const int N_CAL_MAX = 160;              // v8.1: bloques que se guardan para la mediana (10 s = ~90)
const int N_ARRIBA = 30;                // v8.2: 3 s de medianas para el nivel con la cabeza arriba
const float SUELTA_FRAC = 0.40f;        // v8.2: termina al bajar al 40 % del camino arriba -> pico
const unsigned long ASENTAR_MS = 800;   // v8.3: tras subir la cabeza, espera esto para reaprender el nivel
const float MARGEN_MIN_MV    = 2.0f; // el umbral queda al menos 2 mV sobre el basal
const float UMBRAL_OFF_FACTOR = 0.6f; // histeresis relativa a la media basal
const unsigned long T_CONFIRMAR = 300;  // ms sobre/bajo el umbral para cambiar de estado
const unsigned long HOLGURA_MS  = 50;   // tolera que un bloque llegue unos ms antes (300 ms = 4 bloques seguidos)
const unsigned long T_PREMIO    = 1000; // ms sostenidos para el sonido de activacion
const unsigned long WATCHDOG_MS = 500;  // sin datos por mas de esto -> todo se detiene
const int   N_MEDIANA           = 13;   // v8.3 (antes 5): la senal que se compara es la MEDIANA de los ultimos
                                        // 13 bloques (1.3 s): quita el vaiven, los saltos sueltos y los espasmos breves
const unsigned long CAL_MS = 10000;          // 10 s de RMS basal (por reloj: nunca se traba)
const unsigned long CAL_DESCARTAR_MS = 1000;  // se descarta el primer segundo
const int CAL_MIN_MUESTRAS = 20;              // minimo de bloques para aceptar la calibracion
const float CAL_CV_MAX = 0.50f;        // si SD/media pasa esto, AVISA (ya no detiene el atril)

// ------------------------- Luz y sonido -------------------------------
const int BRILLO_MAX = 128;        // tope del 50 %
const int PASO_LUZ   = 2;          // por cada 20 ms -> 0 a 128 en ~1.3 s
const int VOLUMEN    = 15;         // 0 a 30
const unsigned long PAUSA_SONIDO = 3000;  // ms minimos entre sonidos
// Musica: SIEMPRE sonando desde el encendido (arranque, calibracion y sesion).
// Solo se detiene con el PARO. MicroSD (FAT32):
//   /01/001.mp3 ... /01/006.mp3   canciones
//   /ADVERT/0001.mp3               aviso "subio la cabeza"
// Hay DOS listas de 3 pistas (numero de archivo dentro de /01):
//   CABEZA ABAJO  -> pistasAbajo   (por defecto 001, 002, 003)
//   CABEZA ARRIBA -> pistasArriba  (por defecto 004, 005, 006)
// Al cambiar la cabeza de posicion, pasa a la SIGUIENTE pista de la otra lista.
// Cuando una pista termina, sigue la siguiente de la misma lista.
// Cada vez que SUBE la cabeza suena /ADVERT/0001.mp3 encima de la musica y, al
// terminar, el DFPlayer continua la pista que estaba sonando.
// PAGINA WEB: la pagina elige las 3 pistas de cada lista; el atril llama a
// configurarPistas() y las guarda en su memoria (ver aplicarMusica()).
const int CARPETA_MUSICA   = 1;     // carpeta /01
const int PISTAS_POR_LISTA = 3;
const int PISTA_AVISO      = 1;     // /ADVERT/0001.mp3
const unsigned long AVISO_MAX_MS = 10000;   // duracion maxima del aviso (si el DFPlayer no avisa el final)
const unsigned long T_ABAJO_MIN_MS  = 1000;  // la cabeza debe haber estado abajo al menos esto
const unsigned long T_ENTRE_CANCIONES = 5000; // tiempo minimo entre cambios de lista (evita rebotes)
// Luz relajado: cambia de color cada vez mas rapido y mas brillante.
const float PERIODO_COLOR_LENTO  = 40000;  // ms por vuelta de colores con la CABEZA ARRIBA (casi quieta)
const float PERIODO_COLOR_RAPIDO = 4000;   // ms por vuelta con la CABEZA ABAJO (no bajar de ~3000)
const float BRILLO_MIN_FRAC      = 0.15;   // brillo con la cabeza arriba (fraccion de BRILLO_MAX)

// ------------------------- Enlace y paro ------------------------------
// Que premia el atril:
//   true  = RELAJAR: sin contraccion las estrellas suben rapido y la luz crece
//           en color lila; al contraer esperan 1.5 s y bajan rapido.
//   false = CONTRAER: la logica original del documento.
const bool PREMIO_AL_RELAJAR = true;     // (se conserva; lo que manda ahora es ABAJO_ES_ACTIVIDAD)
// Con los semiespinosos, al BAJAR la cabeza el musculo trabaja para sostenerla,
// asi que el RMS SUBE: RMS sobre el umbral = CABEZA ABAJO.
// Si en otro montaje resulta al reves, pon false (o escribe x en el monitor).
const bool ABAJO_ES_ACTIVIDAD = true;
const int  LILA_R = 80, LILA_G = 0, LILA_B = 128;   // color del premio al relajar (0..128)
const unsigned long T_BRILLO_RELAJADO = 2000;      // ms con la cabeza abajo hasta el maximo (luz y estrellas)

const bool USAR_PARO = false;      // pon true cuando el divisor del paro (R6, R7) este conectado
const bool EMG_INVERTIDO = false;  // pon true si el opto invierte (aviso "!! Llegan bytes ...")
const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta

// ------------------------- Red ASTRA y pagina web ----------------------
// La clave va en credenciales.h (copia credenciales_ejemplo.h; ese archivo no
// se sube a GitHub) o aqui abajo. Tiene que ser IGUAL en el Arduino.
#if __has_include("credenciales.h")
#include "credenciales.h"
#endif
#ifndef ASTRA_RED
#define ASTRA_RED   "ASTRA"
#endif
#ifndef ASTRA_CLAVE
#define ASTRA_CLAVE "CAMBIA-ESTA-CLAVE"   // 8 a 63 caracteres
#endif
const bool USAR_WIFI = true;              // false = atril sin red (igual que antes)
const char *RED_SSID  = ASTRA_RED;
const char *RED_CLAVE = ASTRA_CLAVE;
const int  CANAL_WIFI = 6;
const int  MAX_EQUIPOS_RED = 5;           // el Arduino + hasta 4 celulares o computadoras
const int  MAX_PAGINAS = 3;               // paginas abiertas a la vez (las mas viejas se cierran)
const uint16_t PUERTO_ARDUINO = 9001;     // senal cruda del Arduino
const uint16_t PUERTO_AVISO   = 9999;     // aviso UDP "aqui esta ASTRA" (el Arduino tambien tiene la IP fija)
const char *VERSION_ATRIL = "8.3";

// ======================================================================
enum Modo { ARRANQUE, CALIBRANDO, SESION, PAUSA, PARO };
enum EstadoEstrellas { E_BUSCANDO, E_GIRANDO, E_REGRESANDO };
const char *NOMBRE_MODO[] = {"ARRANQUE", "CALIBRANDO", "SESION", "PAUSA", "PARO"};

Modo modo = ARRANQUE;
EstadoEstrellas estadoE = E_GIRANDO;
int  sentidoE = SENTIDO_INICIAL;
bool endstopArmado = false;          // evita contar el mismo picado dos veces
long vueltasEndstop = 0;
long pasosHoming = 0;
bool abajoEsActividad = ABAJO_ES_ACTIVIDAD;
unsigned long tCambioCabeza = 0;     // cuando cambio por ultima vez arriba <-> abajo
bool abajoAntes = false;
bool cambiarCancion = false;         // la cabeza acaba de SUBIR: falta tocar el aviso ADVERT
unsigned long tInicioAbajo = 0, tUltimoCambioCancion = 0;

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
float rmsSuave = 0;                 // mediana de 0.5 s: es la que se compara con el umbral
float ventana[N_MEDIANA];
int ventanaN = 0, ventanaI = 0;
bool hayDatos = false;
bool datosAntes = false;

// --- Calibracion ---
// Acumula solo el RMS basal; no se requiere una referencia de contraccion.
unsigned long tCal = 0;             // cuando empezo la calibracion
int nReposo = 0;
double sumaReposo = 0.0, suma2Reposo = 0.0;
bool calibrado = false;
float reposo = 100, sdReposo = 10;
float umbralK = UMBRAL_K_INICIAL;
float margenRel = MARGEN_REL_INICIAL;   // v8.1
int sensibilidad = 2;                   // v8.1: 1 mas sensible, 2 normal, 3 menos sensible
float calVals[N_CAL_MAX];               // v8.1: la mediana de cada bloque de la calibracion
float histArriba[N_ARRIBA];             // v8.2: ultimas medianas fuera de una activacion
int nArriba = 0, iArriba = 0;
float baseAct = 0, uOnAct = 0, picoAct = 0;   // v8.2: de la activacion en curso
unsigned long tSubio = 0;               // v8.3: cuando se detecto la ultima subida
float uTrasSubir = 0;                   // v8.3: umbral de caida mientras se reaprende el nivel de arriba
volatile float uAhoraPub = 0, uSueltaPub = 0, arribaPub = 0;   // v8.2: para la pagina (los calcula el loop)
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

// --- Motores ---
bool frenando = false;
bool bobinasEstrellas = false, bobinasPlanetas = false;
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
float faseColor = 0;                // 0..1, posicion en la rueda de colores
bool musicaSonando = false;
int cancion = 4;                    // archivo de /01 que esta sonando (001..255)
// Listas de musica (las podra cambiar la pagina web con configurarPistas)
int pistasAbajo[PISTAS_POR_LISTA]  = {4, 5, 6};   // v8.2: /01/004, 005, 006 con la CABEZA ABAJO
int pistasArriba[PISTAS_POR_LISTA] = {1, 2, 3};   // v8.2: /01/001, 002, 003 con la CABEZA ARRIBA
int idxAbajo = -1, idxArriba = -1;  // posicion en cada lista (-1 = aun no suena)
bool listaAbajoSonando = false;     // que lista esta sonando
bool pistaTerminada = false;        // el DFPlayer aviso que termino la pista
bool avisoSonando = false;          // esta sonando /ADVERT/0001.mp3
unsigned long tAviso = 0, tUltimoFin = 0;
uint8_t dfBuf[10];                  // respuesta del DFPlayer (10 bytes)
int dfN = 0;
unsigned long episodioCancion = 0;  // inicio del "cabeza abajo" que ya cambio la cancion
unsigned long tMusicaLista = 2300;  // la musica arranca despues de mandar el volumen
unsigned long ultimoCmdMusica = 0;
unsigned long ultimaLuz = 0, ultimoEstado = 0;

// --- Pagina web ---
// Lo escribe el nucleo 1 (loop) y lo lee el nucleo 0 (red): se copia con candado.
portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;
const int N_LOG = 24, LARGO_LOG = 160;
char logTxt[N_LOG][LARGO_LOG];      // ultimos mensajes (los mismos del monitor serie)
uint32_t nLog = 0;
const int N_BLQ = 64;               // ultimos 6.4 s de bloques recibidos
int32_t blqRms[N_BLQ];
float blqMed[N_BLQ];
uint8_t blqFlag[N_BLQ];             // 1 = sobre el umbral (activacion), 2 = usado en la calibracion
uint32_t nBlq = 0;
struct Activacion { int sid, id; float ini, dur; int rms; };
const int N_ACT = 32;
Activacion actLista[N_ACT];
uint32_t nActLista = 0;
QueueHandle_t colaCmd = NULL;       // comandos de la pagina -> loop
struct PedidoMusica { uint8_t abajo; uint8_t p[3]; };
QueueHandle_t colaMusica = NULL;    // listas de musica de la pagina -> loop
QueueHandle_t colaVolumen = NULL;   // volumen de la pagina -> loop
QueueHandle_t colaAvisos = NULL;    // mensajes de la red -> loop (los imprime)
QueueHandle_t colaSens = NULL;      // v8.1: sensibilidad de la pagina -> loop
volatile bool senalPedida = true;   // v8.1: la pagina quiere ver la senal en vivo
void avisar(int8_t a) { if (colaAvisos) xQueueSend(colaAvisos, &a, 0); }
int volumen = VOLUMEN;              // volumen en uso (0 a 30); VOLUMEN es el de fabrica
bool reiniciarLista = false;        // la lista que suena cambio: empezar ya con la nueva
Preferences prefs;
uint32_t idArranque = 0;            // cambia en cada encendido (la pagina lo detecta)
int sesionId = 0;                   // sube al terminar cada calibracion (sesion nueva)
unsigned long msSesion = 0;         // tiempo en SESION (sin pausas)
unsigned long msSesionIni = 0;      // msSesion al empezar la activacion en curso
int calN = 0;                       // bloques usados en la ultima calibracion
bool calPocos = false, calInestable = false;

// Imprime en el monitor serie y guarda cada linea para la pagina
class Registro : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial.write(c);
    if (c == '\r') return 1;
    if (c == '\n') { cerrar(); return 1; }
    if (n < LARGO_LOG - 1) buf[n++] = (char)c;
    return 1;
  }
  size_t write(const uint8_t *b, size_t k) override { for (size_t i = 0; i < k; i++) write(b[i]); return k; }
 private:
  char buf[LARGO_LOG];
  int n = 0;
  void cerrar() {
    if (!n) return;                 // las lineas vacias solo van al monitor
    buf[n] = 0;
    portENTER_CRITICAL(&candado);
    memcpy(logTxt[nLog % N_LOG], buf, n + 1);
    nLog++;
    portEXIT_CRITICAL(&candado);
    n = 0;
  }
};
Registro Log;

void guardarBloque(int mv, float med, uint8_t flag) {
  portENTER_CRITICAL(&candado);
  blqRms[nBlq % N_BLQ] = mv;
  blqMed[nBlq % N_BLQ] = med;
  blqFlag[nBlq % N_BLQ] = flag;
  nBlq++;
  portEXIT_CRITICAL(&candado);
}

// ======================================================================
//  Utilidades
// ======================================================================
bool topePresionado() {
  if (digitalRead(PIN_TOPE) == HIGH) return false;
  delayMicroseconds(300);
  return digitalRead(PIN_TOPE) == LOW;
}

bool datosOK() {
  return hayDatos && (millis() - tUltimoDato) < WATCHDOG_MS;
}

// CABEZA ABAJO segun el estado del EMG (ver ABAJO_ES_ACTIVIDAD)
bool cabezaAbajo() { return abajoEsActividad ? activo : !activo; }

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

// ---------------- Configuracion de las listas de musica ----------------
// Para la PAGINA WEB: cambia las 3 pistas de una lista (numero de archivo en /01,
// de 1 a 255). abajo = true -> lista de CABEZA ABAJO; false -> CABEZA ARRIBA.
// El cambio se oye en la siguiente pista de esa lista. Devuelve false si algun
// numero no es valido (y no cambia nada).
bool configurarPistas(bool abajo, int p1, int p2, int p3) {
  int nuevas[PISTAS_POR_LISTA] = {p1, p2, p3};
  for (int k = 0; k < PISTAS_POR_LISTA; k++) if (nuevas[k] < 1 || nuevas[k] > 255) return false;
  int *lista = abajo ? pistasAbajo : pistasArriba;
  for (int k = 0; k < PISTAS_POR_LISTA; k++) lista[k] = nuevas[k];
  Log.printf(">> Lista CABEZA %s: %03d, %03d, %03d\n", abajo ? "ABAJO" : "ARRIBA", p1, p2, p3);
  return true;
}

// Listas guardadas en la memoria del ESP32 (las eligio la pagina)
void cargarListas() {
  uint8_t a[PISTAS_POR_LISTA], b[PISTAS_POR_LISTA];
  prefs.begin("astra", false);
  size_t na = prefs.getBytes("abajo", a, sizeof a), nb = prefs.getBytes("arriba", b, sizeof b);
  volumen = constrain((int)prefs.getUChar("vol", VOLUMEN), 0, 30);
  prefs.end();
  if (na == sizeof a && a[0] && a[1] && a[2]) for (int k = 0; k < PISTAS_POR_LISTA; k++) pistasAbajo[k] = a[k];
  if (nb == sizeof b && b[0] && b[1] && b[2]) for (int k = 0; k < PISTAS_POR_LISTA; k++) pistasArriba[k] = b[k];
}

void guardarLista(bool abajo) {
  uint8_t v[PISTAS_POR_LISTA];
  for (int k = 0; k < PISTAS_POR_LISTA; k++) v[k] = (uint8_t)(abajo ? pistasAbajo : pistasArriba)[k];
  prefs.begin("astra", false);
  prefs.putBytes(abajo ? "abajo" : "arriba", v, sizeof v);
  prefs.end();
}

// Pedido de la pagina: cambia, guarda y, si esa lista es la que suena, empieza ya con ella
void aplicarMusica(const PedidoMusica &m) {
  if (!configurarPistas(m.abajo, m.p[0], m.p[1], m.p[2])) return;
  guardarLista(m.abajo);
  if (musicaSonando && (bool)m.abajo == listaAbajoSonando) reiniciarLista = true;
}

// Volumen pedido por la pagina (0 a 30): se aplica ya y se guarda
void aplicarVolumen(int v) {
  volumen = constrain(v, 0, 30);
  dfEnviar(0x06, volumen);
  ultimoCmdMusica = millis();
  prefs.begin("astra", false);
  prefs.putUChar("vol", (uint8_t)volumen);
  prefs.end();
  Log.printf(">> Volumen: %d de 30\n", volumen);
}

// Toca una pista de la lista de cabeza abajo / arriba (carpeta /01).
// avanzar = true: la siguiente de la lista; false: la que tocaba (o la primera).
void tocarLista(bool abajo, bool avanzar) {
  int &idx = abajo ? idxAbajo : idxArriba;
  if (avanzar || idx < 0) idx = (idx + 1) % PISTAS_POR_LISTA;
  cancion = (abajo ? pistasAbajo : pistasArriba)[idx];
  listaAbajoSonando = abajo;
  dfEnviar(0x0F, (CARPETA_MUSICA << 8) | cancion);   // carpeta + archivo
  musicaSonando = true;
  pistaTerminada = false;
  avisoSonando = false;
  ultimoCmdMusica = millis();
  Log.printf("   >> Musica (cabeza %s): /%02d/%03d.mp3\n", abajo ? "ABAJO" : "arriba", CARPETA_MUSICA, cancion);
}

// Lee las respuestas del DFPlayer: solo interesa "termino una pista" (0x3D).
void leerDFPlayer() {
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    if (dfN == 0 && b != 0x7E) continue;           // espera el inicio de trama
    dfBuf[dfN++] = b;
    if (dfN < 10) continue;
    dfN = 0;
    if (dfBuf[9] != 0xEF || dfBuf[3] != 0x3D) continue;
    unsigned long ahora = millis();
    if (ahora - tUltimoFin < 1000) continue;       // el DFPlayer suele mandar el aviso 2 veces
    tUltimoFin = ahora;
    if (avisoSonando) {                            // termino el ADVERT: la cancion sigue sola
      avisoSonando = false;
      Log.println("   >> Fin del aviso: sigue la cancion");
    } else {
      pistaTerminada = true;                       // termino la cancion: toca la siguiente
    }
  }
}

void bobinasE(bool on) {
  if (on == bobinasEstrellas) return;
  if (on) estrellas.enableOutputs(); else estrellas.disableOutputs();
  bobinasEstrellas = on;
}

void bobinasP(bool on) {
  if (on == bobinasPlanetas) return;
  if (on) planetas->enableOutputs(); else planetas->disableOutputs();
  bobinasPlanetas = on;
}

void detenerEstrellasYa() {
  estrellas.setCurrentPosition(estrellas.currentPosition());  // alto inmediato, conserva la posicion
  bobinasE(false);
}

// Frenado suave (800 pasos/s^2). Si ya estan quietas, cancela cualquier destino.
void frenarEstrellas() {
  if (estrellas.speed() == 0) estrellas.moveTo(estrellas.currentPosition());
  else estrellas.stop();
}

// v8.1: mediana de v[0..n) o, con abs = true, mediana de |v - centro| (MAD)
float medianaDe(const float *v, int n, float centro, bool abs) {
  static float t[N_CAL_MAX];
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) t[i] = abs ? fabsf(v[i] - centro) : v[i];
  for (int a = 1; a < n; a++) { float x = t[a]; int b = a - 1; while (b >= 0 && t[b] > x) { t[b + 1] = t[b]; b--; } t[b + 1] = x; }
  return n % 2 ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

// v8.2: nivel con la cabeza arriba (ultimos 3 s fuera de una activacion), nunca menor que el reposo
float nivelArribaActual() {
  if (nArriba < 10) return reposo;
  return max(reposo, medianaDe(histArriba, nArriba, 0.0f, false));
}
float margenSobre(float base) { return max(max(umbralK * sdReposo, MARGEN_MIN_MV), margenRel * base); }
float umbralOnAhora() {
  if (nArriba < 10) return max(umbralOn, uTrasSubir);   // v8.3: reaprendiendo el nivel de arriba
  float b = nivelArribaActual(); return max(umbralOn, b + margenSobre(b));
}
float umbralSueltaAhora() {
  float off = baseAct + UMBRAL_OFF_FACTOR * (uOnAct - baseAct);
  return max(off, baseAct + SUELTA_FRAC * (picoAct - baseAct));
}
void guardarArriba(float v) { histArriba[iArriba] = v; iArriba = (iArriba + 1) % N_ARRIBA; if (nArriba < N_ARRIBA) nArriba++; }

void calcularUmbrales() {
  umbralOn = reposo + max(max(umbralK * sdReposo, MARGEN_MIN_MV), margenRel * reposo);   // v8.1: + margen relativo
  umbralOff = reposo + UMBRAL_OFF_FACTOR * (umbralOn - reposo);
}

void imprimirCalibracion() {
  Log.println();
  Log.println("---- Calibracion ----");
  Log.printf("Reposo: %.0f mV (SD %.1f)\n", reposo, sdReposo);
  Log.printf("Umbral de activacion: %.1f mV (mediana + %.1f SD robusta, minimo +%.0f %%; sensibilidad %d)\n", umbralOn, umbralK, margenRel * 100, sensibilidad);
  Log.printf("Umbral de relajacion: %.0f mV\n", umbralOff);
  Log.printf("Activaciones: %d | tiempo activo total: %.1f s | lineas danadas: %lu\n",
                nActivaciones, tiempoActivoTotal / 1000.0f, lineasMalas);
  Log.println("---------------------");
}

// Termina la activacion en curso. 'fin' es cuando el musculo realmente se
// relajo (para la tabla). La espera de 1.5 s antes de bajar cuenta desde ahora.
// Imprime una linea por activacion: sirve para la tabla de resultados.
void terminarActivacion(unsigned long fin) {
  if (activo) {
    activo = false;
    tCambioCabeza = millis();          // cambia de cabeza abajo <-> arriba
    unsigned long dur = fin - tInicioActivo;
    tiempoActivoTotal += dur;
    float rmsMedio = nRmsAct ? sumaRmsAct / nRmsAct : 0;
    Log.printf("   Activacion #%d: %.1f s, RMS medio %.0f mV\n",
                  nActivaciones, dur / 1000.0f, rmsMedio);
    Activacion a = {sesionId, nActivaciones, msSesionIni / 1000.0f, dur / 1000.0f, (int)lroundf(rmsMedio)};
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
void iniciarSesion();
void iniciarCalibracion();

// Estrellas: la posicion actual es 0 y empiezan a girar (como motor_endstop.ino)
void empezarAGirarE() {
  bobinasE(true);
  estrellas.moveTo((long)sentidoE * MUY_LEJOS);
  estadoE = E_GIRANDO;
  endstopArmado = false;             // se arma hasta que el endstop este suelto
  Log.printf("   Estrellas: girando hacia el lado %s\n", sentidoE > 0 ? "POSITIVO" : "NEGATIVO");
}

// ARRANQUE (como antes): las estrellas buscan el microswitch; al encontrarlo esa
// es la posicion 0 y empieza la calibracion (o la sesion, si ya estaba calibrado).
void iniciarArranque() {
  cerrarActivacion();
  modo = ARRANQUE;
  frenando = false;
  pistaPendiente = 0;
  estadoE = E_BUSCANDO;
  pasosHoming = 0;
  sentidoE = SENTIDO_INICIAL;
  bobinasE(true);
  estrellas.setMaxSpeed(V_HOMING_E);
  estrellas.setSpeed(SENTIDO_INICIAL * V_HOMING_E);
  velE = 0;
  dfVolumenEn = millis() + 2000;     // el DFPlayer pudo reiniciarse (paro): reenvia el volumen
  tMusicaLista = millis() + 2300;    // y despues vuelve a poner la musica
  musicaSonando = false;
  Log.println();
  Log.println(">> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).");
}

// Termina el arranque (como la V3): posicion 0 en el microswitch y las estrellas
// se ALEJAN de el (sentido contrario al de la busqueda); luego sigue su ciclo.
void terminarArranque() {
  estrellas.setCurrentPosition(0);
  sentidoE = -SENTIDO_INICIAL;
  velE = V_ESTRELLAS_LENTO;
  estrellas.setMaxSpeed(velE);
  estrellas.setAcceleration(ACELERACION);
  empezarAGirarE();                  // espera a soltar el microswitch antes de contar el siguiente
  if (calibrado) iniciarSesion(); else iniciarCalibracion();
}

// v8.1: sensibilidad elegida en la pagina (1, 2 o 3)
void aplicarSensibilidad(int n) {
  if (n < 1 || n > 3) return;
  sensibilidad = n; umbralK = SENS_K[n - 1]; margenRel = SENS_REL[n - 1];
  Log.printf(">> Sensibilidad %d (%s)\n", n, n == 1 ? "mas sensible" : n == 2 ? "normal" : "menos sensible");
  if (calibrado) { calcularUmbrales(); imprimirCalibracion(); }
}

void iniciarCalibracion() {
  cerrarActivacion();
  modo = CALIBRANDO;
  tCal = millis();
  nReposo = 0;
  nArriba = 0; iArriba = 0;                   // v8.2: el nivel de arriba se vuelve a medir
  tSubio = 0; uTrasSubir = 0;                  // v8.3
  sumaReposo = suma2Reposo = 0.0;
  pistaPendiente = 0;
  Log.println();
  Log.println(">> CALIBRACION BASAL: cabeza apoyada y estable durante 10 s (luz azul).");
  Log.println(">> Se descarta el primer segundo; no hagas contracciones voluntarias.");
}

void iniciarSesion() {
  modo = SESION;
  activo = false;
  tSobre = tBajo = 0;
  tCambioCabeza = millis();
  abajoAntes = cabezaAbajo();              // empezar la sesion no cambia la cancion
  cambiarCancion = false;
  Log.println();
  Log.println(">> SESION: cabeza abajo = estrellas rapidas, luz brillante y colores rapidos.");
}

void terminarCalibracion() {
  calN = nReposo;
  calPocos = nReposo < CAL_MIN_MUESTRAS;
  calInestable = false;
  sesionId++;                        // para la pagina: sesion nueva desde cero
  msSesion = 0;
  if (nReposo < CAL_MIN_MUESTRAS) {
    // Casi no llegaron datos: no se cambia nada y se sigue (nunca se queda trabado)
    Log.printf("!! Calibracion basal con solo %d bloques: se conservan los umbrales anteriores. Repite con c o BOOT.\n", nReposo);
  } else {
    // v8.1: mediana y MAD (robustas a un movimiento o espasmo breve); antes media y SD
    double media = medianaDe(calVals, min(nReposo, N_CAL_MAX), 0.0f, false);
    float sd = 1.4826f * medianaDe(calVals, min(nReposo, N_CAL_MAX), (float)media, true);
    reposo = (float)media;
    sdReposo = max(sd, 0.5f);
    // Calidad: solo AVISA; el atril sigue funcionando (supervisado por la fisioterapeuta)
    calInestable = media > 0 && (sd / media) > CAL_CV_MAX;
    if (calInestable) {
      Log.println("!! RMS basal inestable (variacion > 50 % de la media). Revisa electrodos;");
      Log.println("!! el atril sigue funcionando. Puedes recalibrar con c o BOOT.");
    }
  }
  calibrado = true;
  calcularUmbrales();
  imprimirCalibracion();
  iniciarSesion();
}

void entrarParo() {
  modo = PARO;
  cerrarActivacion();
  detenerEstrellasYa();
  bobinasP(false);
  pistaPendiente = 0;
  Log.println();
  Log.println("!! PARO DE EMERGENCIA. Al soltarlo, presiona BOOT o escribe r para reanudar.");
}

void reanudarTrasParo() {
  if (paroActivo) { Log.println("El paro sigue presionado: giralo para soltarlo."); return; }
  iniciarArranque();                 // vuelve a buscar el tope antes de moverse (y reenvia el volumen)
}

// ======================================================================
//  Entradas
// ======================================================================
void procesarRms(int mv) {
  unsigned long ahora = millis();
  rmsUltimo = mv;
  ventana[ventanaI] = mv;                          // mediana movil de 0.5 s
  ventanaI = (ventanaI + 1) % N_MEDIANA;
  if (ventanaN < N_MEDIANA) ventanaN++;
  float tmp[N_MEDIANA] = {0};
  for (int k = 0; k < ventanaN; k++) tmp[k] = ventana[k];
  for (int a = 1; a < ventanaN; a++) {             // ordenar (insercion)
    float x = tmp[a]; int b = a - 1;
    while (b >= 0 && tmp[b] > x) { tmp[b + 1] = tmp[b]; b--; }
    tmp[b + 1] = x;
  }
  rmsSuave = tmp[ventanaN / 2];
  hayDatos = true;
  tUltimoDato = ahora;

  if (modo == CALIBRANDO) {
    // Se calibra sobre la misma mediana que se compara despues (el primer segundo no cuenta)
    bool usado = false;
    if (ahora - tCal >= CAL_DESCARTAR_MS && ventanaN == N_MEDIANA) {
      sumaReposo += rmsSuave;
      suma2Reposo += (double)rmsSuave * rmsSuave;
      if (nReposo < N_CAL_MAX) calVals[nReposo] = rmsSuave;   // v8.1
      nReposo++;
      usado = true;
    }
    guardarBloque(mv, rmsSuave, usado ? 2 : 0);
    return;
  }
  if (modo != SESION) { guardarBloque(mv, rmsSuave, 0); return; }

  // Nivel relativo al umbral, NO porcentaje de contraccion voluntaria maxima.
  nivel = constrain((rmsSuave - reposo) / max(umbralOn - reposo, 1.0f), 0.0f, 1.0f);

  // Histeresis + confirmacion en el tiempo
  if (!activo) {
    float uOnAhora = umbralOnAhora();             // v8.2: sobre el nivel actual con la cabeza arriba
    uAhoraPub = uOnAhora; uSueltaPub = 0; arribaPub = nivelArribaActual();
    if (rmsSuave >= uOnAhora) {                    // v8.3: mientras reaprende usa uTrasSubir
      if (!tSobre) { tSobre = ahora; sumaRmsAct = 0; nRmsAct = 0; msSesionIni = msSesion; }
      sumaRmsAct += mv; nRmsAct++;
      if (ahora - tSobre + HOLGURA_MS >= T_CONFIRMAR) {
        activo = true;
        baseAct = nivelArribaActual(); uOnAct = uOnAhora; picoAct = rmsSuave;   // v8.2
        tInicioActivo = tSobre;
        tCambioCabeza = ahora;                             // cambia de cabeza abajo <-> arriba
        nActivaciones++;
        tBajo = 0;
      }
    } else {
      tSobre = 0;
      // v8.3: tras subir espera a que se asiente; ya aprendido, solo guarda valores claramente de arriba
      // (no los que van subiendo al empezar una caida, que inflarian el nivel)
      float bA = nivelArribaActual();
      if ((!tSubio || ahora - tSubio >= ASENTAR_MS) && (nArriba < 10 || rmsSuave < bA + 0.5f * margenSobre(bA))) guardarArriba(rmsSuave);
    }
  } else {
    if (rmsSuave > picoAct) picoAct = rmsSuave;   // v8.2
    float uSuelta = umbralSueltaAhora();
    uSueltaPub = uSuelta;
    if (rmsSuave < uSuelta) {                      // v8.2: antes rmsSuave < umbralOff
      if (!tBajo) { tBajo = ahora; sumaCola = 0; nCola = 0; }
      sumaCola += mv; nCola++;
      if (ahora - tBajo + HOLGURA_MS >= T_CONFIRMAR) {
        terminarActivacion(tBajo);
        nArriba = 0; iArriba = 0;                  // v8.2: la cabeza acaba de subir: aprende 1 s ese nivel
        tSubio = ahora; uTrasSubir = uSuelta + margenSobre(baseAct);   // v8.3
      }
    } else {
      if (tBajo) { sumaRmsAct += sumaCola; nRmsAct += nCola; tBajo = 0; }  // fue una baja corta
      sumaRmsAct += mv; nRmsAct++;
    }
  }
  guardarBloque(mv, rmsSuave, activo ? 1 : 0);
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
    Log.println("!! Llegan bytes del Arduino pero ninguna linea valida.");
    Log.println("!! Revisa que ambos esten a 9600 baud, o cambia EMG_INVERTIDO y vuelve a cargar.");
  }
}

// Prueba en vivo el siguiente orden de bobinas del lazy susan
void cambiarOrdenPlanetas() {
  planetas->disableOutputs();
  bobinasPlanetas = false;
  ordenP = (ordenP + 1) % N_ORDENES_P;
  planetas = &planetasOrden[ordenP];
  velPlanetas = 0;                   // arranca suave con el orden nuevo
  Log.printf(">> Lazy susan: combinacion %d (%s). Si ahora gira parejo, pon ORDEN_PLANETAS = %d.\n",
                ordenP, NOMBRE_ORDEN_P[ordenP], ordenP);
}

// Comandos del monitor serie y de la pagina (todos pasan por aqui, en el loop)
void ejecutarComando(char c) {
    switch (c) {
      case 'c': if (modo == SESION || modo == PAUSA) iniciarCalibracion(); break;
      case 'p':
        if (modo == SESION) { cerrarActivacion(); modo = PAUSA; Log.println(">> PAUSA (p para seguir)"); }
        else if (modo == PAUSA) {
          if (calibrado) iniciarSesion();
          else Log.println("!! Calibracion no valida. Repite con c o BOOT.");
        }
        break;
      case 'r': if (modo == PARO) reanudarTrasParo(); break;
      case 'h': if (modo != PARO) iniciarArranque(); break;      // vuelve a buscar el microswitch
      case 'x':                                                  // invierte que estado es "cabeza abajo"
        abajoEsActividad = !abajoEsActividad;
        tCambioCabeza = millis();
        abajoAntes = cabezaAbajo();
        Log.printf(">> Ahora CABEZA ABAJO = RMS %s el umbral. Si queda bien, pon ABAJO_ES_ACTIVIDAD = %s.\n",
                      abajoEsActividad ? "SOBRE" : "BAJO", abajoEsActividad ? "true" : "false");
        break;
      case '+': if (calibrado) { umbralK = min(umbralK + 0.5f, 8.0f); calcularUmbrales(); imprimirCalibracion(); } break;
      case '-': if (calibrado) { umbralK = max(umbralK - 0.5f, 1.0f); calcularUmbrales(); imprimirCalibracion(); } break;
      case 'i': imprimirCalibracion(); break;
      case 'o': cambiarOrdenPlanetas(); break;
      default: break;
    }
}

void leerComandos() {
  while (Serial.available()) ejecutarComando(Serial.read());
  char c;
  while (colaCmd && xQueueReceive(colaCmd, &c, 0) == pdTRUE) {
    if (modo == PARO) avisar(-7);                // el PARO solo se libera con el boton fisico
    else ejecutarComando(c);
  }
  PedidoMusica m;
  while (colaMusica && xQueueReceive(colaMusica, &m, 0) == pdTRUE) aplicarMusica(m);
  uint8_t vol;
  while (colaVolumen && xQueueReceive(colaVolumen, &vol, 0) == pdTRUE) aplicarVolumen(vol);
  uint8_t sn;
  while (colaSens && xQueueReceive(colaSens, &sn, 0) == pdTRUE) aplicarSensibilidad(sn);   // v8.1
}

void revisarBoot() {
  int v = digitalRead(PIN_BOOT);
  if (v != bootAnterior && millis() - bootCambio > 50) {
    bootCambio = millis();
    bootAnterior = v;
    if (v == LOW) {  // al presionar
      if (modo == PARO) reanudarTrasParo();
      else if (modo == SESION || modo == PAUSA) iniciarCalibracion();
    }
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
    Log.println(">> Paro liberado. Presiona BOOT o escribe r para reanudar.");
  }
}

// ======================================================================
//  Logica principal
// ======================================================================
void actualizarModo() {
  unsigned long ahora = millis();

  // Tiempo de sesion para la pagina (sin pausas)
  static unsigned long tPrevio = 0;
  if (modo == SESION && tPrevio) msSesion += ahora - tPrevio;
  tPrevio = ahora;

  // Avisos de la red (los pone el nucleo 0; se imprimen aqui)
  int8_t aw;
  while (colaAvisos && xQueueReceive(colaAvisos, &aw, 0) == pdTRUE) {
    switch (aw) {
      case 1:  Log.printf(">> Red WiFi \"%s\" lista. En el celular: unete a la red y abre http://%s\n", RED_SSID, WiFi.softAPIP().toString().c_str()); break;
      case -1: Log.println("!! No se pudo crear la red WiFi (la clave debe tener de 8 a 63 caracteres). El atril funciona igual."); break;
      case 2:  Log.println(">> Pagina web lista."); break;
      case -2: Log.println("!! No se pudo iniciar el servidor de la pagina. El atril funciona igual."); break;
      case 3:  Log.println(">> Arduino conectado por WiFi: la senal EMG ya sale a la pagina."); break;
      case -3: Log.println("!! Se perdio la senal WiFi del Arduino (el atril sigue con el RMS del opto)."); break;
      case 5:  Log.println(">> Se abrio la pagina en un celular o computadora."); break;
      case -5: Log.println(">> Se cerro una pagina."); break;
      case -7: Log.println("!! Orden de la pagina ignorada: el PARO esta activo (sueltalo y presiona BOOT)."); break;
      default: break;
    }
  }

  // Watchdog: avisa una vez al perder / recuperar los datos
  bool ok = datosOK();
  if (ok != datosAntes) {
    datosAntes = ok;
    if (!ok) {
      cerrarActivacion(tUltimoDato);                 // no cuenta el tiempo sin senal
      ventanaN = 0;                                  // la mediana empieza de nuevo
      Log.println("!! Sin datos del Arduino: estrellas y planetas quietos hasta que vuelvan.");
    } else {
      tCambioCabeza = ahora;             // al volver los datos, el estado empieza de nuevo
      Log.println(">> Llegan datos del Arduino.");
    }
  }

  // Calibracion basal: termina a los 10 s por reloj, lleguen o no todos los datos
  if (modo == CALIBRANDO && ahora - tCal >= CAL_MS) terminarCalibracion();

  // Musica: siempre sonando (menos en PARO). Lista de cabeza abajo / arriba y
  // aviso ADVERT cada vez que SUBE la cabeza.
  if (modo == SESION && ok) {
    bool abajo = cabezaAbajo();
    if (!abajoAntes && abajo) { tInicioAbajo = ahora; cambiarCancion = false; }   // la cabeza acaba de bajar
    if (abajoAntes && !abajo && ahora - tInicioAbajo >= T_ABAJO_MIN_MS)
      cambiarCancion = true;                             // la cabeza acaba de subir: aviso
    abajoAntes = abajo;
  }
  if (avisoSonando && ahora - tAviso >= AVISO_MAX_MS) avisoSonando = false;
  if (ahora - ultimoCmdMusica >= 300 && (long)(ahora - tMusicaLista) >= 0) {  // tiempo entre comandos
    bool quiereAbajo = (modo == SESION && ok && cabezaAbajo());
    if (modo == PARO) {
      if (musicaSonando) { dfEnviar(0x16, 0); musicaSonando = false; ultimoCmdMusica = ahora; }
      cambiarCancion = avisoSonando = pistaTerminada = false;
      reiniciarLista = false;
    } else if (!musicaSonando) {
      tocarLista(quiereAbajo, false);            // la pista actual de la lista
    } else if (modo == SESION && ok && quiereAbajo != listaAbajoSonando &&
               (ahora - tUltimoCambioCancion >= T_ENTRE_CANCIONES || cambiarCancion)) {
      // (si la cabeza SUBIO de verdad, cambia de lista de inmediato y despues el aviso)
      tUltimoCambioCancion = ahora;
      tocarLista(quiereAbajo, true);             // cambio de cabeza: siguiente pista de la otra lista
    } else if (reiniciarLista && !avisoSonando) {
      reiniciarLista = false;                    // la pagina cambio la lista que suena: empieza con la nueva
      (listaAbajoSonando ? idxAbajo : idxArriba) = -1;
      tocarLista(listaAbajoSonando, false);
    } else if (pistaTerminada) {
      tocarLista(listaAbajoSonando, true);       // termino la pista: siguiente de la misma lista
    } else if (cambiarCancion && ahora - ultimoCmdMusica >= 600) {
      cambiarCancion = false;
      dfEnviar(0x13, PISTA_AVISO);               // /ADVERT/0001.mp3 (luego continua la pista)
      avisoSonando = true; tAviso = ahora; ultimoCmdMusica = ahora;
      Log.println("   >> Cabeza arriba: aviso /ADVERT/0001.mp3");
    }
  }

  // Reenvio del volumen (el DFPlayer pierde la configuracion si se queda sin energia)
  if (dfVolumenEn && (long)(ahora - dfVolumenEn) >= 0) {
    dfVolumenEn = 0;
    dfEnviar(0x06, volumen);
  }

  // Respuestas del DFPlayer (fin de pista / fin del aviso)
  leerDFPlayer();
}

// Velocidad de las estrellas: CABEZA ABAJO acelera hasta V_ESTRELLAS_RAPIDO en
// T_BRILLO_RELAJADO. Cabeza arriba, calibrando o sin datos: lento.
float velEstrellas() {
  if (modo == SESION && datosOK() && cabezaAbajo()) {
    float k = min((float)(millis() - tCambioCabeza) / T_BRILLO_RELAJADO, 1.0f);
    float v = V_ESTRELLAS_LENTO + (V_ESTRELLAS_RAPIDO - V_ESTRELLAS_LENTO) * k;
    return 50.0f * (int)(v / 50.0f);   // en escalones de 50 pasos/s
  }
  return V_ESTRELLAS_LENTO;
}

// Gira -> pisa el endstop -> regresa a 0 -> gira al otro lado -> pisa el endstop
// -> regresa a 0 -> ...  (igual que motor_endstop.ino). Nunca se queda detenido.
void moverEstrellas() {
  if (modo == PARO) return;

  // ---- Arranque: buscar el microswitch ----
  if (estadoE == E_BUSCANDO) {
    if (topePresionado()) {
      Log.println(">> Tope encontrado. Las estrellas empiezan a subir y bajar.");
      terminarArranque();
    } else if (estrellas.runSpeed() && ++pasosHoming > HOMING_MAX_PASOS) {
      Log.println("!! No encontre el microswitch: la posicion 0 queda donde estan ahora (revisa microswitch y SENTIDO_INICIAL).");
      terminarArranque();
    }
    return;
  }

  if (modo == PAUSA) {                       // en pausa frenan suave y esperan
    if (!frenando) { frenarEstrellas(); frenando = true; }
  } else {
    if (frenando) {                          // al seguir, retoman lo que hacian
      frenando = false;
      estrellas.moveTo(estadoE == E_GIRANDO ? (long)sentidoE * MUY_LEJOS : 0);
    }
    float v = velEstrellas();
    if (v != velE) { velE = v; estrellas.setMaxSpeed(v); }

    switch (estadoE) {
      case E_GIRANDO:
        // Esperar a que el endstop este suelto antes de aceptar un nuevo picado
        if (!endstopArmado) {
          if (digitalRead(PIN_TOPE) == HIGH) endstopArmado = true;
        } else if (topePresionado()) {
          // Cada vez que se presiona el microswitch: cambian de sentido de inmediato
          vueltasEndstop++;
          Log.println("   Estrellas: microswitch presionado. Cambio de sentido.");
          sentidoE = -sentidoE;
          empezarAGirarE();                  // gira hacia el otro lado (espera a que se suelte)
        }
        break;
      case E_REGRESANDO:
        if (estrellas.distanceToGo() == 0) {
          sentidoE = -sentidoE;              // ahora hacia el otro lado
          empezarAGirarE();
        }
        break;
      case E_BUSCANDO:
        break;
    }
  }

  if (estrellas.distanceToGo() != 0) { bobinasE(true); estrellas.run(); }
  else bobinasE(false);
}

// El lazy susan gira SIEMPRE desde el encendido (arranque, calibracion, sesion y
// pausa). No depende del microswitch. Solo lo detiene el PARO.
void moverPlanetas() {
  if (modo != PARO) {
    bobinasP(true);
    unsigned long ahora = millis();
    float vObj = ordenP >= 3 ? V_PLANETAS / 2.0f : (float)V_PLANETAS;   // paso completo: mitad de pasos, misma vuelta
    if (velPlanetas < vObj && ahora - tRampaP >= 20) {   // arranque suave
      tRampaP = ahora;
      velPlanetas = min(velPlanetas + ACEL_PLANETAS * 0.02f, vObj);
      planetas->setSpeed(velPlanetas);
    }
    planetas->runSpeed();
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
    case CALIBRANDO: b = 70; break;   // azul durante todo el registro basal
    case PAUSA:      r = 15; g = 15; b = 15; break;
    case PARO:       break;                                         // apagada
    case SESION:
      if (!datosOK())               { r = 40; g = 10; }             // ambar: sin senal
      else {
        // k = 0 con actividad; sube de 0 a 1 en los primeros 5 s por debajo del umbral
        float k = cabezaAbajo() ? min((float)(ahora - tCambioCabeza) / T_BRILLO_RELAJADO, 1.0f) : 0.0f;
        float periodo = PERIODO_COLOR_LENTO + (PERIODO_COLOR_RAPIDO - PERIODO_COLOR_LENTO) * k;
        faseColor += 20.0f / periodo;              // cambia de color mas rapido al relajar
        if (faseColor >= 1.0f) faseColor -= 1.0f;
        float brillo = BRILLO_MAX * (BRILLO_MIN_FRAC + (1.0f - BRILLO_MIN_FRAC) * k);   // y mas brillante
        float h = faseColor * 3.0f; int seg = (int)h; float f = h - seg;   // rueda rojo-verde-azul
        if (seg == 0)      { r = 1 - f; g = f;     b = 0; }
        else if (seg == 1) { r = 0;     g = 1 - f; b = f; }
        else               { r = f;     g = 0;     b = 1 - f; }
        r *= brillo; g *= brillo; b *= brillo;
      }
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
    Serial.printf("[SESION] RMS %4d mV, mediana %4.0f (umbral %.0f) | %s (%s umbral) | estrellas %.0f pasos/s | cancion %d%s\n",
                  rmsUltimo, rmsSuave, umbralOn, cabezaAbajo() ? "CABEZA ABAJO" : "cabeza arriba",
                  activo ? "sobre" : "bajo", velE, cancion, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else if (modo == CALIBRANDO) {
    unsigned long t = millis() - tCal;
    Serial.printf("[CALIBRANDO BASAL, quedan %.1f s] RMS %4d mV%s\n", t < CAL_MS ? (CAL_MS - t) / 1000.0f : 0.0f,
                  rmsUltimo, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else {
    Serial.printf("[%s]%s\n", NOMBRE_MODO[modo], datosOK() ? "" : " sin datos del Arduino");
  }
}

// ======================================================================
//  Red ASTRA, pagina web y WebSocket (nucleo 0). Solo leen el estado y
//  encolan pedidos: todo lo que cambia el atril lo hace el loop.
// ======================================================================
static char js[8192];               // un mensaje JSON para la pagina
static int jl = 0;
char versionPagina[24] = "?";       // v8.3: version de la pagina que se esta sirviendo

void ja(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
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

// Primer indice a mandar: lo pendiente, dentro de lo que se guarda
uint32_t desde(uint32_t pedido, uint32_t total, uint32_t guardados) {
  uint32_t ini = total > guardados ? total - guardados : 0;
  if (pedido < ini) pedido = ini;
  if (pedido > total) pedido = total;
  return pedido;
}

// Arma en js {"type":"atril","st":{...}} con el estado + lo nuevo desde sb, sa, sl (y los avanza)
void armarEstado(uint32_t &sb, uint32_t &sa, uint32_t &sl) {
  static int32_t br[N_BLQ];
  static float bm[N_BLQ];
  static uint8_t bf[N_BLQ];
  static Activacion av[N_ACT];
  static char lt[N_LOG][LARGO_LOG];

  uint32_t nb, na, nl, b0, a0, l0;
  portENTER_CRITICAL(&candado);
  nb = nBlq; na = nActLista; nl = nLog;
  portEXIT_CRITICAL(&candado);
  b0 = desde(sb, nb, N_BLQ);
  a0 = desde(sa, na, N_ACT);
  l0 = desde(sl, nl, N_LOG);
  portENTER_CRITICAL(&candado);
  for (uint32_t i = b0; i < nb; i++) { br[i - b0] = blqRms[i % N_BLQ]; bm[i - b0] = blqMed[i % N_BLQ]; bf[i - b0] = blqFlag[i % N_BLQ]; }
  for (uint32_t i = a0; i < na; i++) av[i - a0] = actLista[i % N_ACT];
  for (uint32_t i = l0; i < nl; i++) memcpy(lt[i - l0], logTxt[i % N_LOG], LARGO_LOG);
  portEXIT_CRITICAL(&candado);
  sb = nb; sa = na; sl = nl;

  unsigned long ahora = millis();
  bool act = activo;
  unsigned long tc = ahora - tCal;
  float restante = modo == CALIBRANDO ? (tc < CAL_MS ? (CAL_MS - tc) / 1000.0f : 0.0f) : 0.0f;

  jl = 0; js[0] = 0;
  ja("{\"type\":\"atril\",\"st\":");
  ja("{\"web\":\"%s\",", versionPagina);     // v8.3: la pagina compara su version con esta
  ja("\"v\":\"%s\",\"boot\":%lu,\"modo\":\"%s\",\"datos\":%d,\"rms\":%d,\"med\":%.1f",
     VERSION_ATRIL, (unsigned long)idArranque, NOMBRE_MODO[modo], datosOK() ? 1 : 0, rmsUltimo, rmsSuave);
  ja(",\"activo\":%d,\"abajo\":%d,\"abajoAct\":%d,\"nivel\":%d,\"durAct\":%.1f",
     act ? 1 : 0, cabezaAbajo() ? 1 : 0, abajoEsActividad ? 1 : 0, (int)(nivel * 100),
     act ? (ahora - tInicioActivo) / 1000.0f : 0.0f);
  ja(",\"cal\":{\"ok\":%d,\"restante\":%.1f,\"total\":%.1f,\"desc\":%.1f,\"n\":%d,\"nMin\":%d,\"reposo\":%.1f,\"sd\":%.2f,\"cv\":%.3f,\"cvMax\":%.2f",
     calibrado ? 1 : 0, restante, CAL_MS / 1000.0f, CAL_DESCARTAR_MS / 1000.0f, modo == CALIBRANDO ? nReposo : calN,
     CAL_MIN_MUESTRAS, reposo, sdReposo, reposo > 0 ? sdReposo / reposo : 0.0f, CAL_CV_MAX);
  ja(",\"k\":%.1f,\"margen\":%.1f,\"rel\":%.2f,\"sens\":%d,\"koff\":%.2f,\"uon\":%.1f,\"uoff\":%.1f,\"pocos\":%d,\"inestable\":%d}",
     umbralK, MARGEN_MIN_MV, margenRel, sensibilidad, UMBRAL_OFF_FACTOR, umbralOn, umbralOff, calPocos ? 1 : 0, calInestable ? 1 : 0);
  ja(",\"uAhora\":%.1f,\"uSuelta\":%.1f,\"arriba\":%.1f", (float)uAhoraPub, (float)uSueltaPub, (float)arribaPub);   // v8.2
  ja(",\"sid\":%d,\"nAct\":%d,\"tAct\":%.1f,\"tSes\":%.1f,\"velE\":%.0f,\"cancion\":%d,\"orden\":%d,\"malas\":%lu,\"rssi\":0",
     sesionId, nActivaciones, tiempoActivoTotal / 1000.0f, msSesion / 1000.0f, velE, cancion, ordenP,
     lineasMalas);
  ja(",\"musica\":{\"carpeta\":%d,\"abajo\":[%d,%d,%d],\"arriba\":[%d,%d,%d],\"sonando\":%d,\"lista\":\"%s\",\"pista\":%d,\"aviso\":%d,\"avisoPista\":%d,\"vol\":%d}",
     CARPETA_MUSICA, pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2],
     musicaSonando ? 1 : 0, listaAbajoSonando ? "abajo" : "arriba", cancion, avisoSonando ? 1 : 0, PISTA_AVISO, volumen);
  ja(",\"bn\":%lu,\"b\":[", (unsigned long)nb);
  for (uint32_t i = 0; i < nb - b0; i++) ja(i ? ",[%ld,%.1f,%d]" : "[%ld,%.1f,%d]", (long)br[i], bm[i], bf[i]);
  ja("],\"an\":%lu,\"acts\":[", (unsigned long)na);
  for (uint32_t i = 0; i < na - a0; i++)
    ja(i ? ",[%d,%d,%.1f,%.1f,%d]" : "[%d,%d,%.1f,%.1f,%d]", av[i].sid, av[i].id, av[i].ini, av[i].dur, av[i].rms);
  ja("],\"ln\":%lu,\"logs\":[", (unsigned long)nl);
  for (uint32_t i = 0; i < nl - l0; i++) { if (i) ja(","); jaTexto(lt[i]); }
  ja("]}}");
}

// Una orden ya validada: "C x" (tecla del monitor serie), "M abajo 1,2,3" (lista de musica) o "V 20" (volumen)
void lineaDelServidor(const char *l) {
  if (l[0] == 'C' && l[1] == ' ' && l[2] && !l[3] && strchr("cprh+-xio", l[2])) {
    char ch = l[2];
    if (colaCmd) xQueueSend(colaCmd, &ch, 0);
  } else if (l[0] == 'M' && l[1] == ' ') {
    char lista[8]; int a = 0, b = 0, c = 0;
    if (sscanf(l + 2, "%7s %d,%d,%d", lista, &a, &b, &c) == 4 && (!strcmp(lista, "abajo") || !strcmp(lista, "arriba")) &&
        a >= 1 && a <= 255 && b >= 1 && b <= 255 && c >= 1 && c <= 255) {
      PedidoMusica m = {(uint8_t)(!strcmp(lista, "abajo") ? 1 : 0), {(uint8_t)a, (uint8_t)b, (uint8_t)c}};
      if (colaMusica) xQueueSend(colaMusica, &m, 0);
    }
  } else if (l[0] == 'V' && l[1] == ' ') {
    int v = atoi(l + 2);
    if (v >= 0 && v <= 30 && l[2] >= '0' && l[2] <= '9') { uint8_t b = (uint8_t)v; if (colaVolumen) xQueueSend(colaVolumen, &b, 0); }
  }
}

// ---------- La pagina en la RAM (se lee de LittleFS una sola vez, al encender) ----------
struct ArchivoWeb { char ruta[48]; const char *tipo; const uint8_t *datos; size_t largo; bool gz; };
const int N_ARCHIVOS = 24;
ArchivoWeb archivos[N_ARCHIVOS];
int nArchivos = 0;
size_t bytesEnRam = 0;

const char *tipoDe(const char *r) {
  const char *e = strrchr(r, '.');
  if (!e) return "application/octet-stream";
  if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(e, ".css")) return "text/css; charset=utf-8";
  if (!strcmp(e, ".js")) return "text/javascript; charset=utf-8";
  if (!strcmp(e, ".json")) return "application/json";
  if (!strcmp(e, ".woff2")) return "font/woff2";
  if (!strcmp(e, ".svg")) return "image/svg+xml";
  if (!strcmp(e, ".png")) return "image/png";
  if (!strcmp(e, ".ico")) return "image/x-icon";
  if (!strcmp(e, ".txt")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void cargarCarpeta(const char *carpeta, int nivel) {
  File d = LittleFS.open(carpeta);
  if (!d || !d.isDirectory() || nivel > 3) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String ruta = f.path();
    if (f.isDirectory()) { cargarCarpeta(ruta.c_str(), nivel + 1); continue; }
    if (nArchivos >= N_ARCHIVOS) break;
    bool gz = ruta.endsWith(".gz");
    if (gz) ruta = ruta.substring(0, ruta.length() - 3);
    ArchivoWeb &a = archivos[nArchivos];
    if (ruta.length() >= sizeof(a.ruta)) continue;
    strcpy(a.ruta, ruta.c_str());
    a.gz = gz; a.tipo = tipoDe(a.ruta); a.largo = f.size();
    uint8_t *buf = (uint8_t *)heap_caps_malloc(a.largo ? a.largo : 1, MALLOC_CAP_8BIT);
    if (buf && f.read(buf, a.largo) != a.largo) { free(buf); buf = NULL; }
    a.datos = buf;
    if (a.datos) bytesEnRam += a.largo;
    nArchivos++;
  }
}

// En setup (antes de mover los motores): monta LittleFS y copia la pagina a la RAM
void prepararPagina() {
#if HAY_PAGINA_INTERNA
  // v8.3: la pagina incluida en el firmware (en la flash del programa: no ocupa RAM ni LittleFS)
  for (int i = 0; i < N_PAGINA_INTERNA && nArchivos < N_ARCHIVOS; i++) {
    ArchivoWeb &a = archivos[nArchivos++];
    strncpy(a.ruta, PAGINA_INTERNA[i].ruta, sizeof(a.ruta) - 1);
    a.ruta[sizeof(a.ruta) - 1] = 0;
    a.tipo = tipoDe(a.ruta); a.datos = PAGINA_INTERNA[i].datos; a.largo = PAGINA_INTERNA[i].largo; a.gz = PAGINA_INTERNA[i].gz;
  }
  strncpy(versionPagina, PAGINA_VERSION, sizeof(versionPagina) - 1);
  Serial.printf(">> Pagina incluida en el firmware (version %s, %d archivos): no hace falta subir LittleFS. RAM libre: %u KB\n",
                versionPagina, nArchivos, (unsigned)(ESP.getFreeHeap() / 1024));
  return;
#endif
  if (!LittleFS.begin(false)) {
    Serial.println("!! LittleFS no tiene la pagina: sube la carpeta data/ con \"Upload LittleFS\". El atril funciona igual.");
    return;
  }
  cargarCarpeta("/", 0);
  int enFlash = 0;
  for (int i = 0; i < nArchivos; i++) if (!archivos[i].datos) enFlash++;
  Serial.printf(">> Pagina: %d archivos (%u KB en RAM%s). RAM libre: %u KB\n", nArchivos, (unsigned)(bytesEnRam / 1024),
                enFlash ? ", algunos se leeran de la flash" : "", (unsigned)(ESP.getFreeHeap() / 1024));
  if (!nArchivos) Serial.println("!! LittleFS esta vacio: sube la carpeta data/ con \"Upload LittleFS\".");
  File fv = LittleFS.open("/version.txt", "r");   // v8.3: version de la pagina de LittleFS
  if (fv) {
    int n = fv.read((uint8_t *)versionPagina, sizeof(versionPagina) - 1);
    versionPagina[n > 0 ? n : 0] = 0;
    for (int i = 0; versionPagina[i]; i++) if (versionPagina[i] == '\n' || versionPagina[i] == '\r') versionPagina[i] = 0;
    fv.close();
  }
  Serial.printf(">> Pagina de LittleFS, version %s (sin pagina_web.h junto al .ino)\n", versionPagina);
}

ArchivoWeb *buscarArchivo(const char *ruta) {
  for (int i = 0; i < nArchivos; i++) if (!strcmp(archivos[i].ruta, ruta)) return &archivos[i];
  return NULL;
}

// ---------- Servidor web (tarea propia del servidor, nucleo 0) ----------
httpd_handle_t web = NULL;
volatile int numPaginas = 0;
volatile int enCola = 0;            // envios pendientes hacia las paginas

// Estado de la senal del Arduino (lo escribe la tarea de red; lo leen los demas)
volatile bool hayArduino = false;
uint32_t muestrasArduino = 0, paquetesArduino = 0, perdidosArduino = 0, bytesMalos = 0;   // solo los escribe la tarea de red
volatile float fsArduino = 0;
uint32_t arduinoDesde = 0;
char ipArduino[16] = "";
char diagArduino[256] = "";         // JSON que manda el Arduino cada segundo
portMUX_TYPE candadoDiag = portMUX_INITIALIZER_UNLOCKED;

const char *PAGINA_SIN_INTERFAZ =
  "<!doctype html><html lang=\"es\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>ASTRA</title><body style=\"font-family:sans-serif;background:#0b1030;color:#eef;padding:24px\">"
  "<h1>ASTRA</h1><p>El atril esta encendido, pero todavia no tiene la pagina.</p>"
  "<p>Sube la carpeta <b>data/</b> del atril con <b>Upload LittleFS</b> en el Arduino IDE (ver docs/ASTRA_SIN_LAPTOP.md).</p>"
  "<p><a style=\"color:#5ee6d0\" href=\"/estado.json\">Diagnostico de la conexion</a></p></body></html>";

// Diagnostico de la red (va a la pagina cada segundo y en /estado.json). Escribe en 'o'
// (cada tarea usa su propio buffer). Devuelve el largo.
int armarServidor(char *o, int cap) {
  char diag[256];
  portENTER_CRITICAL(&candadoDiag);
  memcpy(diag, diagArduino, sizeof diag);
  portEXIT_CRITICAL(&candadoDiag);
  String ip = WiFi.softAPIP().toString();
  int n = snprintf(o, cap,
    "{\"type\":\"servidor\",\"red\":\"%s\",\"ips\":[\"%s\"],\"equipos\":%d,\"paginas\":%d,\"memoria\":%u,\"memoriaMin\":%u,\"t\":%lu"
    ",\"arduino\":{\"conectado\":%s,\"ip\":\"%s\",\"desde\":%lu,\"fs\":%.0f,\"paquetes\":%lu,\"perdidos\":%lu,\"malos\":%lu,\"diag\":%s}"
    ",\"atril\":{\"conectado\":true,\"ip\":\"%s\",\"desde\":0,\"v\":\"%s\"}}",
    RED_SSID, ip.c_str(), (int)WiFi.softAPgetStationNum(), numPaginas, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned long)millis(), hayArduino ? "true" : "false", ipArduino, (unsigned long)arduinoDesde, (double)fsArduino,
    (unsigned long)paquetesArduino, (unsigned long)perdidosArduino, (unsigned long)bytesMalos, diag[0] == '{' ? diag : "null",
    ip.c_str(), VERSION_ATRIL);
  return n < 0 ? 0 : min(n, cap - 1);
}

esp_err_t servirArchivo(httpd_req_t *req) {
  char ruta[64];
  size_t n = strcspn(req->uri, "?#");
  if (n >= sizeof ruta) n = sizeof ruta - 1;
  memcpy(ruta, req->uri, n);
  ruta[n] = 0;
  if (!strcmp(ruta, "/")) strcpy(ruta, "/index.html");
  ArchivoWeb *a = buscarArchivo(ruta);
  if (!a) {
    if (!buscarArchivo("/index.html") && !strcmp(ruta, "/index.html")) {
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      return httpd_resp_send(req, PAGINA_SIN_INTERFAZ, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No existe");
  }
  httpd_resp_set_type(req, a->tipo);
  if (a->gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (a->datos) return httpd_resp_send(req, (const char *)a->datos, a->largo);
  // No cupo en la RAM: se lee de la flash en pedazos
  String nombre = String(a->ruta) + (a->gz ? ".gz" : "");
  File f = LittleFS.open(nombre, "r");
  if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo leer");
  static char trozo[1024];
  size_t k;
  while ((k = f.read((uint8_t *)trozo, sizeof trozo)) > 0)
    if (httpd_resp_send_chunk(req, trozo, k) != ESP_OK) { f.close(); return ESP_FAIL; }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t servirEstado(httpd_req_t *req) {
  static char o[768];               // solo la usa la tarea del servidor web
  int n = armarServidor(o, sizeof o);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, o, n);
}

// Lee "clave":"texto" o "clave":numero de un JSON corto (los mensajes de la pagina)
const char *valorDe(const char *j, const char *clave) {
  char patron[24];
  snprintf(patron, sizeof patron, "\"%s\":", clave);
  const char *p = strstr(j, patron);
  if (!p) return NULL;
  p += strlen(patron);
  while (*p == ' ') p++;
  return p;
}
bool textoDe(const char *j, const char *clave, char *out, size_t n) {
  const char *p = valorDe(j, clave);
  if (!p || *p != '"') return false;
  p++;
  size_t k = 0;
  while (*p && *p != '"' && k < n - 1) out[k++] = *p++;
  out[k] = 0;
  return *p == '"';
}

// Un mensaje de la pagina. Se valida aqui y se encola: lo ejecuta el loop.
void mensajeDePagina(httpd_req_t *req, const char *m) {
  static uint32_t ventana = 0;
  static int cuenta = 0;
  uint32_t ahora = millis();
  if (ahora - ventana > 1000) { ventana = ahora; cuenta = 0; }
  if (++cuenta > 20) return;        // mas de 20 mensajes por segundo: se ignoran

  char tipo[24], t[8];
  if (!textoDe(m, "type", tipo, sizeof tipo)) return;
  char linea[32];
  if (!strcmp(tipo, "ping")) {      // para medir el retraso de la conexion
    const char *v = valorDe(m, "t");
    char r[64];
    int n = snprintf(r, sizeof r, "{\"type\":\"pong\",\"t\":%.3f,\"atril\":%lu}", v ? atof(v) : 0.0, (unsigned long)millis());
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof f);
    f.final = true; f.type = HTTPD_WS_TYPE_TEXT; f.payload = (uint8_t *)r; f.len = n;
    httpd_ws_send_frame(req, &f);
  } else if (!strcmp(tipo, "cmd")) {
    // De la pagina solo: recalibrar (c) y pausa/seguir (p). Nunca con el PARO activo.
    if (!textoDe(m, "c", t, sizeof t) || t[1] || !strchr("cp", t[0]) || !t[0]) return;
    if (modo == PARO) { avisar(-7); return; }
    snprintf(linea, sizeof linea, "C %c", t[0]);
    lineaDelServidor(linea);
  } else if (!strcmp(tipo, "musica")) {
    const char *p = valorDe(m, "p");
    int a, b, c;
    if (!textoDe(m, "l", t, sizeof t) || (strcmp(t, "abajo") && strcmp(t, "arriba")) || !p ||
        sscanf(p, "[%d,%d,%d]", &a, &b, &c) != 3) return;
    snprintf(linea, sizeof linea, "M %s %d,%d,%d", t, a, b, c);
    lineaDelServidor(linea);        // revisa que cada pista este entre 1 y 255
  } else if (!strcmp(tipo, "sensibilidad")) {      // v8.1: 1, 2 o 3
    const char *v = valorDe(m, "n");
    if (!v || *v < '1' || *v > '3' || (v[1] >= '0' && v[1] <= '9')) return;
    uint8_t n = *v - '0';
    if (colaSens) xQueueSend(colaSens, &n, 0);
  } else if (!strcmp(tipo, "senal")) {             // v8.1: ver o no la senal en vivo (para comparar el ruido)
    const char *v = valorDe(m, "on");
    if (!v) return;
    senalPedida = !strncmp(v, "true", 4) || *v == '1';
  } else if (!strcmp(tipo, "volumen")) {
    const char *v = valorDe(m, "v");
    if (!v || *v < '0' || *v > '9') return;
    snprintf(linea, sizeof linea, "V %d", atoi(v));
    lineaDelServidor(linea);        // revisa que este entre 0 y 30
  }
}

// /ws: la pagina se conecta aqui (WebSocket)
esp_err_t manejarWs(httpd_req_t *req) {
  if (req->method == HTTP_GET) return ESP_OK;   // saludo: queda abierto
  httpd_ws_frame_t f;
  memset(&f, 0, sizeof f);
  esp_err_t r = httpd_ws_recv_frame(req, &f, 0);  // primero solo el largo
  if (r != ESP_OK) return r;
  if (f.len > 512) return ESP_FAIL;               // demasiado largo: se cierra esa conexion
  static uint8_t buf[513];
  f.payload = buf;
  if (f.len) {
    r = httpd_ws_recv_frame(req, &f, 512);
    if (r != ESP_OK) return r;
  }
  buf[f.len] = 0;
  if (f.type == HTTPD_WS_TYPE_TEXT) mensajeDePagina(req, (const char *)buf);
  return ESP_OK;
}

// Envios a todas las paginas: los hace la tarea del servidor (httpd_queue_work)
struct Envio { httpd_ws_type_t tipo; size_t largo; uint8_t datos[]; };

// Conexiones que ya fallaron (celular que se fue o se congelo): se cierran y no se les vuelve
// a mandar nada; si no, cada envio pendiente esperaria send_wait_timeout y trabaria la pagina
// de los demas (los motores no: van en el otro nucleo).
static int fdsMalos[8];
static int nMalos = 0;
bool esMalo(int fd) { for (int i = 0; i < nMalos; i++) if (fdsMalos[i] == fd) return true; return false; }

void enviarATodos(void *arg) {
  Envio *e = (Envio *)arg;
  size_t n = 8;
  int fds[8];
  int paginas = 0;
  if (httpd_get_client_list(web, &n, fds) == ESP_OK) {
    // Olvida las conexiones malas que ya se cerraron
    for (int i = 0; i < nMalos;) {
      bool sigue = false;
      for (size_t k = 0; k < n; k++) if (fds[k] == fdsMalos[i]) sigue = true;
      if (sigue) i++; else fdsMalos[i] = fdsMalos[--nMalos];
    }
    // Si hay mas paginas abiertas que MAX_PAGINAS se cierran las sobrantes
    int total = 0;
    for (size_t i = 0; i < n; i++) if (httpd_ws_get_fd_info(web, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) total++;
    for (size_t i = 0; i < n; i++) {
      if (httpd_ws_get_fd_info(web, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET || esMalo(fds[i])) continue;
      if (total > MAX_PAGINAS) { httpd_sess_trigger_close(web, fds[i]); total--; continue; }
      paginas++;
      httpd_ws_frame_t f;
      memset(&f, 0, sizeof f);
      f.final = true; f.type = e->tipo; f.payload = e->datos; f.len = e->largo;
      if (httpd_ws_send_frame_async(web, fds[i], &f) != ESP_OK) {
        if (nMalos < 8) fdsMalos[nMalos++] = fds[i];
        httpd_sess_trigger_close(web, fds[i]);
        paginas--;
      }
    }
  }
  if (paginas != numPaginas) { avisar(paginas > numPaginas ? 5 : -5); numPaginas = paginas; }
  free(e);
  __atomic_sub_fetch(&enCola, 1, __ATOMIC_SEQ_CST);
}

// Encola un envio. Si las paginas no alcanzan a recibir (celular lento), se descarta: la
// pagina lo nota por el numero de secuencia de la senal. Nunca se espera.
bool mandar(httpd_ws_type_t tipo, const void *d, size_t n, int maxCola) {
  if (!web || !n || enCola >= maxCola) return false;
  Envio *e = (Envio *)malloc(sizeof(Envio) + n);
  if (!e) return false;
  e->tipo = tipo; e->largo = n;
  memcpy(e->datos, d, n);
  __atomic_add_fetch(&enCola, 1, __ATOMIC_SEQ_CST);
  if (httpd_queue_work(web, enviarATodos, e) != ESP_OK) { free(e); __atomic_sub_fetch(&enCola, 1, __ATOMIC_SEQ_CST); return false; }
  return true;
}

bool iniciarServidorWeb() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.core_id = 0;                  // nucleo 0: los motores (loop) estan en el 1
  cfg.task_priority = 3;
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 8;        // + 3 internos del servidor + Arduino y UDP = 14 de los 16 sockets de lwIP
  cfg.max_uri_handlers = 4;
  cfg.lru_purge_enable = true;      // si se llena, cierra la conexion mas vieja
  cfg.recv_wait_timeout = 5;
  cfg.send_wait_timeout = 2;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&web, &cfg) != ESP_OK) { web = NULL; return false; }
  httpd_uri_t ws = {};
  ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = manejarWs; ws.is_websocket = true;
  httpd_uri_t estado = {};
  estado.uri = "/estado.json"; estado.method = HTTP_GET; estado.handler = servirEstado;
  httpd_uri_t todo = {};
  todo.uri = "/*"; todo.method = HTTP_GET; todo.handler = servirArchivo;
  httpd_register_uri_handler(web, &ws);
  httpd_register_uri_handler(web, &estado);
  httpd_register_uri_handler(web, &todo);
  return true;
}

// ---------- Senal del Arduino: paquetes 0xAA 0x55 (datos) y 0xAA 0x57 (diagnostico) ----------
static uint8_t pend[1024];          // bytes recibidos sin procesar
static int npend = 0;
static uint8_t emgOut[1400];        // paquetes validos para la pagina
static int nEmgOut = 0;
static int ultimoSeq = -1;

void procesarArduino() {
  int i = 0;
  while (i + 4 <= npend) {
    if (pend[i] != 0xAA) { i++; bytesMalos++; continue; }
    if (pend[i + 1] == 0x55) {
      int n = pend[i + 3], largo = 4 + 2 * n + 1;
      if (n == 0 || n > 64) { i++; bytesMalos++; continue; }
      if (i + largo > npend) break;
      uint8_t chk = 0;
      for (int k = i + 2; k < i + largo - 1; k++) chk ^= pend[k];
      if (chk != pend[i + largo - 1]) { i++; bytesMalos++; continue; }
      int seq = pend[i + 2];
      if (ultimoSeq >= 0) perdidosArduino += (uint8_t)(seq - ultimoSeq - 1);
      ultimoSeq = seq;
      muestrasArduino += n; paquetesArduino++;
      if (nEmgOut + largo > (int)sizeof emgOut) nEmgOut = 0;   // no deberia pasar: se manda cada 40 ms
      memcpy(emgOut + nEmgOut, pend + i, largo);
      nEmgOut += largo;
      i += largo;
    } else if (pend[i + 1] == 0x57) {
      int n = pend[i + 2], largo = 3 + n + 1;
      if (i + largo > npend) break;
      uint8_t chk = 0;
      for (int k = 0; k < n; k++) chk ^= pend[i + 3 + k];
      if (chk == pend[i + largo - 1] && n < (int)sizeof diagArduino && pend[i + 3] == '{') {
        bool limpio = true;
        for (int k = 0; k < n; k++) if (pend[i + 3 + k] < 0x20 || pend[i + 3 + k] > 0x7E) limpio = false;
        if (limpio) {
          portENTER_CRITICAL(&candadoDiag);
          memcpy(diagArduino, pend + i + 3, n);
          diagArduino[n] = 0;
          portEXIT_CRITICAL(&candadoDiag);
        }
      }
      i += largo;
    } else { i++; bytesMalos++; }
  }
  if (i) { memmove(pend, pend + i, npend - i); npend -= i; }
  if (npend >= (int)sizeof pend) npend = 0;    // basura: se descarta
}

void tareaRed(void *) {
  // 1) La red ASTRA (el Arduino y los celulares se unen a ella)
  WiFi.mode(WIFI_AP);
  bool redOk = strlen(RED_CLAVE) >= 8 && strlen(RED_CLAVE) <= 63 &&
               WiFi.softAP(RED_SSID, RED_CLAVE, CANAL_WIFI, 0, MAX_EQUIPOS_RED);
  if (!redOk) { avisar(-1); vTaskDelete(NULL); return; }
  WiFi.setSleep(false);             // respuesta rapida
  avisar(1);
  // 2) La pagina y el WebSocket
  avisar(iniciarServidorWeb() ? 2 : -2);
  // 3) La senal del Arduino
  WiFiServer srvArduino(PUERTO_ARDUINO);
  srvArduino.begin();
  srvArduino.setNoDelay(true);
  WiFiClient arduino;
  WiFiUDP udp;
  uint32_t tDato = 0, tEmg = 0, tEstado = 0, tSeg = 0, muestrasAntes = 0;
  uint32_t sb = 0, sa = 0, sl = 0;
  int senalEnviada = -1;                          // v8.1: ultima orden mandada al Arduino (1 = S, 0 = N)
  for (;;) {
    uint32_t ahora = millis();
    WiFiClient nuevo = srvArduino.accept();
    if (nuevo) {                    // una sola placa: la nueva conexion reemplaza a la vieja
      arduino.stop();
      arduino = nuevo;
      arduino.setNoDelay(true);
      npend = 0; ultimoSeq = -1; tDato = ahora;
      senalEnviada = -1;                          // v8.1: se le dice de nuevo si mandar la senal
      strncpy(ipArduino, arduino.remoteIP().toString().c_str(), sizeof ipArduino - 1);
      arduinoDesde = ahora;
      if (!hayArduino) { hayArduino = true; avisar(3); }
    }
    if (hayArduino) {
      int k = arduino.available();
      if (k > 0) {
        k = arduino.read(pend + npend, min(k, (int)sizeof(pend) - npend));
        if (k > 0) { npend += k; tDato = ahora; procesarArduino(); }
      }
      if (!arduino.connected() || ahora - tDato > 5000) {   // se fue (o conexion "fantasma")
        arduino.stop();
        hayArduino = false; fsArduino = 0;
        avisar(-3);
      }
    }
    // v8.1: el Arduino solo transmite la senal si alguien la esta mirando (menos ruido por WiFi)
    if (hayArduino) {
      int quiero = senalPedida && numPaginas > 0 ? 1 : 0;
      if (quiero != senalEnviada) {
        uint8_t o = quiero ? 'S' : 'N';
        if (arduino.write(&o, 1) == 1) senalEnviada = quiero;
        if (quiero) ultimoSeq = -1;               // la senal vuelve a empezar: ese salto de numeracion no es perdida
      }
    }
    if (nEmgOut && (ahora - tEmg >= 40 || nEmgOut > 1000)) {   // senal: tandas de ~40 ms
      tEmg = ahora;
      mandar(HTTPD_WS_TYPE_BINARY, emgOut, nEmgOut, 8);
      nEmgOut = 0;
    }
    if (ahora - tEstado >= 200) {   // estado del atril: 5 veces por segundo
      tEstado = ahora;
      if (numPaginas > 0 || enCola == 0) {
        armarEstado(sb, sa, sl);
        mandar(HTTPD_WS_TYPE_TEXT, js, jl, 12);
      }
    }
    if (ahora - tSeg >= 1000) {     // cada segundo: diagnostico + aviso UDP
      uint32_t m = muestrasArduino;
      fsArduino = hayArduino && tSeg ? (m - muestrasAntes) * 1000.0f / (ahora - tSeg) : 0;
      muestrasAntes = m;
      tSeg = ahora;
      static char o[768];
      mandar(HTTPD_WS_TYPE_TEXT, o, armarServidor(o, sizeof o), 12);
      if (udp.beginPacket(IPAddress(192, 168, 4, 255), PUERTO_AVISO)) {
        udp.print("ASTRA-SERVIDOR 9001 0\n");
        udp.endPacket();
      }
    }
    vTaskDelay(2);
  }
}

// ======================================================================
void setup() {
  // Bobinas apagadas desde el primer instante
  estrellas.enableOutputs(); estrellas.disableOutputs();
  for (int k = 0; k < N_ORDENES_P; k++) { planetasOrden[k].enableOutputs(); planetasOrden[k].disableOutputs(); }

  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("=== ASTRA - atril v8.3 (sonido + red ASTRA + pagina en 192.168.4.1) ===");

  analogWrite(PIN_ROJO, 0);
  analogWrite(PIN_VERDE, 0);
  analogWrite(PIN_AZUL, 0);
  pinMode(PIN_TOPE, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_PARO, INPUT);

  estrellas.setMaxSpeed(V_ESTRELLAS_LENTO);
  estrellas.setAcceleration(ACELERACION);
  for (int k = 0; k < N_ORDENES_P; k++) planetasOrden[k].setMaxSpeed(V_PLANETAS * 2);

  Serial2.begin(9600, SERIAL_8N1, EMG_RX, EMG_TX, EMG_INVERTIDO);   // enlace con el Arduino

  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque

  cargarListas();                                     // las listas que eligio la pagina
  Serial.printf(">> Musica: cabeza ABAJO %03d %03d %03d | cabeza ARRIBA %03d %03d %03d\n",
                pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2]);
  idArranque = (uint32_t)random(1, 2000000000);
  colaCmd = xQueueCreate(16, sizeof(char));
  colaMusica = xQueueCreate(4, sizeof(PedidoMusica));
  colaVolumen = xQueueCreate(4, sizeof(uint8_t));
  colaAvisos = xQueueCreate(16, sizeof(int8_t));
  colaSens = xQueueCreate(4, sizeof(uint8_t));      // v8.1
  if (USAR_WIFI) {
    prepararPagina();                                 // LittleFS -> RAM, antes de mover los motores
    xTaskCreatePinnedToCore(tareaRed, "red", 8192, NULL, 1, NULL, 0);  // nucleo 0
  }

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