// EMG craneocervical - Atlas2030 / Nuevo Amanecer
// Versión mínima: ADC -> restar nivel de reposo fijo -> notch 60 Hz -> RMS móvil de 200 ms.
#include <math.h>

const int EMG_PIN = A1;

const float FS = 2000.0f;
const unsigned long PERIODO_US = 500;  // 1/FS

// Cuentas -> mV en el pin del ADC (VREF 5000 o 3300 según la placa)
const float VREF_MV = 5000.0f;
const float ESCALA = VREF_MV / 16383.0f;

const float F_NOTCH = 60.0f;
const float Q_NOTCH = 20.0f;
const int N_RMS = 400;  // 200 ms

// Coeficientes y memoria del notch
float b0, b1, b2, a1, a2;
float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

// Nivel DC del ADC (el offset analógico ya centra la señal): se mide una vez al inicio
float baseline = 0;

// RMS móvil exacto (enteros, sin deriva)
int32_t bufRaw[N_RMS], bufNotch[N_RMS];
int idx = 0;
int64_t sumaRaw = 0, sumaNotch = 0;

// Diagnóstico
float minRaw = 1e9f, maxRaw = -1e9f;
unsigned long desbordes = 0;
unsigned long siguienteMuestra;
int contSalida = 0;

void setup() {
  Serial.begin(115200);
  analogReadResolution(14);

  // Notch de 60 Hz
  float w0 = 2.0f * PI * F_NOTCH / FS;
  float alpha = sinf(w0) / (2.0f * Q_NOTCH);
  float c = cosf(w0);
  float a0 = 1.0f + alpha;
  b0 = 1.0f / a0;
  b1 = -2.0f * c / a0;
  b2 = 1.0f / a0;
  a1 = -2.0f * c / a0;
  a2 = (1.0f - alpha) / a0;

  // Nivel DC inicial (0.5 s, músculo relajado)
  siguienteMuestra = micros();
  for (int i = 0; i < 1000; i++) {
    while ((int32_t)(micros() - siguienteMuestra) < 0) {}
    siguienteMuestra += PERIODO_US;
    baseline += analogRead(EMG_PIN);
  }
  baseline /= 1000.0f;
  siguienteMuestra = micros();
}

void loop() {
  unsigned long ahora = micros();
  int32_t retraso = (int32_t)(ahora - siguienteMuestra);
  if (retraso < 0) return;
  if (retraso > (int32_t)PERIODO_US) {  // se perdió una muestra: contar y resincronizar
    desbordes++;
    siguienteMuestra = ahora;
  }
  siguienteMuestra += PERIODO_US;

  float adc = (float)analogRead(EMG_PIN);
  if (adc < minRaw) minRaw = adc;
  if (adc > maxRaw) maxRaw = adc;

  // Restar nivel DC fijo
  float x = adc - baseline;

  // Notch 60 Hz
  float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
  x2 = x1; x1 = x;
  y2 = y1; y1 = y;

  // RMS móvil
  int32_t cr = (int32_t)(x * x), cn = (int32_t)(y * y);
  sumaRaw += cr - bufRaw[idx];
  sumaNotch += cn - bufNotch[idx];
  bufRaw[idx] = cr;
  bufNotch[idx] = cn;
  if (++idx >= N_RMS) idx = 0;

  // Salida a 25 Hz
  if (++contSalida < 80) return;
  contSalida = 0;

  Serial.print("RMS_original:"); Serial.print(sqrtf((float)sumaRaw / N_RMS) * ESCALA, 3);
  Serial.print("\tRMS_notch:");  Serial.print(sqrtf((float)sumaNotch / N_RMS) * ESCALA, 3);
  Serial.print("\tpp_mV:");      Serial.print((maxRaw - minRaw) * ESCALA, 1);
  Serial.print("\tDesb:");       Serial.println(desbordes);
  minRaw = 1e9f;
  maxRaw = -1e9f;
}
