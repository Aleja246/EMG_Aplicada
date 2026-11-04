#pragma once
#include "Arduino.h"
#define WIFI_STA 1
#define WL_CONNECTED 3
class WiFiClass { public: void mode(int){} void setSleep(bool){} bool config(IPAddress,IPAddress,IPAddress){return true;} void setAutoReconnect(bool){} void begin(const char*,const char*){} int status(){return 3;} void disconnect(){} IPAddress localIP(){return IPAddress();} int8_t RSSI(){return -50;} };
extern WiFiClass WiFi;
