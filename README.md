# EMG_Aplicada
Monitor sEMG cervical para Atlas2030 / ASTRA: interfaz web para fisioterapeutas (registro/búsqueda de pacientes,
señal EMG del Arduino en tiempo real, **calibración paso a paso en el atril ESP32**, activaciones e historial de progreso).

## Cómo probarlo (sin hardware)
1. Abre `index.html` en **Google Chrome o Microsoft Edge** (doble clic).
2. Pulsa **Simulación** (arriba a la derecha): simula el Arduino y el atril con la misma lógica del firmware v6.
3. Registra un paciente de prueba → **Iniciar terapia**. El atril simulado hace solo la calibración basal (10 s)
   y la página empieza a registrar la sesión.

## Con ASTRA (Arduino UNO R4 WiFi + atril ESP32 + servidor en la laptop)
1. Escribe el nombre y la clave de la red WiFi (la del consultorio o la zona con cobertura de la laptop) al inicio de
   `firmware/arduino_emg/arduino_emg.ino` y `firmware/esp32_calibracion/esp32_calibracion.ino`, y súbelos a cada placa.
2. En la laptop (con Node.js instalado), doble clic en `servidor/iniciar-servidor.bat`: se abre http://localhost:8080.
3. Enciende las placas: se conectan solas al servidor y la página muestra la señal y el atril en vivo.

Cómo funciona todo y el paso a paso: [`docs/CONEXION_ASTRA.md`](docs/CONEXION_ASTRA.md).

## Estructura
| Archivo | Qué hace |
|---|---|
| `servidor/servidor.js` | Servidor en la laptop (Node.js, sin paquetes): recibe a las placas, WebSocket `/ws` en vivo y API `/api/...` de pacientes y sesiones (guardados en `servidor/datos/`) |
| `index.html` | Todas las pantallas (inicio, registro, búsqueda, sesión, historial, ajustes) |
| `css/style.css` | Estilos |
| `js/sources.js` | Lectura de los paquetes de la señal EMG, USB (Web Serial) y simulador |
| `js/atril.js` | Conexión con el servidor (WebSocket: señal, atril, órdenes y música) y atril simulado |
| `js/dsp.js` | Filtros para ver la señal (pasa altas, 60 Hz y armónicos) y calidad de señal |
| `js/storage.js` | Pacientes y sesiones en el servidor (o en el navegador si no hay servidor), CSV, respaldo, Google Sheets |
| `js/charts.js` | Gráfica de la señal EMG, gráfica del RMS del atril y gráfica de progreso |
| `js/app.js` | Lógica de la interfaz |
| `firmware/arduino_emg/` | Arduino R4 WiFi v5: RMS al ESP32 por el opto (igual que v2) + señal cruda al servidor por WiFi |
| `firmware/esp32_calibracion/` | Atril ESP32 v7: tu código con sonido sin cambios + conexión con el servidor, órdenes de la página y elección de canciones |
| `apps-script/Code.gs` | (Opcional) receptor para Google Sheets |
| `arduino/emg_stream/`, `DIGITALIZARR.ino` | Versiones anteriores (2 kHz por USB; código base original) |

> ⚠️ **Privacidad:** los datos de pacientes se guardan solo en el navegador. Nunca subas CSV ni respaldos a GitHub (el `.gitignore` ya los excluye).
