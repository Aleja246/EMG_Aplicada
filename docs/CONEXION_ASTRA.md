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
| **ESP32 (atril)** | `firmware/esp32_calibracion/esp32_calibracion.ino` (v5.1) | Tu v5 sin cambios en lo que hace: calibración basal automática, mediana de 0.5 s, umbrales, «cabeza abajo», estrellas, lazy susan, luz y música. Lo nuevo: se une a la red ASTRA (`http://192.168.4.200`) y le cuenta a la página todo lo que mide. |
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
   Al **subir** la cabeza (tras ≥ 1 s abajo) cambia la canción.
4. **Seguridad:** sin datos 500 ms → estrellas lentas y luz ámbar.

## Primera vez: cargar los programas

1. **Arduino** (placa «Arduino UNO R4 WiFi»): `firmware/arduino_emg/arduino_emg.ino`. Si dice «No responde el módulo WiFi», actualiza el firmware del módulo (IDE 2 → *Herramientas → Firmware Updater*).
2. **ESP32** («ESP32 Dev Module»): `firmware/esp32_calibracion/esp32_calibracion.ino`. Necesita AccelStepper; WiFi y WebServer vienen con las placas ESP32.
3. La red y la clave están al inicio de **los dos** programas y deben ser iguales: `RED_SSID = "ASTRA"`, `RED_CLAVE = "astra2026"`.

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
7. **Finalizar terapia** guarda la sesión en el historial del paciente y deja el atril en pausa. **Guardar datos en CSV** descarga las activaciones y el RMS.

## Qué se guarda del paciente (todo lo mide el ESP32)

- Cada activación: inicio, duración y RMS medio (mV).
- Resumen: número de activaciones, tiempo total con la cabeza abajo y % de la sesión, episodio más largo, duración media, RMS medio.
- Calibración usada: μ, σ, σ/μ, bloques, factor, U_on, U_off, avisos, y si «activación» = cabeza abajo o arriba.
- Datos de la sesión que escribes tú: músculo/electrodos, posición, fisioterapeuta, notas.
- En **Historial** se grafica el progreso entre sesiones.

## Si algo no conecta

| Indicador | Qué revisar |
|---|---|
| EMG: no se pudo conectar | ¿Laptop en la red ASTRA? Abre `http://192.168.4.1`: debe decir «ASTRA - Arduino EMG v4». |
| Atril: sin respuesta | Abre `http://192.168.4.200`: debe decir «ASTRA - atril ESP32 v5.1». El monitor serie del ESP32 debe decir «WiFi conectado a la red ASTRA» (reintenta cada 15 s si el Arduino arrancó después). |
| «El atril no recibe el RMS del Arduino» | Es el opto, no el WiFi: cable del opto y Arduino encendido. |
| Funciona en el archivo local pero no en GitHub Pages | Bloqueo del navegador (ver arriba): usa el archivo local. |

Las IP se cambian en **Ajustes**. Si prefieres la señal por cable, pon `USB_PARA_PAGINA = true` en el Arduino y marca
«Señal EMG por USB» en Ajustes; en ese caso la laptop **no debe estar cargando** mientras el USB toca a un niño con electrodos.

**Probado sin hardware:** la lógica del ESP32 v5.1 se ejecutó en la computadora con el mismo escenario que tu v5 y
produce exactamente los mismos mensajes; la página se probó contra ella por HTTP y con la simulación.
Falta compilarlo y probarlo en las placas reales.
