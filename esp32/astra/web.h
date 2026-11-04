// web.h — Punto de acceso WiFi + página + WebSocket en ws://192.168.4.1/ws.
// ESPAsyncWebServer atiende la red en su propia tarea: no frena el loop() ni los motores.
// Los comandos de la página NO se ejecutan aquí: se encolan y el loop() los toma,
// así todo el estado lo cambia una sola tarea.
#pragma once
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "pagina.h"

void escribirCsv(Print &o, const String &paciente, const String &fecha);   // en astra.ino

AsyncWebServer servidor(80);
AsyncWebSocket ws("/ws");

struct CmdWeb { char nombre[16]; float valor; bool hayValor; };
const int MAX_CMD = 8;
CmdWeb colaWeb[MAX_CMD];
int cmdIni = 0, cmdN = 0;
volatile bool clienteNuevo = false;

// Busca "clave": en un JSON sencillo y devuelve dónde empieza su valor (sin bibliotecas).
const char *jsonValor(const char *json, const char *clave) {
  char patron[20];
  snprintf(patron, sizeof patron, "\"%s\"", clave);
  const char *p = strstr(json, patron);
  if (!p) return nullptr;
  p += strlen(patron);
  while (*p == ' ' || *p == ':') p++;
  return p;
}

// {"cmd":"sens","k":3} -> nombre "sens", valor 3. Se llama desde la tarea de la red.
void encolarComando(const char *json) {
  CmdWeb c = {};
  const char *p = jsonValor(json, "cmd");
  if (!p || *p != '"') return;
  p++;
  int i = 0;
  while (*p && *p != '"' && i < (int)sizeof(c.nombre) - 1) c.nombre[i++] = *p++;
  c.nombre[i] = 0;
  const char *claves[] = {"k", "s", "on", "v"};
  for (const char *k : claves) {
    const char *q = jsonValor(json, k);
    if (q) { c.valor = strtof(q, nullptr); c.hayValor = true; break; }
  }
  portENTER_CRITICAL(&mux);
  if (cmdN < MAX_CMD) { colaWeb[(cmdIni + cmdN) % MAX_CMD] = c; cmdN++; }
  portEXIT_CRITICAL(&mux);
}

bool sacarComando(CmdWeb &c) {
  bool hay = false;
  portENTER_CRITICAL(&mux);
  if (cmdN) { c = colaWeb[cmdIni]; cmdIni = (cmdIni + 1) % MAX_CMD; cmdN--; hay = true; }
  portEXIT_CRITICAL(&mux);
  return hay;
}

void alEventoWs(AsyncWebSocket *s, AsyncWebSocketClient *cliente, AwsEventType tipo,
                void *arg, uint8_t *datos, size_t len) {
  if (tipo == WS_EVT_CONNECT) {
    clienteNuevo = true;                         // el loop le manda el estado completo
  } else if (tipo == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT && len < 128) {
      char buf[128];
      memcpy(buf, datos, len);
      buf[len] = 0;
      encolarComando(buf);
    }
  }
}

// Nombre de archivo seguro: "astra_Ana_Lopez_2026-11-04_10-30.csv" (sin acentos ni espacios).
String nombreArchivo(const String &paciente, const String &fecha) {
  static const char *CON = "\xA1\xA9\xAD\xB3\xBA\xB1\xBC\x81\x89\x8D\x93\x9A\x91\x9C";  // á é í ó ú ñ ü Á É Í Ó Ú Ñ Ü
  static const char *SIN = "aeiounuAEIOUNU";                                            // (UTF-8: 0xC3 + byte)
  String s = "astra";
  const String partes[] = {paciente, fecha};
  for (const String &parte : partes) {
    if (!parte.length()) continue;
    s += '_';
    for (unsigned i = 0; i < parte.length() && s.length() < 70; i++) {
      unsigned char c = parte[i];
      const char *p = (c == 0xC3 && i + 1 < parte.length()) ? strchr(CON, parte[i + 1]) : nullptr;
      if (p && *p) { s += SIN[p - CON]; i++; }
      else if (isalnum(c) || c == '-') s += (char)c;
      else if (s[s.length() - 1] != '_') s += '_';
    }
  }
  return s + ".csv";
}

void webIniciar() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_NOMBRE, WIFI_CLAVE);
  ws.onEvent(alEventoWs);
  servidor.addHandler(&ws);
  servidor.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "text/html", (const uint8_t *)PAGINA, sizeof(PAGINA) - 1);
  });
  // Resumen de la sesión: la página solo lo descarga, lo arma el ESP32.
  servidor.on("/resumen.csv", HTTP_GET, [](AsyncWebServerRequest *r) {
    String paciente = r->hasParam("p") ? r->getParam("p")->value() : String("");
    String fecha = r->hasParam("f") ? r->getParam("f")->value() : String("");
    AsyncResponseStream *resp = r->beginResponseStream("text/csv; charset=utf-8");
    String disposicion = String("attachment; filename=\"") + nombreArchivo(paciente, fecha) + "\"";
    resp->addHeader("Content-Disposition", disposicion.c_str());
    resp->addHeader("Cache-Control", "no-store");
    escribirCsv(*resp, paciente, fecha);
    r->send(resp);
  });
  servidor.onNotFound([](AsyncWebServerRequest *r) { r->send(404); });
  servidor.begin();
  Serial.printf(">> WiFi \"%s\" (clave %s). Página: http://%s\n", WIFI_NOMBRE, WIFI_CLAVE,
                WiFi.softAPIP().toString().c_str());
}

void webEnviar(const char *txt, size_t len) {
  if (ws.count() && ws.availableForWriteAll()) ws.textAll(txt, len);
}

void webMantener() {                             // libera clientes cerrados
  static unsigned long t = 0;
  if (millis() - t < 1000) return;
  t = millis();
  ws.cleanupClients();
}
