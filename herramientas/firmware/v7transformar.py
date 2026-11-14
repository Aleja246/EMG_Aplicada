# Agrega la pagina web (WiFi, /estado, /cmd, /musica) al firmware del atril v5 con sonido.
# Uso: python3 transformar.py entrada.ino salida.ino
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()


def rep(a, b, n=1):
    global s
    assert s.count(a) == n, 'NO ENCONTRADO (%d): %s' % (s.count(a), a[:90])
    s = s.replace(a, b)


# 1) Serial.print -> Log.print (monitor serie + pagina), salvo la linea de estado de cada 0.5 s
i0 = s.index('void imprimirEstado() {')
i1 = s.index('\n}\n', i0) + 3
antes, estado, despues = s[:i0], s[i0:i1], s[i1:]
for a, b in (('Serial.println(', 'Log.println('), ('Serial.printf(', 'Log.printf('), ('Serial.print(', 'Log.print(')):
    antes = antes.replace(a, b)
    despues = despues.replace(a, b)
s = antes + estado + despues
rep('  Log.println();\n  Log.println("=== ASTRA - atril v5 ===");',
    '  Serial.println();\n  Serial.println("=== ASTRA - atril v7.1 (sonido + servidor de la pagina) ===");')

# 2) cabecera
rep("//  ASTRA - Firmware del atril (ESP32) v2.3", "//  ASTRA - Firmware del atril (ESP32) v7 (sonido + servidor de la pagina)")
rep("""//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca).
""", """//  Placa: "ESP32 Dev Module". Biblioteca: AccelStepper (el DFPlayer se maneja
//  con comandos directos, no necesita su biblioteca). WiFi y Preferences
//  vienen con el paquete de placas ESP32.
//
//  v7 = v5 con sonido + SERVIDOR DE LA PAGINA (la deteccion, la calibracion,
//  los motores, la luz y la logica de la musica NO cambian):
//   - Se une a la red WiFi del consultorio (RED_SSID / RED_CLAVE), encuentra
//     solo al servidor de la pagina (servidor/servidor.js en la laptop avisa
//     por UDP en el puerto 9999) y se conecta por TCP al puerto 9002.
//   - Cada 200 ms le manda una linea JSON con su estado: modo, calibracion,
//     RMS y mediana de cada bloque nuevo, activaciones nuevas, musica y los
//     mensajes nuevos del monitor serie. El servidor lo pasa a la pagina.
//   - Recibe lineas del servidor:  "C x"            = tecla x del monitor serie
//                                  "M abajo 1,2,3"  = pistas de una lista
//                                  "V 20"           = volumen (0 a 30)
//   - Las listas y el volumen elegidos en la pagina se guardan en la memoria
//     del ESP32 (Preferences): al volver a encender siguen igual, con o sin
//     pagina. Si la lista que cambias es la que suena, empieza ya con la nueva.
//   - La red corre en el nucleo 0; motores, luz, musica y deteccion siguen en
//     el nucleo 1: la pagina nunca los frena. Sin WiFi el atril funciona igual.
//   - Para la pagina se numera cada sesion (sube al terminar cada
//     calibracion) y se mide su tiempo sin contar pausas.
//   - Los mensajes (Log.print...) salen por el monitor serie Y a la pagina.
""")
rep("""// PAGINA WEB (futuro): las listas son variables; la pagina solo tiene que llamar
// a configurarPistas(true, a, b, c) para cabeza abajo y configurarPistas(false, a, b, c)
// para cabeza arriba (ver "Configuracion de las listas de musica" mas abajo).
""", """// PAGINA WEB: la pagina elige las 3 pistas de cada lista (/musica); el atril
// llama a configurarPistas() y las guarda en su memoria (ver aplicarMusica()).
""")

# 3) includes
rep("#include <AccelStepper.h>\n", "#include <AccelStepper.h>\n#include <WiFi.h>\n#include <WiFiUdp.h>\n#include <Preferences.h>\n#include <stdarg.h>\n")

# 4) constantes WiFi
rep("""const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta
""", """const unsigned long CORTE_LINEA_MS = 50;  // una linea con un hueco mayor a esto se descarta

// ------------------------- WiFi y servidor de la pagina ----------------
// La misma red WiFi que la laptop y el Arduino (la del consultorio, o la
// "zona con cobertura" de la laptop). Igual que en arduino_emg.ino.
const bool USAR_WIFI = true;
const char *RED_SSID  = "ASTRA";          // <-- nombre de la red WiFi
const char *RED_CLAVE = "astra2026";      // <-- su clave
// El servidor se encuentra solo (aviso UDP). Si la red no deja pasar ese
// aviso, escribe aqui la IP de la laptop (la muestra el servidor al abrir).
const char *SERVIDOR_IP = "";             // p. ej. "192.168.1.50"; vacio = buscarlo solo
const uint16_t PUERTO_SERVIDOR = 9002;
const uint16_t PUERTO_AVISO    = 9999;
const char *VERSION_ATRIL = "7.1";
""")

# 5) registro de mensajes + datos para la pagina
rep("""// ======================================================================
//  Utilidades
// ======================================================================
""", """// --- Pagina web ---
// Lo escribe el nucleo 1 (loop) y lo lee el nucleo 0 (red): se copia con candado.
portMUX_TYPE candado = portMUX_INITIALIZER_UNLOCKED;
const int N_LOG = 24, LARGO_LOG = 160;
char logTxt[N_LOG][LARGO_LOG];      // ultimos mensajes (los mismos del monitor serie)
uint32_t nLog = 0;
const int N_BLQ = 64;               // ultimos 6.4 s de bloques recibidos
int32_t blqRms[N_BLQ];
float blqMed[N_BLQ];
uint8_t blqFlag[N_BLQ];             // 1 = sobre el umbral (activacion), 2 = usado en la calibracion
uint32_t nBlq = 0;
struct Activacion { int sid, id; float ini, dur; int rms; };
const int N_ACT = 32;
Activacion actLista[N_ACT];
uint32_t nActLista = 0;
QueueHandle_t colaCmd = NULL;       // comandos de la pagina -> loop
struct PedidoMusica { uint8_t abajo; uint8_t p[3]; };
QueueHandle_t colaMusica = NULL;    // listas de musica de la pagina -> loop
QueueHandle_t colaVolumen = NULL;   // volumen de la pagina -> loop
int volumen = VOLUMEN;              // volumen en uso (0 a 30); VOLUMEN es el de fabrica
bool reiniciarLista = false;        // la lista que suena cambio: empezar ya con la nueva
Preferences prefs;
volatile int8_t avisoWifi = 0;      // 1/-1 red, 2/-2 servidor (lo imprime el loop)
IPAddress ipServidor;
uint32_t idArranque = 0;            // cambia en cada encendido (la pagina lo detecta)
int sesionId = 0;                   // sube al terminar cada calibracion (sesion nueva)
unsigned long msSesion = 0;         // tiempo en SESION (sin pausas)
unsigned long msSesionIni = 0;      // msSesion al empezar la activacion en curso
int calN = 0;                       // bloques usados en la ultima calibracion
bool calPocos = false, calInestable = false;

// Imprime en el monitor serie y guarda cada linea para la pagina
class Registro : public Print {
 public:
  size_t write(uint8_t c) override {
    Serial.write(c);
    if (c == '\\r') return 1;
    if (c == '\\n') { cerrar(); return 1; }
    if (n < LARGO_LOG - 1) buf[n++] = (char)c;
    return 1;
  }
  size_t write(const uint8_t *b, size_t k) override { for (size_t i = 0; i < k; i++) write(b[i]); return k; }
 private:
  char buf[LARGO_LOG];
  int n = 0;
  void cerrar() {
    if (!n) return;                 // las lineas vacias solo van al monitor
    buf[n] = 0;
    portENTER_CRITICAL(&candado);
    memcpy(logTxt[nLog % N_LOG], buf, n + 1);
    nLog++;
    portEXIT_CRITICAL(&candado);
    n = 0;
  }
};
Registro Log;

void guardarBloque(int mv, float med, uint8_t flag) {
  portENTER_CRITICAL(&candado);
  blqRms[nBlq % N_BLQ] = mv;
  blqMed[nBlq % N_BLQ] = med;
  blqFlag[nBlq % N_BLQ] = flag;
  nBlq++;
  portEXIT_CRITICAL(&candado);
}

// ======================================================================
//  Utilidades
// ======================================================================
""")

# 6) activaciones para la pagina
rep("""    Log.printf("   Activacion #%d: %.1f s, RMS medio %.0f mV\\n",
                  nActivaciones, dur / 1000.0f, rmsMedio);
""", """    Log.printf("   Activacion #%d: %.1f s, RMS medio %.0f mV\\n",
                  nActivaciones, dur / 1000.0f, rmsMedio);
    Activacion a = {sesionId, nActivaciones, msSesionIni / 1000.0f, dur / 1000.0f, (int)lroundf(rmsMedio)};
    portENTER_CRITICAL(&candado);
    actLista[nActLista % N_ACT] = a;
    nActLista++;
    portEXIT_CRITICAL(&candado);
""")
rep("""      if (!tSobre) { tSobre = ahora; sumaRmsAct = 0; nRmsAct = 0; }""",
    """      if (!tSobre) { tSobre = ahora; sumaRmsAct = 0; nRmsAct = 0; msSesionIni = msSesion; }""")

# 7) sesion nueva al terminar la calibracion + datos de calidad
rep("""void terminarCalibracion() {
  if (nReposo < CAL_MIN_MUESTRAS) {""", """void terminarCalibracion() {
  calN = nReposo;
  calPocos = nReposo < CAL_MIN_MUESTRAS;
  calInestable = false;
  sesionId++;                        // para la pagina: sesion nueva desde cero
  msSesion = 0;
  if (nReposo < CAL_MIN_MUESTRAS) {""")
rep("""    if (media > 0 && (sd / media) > CAL_CV_MAX) {""", """    calInestable = media > 0 && (sd / media) > CAL_CV_MAX;
    if (calInestable) {""")

# 8) bloques para la pagina
rep("""  if (modo == CALIBRANDO) {
    // Se calibra sobre la misma mediana que se compara despues (el primer segundo no cuenta)
    if (ahora - tCal >= CAL_DESCARTAR_MS && ventanaN == N_MEDIANA) {
      sumaReposo += rmsSuave;
      suma2Reposo += (double)rmsSuave * rmsSuave;
      nReposo++;
    }
    return;
  }
  if (modo != SESION) return;
""", """  if (modo == CALIBRANDO) {
    // Se calibra sobre la misma mediana que se compara despues (el primer segundo no cuenta)
    bool usado = false;
    if (ahora - tCal >= CAL_DESCARTAR_MS && ventanaN == N_MEDIANA) {
      sumaReposo += rmsSuave;
      suma2Reposo += (double)rmsSuave * rmsSuave;
      nReposo++;
      usado = true;
    }
    guardarBloque(mv, rmsSuave, usado ? 2 : 0);
    return;
  }
  if (modo != SESION) { guardarBloque(mv, rmsSuave, 0); return; }
""")
rep("""      sumaRmsAct += mv; nRmsAct++;
    }
  }

}""", """      sumaRmsAct += mv; nRmsAct++;
    }
  }
  guardarBloque(mv, rmsSuave, activo ? 1 : 0);
}""")

# 9) listas de musica desde la pagina (se guardan en la memoria del ESP32)
rep("""// Toca una pista de la lista de cabeza abajo / arriba (carpeta /01).""",
    """// Listas guardadas en la memoria del ESP32 (las eligio la pagina)
void cargarListas() {
  uint8_t a[PISTAS_POR_LISTA], b[PISTAS_POR_LISTA];
  prefs.begin("astra", false);
  size_t na = prefs.getBytes("abajo", a, sizeof a), nb = prefs.getBytes("arriba", b, sizeof b);
  volumen = constrain((int)prefs.getUChar("vol", VOLUMEN), 0, 30);
  prefs.end();
  if (na == sizeof a && a[0] && a[1] && a[2]) for (int k = 0; k < PISTAS_POR_LISTA; k++) pistasAbajo[k] = a[k];
  if (nb == sizeof b && b[0] && b[1] && b[2]) for (int k = 0; k < PISTAS_POR_LISTA; k++) pistasArriba[k] = b[k];
}

void guardarLista(bool abajo) {
  uint8_t v[PISTAS_POR_LISTA];
  for (int k = 0; k < PISTAS_POR_LISTA; k++) v[k] = (uint8_t)(abajo ? pistasAbajo : pistasArriba)[k];
  prefs.begin("astra", false);
  prefs.putBytes(abajo ? "abajo" : "arriba", v, sizeof v);
  prefs.end();
}

// Pedido de la pagina: cambia, guarda y, si esa lista es la que suena, empieza ya con ella
void aplicarMusica(const PedidoMusica &m) {
  if (!configurarPistas(m.abajo, m.p[0], m.p[1], m.p[2])) return;
  guardarLista(m.abajo);
  if (musicaSonando && (bool)m.abajo == listaAbajoSonando) reiniciarLista = true;
}

// Volumen pedido por la pagina (0 a 30): se aplica ya y se guarda
void aplicarVolumen(int v) {
  volumen = constrain(v, 0, 30);
  dfEnviar(0x06, volumen);
  ultimoCmdMusica = millis();
  prefs.begin("astra", false);
  prefs.putUChar("vol", (uint8_t)volumen);
  prefs.end();
  Log.printf(">> Volumen: %d de 30\\n", volumen);
}

// Toca una pista de la lista de cabeza abajo / arriba (carpeta /01).""")
# el reenvio del volumen usa el volumen elegido
rep("    dfEnviar(0x06, VOLUMEN);", "    dfEnviar(0x06, volumen);")
rep("""      cambiarCancion = avisoSonando = pistaTerminada = false;""",
    """      cambiarCancion = avisoSonando = pistaTerminada = false;
      reiniciarLista = false;""")
rep("""    } else if (pistaTerminada) {
      tocarLista(listaAbajoSonando, true);       // termino la pista: siguiente de la misma lista""",
    """    } else if (reiniciarLista && !avisoSonando) {
      reiniciarLista = false;                    // la pagina cambio la lista que suena: empieza con la nueva
      (listaAbajoSonando ? idxAbajo : idxArriba) = -1;
      tocarLista(listaAbajoSonando, false);
    } else if (pistaTerminada) {
      tocarLista(listaAbajoSonando, true);       // termino la pista: siguiente de la misma lista""")

# 10) comandos: serie y pagina por el mismo camino
rep("""void leerComandos() {
  while (Serial.available()) {
    char c = Serial.read();
    switch (c) {""", """// Comandos del monitor serie y de la pagina (todos pasan por aqui, en el loop)
void ejecutarComando(char c) {
    switch (c) {""")
rep("""      case 'o': cambiarOrdenPlanetas(); break;
      default: break;
    }
  }
}""", """      case 'o': cambiarOrdenPlanetas(); break;
      default: break;
    }
}

void leerComandos() {
  while (Serial.available()) ejecutarComando(Serial.read());
  char c;
  while (colaCmd && xQueueReceive(colaCmd, &c, 0) == pdTRUE) ejecutarComando(c);
  PedidoMusica m;
  while (colaMusica && xQueueReceive(colaMusica, &m, 0) == pdTRUE) aplicarMusica(m);
  uint8_t vol;
  while (colaVolumen && xQueueReceive(colaVolumen, &vol, 0) == pdTRUE) aplicarVolumen(vol);
}""")

# 11) tiempo de sesion y avisos de WiFi
rep("""void actualizarModo() {
  unsigned long ahora = millis();
""", """void actualizarModo() {
  unsigned long ahora = millis();

  // Tiempo de sesion para la pagina (sin pausas)
  static unsigned long tPrevio = 0;
  if (modo == SESION && tPrevio) msSesion += ahora - tPrevio;
  tPrevio = ahora;

  // Avisos de la red (los pone la tarea de WiFi; se imprimen aqui)
  int8_t aw = avisoWifi;
  if (aw) {
    avisoWifi = 0;
    if (aw == 1) Log.printf(">> WiFi conectado a la red %s (IP del atril: %s). Buscando el servidor de la pagina...\\n", RED_SSID, WiFi.localIP().toString().c_str());
    else if (aw == 2) Log.printf(">> Conectado al servidor de la pagina (%s).\\n", ipServidor.toString().c_str());
    else if (aw == -2) Log.println("!! Se cerro la conexion con el servidor de la pagina (reintentando).");
    else Log.printf("!! Se perdio la red WiFi %s (el atril sigue funcionando; reintentando).\\n", RED_SSID);
  }
""")

# 12) conexion con el servidor de la pagina
rep("""// ======================================================================
void setup() {""", """// ======================================================================
//  Servidor de la pagina (nucleo 0). Solo lee el estado y encola pedidos:
//  todo lo que cambia el atril lo hace el loop (ejecutarComando / aplicarMusica).
// ======================================================================
static char js[8192];               // una linea JSON para el servidor
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
  ja("\\"");
  for (; *t && jl < (int)sizeof(js) - 8; t++) {
    unsigned char c = *t;
    if (c == '"' || c == '\\\\') ja("\\\\%c", c);
    else if (c < 0x20) ja(" ");
    else { js[jl++] = c; js[jl] = 0; }
  }
  ja("\\"");
}

// Primer indice a mandar: lo pendiente, dentro de lo que se guarda
uint32_t desde(uint32_t pedido, uint32_t total, uint32_t guardados) {
  uint32_t ini = total > guardados ? total - guardados : 0;
  if (pedido < ini) pedido = ini;
  if (pedido > total) pedido = total;
  return pedido;
}

// Arma en js el estado + lo nuevo desde los indices sb, sa, sl (y los avanza)
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
  ja("{\\"v\\":\\"%s\\",\\"boot\\":%lu,\\"modo\\":\\"%s\\",\\"datos\\":%d,\\"rms\\":%d,\\"med\\":%.1f",
     VERSION_ATRIL, (unsigned long)idArranque, NOMBRE_MODO[modo], datosOK() ? 1 : 0, rmsUltimo, rmsSuave);
  ja(",\\"activo\\":%d,\\"abajo\\":%d,\\"abajoAct\\":%d,\\"nivel\\":%d,\\"durAct\\":%.1f",
     act ? 1 : 0, cabezaAbajo() ? 1 : 0, abajoEsActividad ? 1 : 0, (int)(nivel * 100),
     act ? (ahora - tInicioActivo) / 1000.0f : 0.0f);
  ja(",\\"cal\\":{\\"ok\\":%d,\\"restante\\":%.1f,\\"total\\":%.1f,\\"desc\\":%.1f,\\"n\\":%d,\\"nMin\\":%d,\\"reposo\\":%.1f,\\"sd\\":%.2f,\\"cv\\":%.3f,\\"cvMax\\":%.2f",
     calibrado ? 1 : 0, restante, CAL_MS / 1000.0f, CAL_DESCARTAR_MS / 1000.0f, modo == CALIBRANDO ? nReposo : calN,
     CAL_MIN_MUESTRAS, reposo, sdReposo, reposo > 0 ? sdReposo / reposo : 0.0f, CAL_CV_MAX);
  ja(",\\"k\\":%.1f,\\"margen\\":%.1f,\\"koff\\":%.2f,\\"uon\\":%.1f,\\"uoff\\":%.1f,\\"pocos\\":%d,\\"inestable\\":%d}",
     umbralK, MARGEN_MIN_MV, UMBRAL_OFF_FACTOR, umbralOn, umbralOff, calPocos ? 1 : 0, calInestable ? 1 : 0);
  ja(",\\"sid\\":%d,\\"nAct\\":%d,\\"tAct\\":%.1f,\\"tSes\\":%.1f,\\"velE\\":%.0f,\\"cancion\\":%d,\\"orden\\":%d,\\"malas\\":%lu,\\"rssi\\":%d",
     sesionId, nActivaciones, tiempoActivoTotal / 1000.0f, msSesion / 1000.0f, velE, cancion, ordenP,
     lineasMalas, (int)WiFi.RSSI());
  ja(",\\"musica\\":{\\"carpeta\\":%d,\\"abajo\\":[%d,%d,%d],\\"arriba\\":[%d,%d,%d],\\"sonando\\":%d,\\"lista\\":\\"%s\\",\\"pista\\":%d,\\"aviso\\":%d,\\"avisoPista\\":%d,\\"vol\\":%d}",
     CARPETA_MUSICA, pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2],
     musicaSonando ? 1 : 0, listaAbajoSonando ? "abajo" : "arriba", cancion, avisoSonando ? 1 : 0, PISTA_AVISO, volumen);
  ja(",\\"bn\\":%lu,\\"b\\":[", (unsigned long)nb);
  for (uint32_t i = 0; i < nb - b0; i++) ja(i ? ",[%ld,%.1f,%d]" : "[%ld,%.1f,%d]", (long)br[i], bm[i], bf[i]);
  ja("],\\"an\\":%lu,\\"acts\\":[", (unsigned long)na);
  for (uint32_t i = 0; i < na - a0; i++)
    ja(i ? ",[%d,%d,%.1f,%.1f,%d]" : "[%d,%d,%.1f,%.1f,%d]", av[i].sid, av[i].id, av[i].ini, av[i].dur, av[i].rms);
  ja("],\\"ln\\":%lu,\\"logs\\":[", (unsigned long)nl);
  for (uint32_t i = 0; i < nl - l0; i++) { if (i) ja(","); jaTexto(lt[i]); }
  ja("]}\\n");
}

// Una linea del servidor: "C x" (tecla del monitor serie) o "M abajo 1,2,3" (lista de musica)
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

void tareaRed(void *) {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);              // respuesta rapida
  WiFi.setAutoReconnect(true);
  WiFi.begin(RED_SSID, RED_CLAVE);
  WiFiUDP udp;
  WiFiClient cli;
  bool red = false, conectado = false, hayServidor = false;
  unsigned long tIntentoRed = millis(), tAviso = 0, tIntento = 0, tEnvio = 0;
  uint32_t sb = 0, sa = 0, sl = 0;
  char lin[96]; int nlin = 0;
  for (;;) {
    unsigned long ahora = millis();
    bool r = WiFi.status() == WL_CONNECTED;
    if (r != red) {
      red = r; avisoWifi = r ? 1 : -1; tIntentoRed = ahora;
      if (r) {
        udp.begin(PUERTO_AVISO);
        if (SERVIDOR_IP[0] && ipServidor.fromString(SERVIDOR_IP)) { hayServidor = true; tAviso = ahora; }
      }
    }
    if (!r && ahora - tIntentoRed > 15000) {     // sin red por 15 s: vuelve a intentar
      tIntentoRed = ahora;
      WiFi.disconnect();
      WiFi.begin(RED_SSID, RED_CLAVE);
    }
    // Aviso del servidor: "ASTRA-SERVIDOR 9001 9002"
    if (r && udp.parsePacket() > 0) {
      char t[48]; int k = udp.read(t, sizeof(t) - 1); if (k < 0) k = 0; t[k] = 0;
      if (!strncmp(t, "ASTRA-SERVIDOR", 14)) { ipServidor = udp.remoteIP(); hayServidor = true; tAviso = ahora; }
    }
    // Conexion TCP con el servidor
    if (conectado && !cli.connected()) { conectado = false; cli.stop(); avisoWifi = -2; }
    if (!conectado && r && hayServidor && ahora - tIntento >= 3000 && (SERVIDOR_IP[0] || ahora - tAviso < 5000)) {
      tIntento = ahora;
      if (cli.connect(ipServidor, PUERTO_SERVIDOR, 2000)) {
        cli.setNoDelay(true);
        conectado = true; avisoWifi = 2;
        sb = sa = sl = 0;                        // le manda lo que tenga guardado (el servidor quita lo repetido)
        nlin = 0;
      }
    }
    if (conectado) {
      while (cli.available()) {                 // ordenes del servidor, una por linea
        char ch = cli.read();
        if (ch == '\\n') { lin[nlin] = 0; lineaDelServidor(lin); nlin = 0; }
        else if (ch != '\\r' && nlin < (int)sizeof(lin) - 1) lin[nlin++] = ch;
      }
      if (ahora - tEnvio >= 200) {               // el estado sale 5 veces por segundo
        tEnvio = ahora;
        armarEstado(sb, sa, sl);
        if (cli.write((const uint8_t *)js, jl) != (size_t)jl) { conectado = false; cli.stop(); avisoWifi = -2; }
      }
    }
    vTaskDelay(5);
  }
}

// ======================================================================
void setup() {""")

# 13) setup: listas guardadas, colas y tarea de red
rep("""  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque
""", """  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);     // DFPlayer
  // El volumen se manda 2 s despues (iniciarArranque), sin detener el arranque

  cargarListas();                                     // las listas que eligio la pagina
  Serial.printf(">> Musica: cabeza ABAJO %03d %03d %03d | cabeza ARRIBA %03d %03d %03d\\n",
                pistasAbajo[0], pistasAbajo[1], pistasAbajo[2], pistasArriba[0], pistasArriba[1], pistasArriba[2]);
  idArranque = (uint32_t)random(1, 2000000000);
  colaCmd = xQueueCreate(16, sizeof(char));
  colaMusica = xQueueCreate(4, sizeof(PedidoMusica));
  colaVolumen = xQueueCreate(4, sizeof(uint8_t));
  if (USAR_WIFI) xTaskCreatePinnedToCore(tareaRed, "red", 8192, NULL, 1, NULL, 0);  // nucleo 0
""")
open(dst, 'w').write(s)
print('OK', dst)
