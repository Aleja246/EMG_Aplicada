
#include <math.h>

const int EMG_PIN = A1;

const float FS = 2000.0f;
const float F_NOTCH = 60.0f;
const float Q = 20.0f;

const unsigned long PERIODO_US = 500;
const int N = 400;  // Ventana RMS de 200 ms

// Coeficientes del notch
float b0, b1, b2, a1, a2;

// Memoria del filtro
float x1 = 0, x2 = 0;
float salidaAnterior1 = 0, salidaAnterior2 = 0;

// Offset del ADC
float baseline = 0;

// Acumuladores RMS
float sumaRaw = 0;
float sumaNotch = 0;

int muestras = 0;
unsigned long siguienteMuestra;

void setup() {
  Serial.begin(115200);
  analogReadResolution(14);

  // Calibrar offset durante 0.5 segundos
  // Mantener la entrada sin señal AC.
  for (int i = 0; i < 1000; i++) {
    unsigned long inicio = micros();

    baseline += analogRead(EMG_PIN);

    while (micros() - inicio < PERIODO_US) {}
  }

  baseline /= 1000.0f;

  // Calcular coeficientes del notch de 60 Hz
  float w0 = 2.0f * PI * F_NOTCH / FS;
  float alpha = sinf(w0) / (2.0f * Q);
  float c = cosf(w0);

  float a0 = 1.0f + alpha;

  b0 = 1.0f / a0;
  b1 = -2.0f * c / a0;
  b2 = 1.0f / a0;

  a1 = -2.0f * c / a0;
  a2 = (1.0f - alpha) / a0;

  siguienteMuestra = micros();
}

void loop() {

  if ((int32_t)(micros() - siguienteMuestra) < 0) {
    return;
  }

  siguienteMuestra += PERIODO_US;

  // Leer ADC y quitar offset
  float x = (float)analogRead(EMG_PIN) - baseline;

// Filtro notch de 60 Hz
float y = b0*x + b1*x1 + b2*x2
          - a1*salidaAnterior1
          - a2*salidaAnterior2;

// Actualizar muestras anteriores
x2 = x1;
x1 = x;

salidaAnterior2 = salidaAnterior1;
salidaAnterior1 = y;

  // Acumular RMS
  sumaRaw += x*x;
  sumaNotch += y*y;

  muestras++;

  if (muestras >= N) {

    // Convertir cuentas ADC a mV aproximados
    float escala = 5000.0f / 16383.0f;

    float rmsRaw = sqrtf(sumaRaw / N) * escala;
    float rmsNotch = sqrtf(sumaNotch / N) * escala;

    // Mostrar en Serial Plotter
    Serial.print("RMS_original:");
    Serial.print(rmsRaw, 3);

    Serial.print("\tRMS_notch:");
    Serial.println(rmsNotch, 3);

    sumaRaw = 0;
    sumaNotch = 0;
    muestras = 0;
  }
}

