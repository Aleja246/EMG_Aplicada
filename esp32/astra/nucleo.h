// nucleo.h — Lógica pura del ESP32 (sin Arduino): se puede probar en la computadora.
//   LectorLineas : lee "R,0.052\n" del Arduino y descarta líneas dañadas.
//   Estad        : media y desviación estándar (Welford).
//   MaxSostenido : máximo que se mantiene 2 muestras seguidas (100 ms).
//   Umbrales     : Hodges y Bui: alto = media + k·SD, bajo = media + k/2·SD.
//   Detector     : cuenta un levantamiento cuando el RMS pasa el umbral alto
//                  durante 100 ms (2 muestras); el siguiente solo se cuenta
//                  después de bajar del umbral bajo.
//   Metricas     : lo que se guarda de cada activación y los totales de la sesión.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <math.h>

namespace nucleo {

const int   MUESTRAS_CONFIRMAR = 2;        // 2 x 50 ms = 100 ms sobre el umbral alto
const float SD_MIN_RELATIVA    = 0.05f;    // la SD efectiva nunca es menor al 5 % de la media...
const float SD_MIN_ABSOLUTA    = 0.0005f;  // ...ni menor a esto (unidades del Arduino)
const float MARGEN_PASIVA      = 1.05f;    // el umbral queda 5 % arriba del artefacto pasivo

// ---------------------------------------------------------------------------
class LectorLineas {
 public:
  uint32_t malas = 0;                       // líneas dañadas (diagnóstico)

  // Procesa un byte. Devuelve true al completar una línea válida "R,<número>".
  bool procesar(char c, uint32_t ahoraMs, float &valor) {
    if (len > 0 && ahoraMs - tUltimo > CORTE_MS) mala = true;   // hueco a media línea
    tUltimo = ahoraMs;
    if (c == '\r') return false;
    if (c != '\n') {
      if (len < sizeof(buf) - 1) buf[len++] = c;
      else mala = true;
      return false;
    }
    bool ok = false;
    if (len > 0) {
      buf[len] = 0;
      ok = !mala && interpretar(valor);
      if (!ok) malas++;
    }
    len = 0;
    mala = false;
    return ok;
  }

 private:
  static const uint32_t CORTE_MS = 50;
  char buf[16];
  uint8_t len = 0;
  bool mala = false;
  uint32_t tUltimo = 0;

  bool interpretar(float &valor) {
    if (len < 3 || buf[0] != 'R' || buf[1] != ',') return false;
    char *fin;
    float v = strtof(buf + 2, &fin);
    if (fin == buf + 2 || *fin != 0 || !isfinite(v) || v < 0 || v > 100000.0f) return false;
    valor = v;
    return true;
  }
};

// ---------------------------------------------------------------------------
struct Estad {
  uint32_t n = 0;
  double media = 0, m2 = 0;
  void reiniciar() { n = 0; media = 0; m2 = 0; }
  void agregar(float x) {
    n++;
    double d = x - media;
    media += d / n;
    m2 += d * (x - media);
  }
  float sd() const { return n > 1 ? (float)sqrt(m2 / (n - 1)) : 0.0f; }
};

// Máximo que se sostiene 2 muestras seguidas: un pico aislado no cuenta.
struct MaxSostenido {
  float maximo = 0, previo = -1;
  void reiniciar() { maximo = 0; previo = -1; }
  void corte() { previo = -1; }             // hubo un hueco: no unir muestras
  void agregar(float x) {
    if (previo >= 0) {
      float m = x < previo ? x : previo;
      if (m > maximo) maximo = m;
    }
    previo = x;
  }
};

// ---------------------------------------------------------------------------
struct Umbrales {
  float media = 0, sd = 0;    // del reposo
  float piso = 0;             // de la calibración pasiva (0 = no se hizo)
  float k = 3.0f;             // sensibilidad (control deslizable de la página)
  bool  listo = false;        // hay calibración de reposo

  float sdEfectiva() const {
    float s = sd, m = SD_MIN_RELATIVA * media;
    if (s < m) s = m;
    if (s < SD_MIN_ABSOLUTA) s = SD_MIN_ABSOLUTA;
    return s;
  }
  float alto() const {
    float a = media + k * sdEfectiva();
    return a > piso ? a : piso;
  }
  float bajo() const { return media + 0.5f * (alto() - media); }
};

// ---------------------------------------------------------------------------
enum Evento { NADA, INICIO, FIN };

class Detector {
 public:
  bool activo = false;
  uint32_t tInicio = 0, tFin = 0;           // ms
  float pico = 0;

  void reiniciar() { activo = false; sobre = 0; }
  float media() const { return n ? suma / n : 0; }
  float duracionS() const { return (tFin - tInicio) / 1000.0f; }

  Evento procesar(float x, uint32_t t, float alto, float bajo) {
    if (!activo) {
      if (x <= alto) { sobre = 0; return NADA; }
      if (sobre == 0) { tPrimero = t; sumaPend = 0; picoPend = 0; }
      sobre++;
      sumaPend += x;
      if (x > picoPend) picoPend = x;
      if (sobre < MUESTRAS_CONFIRMAR) return NADA;
      activo = true;                         // 100 ms sobre el umbral alto: cuenta
      sobre = 0;
      tInicio = tPrimero;
      suma = sumaPend;
      n = MUESTRAS_CONFIRMAR;
      pico = picoPend;
      return INICIO;
    }
    if (x < bajo) { activo = false; tFin = t; return FIN; }   // se rearma
    suma += x;
    n++;
    if (x > pico) pico = x;
    return NADA;
  }

  // Cierra a la fuerza una activación en curso (fin de sesión, sin señal, paro).
  bool cerrar(uint32_t t) {
    sobre = 0;
    if (!activo) return false;
    activo = false;
    tFin = t;
    return true;
  }

 private:
  uint8_t sobre = 0;
  uint32_t tPrimero = 0;
  float sumaPend = 0, picoPend = 0, suma = 0;
  uint32_t n = 0;
};

// ---------------------------------------------------------------------------
struct Act {
  float t0;     // s desde el inicio de la sesión
  float dur;    // s sobre el umbral
  float media;  // RMS medio durante la activación
  float pico;   // RMS máximo
  float resp;   // s desde que empezó la invitación (-1 = no respondía a una)
};

const int MAX_ACTS = 400;                   // ~8 KB; si hay más, se cuentan pero no se guardan

struct Metricas {
  uint32_t n = 0;                           // levantamientos contados
  uint16_t guardadas = 0;                   // activaciones en acts[]
  uint32_t terminadas = 0;                  // activaciones con duración conocida
  uint16_t rondas = 0;
  uint16_t invitaciones = 0, respondidas = 0;
  float sumaResp = 0, sumaDur = 0, sumaMedia = 0;
  MaxSostenido pico;                        // máximo de la sesión (referencia de respaldo)
  Act acts[MAX_ACTS];

  void reiniciar() {
    n = 0; guardadas = 0; terminadas = 0; rondas = 0;
    invitaciones = 0; respondidas = 0;
    sumaResp = sumaDur = sumaMedia = 0;
    pico.reiniciar();
  }
  void agregar(const Act &a) {
    if (guardadas < MAX_ACTS) acts[guardadas++] = a;
    terminadas++;
    sumaDur += a.dur;
    sumaMedia += a.media;
  }
  float durMedia() const { return terminadas ? sumaDur / terminadas : 0; }
  float ampMedia(float ref) const {         // % de la referencia
    return (terminadas && ref > 0) ? 100.0f * (sumaMedia / terminadas) / ref : 0;
  }
  float respMedia() const { return respondidas ? sumaResp / respondidas : -1; }
  float porMinuto(float durS) const { return durS >= 1 ? n * 60.0f / durS : 0; }
};

}  // namespace nucleo
