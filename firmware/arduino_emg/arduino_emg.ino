/*
  =====================================================================
   ASTRA - Arduino UNO R4 (lado del nino): diagnostico_emg_enlace.ino  v2
   Mide la senal de Nova y la resume en un numero cada 100 ms.
  ---------------------------------------------------------------------
   Lee A0 (JP3 puenteado) 1000 veces por segundo y, cada 100 ms, calcula
   tres RMS (en voltios, a la salida EMG_Out):
     Bruto   : senal sin DC, sin filtrar
     Sin60   : quitando solo 60 Hz
     SinArm  : quitando 60, 120, 180, 240, 300, 360 y 420 Hz (red y armonicos)

   Salidas:
     - USB (Serial, 250000 baud): "Bruto:x,Sin60:y,SinArm:z" para el Serial
       Plotter de la laptop (igual que el diagnostico del biceps).
     - Opto (Serial1, pin D1, 9600 baud): solo SinArm en milivoltios, una
       linea de texto por bloque ("640\n"). Es lo que lee el ESP32.

   Como leerlo (en reposo):
     Bruto mucho mayor que Sin60   -> hay mucho 60 Hz
     Sin60 mucho mayor que SinArm  -> los armonicos de la red dominan
     SinArm sigue alto (> ~0.05 V) -> ruido de banda ancha: electrodos, cables o la placa
   Al contraer, SinArm debe subir claramente.

   Regla de diseno: el Arduino solo mide y envia; nunca controla un actuador.

   Cambios v2 (mismo procesamiento que v1):
     - El envio ya no puede retrasar el muestreo: al opto sale 1 caracter
       cada 2 ms (a 9600 baud uno tarda 1.04 ms, asi nunca hay que esperar
       a que salga el anterior) y al USB solo lo que cabe antes de la
       siguiente muestra. Resuelve el pendiente "Envio en el Arduino".
     - El centro (DC) arranca en la primera lectura real, no en 2.5 V fijo.
     - Los primeros 0.5 s no se mandan al ESP32 (filtros asentandose).
     - TS_US sale de FS, y solo se crean los notch por debajo de FS/2.
     - PIN_MUESTREO opcional para ver el muestreo con el osciloscopio.

   Placa: "Arduino UNO R4 WiFi" (o Minima). Sin bibliotecas extra.
  =====================================================================
*/
#include <Arduino.h>
#include <math.h>

// ------------------------- Parametros ajustables -----------------------
const int   PIN_EMG  = A0;
const float FS       = 1000.0f;   // Hz. Si cambia, los notch se recalculan solos
const int   N_BLOQUE = 100;       // muestras por RMS (100 = 100 ms = cada cuanto llega un dato al ESP32)
const float ALPHA_DC = 0.003f;    // que tan lento sigue el centro de la senal (~0.5 Hz)
const int   N_ARM    = 7;         // notch en 60, 120, ... 420 Hz
const float Q_NOTCH  = 20.0f;     // ancho de cada notch = f / Q (3 Hz a 60 Hz)

const float V_REF = 5.0f, ADC_MAX = 16383.0f;     // 14 bits, referencia de 5 V

const unsigned long BAUD_USB  = 250000;
const unsigned long BAUD_OPTO = 9600;
const int TICKS_POR_CARACTER  = 2;     // opto: 1 caracter cada 2 muestras (2 ms)
const int BLOQUES_ASENTAR     = 5;     // primeros 5 bloques (0.5 s) no van al ESP32
const uint32_t MARGEN_US      = 150;   // tiempo que se deja libre antes de la siguiente muestra
const int PIN_MUESTREO        = -1;    // pon p. ej. 7 para ver cada muestra en el osciloscopio

const uint32_t TS_US = (uint32_t)(1000000.0f / FS + 0.5f);   // 1/FS en microsegundos

// ------------------------- Notch biquad (forma directa II transpuesta) --
struct Notch {
  float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  void init(float f, float Q) {
    float w0 = 2.0f * PI * f / FS, c = cosf(w0), al = sinf(w0) / (2.0f * Q), a0 = 1.0f + al;
    b0 = 1.0f / a0; b1 = -2.0f * c / a0; b2 = 1.0f / a0; a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0;
  }
  float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

Notch n60, arm[N_ARM];
int nArm = 0;                       // cuantos armonicos caben debajo de FS/2

float dc = 2.5f;

// RMS en bloques de N_BLOQUE muestras
double s_bruto = 0, s_60 = 0, s_arm = 0;
int cuenta = 0;
int bloquesHechos = 0;

// ------------------------- Salida sin esperas --------------------------
char usbLinea[64];  int usbLen = 0,  usbPos = 0;
char optoLinea[12]; int optoLen = 0, optoPos = 0;
int ticksOpto = 0;

// Escribe "1.234" (voltios con 3 decimales) usando enteros
int ponerVoltios(char *dst, int cap, const char *etiqueta, float v) {
  long mv = lroundf(v * 1000.0f);
  if (mv < 0) mv = 0;
  return snprintf(dst, cap, "%s:%ld.%03ld", etiqueta, mv / 1000, mv % 1000);
}

void prepararLineas(float rmsBruto, float rms60, float rmsArm) {
  // USB: igual que antes -> Bruto:0.123,Sin60:0.045,SinArm:0.012
  int n = 0;
  n += ponerVoltios(usbLinea + n, sizeof(usbLinea) - n, "Bruto", rmsBruto);
  usbLinea[n++] = ',';
  n += ponerVoltios(usbLinea + n, sizeof(usbLinea) - n, "Sin60", rms60);
  usbLinea[n++] = ',';
  n += ponerVoltios(usbLinea + n, sizeof(usbLinea) - n, "SinArm", rmsArm);
  usbLinea[n++] = '\r';
  usbLinea[n++] = '\n';
  usbLen = n;
  usbPos = 0;                       // si quedaba algo sin mandar, lo nuevo lo reemplaza

  // Opto: solo SinArm en mV, de 1 a 6 cifras y fin de linea -> "640\n"
  if (bloquesHechos > BLOQUES_ASENTAR) {
    long mv = lroundf(rmsArm * 1000.0f);
    mv = constrain(mv, 0L, 999999L);
    optoLen = snprintf(optoLinea, sizeof(optoLinea), "%ld\n", mv);
    optoPos = 0;
    ticksOpto = TICKS_POR_CARACTER;  // el primer caracter sale ya
  }
}

// Se llama una vez por muestra. Nunca espera a que un caracter termine de salir.
void enviarPendiente(uint32_t tSiguiente) {
  if (optoPos < optoLen && ++ticksOpto >= TICKS_POR_CARACTER) {
    ticksOpto = 0;
    Serial1.write((uint8_t)optoLinea[optoPos++]);
  }
  while (usbPos < usbLen && (int32_t)(tSiguiente - micros()) > (int32_t)MARGEN_US) {
    Serial.write((uint8_t)usbLinea[usbPos++]);
  }
}

// ======================================================================
void setup() {
  Serial.begin(BAUD_USB);
  Serial1.begin(BAUD_OPTO);          // enlace al ESP32 por el opto (pin D1)
  analogReadResolution(14);
  if (PIN_MUESTREO >= 0) pinMode(PIN_MUESTREO, OUTPUT);

  n60.init(60.0f, Q_NOTCH);
  for (int k = 0; k < N_ARM; k++) {
    float f = 60.0f * (k + 1);
    if (f < 0.5f * FS) arm[nArm++].init(f, Q_NOTCH);
  }

  // El centro arranca donde esta la senal de verdad (TL431, ~2.5 V)
  float s = 0;
  for (int i = 0; i < 16; i++) s += analogRead(PIN_EMG) * V_REF / ADC_MAX;
  dc = s / 16.0f;
}

void loop() {
  static uint32_t tSiguiente = micros();
  while ((int32_t)(micros() - tSiguiente) < 0) {}   // espera exacta, sin delay()
  tSiguiente += TS_US;
  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, HIGH);

  float v = analogRead(PIN_EMG) * V_REF / ADC_MAX;
  dc += ALPHA_DC * (v - dc);
  float x = v - dc;

  float y60 = n60.run(x);
  float ya = x;
  for (int k = 0; k < nArm; k++) ya = arm[k].run(ya);

  s_bruto += (double)x * x;
  s_60    += (double)y60 * y60;
  s_arm   += (double)ya * ya;

  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, LOW);

  if (++cuenta >= N_BLOQUE) {
    float rmsBruto = sqrt(s_bruto / N_BLOQUE);
    float rms60    = sqrt(s_60 / N_BLOQUE);
    float rmsArm   = sqrt(s_arm / N_BLOQUE);
    if (bloquesHechos <= BLOQUES_ASENTAR) bloquesHechos++;
    prepararLineas(rmsBruto, rms60, rmsArm);
    s_bruto = s_60 = s_arm = 0;
    cuenta = 0;
  }

  enviarPendiente(tSiguiente);
}
