# Conexión de la página con ASTRA (Arduino + atril ESP32)

La página ahora trabaja con los **dos** aparatos del sistema ASTRA al mismo tiempo:

| Aparato | Qué le da a la página | Cómo |
|---|---|---|
| **Arduino UNO R4 WiFi** (`firmware/arduino_emg`) | La **señal EMG cruda** (1000 muestras/s, no el RMS) para verla en tiempo real | WiFi: crea la red **ASTRA** y manda la señal en `http://192.168.4.1/stream` |
| **Atril ESP32** (`firmware/esp32_calibracion`) | Su estado en vivo: fase de calibración, RMS de cada bloque, umbrales, activaciones y mensajes. Recibe los pasos de la calibración. | WiFi: se une a la red ASTRA con la IP fija `192.168.4.200` (`/estado` y `/cmd`) |

```
Electrodos → Nova → Arduino R4 ──opto (RMS cada 100 ms, sin cambios)──→ ESP32 → motores, luz, sonido
                       │  WiFi "ASTRA"                                      │ WiFi
                       └──── señal cruda ──→  LAPTOP (página)  ←── estado / comandos ──┘
```

- **La calibración y la detección siguen en el ESP32**, igual que antes. La página no decide nada:
  manda los pasos y muestra lo que mide el atril.
- **El opto sigue siendo el único enlace entre el Arduino y el ESP32.** Si la página o el WiFi fallan,
  el atril sigue funcionando (y se puede calibrar con el botón BOOT).
- **Seguridad eléctrica:** como todo es inalámbrico, la laptop no toca la tierra del niño ni la del atril.
  Ambos aparatos siguen con su propio power bank.

## 1. Cargar los programas (una vez)

1. **Arduino** (placa «Arduino UNO R4 WiFi»): abre `firmware/arduino_emg/arduino_emg.ino` y súbelo.
   Si el monitor serie dice «No responde el módulo WiFi», actualiza el firmware del módulo:
   IDE 2 → *Herramientas → Firmware Updater*.
2. **ESP32** (placa «ESP32 Dev Module»): abre `firmware/esp32_calibracion/esp32_calibracion.ino` y súbelo.
   Necesita la biblioteca AccelStepper (como antes); WiFi y WebServer vienen con las placas ESP32.
3. El nombre y la clave de la red están al inicio de **los dos** programas y deben ser iguales:
   `RED_SSID = "ASTRA"`, `RED_CLAVE = "astra2026"`. Cámbialos si quieres, en ambos.

## 2. Cada sesión

1. Enciende el Arduino y el atril (power banks). En unos segundos aparece la red WiFi **ASTRA**.
2. Conecta la laptop a la red **ASTRA** (clave `astra2026`). No tiene internet: es normal.
   En Windows, si ofrece cambiarse a otra red «con internet», di que no.
3. Abre la página **desde el archivo**: doble clic en `index.html` (Chrome o Edge).
   - Desde GitHub Pages (`https://…github.io`) el navegador bloquea las direcciones `http://192.168…`
     de la red local, o pide permiso de «dispositivos de la red local». Si sale ese permiso, acéptalo;
     si no funciona, usa el archivo local.
4. Pulsa **Conectar ASTRA**. Arriba a la derecha deben quedar dos indicadores verdes:
   **EMG (WiFi): 1000 Hz** y **Atril: CALIBRANDO · ESPERA** (o el modo en que esté).
5. Busca o registra al paciente y pulsa **Iniciar terapia**.

## 3. Calibración paso a paso (desde la página)

El atril ya no calibra solo al encender: **espera** a que le indiques cada paso.

| Botón en la página | Qué hace el atril | Duración |
|---|---|---|
| **Comenzar calibración** | Borra la calibración anterior y espera (luz blanca tenue) | — |
| **Comenzar reposo** | Mide el reposo (descarta el primer 1 s). Luz azul. Al terminar muestra media y SD y espera | 10 s |
| **Comenzar calibración pasiva** | Mide el artefacto del movimiento pasivo (descarta 0.5 s). Luz azul. Al terminar muestra media y pico y espera | 10 s |
| **Comenzar calibración activa** | 5 × (¡ARRIBA! 4 s luz verde + ABAJO 4 s). Solo mide la parte estable de cada ARRIBA (sin 0.7 s iniciales ni 0.3 s finales) | 40 s |
| (automático) | Con las 3 fases hechas calcula los resultados: reposo, SD, pico pasivo, RMS de referencia, SNR, artefacto, **U_on** y **U_off**, con las mismas advertencias de siempre | — |
| **▶ Iniciar sesión** | Aplica los umbrales y empieza la sesión (estrellas, luz y sonido). La página empieza a registrar | — |

Mientras se mide una fase, la página muestra la indicación grande (REPOSO / PASIVA / ¡ARRIBA! (2/5) / ABAJO),
la cuenta regresiva, si ese momento **se mide o se descarta**, y el RMS que llega al atril.

- Cada fase hecha se puede **Repetir**. Una fase en curso se puede **Cancelar**.
- El orden se respeta: la pasiva necesita el reposo y la activa necesita la pasiva.
- Los umbrales de la sesión **solo cambian** cuando las tres fases están hechas y la calibración es válida.
- **Sin la página:** el botón **BOOT** del ESP32 hace «siguiente paso» (reposo → pasiva → activa → iniciar sesión).
  En el monitor serie: `c` comenzar, `1` `2` `3` fases, `x` cancelar, `s` sesión, `n` siguiente.
- Si prefieres la calibración de antes (todo seguido y la sesión empieza sola), pon
  `CALIBRACION_AUTOMATICA = true` en `esp32_calibracion.ino`.

## 4. Durante la sesión

- **Señal EMG del Arduino:** la señal cruda filtrada en la página (pasa altas 20 Hz y 60 Hz con armónicos,
  igual que el Arduino), en mV a la salida de Nova. Verde = activación detectada por el atril
  (o ARRIBA en la calibración); ámbar = fase de calibración.
- **RMS que recibe el atril:** exactamente los números que llegan por el opto, con U_on y U_off.
  Verde = activación; azul = bloque que la calibración usó.
- **Controles:** Pausa / Seguir, Umbral − / +, Recalibrar (la terapia continúa después).
- **Activaciones:** las mide el atril (inicio, duración, RMS medio y % de la referencia), igual que
  las líneas «Activacion #n» del monitor serie.
- **Mensajes del atril:** los mismos textos del monitor serie del ESP32.
- **Finalizar terapia** guarda la sesión en el historial y deja el atril en pausa.

## 5. Si algo no conecta

| Indicador | Qué revisar |
|---|---|
| EMG: no se pudo conectar | ¿La laptop está en la red ASTRA? Abre `http://192.168.4.1` en el navegador: debe decir «ASTRA - Arduino EMG v4». |
| Atril: sin respuesta | Abre `http://192.168.4.200`: debe decir «ASTRA - atril ESP32 v3.0». En el monitor serie del ESP32 debe salir «WiFi conectado a la red ASTRA». El ESP32 tiene que encenderse después o al mismo tiempo que el Arduino (si no, reintenta cada 15 s). |
| Alarma «El atril no recibe el RMS del Arduino» | Es el opto, no el WiFi: cable del opto, Arduino encendido. |
| Funciona en archivo local pero no en GitHub Pages | Es el bloqueo del navegador explicado arriba: usa el archivo local. |
| La señal EMG se corta a ratos | Acerca la laptop al Arduino. El atril no se ve afectado. |

Las IP se cambian en **Ajustes** (IP del Arduino, IP del atril). Si un día prefieres la señal por cable,
pon `USB_PARA_PAGINA = true` en el Arduino y marca «Señal EMG por USB» en Ajustes. En ese caso la laptop
**no debe estar cargando** mientras el USB toca a un niño con electrodos.

## 6. Qué cambió en los programas

- **Arduino v4:** el muestreo de 1 kHz pasó a una interrupción de temporizador, porque el WiFi del R4
  hace esperas de varios milisegundos. Las líneas del opto salen de una cola de 6.4 s, siempre completas.
  El procesamiento y el formato que recibe el ESP32 no cambian.
- **ESP32 v3.0:** calibración paso a paso, WiFi en el núcleo 0 (los motores siguen en el núcleo 1 y la
  página no los frena), y cada sesión nueva reinicia el conteo de activaciones y el tiempo (sin pausas).
  Las reglas de calibración y detección no cambian.

**Límite que sigue igual** (ver el documento del sistema): U_off = 0.6 × U_on puede quedar por debajo
del reposo, y entonces una activación casi nunca termina. La página lo advierte al terminar la calibración.

**No probado en hardware.** Los programas se revisaron con un compilador de C++, pero no se compilaron con
las placas reales. Revisa en el monitor serie que el Arduino diga «Red WiFi ASTRA lista» y que el
ESP32 diga «WiFi conectado».
