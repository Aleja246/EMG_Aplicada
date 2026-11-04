/*
  emg_usb.h - Linea "Bruto:x,Sin60:y,SinArm:z" para el Serial Plotter.
  Mismo formato que prepararLineas() de v2. Solo se usa en el loop()
  (snprintf no se llama dentro de la interrupcion).
*/
#pragma once
#include <stdio.h>
#include <math.h>

// Escribe "1.234" (voltios con 3 decimales) usando enteros
static int ponerVoltios(char *dst, int cap, const char *etiqueta, float v) {
  long mv = lroundf(v * 1000.0f);
  if (mv < 0) mv = 0;
  return snprintf(dst, cap, "%s:%ld.%03ld", etiqueta, mv / 1000, mv % 1000);
}

// USB: igual que antes -> Bruto:0.123,Sin60:0.045,SinArm:0.012
static int armarLineaUSB(char *usbLinea, int cap, float rmsBruto, float rms60, float rmsArm) {
  int n = 0;
  n += ponerVoltios(usbLinea + n, cap - n, "Bruto", rmsBruto);
  usbLinea[n++] = ',';
  n += ponerVoltios(usbLinea + n, cap - n, "Sin60", rms60);
  usbLinea[n++] = ',';
  n += ponerVoltios(usbLinea + n, cap - n, "SinArm", rmsArm);
  usbLinea[n++] = '\r';
  usbLinea[n++] = '\n';
  return n;
}
