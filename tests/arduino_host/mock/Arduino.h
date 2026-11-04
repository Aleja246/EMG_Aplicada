// Arduino.h de prueba (PC): lo justo para compilar diagnostico_emg_enlace.ino v2
// y el nucleo v3. analogRead() entrega una senal sintetica fija.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <string>

#define PI 3.1415926535897932384626433832795
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define A0 14
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))

namespace mock {
extern std::vector<int> adc;          // senal de entrada (cuentas)
extern size_t lecturas;               // cuantas veces se llamo analogRead
extern uint32_t ahora_us;             // reloj simulado
struct Evento { long muestra; uint8_t byte; };
extern std::vector<Evento> opto;      // bytes al opto con la muestra en que salieron
extern std::string usb;               // todo lo escrito al USB
}

inline int analogRead(int) { int v = mock::adc[mock::lecturas % mock::adc.size()]; mock::lecturas++; return v; }
inline void analogReadResolution(int) {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline uint32_t micros() { return mock::ahora_us++; }   // avanza 1 us por llamada

struct MockSerial {
  int puerto;
  void begin(unsigned long) {}
  size_t write(uint8_t c) {
    if (puerto == 1) mock::opto.push_back({(long)mock::lecturas - 17, c});  // 16 lecturas de DC + la actual
    else mock::usb.push_back((char)c);
    return 1;
  }
};
extern MockSerial Serial, Serial1;
