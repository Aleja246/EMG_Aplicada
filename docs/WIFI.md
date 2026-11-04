# EMG por WiFi (Arduino UNO R4 WiFi → página web)

El Arduino del lado del niño sigue mandando **solo el RMS** al ESP32 del atril por el opto, igual que antes. **Además** transmite por WiFi la **señal EMG filtrada** (no el RMS) para verla en tiempo real y registrarla desde la página.

```
 Electrodos → Nova → A0 ─┐
                         │  interrupción de 1 kHz (temporizador de hardware)
                         ├─ RMS cada 100 ms → Serial1 / D1 → opto PC817 → ESP32 del atril   (SIN CAMBIOS)
                         ├─ Bruto/Sin60/SinArm → USB (Serial Plotter)                        (SIN CAMBIOS)
                         └─ SinArm (EMG filtrada) → búfer 2 s → WiFi (TCP 5005)
                                                                   │
                         laptop: puente/puente_emg.py  ◄───────────┘   (red local, sin internet)
                                       │ ws://127.0.0.1:8787
                         página (GitHub Pages, HTTPS) → gráfica en tiempo real + registro CSV
```

## Por qué hace falta un intermediario (el «puente»)

| Restricción | Qué implica |
|---|---|
| **HTTPS / contenido mixto** | GitHub Pages sirve la página por HTTPS. Un sitio HTTPS **no puede** abrir `ws://192.168.x.x` ni `http://192.168.x.x`; el navegador lo bloquea. |
| **El Arduino no puede ser HTTPS** | WiFiS3 solo tiene cliente TLS; no puede servir HTTPS/WSS con un certificado válido en la red local. |
| **GitHub Pages es estático** | No recibe ni guarda datos. No hay servidor donde el Arduino pueda «subir» la señal. |
| **Excepción de los navegadores** | `ws://127.0.0.1` (la propia computadora) sí se permite desde una página HTTPS: se considera de confianza. |
| **CORS** | No aplica a WebSocket. El puente revisa la cabecera `Origin` y solo acepta la página de GitHub Pages (`https://aleja246.github.io`), `localhost` y el archivo local. |

Por eso la solución mínima es un programa pequeño en la laptop, `puente/puente_emg.py`. Solo usa la biblioteca estándar de Python (no hay que instalar nada) y hace dos cosas:

1. Recibe la señal del Arduino por la red local.
2. La reenvía a la página por `ws://127.0.0.1:8787`.

No guarda nada ni usa internet. Se descartó un servidor en la nube porque mandaría señales de menores a internet, dependería de la conexión y agregaría retraso.

## Garantías para el atril (lo que **no** cambia)

- **El procesamiento es el mismo.** `emg_nucleo.h` es el mismo código de `diagnostico_emg_enlace.ino` v2: mismas constantes, expresiones y orden (DC → notch 60 → notch 60..420 → RMS cada 100 muestras).
- **El opto manda exactamente lo mismo.** La prueba `tests/arduino_host/run.sh` compila la **v2 original sin tocar** y el núcleo v3 en la PC y les da la misma señal de 2 minutos (EMG, red de 60 Hz y armónicos, latido, deriva y tramos saturados). Los **3240 bytes al opto son idénticos y salen en la misma muestra**. También son idénticas las 1200 líneas USB.
- **El WiFi no atrasa el muestreo.** En v2 el muestreo estaba en `loop()`, y WiFiS3 bloquea `loop()` varios ms en cada envío y hasta 10 s al conectarse a la red. En v3 el muestreo y el envío al opto corren en una **interrupción de temporizador de hardware a 1000 Hz exactos** (48 000 ciclos de reloj). El `loop()` solo atiende el USB y el WiFi.
- **Prioridades de interrupción**, verificadas en el código del núcleo:
  - Los UART (opto, USB y módulo WiFi) tienen prioridad 12.
  - El tick de `millis()` tiene prioridad 8.
  - El muestreo va en **13**, así que todos ellos pueden interrumpirlo.
  - Esto es obligatorio: `Serial1.write()` del núcleo R4 espera a que la interrupción del UART confirme el envío. Si el muestreo tuviera más prioridad, se bloquearía. El sketch lo comprueba con `static_assert` y verifica con `NVIC_GetPriority` la prioridad aplicada.
- **Si el WiFi falla, el atril no se entera.** El canal WiFi tiene un búfer de 2 s. Si se llena, se pierden muestras **solo del WiFi**; el RMS y el opto siguen igual. Si el temporizador no pudiera iniciarse, el sketch vuelve al método de v2 (muestreo en `loop()`) y desactiva el WiFi.
- **El ESP32 (`astra_atril.ino`) no cambió.** El formato (`"640\n"`) y la frecuencia (cada 100 ms, 9600 baud) son los mismos.

## Archivos

| Archivo | Qué es |
|---|---|
| `arduino/diagnostico_emg_enlace_wifi/diagnostico_emg_enlace_wifi.ino` | Firmware v3 para el UNO R4 WiFi |
| `…/emg_nucleo.h` | Procesamiento de cada muestra (= v2) |
| `…/emg_flujo.h` | Búfer circular y formato del paquete WiFi |
| `…/emg_usb.h` | Línea `Bruto:…,Sin60:…,SinArm:…` del USB (= v2) |
| `…/arduino_secrets_ejemplo.h` | Plantilla para la red WiFi (la real **no** se sube) |
| `arduino/diagnostico_emg_enlace/` y `arduino/astra_atril/` | v2 y ESP32 **originales, sin cambios** (referencia) |
| `puente/puente_emg.py` | Puente local (laptop) |
| `puente/simulador_arduino.py` | Hace de Arduino para probar todo sin la placa |
| `tests/arduino_host/run.sh` | Pruebas en la PC (equivalencia v2/v3, búfer, API) |

## Pasos

### 1. Configurar el WiFi (una vez)
1. En `arduino/diagnostico_emg_enlace_wifi/`, copia `arduino_secrets_ejemplo.h` y guárdalo como **`arduino_secrets.h`** en la misma carpeta.
2. Escribe el nombre y la clave de la red:
   ```c
   #define SECRET_SSID "NombreDeTuRed"
   #define SECRET_PASS "ClaveDeTuRed"
   #define SECRET_PUENTE_IP ""      // vacío = el Arduino encuentra el puente solo
   ```
   - La red debe ser de **2.4 GHz**, WPA2 personal (con clave normal).
   - **No** sirven redes con portal de inicio de sesión ni WPA2-Enterprise (eduroam y similares).
   - **No** sirven redes que aíslan a los dispositivos entre sí (muchas redes de invitados).
   - Lo más confiable es un router propio o el **hotspot de un celular**. La laptop y el Arduino deben estar en **la misma red**.
3. `arduino_secrets.h` está en `.gitignore`. Antes de cada commit revisa con `git status` que no aparezca.

### 2. Cargar el Arduino
1. Arduino IDE 2 → *Boards Manager* → instala **Arduino UNO R4 Boards** (trae WiFiS3).
2. Abre `diagnostico_emg_enlace_wifi.ino`. Placa: **Arduino UNO R4 WiFi**. Carga.
3. Abre el Monitor Serie a **250000** baud. Debes ver líneas `# …`:
   - `# ASTRA v3: muestreo a 1 kHz por interrupción`
   - `# WiFi: conectado. IP del Arduino 192.168.…`
   - `# Puente: …`
   - Si aparece un aviso de firmware del módulo WiFi: *Herramientas → Firmware Updater*.
4. Escribe `i` y Enter para ver el diagnóstico: muestras, duración máxima de la interrupción, **atrasos** (debe ser 0), estado del WiFi, RSSI y muestras no enviadas.
5. El LED integrado indica el estado:
   - **fijo**: transmitiendo;
   - **parpadeo rápido**: hay red, busca el puente;
   - **parpadeo lento**: sin red;
   - **apagado**: WiFi desactivado.

Con WiFi, durante la sesión **no hace falta el cable USB** a la laptop. Eso mantiene aislada la tierra de Nova, que es la regla de seguridad del documento ASTRA.

### 3. Abrir el puente en la laptop
1. Instala Python 3.8 o más nuevo (python.org; en Windows marca «Add Python to PATH»).
2. En una terminal, dentro de la carpeta del repositorio, corre `python puente/puente_emg.py` (en Windows también sirve `py puente/puente_emg.py`).
3. La primera vez el **firewall** pregunta: permite el acceso en **redes privadas** (puertos 5005 TCP y 5006 UDP).
4. El puente imprime la IP de la laptop. Si el Arduino no lo encuentra solo, copia esa IP en `SECRET_PUENTE_IP` y vuelve a cargar el sketch.
5. Deja la ventana abierta mientras uses la página.

### 4. Ver y registrar la EMG en la página
1. Abre la página en **Chrome o Edge** de escritorio (la que tenga los cambios; ver «Antes de fusionar», abajo).
2. Pulsa **Arduino WiFi** (arriba a la derecha). Si el navegador pide permiso para «acceder a dispositivos de la red local», pulsa **Permitir**.
3. El indicador pasa a **WiFi: conectado · 1000 Hz**.
4. Para ver la señal sin paciente: en Inicio pulsa **Monitor y registro de señal (sin paciente)**. También funciona dentro de una sesión de terapia; en ambos casos se ve la EMG filtrada con amplitud positiva y negativa, la envolvente y el eje en segundos.
5. Para registrar: **● Iniciar registro**, después **■ Detener**, después **⬇ Descargar CSV**.

**Antes de fusionar la rama:** GitHub Pages sigue mostrando la versión de `main`. Para probar esta rama, abre `index.html` con doble clic, o corre `python -m http.server 8000` en la carpeta y abre `http://localhost:8000`. El puente acepta ambos.

**Sin la placa:** en otra terminal corre `python puente/simulador_arduino.py`.

### CSV del registro
```
indice_muestra,tiempo_s,emg_filtrada_mV
14200,0.000,-116.6
14201,0.001,86.8
```
- `tiempo_s = (indice − primer índice) / 1000`. Sale del contador de muestras del Arduino, no de la hora de llegada, así que es exacto aunque los paquetes lleguen en ráfagas.
- Si se pierden muestras, `indice_muestra` **salta**. No se inventan valores.
- `emg_filtrada_mV` es SinArm a la salida de Nova, con resolución de 0.1 mV. Con ganancia ≈ 1000, 1 mV aquí ≈ 1 µV en la piel.
- Límite: unos 66 min por registro; al llegar se detiene solo.

## Solución de problemas

| Indicador o alarma | Qué revisar |
|---|---|
| «WiFi: buscando el puente…» / «Sin conexión con el puente» | `puente_emg.py` no está abierto, o la dirección en Ajustes no es `ws://127.0.0.1:8787` |
| «Puente listo, esperando al Arduino» | El Arduino no llegó a la laptop. Revisa:<br>• el LED del Arduino;<br>• que estén en la misma red;<br>• el firewall;<br>• que la red no aísle dispositivos;<br>• si hace falta, `SECRET_PUENTE_IP` |
| «Se perdieron N muestras por WiFi» | Señal débil: acerca el router o el celular, y revisa el RSSI con `i` (mejor que −70 dBm) |
| El puente dice «origen no permitido» | Abriste la página desde otra dirección: `python puente/puente_emg.py --origen https://esa-direccion` |
| Atrasos > 0 en el diagnóstico `i` | Anótalo y avísanos: no debería pasar |

## Qué se probó aquí y qué requiere la placa

Probado sin hardware:
- Equivalencia v2/v3 del opto y del USB, bit a bit.
- Búfer y paquetes: interrupciones simuladas en medio de la copia y atrasos de 1.5 s y 6 s. Sin datos mezclados; las pérdidas coinciden con los huecos.
- Compatibilidad del sketch con las firmas reales de WiFiS3 y FspTimer.
- Puente con el simulador: descubrimiento UDP, resincronización con bytes basura, rechazo de orígenes ajenos.
- Página en Chromium, servida también por HTTPS: el CSV coincide muestra por muestra con lo enviado; se probaron pérdidas, reinicio del Arduino, caída y reconexión del puente, sesión completa y el modo Simulación.

**No** se pudo compilar para la placa: el compilador ARM no estaba disponible en este entorno. Compila en el IDE antes de usarlo.

Requieren la placa (hazlas en este orden):
- [ ] Compila y carga sin errores en Arduino IDE 2 (UNO R4 WiFi).
- [ ] `i` muestra **atrasos 0** e intervalo máximo ≈ 1000 µs, con la interrupción máxima muy por debajo de 1000 µs, tras varios minutos transmitiendo por WiFi.
- [ ] Con `PIN_MUESTREO = 7` y osciloscopio: pulsos cada 1.000 ms, también mientras el WiFi se conecta.
- [ ] El ESP32 recibe las mismas líneas que con v2: monitor del ESP32 sin «lineas danadas», el RMS en reposo y en contracción parecido a v2, y sin «SIN DATOS DEL ARDUINO» al conectar o desconectar el WiFi ni al cerrar el puente.
- [ ] **Ruido del WiFi en el EMG:** compara SinArm en reposo con `USAR_WIFI = true` y con `false`. Si sube con WiFi, separa la antena del R4 de los cables de los electrodos y de Nova.
- [ ] Batería del lado del niño: el WiFi consume más (decenas a cientos de mA). Mide cuánto dura.
- [ ] Pérdidas y latencia en el lugar real de la terapia (distancia al router o celular).
- [ ] El atril sigue igual: calibración, activaciones, estrellas, luz y sonido.

## Seguridad y privacidad
- La clave WiFi solo vive en `arduino_secrets.h`, que no se sube.
- El puente solo atiende a la página en `127.0.0.1`; ninguna otra computadora puede leer la señal por ahí. Además revisa el `Origin`.
- El puerto 5005 acepta el Arduino desde la red local **sin contraseña**. Usa una red privada (router propio o hotspot).
- Ni el puente ni la página mandan datos a internet. El CSV se guarda en la laptop: no lo subas a GitHub (`.gitignore` excluye `*.csv`).
