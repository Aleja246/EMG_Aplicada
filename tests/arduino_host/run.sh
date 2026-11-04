#!/bin/sh
# Pruebas en la PC (no necesitan la placa). Uso: sh tests/arduino_host/run.sh
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/emg_host_tests; mkdir -p "$OUT"
FL="-std=gnu++11 -O1 -ffp-contract=off -Wall -Wno-unused-function -Imock"
g++ $FL comun.cpp v2_harness.cpp -o "$OUT/v2"
g++ $FL comun.cpp v3_harness.cpp -o "$OUT/v3"
g++ -std=gnu++11 -O1 -Wall -Wno-unused-function flujo_test.cpp -o "$OUT/flujo"
"$OUT/v2" "$OUT/opto_v2.txt" "$OUT/usb_v2.txt"
"$OUT/v3" "$OUT/opto_v3.txt" "$OUT/usb_v3.txt"
echo "Opto v2: $(wc -l < "$OUT/opto_v2.txt") bytes, v3: $(wc -l < "$OUT/opto_v3.txt") bytes"
cmp "$OUT/opto_v2.txt" "$OUT/opto_v3.txt" && echo "OK: bytes al opto IDENTICOS (mismo byte en la misma muestra)"
cmp "$OUT/usb_v2.txt" "$OUT/usb_v3.txt" && echo "OK: lineas USB IDENTICAS ($(grep -c SinArm "$OUT/usb_v2.txt") lineas)"
"$OUT/flujo"

# El sketch v3 completo contra las firmas reales de WiFiS3 / FspTimer / nucleo (api_mock/)
T=$(mktemp -d); D=../../arduino/diagnostico_emg_enlace_wifi
cp $D/*.h $D/*.ino "$T/"; cp "$T/arduino_secrets_ejemplo.h" "$T/arduino_secrets.h"
g++ -std=gnu++11 -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Iapi_mock -include api_mock/Arduino.h -x c++ "$T/diagnostico_emg_enlace_wifi.ino" \
  && echo "OK: el sketch v3 usa la API de WiFiS3/FspTimer con los tipos correctos (no reemplaza compilar para la placa)"
rm -rf "$T"
