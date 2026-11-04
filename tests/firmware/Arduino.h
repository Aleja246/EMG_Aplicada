#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <string>
#include <deque>
#include <vector>
#include <map>
#include <algorithm>
using std::min; using std::max;
#define PI 3.14159265f
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define LED_BUILTIN 13
#define A0 14
#define SERIAL_8N1 0
template<class T> T constrain(T x, T a, T b){ return x<a?a:(x>b?b:x); }
extern uint32_t g_ms;
inline unsigned long millis(){ return g_ms; } inline unsigned long micros(){ return (unsigned long)g_ms*1000UL; }
inline void delay(unsigned long){} inline void delayMicroseconds(unsigned){}
inline int digitalRead(int){ return 1; } inline void digitalWrite(int,int){} inline void pinMode(int,int){} inline int analogRead(int){return 0;} inline void analogWrite(int,int){} inline void analogReadResolution(int){}
inline long random(long a,long b){ return a + 7 % (b-a); }
class String {
 public:
  std::string s;
  String(const char* c=""):s(c){} String(const std::string& c):s(c){}
  String operator+(const char* c) const { return String(s + c); }
  String operator+(const String& o) const { return String(s + o.s); }
  unsigned length() const { return s.size(); }
  char operator[](unsigned i) const { return s[i]; }
  long toInt() const { return atol(s.c_str()); } float toFloat() const { return (float)atof(s.c_str()); }
  const char* c_str() const { return s.c_str(); }
};
inline String operator+(const char* a, const String& b){ return String(std::string(a)+b.s); }
class Print { public: size_t write(uint8_t){return 1;} size_t write(const uint8_t*, size_t n){return n;} size_t print(const char*){return 0;} template<class T> size_t print(const T&){return 0;} size_t println(const char* =""){return 0;} size_t printf(const char*, ...) __attribute__((format(printf,2,3))){return 0;} };
class HardwareSerial : public Print { public: void begin(unsigned long, int=0, int=0, int=0, bool=false){} int available(){return 0;} int read(){return -1;} void setTxBufferSize(int){} int availableForWrite(){return 64;} operator bool(){return true;} };
extern HardwareSerial Serial, Serial1, Serial2;
class IPAddress { public: IPAddress(int,int,int,int){} IPAddress(){} String toString() const {return String();} };
