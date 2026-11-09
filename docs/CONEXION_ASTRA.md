# ASTRA + página web: cómo funciona y cómo conectarlo

## Qué hace cada parte

```
Electrodos → Nova → Arduino UNO R4 WiFi ──opto: RMS cada 100 ms──→ ESP32 (atril) → estrellas, lazy susan, luz, música
                         │  red WiFi "ASTRA"                               │ WiFi
                         └── señal EMG cruda (1000/s) ──→ LAPTOP ←── estado, activaciones, comandos ──┘
                                                        (página)
```

| Parte | Programa | Qué hace |
|---|---|---|
| **Arduino UNO R4 WiFi** | `firmware/arduino_emg/arduino_emg.ino` (v4) | Lee la señal 1000 veces/s, quita DC, 60 Hz y armónicos, y cada 100 ms manda **un número (RMS en mV) al ESP32 por el opto** (igual que tu v2). Además crea la red WiFi **ASTRA** y manda la **señal cruda** a la página en `http://192.168.4.1/stream`. |
| **ESP32 (atril)** | `firmware/esp32_calibracion/esp32_calibracion.ino` (v6) | Tu versión con sonido sin cambios en lo que hace: calibración basal automática, mediana de 0.5 s, umbrales, «cabeza abajo», estrellas, lazy susan, luz y música (dos listas + aviso). Lo nuevo: se une a la red ASTRA (`http://192.168.4.200`), le cuenta a la página todo lo que mide y deja **elegir las canciones** desde la página (las guarda en su memoria). |
| **Página** | `index.html` | Muestra la señal EMG del Arduino y el RMS del atril en tiempo real, y **guarda la sesión del paciente con los datos que da el ESP32** (activaciones, tiempos, RMS, calibración). Manda los mismos comandos que el monitor serie. |

El opto sigue siendo el único enlace entre el Arduino y el ESP32. Si la página o el WiFi fallan, el atril funciona igual que antes.
Todo es inalámbrico: la laptop no toca la tierra del niño ni la del atril (cada placa con su power bank).

## Cómo decide el ESP32 (v5)

1. **Arranque:** las estrellas buscan el microswitch (ahí queda la posición 0).
2. **Calibración basal automática (10 s, luz azul):** cabeza apoyada y estable, sin pedir contracciones.
   Se descarta el primer segundo. Con la **mediana de 0.5 s** del RMS calcula
   - μ = media basal, σ = desviación estándar (mínimo 0.5 mV)
   - **U_on = μ + máx(2·σ, 2 mV)** (el factor 2 se cambia con «Umbral +/−» de 0.5 en 0.5)
   - **U_off = μ + 0.6·(U_on − μ)**
   Termina a los 10 s por reloj. Si llegaron menos de 20 bloques conserva los umbrales anteriores; si la señal fue inestable (σ/μ > 50 %) solo avisa.
3. **Sesión:** cuando la mediana pasa **U_on durante 300 ms** empieza una activación; termina cuando baja de **U_off durante 300 ms**.
   Con `ABAJO_ES_ACTIVIDAD = true`, activación = **cabeza abajo**: estrellas aceleran de 600 a 1000 pasos/s, luz brillante y colores rápidos.
   **Música:** con la cabeza abajo suena la lista «abajo» (001–003) y con la cabeza arriba la lista «arriba» (004–006).
   Al **subir** la cabeza (tras ≥ 1 s abajo) suena el aviso `/ADVERT/0001.mp3` y luego sigue la canción.
4. **Seguridad:** sin datos 500 ms → estrellas lentas y luz ámbar.

## Primera vez: cargar los programas (Arduino IDE 2)

**Una sola vez en la computadora:**
1. Instala el **Arduino IDE 2** (arduino.cc/en/software).
2. *Herramientas → Placa → Gestor de placas*: instala **«Arduino UNO R4 Boards»** y **«esp32» (de Espressif Systems)**.
3. *Herramientas → Administrar bibliotecas*: instala **AccelStepper** (de Mike McCauley). Lo demás (WiFiS3, WiFi, WebServer, Preferences) ya viene con las placas.

**Arduino UNO R4 WiFi** (lado del niño):
1. Abre `firmware/arduino_emg/arduino_emg.ino` (la carpeta y el archivo deben llamarse igual).
2. Conéctalo por USB. *Herramientas → Placa*: **Arduino UNO R4 WiFi**; *Puerto*: el que aparece al conectarlo.
3. Pulsa **Subir** (flecha →).
4. Abre el *Monitor Serie* a **250000 baud**: debe decir «Red WiFi ASTRA lista».
   Si dice «No responde el módulo WiFi», actualiza el módulo: *Herramientas → Firmware Updater*.
5. Desconecta el USB y vuelve a conectarlo a su power bank.

**ESP32** (atril):
1. Abre `firmware/esp32_calibracion/esp32_calibracion.ino`.
2. *Placa*: **ESP32 Dev Module**; *Puerto*: el del ESP32. Pulsa **Subir** (si se queda en «Connecting…», mantén presionado **BOOT** hasta que empiece).
3. *Monitor Serie* a **115200 baud** y **«Sin ajuste de línea»**: debe decir «WiFi conectado a la red ASTRA» (con el Arduino encendido).
4. Desconecta el USB y vuelve a su power bank.

**MicroSD del DFPlayer** (FAT32):
```
/01/001.mp3  /01/002.mp3  /01/003.mp3   ← cabeza ABAJO
/01/004.mp3  /01/005.mp3  /01/006.mp3   ← cabeza ARRIBA
/ADVERT/0001.mp3                         ← aviso al subir la cabeza
```

La red y la clave están al inicio de **los dos** programas y deben ser iguales: `RED_SSID = "ASTRA"`, `RED_CLAVE = "astra2026"`.
**No conectes las dos placas por USB a la misma laptop a la vez con el niño conectado**: unirías las tierras que el opto mantiene separadas.

## Cada sesión

1. Enciende el Arduino y el atril. En unos segundos aparece la red **ASTRA**.
2. Conecta la laptop a la red **ASTRA** (clave `astra2026`; no tiene internet, es normal).
3. Abre `index.html` **desde el archivo** (doble clic, Chrome o Edge) y pulsa **Conectar ASTRA**.
   Desde GitHub Pages (`https://…`) el navegador bloquea las direcciones `http://192.168…` de la red local.
4. Busca o registra al paciente → **Iniciar terapia**.
5. **Si el atril todavía está calibrando** (o buscando el tope), la página muestra la cuenta regresiva del registro basal y,
   al terminar, **empieza a registrar la sesión sola**.
   **Si el atril ya estaba en sesión**, pulsa **Registrar la sesión desde ahora** (o **Recalibrar primero**).
6. Durante la sesión: **Pausa / Seguir**, **Umbral − / +**, **Recalibrar**, **Invertir abajo / arriba** (tecla `x`).
   En **Música del atril** eliges qué suena con la cabeza **abajo** (las tres 001 → 002 → 003 en orden, o solo una) y con la cabeza
   **arriba** (004 → 005 → 006, o solo una). Si esa lista está sonando, cambia en ese momento; si no, se oye la próxima vez.
   El atril guarda la elección en su memoria (sigue igual al apagarlo, con o sin página), y la página se la vuelve a mandar si el atril se reinicia.
7. **Finalizar terapia** guarda la sesión en el historial del paciente y deja el atril en pausa. **Guardar datos en CSV** descarga las activaciones y el RMS.

## Qué se guarda del paciente (todo lo mide el ESP32)

- Cada activación: inicio, duración y RMS medio (mV).
- Resumen: número de activaciones, tiempo total con la cabeza abajo y % de la sesión, episodio más largo, duración media, RMS medio.
- Calibración usada: μ, σ, σ/μ, bloques, factor, U_on, U_off, avisos, y si «activación» = cabeza abajo o arriba.
- Las listas de música que sonaron.
- Datos de la sesión que escribes tú: músculo/electrodos, posición, fisioterapeuta, notas.
- En **Historial** se grafica el progreso entre sesiones.

## Si algo no conecta

| Indicador | Qué revisar |
|---|---|
| EMG: no se pudo conectar | ¿Laptop en la red ASTRA? Abre `http://192.168.4.1`: debe decir «ASTRA - Arduino EMG v4». |
| Atril: sin respuesta | Abre `http://192.168.4.200`: debe decir «ASTRA - atril ESP32 v6». El monitor serie del ESP32 debe decir «WiFi conectado a la red ASTRA» (reintenta cada 15 s si el Arduino arrancó después). |
| «El atril no recibe el RMS del Arduino» | Es el opto, no el WiFi: cable del opto y Arduino encendido. |
| Funciona en el archivo local pero no en GitHub Pages | Bloqueo del navegador (ver arriba): usa el archivo local. |

Las IP se cambian en **Ajustes**. Si prefieres la señal por cable, pon `USB_PARA_PAGINA = true` en el Arduino y marca
«Señal EMG por USB» en Ajustes; en ese caso la laptop **no debe estar cargando** mientras el USB toca a un niño con electrodos.

**Probado sin hardware:** la lógica del ESP32 v6 se ejecutó en la computadora con el mismo escenario que tu versión con sonido y
produce exactamente los mismos mensajes y los mismos comandos al DFPlayer; la elección de canciones se probó aparte
(cambia al momento, no corta el aviso, se guarda) y la página se probó contra ese firmware por HTTP y con la simulación.
Falta compilarlo y probarlo en las placas reales.
