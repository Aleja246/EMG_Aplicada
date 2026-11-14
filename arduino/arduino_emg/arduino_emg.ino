/*
  =====================================================================
   ASTRA - Arduino UNO R4 (lado del nino): arduino_emg.ino  v3
   Mide la senal de Nova, manda el RMS al ESP32 por el opto y la senal
   CRUDA a la pagina web por USB.
  ---------------------------------------------------------------------
   Lee A0 (JP3 puenteado) 1000 veces por segundo y, cada 100 ms, calcula
   tres RMS (en voltios, a la salida EMG_Out):
     Bruto   : senal sin DC, sin filtrar
     Sin60   : quitando solo 60 Hz
     SinArm  : quitando 60, 120, 180, 240, 300, 360 y 420 Hz (red y armonicos)

   Salidas:
     - Opto (Serial1, pin D1, 9600 baud): solo SinArm en milivoltios, una
       linea de texto por bloque ("640\n"). Es lo que lee el ESP32. SIN CAMBIOS.
     - USB (Serial, 250000 baud), segun USB_PARA_PAGINA:
         true  -> muestras CRUDAS del ADC para la pagina web (Web Serial),
                  en paquetes binarios de 20 muestras (20 ms):
                    0xAA 0x55 | seq | n | n x uint16 little-endian | checksum
                  checksum = XOR de seq, n y los bytes de las muestras.
                  La pagina filtra (DC, pasa altas, notch 60 Hz y armonicos)
                  y dibuja la senal en tiempo real.
         false -> "Bruto:x,Sin60:y,SinArm:z" para el Serial Plotter (como v2).

   SEGURIDAD: la laptop que abre la pagina NO debe estar cargando mientras
   el USB del Arduino toca a un nino con electrodos. El ESP32 se conecta a
   la pagina por Bluetooth (no por USB a la misma laptop) para que el opto
   siga aislando las tierras.

   Regla de diseno: el Arduino solo mide y envia; nunca controla un actuador.

   Cambios v3 respecto a v2 (mismo procesamiento y mismo envio al ESP32):
     - USB_PARA_PAGINA: la senal cruda de 1 kHz va a la pagina por USB.
       Los paquetes salen poco a poco, igual que antes: solo mientras falten
       mas de MARGEN_US para la siguiente muestra, asi el envio nunca
       retrasa el muestreo. Si la cola se llena (pagina cerrada o lenta) se
       descarta el paquete y la pagina lo detecta por el numero seq.

   Placa: "Arduino UNO R4 WiFi" (o Minima). Sin bibliotecas extra.
  =====================================================================
*/
#include <Arduino.h>
#include <math.h>

// ------------------------- Parametros ajustables -----------------------
const int   PIN_EMG  = A0;
const float FS       = 1000.0f;   // Hz. Si cambia, los notch se recalculan solos (y cambia "Frecuencia" en la pagina)
const int   N_BLOQUE = 100;       // muestras por RMS (100 = 100 ms = cada cuanto llega un dato al ESP32)
const float ALPHA_DC = 0.003f;    // que tan lento sigue el centro de la senal (~0.5 Hz)
const int   N_ARM    = 7;         // notch en 60, 120, ... 420 Hz
const float Q_NOTCH  = 20.0f;     // ancho de cada notch = f / Q (3 Hz a 60 Hz)

const float V_REF = 5.0f, ADC_MAX = 16383.0f;     // 14 bits, referencia de 5 V

const bool USB_PARA_PAGINA = true;   // true: senal cruda para la pagina. false: texto para el Serial Plotter

const unsigned long BAUD_USB  = 250000;   // la pagina abre el puerto a esta misma velocidad
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
char usbLinea[64];  int usbLen = 0,  usbPos = 0;      // modo Serial Plotter
char optoLinea[12]; int optoLen = 0, optoPos = 0;
int ticksOpto = 0;

// Modo pagina: paquetes de muestras crudas y una cola circular hacia el USB
const int N_PAQUETE = 20;                       // muestras por paquete (20 ms)
const int LARGO_PAQUETE = 4 + 2 * N_PAQUETE + 1;
uint8_t paquete[LARGO_PAQUETE];
int nPaquete = 0;
uint8_t seqPaquete = 0;
const int COLA_USB = 512;                       // ~11 paquetes de reserva
uint8_t colaUsb[COLA_USB];
int colaIni = 0, colaFin = 0;                   // vacia cuando son iguales

int colaLibre() { return (colaIni - colaFin - 1 + COLA_USB) % COLA_USB; }

void guardarMuestraCruda(uint16_t cuentaAdc) {
  paquete[4 + 2 * nPaquete]     = cuentaAdc & 0xFF;
  paquete[4 + 2 * nPaquete + 1] = cuentaAdc >> 8;
  if (++nPaquete < N_PAQUETE) return;
  nPaquete = 0;
  paquete[0] = 0xAA;
  paquete[1] = 0x55;
  paquete[2] = seqPaquete++;          // sube aunque el paquete se descarte: la pagina ve el hueco
  paquete[3] = N_PAQUETE;
  uint8_t chk = 0;
  for (int i = 2; i < LARGO_PAQUETE - 1; i++) chk ^= paquete[i];
  paquete[LARGO_PAQUETE - 1] = chk;
  if (colaLibre() < LARGO_PAQUETE) return;     // cola llena: se pierde este paquete
  for (int i = 0; i < LARGO_PAQUETE; i++) {
    colaUsb[colaFin] = paquete[i];
    colaFin = (colaFin + 1) % COLA_USB;
  }
}

// Escribe "1.234" (voltios con 3 decimales) usando enteros
int ponerVoltios(char *dst, int cap, const char *etiqueta, float v) {
  long mv = lroundf(v * 1000.0f);
  if (mv < 0) mv = 0;
  return snprintf(dst, cap, "%s:%ld.%03ld", etiqueta, mv / 1000, mv % 1000);
}

void prepararLineas(float rmsBruto, float rms60, float rmsArm) {
  if (!USB_PARA_PAGINA) {
    // USB: igual que v2 -> Bruto:0.123,Sin60:0.045,SinArm:0.012
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
  }

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
  if (USB_PARA_PAGINA) {
    while (colaIni != colaFin && (int32_t)(tSiguiente - micros()) > (int32_t)MARGEN_US) {
      Serial.write(colaUsb[colaIni]);
      colaIni = (colaIni + 1) % COLA_USB;
    }
  } else {
    while (usbPos < usbLen && (int32_t)(tSiguiente - micros()) > (int32_t)MARGEN_US) {
      Serial.write((uint8_t)usbLinea[usbPos++]);
    }
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

  int cuentaAdc = analogRead(PIN_EMG);
  float v = cuentaAdc * V_REF / ADC_MAX;
  dc += ALPHA_DC * (v - dc);
  float x = v - dc;

  float y60 = n60.run(x);
  float ya = x;
  for (int k = 0; k < nArm; k++) ya = arm[k].run(ya);

  s_bruto += (double)x * x;
  s_60    += (double)y60 * y60;
  s_arm   += (double)ya * ya;

  if (USB_PARA_PAGINA) guardarMuestraCruda((uint16_t)cuentaAdc);

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
