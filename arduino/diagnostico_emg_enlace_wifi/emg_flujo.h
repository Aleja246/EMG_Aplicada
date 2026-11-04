/*
  emg_flujo.h - Bufer circular y paquetes de la senal filtrada para el WiFi
  ---------------------------------------------------------------------
  La interrupcion de 1 kHz guarda cada muestra SinArm (ya filtrada) en un
  bufer circular. El loop() la saca en paquetes y la manda por WiFi.
  Si el WiFi se atrasa mas de lo que cabe en el bufer (~2 s), se pierden
  muestras SOLO del canal WiFi: el muestreo, el RMS y el opto no se enteran.

  Formato del paquete (little-endian), version 1:
    off  bytes  campo
     0    2     'E' 'M'                (0x45 0x4D)
     2    1     version = 1
     3    1     n = muestras en este paquete (1..MAX_POR_PAQUETE)
     4    2     fs en Hz (1000)
     6    4     indice de la primera muestra (cuenta desde el arranque)
    10    2     microvoltios por unidad (100 -> cada unidad = 0.1 mV)
    12   2n     muestras int16 (SinArm a la salida de Nova, en 0.1 mV)
  12+2n   2     CRC-16/CCITT-FALSE de los bytes 0 .. 11+2n

  El indice permite a la pagina poner cada muestra en su tiempo exacto
  (t = indice / fs) y saber cuantas faltan si se perdio algo.
  La misma definicion esta en puente/puente_emg.py y js/sources.js.
*/
#pragma once
#include <stdint.h>
#include <math.h>

#ifndef PUNTO_PREEMPCION
#define PUNTO_PREEMPCION()   // en la PC, las pruebas simulan aqui la interrupcion
#endif

namespace flujo {

const uint32_t TAM_BUFER       = 2048;              // potencia de 2 (2.0 s a 1 kHz, 4 KB)
const uint32_t MASCARA         = TAM_BUFER - 1;
const int      MAX_POR_PAQUETE = 100;               // 100 muestras = 100 ms por paquete
const int      MIN_POR_PAQUETE = 50;                // se envia cada ~50 ms
const uint16_t UV_POR_UNIDAD   = 100;               // 0.1 mV
const int      CABECERA        = 12;
const int      TAM_MAX_PAQUETE = CABECERA + 2 * MAX_POR_PAQUETE + 2;

static int16_t bufer[TAM_BUFER];
static volatile uint32_t cabeza = 0;    // muestras escritas desde el arranque (solo la escribe la interrupcion)
static uint32_t cola = 0;               // siguiente muestra a enviar (solo la usa el loop)
static uint32_t perdidas = 0;           // muestras que el WiFi no alcanzo a enviar

// --- Lado de la interrupcion: una escritura, sin esperas ---
static inline void guardar(float voltios) {
  float u = voltios * (1000000.0f / UV_POR_UNIDAD);       // V -> unidades de 0.1 mV
  long q = lroundf(u);
  if (q > 32767) q = 32767;
  if (q < -32767) q = -32767;
  uint32_t h = cabeza;
  bufer[h & MASCARA] = (int16_t)q;
  cabeza = h + 1;                                          // se publica despues de escribir
}

// --- Lado del loop() ---
static inline uint32_t leerCabeza() { return cabeza; }   // 32 bits: lectura atomica en Cortex-M4

// Descarta lo pendiente (al conectar: se empieza desde "ahora")
static inline void saltarAlPresente() { cola = leerCabeza(); }

// Muestras listas para enviar. Si el bufer se lleno, salta y cuenta las perdidas.
static uint32_t disponibles() {
  uint32_t h = leerCabeza();
  uint32_t d = h - cola;
  if (d > TAM_BUFER - MAX_POR_PAQUETE) {          // deja margen: la interrupcion sigue escribiendo
    uint32_t nueva = h - TAM_BUFER / 2;
    perdidas += nueva - cola;
    cola = nueva;
    d = h - cola;
  }
  return d;
}

static uint16_t crc16(const uint8_t *p, int n) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; i++) {
    c ^= (uint16_t)p[i] << 8;
    for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
  }
  return c;
}

static inline void escribir16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static inline void escribir32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = v >> 24; }

// Arma un paquete con hasta 'maxN' muestras desde 'cola'. Devuelve su largo en
// bytes (0 si no hay nada o si la interrupcion alcanzo lo que se copiaba).
// No mueve 'cola': hay que llamar confirmar(n) cuando el envio salio bien.
static int armarPaquete(uint8_t *pkt, int maxN, int *nMuestras, uint16_t fsHz) {
  uint32_t d = disponibles();
  if (d == 0) return 0;
  int n = (int)(d < (uint32_t)maxN ? d : (uint32_t)maxN);
  uint32_t inicio = cola;

  pkt[0] = 'E'; pkt[1] = 'M'; pkt[2] = 1; pkt[3] = (uint8_t)n;
  escribir16(pkt + 4, fsHz);
  escribir32(pkt + 6, inicio);
  escribir16(pkt + 10, UV_POR_UNIDAD);
  for (int i = 0; i < n; i++) {
    PUNTO_PREEMPCION();
    uint16_t v = (uint16_t)bufer[(inicio + i) & MASCARA];
    escribir16(pkt + CABECERA + 2 * i, v);
  }
  // Si mientras copiabamos la interrupcion dio la vuelta al bufer, lo copiado
  // puede estar mezclado: se descarta y se cuenta como perdido.
  if (leerCabeza() - inicio > TAM_BUFER) {
    disponibles();
    return 0;
  }
  int len = CABECERA + 2 * n;
  escribir16(pkt + len, crc16(pkt, len));
  *nMuestras = n;
  return len + 2;
}

static inline void confirmar(int n) { cola += (uint32_t)n; }

}  // namespace flujo
