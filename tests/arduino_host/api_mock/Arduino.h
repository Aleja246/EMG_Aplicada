// Firmas copiadas del nucleo arduino:renesas_uno (ArduinoCore-renesas) y de
// ArduinoCore-API, solo para comprobar en la PC que el sketch v3 usa bien la API
// (tipos, sobrecargas, argumentos). No ejecuta nada.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define PI 3.1415926535897932384626433832795
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define LED_BUILTIN 13
#define A0 14
#define constrain(amt,low,high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
class String { public: const char *c_str() const; };
class IPAddress {
 public:
  IPAddress(); IPAddress(uint8_t, uint8_t, uint8_t, uint8_t);
  bool fromString(const char *address);
  String toString() const;
  bool operator==(const IPAddress &addr) const;
  bool operator!=(const IPAddress &addr) const;
};
class UART {
 public:
  void begin(unsigned long);
  size_t write(uint8_t c);
  size_t write(uint8_t *c, size_t len);
  int available(); int read();
  size_t print(const char *); size_t println(const char *); size_t println();
};
extern UART _UART1_, _UART2_;
#define Serial _UART1_
#define Serial1 _UART2_
int analogRead(int pinNumber);
void analogReadResolution(int bits);
void pinMode(int, int);
void digitalWrite(int, int);
unsigned long millis(); unsigned long micros();
void noInterrupts(); void interrupts();
// FSP / CMSIS
typedef int IRQn_Type;
typedef enum { FSP_PRIV_CLOCK_PCLKB = 3, FSP_PRIV_CLOCK_PCLKD = 5 } fsp_priv_clock_t;
uint32_t R_FSP_SystemClockHzGet(fsp_priv_clock_t clock);
void NVIC_SetPriority(IRQn_Type IRQn, uint32_t priority);
uint32_t NVIC_GetPriority(IRQn_Type IRQn);
