// EMG craneocervical (esplenios) - Atlas2030 / Nuevo Amanecer
// Cadena: ADC -> quitar offset (adaptativo) -> notch 60 Hz -> rectificar -> pasa-bajas (envolvente lineal)
// Métricas: envolvente, RMS móvil 200 ms (corregido por ruido de reposo), activación con histéresis.
#include <math.h>

const int EMG_PIN = A1;

const float FS = 2000.0f;
const unsigned long PERIODO_US = 500;  // 1/FS

// Conversión cuentas -> mV en el pin del ADC (ajustar a tu placa: 5000 o 3300)
const float VREF_MV = 5000.0f;
const float MV_POR_CUENTA = VREF_MV / 16383.0f;
// Ganancia total de la parte analógica (preamp*banda*postamp). Con 1.0 los mV son "en el pin".
const float GANANCIA_ANALOGICA = 1.0f;
const float ESCALA = MV_POR_CUENTA / GANANCIA_ANALOGICA;

const float F_NOTCH = 60.0f;
const float Q_NOTCH = 20.0f;
const float FC_ENVOLVENTE = 6.0f;  // Hz. Más alto = más rápida y más rizada
const int N_RMS = 400;             // 200 ms

// Reposo: calibración inicial (músculo RELAJADO)
const float T_BASELINE_S = 0.5f;
const float T_CALENTAR_S = 1.0f;   // deja asentar los filtros
const float T_REPOSO_S = 1.0f;     // mide ruido de reposo
const float FACTOR_ON = 3.0f;      // umbral de activación = reposo * FACTOR
const float FACTOR_OFF = 2.0f;
// Envolvente (mV) en contracción máxima voluntaria; 0 = no normalizar
const float ENV_MVC_MV = 0.0f;

const float ALFA_BASELINE = 1.0f / (FS * 1.0f);  // seguidor de offset, tau = 1 s

struct Biquad {
  float b0, b1, b2, a1, a2;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  float paso(float x) {
    float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x;
    y2 = y1; y1 = y;
    return y;
  }

  void notch(float f0, float q) {
    float w0 = 2.0f * PI * f0 / FS;
    float alpha = sinf(w0) / (2.0f * q);
    float c = cosf(w0);
    float a0 = 1.0f + alpha;
    b0 = 1.0f / a0;
    b1 = -2.0f * c / a0;
    b2 = 1.0f / a0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void pasaBajas(float fc) {  // Butterworth 2º orden
    float w0 = 2.0f * PI * fc / FS;
    float alpha = sinf(w0) / (2.0f * 0.70710678f);
    float c = cosf(w0);
    float a0 = 1.0f + alpha;
    b0 = (1.0f - c) / 2.0f / a0;
    b1 = (1.0f - c) / a0;
    b2 = b0;
    a1 = -2.0f * c / a0;
    a2 = (1.0f - alpha) / a0;
  }
};

Biquad notch, envLPF;

float baseline = 0;
float envCuentas = 0;  // envolvente lineal (cuentas)
float envReposo = 0;   // envolvente en reposo (cuentas)
float ms2Reposo = 0;   // potencia RMS en reposo (cuentas^2)
bool activo = false;
float umbralOn = 1e9f, umbralOff = 1e9f;

// RMS móvil exacto (enteros, sin deriva)
int32_t bufCuad[N_RMS];
int idxRms = 0;
int64_t sumaCuad = 0;

// Diagnóstico por ventana de impresión
float minRaw = 1e9f, maxRaw = -1e9f;
unsigned long desbordes = 0;
unsigned long siguienteMuestra;

float procesar(float adc) {
  if (adc < minRaw) minRaw = adc;
  if (adc > maxRaw) maxRaw = adc;

  // 1) Offset adaptativo (sigue la deriva del nivel DC del offset analógico)
  baseline += ALFA_BASELINE * (adc - baseline);
  float x = adc - baseline;

  // 2) Notch 60 Hz
  float y = notch.paso(x);

  // 3) Rectificación + 4) envolvente lineal
  envCuentas = envLPF.paso(fabsf(y));
  if (envCuentas < 0) envCuentas = 0;

  // 5) RMS móvil
  int32_t c = (int32_t)(y * y);
  sumaCuad += c - bufCuad[idxRms];
  bufCuad[idxRms] = c;
  if (++idxRms >= N_RMS) idxRms = 0;

  return y;
}

float esperarMuestra() {
  while ((int32_t)(micros() - siguienteMuestra) < 0) {}
  siguienteMuestra += PERIODO_US;
  return (float)analogRead(EMG_PIN);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(14);

  notch.notch(F_NOTCH, Q_NOTCH);
  envLPF.pasaBajas(FC_ENVOLVENTE);

  siguienteMuestra = micros();

  // Offset inicial
  int nBase = (int)(T_BASELINE_S * FS);
  for (int i = 0; i < nBase; i++) baseline += esperarMuestra();
  baseline /= nBase;

  // Calentar filtros
  for (int i = 0; i < (int)(T_CALENTAR_S * FS); i++) procesar(esperarMuestra());

  // Medir reposo
  int nRep = (int)(T_REPOSO_S * FS);
  float sumaEnv = 0;
  for (int i = 0; i < nRep; i++) {
    procesar(esperarMuestra());
    sumaEnv += envCuentas;
  }
  envReposo = sumaEnv / nRep;
  ms2Reposo = (float)sumaCuad / N_RMS;

  umbralOn = envReposo * FACTOR_ON;
  umbralOff = envReposo * FACTOR_OFF;

  siguienteMuestra = micros();
  minRaw = 1e9f;
  maxRaw = -1e9f;
}

int contSalida = 0;

void loop() {
  unsigned long ahora = micros();
  int32_t retraso = (int32_t)(ahora - siguienteMuestra);
  if (retraso < 0) return;
  if (retraso > (int32_t)PERIODO_US) {  // nos pasamos de una muestra: lo contamos y resincronizamos
    desbordes++;
    siguienteMuestra = ahora;
  }
  siguienteMuestra += PERIODO_US;

  procesar((float)analogRead(EMG_PIN));

  // Salida a 25 Hz (cada 80 muestras)
  if (++contSalida < 80) return;
  contSalida = 0;

  float envMv = envCuentas * ESCALA;
  float rmsCuentas = sqrtf((float)sumaCuad / N_RMS);
  float rmsMv = rmsCuentas * ESCALA;

  // RMS sin el ruido de reposo (las potencias se suman en cuadratura)
  float ms2 = (float)sumaCuad / N_RMS - ms2Reposo;
  float rmsNetoMv = sqrtf(ms2 > 0 ? ms2 : 0) * ESCALA;

  // Activación con histéresis
  if (!activo && envCuentas > umbralOn) activo = true;
  else if (activo && envCuentas < umbralOff) activo = false;

  // % de activación (para actuadores) si se definió ENV_MVC_MV
  float pct = 0;
  if (ENV_MVC_MV > 0) {
    pct = 100.0f * (envMv - envReposo * ESCALA) / (ENV_MVC_MV - envReposo * ESCALA);
    pct = constrain(pct, 0.0f, 100.0f);
  }

  float ppMv = (maxRaw - minRaw) * ESCALA;  // diagnóstico: compara con el osciloscopio
  minRaw = 1e9f;
  maxRaw = -1e9f;

  Serial.print("Env_mV:");     Serial.print(envMv, 3);
  Serial.print("\tRMS_mV:");    Serial.print(rmsMv, 3);
  Serial.print("\tRMSneto_mV:"); Serial.print(rmsNetoMv, 3);
  Serial.print("\tpp_mV:");     Serial.print(ppMv, 1);
  Serial.print("\tAct:");       Serial.print(activo ? 1 : 0);
  Serial.print("\tPct:");       Serial.print(pct, 1);
  Serial.print("\tDesb:");      Serial.println(desbordes);
}
