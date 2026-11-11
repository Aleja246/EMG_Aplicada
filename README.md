# EMG_Aplicada
Monitor sEMG cervical para Atlas2030 / ASTRA: interfaz web para fisioterapeutas.
Pacientes identificados solo con un **ID** (sin nombre), terapia en vivo con la señal EMG, el tiempo con la cabeza
caída y los episodios, y un **expediente por paciente** con resumen, progreso y descarga en **Excel**.

## ✨ Versión sin laptop (esta rama: `claude/astra-sin-laptop`)
El **atril (ESP32) crea la red WiFi «ASTRA» y sirve la página** en **http://192.168.4.1**. El Arduino se une a esa
red y el celular (Android o iPhone) también: no hace falta laptop, Node.js ni internet. Los pacientes y las sesiones
se guardan en el navegador del celular (IndexedDB) y se exportan a Excel o a un respaldo.

- **Guía completa** (instalación en Windows, celular, datos, respaldos, diagnóstico, seguridad y pruebas):
  [`docs/ASTRA_SIN_LAPTOP.md`](docs/ASTRA_SIN_LAPTOP.md)
- Atril: `firmware/esp32_atril_ap/` (sketch + `pagina_web.h` con la página dentro del firmware + `credenciales_ejemplo.h`;
  la carpeta `data/` es solo para LittleFS, opcional). **Carga siempre la carpeta completa.**
- Sensor: `firmware/arduino_emg_ap/` (tu v5 con solo 10 renglones de red distintos + `credenciales_ejemplo.h`)
- La versión con laptop (`servidor/servidor.js`) sigue en la rama `claude/gracious-newton-89dse9`.

## Cómo probarlo (sin hardware)
1. Abre `index.html` en **Google Chrome o Microsoft Edge** (doble clic).
2. **Ajustes → Modo demostración (sin equipo)** (los registros se guardan en ese navegador): simula el sensor y el atril con la misma lógica del firmware.
3. Registra un paciente de prueba → **Iniciar terapia** → **Comenzar**. El atril simulado se calibra (10 s)
   y la página empieza a registrar la sesión sola.

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
| `index.html` | Todas las pantallas (inicio, registro, búsqueda por ID, expediente, terapia en vivo, fin de sesión, ajustes) |
| `css/style.css` | Estilos |
| `js/sources.js` | Lectura de los paquetes de la señal EMG, USB (Web Serial) y simulador |
| `js/atril.js` | Conexión en vivo con el atril (WebSocket `/ws`: señal, estado, órdenes, música, volumen, retraso) y atril simulado |
| `js/dsp.js` | Filtros para ver la señal (pasa altas, 60 Hz y armónicos) y calidad de señal |
| `js/storage.js` | Pacientes, sesiones y señales en el navegador (IndexedDB), respaldo e importación sin duplicados |
| `js/charts.js` | Gráfica de la señal EMG, gráfica de la señal guardada y gráfica de progreso |
| `js/excel.js` | Genera los archivos `.xlsx` (sesión y expediente completo) sin librerías |
| `fonts/orbitron.woff2` | Tipografía Orbitron (SIL Open Font License) del título ASTRA |
| `js/app.js` | Lógica de la interfaz |
| `firmware/arduino_emg/` | Arduino R4 WiFi v5: RMS al ESP32 por el opto (igual que v2) + señal cruda al servidor por WiFi |
| `firmware/esp32_calibracion/` | Atril ESP32 v7.1: tu código con sonido sin cambios + conexión con el servidor, órdenes de la página, elección de canciones y volumen |
| `firmware/esp32_atril_ap/` | **Atril v8.3 sin laptop**: red ASTRA, página dentro del firmware (`pagina_web.h`; `data/` para LittleFS es opcional), WebSocket, señal del Arduino por WiFi |
| `firmware/arduino_emg_ap/` | **Arduino sin laptop**: se une a la red del atril (192.168.4.1) |
| `herramientas/preparar_littlefs.py` | Si cambias la página: genera `pagina_web.h` y `data/` (comprime y revisa que no use internet). La versión está en `js/version.js` |
| `js/version.js` | Versión de la página (la página avisa si el atril sirve otra versión) |
| `herramientas/firmware/` | Tu firmware original del atril y los scripts que generan el v7.1 y el v8 a partir de él |
| `apps-script/Code.gs` | (Opcional) receptor para Google Sheets |
| `arduino/emg_stream/`, `DIGITALIZARR.ino` | Versiones anteriores (2 kHz por USB; código base original) |

> ⚠️ **Privacidad:** no se guarda el nombre del paciente; se identifica con un ID (AST-0001…). Los datos quedan en la laptop (`servidor/datos/`) o en el navegador. Nunca subas CSV ni respaldos a GitHub (el `.gitignore` ya los excluye).
