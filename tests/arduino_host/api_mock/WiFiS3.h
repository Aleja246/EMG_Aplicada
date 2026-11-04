#pragma once
#include "Arduino.h"
#define WL_NO_SHIELD 255
enum wl_status_t { WL_NO_MODULE = WL_NO_SHIELD, WL_IDLE_STATUS = 0, WL_NO_SSID_AVAIL, WL_SCAN_COMPLETED, WL_CONNECTED, WL_CONNECT_FAILED, WL_CONNECTION_LOST, WL_DISCONNECTED };
class CWifi {
 public:
  static const char *firmwareVersion();
  int begin(const char *ssid, const char *passphrase);
  IPAddress localIP();
  int32_t RSSI();
  uint8_t status();
};
extern CWifi WiFi;
class WiFiClient {
 public:
  virtual int connect(IPAddress ip, uint16_t port);
  virtual int connect(const char *host, uint16_t port);
  virtual size_t write(uint8_t);
  virtual size_t write(const uint8_t *buf, size_t size);
  virtual void stop();
  void setConnectionTimeout(int timeout);
};
class WiFiUDP {
 public:
  virtual uint8_t begin(uint16_t);
  virtual void stop();
  virtual int parsePacket();
  virtual int read(unsigned char *buffer, size_t len);
  virtual IPAddress remoteIP();
};
