/*
  emg_rms.ino — Arduino UNO R4: SOLO MIDE (versión "SinArm").
  ---------------------------------------------------------------------------
  Regla del proyecto: un solo cerebro.
    Arduino : mide. No sabe nada de calibración ni de actuadores.
    ESP32   : decide todo (calibración, detección, actuadores, métricas, página).

  Qué hace (basado en DIGITALIZARR.ino):
    1. Muestrea A1 a 2000 Hz con el ADC de 14 bits.
    2. Quita el offset (DC) y el ruido de la red: notch en 60 Hz y sus
       armónicos (120, 180, ... 480 Hz) -> "SinArm".
    3. Cada 50 ms calcula el RMS de los últimos 200 ms (ventana deslizante).
    4. Manda UNA línea por el opto (Serial1 = pin D1/TX, 9600 baud):
           R,0.052          <- RMS en mV, termina en '\n'
       20 líneas/s x 8 bytes ~ 17 % de la capacidad del enlace.
    5. Copia legible por USB (Monitor / Serial Plotter a 115200): RMS:0.052

  Conexión: D1 (TX) -> resistencia -> LED del PC817 -> ESP32 IO35 (como antes).
  Placa: Arduino UNO R4 Minima o UNO R4 WiFi. Sin bibliotecas extra.
*/
#include <math.h>

// ------------------------------ Ajustes ------------------------------------
const int   EMG_PIN       = A1;
const unsigned long PERIODO_US = 500;               // 2000 Hz
const float FS            = 1000000.0f / PERIODO_US;
const float MV_POR_CUENTA = 5000.0f / 16383.0f;     // igual que DIGITALIZARR (14 bits, 5 V)

const float F_RED         = 60.0f;   // frecuencia de la red eléctrica
const int   N_ARMONICOS   = 8;       // notch en 60, 120, ..., 480 Hz
const float Q_NOTCH       = 25.0f;   // ancho de cada notch = f / Q (60 Hz -> 2.4 Hz)
const bool  PASA_ALTAS_20HZ = false; // true = quita también movimiento y latido (< 20 Hz).
                                     // Úsalo solo si con el ATLAS caminando hay muchos falsos.

const int   MUESTRAS_BLOQUE = 100;   // 100 muestras = 50 ms -> una línea cada 50 ms
const int   N_BLOQUES       = 4;     // 4 bloques = RMS de 200 ms
const int   DECIMALES       = 3;     // R,0.052
const long  BAUD_ENLACE     = 9600;  // debe coincidir con el ESP32
const bool  ECO_USB         = true;  // copia por USB para el Serial Plotter

// ------------------------------ Filtros ------------------------------------
struct Biquad {                       // forma directa II transpuesta
  float b0, b1, b2, a1, a2, z1, z2;
  void notch(float f0, float q) {
    float w = 2.0f * PI * f0 / FS, c = cosf(w), al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    b0 = 1.0f / a0; b1 = -2.0f * c / a0; b2 = b0; a1 = b1; a2 = (1.0f - al) / a0; z1 = z2 = 0;
  }
  void pasaAltas(float fc, float q) {
    float w = 2.0f * PI * fc / FS, c = cosf(w), al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    b0 = (1.0f + c) / (2.0f * a0); b1 = -(1.0f + c) / a0; b2 = b0;
    a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0; z1 = z2 = 0;
  }
  float procesar(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

Biquad notch[N_ARMONICOS];
Biquad pasaAltas;
int    nNotch = 0;

float offsetAdc = 0;                  // cuentas
float sumaBloque = 0;                 // suma de cuadrados del bloque actual
float sumas[N_BLOQUES];               // últimos 4 bloques
int   nMuestras = 0, iBloque = 0, bloquesListos = 0;
unsigned long siguienteMuestra;

// Escribe v con 'dec' decimales sin usar printf de float. Devuelve los caracteres.
int formatear(char *s, float v, int dec) {
  if (!(v >= 0.0f)) v = 0.0f;         // también atrapa NaN
  if (v > 9999.0f) v = 9999.0f;
  unsigned long escala = 1;
  for (int i = 0; i < dec; i++) escala *= 10;
  unsigned long total = (unsigned long)(v * escala + 0.5f);
  unsigned long ent = total / escala, fr = total % escala;
  char tmp[12];
  int t = 0, n = 0;
  do { tmp[t++] = '0' + ent % 10; ent /= 10; } while (ent);
  while (t) s[n++] = tmp[--t];
  if (dec > 0) {
    s[n++] = '.';
    for (unsigned long d = escala / 10; d; d /= 10) s[n++] = '0' + (fr / d) % 10;
  }
  return n;
}

void enviar(float rms) {
  char linea[24];
  int n = 0;
  linea[n++] = 'R';
  linea[n++] = ',';
  n += formatear(linea + n, rms, DECIMALES);
  linea[n++] = '\n';
  Serial1.write((const uint8_t *)linea, n);        // al ESP32 (por el opto)
  if (ECO_USB) {
    Serial.print("RMS:");
    Serial.write((const uint8_t *)linea + 2, n - 2);
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(BAUD_ENLACE);
  analogReadResolution(14);

  // Offset del ADC: promedio de 0.5 s (como DIGITALIZARR). Luego se sigue la deriva lenta.
  for (int i = 0; i < 1000; i++) {
    unsigned long t0 = micros();
    offsetAdc += analogRead(EMG_PIN);
    while (micros() - t0 < PERIODO_US) {}
  }
  offsetAdc /= 1000.0f;

  for (int k = 1; k <= N_ARMONICOS; k++) {
    if (F_RED * k >= 0.45f * FS) break;              // no pasar de la mitad de FS
    notch[nNotch++].notch(F_RED * k, Q_NOTCH);
  }
  pasaAltas.pasaAltas(20.0f, 0.7071f);
  for (int b = 0; b < N_BLOQUES; b++) sumas[b] = 0;
  siguienteMuestra = micros();
}

void loop() {
  long atraso = (long)(micros() - siguienteMuestra);
  if (atraso < 0) return;                            // todavía no toca muestrear
  if (atraso > 20000) siguienteMuestra = micros();   // atraso > 20 ms: resincroniza
  siguienteMuestra += PERIODO_US;

  float x = (float)analogRead(EMG_PIN);
  offsetAdc += 0.0005f * (x - offsetAdc);            // deriva del offset (~0.2 Hz)
  float y = (x - offsetAdc) * MV_POR_CUENTA;          // mV
  if (PASA_ALTAS_20HZ) y = pasaAltas.procesar(y);
  for (int k = 0; k < nNotch; k++) y = notch[k].procesar(y);

  sumaBloque += y * y;
  if (++nMuestras < MUESTRAS_BLOQUE) return;

  // Cada 50 ms: guarda el bloque y manda el RMS de los últimos 200 ms
  sumas[iBloque] = sumaBloque;
  iBloque = (iBloque + 1) % N_BLOQUES;
  sumaBloque = 0;
  nMuestras = 0;
  if (bloquesListos < 20) { bloquesListos++; return; } // 1 s para que se asienten los filtros

  float total = 0;
  for (int b = 0; b < N_BLOQUES; b++) total += sumas[b];
  enviar(sqrtf(total / (MUESTRAS_BLOQUE * N_BLOQUES)));
}
