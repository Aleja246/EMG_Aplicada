#include "Arduino.h"
namespace mock { std::vector<int> adc; size_t lecturas = 0; uint32_t ahora_us = 0; std::vector<Evento> opto; std::string usb; }
MockSerial Serial{0}, Serial1{1};
