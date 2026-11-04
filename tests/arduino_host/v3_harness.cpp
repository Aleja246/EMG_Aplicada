// Corre el nucleo v3 (lo que hace la interrupcion) con la misma senal.
#include "Arduino.h"
#include "entrada.h"
#include "../../arduino/diagnostico_emg_enlace_wifi/emg_nucleo.h"
#include "../../arduino/diagnostico_emg_enlace_wifi/emg_usb.h"
static void optoTx(uint8_t c) { Serial1.write(c); }
int main(int argc, char **argv) {
  int N = 120000;
  mock::adc = generarEntrada(N + 16);
  emg::iniciar(optoTx, [] { return analogRead(A0); });
  uint32_t ultimo = 0;
  for (int i = 0; i < N; i++) {
    emg::procesarMuestra(analogRead(A0));            // = la interrupcion
    if (emg::pub.bloque != ultimo) {                 // = el loop() del USB
      ultimo = emg::pub.bloque;
      char linea[64];
      int n = armarLineaUSB(linea, sizeof(linea), emg::pub.rmsBruto, emg::pub.rms60, emg::pub.rmsArm);
      for (int k = 0; k < n; k++) Serial.write((uint8_t)linea[k]);
    }
  }
  FILE *f = fopen(argv[1], "w");
  for (auto &e : mock::opto) fprintf(f, "%ld %d\n", e.muestra, e.byte);
  fclose(f);
  f = fopen(argv[2], "w"); fputs(mock::usb.c_str(), f); fclose(f);
  // escribirEntero == snprintf("%ld\n") en todo el rango que puede salir
  for (long v = 0; v <= 999999; v++) {
    char a[16], b[16];
    int la = emg::escribirEntero(a, v), lb = snprintf(b, sizeof(b), "%ld\n", v);
    if (la != lb || strcmp(a, b) != 0) { printf("FALLA escribirEntero(%ld)\n", v); return 1; }
  }
  printf("escribirEntero: identico a snprintf para 0..999999\n");
  return 0;
}
