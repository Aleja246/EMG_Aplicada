// Senal de prueba deterministica (2 min a 1 kHz): reposo, contracciones de
// distinta fuerza, 60 Hz + armonicos, latido, deriva lenta y tramos saturados.
#pragma once
#include <vector>
#include <math.h>
#include <stdint.h>
static std::vector<int> generarEntrada(int n) {
  std::vector<int> v(n);
  uint32_t s = 12345;
  auto rnd = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; };
  for (int i = 0; i < n; i++) {
    double t = i / 1000.0;
    double base = 8192 + 300 * sin(2 * M_PI * 0.05 * t);              // deriva lenta
    double red = 40 * sin(2 * M_PI * 60 * t) + 12 * sin(2 * M_PI * 180 * t) + 6 * sin(2 * M_PI * 300 * t + 1);
    double fase = fmod(t, 12.0), amp = 8;                              // ruido de reposo
    if (fase > 3 && fase < 8) amp = 60 + 40 * sin(t);                  // contraccion
    if (fase > 9 && fase < 9.3) amp = 400;                             // pico de movimiento
    double ruido = (rnd() + rnd() + rnd() - 1.5) * 2 * amp;
    double latido = (fmod(t, 0.8) < 0.04) ? 150 * sin(M_PI * fmod(t, 0.8) / 0.04) : 0;
    double x = base + red + ruido + latido;
    if (t > 70 && t < 72) x = 16383;                                   // electrodo suelto
    if (t > 90 && t < 90.5) x = 0;
    int q = (int)lround(x);
    v[i] = q < 0 ? 0 : (q > 16383 ? 16383 : q);
  }
  return v;
}
