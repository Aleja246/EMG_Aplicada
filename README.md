# EMG_Aplicada
Monitor sEMG cervical para Atlas2030 / ASTRA: interfaz web para fisioterapeutas (registro/búsqueda de pacientes,
señal EMG del Arduino en tiempo real, **calibración paso a paso en el atril ESP32**, activaciones e historial de progreso).

## Cómo probarlo (sin hardware)
1. Abre `index.html` en **Google Chrome o Microsoft Edge** (doble clic).
2. Pulsa **Simulación** (arriba a la derecha). Simula el Arduino y el atril con la misma lógica del firmware.
3. Registra un paciente de prueba → **Iniciar terapia** → **Comenzar calibración** → **Comenzar reposo** →
   **Comenzar calibración pasiva** → **Comenzar calibración activa** → **Iniciar sesión**.

## Con ASTRA (Arduino UNO R4 WiFi + atril ESP32)
1. Sube `firmware/arduino_emg/arduino_emg.ino` al Arduino y `firmware/esp32_calibracion/esp32_calibracion.ino` al ESP32.
2. Enciende ambos (power banks) y conecta la laptop a la red WiFi **ASTRA** (clave `astra2026`).
3. Abre `index.html` desde el archivo y pulsa **Conectar ASTRA**.

Guía completa de la conexión y de la calibración paso a paso: [`docs/CONEXION_ASTRA.md`](docs/CONEXION_ASTRA.md).

## Estructura
| Archivo | Qué hace |
|---|---|
| `index.html` | Todas las pantallas (inicio, registro, búsqueda, sesión, historial, ajustes) |
| `css/style.css` | Estilos |
| `js/sources.js` | Señal EMG cruda del Arduino: WiFi (`/stream`), USB (Web Serial) y simulador |
| `js/atril.js` | Conexión con el atril ESP32 (`/estado`, `/cmd`) y atril simulado |
| `js/dsp.js` | Filtros para ver la señal (pasa altas, 60 Hz y armónicos) y calidad de señal |
| `js/storage.js` | Guardado local, CSV, respaldo, envío a Google Sheets |
| `js/charts.js` | Gráfica de la señal EMG, gráfica del RMS del atril y gráfica de progreso |
| `js/app.js` | Lógica de la interfaz |
| `firmware/arduino_emg/` | Arduino R4 WiFi v4: RMS al ESP32 por el opto + red WiFi ASTRA con la señal cruda |
| `firmware/esp32_calibracion/` | Atril ESP32 v3.0: calibración paso a paso, detección, actuadores y servidor web |
| `apps-script/Code.gs` | (Opcional) receptor para Google Sheets |
| `arduino/emg_stream/`, `DIGITALIZARR.ino` | Versiones anteriores (2 kHz por USB; código base original) |

> ⚠️ **Privacidad:** los datos de pacientes se guardan solo en el navegador. Nunca subas CSV ni respaldos a GitHub (el `.gitignore` ya los excluye).
