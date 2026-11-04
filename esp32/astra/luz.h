// luz.h — Tira RGB (toda la tira de un solo color) por el ULN2803, como en v2.3.
// Reglas de seguridad: brillo máximo 50 %, cambios suaves (~0.4 s) y nada de
// destellos (WCAG 2.3.1 permite máx. 3 por segundo; aquí máx. 1 cambio de color
// por segundo, lo decide astra.ino). "Respirar" = un ciclo cada 4 s.
#pragma once

struct Color { uint8_t r, g, b; };

const uint8_t BRILLO_MAX = 128;                  // 50 % (consumo de corriente)
const Color APAGADA      = {0, 0, 0};
const Color BLANCO_TENUE = {12, 12, 12};         // en espera
const Color AZUL_TENUE   = {0, 0, 30};           // base de la terapia
// Un color nuevo en cada levantamiento, en este orden (nunca se repite seguido)
const Color COLORES[]    = {{128, 0, 0},         // rojo
                            {0, 128, 0},         // verde
                            {128, 40, 0},        // naranja
                            {80, 0, 128},        // morado
                            {128, 90, 0}};       // amarillo
const int N_COLORES = 5;
const unsigned long PERIODO_RESPIRA_MS = 4000;   // invitación: azul que respira
const float PASO_LUZ = 6;                        // por cada 20 ms: 0 -> 128 en ~0.4 s

Color luzObjetivo = APAGADA;
bool  luzRespirando = false;
unsigned long luzT0 = 0, luzUltima = 0;
float luzR = 0, luzG = 0, luzB = 0;

void luzIniciar() {
  analogWrite(PIN_ROJO, 0);
  analogWrite(PIN_VERDE, 0);
  analogWrite(PIN_AZUL, 0);
}

void luzFija(Color c) { luzObjetivo = c; luzRespirando = false; }

void luzRespira() {
  if (luzRespirando) return;
  luzRespirando = true;
  luzT0 = millis();
}

// Llamar en cada vuelta del loop: avanza la transición cada 20 ms.
void luzActualizar() {
  unsigned long ahora = millis();
  if (ahora - luzUltima < 20) return;
  luzUltima = ahora;

  float r = luzObjetivo.r, g = luzObjetivo.g, b = luzObjetivo.b;
  if (luzRespirando) {                           // 8 -> 90 -> 8 de azul en 4 s (seno)
    float fase = (float)((ahora - luzT0) % PERIODO_RESPIRA_MS) / PERIODO_RESPIRA_MS;
    r = 0; g = 0; b = 8 + 82 * (0.5f - 0.5f * cosf(2 * PI * fase));
  }
  if (!USAR_LUZ) r = g = b = 0;

  luzR += constrain(r - luzR, -PASO_LUZ, PASO_LUZ);
  luzG += constrain(g - luzG, -PASO_LUZ, PASO_LUZ);
  luzB += constrain(b - luzB, -PASO_LUZ, PASO_LUZ);
  analogWrite(PIN_ROJO,  (int)min(luzR, (float)BRILLO_MAX));
  analogWrite(PIN_VERDE, (int)min(luzG, (float)BRILLO_MAX));
  analogWrite(PIN_AZUL,  (int)min(luzB, (float)BRILLO_MAX));
}
