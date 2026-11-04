/*
  emg_nucleo.h - Procesamiento de cada muestra (ASTRA, lado del nino)
  ---------------------------------------------------------------------
  Es EL MISMO procesamiento de diagnostico_emg_enlace.ino v2, copiado
  linea por linea (mismas constantes, mismas expresiones, mismo orden):
    quitar DC -> notch 60 Hz -> notch 60..420 Hz -> RMS cada 100 muestras
    -> linea "640\n" al opto, 1 caracter cada 2 muestras.

  Lo unico que cambia es DONDE corre: aqui se llama desde la interrupcion
  de un temporizador de hardware a 1000 Hz, asi el WiFi (que bloquea el
  loop() varios ms o segundos) no puede atrasar el muestreo ni el opto.

  Reglas para que sea seguro dentro de una interrupcion:
    - sin snprintf/printf, sin memoria dinamica, sin String;
    - el opto se escribe con un "gancho" (Serial1.write en el sketch);
    - lo que lee el loop() (RMS para el USB y la muestra filtrada para el
      WiFi) se publica en variables 'volatile'.

  tests/arduino_host/ compila este archivo en la PC junto con la v2
  original y comprueba que los bytes al opto son identicos, muestra a
  muestra.
*/
#pragma once
#include <math.h>
#include <stdint.h>

namespace emg {

// ------------------------- Parametros (identicos a v2) -----------------
const float FS       = 1000.0f;   // Hz. Si cambia, los notch se recalculan solos
const int   N_BLOQUE = 100;       // muestras por RMS (100 = 100 ms = cada cuanto llega un dato al ESP32)
const float ALPHA_DC = 0.003f;    // que tan lento sigue el centro de la senal (~0.5 Hz)
const int   N_ARM    = 7;         // notch en 60, 120, ... 420 Hz
const float Q_NOTCH  = 20.0f;     // ancho de cada notch = f / Q (3 Hz a 60 Hz)

const float V_REF = 5.0f, ADC_MAX = 16383.0f;     // 14 bits, referencia de 5 V

const int TICKS_POR_CARACTER  = 2;     // opto: 1 caracter cada 2 muestras (2 ms)
const int BLOQUES_ASENTAR     = 5;     // primeros 5 bloques (0.5 s) no van al ESP32

// ------------------------- Notch biquad (forma directa II transpuesta) --
struct Notch {
  float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  void init(float f, float Q) {
    float w0 = 2.0f * PI * f / FS, c = cosf(w0), al = sinf(w0) / (2.0f * Q), a0 = 1.0f + al;
    b0 = 1.0f / a0; b1 = -2.0f * c / a0; b2 = 1.0f / a0; a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0;
  }
  float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// ------------------------- Estado (solo lo toca la interrupcion) --------
static Notch n60, arm[N_ARM];
static int nArm = 0;                       // cuantos armonicos caben debajo de FS/2

static float dc = 2.5f;

static double s_bruto = 0, s_60 = 0, s_arm = 0;
static int cuenta = 0;
static int bloquesHechos = 0;

static char optoLinea[12]; static int optoLen = 0, optoPos = 0;
static int ticksOpto = 0;

static void (*enviarOpto)(uint8_t c) = nullptr;   // Serial1.write en el sketch

// ------------------------- Lo que se publica al loop() ------------------
struct Publicado {
  uint32_t bloque;           // cuantos bloques de RMS van (sube cada 100 ms)
  float rmsBruto, rms60, rmsArm;
};
static volatile Publicado pub = {0, 0, 0, 0};
static volatile float ultimaFiltrada = 0;     // ultima muestra SinArm (V)

// "%ld\n" para 0..999999 sin snprintf (mismos bytes; se prueba en la PC)
static int escribirEntero(char *dst, long v) {
  char tmp[8]; int n = 0;
  do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0 && n < 7);
  int k = 0;
  while (n > 0) dst[k++] = tmp[--n];
  dst[k++] = '\n';
  dst[k] = 0;
  return k;
}

// Igual que la parte "Opto" de prepararLineas() en v2
static void prepararOpto(float rmsArm) {
  if (bloquesHechos > BLOQUES_ASENTAR) {
    long mv = lroundf(rmsArm * 1000.0f);
    mv = constrain(mv, 0L, 999999L);
    optoLen = escribirEntero(optoLinea, mv);
    optoPos = 0;
    ticksOpto = TICKS_POR_CARACTER;  // el primer caracter sale ya
  }
}

// Igual que la parte "Opto" de enviarPendiente() en v2
static inline void enviarPendienteOpto() {
  if (optoPos < optoLen && ++ticksOpto >= TICKS_POR_CARACTER) {
    ticksOpto = 0;
    enviarOpto((uint8_t)optoLinea[optoPos++]);
  }
}

// Igual que el setup() de v2 (notch). 'leer' devuelve analogRead(PIN_EMG).
template <typename LeerAdc>
static void iniciar(void (*opto)(uint8_t), LeerAdc leer) {
  enviarOpto = opto;
  n60.init(60.0f, Q_NOTCH);
  for (int k = 0; k < N_ARM; k++) {
    float f = 60.0f * (k + 1);
    if (f < 0.5f * FS) arm[nArm++].init(f, Q_NOTCH);
  }

  // El centro arranca donde esta la senal de verdad (TL431, ~2.5 V)
  float s = 0;
  for (int i = 0; i < 16; i++) s += leer() * V_REF / ADC_MAX;
  dc = s / 16.0f;
}

// Una muestra = el cuerpo de loop() de v2 (sin la espera ni el USB).
// 'adc' es el valor de analogRead(PIN_EMG). Devuelve SinArm en voltios.
static inline float procesarMuestra(int adc) {
  float v = adc * V_REF / ADC_MAX;
  dc += ALPHA_DC * (v - dc);
  float x = v - dc;

  float y60 = n60.run(x);
  float ya = x;
  for (int k = 0; k < nArm; k++) ya = arm[k].run(ya);

  s_bruto += (double)x * x;
  s_60    += (double)y60 * y60;
  s_arm   += (double)ya * ya;

  if (++cuenta >= N_BLOQUE) {
    float rmsBruto = sqrt(s_bruto / N_BLOQUE);
    float rms60    = sqrt(s_60 / N_BLOQUE);
    float rmsArm   = sqrt(s_arm / N_BLOQUE);
    if (bloquesHechos <= BLOQUES_ASENTAR) bloquesHechos++;
    prepararOpto(rmsArm);
    pub.rmsBruto = rmsBruto; pub.rms60 = rms60; pub.rmsArm = rmsArm;
    pub.bloque = pub.bloque + 1;
    s_bruto = s_60 = s_arm = 0;
    cuenta = 0;
  }

  enviarPendienteOpto();
  ultimaFiltrada = ya;
  return ya;
}

}  // namespace emg
