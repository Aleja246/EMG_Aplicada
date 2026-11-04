#pragma once
#include "Arduino.h"
enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_OPTIONS };
extern std::map<std::string,std::string> g_args; extern std::string g_body; extern int g_code;
class WebServer { public: WebServer(int){} void on(const char*, HTTPMethod, void(*)()){} void onNotFound(void(*)()){} void begin(){} void handleClient(){}
  bool hasArg(const char* k){return g_args.count(k)>0;} String arg(const char* k){ auto i=g_args.find(k); return String(i==g_args.end()?"":i->second.c_str()); }
  void sendHeader(const char*, const char*){} void send(int c, const char* =0, const String& b = String()){ g_code=c; g_body=b.s; } HTTPMethod method(){return HTTP_GET;} };
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)
typedef void* QueueHandle_t;
#define pdTRUE 1
QueueHandle_t xQueueCreate(int,int); int xQueueSend(QueueHandle_t, const void*, int); int xQueueReceive(QueueHandle_t, void*, int);
inline void vTaskDelay(int){} inline void xTaskCreatePinnedToCore(void(*)(void*), const char*, int, void*, int, void*, int){}
