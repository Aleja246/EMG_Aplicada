// motores.h — Estrellas (suben por escalones) y planetas (lazy susan), 28BYJ-48 + ULN2003.
// Igual que v2.3: AccelStepper en medio paso, orden de bobinas IN1, IN3, IN2, IN4,
// microswitch en el TOPE, fijarTope() sin tirón y bobinas apagadas en reposo.
//
// Coordenadas: "cero" = estrellas ABAJO. Cada levantamiento suma un escalón (8 cm) al
// objetivo y el motor lo alcanza a su ritmo. 5 escalones = 40 cm = tope (microswitch).
// Al encender (y al reanudar tras el paro) suben hasta el microswitch para conocer su
// posición y luego bajan a cero.
#pragma once
#include <AccelStepper.h>

void avisar(const char *fmt, ...);               // en astra.ino

// Pines: planetas IO23, 22, 21, 19 (J4); estrellas IO18, 17, 16, 4 (J3).
// Si un motor VIBRA en vez de girar, cambia su ORDEN_B a true.
const bool ORDEN_B_PLANETAS  = false;
const bool ORDEN_B_ESTRELLAS = false;
AccelStepper planetas(AccelStepper::HALF4WIRE, 23, ORDEN_B_PLANETAS ? 22 : 21, ORDEN_B_PLANETAS ? 21 : 22, 19, false);
AccelStepper estrellas(AccelStepper::HALF4WIRE, 18, ORDEN_B_ESTRELLAS ? 17 : 16, ORDEN_B_ESTRELLAS ? 16 : 17, 4, false);

const bool  USAR_TOPE    = true;     // false si no hay microswitch: enciende con las estrellas ABAJO
const int   DIR_SUBIR    = 1;        // si al buscar el tope las estrellas BAJAN, cambia a -1
const float CARRETE_CM   = 3.0f;     // diámetro del carrete
const float ESCALON_CM   = 8.0f;
// Pasos por escalón = 8 cm / (pi x diámetro) x 4096 (medio paso) = 3477
const long  PASOS_ESCALON = (long)(ESCALON_CM / (3.14159265f * CARRETE_CM) * 4096.0f + 0.5f);
const long  RECORRIDO     = PASOS_ESCALON * N_ESCALONES;   // fondo -> tope (40 cm)
const long  POS_TOPE      = 0;
const int   V_HOMING      = 400;     // pasos/s buscando el tope
const int   V_ESTRELLAS   = 700;     // pasos/s (~10 rpm): un escalón tarda ~5 s
const int   ACELERACION   = 800;     // pasos/s^2
const int   V_PLANETAS    = 120;     // pasos/s: giran lento
const int   ACEL_PLANETAS = 200;     // pasos/s^2: arranque suave
const long  TOPE_SUELTA   = 1000;    // al bajar del tope el microswitch debe soltarse antes de esto

enum EstadoEstrellas { EST_BUSCANDO, EST_LISTAS, EST_SIN_TOPE, EST_APAGADAS };
const char *NOMBRE_EST[] = {"BUSCANDO_CERO", "OK", "SIN_TOPE", "APAGADAS"};
EstadoEstrellas estE = EST_BUSCANDO;
int  escalonObjetivo = 0;
bool estrellasEnPausa = false;       // sin señal o paro: no se mueven
bool bobinasEstrellas = false, bobinasPlanetas = false;
long pasosHoming = 0;
float velPlanetas = 0;
unsigned long tRampaP = 0;

long posDeEscalon(int e) { return DIR_SUBIR * ((long)e * PASOS_ESCALON - RECORRIDO); }

bool topePresionado() {
  if (digitalRead(PIN_TOPE) == HIGH) return false;
  delayMicroseconds(300);
  return digitalRead(PIN_TOPE) == LOW;
}

void bobinasE(bool on) {
  if (on == bobinasEstrellas) return;
  if (on) estrellas.enableOutputs(); else estrellas.disableOutputs();
  bobinasEstrellas = on;
}

void bobinasP(bool on) {
  if (on == bobinasPlanetas) return;
  if (on) planetas.enableOutputs(); else planetas.disableOutputs();
  bobinasPlanetas = on;
}

// El microswitch marca el tope. La posición pasa a ~0 conservando la fase de las
// bobinas (0 a 7 medios pasos, < 0.2 mm): sin tirón y sin empujar el microswitch.
void fijarTope() {
  long fase = estrellas.currentPosition() & 7;
  long p = (DIR_SUBIR > 0) ? fase : (fase ? fase - 8 : 0);
  estrellas.setCurrentPosition(POS_TOPE + p);    // también detiene el motor ahí mismo
}

void motoresIniciar() {
  estrellas.enableOutputs(); estrellas.disableOutputs();   // bobinas apagadas desde el inicio
  planetas.enableOutputs();  planetas.disableOutputs();
  estrellas.setMaxSpeed(V_ESTRELLAS);
  estrellas.setAcceleration(ACELERACION);
  planetas.setMaxSpeed(V_PLANETAS * 2);
}

// Sube hasta el microswitch y luego baja al escalón objetivo.
void estrellasBuscarCero() {
  estrellasEnPausa = false;
  if (!USAR_ESTRELLAS) { estE = EST_APAGADAS; return; }
  estE = EST_BUSCANDO;
  pasosHoming = 0;
  bobinasE(true);
  estrellas.setMaxSpeed(V_ESTRELLAS > V_HOMING ? V_ESTRELLAS : V_HOMING);  // si no, setSpeed lo recorta
  estrellas.setSpeed(DIR_SUBIR * V_HOMING);
  Serial.println(">> Estrellas: buscando el tope (microswitch) para saber dónde está el cero.");
}

void estrellasIrA(int e) {
  escalonObjetivo = constrain(e, 0, N_ESCALONES);
  if (estE == EST_LISTAS && !estrellasEnPausa) estrellas.moveTo(posDeEscalon(escalonObjetivo));
}

void estrellasPausar() {                         // sin señal: frenan suave y esperan
  estrellasEnPausa = true;
  if (estE == EST_LISTAS) estrellas.stop();
}

void estrellasSeguir() {
  estrellasEnPausa = false;
  if (estE == EST_LISTAS) estrellas.moveTo(posDeEscalon(escalonObjetivo));
}

void estrellasDetenerYa() {                      // paro: alto inmediato, bobinas apagadas
  estrellasEnPausa = true;
  estrellas.setCurrentPosition(estrellas.currentPosition());
  bobinasE(false);
}

// ¿Ya llegaron al escalón objetivo? (si están apagadas o sin tope, cuenta como sí)
bool estrellasEnObjetivo() {
  if (estE == EST_BUSCANDO) return false;
  if (estE != EST_LISTAS) return true;
  return estrellas.distanceToGo() == 0 &&
         labs(estrellas.currentPosition() - posDeEscalon(escalonObjetivo)) <= 8;
}

// Llamar en cada vuelta del loop (nunca bloquea).
void estrellasActualizar() {
  switch (estE) {
    case EST_APAGADAS:
    case EST_SIN_TOPE:
      return;
    case EST_BUSCANDO:
      if (estrellasEnPausa) return;
      if (!USAR_TOPE || topePresionado()) {
        if (USAR_TOPE) fijarTope();
        else estrellas.setCurrentPosition(posDeEscalon(0));   // arrancó abajo
        estE = EST_LISTAS;
        estrellas.setMaxSpeed(V_ESTRELLAS);
        estrellas.setAcceleration(ACELERACION);
        estrellas.moveTo(posDeEscalon(escalonObjetivo));
        avisar("Estrellas listas: van al escalón %d.", escalonObjetivo);
      } else if (estrellas.runSpeed() && ++pasosHoming > RECORRIDO * 13 / 10) {
        bobinasE(false);
        estE = EST_SIN_TOPE;
        avisar("No encontré el tope: estrellas desactivadas (revisa microswitch y DIR_SUBIR).");
      }
      return;
    case EST_LISTAS:
      break;
  }
  if (estrellasEnPausa) {                        // sin señal: terminan de frenar y esperan
    if (estrellas.distanceToGo() != 0) { bobinasE(true); estrellas.run(); }
    else bobinasE(false);
    return;
  }

  long objetivo = posDeEscalon(escalonObjetivo);
  if (USAR_TOPE) {
    bool subiendo = estrellas.speed() * DIR_SUBIR > 0;
    if (subiendo && topePresionado()) {
      fijarTope();                               // tope real: resincroniza
    } else if (!subiendo && labs(estrellas.currentPosition() - POS_TOPE) > TOPE_SUELTA && topePresionado()) {
      estrellasDetenerYa();                      // lejos del tope no puede estar presionado
      estE = EST_SIN_TOPE;
      avisar("El microswitch sigue presionado lejos del tope (¿NC o atorado?). Estrellas desactivadas.");
      return;
    }
  }
  // Si quedaron quietas fuera del objetivo (p. ej. tras fijarTope), vuelven a ir.
  if (estrellas.distanceToGo() == 0 && labs(estrellas.currentPosition() - objetivo) > 8)
    estrellas.moveTo(objetivo);
  if (estrellas.distanceToGo() != 0) { bobinasE(true); estrellas.run(); }
  else bobinasE(false);
}

// Planetas: giran lento y no paran mientras 'girar' sea true.
void planetasActualizar(bool girar) {
  if (!girar || !USAR_PLANETAS) {
    bobinasP(false);
    velPlanetas = 0;                             // al volver, arranca suave otra vez
    return;
  }
  bobinasP(true);
  unsigned long ahora = millis();
  if (velPlanetas < V_PLANETAS && ahora - tRampaP >= 20) {
    tRampaP = ahora;
    velPlanetas = min(velPlanetas + ACEL_PLANETAS * 0.02f, (float)V_PLANETAS);
    planetas.setSpeed(velPlanetas);
  }
  planetas.runSpeed();
}
