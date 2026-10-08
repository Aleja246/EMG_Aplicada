# EMG_Aplicada
Monitor sEMG cervical para Atlas2030: interfaz web para fisioterapeutas (registro/búsqueda de pacientes,
calibración en 3 fases, señal y envolvente en tiempo real, métricas por activación e historial de progreso).

## Cómo probarlo (sin Arduino)
1. Abre `index.html` en **Google Chrome o Microsoft Edge** (doble clic).
2. Pulsa **Simulación** (arriba a la derecha), registra un paciente de prueba y haz la terapia.

## Con el Arduino
1. Sube `arduino/emg_stream/emg_stream.ino` a la placa (cierra el Monitor Serie después).
2. Abre la página (en GitHub Pages o en `localhost`; Web Serial no funciona desde otros orígenes inseguros) y pulsa **Conectar Arduino**.

## Estructura
| Archivo | Qué hace |
|---|---|
| `index.html` | Todas las pantallas (inicio, registro, búsqueda, sesión, historial, ajustes) |
| `css/style.css` | Estilos |
| `js/dsp.js` | Filtros (pasa altas, notch, envolvente), RMS, MAV, detección de activaciones, calidad de señal |
| `js/calibration.js` | Calibración reposo / pasiva / activa y cálculo de umbrales |
| `js/sources.js` | Conexión con Arduino (Web Serial) y simulador |
| `js/storage.js` | Guardado local, CSV, respaldo, envío a Google Sheets |
| `js/charts.js` | Gráfica en tiempo real y gráfica de progreso |
| `js/app.js` | Lógica de la interfaz |
| `arduino/emg_stream/` | Firmware que transmite la señal cruda a 2 kHz |
| `apps-script/Code.gs` | (Opcional) receptor para Google Sheets |
| `DIGITALIZARR.ino` | Código base original (notch + RMS en el Arduino) |

Guía completa: [`docs/GUIA.md`](docs/GUIA.md).

> ⚠️ **Privacidad:** los datos de pacientes se guardan solo en el navegador. Nunca subas CSV ni respaldos a GitHub (el `.gitignore` ya los excluye).
