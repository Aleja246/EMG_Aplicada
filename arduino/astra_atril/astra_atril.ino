// =====================================================================
//  ASTRA - Firmware del atril (ESP32) v2
//  La senal sEMG controla los actuadores.
// ---------------------------------------------------------------------
//  Entrada: el Arduino UNO R4 (diagnostico_emg_enlace.ino) manda por el
//  opto, cada 100 ms, una linea con el RMS en milivoltios ("640\n").
//
//  Que hace:
//   1. ARRANQUE: sube las estrellas hasta el microswitch (posicion 0) y
//      las baja al fondo.
//   2. CALIBRANDO: 5 s en REPOSO (luz azul) y 6 s CONTRAYENDO (luz verde).
//      U_on = max(R + 3 SD, R + 20 % (C - R)),  U_off = R + (U_on - R) / 2
//   3. SESION:
//      - Activacion: RMS suave sobre U_on por 300 ms. Termina: bajo U_off
//        por 300 ms.
//      - Estrellas: suben durante la activacion (250 + 450 x nivel pasos/s).
//        Al terminar frenan, esperan 1.5 s y bajan despacio (300 pasos/s).
//      - Planetas: giran lento y continuo, sin importar el esfuerzo.
//      - Luz: azul tenue en reposo, dorada que crece con el esfuerzo,
//        verde azulado arriba. Cambios lentos (>= 1 s, sin parpadeo).
//      - Sonido: pista 0001.mp3 al sostener 1 s; 0002.mp3 al llegar arriba.
//   4. Seguridad: si no llega una linea valida en 500 ms, todo se detiene.
//      Paro de emergencia por IO34 (activar USAR_PARO cuando este cableado).
//
//  Comandos (monitor serie a 115200, "Sin ajuste de linea"):
//   c = recalibrar   p = pausa / seguir   r = reanudar tras el paro
//   h = volver a buscar el tope   + / - = umbral   i = info de calibracion
//  Boton BOOT de la placa: recalibrar (o reanudar tras el paro).
//
//  Placa: "ESP32 Dev Module". Bibliotecas: AccelStepper, DFRobotDFPlayerMini.
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
// =====================================================================

#include <AccelStepper.h>
#include <DFRobotDFPlayerMini.h>

// ------------------------- Pines (esquematico) -------------------------
// Motor de giro (J4):      IN1..IN4 = IO23, IO22, IO21, IO19
// Motor de estrellas (J3): IN1..IN4 = IO18, IO17, IO16, IO4
// El 28BYJ-48 necesita el orden IN1, IN3, IN2, IN4 en AccelStepper.
// El "false" final: los pines se configuran en setup() con las bobinas apagadas.
AccelStepper planetas(AccelStepper::HALF4WIRE, 23, 21, 22, 19, false);
AccelStepper estrellas(AccelStepper::HALF4WIRE, 18, 16, 17, 4, false);

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
const int  V_SUBIR_MIN   = 250;    // pasos/s con contraccion debil
const int  V_SUBIR_MAX   = 700;    // pasos/s con contraccion fuerte
const int  V_BAJAR       = 300;    // pasos/s al bajar
const int  ACELERACION   = 800;    // pasos/s^2
const int  V_PLANETAS    = 120;    // pasos/s (~1.8 rpm)
const unsigned long ESPERA_BAJAR = 1500;  // ms relajado antes de que bajen

// ------------------------- Ajustes de deteccion -----------------------
const float UMBRAL_PCT_INICIAL = 0.20;  // umbral = reposo + 20 % del rango
const unsigned long T_CONFIRMAR = 300;  // ms sobre/bajo el umbral para cambiar de estado
const unsigned long HOLGURA_MS  = 50;   // tolera que un bloque llegue unos ms antes (300 ms = 4 bloques seguidos)
const unsigned long T_PREMIO    = 1000; // ms sostenidos para el sonido de activacion
const unsigned long WATCHDOG_MS = 500;  // sin datos por mas de esto -> todo se detiene
const float SUAVIZADO           = 0.5;  // 0..1 (mas alto = responde mas rapido)
const unsigned long T_REPOSO_MS   = 5000;
const unsigned long T_CONTRAE_MS  = 6000;
const unsigned long T_PREPARAR_MS = 1000;  // primer segundo de la fase activa no cuenta

// ------------------------- Luz y sonido -------------------------------
const int BRILLO_MAX = 128;        // tope del 50 %
const int PASO_LUZ   = 2;          // por cada 20 ms -> 0 a 128 en ~1.3 s
const int VOLUMEN    = 15;         // 0 a 30
const unsigned long PAUSA_SONIDO = 3000;  // ms minimos entre sonidos

// ------------------------- Enlace y paro ------------------------------
const bool USAR_PARO = false;      // pon true cuando el divisor del paro (R6, R7) este conectado
const bool EMG_INVERTIDO = false;  // pon true si el opto invierte (aviso "!! Llegan bytes ...")
const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta

// ======================================================================
enum Modo { ARRANQUE, CALIBRANDO, SESION, PAUSA, PARO };
enum ModoEstrellas { E_HOMING, E_AL_FONDO, E_CONTROL, E_QUIETAS, E_SIN_TOPE };
const char *NOMBRE_MODO[] = {"ARRANQUE", "CALIBRANDO", "SESION", "PAUSA", "PARO"};

Modo modo = ARRANQUE;
ModoEstrellas modoE = E_HOMING;
const long POS_TOPE  = 0;
const long POS_FONDO = -DIR_SUBIR * RECORRIDO;

// --- Sonido ---
DFRobotDFPlayerMini dfplayer;
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
const int MAX_MUESTRAS = 80;
float bufReposo[MAX_MUESTRAS], bufContrae[MAX_MUESTRAS];
int nReposo = 0, nContrae = 0;
bool faseContrae = false;
unsigned long tFase = 0;
bool calibrado = false;
float reposo = 100, sdReposo = 10, maximo = 1000;
float umbralPct = UMBRAL_PCT_INICIAL;
float umbralOn = 300, umbralOff = 200;

// --- Deteccion ---
bool activo = false;
unsigned long tSobre = 0, tBajo = 0, tInicioActivo = 0, tFinActivo = 0;
bool premioActivacion = false, premioCima = false;
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

void calcularUmbrales() {
  float rango = maximo - reposo;
  umbralOn = max(reposo + 3.0f * sdReposo, reposo + umbralPct * rango);
  umbralOff = reposo + 0.5f * (umbralOn - reposo);
}

void imprimirCalibracion() {
  Serial.println();
  Serial.println("---- Calibracion ----");
  Serial.printf("Reposo: %.0f mV (SD %.1f)\n", reposo, sdReposo);
  Serial.printf("Contraccion de referencia: %.0f mV\n", maximo);
  Serial.printf("Umbral de activacion: %.0f mV (%.0f %% del rango)\n", umbralOn, umbralPct * 100);
  Serial.printf("Umbral de relajacion: %.0f mV\n", umbralOff);
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
    tFinActivo = millis();
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
  estrellas.setMaxSpeed(V_HOMING > V_SUBIR_MAX ? V_HOMING : V_SUBIR_MAX);  // si no, setSpeed lo recorta
  estrellas.setSpeed(DIR_SUBIR * V_HOMING);
  dfVolumenEn = millis() + 2000;     // el DFPlayer pudo reiniciarse (paro): reenvia el volumen
  Serial.println();
  Serial.println(">> Buscando el tope de las estrellas (presiona el microswitch si no hay mecanismo).");
}

void iniciarCalibracion() {
  cerrarActivacion();
  modo = CALIBRANDO;
  faseContrae = false;
  nReposo = nContrae = 0;
  tFase = millis();
  pistaPendiente = 0;
  Serial.println();
  Serial.println(">> CALIBRACION 1/2: REPOSO. Quedate relajado 5 s (luz azul).");
}

void iniciarSesion() {
  modo = SESION;
  if (modoE != E_SIN_TOPE) modoE = E_CONTROL;
  subiendo = bajando = frenando = false;
  activo = false;
  tSobre = tBajo = 0;
  tFinActivo = millis();           // tras una pausa esperan 1.5 s antes de bajar
  rmsSuave = reposo;               // no arrastra la contraccion de la calibracion
  planetas.setSpeed(V_PLANETAS);
  Serial.println();
  Serial.println(">> SESION: contrae para subir las estrellas.");
}

void terminarCalibracion() {
  // Reposo: media y desviacion estandar
  float s = 0, s2 = 0;
  for (int i = 0; i < nReposo; i++) { s += bufReposo[i]; s2 += bufReposo[i] * bufReposo[i]; }
  float media = s / nReposo;
  float var = s2 / nReposo - media * media;
  float sd = sqrtf(max(var, 0.0f));

  // Contraccion: percentil 80 (aguanta que no se contraiga todo el tiempo)
  for (int i = 1; i < nContrae; i++) {          // ordenamiento por insercion
    float v = bufContrae[i]; int j = i - 1;
    while (j >= 0 && bufContrae[j] > v) { bufContrae[j + 1] = bufContrae[j]; j--; }
    bufContrae[j + 1] = v;
  }
  float p80 = bufContrae[(int)(0.8f * (nContrae - 1))];

  reposo = media;
  sdReposo = sd;
  if (p80 - media < max(3.0f * sd, 50.0f)) {
    maximo = media + 500;
    Serial.println();
    Serial.println("!! La contraccion no se distinguio del reposo. Uso valores por defecto.");
    Serial.println("!! Revisa electrodos y repite con BOOT o escribiendo c.");
  } else {
    maximo = p80;
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
    if (!faseContrae) {
      if (nReposo < MAX_MUESTRAS) bufReposo[nReposo++] = mv;
    } else if (ahora - tFase >= T_PREPARAR_MS) {
      if (nContrae < MAX_MUESTRAS) bufContrae[nContrae++] = mv;
    }
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
        premioActivacion = false;
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

  if (activo) estrellas.setMaxSpeed(V_SUBIR_MIN + (V_SUBIR_MAX - V_SUBIR_MIN) * nivel);
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
      case 'c': if (modo == SESION || modo == PAUSA) iniciarCalibracion(); break;
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
      if (!activo) tFinActivo = ahora;   // al volver, 1.5 s para reconfirmar antes de bajar
      Serial.println(">> Llegan datos del Arduino.");
    }
  }

  if (modo == CALIBRANDO) {
    if (!ok) {   // sin datos: la fase actual empieza de nuevo cuando vuelvan
      tFase = ahora;
      if (faseContrae) nContrae = 0; else nReposo = 0;
    } else if (!faseContrae && ahora - tFase >= T_REPOSO_MS && nReposo >= 10) {
      faseContrae = true;
      tFase = ahora;
      Serial.println(">> CALIBRACION 2/2: CONTRAE con fuerza y sostenla 6 s (luz verde).");
    } else if (faseContrae && ahora - tFase >= T_CONTRAE_MS && nContrae >= 10) {
      terminarCalibracion();
    }
  }

  // Sonido de activacion sostenida
  if (modo == SESION && activo && ok && !premioActivacion && ahora - tInicioActivo >= T_PREMIO) {
    premioActivacion = true;
    sonar(1);
  }

  // Pista pendiente: suena al cumplirse la pausa, si su motivo sigue vigente
  if (pistaPendiente) {
    bool vigente = (modo == SESION) && ok && activo && (pistaPendiente == 1 || premioCima);
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

void moverEstrellas() {
  unsigned long ahora = millis();

  switch (modoE) {
    case E_HOMING:
      if (modo == PARO) return;
      if (topePresionado()) {
        fijarTope();
        Serial.println(">> Tope encontrado. Bajando las estrellas al fondo.");
        modoE = E_AL_FONDO;
        bobinasE(true);
        estrellas.setMaxSpeed(V_SUBIR_MAX);
        estrellas.setAcceleration(ACELERACION);
        estrellas.moveTo(POS_FONDO);
        if (calibrado) iniciarSesion(); else iniciarCalibracion();
      } else if (estrellas.runSpeed()) {
        if (++pasosHoming > RECORRIDO * 13 / 10) {
          bobinasE(false);
          modoE = E_SIN_TOPE;
          Serial.println("!! No encontre el tope. Las estrellas quedan desactivadas (revisa microswitch y DIR_SUBIR).");
          if (calibrado) iniciarSesion(); else iniciarCalibracion();
        }
      }
      return;

    case E_AL_FONDO:
      if (modo == PARO) return;
      if (estrellas.distanceToGo() == 0) { bobinasE(false); modoE = E_QUIETAS; }
      else estrellas.run();
      return;

    case E_SIN_TOPE:
    case E_QUIETAS:
      if (modo == SESION && modoE == E_QUIETAS) modoE = E_CONTROL;
      return;

    case E_CONTROL:
      break;
  }

  // ---- E_CONTROL ----
  if (modo == PARO) return;
  bool puedeMover = (modo == SESION) && datosOK();

  if (!puedeMover) {
    // Pausa, calibracion o sin datos: frenan suave (~0.4 s) y se quedan quietas
    if (!frenando) { frenarEstrellas(); frenando = true; }
    subiendo = bajando = false;
  } else {
    frenando = false;
    if (activo) {
      if (!subiendo) {
        bobinasE(true);
        estrellas.setAcceleration(ACELERACION);
        estrellas.moveTo(POS_TOPE);
        subiendo = true; bajando = false;
      }
    } else if (ahora - tFinActivo >= ESPERA_BAJAR) {
      if (!bajando) {
        bobinasE(true);
        estrellas.setMaxSpeed(V_BAJAR);
        estrellas.setAcceleration(ACELERACION);
        estrellas.moveTo(POS_FONDO);
        bajando = true; subiendo = false;
      }
    } else if (subiendo) {
      frenarEstrellas();             // frena suave y espera 1.5 s
      subiendo = false;
    }
  }

  // Microswitch: tope real. Si se presiona yendo hacia arriba (subiendo o
  // frenando por inercia), la posicion vuelve a 0 y se detienen.
  if ((subiendo || estrellas.speed() * DIR_SUBIR > 0) && topePresionado()) {
    fijarTope();
  }

  if (estrellas.distanceToGo() != 0) { bobinasE(true); estrellas.run(); }
  else bobinasE(false);

  // Premio al llegar arriba
  float a = altura();
  if (!premioCima && activo && a >= 0.99f && modo == SESION && datosOK()) {
    premioCima = true;
    sonar(2);
    Serial.println("   *** Estrellas arriba ***");
  } else if (premioCima && a < 0.80f) {
    premioCima = false;
  }
}

void moverPlanetas() {
  if (modo == SESION && datosOK()) {
    bobinasP(true);
    planetas.runSpeed();
  } else {
    bobinasP(false);
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
      else if (premioCima && activo){ g = 110; b = 50; }            // verde azulado: arriba
      else if (activo)              { r = 40 + 88 * nivel; g = 15 + 45 * nivel; }  // dorado
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
    Serial.printf("[SESION] RMS %4d mV | nivel %3d %% | %s | estrellas %3d %% | activaciones %d%s\n",
                  rmsUltimo, (int)(nivel * 100), activo ? "ACTIVO" : "reposo",
                  (int)(altura() * 100), nActivaciones, datosOK() ? "" : " | SIN DATOS DEL ARDUINO");
  } else if (modo == CALIBRANDO) {
    Serial.printf("[CALIBRANDO %s] RMS %4d mV%s\n", faseContrae ? "contraccion" : "reposo",
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
  Serial.println("=== ASTRA - atril v2 ===");

  analogWrite(PIN_ROJO, 0);
  analogWrite(PIN_VERDE, 0);
  analogWrite(PIN_AZUL, 0);
  pinMode(PIN_TOPE, INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_PARO, INPUT);

  estrellas.setMaxSpeed(V_SUBIR_MAX);
  estrellas.setAcceleration(ACELERACION);
  planetas.setMaxSpeed(V_PLANETAS * 2);

  Serial2.begin(9600, SERIAL_8N1, EMG_RX, EMG_TX, EMG_INVERTIDO);   // enlace con el Arduino

  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  delay(1500);
  if (dfplayer.begin(Serial1, true, true)) Serial.println("DFPlayer listo.");
  else Serial.println("DFPlayer no contesto al iniciar. Sigo; si esta conectado y con energia, sonara.");
  dfEnviar(0x06, VOLUMEN);

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
