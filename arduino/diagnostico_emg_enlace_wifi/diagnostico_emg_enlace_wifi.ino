/*
  =====================================================================
   ASTRA - Arduino UNO R4 WiFi (lado del nino): diagnostico_emg_enlace_wifi.ino  v3
   Mide la senal de Nova, manda el RMS al ESP32 (igual que v2) y ADEMAS
   transmite la senal EMG filtrada por WiFi para verla en la pagina web.
  ---------------------------------------------------------------------
   Igual que v2 (sin cambios):
     - Lee A0 a 1000 Hz, 14 bits, 5 V.
     - Quita DC (alfa 0.003), notch 60 Hz y notch 60..420 Hz (Q = 20).
     - RMS cada 100 muestras. Bruto / Sin60 / SinArm.
     - USB (Serial, 250000 baud): "Bruto:x,Sin60:y,SinArm:z" para el Serial Plotter.
     - Opto (Serial1, pin D1, 9600 baud): solo SinArm en mV, "640\n" cada
       100 ms, 1 caracter cada 2 ms, nada en los primeros 0.5 s.
     El codigo de ese procesamiento esta en emg_nucleo.h, copiado de v2.

   Nuevo en v3:
     - El muestreo corre en la interrupcion de un temporizador de hardware
       (1000 Hz exactos), no en el loop(). WiFiS3 bloquea el loop() varios
       ms en cada envio (y hasta 10 s al conectarse a la red); asi eso no
       atrasa ninguna muestra ni el opto.
     - WiFi (biblioteca WiFiS3): manda SinArm (la misma senal de la que sale
       el RMS, en mV con resolucion de 0.1 mV) en paquetes de 50-100
       muestras con su numero de muestra, por TCP, al "puente" que corre en
       la laptop (puente/puente_emg.py). El puente se la pasa a la pagina.
       Ver docs/WIFI.md: por que hace falta el puente (HTTPS / contenido mixto).
     - El puente se encuentra solo (aviso UDP en el puerto 5006) o con una
       IP fija en arduino_secrets.h.
     - LED integrado (D13): fijo = transmitiendo; parpadeo rapido = WiFi
       listo, buscando el puente; parpadeo lento = sin red; apagado = WiFi
       desactivado o sin modulo.
     - Escribe 'i' en el monitor serie (250000) para ver el diagnostico.

   Regla de diseno (igual que v2): el Arduino solo mide y envia; nunca
   controla un actuador. El ESP32 del atril no cambia.

   Placa: "Arduino UNO R4 WiFi". Bibliotecas: WiFiS3 (viene con la placa).
   Antes de cargar: copia arduino_secrets_ejemplo.h como arduino_secrets.h
   y pon tu red. arduino_secrets.h NO se sube a GitHub (.gitignore).
  =====================================================================
*/
#include <Arduino.h>
#include <math.h>
#include <FspTimer.h>
#include <WiFiS3.h>

#include "emg_nucleo.h"
#include "emg_flujo.h"
#include "emg_usb.h"

#if __has_include("arduino_secrets.h")
#include "arduino_secrets.h"
#else
#error "Falta arduino_secrets.h: copia arduino_secrets_ejemplo.h, guardalo como arduino_secrets.h y pon el nombre y la clave de tu red WiFi."
#endif
#ifndef SECRET_PUENTE_IP
#define SECRET_PUENTE_IP ""
#endif

// ------------------------- Parametros ajustables -----------------------
const int   PIN_EMG  = A0;
const unsigned long BAUD_USB  = 250000;
const unsigned long BAUD_OPTO = 9600;
const uint32_t MARGEN_US      = 150;   // solo modo de respaldo: tiempo libre antes de la siguiente muestra
const int PIN_MUESTREO        = -1;    // pon p. ej. 7 para ver cada muestra en el osciloscopio

const bool USAR_WIFI          = true;  // false = se comporta como v2 (sin WiFi)
const bool MENSAJES_USB       = true;  // avisos de WiFi en el USB (lineas que empiezan con '#')
const bool USAR_LED           = true;  // LED integrado como indicador del WiFi

const uint16_t PUERTO_TCP     = 5005;  // puente: recibe la senal
const uint16_t PUERTO_UDP     = 5006;  // puente: aviso "aqui estoy"

// Prioridad de la interrupcion de muestreo. DEBE ser mayor (numero) que la de
// los UART del nucleo (12): Serial1.write() espera a que la interrupcion del
// UART confirme el envio, y esa interrupcion tiene que poder entrar. 13 tambien
// deja entrar a los UART del USB y del modulo WiFi.
const uint8_t PRIORIDAD_MUESTREO = 13;
static_assert(PRIORIDAD_MUESTREO > 12, "La interrupcion de muestreo debe tener menos prioridad que los UART (12)");

const uint32_t TS_US = (uint32_t)(1000000.0f / emg::FS + 0.5f);   // 1/FS en microsegundos

// Tipos antes de cualquier funcion: el IDE de Arduino pone los prototipos que
// genera antes de la primera funcion, y ahi el tipo ya tiene que existir.
enum EstadoRed : uint8_t { RED_APAGADA, RED_UNIRSE, RED_ESPERAR_IP, RED_BUSCAR_PUENTE, RED_CONECTAR, RED_TRANSMITIR };

// ------------------------- Muestreo -------------------------------------
FspTimer temporizador;
bool muestreoPorInterrupcion = false;

// Diagnostico de la interrupcion (se lee con 'i')
volatile uint32_t isrDuracionMax = 0, isrIntervaloMax = 0, isrAtrasos = 0, isrMuestras = 0;
volatile uint32_t isrUltima = 0;

static void optoTx(uint8_t c) { Serial1.write(c); }

static inline void tomarMuestra() {
  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, HIGH);
  float ya = emg::procesarMuestra(analogRead(PIN_EMG));
  if (PIN_MUESTREO >= 0) digitalWrite(PIN_MUESTREO, LOW);
  flujo::guardar(ya);
}

void alMuestrear(timer_callback_args_t *) {
  uint32_t t0 = micros();
  tomarMuestra();
  uint32_t dur = micros() - t0;
  if (dur > isrDuracionMax) isrDuracionMax = dur;
  if (isrMuestras > 0) {
    uint32_t d = t0 - isrUltima;
    if (d > isrIntervaloMax) isrIntervaloMax = d;
    if (d > TS_US + TS_US / 2) isrAtrasos = isrAtrasos + 1;
  }
  isrUltima = t0;
  isrMuestras = isrMuestras + 1;
}

// Temporizador de hardware a 1000 Hz exactos (cuentas enteras del reloj).
bool iniciarTemporizador() {
  uint8_t tipo = 0;
  int8_t canal = FspTimer::get_available_timer(tipo);
  if (canal < 0) return false;
  uint32_t reloj = (tipo == GPT_TIMER) ? R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKD)
                                       : R_FSP_SystemClockHzGet(FSP_PRIV_CLOCK_PCLKB);
  uint32_t cuentas = (uint32_t)(reloj / (uint32_t)emg::FS);
  if (cuentas < 1000 || cuentas > 65535) return false;   // tambien vale para canales de 16 bits
  if (!temporizador.begin(TIMER_MODE_PERIODIC, tipo, canal, cuentas, cuentas / 2, TIMER_SOURCE_DIV_1, alMuestrear)) return false;
  if (!temporizador.setup_overflow_irq(PRIORIDAD_MUESTREO)) return false;
  if (!temporizador.open()) return false;
  IRQn_Type irq = temporizador.get_cfg()->cycle_end_irq;
  NVIC_SetPriority(irq, PRIORIDAD_MUESTREO);              // se fija explicitamente
  if (NVIC_GetPriority(irq) != PRIORIDAD_MUESTREO) return false;
  return temporizador.start();
}

// ------------------------- USB: lineas para el Serial Plotter (emg_usb.h) --
uint32_t ultimoBloqueUSB = 0;

// Lee lo que publico la interrupcion (copia corta con interrupciones apagadas)
bool tomarBloque(float &b, float &s60, float &sa) {
  noInterrupts();
  uint32_t bloque = emg::pub.bloque;
  b = emg::pub.rmsBruto; s60 = emg::pub.rms60; sa = emg::pub.rmsArm;
  interrupts();
  if (bloque == ultimoBloqueUSB) return false;
  ultimoBloqueUSB = bloque;                 // si se juntaron varios, va el mas nuevo (como v2)
  return true;
}

void mensaje(const char *texto) {
  if (!MENSAJES_USB) return;
  Serial.print("# ");
  Serial.println(texto);
}

// ------------------------- WiFi -----------------------------------------
EstadoRed red = RED_APAGADA;
unsigned long tEstado = 0, tRevision = 0, tReintento = 0;
unsigned long esperaReintento = 0;

WiFiClient cliente;
WiFiUDP udp;
bool udpAbierto = false;
IPAddress ipPuente;
uint16_t puertoPuente = PUERTO_TCP;
bool ipPuenteFija = false;

uint32_t paquetesEnviados = 0, fallosEnvio = 0, conexiones = 0;
uint8_t paquete[flujo::TAM_MAX_PAQUETE];

void cambiarRed(EstadoRed e) { red = e; tEstado = millis(); tRevision = millis(); }

void perderRed(const char *motivo) {
  mensaje(motivo);
  cliente.stop();
  if (udpAbierto) { udp.stop(); udpAbierto = false; }
  esperaReintento = 2000;
  tReintento = millis();
  cambiarRed(RED_UNIRSE);
}

void iniciarWiFi() {
  if (!USAR_WIFI) { cambiarRed(RED_APAGADA); return; }
  if (strlen(SECRET_PUENTE_IP) > 0) {
    ipPuenteFija = ipPuente.fromString(SECRET_PUENTE_IP);
    if (!ipPuenteFija) mensaje("SECRET_PUENTE_IP no es una IP valida; se buscara el puente solo.");
  }
  esperaReintento = 0;
  tReintento = millis();
  cambiarRed(RED_UNIRSE);
}

bool redSigueConectada() {
  if (millis() - tRevision < 2000) return true;
  tRevision = millis();
  return WiFi.status() == WL_CONNECTED;
}

// Aviso del puente: "ASTRA-EMG-PUENTE <version> <puerto>"
bool leerAvisoPuente() {
  int n = udp.parsePacket();
  if (n <= 0) return false;
  char txt[64];
  int k = udp.read((unsigned char *)txt, sizeof(txt) - 1);
  if (k <= 0) return false;
  txt[k] = 0;
  const char *prefijo = "ASTRA-EMG-PUENTE";
  if (strncmp(txt, prefijo, strlen(prefijo)) != 0) return false;
  const char *esp = strrchr(txt, ' ');
  long p = esp ? atol(esp + 1) : 0;
  if (p <= 0 || p > 65535) return false;
  IPAddress ip = udp.remoteIP();
  if (ip == IPAddress(0, 0, 0, 0)) return false;
  ipPuente = ip;
  puertoPuente = (uint16_t)p;
  return true;
}

void atenderRed() {
  char txt[96];
  switch (red) {
    case RED_APAGADA:
      return;

    case RED_UNIRSE: {
      if (millis() - tReintento < esperaReintento) return;
      if (WiFi.status() == WL_NO_MODULE) {
        mensaje("No responde el modulo WiFi (ESP32-S3). Sigo sin WiFi; el RMS al ESP32 no se afecta.");
        cambiarRed(RED_APAGADA);
        return;
      }
      snprintf(txt, sizeof(txt), "WiFi: conectando a \"%s\"...", SECRET_SSID);
      mensaje(txt);
      // Bloquea hasta ~10 s. El muestreo y el opto siguen en la interrupcion.
      int r = WiFi.begin(SECRET_SSID, SECRET_PASS);
      if (r == WL_CONNECTED) {
        cambiarRed(RED_ESPERAR_IP);
      } else {
        mensaje("WiFi: no se pudo conectar (revisa nombre y clave). Reintento en 5 s.");
        esperaReintento = 5000;
        tReintento = millis();
      }
      return;
    }

    case RED_ESPERAR_IP: {
      if (millis() - tRevision < 250) return;
      tRevision = millis();
      IPAddress ip = WiFi.localIP();
      if (ip != IPAddress(0, 0, 0, 0)) {
        snprintf(txt, sizeof(txt), "WiFi: conectado. IP del Arduino %s", ip.toString().c_str());
        mensaje(txt);
        mensaje(ipPuenteFija ? "Puente: usando la IP de arduino_secrets.h" : "Puente: buscando (aviso UDP 5006)...");
        cambiarRed(RED_BUSCAR_PUENTE);
      } else if (millis() - tEstado > 10000) {
        perderRed("WiFi: no llego la IP (DHCP). Reintentando.");
      }
      return;
    }

    case RED_BUSCAR_PUENTE: {
      if (!redSigueConectada()) { perderRed("WiFi: se perdio la red."); return; }
      if (millis() - tReintento < esperaReintento) return;
      if (ipPuenteFija) { puertoPuente = PUERTO_TCP; cambiarRed(RED_CONECTAR); return; }
      if (!udpAbierto) {
        udpAbierto = udp.begin(PUERTO_UDP);
        if (!udpAbierto) { esperaReintento = 2000; tReintento = millis(); return; }
      }
      static unsigned long tSondeo = 0;
      if (millis() - tSondeo < 250) return;
      tSondeo = millis();
      if (leerAvisoPuente()) cambiarRed(RED_CONECTAR);
      return;
    }

    case RED_CONECTAR: {
      cliente.stop();
      cliente.setConnectionTimeout(3000);
      snprintf(txt, sizeof(txt), "Puente: conectando a %s:%u...", ipPuente.toString().c_str(), (unsigned)puertoPuente);
      mensaje(txt);
      if (cliente.connect(ipPuente, puertoPuente)) {
        flujo::saltarAlPresente();        // se empieza desde "ahora", sin datos viejos
        conexiones++;
        mensaje("Puente: conectado. Transmitiendo la senal filtrada.");
        cambiarRed(RED_TRANSMITIR);
      } else {
        mensaje("Puente: no contesto. Esta abierto puente_emg.py en la laptop? Reintento en 2 s.");
        cliente.stop();
        esperaReintento = 2000;
        tReintento = millis();
        cambiarRed(RED_BUSCAR_PUENTE);
      }
      return;
    }

    case RED_TRANSMITIR: {
      if (!redSigueConectada()) { perderRed("WiFi: se perdio la red."); return; }
      // Hasta 4 paquetes por vuelta: asi se pone al dia despues de un atraso.
      for (int k = 0; k < 4 && flujo::disponibles() >= (uint32_t)flujo::MIN_POR_PAQUETE; k++) {
        int n = 0;
        int len = flujo::armarPaquete(paquete, flujo::MAX_POR_PAQUETE, &n, (uint16_t)emg::FS);
        if (len == 0) continue;
        if (cliente.write(paquete, (size_t)len) == (size_t)len) {
          flujo::confirmar(n);
          paquetesEnviados++;
        } else {
          fallosEnvio++;
          mensaje("Puente: fallo el envio; reconectando.");
          cliente.stop();
          esperaReintento = 1000;
          tReintento = millis();
          cambiarRed(RED_BUSCAR_PUENTE);
          return;
        }
      }
      return;
    }
  }
}

void actualizarLed() {
  if (!USAR_LED) return;
  bool on;
  unsigned long t = millis();
  switch (red) {
    case RED_TRANSMITIR:    on = true; break;
    case RED_BUSCAR_PUENTE:
    case RED_CONECTAR:      on = (t / 125) % 2; break;   // rapido
    case RED_UNIRSE:
    case RED_ESPERAR_IP:    on = (t / 500) % 2; break;   // lento
    default:                on = false; break;
  }
  digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
}

void imprimirDiagnostico() {
  noInterrupts();
  uint32_t dmax = isrDuracionMax, imax = isrIntervaloMax, atr = isrAtrasos, n = isrMuestras;
  isrDuracionMax = 0; isrIntervaloMax = 0;
  interrupts();
  char txt[128];
  const char *nombres[] = {"apagado", "uniendose a la red", "esperando IP", "buscando puente", "conectando", "transmitiendo"};
  Serial.println();
  Serial.println("# ---- Diagnostico v3 ----");
  snprintf(txt, sizeof(txt), "# Muestreo: %s, %lu muestras, interrupcion max %lu us, intervalo max %lu us, atrasos %lu",
           muestreoPorInterrupcion ? "interrupcion 1 kHz" : "RESPALDO en loop (sin WiFi)",
           (unsigned long)n, (unsigned long)dmax, (unsigned long)imax, (unsigned long)atr);
  Serial.println(txt);
  snprintf(txt, sizeof(txt), "# WiFi: %s, firmware modulo %s", nombres[red], USAR_WIFI ? WiFi.firmwareVersion() : "-");
  Serial.println(txt);
  if (red >= RED_BUSCAR_PUENTE) {
    snprintf(txt, sizeof(txt), "# IP %s, RSSI %ld dBm, puente %s:%u", WiFi.localIP().toString().c_str(),
             (long)WiFi.RSSI(), ipPuente.toString().c_str(), (unsigned)puertoPuente);
    Serial.println(txt);
  }
  snprintf(txt, sizeof(txt), "# Paquetes enviados %lu, fallos %lu, conexiones %lu, muestras no enviadas (WiFi lento) %lu",
           (unsigned long)paquetesEnviados, (unsigned long)fallosEnvio, (unsigned long)conexiones, (unsigned long)flujo::perdidas);
  Serial.println(txt);
  Serial.println("# ------------------------");
}

void atenderUSB() {
  float b, s60, sa;
  if (tomarBloque(b, s60, sa)) {
    char linea[64];
    int n = armarLineaUSB(linea, sizeof(linea), b, s60, sa);
    Serial.write((uint8_t *)linea, n);
  }
  while (Serial.available()) {
    int c = Serial.read();
    if (c == 'i') imprimirDiagnostico();
  }
}

// ------------------------- Respaldo: muestreo en el loop (como v2) ------
// Solo si el temporizador no arranco. Sin WiFi, para no atrasar muestras.
char lineaRespaldo[64]; int lenRespaldo = 0, posRespaldo = 0;

void loopRespaldo() {
  static uint32_t tSiguiente = micros();
  while ((int32_t)(micros() - tSiguiente) < 0) {}   // espera exacta, sin delay()
  tSiguiente += TS_US;
  tomarMuestra();
  float b, s60, sa;
  if (tomarBloque(b, s60, sa)) { lenRespaldo = armarLineaUSB(lineaRespaldo, sizeof(lineaRespaldo), b, s60, sa); posRespaldo = 0; }
  while (posRespaldo < lenRespaldo && (int32_t)(tSiguiente - micros()) > (int32_t)MARGEN_US) {
    Serial.write((uint8_t)lineaRespaldo[posRespaldo++]);
  }
}

// ======================================================================
void setup() {
  Serial.begin(BAUD_USB);
  Serial1.begin(BAUD_OPTO);          // enlace al ESP32 por el opto (pin D1)
  analogReadResolution(14);
  if (PIN_MUESTREO >= 0) pinMode(PIN_MUESTREO, OUTPUT);
  if (USAR_LED) { pinMode(LED_BUILTIN, OUTPUT); digitalWrite(LED_BUILTIN, LOW); }

  emg::iniciar(optoTx, [] { return analogRead(PIN_EMG); });

  // Desde aqui la interrupcion es la UNICA que llama analogRead() y Serial1.write().
  muestreoPorInterrupcion = iniciarTemporizador();
  if (muestreoPorInterrupcion) {
    mensaje("ASTRA v3: muestreo a 1 kHz por interrupcion. Escribe 'i' para diagnostico.");
    iniciarWiFi();
  } else {
    mensaje("!! No se pudo iniciar el temporizador: muestreo en el loop como v2 y SIN WiFi.");
  }
}

void loop() {
  if (!muestreoPorInterrupcion) { loopRespaldo(); return; }
  atenderUSB();
  atenderRed();
  actualizarLed();
}
