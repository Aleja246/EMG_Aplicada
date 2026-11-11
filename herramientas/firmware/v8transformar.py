# ASTRA - atril v8 SIN LAPTOP: el ESP32 crea la red WiFi "ASTRA", sirve la pagina
# (LittleFS) y habla con ella por WebSocket. Recibe la senal cruda del Arduino por WiFi.
#
# Se aplica sobre el firmware ORIGINAL del atril (v5 con sonido). Reutiliza, sin
# cambios, las adaptaciones ya verificadas del v7.1 (secciones 1 y 5 a 10 de
# v7transformar.py: mensajes, bloques, activaciones, musica y volumen) y solo
# reemplaza la parte de red (secciones 2, 3, 4, 11, 12 y 13).
#
# Uso: python3 v8transformar.py original.ino salida.ino
import os
import sys

aqui = os.path.dirname(os.path.abspath(__file__))
v7 = open(os.path.join(aqui, 'v7transformar.py')).read()


def seccion(ini, fin):
    return v7[v7.index(ini):v7.index(fin)]


exec(v7[:v7.index('# 1)')])                   # lee el original y define rep()
exec(seccion('# 1)', '# 2)'))                 # Serial.print -> Log.print
exec(seccion('# 5)', '# 11)'))                # datos para la pagina, musica, volumen, comandos
rep("""QueueHandle_t colaVolumen = NULL;   // volumen de la pagina -> loop
""", """QueueHandle_t colaVolumen = NULL;   // volumen de la pagina -> loop
QueueHandle_t colaAvisos = NULL;    // mensajes de la red -> loop (los imprime)
void avisar(int8_t a) { if (colaAvisos) xQueueSend(colaAvisos, &a, 0); }
""")
rep("""volatile int8_t avisoWifi = 0;      // 1/-1 red, 2/-2 servidor (lo imprime el loop)
IPAddress ipServidor;
""", "")

rep('  Serial.println("=== ASTRA - atril v7.1 (sonido + servidor de la pagina) ===");',
    '  Serial.println("=== ASTRA - atril v8.2 (sonido + red ASTRA + pagina en 192.168.4.1) ===");')

# 2) cabecera
rep("//  ASTRA - Firmware del atril (ESP32) v2.3", "//  ASTRA - Firmware del atril (ESP32) v8 SIN LAPTOP (sonido + red ASTRA + pagina web)")
rep("""//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca).
""", r"""//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca). WiFi, LittleFS,
//  Preferences y el servidor web (esp_http_server, con WebSocket) vienen con
//  el paquete de placas "esp32" de Espressif 3.x: no hace falta otra biblioteca.
//  Esquema de particiones: "No OTA (2MB APP/2MB SPIFFS)" (o el "Default 4MB
//  with spiffs"); la carpeta data/ se sube con "Upload LittleFS".
//
//  v8 = tu firmware con sonido + RED ASTRA + PAGINA WEB. La deteccion, la
//  calibracion, los motores, el microswitch, la luz, la musica, el PARO y los
//  comandos del monitor serie NO cambian:
//   - Crea la red WiFi "ASTRA" (clave en credenciales.h). El celular se une
//     a esa red y abre http://192.168.4.1: el ESP32 le sirve la pagina desde
//     LittleFS (copiada a la RAM al encender, para no leer la flash durante
//     la terapia).
//   - El Arduino se une a la misma red y manda la senal EMG cruda al puerto
//     9001 (paquetes 0xAA 0x55 de 20 muestras con numero de secuencia). El
//     ESP32 la revisa y la reenvia a la pagina por WebSocket (/ws). Los
//     motores siguen usando SOLO el RMS que llega por el opto (Serial2).
//   - Cada 200 ms manda a la pagina su estado (modo, calibracion, umbrales,
//     RMS, activaciones, musica, mensajes) y cada segundo el diagnostico
//     (Arduino conectado, muestras/s, paquetes perdidos, paginas abiertas).
//   - De la pagina acepta solo: recalibrar (c), pausa/seguir (p), la musica
//     de cada lista y el volumen. Con el PARO activo ignora toda orden de la
//     pagina: el paro solo se libera con el boton fisico (y BOOT o "r").
//   - La red y la pagina corren en el nucleo 0; motores, luz, musica y
//     deteccion en el nucleo 1 (loop): la pagina nunca los frena. Sin WiFi,
//     sin Arduino por WiFi o sin celular, el atril funciona igual.
//   - Los mensajes (Log.print...) salen por el monitor serie Y a la pagina.
""")
rep("""// PAGINA WEB (futuro): las listas son variables; la pagina solo tiene que llamar
// a configurarPistas(true, a, b, c) para cabeza abajo y configurarPistas(false, a, b, c)
// para cabeza arriba (ver "Configuracion de las listas de musica" mas abajo).
""", """// PAGINA WEB: la pagina elige las 3 pistas de cada lista; el atril llama a
// configurarPistas() y las guarda en su memoria (ver aplicarMusica()).
""")

# 3) includes
rep("#include <AccelStepper.h>\n", """#include <AccelStepper.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <stdarg.h>
""")

# 4) constantes de la red
rep("""const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta
""", r"""const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta

// ------------------------- Red ASTRA y pagina web ----------------------
// La clave va en credenciales.h (copia credenciales_ejemplo.h; ese archivo no
// se sube a GitHub) o aqui abajo. Tiene que ser IGUAL en el Arduino.
#if __has_include("credenciales.h")
#include "credenciales.h"
#endif
#ifndef ASTRA_RED
#define ASTRA_RED   "ASTRA"
#endif
#ifndef ASTRA_CLAVE
#define ASTRA_CLAVE "CAMBIA-ESTA-CLAVE"   // 8 a 63 caracteres
#endif
const bool USAR_WIFI = true;              // false = atril sin red (igual que antes)
const char *RED_SSID  = ASTRA_RED;
const char *RED_CLAVE = ASTRA_CLAVE;
const int  CANAL_WIFI = 6;
const int  MAX_EQUIPOS_RED = 5;           // el Arduino + hasta 4 celulares o computadoras
const int  MAX_PAGINAS = 3;               // paginas abiertas a la vez (las mas viejas se cierran)
const uint16_t PUERTO_ARDUINO = 9001;     // senal cruda del Arduino
const uint16_t PUERTO_AVISO   = 9999;     // aviso UDP "aqui esta ASTRA" (el Arduino tambien tiene la IP fija)
const char *VERSION_ATRIL = "8.2";
""")

# 11) tiempo de sesion y avisos de la red
rep("""void actualizarModo() {
  unsigned long ahora = millis();
""", r"""void actualizarModo() {
  unsigned long ahora = millis();

  // Tiempo de sesion para la pagina (sin pausas)
  static unsigned long tPrevio = 0;
  if (modo == SESION && tPrevio) msSesion += ahora - tPrevio;
  tPrevio = ahora;

  // Avisos de la red (los pone el nucleo 0; se imprimen aqui)
  int8_t aw;
  while (colaAvisos && xQueueReceive(colaAvisos, &aw, 0) == pdTRUE) {
    switch (aw) {
      case 1:  Log.printf(">> Red WiFi \"%s\" lista. En el celular: unete a la red y abre http://%s\n", RED_SSID, WiFi.softAPIP().toString().c_str()); break;
      case -1: Log.println("!! No se pudo crear la red WiFi (la clave debe tener de 8 a 63 caracteres). El atril funciona igual."); break;
      case 2:  Log.println(">> Pagina web lista."); break;
      case -2: Log.println("!! No se pudo iniciar el servidor de la pagina. El atril funciona igual."); break;
      case 3:  Log.println(">> Arduino conectado por WiFi: la senal EMG ya sale a la pagina."); break;
      case -3: Log.println("!! Se perdio la senal WiFi del Arduino (el atril sigue con el RMS del opto)."); break;
      case 5:  Log.println(">> Se abrio la pagina en un celular o computadora."); break;
      case -5: Log.println(">> Se cerro una pagina."); break;
      case -7: Log.println("!! Orden de la pagina ignorada: el PARO esta activo (sueltalo y presiona BOOT)."); break;
      default: break;
    }
  }
""")

# 12) red ASTRA, pagina web y WebSocket
rep("""// ======================================================================
void setup() {""", r"""// ======================================================================
//  Red ASTRA, pagina web y WebSocket (nucleo 0). Solo leen el estado y
//  encolan pedidos: todo lo que cambia el atril lo hace el loop.
// ======================================================================
static char js[8192];               // un mensaje JSON para la pagina
static int jl = 0;

void ja(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ja(const char *fmt, ...) {
  if (jl >= (int)sizeof(js) - 1) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(js + jl, sizeof(js) - jl, fmt, ap);
  va_end(ap);
  if (n > 0) jl = min(jl + n, (int)sizeof(js) - 1);
}

// Texto entre comillas, escapando lo necesario para JSON
void jaTexto(const char *t) {
  ja("\"");
  for (; *t && jl < (int)sizeof(js) - 8; t++) {
    unsigned char c = *t;
    if (c == '"' || c == '\\') ja("\\%c", c);
    else if (c < 0x20) ja(" ");
    else { js[jl++] = c; js[jl] = 0; }
  }
  ja("\"");
}

// Primer indice a mandar: lo pendiente, dentro de lo que se guarda
uint32_t desde(uint32_t pedido, uint32_t total, uint32_t guardados) {
  uint32_t ini = total > guardados ? total - guardados : 0;
  if (pedido < ini) pedido = ini;
  if (pedido > total) pedido = total;
  return pedido;
}

// Arma en js {"type":"atril","st":{...}} con el estado + lo nuevo desde sb, sa, sl (y los avanza)
void armarEstado(uint32_t &sb, uint32_t &sa, uint32_t &sl) {
  static int32_t br[N_BLQ];
  static float bm[N_BLQ];
  static uint8_t bf[N_BLQ];
  static Activacion av[N_ACT];
  static char lt[N_LOG][LARGO_LOG];

  uint32_t nb, na, nl, b0, a0, l0;
  portENTER_CRITICAL(&candado);
  nb = nBlq; na = nActLista; nl = nLog;
  portEXIT_CRITICAL(&candado);
  b0 = desde(sb, nb, N_BLQ);
  a0 = desde(sa, na, N_ACT);
  l0 = desde(sl, nl, N_LOG);
  portENTER_CRITICAL(&candado);
  for (uint32_t i = b0; i < nb; i++) { br[i - b0] = blqRms[i % N_BLQ]; bm[i - b0] = blqMed[i % N_BLQ]; bf[i - b0] = blqFlag[i % N_BLQ]; }
  for (uint32_t i = a0; i < na; i++) av[i - a0] = actLista[i % N_ACT];
  for (uint32_t i = l0; i < nl; i++) memcpy(lt[i - l0], logTxt[i % N_LOG], LARGO_LOG);
  portEXIT_CRITICAL(&candado);
  sb = nb; sa = na; sl = nl;

  unsigned long ahora = millis();
  bool act = activo;
  unsigned long tc = ahora - tCal;
  float restante = modo == CALIBRANDO ? (tc < CAL_MS ? (CAL_MS - tc) / 1000.0f : 0.0f) : 0.0f;

  jl = 0; js[0] = 0;
  ja("{\"type\":\"atril\",\"st\":");
  ja("{\"v\":\"%s\",\"boot\":%lu,\"modo\":\"%s\",\"datos\":%d,\"rms\":%d,\"med\":%.1f",
     VERSION_ATRIL, (unsigned long)idArranque, NOMBRE_MODO[modo], datosOK() ? 1 : 0, rmsUltimo, rmsSuave);
  ja(",\"activo\":%d,\"abajo\":%d,\"abajoAct\":%d,\"nivel\":%d,\"durAct\":%.1f",
     act ? 1 : 0, cabezaAbajo() ? 1 : 0, abajoEsActividad ? 1 : 0, (int)(nivel * 100),
     act ? (ahora - tInicioActivo) / 1000.0f : 0.0f);
  ja(",\"cal\":{\"ok\":%d,\"restante\":%.1f,\"total\":%.1f,\"desc\":%.1f,\"n\":%d,\"nMin\":%d,\"reposo\":%.1f,\"sd\":%.2f,\"cv\":%.3f,\"cvMax\":%.2f",
     calibrado ? 1 : 0, restante, CAL_MS / 1000.0f, CAL_DESCARTAR_MS / 1000.0f, modo == CALIBRANDO ? nReposo : calN,
     CAL_MIN_MUESTRAS, reposo, sdReposo, reposo > 0 ? sdReposo / reposo : 0.0f, CAL_CV_MAX);
  ja(",\"k\":%.1f,\"margen\":%.1f,\"koff\":%.2f,\"uon\":%.1f,\"uoff\":%.1f,\"pocos\":%d,\"inestable\":%d}",
     umbralK, MARGEN_MIN_MV, UMBRAL_OFF_FACTOR, umbralOn, umbralOff, calPocos ? 1 : 0, calInestable ? 1 : 0);
  ja(",\"sid\":%d,\"nAct\":%d,\"tAct\":%.1f,\"tSes\":%.1f,\"velE\":%.0f,\"cancion\":%d,\"orden\":%d,\"malas\":%lu,\"rssi\":0",
     sesionId, nActivaciones, tiempoActivoTotal / 1000.0f, msSesion / 1000.0f, velE, cancion, ordenP,
     lineasMalas);
  ja(",\"musica\":{\"carpeta\":%d,\"abajo\":[%d,%d,%d],\"arriba\":[%d,%d,%d],\"sonando\":%d,\"lista\":\"%s\",\"pista\":%d,\"aviso\":%d,\"avisoPista\":%d,\"vol\":%d}",
     CARPETA_MUSICA, pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2],
     musicaSonando ? 1 : 0, listaAbajoSonando ? "abajo" : "arriba", cancion, avisoSonando ? 1 : 0, PISTA_AVISO, volumen);
  ja(",\"bn\":%lu,\"b\":[", (unsigned long)nb);
  for (uint32_t i = 0; i < nb - b0; i++) ja(i ? ",[%ld,%.1f,%d]" : "[%ld,%.1f,%d]", (long)br[i], bm[i], bf[i]);
  ja("],\"an\":%lu,\"acts\":[", (unsigned long)na);
  for (uint32_t i = 0; i < na - a0; i++)
    ja(i ? ",[%d,%d,%.1f,%.1f,%d]" : "[%d,%d,%.1f,%.1f,%d]", av[i].sid, av[i].id, av[i].ini, av[i].dur, av[i].rms);
  ja("],\"ln\":%lu,\"logs\":[", (unsigned long)nl);
  for (uint32_t i = 0; i < nl - l0; i++) { if (i) ja(","); jaTexto(lt[i]); }
  ja("]}}");
}

// Una orden ya validada: "C x" (tecla del monitor serie), "M abajo 1,2,3" (lista de musica) o "V 20" (volumen)
void lineaDelServidor(const char *l) {
  if (l[0] == 'C' && l[1] == ' ' && l[2] && !l[3] && strchr("cprh+-xio", l[2])) {
    char ch = l[2];
    if (colaCmd) xQueueSend(colaCmd, &ch, 0);
  } else if (l[0] == 'M' && l[1] == ' ') {
    char lista[8]; int a = 0, b = 0, c = 0;
    if (sscanf(l + 2, "%7s %d,%d,%d", lista, &a, &b, &c) == 4 && (!strcmp(lista, "abajo") || !strcmp(lista, "arriba")) &&
        a >= 1 && a <= 255 && b >= 1 && b <= 255 && c >= 1 && c <= 255) {
      PedidoMusica m = {(uint8_t)(!strcmp(lista, "abajo") ? 1 : 0), {(uint8_t)a, (uint8_t)b, (uint8_t)c}};
      if (colaMusica) xQueueSend(colaMusica, &m, 0);
    }
  } else if (l[0] == 'V' && l[1] == ' ') {
    int v = atoi(l + 2);
    if (v >= 0 && v <= 30 && l[2] >= '0' && l[2] <= '9') { uint8_t b = (uint8_t)v; if (colaVolumen) xQueueSend(colaVolumen, &b, 0); }
  }
}

// ---------- La pagina en la RAM (se lee de LittleFS una sola vez, al encender) ----------
struct ArchivoWeb { char ruta[48]; const char *tipo; uint8_t *datos; size_t largo; bool gz; };
const int N_ARCHIVOS = 24;
ArchivoWeb archivos[N_ARCHIVOS];
int nArchivos = 0;
size_t bytesEnRam = 0;

const char *tipoDe(const char *r) {
  const char *e = strrchr(r, '.');
  if (!e) return "application/octet-stream";
  if (!strcmp(e, ".html")) return "text/html; charset=utf-8";
  if (!strcmp(e, ".css")) return "text/css; charset=utf-8";
  if (!strcmp(e, ".js")) return "text/javascript; charset=utf-8";
  if (!strcmp(e, ".json")) return "application/json";
  if (!strcmp(e, ".woff2")) return "font/woff2";
  if (!strcmp(e, ".svg")) return "image/svg+xml";
  if (!strcmp(e, ".png")) return "image/png";
  if (!strcmp(e, ".ico")) return "image/x-icon";
  if (!strcmp(e, ".txt")) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void cargarCarpeta(const char *carpeta, int nivel) {
  File d = LittleFS.open(carpeta);
  if (!d || !d.isDirectory() || nivel > 3) return;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String ruta = f.path();
    if (f.isDirectory()) { cargarCarpeta(ruta.c_str(), nivel + 1); continue; }
    if (nArchivos >= N_ARCHIVOS) break;
    bool gz = ruta.endsWith(".gz");
    if (gz) ruta = ruta.substring(0, ruta.length() - 3);
    ArchivoWeb &a = archivos[nArchivos];
    if (ruta.length() >= sizeof(a.ruta)) continue;
    strcpy(a.ruta, ruta.c_str());
    a.gz = gz; a.tipo = tipoDe(a.ruta); a.largo = f.size();
    a.datos = (uint8_t *)heap_caps_malloc(a.largo ? a.largo : 1, MALLOC_CAP_8BIT);
    if (a.datos && f.read(a.datos, a.largo) != a.largo) { free(a.datos); a.datos = NULL; }
    if (a.datos) bytesEnRam += a.largo;
    nArchivos++;
  }
}

// En setup (antes de mover los motores): monta LittleFS y copia la pagina a la RAM
void prepararPagina() {
  if (!LittleFS.begin(false)) {
    Serial.println("!! LittleFS no tiene la pagina: sube la carpeta data/ con \"Upload LittleFS\". El atril funciona igual.");
    return;
  }
  cargarCarpeta("/", 0);
  int enFlash = 0;
  for (int i = 0; i < nArchivos; i++) if (!archivos[i].datos) enFlash++;
  Serial.printf(">> Pagina: %d archivos (%u KB en RAM%s). RAM libre: %u KB\n", nArchivos, (unsigned)(bytesEnRam / 1024),
                enFlash ? ", algunos se leeran de la flash" : "", (unsigned)(ESP.getFreeHeap() / 1024));
  if (!nArchivos) Serial.println("!! LittleFS esta vacio: sube la carpeta data/ con \"Upload LittleFS\".");
}

ArchivoWeb *buscarArchivo(const char *ruta) {
  for (int i = 0; i < nArchivos; i++) if (!strcmp(archivos[i].ruta, ruta)) return &archivos[i];
  return NULL;
}

// ---------- Servidor web (tarea propia del servidor, nucleo 0) ----------
httpd_handle_t web = NULL;
volatile int numPaginas = 0;
volatile int enCola = 0;            // envios pendientes hacia las paginas

// Estado de la senal del Arduino (lo escribe la tarea de red; lo leen los demas)
volatile bool hayArduino = false;
uint32_t muestrasArduino = 0, paquetesArduino = 0, perdidosArduino = 0, bytesMalos = 0;   // solo los escribe la tarea de red
volatile float fsArduino = 0;
uint32_t arduinoDesde = 0;
char ipArduino[16] = "";
char diagArduino[256] = "";         // JSON que manda el Arduino cada segundo
portMUX_TYPE candadoDiag = portMUX_INITIALIZER_UNLOCKED;

const char *PAGINA_SIN_INTERFAZ =
  "<!doctype html><html lang=\"es\"><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
  "<title>ASTRA</title><body style=\"font-family:sans-serif;background:#0b1030;color:#eef;padding:24px\">"
  "<h1>ASTRA</h1><p>El atril esta encendido, pero todavia no tiene la pagina.</p>"
  "<p>Sube la carpeta <b>data/</b> del atril con <b>Upload LittleFS</b> en el Arduino IDE (ver docs/ASTRA_SIN_LAPTOP.md).</p>"
  "<p><a style=\"color:#5ee6d0\" href=\"/estado.json\">Diagnostico de la conexion</a></p></body></html>";

// Diagnostico de la red (va a la pagina cada segundo y en /estado.json). Escribe en 'o'
// (cada tarea usa su propio buffer). Devuelve el largo.
int armarServidor(char *o, int cap) {
  char diag[256];
  portENTER_CRITICAL(&candadoDiag);
  memcpy(diag, diagArduino, sizeof diag);
  portEXIT_CRITICAL(&candadoDiag);
  String ip = WiFi.softAPIP().toString();
  int n = snprintf(o, cap,
    "{\"type\":\"servidor\",\"red\":\"%s\",\"ips\":[\"%s\"],\"equipos\":%d,\"paginas\":%d,\"memoria\":%u,\"memoriaMin\":%u,\"t\":%lu"
    ",\"arduino\":{\"conectado\":%s,\"ip\":\"%s\",\"desde\":%lu,\"fs\":%.0f,\"paquetes\":%lu,\"perdidos\":%lu,\"malos\":%lu,\"diag\":%s}"
    ",\"atril\":{\"conectado\":true,\"ip\":\"%s\",\"desde\":0,\"v\":\"%s\"}}",
    RED_SSID, ip.c_str(), (int)WiFi.softAPgetStationNum(), numPaginas, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
    (unsigned long)millis(), hayArduino ? "true" : "false", ipArduino, (unsigned long)arduinoDesde, (double)fsArduino,
    (unsigned long)paquetesArduino, (unsigned long)perdidosArduino, (unsigned long)bytesMalos, diag[0] == '{' ? diag : "null",
    ip.c_str(), VERSION_ATRIL);
  return n < 0 ? 0 : min(n, cap - 1);
}

esp_err_t servirArchivo(httpd_req_t *req) {
  char ruta[64];
  size_t n = strcspn(req->uri, "?#");
  if (n >= sizeof ruta) n = sizeof ruta - 1;
  memcpy(ruta, req->uri, n);
  ruta[n] = 0;
  if (!strcmp(ruta, "/")) strcpy(ruta, "/index.html");
  ArchivoWeb *a = buscarArchivo(ruta);
  if (!a) {
    if (!buscarArchivo("/index.html") && !strcmp(ruta, "/index.html")) {
      httpd_resp_set_type(req, "text/html; charset=utf-8");
      return httpd_resp_send(req, PAGINA_SIN_INTERFAZ, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No existe");
  }
  httpd_resp_set_type(req, a->tipo);
  if (a->gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  if (a->datos) return httpd_resp_send(req, (const char *)a->datos, a->largo);
  // No cupo en la RAM: se lee de la flash en pedazos
  String nombre = String(a->ruta) + (a->gz ? ".gz" : "");
  File f = LittleFS.open(nombre, "r");
  if (!f) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No se pudo leer");
  static char trozo[1024];
  size_t k;
  while ((k = f.read((uint8_t *)trozo, sizeof trozo)) > 0)
    if (httpd_resp_send_chunk(req, trozo, k) != ESP_OK) { f.close(); return ESP_FAIL; }
  f.close();
  return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t servirEstado(httpd_req_t *req) {
  static char o[768];               // solo la usa la tarea del servidor web
  int n = armarServidor(o, sizeof o);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, o, n);
}

// Lee "clave":"texto" o "clave":numero de un JSON corto (los mensajes de la pagina)
const char *valorDe(const char *j, const char *clave) {
  char patron[24];
  snprintf(patron, sizeof patron, "\"%s\":", clave);
  const char *p = strstr(j, patron);
  if (!p) return NULL;
  p += strlen(patron);
  while (*p == ' ') p++;
  return p;
}
bool textoDe(const char *j, const char *clave, char *out, size_t n) {
  const char *p = valorDe(j, clave);
  if (!p || *p != '"') return false;
  p++;
  size_t k = 0;
  while (*p && *p != '"' && k < n - 1) out[k++] = *p++;
  out[k] = 0;
  return *p == '"';
}

// Un mensaje de la pagina. Se valida aqui y se encola: lo ejecuta el loop.
void mensajeDePagina(httpd_req_t *req, const char *m) {
  static uint32_t ventana = 0;
  static int cuenta = 0;
  uint32_t ahora = millis();
  if (ahora - ventana > 1000) { ventana = ahora; cuenta = 0; }
  if (++cuenta > 20) return;        // mas de 20 mensajes por segundo: se ignoran

  char tipo[24], t[8];
  if (!textoDe(m, "type", tipo, sizeof tipo)) return;
  char linea[32];
  if (!strcmp(tipo, "ping")) {      // para medir el retraso de la conexion
    const char *v = valorDe(m, "t");
    char r[64];
    int n = snprintf(r, sizeof r, "{\"type\":\"pong\",\"t\":%.3f,\"atril\":%lu}", v ? atof(v) : 0.0, (unsigned long)millis());
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof f);
    f.final = true; f.type = HTTPD_WS_TYPE_TEXT; f.payload = (uint8_t *)r; f.len = n;
    httpd_ws_send_frame(req, &f);
  } else if (!strcmp(tipo, "cmd")) {
    // De la pagina solo: recalibrar (c) y pausa/seguir (p). Nunca con el PARO activo.
    if (!textoDe(m, "c", t, sizeof t) || t[1] || !strchr("cp", t[0]) || !t[0]) return;
    if (modo == PARO) { avisar(-7); return; }
    snprintf(linea, sizeof linea, "C %c", t[0]);
    lineaDelServidor(linea);
  } else if (!strcmp(tipo, "musica")) {
    const char *p = valorDe(m, "p");
    int a, b, c;
    if (!textoDe(m, "l", t, sizeof t) || (strcmp(t, "abajo") && strcmp(t, "arriba")) || !p ||
        sscanf(p, "[%d,%d,%d]", &a, &b, &c) != 3) return;
    snprintf(linea, sizeof linea, "M %s %d,%d,%d", t, a, b, c);
    lineaDelServidor(linea);        // revisa que cada pista este entre 1 y 255
  } else if (!strcmp(tipo, "volumen")) {
    const char *v = valorDe(m, "v");
    if (!v || *v < '0' || *v > '9') return;
    snprintf(linea, sizeof linea, "V %d", atoi(v));
    lineaDelServidor(linea);        // revisa que este entre 0 y 30
  }
}

// /ws: la pagina se conecta aqui (WebSocket)
esp_err_t manejarWs(httpd_req_t *req) {
  if (req->method == HTTP_GET) return ESP_OK;   // saludo: queda abierto
  httpd_ws_frame_t f;
  memset(&f, 0, sizeof f);
  esp_err_t r = httpd_ws_recv_frame(req, &f, 0);  // primero solo el largo
  if (r != ESP_OK) return r;
  if (f.len > 512) return ESP_FAIL;               // demasiado largo: se cierra esa conexion
  static uint8_t buf[513];
  f.payload = buf;
  if (f.len) {
    r = httpd_ws_recv_frame(req, &f, 512);
    if (r != ESP_OK) return r;
  }
  buf[f.len] = 0;
  if (f.type == HTTPD_WS_TYPE_TEXT) mensajeDePagina(req, (const char *)buf);
  return ESP_OK;
}

// Envios a todas las paginas: los hace la tarea del servidor (httpd_queue_work)
struct Envio { httpd_ws_type_t tipo; size_t largo; uint8_t datos[]; };

// Conexiones que ya fallaron (celular que se fue o se congelo): se cierran y no se les vuelve
// a mandar nada; si no, cada envio pendiente esperaria send_wait_timeout y trabaria la pagina
// de los demas (los motores no: van en el otro nucleo).
static int fdsMalos[8];
static int nMalos = 0;
bool esMalo(int fd) { for (int i = 0; i < nMalos; i++) if (fdsMalos[i] == fd) return true; return false; }

void enviarATodos(void *arg) {
  Envio *e = (Envio *)arg;
  size_t n = 8;
  int fds[8];
  int paginas = 0;
  if (httpd_get_client_list(web, &n, fds) == ESP_OK) {
    // Olvida las conexiones malas que ya se cerraron
    for (int i = 0; i < nMalos;) {
      bool sigue = false;
      for (size_t k = 0; k < n; k++) if (fds[k] == fdsMalos[i]) sigue = true;
      if (sigue) i++; else fdsMalos[i] = fdsMalos[--nMalos];
    }
    // Si hay mas paginas abiertas que MAX_PAGINAS se cierran las sobrantes
    int total = 0;
    for (size_t i = 0; i < n; i++) if (httpd_ws_get_fd_info(web, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) total++;
    for (size_t i = 0; i < n; i++) {
      if (httpd_ws_get_fd_info(web, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET || esMalo(fds[i])) continue;
      if (total > MAX_PAGINAS) { httpd_sess_trigger_close(web, fds[i]); total--; continue; }
      paginas++;
      httpd_ws_frame_t f;
      memset(&f, 0, sizeof f);
      f.final = true; f.type = e->tipo; f.payload = e->datos; f.len = e->largo;
      if (httpd_ws_send_frame_async(web, fds[i], &f) != ESP_OK) {
        if (nMalos < 8) fdsMalos[nMalos++] = fds[i];
        httpd_sess_trigger_close(web, fds[i]);
        paginas--;
      }
    }
  }
  if (paginas != numPaginas) { avisar(paginas > numPaginas ? 5 : -5); numPaginas = paginas; }
  free(e);
  __atomic_sub_fetch(&enCola, 1, __ATOMIC_SEQ_CST);
}

// Encola un envio. Si las paginas no alcanzan a recibir (celular lento), se descarta: la
// pagina lo nota por el numero de secuencia de la senal. Nunca se espera.
bool mandar(httpd_ws_type_t tipo, const void *d, size_t n, int maxCola) {
  if (!web || !n || enCola >= maxCola) return false;
  Envio *e = (Envio *)malloc(sizeof(Envio) + n);
  if (!e) return false;
  e->tipo = tipo; e->largo = n;
  memcpy(e->datos, d, n);
  __atomic_add_fetch(&enCola, 1, __ATOMIC_SEQ_CST);
  if (httpd_queue_work(web, enviarATodos, e) != ESP_OK) { free(e); __atomic_sub_fetch(&enCola, 1, __ATOMIC_SEQ_CST); return false; }
  return true;
}

bool iniciarServidorWeb() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.core_id = 0;                  // nucleo 0: los motores (loop) estan en el 1
  cfg.task_priority = 3;
  cfg.stack_size = 6144;
  cfg.max_open_sockets = 8;        // + 3 internos del servidor + Arduino y UDP = 14 de los 16 sockets de lwIP
  cfg.max_uri_handlers = 4;
  cfg.lru_purge_enable = true;      // si se llena, cierra la conexion mas vieja
  cfg.recv_wait_timeout = 5;
  cfg.send_wait_timeout = 2;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&web, &cfg) != ESP_OK) { web = NULL; return false; }
  httpd_uri_t ws = {};
  ws.uri = "/ws"; ws.method = HTTP_GET; ws.handler = manejarWs; ws.is_websocket = true;
  httpd_uri_t estado = {};
  estado.uri = "/estado.json"; estado.method = HTTP_GET; estado.handler = servirEstado;
  httpd_uri_t todo = {};
  todo.uri = "/*"; todo.method = HTTP_GET; todo.handler = servirArchivo;
  httpd_register_uri_handler(web, &ws);
  httpd_register_uri_handler(web, &estado);
  httpd_register_uri_handler(web, &todo);
  return true;
}

// ---------- Senal del Arduino: paquetes 0xAA 0x55 (datos) y 0xAA 0x57 (diagnostico) ----------
static uint8_t pend[1024];          // bytes recibidos sin procesar
static int npend = 0;
static uint8_t emgOut[1400];        // paquetes validos para la pagina
static int nEmgOut = 0;
static int ultimoSeq = -1;

void procesarArduino() {
  int i = 0;
  while (i + 4 <= npend) {
    if (pend[i] != 0xAA) { i++; bytesMalos++; continue; }
    if (pend[i + 1] == 0x55) {
      int n = pend[i + 3], largo = 4 + 2 * n + 1;
      if (n == 0 || n > 64) { i++; bytesMalos++; continue; }
      if (i + largo > npend) break;
      uint8_t chk = 0;
      for (int k = i + 2; k < i + largo - 1; k++) chk ^= pend[k];
      if (chk != pend[i + largo - 1]) { i++; bytesMalos++; continue; }
      int seq = pend[i + 2];
      if (ultimoSeq >= 0) perdidosArduino += (uint8_t)(seq - ultimoSeq - 1);
      ultimoSeq = seq;
      muestrasArduino += n; paquetesArduino++;
      if (nEmgOut + largo > (int)sizeof emgOut) nEmgOut = 0;   // no deberia pasar: se manda cada 40 ms
      memcpy(emgOut + nEmgOut, pend + i, largo);
      nEmgOut += largo;
      i += largo;
    } else if (pend[i + 1] == 0x57) {
      int n = pend[i + 2], largo = 3 + n + 1;
      if (i + largo > npend) break;
      uint8_t chk = 0;
      for (int k = 0; k < n; k++) chk ^= pend[i + 3 + k];
      if (chk == pend[i + largo - 1] && n < (int)sizeof diagArduino && pend[i + 3] == '{') {
        bool limpio = true;
        for (int k = 0; k < n; k++) if (pend[i + 3 + k] < 0x20 || pend[i + 3 + k] > 0x7E) limpio = false;
        if (limpio) {
          portENTER_CRITICAL(&candadoDiag);
          memcpy(diagArduino, pend + i + 3, n);
          diagArduino[n] = 0;
          portEXIT_CRITICAL(&candadoDiag);
        }
      }
      i += largo;
    } else { i++; bytesMalos++; }
  }
  if (i) { memmove(pend, pend + i, npend - i); npend -= i; }
  if (npend >= (int)sizeof pend) npend = 0;    // basura: se descarta
}

void tareaRed(void *) {
  // 1) La red ASTRA (el Arduino y los celulares se unen a ella)
  WiFi.mode(WIFI_AP);
  bool redOk = strlen(RED_CLAVE) >= 8 && strlen(RED_CLAVE) <= 63 &&
               WiFi.softAP(RED_SSID, RED_CLAVE, CANAL_WIFI, 0, MAX_EQUIPOS_RED);
  if (!redOk) { avisar(-1); vTaskDelete(NULL); return; }
  WiFi.setSleep(false);             // respuesta rapida
  avisar(1);
  // 2) La pagina y el WebSocket
  avisar(iniciarServidorWeb() ? 2 : -2);
  // 3) La senal del Arduino
  WiFiServer srvArduino(PUERTO_ARDUINO);
  srvArduino.begin();
  srvArduino.setNoDelay(true);
  WiFiClient arduino;
  WiFiUDP udp;
  uint32_t tDato = 0, tEmg = 0, tEstado = 0, tSeg = 0, muestrasAntes = 0;
  uint32_t sb = 0, sa = 0, sl = 0;
  for (;;) {
    uint32_t ahora = millis();
    WiFiClient nuevo = srvArduino.accept();
    if (nuevo) {                    // una sola placa: la nueva conexion reemplaza a la vieja
      arduino.stop();
      arduino = nuevo;
      arduino.setNoDelay(true);
      npend = 0; ultimoSeq = -1; tDato = ahora;
      strncpy(ipArduino, arduino.remoteIP().toString().c_str(), sizeof ipArduino - 1);
      arduinoDesde = ahora;
      if (!hayArduino) { hayArduino = true; avisar(3); }
    }
    if (hayArduino) {
      int k = arduino.available();
      if (k > 0) {
        k = arduino.read(pend + npend, min(k, (int)sizeof(pend) - npend));
        if (k > 0) { npend += k; tDato = ahora; procesarArduino(); }
      }
      if (!arduino.connected() || ahora - tDato > 5000) {   // se fue (o conexion "fantasma")
        arduino.stop();
        hayArduino = false; fsArduino = 0;
        avisar(-3);
      }
    }
    if (nEmgOut && (ahora - tEmg >= 40 || nEmgOut > 1000)) {   // senal: tandas de ~40 ms
      tEmg = ahora;
      mandar(HTTPD_WS_TYPE_BINARY, emgOut, nEmgOut, 8);
      nEmgOut = 0;
    }
    if (ahora - tEstado >= 200) {   // estado del atril: 5 veces por segundo
      tEstado = ahora;
      if (numPaginas > 0 || enCola == 0) {
        armarEstado(sb, sa, sl);
        mandar(HTTPD_WS_TYPE_TEXT, js, jl, 12);
      }
    }
    if (ahora - tSeg >= 1000) {     // cada segundo: diagnostico + aviso UDP
      uint32_t m = muestrasArduino;
      fsArduino = hayArduino && tSeg ? (m - muestrasAntes) * 1000.0f / (ahora - tSeg) : 0;
      muestrasAntes = m;
      tSeg = ahora;
      static char o[768];
      mandar(HTTPD_WS_TYPE_TEXT, o, armarServidor(o, sizeof o), 12);
      if (udp.beginPacket(IPAddress(192, 168, 4, 255), PUERTO_AVISO)) {
        udp.print("ASTRA-SERVIDOR 9001 0\n");
        udp.endPacket();
      }
    }
    vTaskDelay(2);
  }
}

// ======================================================================
void setup() {""")

# El PARO tiene prioridad: las ordenes que vienen de la pagina no se ejecutan con el PARO activo
rep("""  while (colaCmd && xQueueReceive(colaCmd, &c, 0) == pdTRUE) ejecutarComando(c);""",
    """  while (colaCmd && xQueueReceive(colaCmd, &c, 0) == pdTRUE) {
    if (modo == PARO) avisar(-7);                // el PARO solo se libera con el boton fisico
    else ejecutarComando(c);
  }""")

# 13) setup: listas guardadas, colas, pagina y tarea de red
rep("""  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque
""", r"""  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque

  cargarListas();                                     // las listas que eligio la pagina
  Serial.printf(">> Musica: cabeza ABAJO %03d %03d %03d | cabeza ARRIBA %03d %03d %03d\n",
                pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2]);
  idArranque = (uint32_t)random(1, 2000000000);
  colaCmd = xQueueCreate(16, sizeof(char));
  colaMusica = xQueueCreate(4, sizeof(PedidoMusica));
  colaVolumen = xQueueCreate(4, sizeof(uint8_t));
  colaAvisos = xQueueCreate(16, sizeof(int8_t));
  if (USAR_WIFI) {
    prepararPagina();                                 // LittleFS -> RAM, antes de mover los motores
    xTaskCreatePinnedToCore(tareaRed, "red", 8192, NULL, 1, NULL, 0);  // nucleo 0
  }
""")
# =====================================================================
# v8.1: CALIBRACION ROBUSTA Y SENSIBILIDAD (cambios a la logica del atril, a pedido)
#  - El reposo y su dispersion se calculan con MEDIANA y MAD (1.4826 x MAD ~ SD) en vez
#    de media y SD: un movimiento, una risa o un espasmo breve durante los 10 s (comun en
#    paralisis cerebral) ya no suben el umbral.
#  - Umbral de activacion = reposo + el mayor de: k x SD robusta, 2 mV, o un % del reposo.
#    Normal: k = 3 y 20 % (antes k = 2 y 2 mV, demasiado cerca del ruido).
#  - Sensibilidad desde la pagina (1 = mas sensible, 2 = normal, 3 = menos sensible).
#  - Lo demas (mediana de 0.5 s, U_off, confirmaciones de 300 ms, tiempos) NO cambia.
# =====================================================================
r1 = rep
r1("const float UMBRAL_K_INICIAL = 2.0f; // factor sobre la SD basal (ajustable con +/- de 0.5 en 0.5)",
   """const float UMBRAL_K_INICIAL = 3.0f; // factor sobre la SD basal ROBUSTA (v8.1: antes 2.0; +/- de 0.5 en 0.5)
const float MARGEN_REL_INICIAL = 0.20f; // v8.1: el umbral queda al menos 20 % sobre el reposo
// v8.1: sensibilidad elegida en la pagina -> {k, margen relativo}
const float SENS_K[3]   = {2.0f, 3.0f, 4.0f};
const float SENS_REL[3] = {0.10f, 0.20f, 0.35f};
const int N_CAL_MAX = 160;              // v8.1: bloques que se guardan para la mediana (10 s = ~90)""")
r1("float umbralK = UMBRAL_K_INICIAL;", """float umbralK = UMBRAL_K_INICIAL;
float margenRel = MARGEN_REL_INICIAL;   // v8.1
int sensibilidad = 2;                   // v8.1: 1 mas sensible, 2 normal, 3 menos sensible
float calVals[N_CAL_MAX];               // v8.1: la mediana de cada bloque de la calibracion""")
r1("""void iniciarCalibracion() {
  cerrarActivacion();""", """// v8.1: sensibilidad elegida en la pagina (1, 2 o 3)
void aplicarSensibilidad(int n) {
  if (n < 1 || n > 3) return;
  sensibilidad = n; umbralK = SENS_K[n - 1]; margenRel = SENS_REL[n - 1];
  Log.printf(">> Sensibilidad %d (%s)\\n", n, n == 1 ? "mas sensible" : n == 2 ? "normal" : "menos sensible");
  if (calibrado) { calcularUmbrales(); imprimirCalibracion(); }
}

void iniciarCalibracion() {
  cerrarActivacion();""")
r1("""  umbralOn = reposo + max(umbralK * sdReposo, MARGEN_MIN_MV);""",
   """  umbralOn = reposo + max(max(umbralK * sdReposo, MARGEN_MIN_MV), margenRel * reposo);   // v8.1: + margen relativo""")
r1("""  Log.printf("Reposo: %.0f mV (SD %.1f)\\n", reposo, sdReposo);
  Log.printf("Umbral de activacion: %.1f mV (media + %.1f SD)\\n", umbralOn, umbralK);""",
   """  Log.printf("Reposo: %.0f mV (SD %.1f)\\n", reposo, sdReposo);
  Log.printf("Umbral de activacion: %.1f mV (mediana + %.1f SD robusta, minimo +%.0f %%; sensibilidad %d)\\n", umbralOn, umbralK, margenRel * 100, sensibilidad);""")
r1("""      suma2Reposo += (double)rmsSuave * rmsSuave;
      nReposo++;""", """      suma2Reposo += (double)rmsSuave * rmsSuave;
      if (nReposo < N_CAL_MAX) calVals[nReposo] = rmsSuave;   // v8.1
      nReposo++;""")
r1("""    double media = sumaReposo / nReposo;
    double var = suma2Reposo / nReposo - media * media;
    float sd = sqrtf((float)max(var, 0.0));""", """    // v8.1: mediana y MAD (robustas a un movimiento o espasmo breve); antes media y SD
    double media = medianaDe(calVals, min(nReposo, N_CAL_MAX), 0.0f, false);
    float sd = 1.4826f * medianaDe(calVals, min(nReposo, N_CAL_MAX), (float)media, true);""")
r1("""void calcularUmbrales() {""", """// v8.1: mediana de v[0..n) o, con abs = true, mediana de |v - centro| (MAD)
float medianaDe(const float *v, int n, float centro, bool abs) {
  static float t[N_CAL_MAX];
  if (n <= 0) return 0;
  for (int i = 0; i < n; i++) t[i] = abs ? fabsf(v[i] - centro) : v[i];
  for (int a = 1; a < n; a++) { float x = t[a]; int b = a - 1; while (b >= 0 && t[b] > x) { t[b + 1] = t[b]; b--; } t[b + 1] = x; }
  return n % 2 ? t[n / 2] : 0.5f * (t[n / 2 - 1] + t[n / 2]);
}

void calcularUmbrales() {""")
# cola de sensibilidad (pagina -> loop)
r1("""QueueHandle_t colaAvisos = NULL;    // mensajes de la red -> loop (los imprime)""",
   """QueueHandle_t colaAvisos = NULL;    // mensajes de la red -> loop (los imprime)
QueueHandle_t colaSens = NULL;      // v8.1: sensibilidad de la pagina -> loop
volatile bool senalPedida = true;   // v8.1: la pagina quiere ver la senal en vivo""")
r1("""  uint8_t vol;
  while (colaVolumen && xQueueReceive(colaVolumen, &vol, 0) == pdTRUE) aplicarVolumen(vol);""",
   """  uint8_t vol;
  while (colaVolumen && xQueueReceive(colaVolumen, &vol, 0) == pdTRUE) aplicarVolumen(vol);
  uint8_t sn;
  while (colaSens && xQueueReceive(colaSens, &sn, 0) == pdTRUE) aplicarSensibilidad(sn);   // v8.1""")
r1("""  colaAvisos = xQueueCreate(16, sizeof(int8_t));""", """  colaAvisos = xQueueCreate(16, sizeof(int8_t));
  colaSens = xQueueCreate(4, sizeof(uint8_t));      // v8.1""")
# mensajes nuevos de la pagina
r1("""  } else if (!strcmp(tipo, "volumen")) {""", """  } else if (!strcmp(tipo, "sensibilidad")) {      // v8.1: 1, 2 o 3
    const char *v = valorDe(m, "n");
    if (!v || *v < '1' || *v > '3' || (v[1] >= '0' && v[1] <= '9')) return;
    uint8_t n = *v - '0';
    if (colaSens) xQueueSend(colaSens, &n, 0);
  } else if (!strcmp(tipo, "senal")) {             // v8.1: ver o no la senal en vivo (para comparar el ruido)
    const char *v = valorDe(m, "on");
    if (!v) return;
    senalPedida = !strncmp(v, "true", 4) || *v == '1';
  } else if (!strcmp(tipo, "volumen")) {""")
# JSON: sensibilidad y margen relativo
r1("""  ja(",\\"k\\":%.1f,\\"margen\\":%.1f,\\"koff\\":%.2f,\\"uon\\":%.1f,\\"uoff\\":%.1f,\\"pocos\\":%d,\\"inestable\\":%d}",
     umbralK, MARGEN_MIN_MV, UMBRAL_OFF_FACTOR, umbralOn, umbralOff, calPocos ? 1 : 0, calInestable ? 1 : 0);""",
   """  ja(",\\"k\\":%.1f,\\"margen\\":%.1f,\\"rel\\":%.2f,\\"sens\\":%d,\\"koff\\":%.2f,\\"uon\\":%.1f,\\"uoff\\":%.1f,\\"pocos\\":%d,\\"inestable\\":%d}",
     umbralK, MARGEN_MIN_MV, margenRel, sensibilidad, UMBRAL_OFF_FACTOR, umbralOn, umbralOff, calPocos ? 1 : 0, calInestable ? 1 : 0);""")
# senal en vivo: el atril le dice al Arduino "S" o "N" (solo si hay paginas mirando)
r1("""      npend = 0; ultimoSeq = -1; tDato = ahora;""", """      npend = 0; ultimoSeq = -1; tDato = ahora;
      senalEnviada = -1;                          // v8.1: se le dice de nuevo si mandar la senal""")
r1("""  uint32_t sb = 0, sa = 0, sl = 0;
  for (;;) {
    uint32_t ahora = millis();""", """  uint32_t sb = 0, sa = 0, sl = 0;
  int senalEnviada = -1;                          // v8.1: ultima orden mandada al Arduino (1 = S, 0 = N)
  for (;;) {
    uint32_t ahora = millis();""")
r1("""    if (nEmgOut && (ahora - tEmg >= 40 || nEmgOut > 1000)) {   // senal: tandas de ~40 ms""",
   """    // v8.1: el Arduino solo transmite la senal si alguien la esta mirando (menos ruido por WiFi)
    if (hayArduino) {
      int quiero = senalPedida && numPaginas > 0 ? 1 : 0;
      if (quiero != senalEnviada) {
        uint8_t o = quiero ? 'S' : 'N';
        if (arduino.write(&o, 1) == 1) senalEnviada = quiero;
        if (quiero) ultimoSeq = -1;               // la senal vuelve a empezar: ese salto de numeracion no es perdida
      }
    }
    if (nEmgOut && (ahora - tEmg >= 40 || nEmgOut > 1000)) {   // senal: tandas de ~40 ms""")

# =====================================================================
# v8.2: DETECTAR TAMBIEN CUANDO LA CABEZA SUBE (a pedido)
#  Problema: el reposo se calibra con la cabeza APOYADA, pero en la terapia el nino SOSTIENE la
#  cabeza arriba y el semiespinoso trabaja un poco: ese nivel queda sobre el umbral de relajacion
#  y, tras la primera caida, el atril se quedaba en "cabeza abajo".
#  - Nivel "cabeza arriba" actual = mediana de los ultimos 3 s fuera de una activacion (nunca
#    menor que el reposo calibrado). Para una CAIDA la senal tiene que subir sobre ESE nivel el
#    mismo margen de siempre (k x SD robusta, 2 mV o el % de la sensibilidad).
#  - La caida TERMINA (cabeza arriba) cuando la senal baja al 40 % del camino entre el nivel de
#    arriba (antes de caer) y el pico de la caida, o bajo el umbral de relajacion, por 300 ms.
#  - Al empezar la sesion, tras recalibrar y CADA VEZ QUE LA CABEZA SUBE, aprende 1 s el nivel con
#    la cabeza arriba (en ese segundo no busca caidas). Asi se ajusta si el nino la sostiene con mas
#    o menos fuerza que en la calibracion.
#  - Si la cabeza arriba esta tan relajada como en la calibracion, todo es igual que en v8.1.
# =====================================================================
r1("const int N_CAL_MAX = 160;              // v8.1: bloques que se guardan para la mediana (10 s = ~90)",
   "const int N_CAL_MAX = 160;              // v8.1: bloques que se guardan para la mediana (10 s = ~90)\n"
   "const int N_ARRIBA = 30;                // v8.2: 3 s de medianas para el nivel con la cabeza arriba\n"
   "const float SUELTA_FRAC = 0.40f;        // v8.2: termina al bajar al 40 % del camino arriba -> pico")
r1("float calVals[N_CAL_MAX];               // v8.1: la mediana de cada bloque de la calibracion",
   "float calVals[N_CAL_MAX];               // v8.1: la mediana de cada bloque de la calibracion\n"
   "float histArriba[N_ARRIBA];             // v8.2: ultimas medianas fuera de una activacion\n"
   "int nArriba = 0, iArriba = 0;\n"
   "float baseAct = 0, uOnAct = 0, picoAct = 0;   // v8.2: de la activacion en curso\n"
   "volatile float uAhoraPub = 0, uSueltaPub = 0, arribaPub = 0;   // v8.2: para la pagina (los calcula el loop)")
r1("void calcularUmbrales() {\n  umbralOn = reposo",
   "// v8.2: nivel con la cabeza arriba (ultimos 3 s fuera de una activacion), nunca menor que el reposo\n"
   "float nivelArribaActual() {\n"
   "  if (nArriba < 10) return reposo;\n"
   "  return max(reposo, medianaDe(histArriba, nArriba, 0.0f, false));\n"
   "}\n"
   "float margenSobre(float base) { return max(max(umbralK * sdReposo, MARGEN_MIN_MV), margenRel * base); }\n"
   "float umbralOnAhora() { float b = nivelArribaActual(); return max(umbralOn, b + margenSobre(b)); }\n"
   "float umbralSueltaAhora() {\n"
   "  float off = baseAct + UMBRAL_OFF_FACTOR * (uOnAct - baseAct);\n"
   "  return max(off, baseAct + SUELTA_FRAC * (picoAct - baseAct));\n"
   "}\n"
   "void guardarArriba(float v) { histArriba[iArriba] = v; iArriba = (iArriba + 1) % N_ARRIBA; if (nArriba < N_ARRIBA) nArriba++; }\n"
   "\n"
   "void calcularUmbrales() {\n  umbralOn = reposo")
r1("  nReposo = 0;\n  sumaReposo = suma2Reposo = 0.0;",
   "  nReposo = 0;\n  nArriba = 0; iArriba = 0;                   // v8.2: el nivel de arriba se vuelve a medir\n  sumaReposo = suma2Reposo = 0.0;")
r1("  if (!activo) {\n    if (rmsSuave >= umbralOn) {",
   "  if (!activo) {\n"
   "    float uOnAhora = umbralOnAhora();             // v8.2: sobre el nivel actual con la cabeza arriba\n"
   "    uAhoraPub = uOnAhora; uSueltaPub = 0; arribaPub = nivelArribaActual();\n"
   "    if (rmsSuave >= uOnAhora && nArriba >= 10) {   // v8.2: primero aprende 1 s el nivel con la cabeza arriba")
r1("        activo = true;\n",
   "        activo = true;\n        baseAct = nivelArribaActual(); uOnAct = uOnAhora; picoAct = rmsSuave;   // v8.2\n")
r1("    } else tSobre = 0;",
   "    } else { tSobre = 0; guardarArriba(rmsSuave); }   // v8.2: guarda lo que queda bajo el umbral (o todo mientras aprende)")
r1("    if (rmsSuave < umbralOff) {",
   "    if (rmsSuave > picoAct) picoAct = rmsSuave;   // v8.2\n"
   "    float uSuelta = umbralSueltaAhora();\n"
   "    uSueltaPub = uSuelta;\n"
   "    if (rmsSuave < uSuelta) {                      // v8.2: antes rmsSuave < umbralOff")
r1("      if (ahora - tBajo + HOLGURA_MS >= T_CONFIRMAR) terminarActivacion(tBajo);",
   "      if (ahora - tBajo + HOLGURA_MS >= T_CONFIRMAR) {\n"
   "        terminarActivacion(tBajo);\n"
   "        nArriba = 0; iArriba = 0;                  // v8.2: la cabeza acaba de subir: aprende 1 s ese nivel\n"
   "      }")
r1('  ja(",\\"sid\\":%d,\\"nAct\\":%d,',
   '  ja(",\\"uAhora\\":%.1f,\\"uSuelta\\":%.1f,\\"arriba\\":%.1f", (float)uAhoraPub, (float)uSueltaPub, (float)arribaPub);   // v8.2\n'
   '  ja(",\\"sid\\":%d,\\"nAct\\":%d,')

# =====================================================================
# v8.2: ORDEN DE LAS CANCIONES (correccion de la usuaria): en la microSD las pistas 001-003 son
# las de la CABEZA ARRIBA y 004-006 las de la CABEZA ABAJO. Solo cambian los valores por defecto;
# la pagina elige "Cancion 1, 2 o 3" de cada lista y el atril la guarda en su memoria.
# =====================================================================
r1("int pistasAbajo[PISTAS_POR_LISTA]  = {1, 2, 3};   // /01/001, 002, 003 con la CABEZA ABAJO",
   "int pistasAbajo[PISTAS_POR_LISTA]  = {4, 5, 6};   // v8.2: /01/004, 005, 006 con la CABEZA ABAJO")
r1("int pistasArriba[PISTAS_POR_LISTA] = {4, 5, 6};   // /01/004, 005, 006 con la CABEZA ARRIBA",
   "int pistasArriba[PISTAS_POR_LISTA] = {1, 2, 3};   // v8.2: /01/001, 002, 003 con la CABEZA ARRIBA")

open(dst, 'w').write(s)
print('OK', dst)
