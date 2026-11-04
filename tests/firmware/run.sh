#!/bin/bash
# Ejecuta el firmware REAL del ESP32 (esp32_calibracion.ino) en el PC, con reloj virtual y cabeceras de prueba
# (no necesita Arduino IDE ni la placa). Requiere g++.   Uso:  bash tests/firmware/run.sh
cd "$(dirname "$0")" || exit 1
g++ -std=gnu++17 -O1 -Wall -Wno-unused -Wno-sign-compare -I. test_esp.cpp -o /tmp/test_esp_astra || exit 1
fallo=0
for t in base calidad sostenido transicion largo prolongado sinpagina pausa respuestas expira umbral control json; do
  /tmp/test_esp_astra $t || fallo=1
done
rm -f json_*.json
exit $fallo
