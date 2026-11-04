# EMG_Aplicada
Monitor sEMG cervical para Atlas2030: interfaz web para fisioterapeutas (registro/búsqueda de pacientes,
calibración en 3 fases, señal y envolvente en tiempo real, métricas por activación e historial de progreso).

## Cómo probarlo (sin Arduino)
1. Abre `index.html` en **Google Chrome o Microsoft Edge** (doble clic).
2. Pulsa **Simulación** (arriba a la derecha), registra un paciente de prueba y haz la terapia.

## Con el Arduino UNO R4 WiFi del ASTRA (inalámbrico)
1. Copia `arduino/diagnostico_emg_enlace_wifi/arduino_secrets_ejemplo.h` como `arduino_secrets.h`, pon tu red y carga `diagnostico_emg_enlace_wifi.ino`.
2. En la laptop: `python puente/puente_emg.py` (déjalo abierto).
3. En la página pulsa **Arduino WiFi** → **Monitor y registro de señal** → **Iniciar registro** / **Descargar CSV**.

El RMS al ESP32 del atril por el opto no cambia. Guía completa, arquitectura y pruebas: [`docs/WIFI.md`](docs/WIFI.md).

## Con el Arduino por USB
1. Sube `arduino/emg_stream/emg_stream.ino` a la placa (cierra el Monitor Serie después).
2. Abre la página (en GitHub Pages o en `localhost`; Web Serial no funciona desde otros orígenes inseguros) y pulsa **Arduino USB**.

## Estructura
| Archivo | Qué hace |
|---|---|
| `index.html` | Todas las pantallas (inicio, registro, búsqueda, sesión, historial, ajustes) |
| `css/style.css` | Estilos |
| `js/dsp.js` | Filtros (pasa altas, notch, envolvente), RMS, MAV, detección de activaciones, calidad de señal |
| `js/calibration.js` | Calibración reposo / pasiva / activa y cálculo de umbrales |
| `js/sources.js` | Conexión con Arduino (Web Serial y WiFi vía puente) y simulador |
| `js/storage.js` | Guardado local, CSV, respaldo, envío a Google Sheets |
| `js/charts.js` | Gráfica en tiempo real y gráfica de progreso |
| `js/app.js` | Lógica de la interfaz |
| `arduino/emg_stream/` | Firmware que transmite la señal cruda a 2 kHz por USB |
| `arduino/diagnostico_emg_enlace_wifi/` | ASTRA v3 (UNO R4 WiFi): RMS al ESP32 por el opto + EMG filtrada por WiFi |
| `arduino/diagnostico_emg_enlace/`, `arduino/astra_atril/` | ASTRA v2 y ESP32 originales (sin cambios, referencia) |
| `puente/puente_emg.py` | Puente local laptop ↔ página para el WiFi (y `simulador_arduino.py` para probar sin placa) |
| `tests/arduino_host/run.sh` | Pruebas en la PC del firmware (equivalencia con v2, búfer WiFi, API) |
| `apps-script/Code.gs` | (Opcional) receptor para Google Sheets |
| `DIGITALIZARR.ino` | Código base original (notch + RMS en el Arduino) |

Guía completa: [`docs/GUIA.md`](docs/GUIA.md).

> ⚠️ **Privacidad:** los datos de pacientes se guardan solo en el navegador. Nunca subas CSV ni respaldos a GitHub (el `.gitignore` ya los excluye).
