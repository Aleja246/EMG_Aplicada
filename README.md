# ASTRA · móvil terapéutico controlado por sEMG

La regla es **un solo cerebro**:

| Pieza | Qué hace | Qué NO hace |
|---|---|---|
| **Arduino UNO R4** (`arduino/emg_rms`) | Mide. Cada 50 ms manda `R,0.052` (RMS de 200 ms en mV, sin 60 Hz ni armónicos) | No sabe nada de calibración ni de actuadores |
| **ESP32** (`esp32/astra`) | Decide todo: calibración, detección, luz, sonido, estrellas, planetas, métricas, página y CSV | — |
| **Página** (la sirve el ESP32) | Muestra los datos y manda botones | No calcula nada. Si se cierra, la terapia sigue |

Así, cuando algo falla, se sabe en qué pieza buscar: el monitor serie del ESP32 dice si llegan datos del Arduino, y la página dice si llega el ESP32.

```
electrodos → sensor → A1 ─ Arduino ─ D1 (TX) ─ opto PC817 ─ IO35 ─ ESP32 ─┬─ luz, DFPlayer, estrellas, planetas
                           (mide)        9600 baud, 20 líneas/s    (decide) └─ WiFi "ASTRA" → página http://192.168.4.1
```

## Puesta en marcha

1. **Arduino UNO R4**: abre `arduino/emg_rms/emg_rms.ino` y súbelo. No usa bibliotecas (ten actualizado *Arduino UNO R4 Boards* en el Gestor de placas). En el Serial Plotter (115200) verás `RMS`.
2. **ESP32**: en el Arduino IDE instala (Herramientas → Gestionar bibliotecas):
   - **AccelStepper** (Mike McCauley)
   - **ESP Async WebServer** (de *ESP32Async*), que trae **Async TCP** (de *ESP32Async*). Si tenías la versión vieja de *me-no-dev*, desinstálala.

   Abre `esp32/astra/astra.ino` (se abren solas las pestañas `nucleo.h`, `luz.h`, `sonido.h`, `motores.h`, `web.h` y `pagina.h`). Placa **ESP32 Dev Module** → Subir. Si sale *Sketch too big*: Herramientas → Partition Scheme → *Huge APP*.
3. **microSD del DFPlayer** (FAT32). Copia los archivos en este orden:
   ```
   MP3/0001.mp3      canción de fondo (si agregas 0002, 0003…, sube N_CANCIONES en sonido.h)
   ADVERT/0001.mp3   "ding" (máximo 1.5 s)
   ADVERT/0002.mp3   felicitación
   ADVERT/0003.mp3   invitación: "¡Arriba!" o su nombre
   ADVERT/0004.mp3   grabación de la familia
   ```
   Pon la duración real de cada uno en `DURACION_ANUNCIO_MS` (`sonido.h`): así nunca se lanza un anuncio encima de otro.
4. **Página**: conecta la tableta o el teléfono al WiFi **ASTRA** (clave `astra2030`) y abre **http://192.168.4.1**. Si el teléfono avisa "sin internet", elige *mantener conexión*. Si no carga, apaga los datos móviles.
5. En la página: **Reposo · 15 s** → **▶ Iniciar terapia** → … → **■ Terminar**: se descarga el resumen CSV.

Sin página también funciona: el **botón BOOT** del ESP32 calibra el reposo y empieza la sesión sola. El monitor serie del ESP32 (115200) acepta `c` reposo, `p` pasiva, `f` referencia, `i` iniciar, `t` terminar, `+`/`-` sensibilidad, `r` reanudar tras el paro, `?` resumen.

## Los bloques, en orden

| # | Bloque | Cómo se hace con este código | Listo cuando |
|---|---|---|---|
| 1 | Detección sin actuadores | En `astra.ino` pon `USAR_LUZ`, `USAR_SONIDO`, `USAR_ESTRELLAS` y `USAR_PLANETAS` en `false`. Monitor serie: `c` (15 s quieto), `i`, y haz los levantamientos. Cada uno imprime `#N LEVANTAMIENTO` | 10 levantamientos dan 10 conteos y 1 min en reposo da 0 falsos (si hay falsos, sube k con `+`) |
| 2 | Actuadores de uno en uno | Pon en `true` uno a la vez: luz → sonido → estrellas → planetas | Cada levantamiento cambia la luz y suena en menos de 0.5 s |
| 3 | Página | Ya está servida por el ESP32. Para trabajarla sin hardware: abre `esp32/astra/pagina.html?sim` en el navegador y luego corre `python herramientas/html_a_h.py` | Corre 5 min sin trabarse y el contador coincide con el del monitor serie |
| 4 | Métricas y CSV | Las calcula el ESP32; **Terminar** descarga `astra_<paciente>_<fecha>.csv` | Al terminar, la página descarga el resumen |
| 5 | Pruebas, video y seguridad | [`docs/PRUEBAS.md`](docs/PRUEBAS.md) | Tabla con al menos 3 integrantes |

Para repartirlo: una persona hace el 1 y el 2; otra trabaja la página con `?sim`; otra prepara el protocolo, la tabla y el video. El modo simulado solo existe al abrir el archivo con `?sim`: **la página que sirve el ESP32 nunca usa datos simulados** (la rúbrica pide que la señal real dispare la salida).

## Cómo decide el ESP32

**Calibración sin pedirle al niño que suba la cabeza** (Hodges y Bui):
- **Reposo** (15 s, obligatoria): cabeza apoyada en la mano de la terapeuta, dentro del ATLAS (si se puede, caminando). Umbral alto = media + k·SD; umbral bajo = media + k/2·SD; k = 3.
- **Pasiva** (10 s, opcional): la terapeuta mueve la cabeza. Si el movimiento cruza el umbral, el ESP32 lo sube por encima (5 % de margen).
- **Referencia** (45 s, opcional): el móvil hace su espectáculo y la terapeuta le enseña un juguete o lo llama. El RMS más alto es el 100 %; si no pasa nada, se usa el más alto de la sesión. Sin MVC.
- **Sensibilidad**: el control deslizable de la página cambia k en vivo (1.5 a 6).

**Conteo**: un levantamiento cuenta cuando el RMS pasa el umbral alto durante 100 ms (2 muestras); el siguiente solo después de bajar del umbral bajo. Todos se cuentan; si la luz o el sonido del anterior no han terminado, ese levantamiento se cuenta pero no dispara luz ni sonido.

| Momento | Planetas | Luz | Sonido | Estrellas |
|---|---|---|---|---|
| Inicia la terapia | Giran lento sin parar | Azul tenue fijo | Canción de fondo en bucle, volumen bajo | Van a cero (el tope se busca con el microswitch al encender) |
| Levantamiento | Siguen | Siguiente color: rojo, verde, naranja, morado, amarillo | "Ding" | Suben un escalón (8 cm = 3477 pasos) |
| Llegan arriba (5 escalones) | Siguen | Recorre los colores, uno por segundo | Felicitación o familia | Bajan a cero y empieza otra ronda |
| Invitación (8 s sin activación, ajustable) | Siguen | Azul que respira (ciclo de 4 s) | "¡Arriba!" cada 15 s | Bajan un escalón cada 10 s (se puede desactivar) |
| Paro o más de 500 ms sin señal | Se detienen | Apagada | Pausa | Se detienen y vuelven a cero al reanudar |

Luz: máximo un cambio de color por segundo con transición suave (WCAG 2.3.1) y brillo máximo de 50 %.

## Archivos

| Archivo | Qué hace |
|---|---|
| `arduino/emg_rms/emg_rms.ino` | Arduino: muestrea, filtra 60 Hz + armónicos y manda el RMS (basado en `DIGITALIZARR.ino`) |
| `esp32/astra/astra.ino` | ESP32: modos, calibración, sesión, comandos, CSV. Arriba están todos los ajustes |
| `esp32/astra/nucleo.h` | Lógica pura (lectura de líneas, umbrales, detector, métricas); se puede probar en la computadora |
| `esp32/astra/luz.h`, `sonido.h`, `motores.h` | Un actuador por pestaña (lo que ya funcionaba en v2.3) |
| `esp32/astra/web.h` | WiFi, página, WebSocket y `/resumen.csv` |
| `esp32/astra/pagina.html` | La página (fuente). **`pagina.h` se genera** con `python herramientas/html_a_h.py` |
| `docs/PROTOCOLO.md` | Los mensajes entre Arduino, ESP32 y página |
| `docs/PRUEBAS.md` | Bloque 5: protocolo de pruebas, tabla, video y seguridad |
| `anterior/` | La versión anterior (la página procesaba la señal por USB). Ya no se usa |

## Si algo falla

| Síntoma | Qué revisar |
|---|---|
| Monitor del ESP32: "Sin señal del Arduino" | ¿El Arduino tiene `emg_rms.ino` y está encendido? ¿El opto está conectado a D1 (TX) e IO35? |
| "Llegan bytes del Arduino pero ninguna línea válida" | Que ambos estén a 9600 baud; si sigue, cambia `EMG_INVERTIDO` |
| Muchos falsos | Sube k en la página, haz la calibración pasiva, o activa `PASA_ALTAS_20HZ` en el Arduino |
| No cuenta levantamientos | Baja k; repite el reposo (si dice "el reposo varió mucho", se movió) |
| Un motor vibra en vez de girar | `ORDEN_B_PLANETAS` / `ORDEN_B_ESTRELLAS` en `motores.h` |
| Las estrellas bajan al buscar el tope / "No encontré el tope" | `DIR_SUBIR = -1` / revisa el microswitch (o `USAR_TOPE = false` y enciende con las estrellas abajo) |
| No suena el ding | El anuncio solo suena mientras suena la canción: revisa `MP3/0001.mp3` y las carpetas de la microSD |
| La página no abre | ¿Conectado al WiFi ASTRA? Escribe `http://` (no https). Apaga los datos móviles |
| Los motores titubean al abrir la página | Es solo mientras se carga (~30 KB, una vez por sesión); después la página recibe mensajes de ~100 bytes. Si se nota durante la terapia, mueve `estrellasActualizar()` y `planetasActualizar()` a su propia tarea de FreeRTOS |

> ⚠️ **Privacidad:** el CSV se guarda en la tableta. Nunca subas CSV de pacientes a GitHub (el `.gitignore` ya los excluye).
