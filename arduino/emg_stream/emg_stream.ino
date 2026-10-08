/*
  emg_stream.ino — Envía la señal sEMG CRUDA a la página web por USB.

  Todo el procesamiento (notch, pasa altas, envolvente, RMS, MAV) se hace
  en el navegador, así que aquí solo se muestrea a 2000 Hz y se transmite.

  Protocolo (binario, 20 muestras = 10 ms por paquete):
    0xAA 0x55 | seq (1 byte) | n (1 byte) | n x uint16 little-endian | checksum
    checksum = XOR de seq, n y todos los bytes de las muestras.
  - seq sube de 1 en 1 (0..255): la página detecta paquetes perdidos.
  - checksum: la página descarta paquetes corruptos.

  IMPORTANTE: cierra el Monitor Serie / Serial Plotter de Arduino antes de abrir la página;
  solo un programa puede usar el puerto a la vez.
*/

const int EMG_PIN = A1;
const unsigned long PERIODO_US = 500;      // 2000 Hz (debe coincidir con "Frecuencia de muestreo" en Ajustes)
const uint8_t N_PKT = 20;                  // muestras por paquete

uint8_t paquete[4 + 2 * N_PKT + 1];
uint8_t n = 0;
uint8_t seq = 0;
unsigned long siguienteMuestra;

void setup() {
  Serial.begin(500000);                    // en placas con USB nativo (p. ej. Uno R4) el valor se ignora
  analogReadResolution(14);                // 0..16383 ("Máximo del ADC" en Ajustes)
  paquete[0] = 0xAA;
  paquete[1] = 0x55;
  siguienteMuestra = micros();
}

void loop() {
  if ((int32_t)(micros() - siguienteMuestra) < 0) return;
  siguienteMuestra += PERIODO_US;

  uint16_t v = analogRead(EMG_PIN);
  paquete[4 + 2 * n]     = v & 0xFF;
  paquete[4 + 2 * n + 1] = v >> 8;
  n++;

  if (n >= N_PKT) {
    paquete[2] = seq++;
    paquete[3] = N_PKT;
    uint8_t chk = 0;
    for (uint8_t i = 2; i < 4 + 2 * N_PKT; i++) chk ^= paquete[i];
    paquete[4 + 2 * N_PKT] = chk;
    Serial.write(paquete, sizeof(paquete));
    n = 0;
  }
}
