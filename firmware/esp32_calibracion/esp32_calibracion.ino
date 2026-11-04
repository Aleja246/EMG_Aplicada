
// =====================================================================
//  ASTRA - Firmware del atril (ESP32) v2.3
//  La senal sEMG controla los actuadores.
// ---------------------------------------------------------------------
//  Entrada: el Arduino UNO R4 (diagnostico_emg_enlace.ino) manda por el
//  opto, cada 100 ms, una linea con el RMS en milivoltios ("640\n").
//
//  Que hace:
//   1. ARRANQUE: sube las estrellas hasta el microswitch (posicion 0).
//      El lazy susan empieza a girar desde el encendido.
//   2. CALIBRANDO (segun "Calibracion en la pagina sEMG cervical", 60 s):
//      REPOSO 10 s -> PASIVA 10 s -> [ARRIBA 4 s + ABAJO 4 s] x 5
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
//
//  Comandos (monitor serie a 115200, "Sin ajuste de linea"):
//   c = recalibrar   p = pausa / seguir   r = reanudar tras el paro
//   h = volver a buscar el tope   + / - = umbral   i = info de calibracion
//  Boton BOOT de la placa: recalibrar (o reanudar tras el paro).
//
//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca).
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
enum FaseCal { CAL_REPOSO, CAL_PASIVA, CAL_ARRIBA, CAL_ABAJO, CAL_FALLIDA };
FaseCal faseCal = CAL_REPOSO;
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

// Estado que se premia: relajado (PREMIO_AL_RELAJAR) o contrayendo (original)
bool premiado() { return PREMIO_AL_RELAJAR ? !activo : activo; }


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
  Serial.println();
  Serial.println("---- Calibracion ----");
  Serial.printf("Reposo (envolvente): %.1f mV\n", reposo);
  Serial.printf("Ruido en reposo (SD): %.1f mV\n", sdReposo);
  Serial.printf("Pasiva: media %.1f mV, pico %.1f mV\n", pasivaMedia, pasivaPico);
  Serial.printf("Activa: RMS de referencia %.1f mV (envolvente %.1f mV, %d repeticiones)\n",
                activeRms, activeEnv, nRepsOK);
  Serial.printf("Relacion senal/reposo (SNR): %.1f\n", snr);
  Serial.printf("Artefacto pasivo / activa: %.0f %%\n", artefactoPct);
  Serial.printf("Umbral ON: %.1f mV (mayor de media + %.0f SD = %.1f, y %.0f %% activa = %.1f)\n",
                umbralOn, UMBRAL_K_SD, reposo + UMBRAL_K_SD * sdReposo,
                umbralPct * 100, umbralPct * activeEnv);
  Serial.printf("Umbral OFF: %.1f mV (%.1f x umbral ON)\n", umbralOff, UMBRAL_OFF_FACTOR);
  Serial.printf("Activaciones: %d | tiempo activo total: %.1f s | lineas danadas: %lu\n",
                nActivaciones, tiempoActivoTotal / 1000.0f, lineasMalas);
  Serial.println("---------------------");
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
    Serial.printf("   Activacion #%d: %.1f s, RMS medio %.0f mV (%d %% de la referencia)\n",
                  nActivaciones, dur / 1000.0f, rmsMedio, pct);
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
  Serial.println();
  Serial.println(">> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).");
}

void iniciarCalibracion() {
  cerrarActivacion();
  modo = CALIBRANDO;
  faseCal = CAL_REPOSO;
  faseContrae = false;
  bloquesFase = 0; repCal = 0;
  sumaR = suma2R = 0; nR = 0;
  sumaP = 0; nP = 0; picoP = 0;
  sumaA = suma2A = 0; nA = 0;
  sumaRepRms = sumaRepEnv = 0; nRepsOK = 0;
  pistaPendiente = 0;
  Serial.println();
  Serial.println(">> CALIBRACION (60 s): REPOSO 10 s -> PASIVA 10 s -> 5 x (ARRIBA 4 s + ABAJO 4 s)");
  Serial.println(">> FASE 1 - REPOSO (10 s): cabeza apoyada y relajada. No mover al nino.");
}

void iniciarSesion() {
  modo = SESION;
  bajando = false;                 // el vaiven sigue donde iba
  activo = false;
  tSobre = tBajo = 0;
  tInicioPremio = tFinPremio = millis();   // tras una pausa esperan 1.5 s antes de bajar
  premio1 = false;
  rmsSuave = reposo;               // no arrastra la contraccion de la calibracion
  Serial.println();
  Serial.println(PREMIO_AL_RELAJAR ? ">> SESION: relaja para subir las estrellas."
                                   : ">> SESION: contrae para subir las estrellas.");
}

void terminarCalibracion() {
  // Fase 1: reposo
  reposo   = nR ? sumaR / nR : 0;
  float var = nR ? suma2R / nR - (double)reposo * reposo : 0;
  sdReposo = sqrtf(max(var, 0.0f));
  restRms  = nR ? sqrt(suma2R / nR) : 0;
  // Fase 2: pasiva
  pasivaMedia = nP ? sumaP / nP : 0;
  pasivaPico  = picoP;
  // Fase 3: promedio de las repeticiones
  activeRms = nRepsOK ? sumaRepRms / nRepsOK : 0;
  activeEnv = nRepsOK ? sumaRepEnv / nRepsOK : 0;
  snr = restRms > 0 ? activeRms / restRms : 0;
  artefactoPct = activeEnv > 0 ? 100.0f * pasivaPico / activeEnv : 0;
  maximo = activeEnv;                 // el 100 % del esfuerzo de ese nino hoy

  calcularUmbrales();
  imprimirCalibracion();

  // Advertencias automaticas
  if (activeRms <= 0) {
    Serial.println("!! No se detecto contraccion activa. Revisa electrodos y repite (c o BOOT).");
    Serial.println("!! La terapia no puede comenzar sin una calibracion valida.");
    faseCal = CAL_FALLIDA;
    faseContrae = false;
    return;                           // no se inicia la sesion
  }
  if (snr < SNR_MINIMA)
    Serial.println("!! SNR < 3: la contraccion apenas se distingue del reposo (mal contacto o electrodo mal ubicado).");
  if (artefactoPct > ARTEFACTO_MAX_PCT)
    Serial.println("!! Artefacto > 80 %: las transiciones podrian contarse como activaciones (sube el umbral con + o mejora la fijacion de cables).");

  calibrado = true;
  iniciarSesion();
}

// Avanza la calibracion con cada bloque que llega (reloj de las muestras)
void cambiarFaseCal(FaseCal f) {
  faseCal = f;
  bloquesFase = 0;
  faseContrae = (f == CAL_ARRIBA);
  if (f == CAL_PASIVA)
    Serial.println(">> FASE 2 - PASIVA (10 s): el terapeuta mueve la cabeza de abajo hacia arriba y viceversa. El nino NO hace fuerza.");
  else if (f == CAL_ARRIBA)
    Serial.printf(">> FASE 3 - ACTIVA, repeticion %d/%d: ARRIBA! El nino levanta la cabeza y la sostiene (%.0f s).\n",
                  repCal, CAL_REPS, CAL_ARRIBA_S);
  else if (f == CAL_ABAJO)
    Serial.printf("   ABAJO: baja la cabeza y relaja (%.0f s).\n", CAL_ABAJO_S);
}

void calibrarBloque(int mv) {
  float env = mv;                     // envolvente = RMS del bloque de 100 ms
  bloquesFase++;
  switch (faseCal) {
    case CAL_REPOSO:
      if (bloquesFase > (int)lroundf(CAL_DESC_REPOSO_S * BLOQUES_POR_S)) {
        sumaR += env; suma2R += (double)env * env; nR++;
      }
      if (bloquesFase >= (int)lroundf(CAL_REPOSO_S * BLOQUES_POR_S)) cambiarFaseCal(CAL_PASIVA);
      break;

    case CAL_PASIVA:
      if (bloquesFase > (int)lroundf(CAL_DESC_PASIVA_S * BLOQUES_POR_S)) {
        sumaP += env; nP++;
        if (env > picoP) picoP = env;
      }
      if (bloquesFase >= (int)lroundf(CAL_PASIVA_S * BLOQUES_POR_S)) { repCal = 1; cambiarFaseCal(CAL_ARRIBA); }
      break;

    case CAL_ARRIBA: {
      int total = (int)lroundf(CAL_ARRIBA_S * BLOQUES_POR_S);
      int ini = (int)lroundf(CAL_DESC_INI_S * BLOQUES_POR_S);
      int fin = (int)lroundf(CAL_DESC_FIN_S * BLOQUES_POR_S);
      if (bloquesFase > ini && bloquesFase <= total - fin) {     // solo la ventana estable
        sumaA += env; suma2A += (double)env * env; nA++;
      }
      if (bloquesFase >= total) {
        if (nA > 0) {
          float rms = sqrt(suma2A / nA), media = sumaA / nA;
          sumaRepRms += rms; sumaRepEnv += media; nRepsOK++;
          Serial.printf("   Repeticion %d: RMS %.1f mV, envolvente %.1f mV\n", repCal, rms, media);
        }
        sumaA = suma2A = 0; nA = 0;
        cambiarFaseCal(CAL_ABAJO);
      }
      break;
    }

    case CAL_ABAJO:
      if (bloquesFase >= (int)lroundf(CAL_ABAJO_S * BLOQUES_POR_S)) {
        if (repCal < CAL_REPS) { repCal++; cambiarFaseCal(CAL_ARRIBA); }
        else terminarCalibracion();
      }
      break;

    case CAL_FALLIDA:
      break;
  }
}

void entrarParo() {
  modo = PARO;
  cerrarActivacion();
  detenerEstrellasYa();
  bobinasP(false);
  pistaPendiente = 0;
  Serial.println();
  Serial.println("!! PARO DE EMERGENCIA. Al soltarlo, presiona BOOT o escribe r para reanudar.");
}

void reanudarTrasParo() {
  if (paroActivo) { Serial.println("El paro sigue presionado: giralo para soltarlo."); return; }
  iniciarArranque();                 // vuelve a buscar el tope antes de moverse (y reenvia el volumen)
}

// ======================================================================
//  Entradas
// ======================================================================
void procesarRms(int mv) {
  unsigned long ahora = millis();
  rmsUltimo = mv;
  rmsSuave = hayDatos ? SUAVIZADO * mv + (1 - SUAVIZADO) * rmsSuave : mv;
  hayDatos = true;
  tUltimoDato = ahora;

  if (modo == CALIBRANDO) {
    calibrarBloque(mv);
    return;
  }
  if (modo != SESION) return;

  nivel = constrain((rmsSuave - reposo) / max(maximo - reposo, 1.0f), 0.0f, 1.0f);

  // Histeresis + confirmacion en el tiempo
  if (!activo) {
    if (rmsSuave >= umbralOn) {
      if (!tSobre) { tSobre = ahora; sumaRmsAct = 0; nRmsAct = 0; }
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
    Serial.println("!! Llegan bytes del Arduino pero ninguna linea valida.");
    Serial.println("!! Revisa que ambos esten a 9600 baud, o cambia EMG_INVERTIDO y vuelve a cargar.");
  }
}

void leerComandos() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'c': if (modo == SESION || modo == PAUSA || (modo == CALIBRANDO && faseCal == CAL_FALLIDA)) iniciarCalibracion(); break;
      case 'p':
        if (modo == SESION) { cerrarActivacion(); modo = PAUSA; Serial.println(">> PAUSA (p para seguir)"); }
        else if (modo == PAUSA) iniciarSesion();
        break;
      case 'r': if (modo == PARO) reanudarTrasParo(); break;
      case 'h': if (modo != PARO) iniciarArranque(); break;
      case '+': umbralPct = min(umbralPct + 0.05f, 0.80f); calcularUmbrales(); imprimirCalibracion(); break;
      case '-': umbralPct = max(umbralPct - 0.05f, 0.05f); calcularUmbrales(); imprimirCalibracion(); break;
      case 'i': imprimirCalibracion(); break;
      default: break;
    }
  }
}

void revisarBoot() {
  int v = digitalRead(PIN_BOOT);
  if (v != bootAnterior && millis() - bootCambio > 50) {
    bootCambio = millis();
    bootAnterior = v;
    if (v == LOW) {  // al presionar
      if (modo == PARO) reanudarTrasParo();
      else if (modo == SESION || modo == PAUSA || (modo == CALIBRANDO && faseCal == CAL_FALLIDA)) iniciarCalibracion();
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
    Serial.println(">> Paro liberado. Presiona BOOT o escribe r para reanudar.");
  }
}

// ======================================================================
//  Logica principal
// ======================================================================
void actualizarModo() {
  unsigned long ahora = millis();

  // Watchdog: avisa una vez al perder / recuperar los datos
  bool ok = datosOK();
  if (ok != datosAntes) {
    datosAntes = ok;
    if (!ok) {
      cerrarActivacion(tUltimoDato);                 // no cuenta el tiempo sin senal
      Serial.println("!! Sin datos del Arduino: estrellas y planetas quietos hasta que vuelvan.");
    } else {
      if (!activo) { tInicioPremio = tFinPremio = ahora; premio1 = false; }  // 1.5 s para reconfirmar
      Serial.println(">> Llegan datos del Arduino.");
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
        Serial.println(">> USAR_TOPE = false: cuento desde el FONDO (las estrellas deben arrancar abajo).");
        empezarVaiven();
        if (calibrado) iniciarSesion(); else iniciarCalibracion();
      } else if (topePresionado()) {
        fijarTope();
        Serial.println(">> Tope encontrado. Las estrellas empiezan a subir y bajar.");
        empezarVaiven();
        if (calibrado) iniciarSesion(); else iniciarCalibracion();
      } else if (estrellas.runSpeed()) {
        if (++pasosHoming > RECORRIDO * 13 / 10) {
          bobinasE(false);
          modoE = E_SIN_TOPE;
          Serial.println("!! No encontre el tope. Las estrellas quedan desactivadas (revisa microswitch y DIR_SUBIR,");
          Serial.println("!! o pon USAR_TOPE = false y enciende con las estrellas abajo).");
          if (calibrado) iniciarSesion(); else iniciarCalibracion();
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
      Serial.println("!! El microswitch sigue presionado lejos del tope. Revisa que este en NO (no en NC).");
      Serial.println("!! Las estrellas quedan desactivadas; usa h al arreglarlo.");
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
    Serial.println("   *** Estrellas arriba ***");
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
    case CALIBRANDO: if (faseContrae) g = 90; else b = 70; break;   // verde / azul
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
    const char *nf[] = {"REPOSO", "PASIVA", "ARRIBA", "ABAJO", "FALLIDA: c o BOOT para repetir"};
    if (faseCal == CAL_ARRIBA || faseCal == CAL_ABAJO)
      Serial.printf("[CALIBRANDO %s %d/%d] RMS %4d mV%s\n", nf[faseCal], repCal, CAL_REPS,
                    rmsUltimo, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
    else
      Serial.printf("[CALIBRANDO %s] RMS %4d mV%s\n", nf[faseCal],
                    rmsUltimo, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else {
    Serial.printf("[%s]%s\n", NOMBRE_MODO[modo], datosOK() ? "" : " sin datos del Arduino");
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
  Serial.println("=== ASTRA - atril v2.3 ===");

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

