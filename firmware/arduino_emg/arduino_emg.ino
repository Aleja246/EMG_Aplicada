/*
  =====================================================================
   ASTRA - Arduino UNO R4 WiFi (lado del nino): arduino_emg.ino  v4.1
   Mide la senal de Nova, manda el RMS al ESP32 por el opto (cada 100 ms)
   y la senal CRUDA a la pagina web por WiFi.
  ---------------------------------------------------------------------
   Lee A0 (JP3 puenteado) 1000 veces por segundo y, cada 100 ms, calcula
   tres RMS (en voltios, a la salida EMG_Out):
     Bruto   : senal sin DC, sin filtrar
     Sin60   : quitando solo 60 Hz
     SinArm  : quitando 60, 120, 180, 240, 300, 360 y 420 Hz (red y armonicos)

   Salidas:
     - Opto (Serial1, pin D1, 9600 baud): solo SinArm en milivoltios, una
       linea de texto por bloque ("640\n"). Es lo que lee el ESP32.
       Mismo procesamiento y mismo formato que v2.
     - WiFi: el Arduino crea la red "ASTRA" (clave RED_CLAVE) y atiende en
       http://192.168.4.1 :
         /        -> texto con el estado (sirve para probar desde el navegador)
         /stream  -> muestras CRUDAS del ADC (1000 por segundo), sin fin,
                     en paquetes binarios de 20 muestras (20 ms):
                       0xAA 0x55 | seq | n | n x uint16 little-endian | checksum
                     checksum = XOR de seq, n y los bytes de las muestras;
                     seq = numero de paquete (0..255): la pagina ve los huecos.
       La pagina filtra (pasa altas, 60 Hz y armonicos) y dibuja la senal.
       El ESP32 del atril se une a esta misma red.
     - USB (Serial, 250000 baud), segun USB_PARA_PAGINA:
         false -> "Bruto:x,Sin60:y,SinArm:z" para el Serial Plotter (como v2).
         true  -> los mismos paquetes crudos de /stream (pagina por Web Serial).
       Solo se escribe si hay una computadora abierta en el puerto, y nunca
       se espera: si no cabe, se descarta.

   Como leer el Serial Plotter (en reposo):
     Bruto mucho mayor que Sin60   -> hay mucho 60 Hz
     Sin60 mucho mayor que SinArm  -> los armonicos de la red dominan
     SinArm sigue alto (> ~0.05 V) -> ruido de banda ancha: electrodos, cables o la placa
   Al contraer, SinArm debe subir claramente.

   Regla de diseno: el Arduino solo mide y envia; nunca controla un actuador.

   Cambios v4.1 respecto a v4:
     - El Serial Plotter vuelve a graficar: v4 solo escribia si
       Serial.availableForWrite() decia que habia lugar, y en el UNO R4 eso
       no sirve (devuelve 0). Ahora escribe si hay una computadora abierta.
     - http://192.168.4.1 muestra un diagnostico: ultima muestra del ADC,
       minimo y maximo del ultimo bloque (en cuentas y voltios), si la senal
       esta saturada, el ultimo RMS mandado al ESP32 y la frecuencia de
       muestreo medida.

   Cambios v4 respecto a v2 (mismo procesamiento y mismo envio al ESP32):
     - El muestreo de 1 kHz corre en una interrupcion de temporizador
       (FspTimer). El WiFi del R4 hace esperas de varios ms en cada envio;
       asi nunca retrasa una muestra.
     - Los RMS de cada bloque pasan por una cola de 6.4 s y el loop los
       manda al opto como linea completa. Si el WiFi tarda, la linea sale
       un poco despues pero completa, y ningun bloque se pierde (el ESP32
       calibra con el "reloj de las muestras": cuenta bloques, no tiempo).
     - WiFi: punto de acceso "ASTRA" + /stream con la senal cruda.
     - El USB nunca bloquea (solo escribe lo que cabe).

   SEGURIDAD: con WiFi no hace falta ningun cable entre el nino y la
   laptop; el opto sigue aislando las tierras del nino y del atril. Si
   usas USB_PARA_PAGINA, la laptop NO debe estar cargando mientras el USB
   del Arduino toca a un nino con electrodos.

   Placa: "Arduino UNO R4 WiFi". Bibliotecas: WiFiS3 (viene con el paquete
   de placas UNO R4). Si el WiFi no arranca, actualiza el firmware del
   modulo (Herramientas > Firmware Updater en el IDE 2).
  =====================================================================
*/
#include <Arduino.h>
#include <math.h>
#include <WiFiS3.h>
#include "FspTimer.h"

// ------------------------- Parametros ajustables -----------------------
const int   PIN_EMG  = A0;
const float FS       = 1000.0f;   // Hz. Si cambia, los notch se recalculan solos (y cambia "Frecuencia" en la pagina)
const int   N_BLOQUE = 100;       // muestras por RMS (100 = 100 ms = cada cuanto llega un dato al ESP32)
const float ALPHA_DC = 0.003f;    // que tan lento sigue el centro de la senal (~0.5 Hz)
const int   N_ARM    = 7;         // notch en 60, 120, ... 420 Hz
const float Q_NOTCH  = 20.0f;     // ancho de cada notch = f / Q (3 Hz a 60 Hz)

const float V_REF = 5.0f, ADC_MAX = 16383.0f;     // 14 bits, referencia de 5 V

const unsigned long BAUD_USB  = 250000;
const unsigned long BAUD_OPTO = 9600;
const int BLOQUES_ASENTAR     = 5;     // primeros 5 bloques (0.5 s) no van al ESP32
const int PIN_MUESTREO        = -1;    // pon p. ej. 7 para ver cada muestra en el osciloscopio

// ------------------------- WiFi y pagina -------------------------------
// RED_SSID / RED_CLAVE deben ser iguales en esp32_calibracion.ino.
const bool USAR_WIFI       = true;
const char RED_SSID[]      = "ASTRA";
const char RED_CLAVE[]     = "astra2026";      // minimo 8 caracteres
const IPAddress IP_ARDUINO(192, 168, 4, 1);    // "IP del Arduino" en los Ajustes de la pagina
const bool USB_PARA_PAGINA = false;            // true: paquetes crudos por USB en vez del Serial Plotter
const char VERSION_EMG[]   = "4.1";

// ------------------------- Notch biquad (forma directa II transpuesta) --
struct Notch {
  float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  void init(float f, float Q) {
    float w0 = 2.0f * PI * f / FS, c = cosf(w0), al = sinf(w0) / (2.0f * Q), a0 = 1.0f + al;
    b0 = 1.0f / a0; b1 = -2.0f * c / a0; b2 = 1.0f / a0; a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0;
  }
  float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

// ---- Todo esto lo usa SOLO la interrupcion de muestreo ----
Notch n60, arm[N_ARM];
int nArm = 0;                       // cuantos armonicos caben debajo de FS/2
float dc = 2.5f;
double s_bruto = 0, s_60 = 0, s_arm = 0;   // RMS en bloques de N_BLOQUE muestras
int cuenta = 0;
int bloquesHechos = 0;

// ---- Muestras crudas: la interrupcion escribe, el loop lee ----
const uint32_t N_CRUDO = 2048;      // 2 s de reserva (potencia de 2)
volatile uint16_t crudo[N_CRUDO];
volatile uint32_t nCrudo = 0;       // muestras escritas desde el arranque

// ---- Cola de bloques RMS (6.4 s): la interrupcion escribe, el loop lee ----
const uint32_t N_COLA = 64;
volatile float colaBruto[N_COLA], cola60[N_COLA], colaArm[N_COLA];
volatile bool colaOpto[N_COLA];
volatile uint32_t colaEsc = 0, colaLei = 0;
volatile uint32_t bloquesPerdidos = 0;

// ---- Diagnostico (lo muestra http://192.168.4.1) ----
volatile uint16_t ultimoAdc = 0;
volatile uint16_t minBloque = 0, maxBloque = 0;   // del ultimo bloque completo
uint16_t minAcum = 65535, maxAcum = 0;            // del bloque en curso (solo la interrupcion)
volatile float ultimoRmsMv = 0;
uint32_t tInicioMuestreo = 0;

// ---- Paquetes para la pagina ----
const int N_PAQ = 20;                         // muestras por paquete (20 ms)
const int LARGO_PAQ = 4 + 2 * N_PAQ + 1;      // 45 bytes
const int MAX_PAQ_WIFI = 10;                  // hasta 200 ms por envio
uint8_t bufWifi[LARGO_PAQ * MAX_PAQ_WIFI];
uint8_t bufUsb[LARGO_PAQ];

FspTimer temporizador;
bool wifiListo = false;
WiFiServer servidor(80);
WiFiClient flujo;
bool transmitiendo = false;
uint32_t sigWifi = 0, sigUsb = 0;             // siguiente muestra a mandar
uint32_t tRevisar = 0, tEnvio = 0, tVivo = 0;

// ======================================================================
//  Muestreo (interrupcion a FS). Mismo procesamiento que v2.
// ======================================================================
void alMuestrear(timer_callback_args_t *) {
  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, HIGH);

  int cuentaAdc = analogRead(PIN_EMG);
  ultimoAdc = (uint16_t)cuentaAdc;
  if (cuentaAdc < minAcum) minAcum = cuentaAdc;
  if (cuentaAdc > maxAcum) maxAcum = cuentaAdc;
  crudo[nCrudo & (N_CRUDO - 1)] = (uint16_t)cuentaAdc;
  nCrudo = nCrudo + 1;

  float v = cuentaAdc * V_REF / ADC_MAX;
  dc += ALPHA_DC * (v - dc);
  float x = v - dc;

  float y60 = n60.run(x);
  float ya = x;
  for (int k = 0; k < nArm; k++) ya = arm[k].run(ya);

  s_bruto += (double)x * x;
  s_60    += (double)y60 * y60;
  s_arm   += (double)ya * ya;

  if (++cuenta >= N_BLOQUE) {
    minBloque = minAcum; maxBloque = maxAcum; minAcum = 65535; maxAcum = 0;
    if (bloquesHechos <= BLOQUES_ASENTAR) bloquesHechos++;
    if (colaEsc - colaLei < N_COLA) {
      uint32_t i = colaEsc % N_COLA;
      colaBruto[i] = sqrt(s_bruto / N_BLOQUE);
      cola60[i]    = sqrt(s_60 / N_BLOQUE);
      colaArm[i]   = sqrt(s_arm / N_BLOQUE);
      colaOpto[i]  = bloquesHechos > BLOQUES_ASENTAR;
      ultimoRmsMv  = colaArm[i] * 1000.0f;
      colaEsc = colaEsc + 1;
    } else {
      bloquesPerdidos = bloquesPerdidos + 1;   // el loop lleva > 6.4 s sin vaciar la cola
    }
    s_bruto = s_60 = s_arm = 0;
    cuenta = 0;
  }

  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, LOW);
}

bool iniciarTemporizador() {
  uint8_t tipo = GPT_TIMER;
  int8_t canal = FspTimer::get_available_timer(tipo);
  if (canal < 0) canal = FspTimer::get_available_timer(tipo, true);   // uno reservado para PWM (no usamos PWM)
  if (canal < 0) { tipo = AGT_TIMER; canal = FspTimer::get_available_timer(tipo); }
  if (canal < 0) return false;
  if (!temporizador.begin(TIMER_MODE_PERIODIC, tipo, canal, FS, 0.0f, alMuestrear)) return false;
  if (!temporizador.setup_overflow_irq()) return false;
  if (!temporizador.open()) return false;
  return temporizador.start();
}

// ======================================================================
//  Opto (ESP32) y Serial Plotter: una linea por bloque
// ======================================================================
// Escribe "1.234" (voltios con 3 decimales) usando enteros
int ponerVoltios(char *dst, int cap, const char *etiqueta, float v) {
  long mv = lroundf(v * 1000.0f);
  if (mv < 0) mv = 0;
  return snprintf(dst, cap, "%s:%ld.%03ld", etiqueta, mv / 1000, mv % 1000);
}

void vaciarCola() {
  while (colaLei != colaEsc) {
    uint32_t i = colaLei % N_COLA;
    float rmsBruto = colaBruto[i], rms60 = cola60[i], rmsArm = colaArm[i];
    bool alOpto = colaOpto[i];
    colaLei = colaLei + 1;

    // Opto: solo SinArm en mV, de 1 a 6 cifras y fin de linea -> "640\n"
    // La linea sale completa (unos 5 ms a 9600 baud): el ESP32 descarta
    // las lineas con huecos de mas de 50 ms.
    if (alOpto) {
      long mv = lroundf(rmsArm * 1000.0f);
      mv = constrain(mv, 0L, 999999L);
      char l[12];
      int n = snprintf(l, sizeof l, "%ld\n", mv);
      Serial1.write((const uint8_t *)l, n);
    }

    // USB: igual que v2 -> Bruto:0.123,Sin60:0.045,SinArm:0.012
    if (!USB_PARA_PAGINA && Serial) {
      char u[64];
      int n = 0;
      n += ponerVoltios(u + n, sizeof(u) - n, "Bruto", rmsBruto);
      u[n++] = ',';
      n += ponerVoltios(u + n, sizeof(u) - n, "Sin60", rms60);
      u[n++] = ',';
      n += ponerVoltios(u + n, sizeof(u) - n, "SinArm", rmsArm);
      u[n++] = '\r';
      u[n++] = '\n';
      Serial.write((const uint8_t *)u, n);     // ~40 bytes cada 100 ms: el muestreo no se entera (va en la interrupcion)
    }
  }
}

// ======================================================================
//  Paquetes de muestras crudas (WiFi y USB)
// ======================================================================
// Arma hasta maxPaq paquetes desde 'sig' (siempre multiplo de N_PAQ).
// Si la pagina se atraso mas de lo que se guarda, salta adelante: el
// numero seq deja ver el hueco.
int armarPaquetes(uint32_t &sig, uint8_t *buf, int maxPaq) {
  uint32_t hay = nCrudo;
  if (hay - sig > N_CRUDO - 4 * N_PAQ) sig = hay - (hay % N_PAQ) - 2 * N_PAQ;
  int len = 0;
  for (int paq = 0; paq < maxPaq && hay - sig >= (uint32_t)N_PAQ; paq++) {
    uint8_t *p = buf + len;
    p[0] = 0xAA;
    p[1] = 0x55;
    p[2] = (uint8_t)(sig / N_PAQ);
    p[3] = N_PAQ;
    uint8_t chk = p[2] ^ p[3];
    for (int i = 0; i < N_PAQ; i++) {
      uint16_t v = crudo[(sig + i) & (N_CRUDO - 1)];
      p[4 + 2 * i] = v & 0xFF;
      p[5 + 2 * i] = v >> 8;
      chk ^= p[4 + 2 * i] ^ p[5 + 2 * i];
    }
    p[LARGO_PAQ - 1] = chk;
    sig += N_PAQ;
    len += LARGO_PAQ;
  }
  return len;
}

// Empieza a mandar desde "ahora" (sin la reserva vieja)
uint32_t inicioAlineado() {
  uint32_t hay = nCrudo;
  return hay - (hay % N_PAQ);
}

void mandarCrudoUsb() {
  if (!Serial) { sigUsb = inicioAlineado(); return; }
  for (int k = 0; k < 4; k++) {                 // hasta 4 paquetes (80 ms) por vuelta
    int len = armarPaquetes(sigUsb, bufUsb, 1);
    if (!len) break;
    Serial.write(bufUsb, len);
  }
}

// ======================================================================
//  WiFi: punto de acceso + servidor web
// ======================================================================
void iniciarWifi() {
  if (WiFi.status() == WL_NO_MODULE) {
    if (!USB_PARA_PAGINA) Serial.println("!! No responde el modulo WiFi del R4.");
    return;
  }
  WiFi.config(IP_ARDUINO);
  if (WiFi.beginAP(RED_SSID, RED_CLAVE) != WL_AP_LISTENING) {
    if (!USB_PARA_PAGINA) Serial.println("!! No se pudo crear la red WiFi.");
    return;
  }
  servidor.begin();
  wifiListo = true;
  if (!USB_PARA_PAGINA) {
    Serial.print(">> Red WiFi ");
    Serial.print(RED_SSID);
    Serial.print(" lista. Senal en http://");
    Serial.print(IP_ARDUINO);
    Serial.println("/stream");
  }
}

void cerrarFlujo() {
  flujo.stop();
  transmitiendo = false;
}

const char CORS[] =
  "Access-Control-Allow-Origin: *\r\n"
  "Access-Control-Allow-Private-Network: true\r\n"
  "Cache-Control: no-store\r\n"
  "Connection: close\r\n";

void responderTexto(WiFiClient &c, const char *estado, const char *texto) {
  char h[384];
  int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\n%sContent-Length: %d\r\n\r\n",
                   estado, CORS, (int)strlen(texto));
  c.write((const uint8_t *)h, n);
  if (*texto) c.write((const uint8_t *)texto, strlen(texto));
  c.stop();
}

// Lee la primera linea de la peticion ("GET /stream HTTP/1.1") y responde
void atenderPeticion(WiFiClient &c) {
  char l[96];
  int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 400 && n < (int)sizeof(l) - 1) {
    if (!c.available()) { if (!c.connected()) break; continue; }
    int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') break;
    if (ch != '\r') l[n++] = (char)ch;
  }
  l[n] = 0;
  uint8_t resto[64];                            // el resto de la peticion (cabeceras) no se usa
  for (uint32_t t1 = millis(); c.available() && millis() - t1 < 100; ) c.read(resto, sizeof resto);

  if (!strncmp(l, "OPTIONS ", 8)) {             // permiso previo del navegador
    char h[320];
    int k = snprintf(h, sizeof h, "HTTP/1.1 204 No Content\r\n%sAccess-Control-Allow-Methods: GET\r\n"
                     "Access-Control-Allow-Headers: *\r\nContent-Length: 0\r\n\r\n", CORS);
    c.write((const uint8_t *)h, k);
    c.stop();
  } else if (!strncmp(l, "GET /stream", 11)) {
    if (transmitiendo) cerrarFlujo();           // solo una pagina a la vez: la nueva reemplaza
    char h[320];
    int k = snprintf(h, sizeof h, "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "X-Content-Type-Options: nosniff\r\n%s\r\n", CORS);
    c.write((const uint8_t *)h, k);
    flujo = c;
    transmitiendo = true;
    sigWifi = inicioAlineado();
    tVivo = millis();
  } else if (!strncmp(l, "GET ", 4)) {
    char t[640];
    uint16_t mn = minBloque, mx = maxBloque, ua = ultimoAdc;
    float seg = (millis() - tInicioMuestreo) / 1000.0f;
    const char *estado = (mn <= ADC_MAX * 0.01f || mx >= ADC_MAX * 0.99f) ? "SATURADA (pegada a 0 V o a 5 V): revisa electrodos, referencia y cable de Nova"
                       : (mx - mn < 3) ? "PLANA (no cambia): Nova sin alimentacion o cable de senal suelto" : "bien (centro ~2.5 V)";
    snprintf(t, sizeof t, "ASTRA - Arduino EMG v%s\nMuestreo: %d Hz (medido: %.1f Hz)\nMuestras: %lu\nBloques perdidos: %lu\n"
             "Pagina conectada: %s\nLa senal cruda sale en /stream\n\n"
             "Diagnostico de la senal (A0):\n  ultima muestra: %u cuentas = %.3f V\n"
             "  ultimo bloque (100 ms): min %u (%.3f V), max %u (%.3f V)\n  senal: %s\n"
             "  ultimo RMS al ESP32: %.0f mV\n(recarga la pagina para actualizar)\n",
             VERSION_EMG, (int)FS, seg > 0 ? nCrudo / seg : 0.0f, (unsigned long)nCrudo, (unsigned long)bloquesPerdidos,
             transmitiendo ? "si" : "no", ua, ua * V_REF / ADC_MAX, mn, mn * V_REF / ADC_MAX, mx, mx * V_REF / ADC_MAX, estado,
             (float)ultimoRmsMv);
    responderTexto(c, "200 OK", t);
  } else {
    responderTexto(c, "400 Bad Request", "");
  }
}

void atenderWifi() {
  uint32_t ahora = millis();
  if (ahora - tRevisar >= 100) {                // nuevas peticiones, 10 veces por segundo
    tRevisar = ahora;
    WiFiClient nuevo = servidor.available();
    if (nuevo && !(transmitiendo && nuevo == flujo)) atenderPeticion(nuevo);
  }
  if (transmitiendo && ahora - tEnvio >= 40) {  // la senal sale en tandas de ~40 ms
    tEnvio = ahora;
    int len = armarPaquetes(sigWifi, bufWifi, MAX_PAQ_WIFI);
    if (len && flujo.write(bufWifi, len) != (size_t)len) cerrarFlujo();   // la pagina se fue
  }
  if (transmitiendo && ahora - tVivo >= 1000) {
    tVivo = ahora;
    if (!flujo.connected()) cerrarFlujo();
  }
}

// ======================================================================
void setup() {
  Serial.begin(BAUD_USB);
  Serial1.begin(BAUD_OPTO);          // enlace al ESP32 por el opto (pin D1)
  analogReadResolution(14);
  if (PIN_MUESTREO >= 0) pinMode(PIN_MUESTREO, OUTPUT);

  n60.init(60.0f, Q_NOTCH);
  for (int k = 0; k < N_ARM; k++) {
    float f = 60.0f * (k + 1);
    if (f < 0.5f * FS) arm[nArm++].init(f, Q_NOTCH);
  }

  // El centro arranca donde esta la senal de verdad (TL431, ~2.5 V)
  float s = 0;
  for (int i = 0; i < 16; i++) s += analogRead(PIN_EMG) * V_REF / ADC_MAX;
  dc = s / 16.0f;

  tInicioMuestreo = millis();
  if (!iniciarTemporizador()) {
    // Sin temporizador no hay muestreo fiable: lo avisa y parpadea el LED
    pinMode(LED_BUILTIN, OUTPUT);
    for (;;) {
      Serial.println("!! No hay temporizador libre para el muestreo.");
      digitalWrite(LED_BUILTIN, HIGH); delay(200);
      digitalWrite(LED_BUILTIN, LOW);  delay(800);
    }
  }

  // El muestreo ya corre; crear la red tarda unos segundos (la cola guarda los bloques)
  if (USAR_WIFI) iniciarWifi();
}

void loop() {
  vaciarCola();
  if (USB_PARA_PAGINA) mandarCrudoUsb();
  if (wifiListo) atenderWifi();
}
