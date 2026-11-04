// Prueba del bufer circular y los paquetes WiFi (emg_flujo.h).
// Simula la interrupcion de 1 kHz "entrando" en cualquier punto mientras el
// loop() arma un paquete, y atrasos del WiFi de hasta 5 s.
// Comprueba: CRC correcto, ningun dato mezclado, indices exactos y que las
// muestras perdidas coinciden con los huecos.
#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <vector>
static void preempcion();
#define PUNTO_PREEMPCION() preempcion()
#include "../../arduino/diagnostico_emg_enlace_wifi/emg_flujo.h"

static uint32_t semilla = 777;
static double rnd() { semilla = semilla * 1664525u + 1013904223u; return (semilla >> 8) / 16777216.0; }

// Valor de la muestra i (voltios), incluye valores fuera de rango para probar el recorte
static float valor(uint32_t i) { return (float)(3.5 * sin(i * 0.013) * sin(i * 0.00071) + 0.0001 * (i % 7)); }
static int16_t esperado(uint32_t i) {
  long q = lroundf(valor(i) * (1000000.0f / 100));
  if (q > 32767) q = 32767;
  if (q < -32767) q = -32767;
  return (int16_t)q;
}

static uint32_t isrN = 0;
static void isr() { flujo::guardar(valor(isrN)); isrN++; }
static double pPre = 0, pVuelta = 0;
static void preempcion() {
  if (rnd() < pPre) {
    int k = (rnd() < pVuelta) ? 2100 + (int)(rnd() * 300) : 1 + (int)(rnd() * 3);  // a veces da la vuelta entera
    for (int j = 0; j < k; j++) isr();
  }
}

static uint16_t crcRef(const uint8_t *p, int n) {  // implementacion independiente (tabla)
  static uint16_t t[256]; static bool lista = false;
  if (!lista) { for (int i = 0; i < 256; i++) { uint16_t c = i << 8; for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1; t[i] = c; } lista = true; }
  uint16_t c = 0xFFFF;
  for (int i = 0; i < n; i++) c = (uint16_t)((c << 8) ^ t[((c >> 8) ^ p[i]) & 0xFF]);
  return c;
}

int main() {
  const uint8_t prueba[] = "123456789";
  if (flujo::crc16(prueba, 9) != 0x29B1 || crcRef(prueba, 9) != 0x29B1) { printf("FALLA: CRC-16/CCITT-FALSE\n"); return 1; }

  uint8_t pkt[flujo::TAM_MAX_PAQUETE];
  uint32_t esperadoIdx = 0;
  int fallas = 0;
  struct Caso { const char *nombre; double pPre, pVuelta; int atrasoMs; bool sinPerdidas; };
  const Caso casos[] = {
    {"A realista: atrasos WiFi de 1.5 s", 0.0, 0.0, 1500, true},
    {"B interrupcion durante la copia (20%) + atrasos de 1.5 s", 0.2, 0.0, 1500, true},
    {"C extremo: atrasos de 6 s y vueltas completas del bufer durante la copia", 0.05, 0.05, 6000, false},
  };
  for (const Caso &c : casos) {
    pPre = c.pPre; pVuelta = c.pVuelta;
    uint32_t perdidas0 = flujo::perdidas, huecos = 0, ok = 0, paquetes = 0, descartados = 0, n0 = isrN;
    for (int ms = 0; ms < 300000; ms++) {                       // 5 min por caso
      isr();
      bool atraso = (ms % 20000) >= 20000 - c.atrasoMs;
      if (atraso || (ms % 10) != 0) continue;                   // el loop corre cada 10 ms
      for (int vuelta = 0; vuelta < 8 && flujo::disponibles() >= (uint32_t)flujo::MIN_POR_PAQUETE; vuelta++) {
        int n = 0;
        int len = flujo::armarPaquete(pkt, flujo::MAX_POR_PAQUETE, &n, 1000);
        if (len == 0) { descartados++; continue; }
        paquetes++;
        int nn = pkt[3];
        uint32_t ini = pkt[6] | (pkt[7] << 8) | (pkt[8] << 16) | ((uint32_t)pkt[9] << 24);
        uint16_t crc = pkt[len - 2] | (pkt[len - 1] << 8);
        if (pkt[0] != 'E' || pkt[1] != 'M' || pkt[2] != 1 || nn != n || len != 12 + 2 * nn + 2 ||
            crc != crcRef(pkt, len - 2) || (pkt[4] | (pkt[5] << 8)) != 1000 || (pkt[10] | (pkt[11] << 8)) != 100) {
          printf("FALLA: cabecera/CRC\n"); return 1;
        }
        if (ini < esperadoIdx) { printf("FALLA: indice hacia atras\n"); return 1; }
        huecos += ini - esperadoIdx;
        for (int k = 0; k < nn; k++) {
          int16_t v = (int16_t)(pkt[12 + 2 * k] | (pkt[13 + 2 * k] << 8));
          if (v != esperado(ini + k)) { if (fallas++ < 5) printf("FALLA: muestra %u = %d, esperaba %d\n", ini + k, v, esperado(ini + k)); }
          else ok++;
        }
        flujo::confirmar(n);
        esperadoIdx = ini + nn;
      }
    }
    uint32_t perdidas = flujo::perdidas - perdidas0;
    printf("%s\n   muestras generadas %u, enviadas y correctas %u, huecos %u, perdidas contadas %u, paquetes %u (descartados %u)\n",
           c.nombre, isrN - n0, ok, huecos, perdidas, paquetes, descartados);
    if (huecos != perdidas) { printf("FALLA: huecos != perdidas contadas\n"); return 1; }
    if (c.sinPerdidas && huecos != 0) { printf("FALLA: con atrasos < 2 s no debe perderse nada\n"); return 1; }
  }
  if (fallas) { printf("FALLA: %d muestras mezcladas\n", fallas); return 1; }
  printf("OK: ningun dato mezclado; cada muestra perdida se ve como hueco en el indice.\n");
  return 0;
}
