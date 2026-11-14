/*
  =====================================================================
   ASTRA - Arduino UNO R4 WiFi (lado del nino): arduino_emg_ap.ino  v5.1-AP
  ---------------------------------------------------------------------
   v5.1-AP (senal limpia con el WiFi encendido):
     - Cada envio por WiFi hace que el modulo WiFi del R4 consuma corriente de
       golpe; el ADC mide contra los 5 V de la placa, asi que esas muestras
       salen movidas. Ahora las muestras tomadas MIENTRAS se transmite (y
       TX_COLA_MS despues) se APARTAN del RMS (el del opto y el del Serial
       Plotter). Los filtros siguen recibiendo TODAS las muestras, como antes.
       Sin WiFi no se aparta nada: el RMS es exactamente el de siempre.
     - El envio por WiFi se hace justo DESPUES de mandar la linea al opto
       (10 veces por segundo, antes 25): la siguiente linea tiene ~100 ms de
       margen aunque el modulo WiFi tarde.
     - Si un envio tarda mas de ENVIO_LENTO_MS, la senal en vivo se pausa 3 s
       (el opto nunca se pausa).
     - El atril puede pedir "S" (mandar la senal en vivo) o "N" (no mandarla):
       no se manda si no hay ninguna pagina abierta.
     - En el paquete crudo, el bit 15 marca las muestras apartadas (la pagina
       las sabe distinguir). El diagnostico informa cuantas, el ruido con y sin
       transmision, el envio mas lento y la mayor espera entre lineas del opto.
  ---------------------------------------------------------------------
   VERSION SIN LAPTOP: es el mismo arduino_emg.ino v5, linea por linea.
   Solo cambia a donde se conecta por WiFi:
     - Red WiFi: la que crea el ESP32 del atril (ASTRA_RED / ASTRA_CLAVE,
       en credenciales.h o abajo en "WiFi y servidor de la pagina").
     - Servidor: el propio ESP32, siempre en 192.168.4.1 (puerto 9001).
     - WiFi.setTimeout(0): unirse a la red ya no detiene el loop (antes
       podia esperar hasta 10 s); el modulo WiFi sigue intentando solo.
     - Si no hay red, vuelve a pedirla cada 15 s (antes cada 60 s).
   El muestreo, los filtros, el RMS, el opto al ESP32, el Serial Plotter y
   el formato de los paquetes NO cambian.
   Mide la senal de Nova, manda el RMS al ESP32 por el opto (cada 100 ms)
   y la senal CRUDA al servidor de la pagina por WiFi.
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
     - WiFi: el Arduino se une a la red WiFi del consultorio (o a la zona
       con cobertura de la laptop) = RED_SSID / RED_CLAVE, encuentra solo al
       SERVIDOR de la pagina (servidor/servidor.js en la laptop: avisa por
       UDP en el puerto 9999) y le manda por TCP (puerto 9001) las muestras
       CRUDAS del ADC (1000 por segundo) en paquetes de 20 muestras (20 ms):
         0xAA 0x55 | seq | n | n x uint16 little-endian | checksum
         checksum = XOR de seq, n y los bytes; seq = numero de paquete.
       Cada segundo agrega un paquete de diagnostico (0xAA 0x57 | n | texto
       JSON | checksum): min/max de la senal, frecuencia medida, ultimo RMS.
       El servidor la reparte a las paginas abiertas (WebSocket).
       Sin red o sin servidor el Arduino sigue igual: mide y manda al ESP32.
     - USB (Serial, 250000 baud), segun USB_PARA_PAGINA:
         false -> "Bruto:x,Sin60:y,SinArm:z" para el Serial Plotter (como v2).
         true  -> los mismos paquetes crudos (pagina por Web Serial, sin servidor).
       Solo se escribe si hay una computadora abierta en el puerto, y nunca
       se espera: si no cabe, se descarta.

   Como leer el Serial Plotter (en reposo):
     Bruto mucho mayor que Sin60   -> hay mucho 60 Hz
     Sin60 mucho mayor que SinArm  -> los armonicos de la red dominan
     SinArm sigue alto (> ~0.05 V) -> ruido de banda ancha: electrodos, cables o la placa
   Al contraer, SinArm debe subir claramente.

   Regla de diseno: el Arduino solo mide y envia; nunca controla un actuador.

   Cambios v5 respecto a v4.1 (mismo muestreo, mismo procesamiento, mismo
   envio al ESP32 y mismo Serial Plotter):
     - Ya NO crea su propia red: se une a la red del consultorio y manda la
       senal al servidor de la pagina (nadie tiene que conectarse a la red
       del Arduino). El modulo WiFi se reconecta solo si se cae la red.
     - La cola de bloques RMS sube a 12.8 s, para que conectarse a la red
       (algunos segundos al encender) nunca haga perder lineas del opto.
     - El diagnostico (min/max, frecuencia, ultimo RMS) va al servidor y se
       ve en la pagina; en el monitor serie salen los avisos de conexion.

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

// ------------------------- WiFi y servidor de la pagina ----------------
// La red WiFi a la que se une (la misma que la laptop y el ESP32). Escribe
// aqui la red del consultorio, o la "zona con cobertura" de la laptop.
const bool USAR_WIFI       = true;
// VERSION SIN LAPTOP: la red la crea el ESP32 del atril. La clave va en
// credenciales.h (copia credenciales_ejemplo.h; ese archivo no se sube a
// GitHub) o aqui abajo. Tiene que ser IGUAL en el Arduino y en el ESP32.
#if __has_include("credenciales.h")
#include "credenciales.h"
#endif
#ifndef ASTRA_RED
#define ASTRA_RED   "ASTRA"
#endif
#ifndef ASTRA_CLAVE
#define ASTRA_CLAVE "CAMBIA-ESTA-CLAVE"   // 8 a 63 caracteres
#endif
const char RED_SSID[]      = ASTRA_RED;        // la red que crea el ESP32
const char RED_CLAVE[]     = ASTRA_CLAVE;      // su clave
// El servidor es el ESP32 del atril: siempre esta en 192.168.4.1.
const char SERVIDOR_IP[]   = "192.168.4.1";
const uint16_t PUERTO_SERVIDOR = 9001;
const uint16_t PUERTO_AVISO    = 9999;
const bool USB_PARA_PAGINA = false;            // true: paquetes crudos por USB en vez del Serial Plotter
const char VERSION_EMG[]   = "5.1-AP";
const uint8_t  TX_COLA_MS      = 5;             // ms que se siguen apartando despues de cada envio WiFi
const uint32_t ENVIO_LENTO_MS  = 60;            // un envio mas lento que esto pausa la senal en vivo 3 s
const uint16_t MARCA_TX        = 0x8000;        // bit 15 de una muestra cruda: tomada durante un envio WiFi

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
// v5.1: muestras apartadas porque se tomaron mientras el WiFi transmitia
int nLimpias = 0;                       // muestras del bloque que cuentan para el RMS
double t_bruto = 0, t_60 = 0, t_arm = 0;   // las apartadas (por si hubiera que usarlas todas)
volatile bool wifiOcupado = false;      // el loop esta mandando algo por WiFi
uint8_t colaTx = 0;                     // ms que se siguen apartando despues de mandar
volatile uint32_t apartadas = 0;        // total de muestras apartadas
volatile float rmsTxMv = 0;             // SinArm de las muestras apartadas del ultimo bloque que tuvo (mV)
volatile float rmsLimpioMv = 0;         // SinArm del ultimo bloque, sin las apartadas (mV)

// ---- Muestras crudas: la interrupcion escribe, el loop lee ----
const uint32_t N_CRUDO = 2048;      // 2 s de reserva (potencia de 2)
volatile uint16_t crudo[N_CRUDO];
volatile uint32_t nCrudo = 0;       // muestras escritas desde el arranque

// ---- Cola de bloques RMS (12.8 s): la interrupcion escribe, el loop lee ----
const uint32_t N_COLA = 128;
volatile float colaBruto[N_COLA], cola60[N_COLA], colaArm[N_COLA];
volatile bool colaOpto[N_COLA];
volatile uint32_t colaEsc = 0, colaLei = 0;
volatile uint32_t bloquesPerdidos = 0;

// ---- Diagnostico (va al servidor cada segundo y se ve en la pagina) ----
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
WiFiUDP udp;                                  // escucha el aviso del servidor
WiFiClient flujo;                             // conexion TCP con el servidor
bool transmitiendo = false;
IPAddress ipServidor;
bool hayServidor = false;
uint32_t tAviso = 0;                          // ultimo aviso UDP recibido
uint32_t sigWifi = 0, sigUsb = 0;             // siguiente muestra a mandar
uint32_t tRevisar = 0, tEnvio = 0, tDiag = 0, tIntento = 0, tRed = 0;
bool redAntes = false;
// v5.1: senal en vivo ordenada y medida
bool enviarSenal = true;                      // el atril manda "S" / "N"
bool hayHueco = false;                        // acaba de salir una linea al opto
uint32_t pausaSenalHasta = 0;                 // senal en vivo pausada por un envio lento
uint32_t envioMax = 0, nLentos = 0, tRevisarOrden = 0, apartadasAntes = 0, tApartadas = 0, apartadasSeg = 0;
uint32_t tOptoMax = 0;

// ======================================================================
//  Muestreo (interrupcion a FS). Mismo procesamiento que v2.
// ======================================================================
void alMuestrear(timer_callback_args_t *) {
  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, HIGH);

  int cuentaAdc = analogRead(PIN_EMG);
  // v5.1: esta muestra se tomo mientras (o justo despues de que) el WiFi transmitia?
  bool tx = wifiOcupado || colaTx;
  if (wifiOcupado) colaTx = TX_COLA_MS; else if (colaTx) colaTx--;
  ultimoAdc = (uint16_t)cuentaAdc;
  if (cuentaAdc < minAcum) minAcum = cuentaAdc;
  if (cuentaAdc > maxAcum) maxAcum = cuentaAdc;
  crudo[nCrudo & (N_CRUDO - 1)] = (uint16_t)cuentaAdc | (tx ? MARCA_TX : 0);
  nCrudo = nCrudo + 1;

  float v = cuentaAdc * V_REF / ADC_MAX;
  dc += ALPHA_DC * (v - dc);
  float x = v - dc;

  float y60 = n60.run(x);
  float ya = x;
  for (int k = 0; k < nArm; k++) ya = arm[k].run(ya);

  if (!tx) {
    s_bruto += (double)x * x;
    s_60    += (double)y60 * y60;
    s_arm   += (double)ya * ya;
    nLimpias++;
  } else {                                     // v5.1: apartada (no cuenta para el RMS)
    t_bruto += (double)x * x;
    t_60    += (double)y60 * y60;
    t_arm   += (double)ya * ya;
    apartadas = apartadas + 1;
  }

  if (++cuenta >= N_BLOQUE) {
    // v5.1: RMS solo con las muestras limpias. Sin WiFi son las 100 (igual que siempre).
    // Si quedaran menos de la mitad, se usan todas (nunca se deja de mandar al opto).
    int nApart = N_BLOQUE - nLimpias;
    if (nApart > 0) rmsTxMv = sqrt(t_arm / nApart) * 1000.0f;
    int n = nLimpias;
    if (n < N_BLOQUE / 2) { n = N_BLOQUE; s_bruto += t_bruto; s_60 += t_60; s_arm += t_arm; }
    rmsLimpioMv = sqrt(s_arm / n) * 1000.0f;
    minBloque = minAcum; maxBloque = maxAcum; minAcum = 65535; maxAcum = 0;
    if (bloquesHechos <= BLOQUES_ASENTAR) bloquesHechos++;
    if (colaEsc - colaLei < N_COLA) {
      uint32_t i = colaEsc % N_COLA;
      colaBruto[i] = sqrt(s_bruto / n);
      cola60[i]    = sqrt(s_60 / n);
      colaArm[i]   = sqrt(s_arm / n);
      colaOpto[i]  = bloquesHechos > BLOQUES_ASENTAR;
      ultimoRmsMv  = colaArm[i] * 1000.0f;
      colaEsc = colaEsc + 1;
    } else {
      bloquesPerdidos = bloquesPerdidos + 1;   // el loop lleva > 6.4 s sin vaciar la cola
    }
    s_bruto = s_60 = s_arm = 0;
    t_bruto = t_60 = t_arm = 0;
    cuenta = 0;
    nLimpias = 0;
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

uint32_t tUltOpto = 0, optoMax = 0;   // v5.1: mayor espera entre lineas del opto (diagnostico)

// Devuelve cuantos bloques se mandaron (v5.1: el WiFi transmite justo despues)
int vaciarCola() {
  int hechos = 0;
  while (colaLei != colaEsc) {
    hechos++;
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
      uint32_t t = millis();
      if (tUltOpto && t - tUltOpto > optoMax) optoMax = t - tUltOpto;
      tUltOpto = t;
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
  return hechos;
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
//  WiFi: se une a la red, encuentra al servidor y le manda la senal
// ======================================================================
void mensaje(const char *t) { if (!USB_PARA_PAGINA) Serial.println(t); }

void iniciarWifi() {
  if (WiFi.status() == WL_NO_MODULE) { mensaje("!! No responde el modulo WiFi del R4."); return; }
  mensaje(">> Conectando a la red WiFi...");
  WiFi.setTimeout(0);                          // begin() no espera: el loop sigue mandando al opto
  WiFi.begin(RED_SSID, RED_CLAVE);             // el modulo WiFi termina de conectarse solo
  wifiListo = true;
  tRed = millis();
}

void cerrarFlujo() {
  flujo.stop();
  if (transmitiendo) mensaje("!! Se cerro la conexion con el servidor de la pagina.");
  transmitiendo = false;
}

// v5.1: todo lo que hace transmitir al modulo WiFi va entre wifiOcupado = true / false
// (asi la interrupcion aparta las muestras tomadas mientras tanto)
size_t mandarWifi(const uint8_t *b, size_t n) {
  uint32_t t0 = millis();
  wifiOcupado = true;
  size_t r = flujo.write(b, n);
  wifiOcupado = false;
  uint32_t d = millis() - t0;
  if (d > envioMax) envioMax = d;
  if (r == n && d > ENVIO_LENTO_MS) {          // WiFi lento: la senal en vivo descansa 3 s (el opto sigue)
    pausaSenalHasta = millis() + 3000;
    nLentos++;
  }
  return r;
}

// Paquete de diagnostico: AA 57 | n | texto JSON | checksum (XOR del texto)
void mandarDiagnostico() {
  char t[256];
  uint16_t mn = minBloque, mx = maxBloque, ua = ultimoAdc;
  float seg = (millis() - tInicioMuestreo) / 1000.0f;
  int n = snprintf(t, sizeof t, "{\"v\":\"%s\",\"adc\":%u,\"min\":%u,\"max\":%u,\"fs\":%.1f,\"rms\":%.0f,\"perdidos\":%lu"
                   ",\"sen\":%d,\"tx\":%lu,\"rTx\":%.1f,\"rOk\":%.1f,\"wMax\":%lu,\"lentos\":%lu,\"opto\":%lu}",
                   VERSION_EMG, ua, mn, mx, seg > 0 ? nCrudo / seg : 0.0f, (float)ultimoRmsMv, (unsigned long)bloquesPerdidos,
                   enviarSenal ? 1 : 0, (unsigned long)apartadasSeg, (float)rmsTxMv, (float)rmsLimpioMv,
                   (unsigned long)envioMax, (unsigned long)nLentos, (unsigned long)optoMax);
  envioMax = 0;
  if (millis() - tOptoMax > 10000) { tOptoMax = millis(); optoMax = 0; }   // la mayor espera de los ultimos ~10 s
  if (n <= 0 || n > 250) return;
  uint8_t p[256 + 4];
  p[0] = 0xAA; p[1] = 0x57; p[2] = (uint8_t)n;
  uint8_t chk = 0;
  for (int i = 0; i < n; i++) { p[3 + i] = t[i]; chk ^= (uint8_t)t[i]; }
  p[3 + n] = chk;
  if (mandarWifi(p, n + 4) != (size_t)(n + 4)) cerrarFlujo();
}

void atenderWifi() {
  uint32_t ahora = millis();

  // Red: el modulo se reconecta solo; aqui solo se avisa el cambio
  if (ahora - tRevisar >= 500) {
    tRevisar = ahora;
    bool red = WiFi.status() == WL_CONNECTED;
    if (red != redAntes) {
      redAntes = red;
      if (red) {
        udp.begin(PUERTO_AVISO);
        if (!USB_PARA_PAGINA) { Serial.print(">> WiFi conectado a "); Serial.print(RED_SSID); Serial.print(". IP del Arduino: "); Serial.println(WiFi.localIP()); }
        if (SERVIDOR_IP[0] && ipServidor.fromString(SERVIDOR_IP)) { hayServidor = true; tAviso = ahora; }
      } else { mensaje("!! Sin red WiFi (el Arduino sigue midiendo y mandando al ESP32)."); cerrarFlujo(); }
    }
    // Sin red (p. ej. el atril todavia arranca): vuelve a pedirla cada 15 s (no espera)
    if (!red && ahora - tRed > 15000) { tRed = ahora; wifiOcupado = true; WiFi.begin(RED_SSID, RED_CLAVE); wifiOcupado = false; }
    // Aviso del servidor por UDP: "ASTRA-SERVIDOR 9001 9002"
    if (red) {
      int n = udp.parsePacket();
      if (n > 0) {
        char t[48]; int k = udp.read((unsigned char *)t, sizeof(t) - 1); if (k < 0) k = 0; t[k] = 0;
        if (!strncmp(t, "ASTRA-SERVIDOR", 14)) {
          IPAddress ip = udp.remoteIP();
          if (!hayServidor || ip != ipServidor) {
            if (!USB_PARA_PAGINA) { Serial.print(">> Servidor de la pagina encontrado en "); Serial.println(ip); }
          }
          ipServidor = ip; hayServidor = true; tAviso = ahora;
        }
      }
    }
  }

  // Conexion TCP con el servidor (solo si se sabe donde esta)
  if (!transmitiendo && hayServidor && redAntes && ahora - tIntento >= 3000 &&
      (SERVIDOR_IP[0] || ahora - tAviso < 5000)) {
    tIntento = ahora;
    wifiOcupado = true;
    bool ok = flujo.connect(ipServidor, PUERTO_SERVIDOR);
    wifiOcupado = false;
    if (ok) {
      transmitiendo = true;
      enviarSenal = true;                       // hasta que el atril diga otra cosa
      sigWifi = inicioAlineado();               // empieza desde "ahora"
      tDiag = 0;
      mensaje(">> Conectado al servidor: la senal ya sale a la pagina.");
    }
  }

  // v5.1: ordenes del atril ("S" = mandar la senal en vivo, "N" = no), cada medio segundo
  if (transmitiendo && ahora - tRevisarOrden >= 500) {
    tRevisarOrden = ahora;
    if (flujo.available() > 0) {
      uint8_t o[8];
      int k = flujo.read(o, sizeof o);
      for (int i = 0; i < k; i++) {
        if (o[i] == 'S' && !enviarSenal) { enviarSenal = true; sigWifi = inicioAlineado(); }
        else if (o[i] == 'N') enviarSenal = false;
      }
    }
  }

  // v5.1: la senal sale justo despues de cada linea del opto (~10 veces por segundo)
  bool pausada = pausaSenalHasta && (int32_t)(ahora - pausaSenalHasta) < 0;
  if (transmitiendo && hayHueco) {
    hayHueco = false;
    if (!enviarSenal || pausada) sigWifi = inicioAlineado();   // no se manda: no se acumula
    else {
      tEnvio = ahora;
      int len = armarPaquetes(sigWifi, bufWifi, MAX_PAQ_WIFI);
      if (len && mandarWifi(bufWifi, len) != (size_t)len) cerrarFlujo();   // el servidor se fue
    }
  }
  if (ahora - tApartadas >= 1000) {             // muestras apartadas en el ultimo segundo
    uint32_t a = apartadas;
    apartadasSeg = a - apartadasAntes; apartadasAntes = a; tApartadas = ahora;
  }
  if (transmitiendo && ahora - tDiag >= 1000) {
    tDiag = ahora;
    if (!flujo.connected()) cerrarFlujo(); else mandarDiagnostico();
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

  // El muestreo ya corre; unirse a la red tarda unos segundos (la cola guarda los bloques)
  if (USAR_WIFI) iniciarWifi();
}

void loop() {
  if (vaciarCola()) hayHueco = true;           // v5.1: acaba de salir al opto: ahora puede transmitir el WiFi
  if (USB_PARA_PAGINA) mandarCrudoUsb();
  if (wifiListo) atenderWifi();
}
