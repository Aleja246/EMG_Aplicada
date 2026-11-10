# ASTRA: cómo funciona y cómo conectarlo (servidor en la laptop)

## Cómo funciona

```
 Electrodos → Nova → Arduino UNO R4 WiFi ──opto (RMS cada 100 ms, igual que siempre)──→ ESP32 (atril)
                          │                                                              │  estrellas, lazy susan,
                          │ WiFi: señal EMG cruda (1000/s)                                │  luz y música
                          ▼                                                              │
                   ┌──────────────────────────────────────┐   WiFi: estado, activaciones,│
                   │  SERVIDOR en la laptop               │◄──────── mensajes y música ──┘
                   │  servidor/servidor.js (Node.js)      │────────► órdenes (pausa, umbral, recalibrar, canciones)
                   │  · avisa por la red "aquí estoy"     │
                   │  · guarda pacientes y sesiones       │
                   └───────────────┬──────────────────────┘
                     WebSocket /ws │ (en vivo)      HTTP /api/... (pacientes y sesiones)
                                   ▼
                      PÁGINA en Chrome o Edge: http://localhost:8080
```

| Parte | Programa | Qué hace |
|---|---|---|
| **Arduino UNO R4 WiFi** | `firmware/arduino_emg/arduino_emg.ino` (v5) | Mide igual que tu v2 y le manda el RMS al ESP32 por el opto **exactamente igual**. Además se une a la red WiFi, encuentra solo al servidor y le manda la **señal cruda** (1000 muestras/s). |
| **ESP32 (atril)** | `firmware/esp32_calibracion/esp32_calibracion.ino` (v7) | Tu código con sonido **sin cambios en lo que hace** (calibración, detección, motores, luz, música). Además se une a la red, encuentra al servidor, le manda su estado 5 veces por segundo y recibe las órdenes de la página (incluida la elección de canciones). |
| **Servidor** | `servidor/servidor.js` | Corre en la laptop. Recibe a las dos placas, reparte todo en vivo a la página por **WebSocket** y guarda pacientes y sesiones en `servidor/datos/` (API **HTTP** `/api/...`). |
| **Página** | `index.html` (la abre el servidor) | Registro y búsqueda de pacientes, gráfica de la señal EMG del Arduino en vivo, estado del atril, calibración, activaciones, música, historial con la señal de cada sesión y notas. |

- **Nadie se conecta a la red del Arduino.** La laptop y las dos placas están en la misma red WiFi:
  la del consultorio, o la **zona con cobertura (hotspot)** de la laptop.
- **El opto sigue siendo el único enlace entre el Arduino y el ESP32.** Si se cae el WiFi o se cierra la página,
  el atril sigue funcionando igual que antes.
- **Las placas encuentran solas al servidor:** cada segundo el servidor avisa por la red «aquí está ASTRA» (UDP, puerto 9999).
  No hay que escribir ninguna IP.
- **Los datos de los pacientes se quedan en la laptop** (`servidor/datos/`), nunca en internet ni en GitHub.

## Paso a paso

### 1. Una sola vez: preparar la laptop
1. Instala **Node.js** (versión LTS) desde nodejs.org. No hace falta instalar nada más.
2. Instala el **Arduino IDE 2** y, dentro de él:
   - *Herramientas → Placa → Gestor de placas*: **Arduino UNO R4 Boards** y **esp32 (Espressif Systems)**.
   - *Herramientas → Administrar bibliotecas*: **AccelStepper**.
3. Descarga el proyecto de GitHub (**Code → Download ZIP**) y descomprímelo.

### 2. Elegir la red WiFi
La laptop, el Arduino y el ESP32 tienen que estar en **la misma red**:
- **Opción A, la del consultorio:** usa su nombre y su clave.
- **Opción B, recomendada si la red del consultorio es de invitados o bloquea equipos:** en Windows, *Configuración → Red e Internet →
  Zona con cobertura inalámbrica móvil*. Actívala y anota el **nombre** y la **contraseña**. La laptop sigue con internet.

Escribe ese nombre y esa clave **al inicio de los dos programas**:
```
RED_SSID  = "nombre de la red"
RED_CLAVE = "clave"
```

### 3. Cargar los programas (una sola vez, o cuando cambies la red)
1. **Arduino:**
   - Abre `firmware/arduino_emg/arduino_emg.ino`, elige la placa **Arduino UNO R4 WiFi** y su puerto, y pulsa **Subir**.
   - En el Monitor Serie (250000 baud) verás: «WiFi conectado…», «Servidor de la pagina encontrado…» y «Conectado al servidor».
     Esto último sale cuando el servidor del paso 4 ya está abierto.
2. **ESP32:**
   - Abre `firmware/esp32_calibracion/esp32_calibracion.ino`, elige **ESP32 Dev Module** y su puerto, y pulsa **Subir**.
     Si se queda en «Connecting…», mantén presionado **BOOT**.
   - En el Monitor Serie (115200 baud, «Sin ajuste de línea») verás: «WiFi conectado a la red…» y «Conectado al servidor de la pagina».
3. Desconecta los USB y deja cada placa en su power bank.
   **No conectes las dos placas por USB a la laptop con el niño conectado:** unirías las tierras que el opto mantiene separadas.

### 4. Cada sesión
1. Conecta la laptop a la red elegida (o activa su zona con cobertura).
2. Abre **`servidor/iniciar-servidor.bat`** con doble clic (en Mac: `iniciar-servidor.command`).
   - Se abre una ventana negra (déjala abierta) y el navegador en **http://localhost:8080**.
   - La primera vez Windows pregunta si permites que Node.js use la red: elige **Redes privadas → Permitir**.
3. Enciende el Arduino y el atril. En unos segundos, arriba a la derecha de la página: **«EMG (WiFi): ~1000 Hz»** y **«Atril: …»** en verde.
4. **Registrar paciente** o **Buscar paciente** → **Iniciar terapia**.
5. El atril calibra solo (10 s de RMS basal con la cabeza apoyada) y la página **empieza a registrar la sesión** al terminar.
   Si el atril ya estaba en sesión, pulsa **Registrar la sesión desde ahora**.
6. **En vivo** ves:
   - la señal EMG del Arduino;
   - el RMS y la mediana que usa el atril, con U_on y U_off;
   - la cabeza abajo / arriba, las activaciones y los mensajes del monitor serie.

   Puedes usar **Pausa**, **Umbral − / +**, **Recalibrar** e **Invertir abajo / arriba**.
7. **Música del atril:** elige la canción para la cabeza **abajo** (001–003, o las tres en orden) y para la cabeza **arriba** (004–006).
   El aviso `/ADVERT/0001.mp3` suena cada vez que sube la cabeza y luego sigue la canción. El ESP32 guarda la elección en su memoria.
8. **Finalizar terapia** guarda la sesión del paciente en la laptop: activaciones, tiempos, RMS, calibración, música y la señal RMS de toda la sesión.
9. En **Historial** del paciente:
   - el progreso entre sesiones;
   - el botón **Señal** para ver la de cada sesión;
   - el CSV;
   - las **notas de la fisioterapeuta**.

**Ver desde otro equipo** (por ejemplo, una tablet en la misma red): abre `http://IP-de-la-laptop:8080`. La IP aparece en la ventana del servidor.

**Datos que ya tenías en el navegador:** en *Ajustes* aparece **«Subir al servidor los datos guardados en este navegador»**.

**MicroSD del DFPlayer** (FAT32): `/01/001.mp3` … `/01/006.mp3` y `/ADVERT/0001.mp3`.

## Si algo no conecta

| Lo que ves | Qué revisar |
|---|---|
| La página dice «No hay conexión con el servidor» | ¿Está abierta la ventana negra del servidor? Abre la página desde **http://localhost:8080**, no con doble clic en `index.html`. |
| «EMG: Arduino sin conectar» | Monitor Serie del Arduino: ¿dice «WiFi conectado»? Si no, revisa `RED_SSID` / `RED_CLAVE`. ¿Dice «Servidor encontrado»? Si no, ver la fila de abajo. |
| «Atril: ESP32 sin conectar» | Lo mismo en el Monitor Serie del ESP32. |
| Las placas se conectan a la red pero nunca «encuentran el servidor» | La red bloquea los avisos entre equipos (redes de invitados), o el firewall de Windows bloqueó Node.js. Usa la **zona con cobertura** de la laptop, o escribe la IP de la laptop en `SERVIDOR_IP` en los dos programas (la muestra la ventana del servidor). |
| «Señal saturada» | Electrodos, electrodo de referencia o cable de Nova: la señal está pegada a 0 V o a 5 V. Pasa el cursor sobre el indicador EMG para ver el rango que mide el Arduino. |

## Qué se agregó a tus programas (y qué no cambió)
- **Arduino v5:** el muestreo, los filtros, el RMS, el opto y el Serial Plotter son los mismos de tu v2.
  - El muestreo va en una interrupción, para que el WiFi nunca lo retrase.
  - Se agregó: unirse a la red, encontrar al servidor y mandarle la señal cruda y un diagnóstico cada segundo.
- **ESP32 v7:** probado en la computadora con el mismo escenario que tu código; imprime los mismos mensajes y manda los mismos comandos al DFPlayer.
  - Se agregó, en el otro núcleo del ESP32: la conexión con el servidor, las órdenes de la página y la elección de canciones (guardada en su memoria).

**Falta probarlo en las placas reales:** se probó con un servidor real, con la lógica del ESP32 corriendo en la computadora conectada a ese servidor, con un Arduino simulado y con la página en el navegador.
