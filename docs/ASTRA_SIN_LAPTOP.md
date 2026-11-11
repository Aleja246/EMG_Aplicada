# ASTRA sin laptop: el atril crea la red y sirve la página

Con esta versión **no se necesita laptop, Node.js, internet ni GitHub Pages** durante la terapia:

1. Enciendes el atril (ESP32) y el sensor (Arduino UNO R4 WiFi).
2. El atril crea la red WiFi **ASTRA** y el Arduino se une a ella solo.
3. Unes tu celular (Android o iPhone) o una computadora a la red **ASTRA**.
4. En el navegador abres **http://192.168.4.1** y aparece la página de ASTRA.

> La versión anterior (con laptop y `servidor/servidor.js`) sigue intacta en la rama
> `claude/gracious-newton-89dse9`. Esta versión está en la rama `claude/astra-sin-laptop`.

---

## 1. Cómo funciona

```
 Electrodos → Nova → Arduino UNO R4 WiFi ──opto (RMS cada 100 ms, IGUAL que siempre)──→ ESP32 (atril)
                         │                                                              │ estrellas, lazy susan,
                         │ WiFi "ASTRA": señal EMG cruda, 1000 muestras/s               │ luz, música, PARO
                         │ (paquetes de 20 muestras con número de secuencia)            │ (núcleo 1: loop)
                         └──────────────────────► TCP 9001 ─────────────────────────────┤
                                                                                        │ red ASTRA + página
                                                       http://192.168.4.1  (LittleFS)   │ (núcleo 0)
                                     celular ◄──────── WebSocket /ws ───────────────────┘
                                     (los pacientes y las sesiones se guardan en el celular: IndexedDB)
```

- **Los motores dependen solo del RMS que llega por el opto**, como siempre. La señal que ve el celular no
  controla nada.
- La red y la página corren en el **núcleo 0** del ESP32. Motores, luz, música, detección y PARO siguen en el
  **núcleo 1** (el `loop` de siempre). Si el celular se desconecta, se congela o se va, el atril sigue igual.
- Si el Arduino pierde el WiFi, sigue midiendo y mandando el RMS por el opto. Si el atril no puede crear la red,
  funciona igual que sin página.
- **Nada sale a internet.** La página no usa CDN ni fuentes externas: todo viene del ESP32.

### Qué cambió en tus códigos (y qué no)

| | Qué cambió | Qué NO cambió |
|---|---|---|
| **Arduino** `firmware/arduino_emg_ap/arduino_emg_ap.ino` | Es tu `arduino_emg.ino` v5 **línea por línea**, salvo 10 renglones de red: se une a la red ASTRA (clave en `credenciales.h`), el servidor está fijo en `192.168.4.1`, `WiFi.setTimeout(0)` (unirse a la red ya no detiene el `loop`; antes podía detenerlo hasta 10 s y retrasar el opto) y vuelve a pedir la red cada 15 s en vez de cada 60 s. | Muestreo de 1000 Hz en la interrupción, A0, filtros (60 Hz y armónicos), RMS cada 100 ms, opto (Serial1, 9600, `"640\n"`), Serial Plotter, formato de los paquetes. |
| **ESP32** `firmware/esp32_atril_ap/esp32_atril_ap.ino` (v8) | Se generó **desde tu firmware original** (v5 con sonido, guardado en `herramientas/firmware/`) con las mismas adaptaciones ya probadas del v7.1, y una parte de red nueva: crea la red ASTRA, sirve la página desde LittleFS, recibe la señal del Arduino por WiFi y habla con la página por WebSocket. | Pines, orden de bobinas, velocidades, aceleraciones, límites, microswitch, calibración basal, umbrales, histéresis, confirmaciones de tiempo, detección cabeza arriba/abajo, luz, DFPlayer, listas, avisos, PARO, BOOT y todos los comandos del monitor serie. |
| **Página** | La sirve el ESP32. Los registros se guardan en el navegador (IndexedDB). El registro pide solo sexo, edad en años, GMFCS y diagnóstico opcional. Se añadió un diagnóstico de conexión en Ajustes. | Diseño, pantallas, terapia en vivo, expediente, progreso, Excel, música, volumen y modo demostración. |

---

## 2. Lo que necesitas (Windows)

| Programa / biblioteca | Versión | Para qué |
|---|---|---|
| **Arduino IDE** | 2.3.x | Cargar los programas |
| Paquete de placas **esp32** (Espressif Systems) | **3.0.0 o más reciente** (la última al escribir esto es 3.3.12) | ESP32 Dev Module. Ya trae WiFi, LittleFS, Preferences y el servidor web con WebSocket (`esp_http_server`): **no hace falta ninguna otra biblioteca de servidor**. Con la versión 2.x **no** compila. |
| Paquete de placas **Arduino UNO R4 Boards** | 1.0.5 o más reciente | Arduino UNO R4 WiFi (incluye WiFiS3) |
| Biblioteca **AccelStepper** (Mike McCauley) | 1.64 | Motores del atril (la misma de siempre) |
| Complemento **arduino-littlefs-upload** (Earle Philhower) | 1.6.3 o más reciente | Subir la página (carpeta `data/`) al ESP32 |

> No instales ESPAsyncWebServer, AsyncTCP ni WebSockets: no se usan. Así se evitan choques entre bibliotecas.

---

## 3. Instalación paso a paso (una sola vez)

### 3.1 Arduino IDE y paquetes
1. Instala el **Arduino IDE 2** (arduino.cc → Software).
2. *Archivo → Preferencias → URLs adicionales del gestor de placas*: agrega
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
3. *Herramientas → Placa → Gestor de placas*:
   - busca **esp32** (de Espressif Systems) e instala la versión **3.x** más reciente;
   - busca **Arduino UNO R4 Boards** e instálalo.
4. *Herramientas → Administrar bibliotecas*: busca **AccelStepper** (Mike McCauley) e instala la 1.64.

### 3.2 Complemento para subir LittleFS
1. Descarga el archivo **`arduino-littlefs-upload-1.6.3.vsix`** (o la versión más reciente) desde
   https://github.com/earlephilhower/arduino-littlefs-upload/releases
2. Cópialo a `C:\Users\<tu usuario>\.arduinoIDE\plugins\`. Si la carpeta `plugins` no existe, créala.
   **No** descomprimas el `.vsix`.
3. Cierra y vuelve a abrir el Arduino IDE.

### 3.3 Descargar el proyecto
En GitHub, cambia a la rama **`claude/astra-sin-laptop`** → **Code → Download ZIP** y descomprímelo
(no trabajes dentro del ZIP).

### 3.4 La clave de la red ASTRA
La clave **no** se sube a GitHub. En **cada una** de estas dos carpetas:

- `firmware/esp32_atril_ap/`
- `firmware/arduino_emg_ap/`

1. Copia `credenciales_ejemplo.h` y nombra la copia **`credenciales.h`**. En Windows, activa
   *Ver → Extensiones de nombre de archivo* para que no quede como `credenciales.h.txt`.
2. Ábrela con el Bloc de notas y escribe tu clave:
   ```c
   #define ASTRA_RED   "ASTRA"
   #define ASTRA_CLAVE "tu-clave-de-8-a-63-caracteres"
   ```
3. **La misma clave en las dos carpetas.** Debe tener de 8 a 63 caracteres; si no, el atril no crea la red
   y lo avisa en el monitor serie.

Si no creas `credenciales.h`, se usa la clave de ejemplo `CAMBIA-ESTA-CLAVE`. Funciona, pero es pública:
cámbiala.

### 3.5 Cargar el ESP32 (atril)
1. Conecta el ESP32 por USB y abre `firmware/esp32_atril_ap/esp32_atril_ap.ino`.
2. *Herramientas*:
   - **Placa:** `ESP32 Dev Module`
   - **Partition Scheme:** `No OTA (2MB APP/2MB SPIFFS)` (también sirve `Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS)`)
   - **Puerto:** el del ESP32
3. Pulsa **Subir**. Si se queda en `Connecting...`, mantén presionado **BOOT** hasta que empiece.

### 3.6 Subir la página al ESP32 (LittleFS)
La página ya viene preparada en `firmware/esp32_atril_ap/data/` (10 archivos comprimidos, 53 KB).

1. **Cierra el Monitor Serie** (si está abierto, la subida falla).
2. Con el mismo sketch abierto y la **misma** *Partition Scheme* del paso anterior, presiona
   **Ctrl + Shift + P** y elige **`Upload LittleFS to Pico/ESP8266/ESP32`**.
3. Espera a que diga que terminó.

Hay que repetir este paso **solo** si cambias la página, si cambias la *Partition Scheme* o si borras la flash
del ESP32. Subir el sketch **no** borra la página.

### 3.7 Comprobar el atril
Abre el **Monitor Serie a 115200 baudios** («Sin ajuste de línea») y presiona **EN/RST**. Debe decir:
```
=== ASTRA - atril v8 (sonido + red ASTRA + pagina en 192.168.4.1) ===
>> Pagina: 10 archivos (53 KB en RAM). RAM libre: ... KB
>> Red WiFi "ASTRA" lista. En el celular: unete a la red y abre http://192.168.4.1
>> Pagina web lista.
```
Si dice `LittleFS no tiene la pagina`, repite el paso 3.6.

### 3.8 Cargar el Arduino (sensor)
1. Conecta el Arduino UNO R4 WiFi por USB y abre `firmware/arduino_emg_ap/arduino_emg_ap.ino`.
2. *Placa:* `Arduino UNO R4 WiFi`; *Puerto:* el del Arduino. Pulsa **Subir**.
3. Abre el **Monitor Serie a 250000 baudios**. Con el atril encendido verás:
   ```
   >> Conectando a la red WiFi...
   >> WiFi conectado a ASTRA. IP del Arduino: 192.168.4.x
   >> Conectado al servidor: la senal ya sale a la pagina.
   ```
   En el atril aparece `>> Arduino conectado por WiFi: la senal EMG ya sale a la pagina.`
4. Si dice `No responde el modulo WiFi del R4`: *Herramientas → Firmware Updater* y actualiza el módulo WiFi.

### 3.9 Listo
Desconecta los USB y deja cada placa en su power bank.

> ⚠️ **No conectes las dos placas por USB a la laptop con el niño conectado:** unirías las tierras que el opto
> mantiene separadas. Para cargar los programas, el niño no debe tener los electrodos puestos.

---

## 4. Usar ASTRA con el celular (cada sesión)

1. Enciende el **atril** y el **sensor**. El orden no importa: el Arduino espera la red y se conecta solo.
2. En el celular: **Ajustes → WiFi → ASTRA** → escribe la clave.
   - **Android** avisa «Esta red no tiene acceso a Internet»: elige **Mantener conexión / Sí**.
     Si después la página no abre, **apaga los datos móviles** mientras usas ASTRA: algunos Android mandan todo
     por datos cuando el WiFi no tiene internet.
   - **iPhone** muestra «Sin conexión a Internet» bajo el nombre de la red: es normal y no impide usar ASTRA.
3. Abre **Chrome** (Android) o **Safari** (iPhone) y escribe exactamente **`http://192.168.4.1`**
   (con `http://`, no `https://`).
4. Recomendado: guárdala en la pantalla de inicio.
   - Safari: botón Compartir → **Agregar a inicio**.
   - Chrome: menú ⋮ → **Agregar a la pantalla principal**.

   En iPhone esto además protege los registros (ver sección 6).
5. Arriba a la derecha debe decir **«Equipo listo»** (punto verde).
6. **Registrar paciente** o **Buscar paciente** → **Iniciar terapia** → escribe el fisioterapeuta responsable →
   **Comenzar**. El atril calibra 10 s (cabeza apoyada y quieta) y la sesión empieza sola.
7. Durante la terapia la pantalla muestra solo:
   - postura estimada y tiempo de sesión;
   - la señal EMG en tiempo real;
   - tiempo con la cabeza abajo y número de episodios;
   - el estado del equipo.

   Botones: **Pausar / Continuar**, **Volver a calibrar** y **Finalizar sesión**.
8. Al finalizar: revisa el resumen, escribe observaciones, **Descargar Excel** (opcional) y **Guardar sesión**.

**Ajustes** (arriba a la derecha):
- la canción con la cabeza arriba (pistas 004–006) y con la cabeza abajo (001–003);
- el volumen (0–30): el atril lo guarda en su memoria;
- descargar e importar respaldos;
- el **Diagnóstico de la conexión**;
- el **Modo demostración**, para practicar sin el equipo.

Una computadora con WiFi se usa igual: se une a la red ASTRA y abre `http://192.168.4.1`. Pueden estar
**hasta 3 páginas abiertas a la vez**. Si se abre una cuarta, el atril cierra una de las anteriores y esa vuelve a intentar conectarse sola.

---

## 5. Dónde se guardan los pacientes y las sesiones

- En el **navegador del dispositivo** donde los registras (IndexedDB, base `astra`): pacientes, sesiones, la
  señal de cada sesión y los resultados de la calibración.
- **No** se guardan en el ESP32. El ESP32 solo recuerda la música elegida y el volumen.
- **No** se mandan a internet ni a otro equipo.
- **Cada dispositivo tiene sus propios registros.** Lo que guardas en un celular no aparece en otro, ni en otro
  navegador del mismo celular. Para pasar los datos se usa un **respaldo** (sección 6).
- Se conservan al cerrar y volver a abrir el navegador, **pero se pierden** si:
  - borras los datos de navegación o del sitio;
  - usas una pestaña privada o de incógnito (ahí no se guarda nada al cerrarla);
  - desinstalas el navegador;
  - en **iPhone**, Safari puede borrar los datos de un sitio que no abres en **7 días de uso de Safari**. Agregar
    ASTRA a la pantalla de inicio evita esto en la práctica, pero haz respaldos de todos modos;
  - el teléfono se queda sin espacio (el navegador puede liberar datos; la página pide almacenamiento
    persistente, pero el navegador decide).
- Si el navegador no permite guardar, la página **lo avisa con un mensaje**; nunca se pierde un registro en
  silencio. Si un guardado falla, también avisa y sugiere descargar el Excel.
- El ID de cada paciente (AST-0001, AST-0002…) es único en ese dispositivo. No se guarda nombre ni fecha de
  nacimiento.
- Si tenías registros de la versión anterior guardados en el mismo navegador (localStorage), se pasan solos a
  IndexedDB la primera vez.

---

## 6. Exportar e importar

| Qué | Dónde | Qué contiene |
|---|---|---|
| **Excel de una sesión** | Al finalizar (**Descargar Excel**) o en el expediente (botón **⬇** de la sesión) | Hoja *Sesión*: paciente, fecha, fisioterapeuta, duración, umbral, tiempos con la cabeza erguida y caída, caídas, caída más larga, caídas por minuto, calibración (reposo, desviación, umbral de relajación, si fue inestable) y observaciones. Hoja *Episodios*. Hoja *Señal* (RMS cada 100 ms). |
| **Excel del expediente** | Expediente → **⬇ Excel de todas las sesiones** | Hoja *Paciente* y hoja *Sesiones* (una fila por sesión) |
| **Respaldo** (`ASTRA-respaldo-AAAA-MM-DD.json`) | Ajustes → **Descargar respaldo** | Todos los pacientes, sesiones y señales de ese navegador |
| **Importar respaldo** | Ajustes → **Importar respaldo…** | Suma los datos del archivo sin borrar nada. Lo que ya existe no se duplica. Si un paciente importado trae un ID que **otro** paciente ya usa, recibe un ID nuevo y el anterior queda guardado como `codigoOriginal`. |

- **Android:** los archivos van a *Descargas*.
- **iPhone (Safari):** aparece un aviso de descarga y el archivo queda en la app **Archivos → Descargas**.
  Para importar, elige el archivo desde **Archivos**.
- Como `http://192.168.4.1` no es una conexión segura (https), los navegadores **no** ofrecen el botón
  «Compartir archivo» desde la página. Usa la descarga normal.
- Los respaldos y los Excel **contienen datos clínicos sin cifrar**: guárdalos en un lugar protegido y no los
  subas a GitHub (el `.gitignore` ya excluye `*.xlsx`, `*.csv` y `ASTRA-respaldo-*.json`).
- Recomendación: descarga un respaldo al terminar cada día de terapia.

---

## 7. Si algo no funciona

Primero abre **Ajustes → Diagnóstico de la conexión**. Muestra:
- si la página está conectada al atril y el retraso de ida y vuelta;
- si el Arduino está conectado por WiFi;
- las muestras por segundo que llegan al atril y a la página (deben ser ~1000);
- los paquetes perdidos de cada tramo;
- el rango de la señal en A0;
- el estado, el RMS y los umbrales del atril;
- la memoria libre y la versión.

También puedes abrir `http://192.168.4.1/estado.json`.

| Lo que ves | Qué revisar |
|---|---|
| No aparece la red **ASTRA** | ¿El atril está encendido? En el monitor serie del ESP32 (115200) debe decir `Red WiFi "ASTRA" lista`. Si dice `No se pudo crear la red WiFi`, la clave de `credenciales.h` debe tener de 8 a 63 caracteres. |
| El celular se une pero la página no abre | Escribe `http://192.168.4.1` completo (sin `https`). Android: elige «Mantener conexión» y apaga los datos móviles. Cierra la VPN si tienes una. |
| La página dice «LittleFS no tiene la página» / «todavía no tiene la página» | Repite el paso 3.6 (Upload LittleFS) con la misma *Partition Scheme* con la que subiste el sketch. |
| Arriba dice **«Sin conexión con el atril»** | El celular se salió de la red ASTRA (a veces vuelve solo a otra red con internet). Vuelve a unirte a ASTRA. La página se reconecta sola. |
| **«Sensor sin conexión»** / no se ve la señal EMG | Monitor serie del Arduino (250000): ¿dice `WiFi conectado a ASTRA`? Si no, la clave de `credenciales.h` del Arduino no es igual a la del atril. ¿Dice `Conectado al servidor`? Si no, ¿el atril está encendido? El Arduino reintenta solo. |
| **«El atril no recibe la señal»** | Es el opto (no el WiFi): revisa el cable del opto y que el Arduino esté encendido. Este aviso es el mismo de siempre. |
| La señal se ve a saltos / «Paquetes perdidos» sube | Acerca el celular al atril. Cierra otras páginas de ASTRA abiertas. Si «Muestras/s que recibe el atril» está lejos de 1000, el problema es entre el Arduino y el atril (distancia o interferencia). |
| «La señal está en el límite» | Electrodos, electrodo de referencia o cable de Nova (igual que antes). |
| «Paro de emergencia activado» | Suelta el hongo. Luego presiona **BOOT** o escribe `r` en el monitor serie. **Desde la página no se puede liberar.** |
| La subida de LittleFS falla | Cierra el Monitor Serie. Revisa que el `.vsix` esté en `C:\Users\<usuario>\.arduinoIDE\plugins\` y reinicia el IDE. |
| Error al compilar el ESP32 con `esp_http_server` o `is_websocket` | Tienes el paquete esp32 2.x: actualízalo a 3.x. |

---

## 8. Seguridad, límites y lo que haría falta para uso clínico

**Lo que hace esta versión:**
- **El PARO manda.** Con el PARO activo, el atril ignora toda orden de la página (lo comprueba dos veces: al
  recibirla y en el `loop`). El paro solo se libera con el botón físico y BOOT, o con `r` por el monitor serie.
- **La página solo puede:** recalibrar (`c`, únicamente en SESIÓN o PAUSA), pausar/continuar (`p`), elegir pistas
  (1–255) y ajustar el volumen (0–30). Las demás teclas del monitor serie (`h`, `x`, `+`, `-`, `o`, `r`, `i`) no se
  aceptan desde la página. Cada mensaje se valida, y se ignoran más de 20 mensajes por segundo y los mensajes de
  más de 512 bytes.
- La red usa **WPA2** con tu clave, para hasta 5 equipos (el Arduino y 4 celulares o computadoras).

**Limitaciones que debes conocer:**
- **HTTP sin cifrar dentro de la red ASTRA.** WPA2 protege el aire, pero cualquiera que conozca la clave puede
  abrir la página, ver la señal y usar los controles anteriores. No hay usuario ni contraseña en la página.
  Cambia la clave de ejemplo y no la compartas.
- **IndexedDB no está cifrada.** Quien tenga acceso al celular desbloqueado puede ver los registros: usa bloqueo
  de pantalla. Los respaldos y los Excel tampoco están cifrados.
- No hay sincronización entre dispositivos ni copia automática: solo respaldos manuales.
- Los registros no guardan nombre ni fecha de nacimiento, pero edad, sexo, GMFCS, diagnóstico, fechas y
  observaciones siguen siendo datos de salud. **Para las pruebas, usa datos ficticios.**
- **Este sistema no es un dispositivo médico validado.** No se ha demostrado su seguridad clínica.

**Antes de un uso clínico real haría falta, como mínimo:**
- un análisis de riesgos del sistema completo (por ejemplo, ISO 14971);
- verificación y validación del software (IEC 62304) y de la parte eléctrica (IEC 60601-1, incluido el
  aislamiento del paciente);
- cifrado y control de acceso (usuarios, contraseñas, HTTPS u otra protección);
- cifrado de los datos en el dispositivo y de los respaldos;
- un procedimiento de respaldo y conservación de registros;
- consentimiento informado;
- cumplir la ley de protección de datos personales de salud que aplique;
- y la aprobación del comité de ética o la autoridad correspondiente.

---

## 9. Recursos del ESP32 (por qué es estable)

- **Núcleos:** la red (WiFi, servidor web, WebSocket y la señal del Arduino) corre en el **núcleo 0**: tarea
  `red` con prioridad 1 y el servidor web con prioridad 3. El `loop` con motores, luz, música, detección y PARO
  sigue solo en el **núcleo 1**. Se comunican con colas (comandos, música, volumen, avisos) y con copias
  protegidas por candado; la red nunca cambia el estado del atril directamente.
- **Flash:** la página (53 KB) se copia **a la RAM al encender**, antes de mover los motores. Durante la terapia
  no se lee la flash: leerla detiene un instante el otro núcleo. Solo se escribe la flash al cambiar la música o
  el volumen, igual que en el v7.1.
- **RAM:** 53 KB de la página + unos 8 KB del estado + las pilas de las tareas (8 KB la de red, 6 KB la del
  servidor). La memoria libre real se ve en el diagnóstico y en el monitor serie al encender. Si algún archivo no
  cupiera, se lee de la flash en pedazos y se avisa.
- **Sockets:** 8 conexiones para el servidor (+3 internas) + Arduino + aviso UDP = 14 de los 16 de lwIP.
  Como máximo hay 3 páginas en vivo.
- **Celular lento o que se va:** los envíos a las páginas nunca esperan al loop. Si un celular no recibe, sus
  mensajes se descartan (la página lo nota por el número de secuencia). Al primer envío fallido, esa conexión se
  cierra y no se le vuelve a intentar: así un celular congelado no traba la página de los demás.
- **Flash del programa:** la imagen debería caber en los 2 MB de `No OTA` con amplio margen, y probablemente
  también en los 1,25 MB de `Default`. El tamaño exacto lo muestra el Arduino IDE al compilar (aquí no se pudo
  compilar para la placa real).

---

## 10. Qué se verificó y qué falta probar con el hardware

**Verificado en la computadora** (no hubo forma de compilar para las placas reales: las descargas de los
compiladores del ESP32 y del UNO R4 están bloqueadas en este entorno):

| Prueba | Resultado |
|---|---|
| El firmware v8 del atril compilado con g++ contra los encabezados **reales** de `esp_http_server.h` de ESP-IDF 5.1, 5.4 y 5.5 (paquete esp32 3.0 a 3.3), con `-Wall -Wextra` | Sin errores ni advertencias del firmware |
| Mismo escenario de calibración, sesión, música, pausa, umbral, inversión y recalibración: **tu firmware original contra el v8** | Las 60 líneas del monitor serie y todos los comandos al DFPlayer son **idénticos**, por el monitor serie y por la cola de la página |
| Seguridad del atril (10 pruebas) | El PARO no se puede liberar desde la página. Se rechazan 19 mensajes no permitidos o fuera de rango. Se aceptan como máximo 20 mensajes por segundo. `p` pausa y continúa |
| El Arduino `arduino_emg_ap.ino` corriendo en la PC: interrupción de 1 kHz, `loop` real, WiFi simulado con la red del atril que aparece tarde | El opto manda 10 líneas por segundo desde el arranque, con un hueco máximo de 100 ms. El v5 original detenía el opto 10 s en el mismo caso |
| Cadena completa en la PC: Arduino (código real) → TCP → atril v8 (código real) → WebSocket → página servida desde `data/` en Chromium, con pantalla de celular (Pixel 7 y iPhone 13) | 1000 muestras/s, 0 paquetes perdidos, retraso de ~1 ms en local. Registro, música, volumen, calibración, sesión, conteo de episodios, pausa/continuar, finalizar, Excel (verificado con openpyxl), ver la señal, persistencia al recargar y **ningún pedido a otro sitio** |
| Respaldos | Importar en otro navegador, reimportar sin duplicar, cambio de ID cuando choca, señal incluida, aviso si el navegador no permite guardar |
| Robustez | Celular congelado (la página de los demás sigue a 1000 muestras/s). 6 páginas abiertas (el atril mantiene 3). El Arduino se apaga y vuelve (se reconecta solo). El atril se reinicia (el Arduino se reconecta solo) |
| La terapia en un celular | Todo cabe en la pantalla, sin desplazarse ni desbordar a lo ancho |

**Pendiente con el hardware real (hazlo en este orden y sin niño conectado al principio):**
1. Compilar las dos placas en el Arduino IDE y anotar el tamaño del programa y la «RAM libre» que imprime el atril.
2. Que la red ASTRA aparezca y que la página abra en tu Android y en tu iPhone (con y sin datos móviles).
3. Que el Arduino se una a la red, y que el Diagnóstico muestre unas 1000 muestras/s y casi 0 paquetes perdidos a
   la distancia real de uso.
4. **Motores con la página abierta:** que las estrellas y el lazy susan giren igual de suaves que sin página,
   también mientras la página carga y con 2–3 celulares conectados.
5. PARO (con `USAR_PARO = true` y el divisor conectado): con la página abierta, presionarlo detiene todo y ningún
   botón de la página lo libera.
6. Quitar el WiFi del Arduino (desconectar su antena o alejarlo): el atril debe seguir funcionando con el opto.
7. Apagar y encender el atril a mitad de sesión: el Arduino y la página se reconectan solos.
8. Una sesión completa: guardar, cerrar el navegador, volver a abrir y ver el registro. Descargar el Excel y un
   respaldo en cada celular.
9. En iPhone: que los archivos lleguen a **Archivos → Descargas** y que se puedan importar desde ahí.
