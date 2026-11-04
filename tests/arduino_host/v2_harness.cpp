// Corre la v2 ORIGINAL (sin tocar) con la senal de prueba.
#include "Arduino.h"
#include "entrada.h"
namespace v2 {
#include "../../arduino/diagnostico_emg_enlace/diagnostico_emg_enlace.ino"
}
int main(int argc, char **argv) {
  int N = 120000;
  mock::adc = generarEntrada(N + 16);
  v2::setup();
  while (mock::lecturas < (size_t)(N + 16)) v2::loop();
  FILE *f = fopen(argv[1], "w");
  for (auto &e : mock::opto) fprintf(f, "%ld %d\n", e.muestra, e.byte);
  fclose(f);
  f = fopen(argv[2], "w"); fputs(mock::usb.c_str(), f); fclose(f);
  return 0;
}
