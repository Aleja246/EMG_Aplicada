// ASTRA - prueba SOLO de la tira LED RGB (ESP32), sin motores, WiFi ni sonido.
// Placa: "ESP32 Dev Module". Monitor serie a 115200.
//
// Pines (los mismos del atril):  ROJO IO25, VERDE IO33, AZUL IO32  -> ULN2803.
//
// Que hace:
//   1) Cada color por separado, 3 s: ROJO, VERDE, AZUL (y 2 s apagada entre ellos).
//   2) Mezclas: amarillo (R+G), magenta (R+B), cian (G+B) y blanco (R+G+B).
//   3) Degradado lento de cada color (sube y baja) para ver si el PWM funciona.
//   Luego repite. Si un color NO enciende, mira la tabla del final.
//
// Teclas en el monitor serie (sin ajuste de linea):
//   r = solo rojo   g = solo verde   b = solo azul   w = blanco   o = apagar
//   a = volver a la secuencia automatica

const int PIN_ROJO  = 25;
const int PIN_VERDE = 33;
const int PIN_AZUL  = 32;
const int BRILLO    = 128;   // 0..255 (el atril usa maximo 128 = 50 %)

bool automatico = true;

void color(int r, int g, int b) {
  analogWrite(PIN_ROJO, r);
  analogWrite(PIN_VERDE, g);
  analogWrite(PIN_AZUL, b);
}

void esperar(unsigned long ms) {          // espera, pero atiende teclas
  unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    if (Serial.available()) return;
    delay(10);
  }
}

void degradado(int canal, const char *nombre) {
  Serial.printf("Degradado de %s (sube y baja, 2 s cada tramo)\n", nombre);
  for (int sube = 0; sube < 2; sube++) {
    for (int i = 0; i <= BRILLO; i++) {
      int v = sube ? BRILLO - i : i;
      color(canal == 0 ? v : 0, canal == 1 ? v : 0, canal == 2 ? v : 0);
      delay(2000 / BRILLO);
      if (Serial.available()) return;
    }
  }
}

void secuencia() {
  Serial.println("\n--- COLORES SOLOS (3 s cada uno) ---");
  Serial.println("ROJO  (IO25)");  color(BRILLO, 0, 0); esperar(3000); if (Serial.available()) return;
  color(0, 0, 0); esperar(1000);
  Serial.println("VERDE (IO33)");  color(0, BRILLO, 0); esperar(3000); if (Serial.available()) return;
  color(0, 0, 0); esperar(1000);
  Serial.println("AZUL  (IO32)");  color(0, 0, BRILLO); esperar(3000); if (Serial.available()) return;
  color(0, 0, 0); esperar(1000);

  Serial.println("\n--- MEZCLAS (3 s cada una) ---");
  Serial.println("AMARILLO (rojo + verde)"); color(BRILLO, BRILLO, 0); esperar(3000); if (Serial.available()) return;
  Serial.println("MAGENTA  (rojo + azul)");  color(BRILLO, 0, BRILLO); esperar(3000); if (Serial.available()) return;
  Serial.println("CIAN     (verde + azul)"); color(0, BRILLO, BRILLO); esperar(3000); if (Serial.available()) return;
  Serial.println("BLANCO   (los tres)");     color(BRILLO, BRILLO, BRILLO); esperar(3000); if (Serial.available()) return;
  color(0, 0, 0); esperar(1000);

  Serial.println("\n--- DEGRADADOS ---");
  degradado(0, "ROJO");  if (Serial.available()) return;
  degradado(1, "VERDE"); if (Serial.available()) return;
  degradado(2, "AZUL");
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_ROJO, OUTPUT);
  pinMode(PIN_VERDE, OUTPUT);
  pinMode(PIN_AZUL, OUTPUT);
  color(0, 0, 0);
  delay(500);
  Serial.println("\n=== ASTRA - prueba de la tira LED RGB ===");
  Serial.println("Teclas: r g b = un color | w = blanco | o = apagar | a = secuencia automatica");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    while (Serial.available()) Serial.read();
    if (c == 'a') { automatico = true; Serial.println(">> Secuencia automatica."); }
    else if (c == 'r') { automatico = false; color(BRILLO, 0, 0); Serial.println(">> Solo ROJO (IO25)"); }
    else if (c == 'g') { automatico = false; color(0, BRILLO, 0); Serial.println(">> Solo VERDE (IO33)"); }
    else if (c == 'b') { automatico = false; color(0, 0, BRILLO); Serial.println(">> Solo AZUL (IO32)"); }
    else if (c == 'w') { automatico = false; color(BRILLO, BRILLO, BRILLO); Serial.println(">> BLANCO"); }
    else if (c == 'o') { automatico = false; color(0, 0, 0); Serial.println(">> Apagada"); }
  }
  if (automatico) secuencia();
  else delay(20);
}

/*
  COMO LEER EL RESULTADO
  ----------------------
  Todo bien: rojo, verde y azul enciende cada uno; amarillo = rojo + verde, blanco con los tres.

  El ROJO no enciende (ni solo ni en mezclas: no hay amarillo ni magenta, y el blanco sale cian):
    1) Cable del ESP32 IO25 al ULN2803 (entradas I1 e I2): contacto flojo o cable roto.
    2) Salidas O1 y O2 del ULN2803 -> cable R de la tira. Mide continuidad.
    3) Cable rojo/R de la tira (en los conectores 5050 suele ser el que se rompe).
    4) El ULN2803 mismo: si el canal esta danado, pasa el pin IO25 a otra entrada libre
       (por ejemplo I7 / O7) y cambia PIN_ROJO por ese GPIO libre.
    5) Para saber si falla el ESP32 o el ULN2803, mide con multimetro en CC con la tecla r:
       - En el pin IO25 debe dar ~1.6 V (es un PWM al 50 %). Si da 0 V, falla el ESP32 o su cable.
       - Si IO25 da 1.6 V pero la salida O1 del ULN2803 no baja de 5 V a ~0 V (en promedio ~2.5 V),
         falla el ULN2803.

  El rojo enciende PERO el verde o el azul no: es el mismo analisis con IO33 (verde) o IO32 (azul).

  Se ve rojo SIEMPRE o el color no cambia: cable en cortocircuito a otro canal.
*/
