# Calibración basal, verificación supervisada y ajuste del umbral

Versión 4 del sistema: ESP32 `esp32_calibracion.ino` v4.0 + página web. El Arduino **no cambió** (sigue procesando la señal y calculando el RMS que manda por el opto).

> **Estado de las pruebas:** todo se probó en el PC (firmware ejecutado con reloj virtual, simulación y navegador). **No se ha probado con las placas reales ni con niños.** Ver el punto 10.

---

## 1. Archivos modificados

| Archivo | Cambio |
|---|---|
| `firmware/esp32_calibracion/esp32_calibracion.ino` | **v4.0.** Calibración basal con calidad, estados sostenidos, planificador de verificaciones, eventos con ventana de RMS, umbral ajustable con historial, modo supervisado, parámetros configurables y 5 endpoints nuevos. Motores, luz, DFPlayer, tope y paro **no se tocaron** (salvo que, en modo supervisado, `premiado()` no da premio; ver 3). |
| `firmware/arduino_emg/arduino_emg.ino` | **Sin cambios.** |
| `js/atril.js` | Cliente HTTP con los endpoints nuevos, reconexión con cursor de revisiones, y **atril simulado** que reproduce la lógica del firmware (reloj virtual). |
| `js/analisis.js` *(nuevo)* | Análisis de confirmaciones y propuesta de umbral (código puro, probado en Node). |
| `js/app.js`, `index.html`, `css/style.css` | Flujo de calibración basal, aviso discreto, panel de verificación, panel de ajuste, panel de pruebas de la simulación, registro de la sesión. |
| `js/dsp.js`, `js/sources.js`, `js/charts.js`, `js/storage.js` | Ajustes menores (simulador coherente con el RMS, gráfica de RMS, ajustes nuevos). |
| `tests/` *(nuevo)* | `sim.test.js`, `ui.test.js`, `firmware/run.sh` + cabeceras de prueba. |
| `docs/CONEXION_ASTRA.md`, `README.md` | Actualizados. |

---

## 2. Cómo funciona la calibración basal

No se pide **ningún** movimiento pasivo, contracción voluntaria, MVC ni repetición.

1. La fisioterapeuta coloca al niño cómodo y estable, con la cabeza apoyada, y pulsa **Comenzar calibración** y luego **Registrar RMS basal (10 s)**. (Sin la página: tecla `c` y `1`, o el botón BOOT del ESP32.)
2. El ESP32 recibe los bloques de RMS (uno cada 100 ms) y **descarta el primer segundo** (los filtros se estabilizan). Usa los 90 bloques siguientes.
3. Calcula la **media** y la **desviación estándar** (muestral) del RMS basal.
4. **Evalúa la calidad** (ver abajo). Si es rechazada, no se aplica.
5. Si es aceptable, calcula el **umbral preliminar** y queda `LISTA`. La sesión no empieza sola: tú pulsas **Iniciar sesión**.

**Calidad y estabilidad** (en mV de RMS; todos los límites son constantes al inicio del `.ino`):

| Criterio | Advertencia | Rechazo |
|---|---|---|
| Datos interrumpidos durante el registro | — | sí |
| Media | — | < 2 mV (señal casi nula) o > 1500 mV |
| Bloque saturado | — | ≥ 2400 mV |
| Señal plana (SD ≈ 0) | — | sí |
| Variabilidad (SD / media) | > 0.30 | > 0.60 |
| Deriva (2.ª mitad vs 1.ª) | > 25 % | > 50 % |
| Picos aislados (> media + 6 SD) | sí | — |
| Menos del 80 % de los bloques esperados | — | sí |

Una calibración rechazada **no deja iniciar la sesión**. La fisioterapeuta puede repetir el registro o, como decisión clínica explícita, **«Aceptar de todas formas»** (tecla `f`): queda marcada como *aceptada a la fuerza* en el registro.

---

## 3. Cómo se calcula y se usa el umbral

```
U_ON  = media_basal + K × SD_basal                 K = 3    (configurable)
U_OFF = media_basal + F × (U_ON − media_basal)     F = 0.6  (configurable)
```

K y F se cambian en **Ajustes → Parámetros del atril** (se guardan en el ESP32). Si el umbral vigente es el basal sin ajustes, se recalcula al instante con los factores nuevos; si ya fue ajustado y aprobado, **no se pisa**.

El umbral es **preliminar**. Durante la sesión:

- **Estado con histéresis (Schmitt):** pasa a **ALTO** cuando el RMS suavizado (peso 0.5) sube a U_ON, y a **BAJO** cuando baja de U_OFF; entre los dos mantiene el estado anterior.
- **Estado sostenido:** el estado se mantiene **3 s** (configurable) **y** la ventana de RMS de esos 3 s es coherente con él (ALTO: media ≥ U_ON; BAJO: media < U_OFF) **y es estable** (las dos mitades de la ventana no difieren más de 35 %; si cruza un cambio de postura, no se pregunta). Eso solo lo hace *candidato a confirmación*; **no genera un aviso cada 3 s**, y el ESP32 sigue vigilando el RMS aunque no haya aviso.
- **Modo supervisado:** mientras no haya evidencia, el umbral **no controla los actuadores**: estrellas lentas, luz azul tenue y sin sonido (el premio por relajar queda desactivado). El conteo de «activaciones RMS» (≥ U_ON durante 300 ms) sigue existiendo, pero es informativo.
  El control automático se habilita solo con una orden explícita de la fisioterapeuta (botón en la página, tecla `a`), y la página solo lo ofrece cuando el análisis lo respalda (≥ 5 observaciones independientes por postura y exactitud balanceada ≥ 0.85). `CONTROL_AUTOMATICO_INICIAL = true` lo habilita desde el arranque.

---

## 4. Cuándo aparecen las confirmaciones

Aparece una pregunta cuando se cumplen **todas**:

- Hay una **sesión en marcha** (no en pausa), con datos del Arduino y **la página conectada** (consultó al atril en los últimos 3 s; si no, no se gasta el cupo).
- No hay otra pregunta pendiente.
- Queda cupo en el periodo actual.
- Pasó el **intervalo mínimo** desde la pregunta anterior (45 s en los primeros 5 min; 2 min en los ciclos).
- Se alcanzó el **turno repartido** del periodo (la pregunta *n* no antes de `inicio + n × duración/(máx+1)`): evita concentrar todo al inicio y evita tres avisos seguidos al empezar un ciclo.
- Existe un **estado sostenido estable ahora**.
- Su ventana de 3 s **no se solapa** con la de una pregunta o confirmación manual anterior.
- **Prioridad** (puntos): episodio nuevo 3; el mismo estado que sigue (comprobación periódica) 1, 2, 3… cada `T_REVERIFICAR` = 120 s; +2 si ese estado está poco representado en las respuestas; +2 si ese estado ya generó errores. Se pregunta con ≥ 2 puntos. Así un estado que dura mucho se vuelve a comprobar cada ~4 min como mínimo, sin saturar.

Si no hay eventos adecuados, se hacen menos preguntas; **el cupo no usado no se acumula**.

**La pregunta** (aviso discreto, flotante, sin sonido por defecto y sin bloquear la página):

> *Se detectó un estado muscular sostenido. ¿El niño tiene la cabeza y el cuello erguidos?*
> Sí, cabeza y cuello erguidos. · No, cabeza y cuello caídos. · No se puede determinar / descartar.

El aviso **no dice si el RMS estaba alto o bajo** (para no sesgar la respuesta). Caduca a los 2 min de terapia efectiva (cuenta como usada). Si el estado del RMS cambió desde que apareció, el aviso lo advierte.

**Confirmación manual:** botones en el panel *Verificación del umbral*; usan la ventana de RMS de ese momento y **no gastan el cupo automático**.

---

## 5. Ciclos de 5 y 10 minutos

| Tiempo efectivo de terapia | Máx. confirmaciones automáticas | Intervalo mínimo |
|---|---|---|
| 0–5 min | **4** | 45 s |
| 5–15 min | **3** | 120 s |
| 15–25 min | **3** | 120 s |
| 25–35 min … | **3** (se repite hasta el final) | 120 s |

- El ESP32 lo calcula con `msSesion` (tiempo en `SESION`; **la pausa no cuenta**) y temporizadores no bloqueantes. Al empezar un periodo nuevo solo se **reinicia el contador automático**: se conservan calibración, umbral vigente, observaciones e historial de ajustes. No se repite la calibración basal.
- Duraciones y máximos son configurables (`v1s`, `v1max`, `v1min`, `vcs`, `vcmax`, `vcmin`).
- Los contadores viven en el ESP32 y la página solo los muestra: siempre están sincronizados.

---

## 6. Cómo se registra la retroalimentación

Cada evento se crea **en el ESP32 en el momento de la detección**, con un identificador (`boot:id`) y la ventana de RMS **del evento** (no la del momento de responder). Guarda:

momento (tiempo efectivo de terapia + hora aproximada) · RMS medio · SD · mínimo · máximo (ventana de ~3 s) · estado ALTO/BAJO · U_ON y U_OFF vigentes · postura confirmada · resultado (`coincide` / `no coincide` / `sin determinar` / `sin respuesta`) · calidad (ventana y calibración basal) · periodo (primeros 5 min / ciclo *n*) · origen (automática / manual).

- Las confirmaciones **correctas e incorrectas** se conservan.
- «Coincide» significa que la postura es la que predice la **hipótesis** (RMS alto ↔ erguida, configurable a «RMS alto ↔ caída»). Es lo que se verifica, no algo que se asuma.
- Las muestras dentro de una ventana **no** son observaciones independientes (ver 7).
- Todo se guarda con la sesión: `confirmaciones`, `ajustes`, `calibraciones`, `parametros`; y se exporta a CSV (`…_confirmaciones.csv`).
- Panel **Verificación del umbral**: contadores del periodo, tabla de confirmaciones, coincidencias, falsos positivos/negativos, descartadas y sin respuesta.

---

## 7. Cómo se decide si un umbral debe ajustarse

Ver el encabezado de `js/analisis.js`. Resumen (todas las condiciones; si falla una, **no se propone nada**):

1. **Observaciones independientes:** una por ventana; las ventanas de la misma postura separadas menos de 60 s se fusionan. Se excluyen «no se puede determinar» y ventanas de calidad dudosa.
2. **Mínimo de 4 observaciones independientes por postura.** Con menos: *datos insuficientes*, se conserva el umbral.
3. **Separación real:** AUC ≥ 0.75. Si AUC ≤ 0.25 → **relación invertida** (RMS más alto con la cabeza caída): se avisa y no se ajusta. Entre 0.25 y 0.75 → *el RMS no separa de forma confiable*: no se fuerza.
4. Se mide el **umbral actual** con esas observaciones (falsos positivos y negativos, exactitud balanceada).
5. **Umbral candidato:** punto medio entre observaciones que mejor separa.
6. **Validación cruzada dejando una fuera:** el candidato se busca sin esa observación y se prueba en ella. Debe dar ≥ 0.75.
7. Debe **mejorar** al actual (≥ +0.10 en el ajuste y ≥ +0.05 en la validación), cambiar ≥ 10 %, quedar ≥ 2 SD sobre el reposo, y estar entre 1/3× y 3× el actual (entre 2× y 3× se advierte que conviene revisar electrodos y repetir la basal).
8. Si el umbral actual ya clasifica bien (≥ 0.90), *funciona bien*: no se toca.
9. **Nunca se modifica con una sola confirmación incorrecta.**

**Propuesta en la página:** umbral actual vs propuesto, motivo, exactitud, falsos positivos/negativos, **Aprobar y enviar al atril** / **Rechazar** / **Restaurar el umbral anterior**. Un ajuste rechazado no se vuelve a proponer hasta tener 3 observaciones independientes nuevas. El ESP32 solo recibe el umbral después de aprobarlo.

---

## 8. Cómo se comunica la página con el ESP32

HTTP por WiFi (red `ASTRA`), `http://192.168.4.200`. La página consulta `/estado` ~6 veces por segundo y manda órdenes:

| Endpoint | Para qué |
|---|---|
| `/estado?b=&a=&l=&r=` | Estado, RMS y umbrales, contadores de verificación, parámetros, historial de umbrales, mensajes y **eventos nuevos o modificados** (cursor `r` = revisión). |
| `/cmd?c=` | `c` comenzar, `1` registrar basal, `x` cancelar, `f` aceptar a la fuerza, `s` iniciar sesión, `p` pausa, `r`, `h`, `a` control automático, `n`, `i`. |
| `/conf?id=&p=E\|C\|D` | Postura de un evento pendiente. |
| `/manual?p=&n=` | Confirmación manual. |
| `/umbral?on=&off=&n=` | Umbral aprobado. |
| `/restaurar?n=` | Vuelve al umbral anterior. |
| `/cfg?k=&v=` | Parámetros (validados con rangos). |

- **Sin duplicados:** `/conf` es idempotente por identificador de evento; `/manual`, `/umbral` y `/restaurar` llevan un identificador `n` y el ESP32 ignora un reintento con el mismo `n`. La página reintenta hasta que `/estado` muestra que se aplicó.
- **Las órdenes entran a una cola** y las ejecuta el `loop()` (núcleo 1): la red (núcleo 0) nunca cambia el estado directamente ni frena los motores.
- **Si la página se desconecta**, el ESP32 conserva umbrales, parámetros, eventos (los últimos 48) y contadores, sigue monitoreando y **no hace preguntas** (no gasta el cupo). Al volver, la página pide todo desde cero y reconoce lo que ya tenía por su identificador. Si el ESP32 se reinicia, la página lo detecta.
- **Aislamiento eléctrico:** no se añadió ninguna conexión entre Arduino y ESP32. El opto sigue siendo el único enlace; todo lo nuevo viaja por WiFi.

---

## 9. Cómo probarlo paso a paso (sin niños)

### A. Solo con la simulación (sin placas)
1. Doble clic en `index.html` (Chrome o Edge) → registra un paciente → **Iniciar terapia** → **Simulación** (arriba).
2. Se abre el **Panel de pruebas de la simulación** (al final de la página).
3. **Calibración:** *Comenzar calibración* → *Registrar RMS basal (10 s)*. Ves los 10 s, la calidad, la media, la SD y U_ON/U_OFF (cámbialos en Ajustes → K y F).
   - Para ver el rechazo: antes de registrar, en el panel elige *Calidad del registro basal* = Ruidoso / Plano / Casi nulo / Deriva fuerte / Saturado → sale «no es aceptable» con el motivo y el botón *Aceptar de todas formas*. Con *Deriva moderada* queda aceptada con advertencia.
4. **Sesión:** *Iniciar sesión* (queda «supervisado»). En el panel, *Mantener cabeza caída* o *erguida*. Tras 3 s sostenidos y con la página conectada aparece el **aviso** abajo a la derecha. Respóndelo y mira la tabla.
   *Para no esperar:* en **Ajustes → Parámetros del atril** pon `Primer periodo de verificación = 60`, `intervalo = 10`, o usa *Velocidad del tiempo* 10×.
5. **Cupos y ciclos largos:** velocidad 300×, *Respuestas automáticas = Sí* y *Postura cambiante*: en unos segundos pasan horas de terapia; mira *Preguntas en este periodo* (máx. 4 y luego 3).
6. **Estado prolongado:** *Mantener cabeza erguida* y velocidad alta: pocas comprobaciones, separadas.
7. **Respuestas incorrectas:** *Error de la fisioterapeuta simulada* = 20–45 %.
8. **Propuesta de ajuste:** pon *RMS con cabeza caída* = 52 (más alto que el umbral basal ≈ 39), erguida = 130, respuestas automáticas Sí, velocidad 300×. Tras ~1–2 h simuladas aparece **Propuesta de ajuste** → *Aprobar* (el atril cambia el U_ON), *Restaurar el umbral anterior*, o *Rechazar*.
   - *Relación invertida* y *Sin relación* en el panel muestran que **no** se propone nada.
   - Pocas confirmaciones → «datos insuficientes».
9. **Pausa:** *Pausa*: el tiempo efectivo y los periodos se detienen. *Seguir*.
10. **Desconexión de la página:** *Cortar la página 15 s*: el atril sigue; al volver se resincroniza sin duplicar.
11. **Finalizar terapia** y revisa el historial; *Guardar datos en CSV* descarga las confirmaciones.

### B. Con el hardware (sin niño)
1. Carga los dos programas, enciende ambos, conéctate a la red `ASTRA` y pulsa **Conectar ASTRA**. Revisa el monitor serie (`WiFi conectado`).
2. **Primero con una persona adulta voluntaria o con el sensor sobre ti** (no con un niño), con electrodos puestos y quieta: registra el basal. Si sale rechazado, el motivo te dice qué revisar.
3. Mueve la cabeza/contrae el músculo (adulto) y mira el aviso y el RMS. Responde honestamente.
4. Comprueba: pausa, desconectar el WiFi de la laptop 1 min y volver, `Ajustes → parámetros`, aprobar/restaurar un umbral.
5. **Antes de cualquier uso con niños:** prueba con el ESP32 y el Arduino en banco con los motores conectados y revisa que en modo supervisado no se mueven según el RMS.

### C. Pruebas automáticas
```bash
node tests/sim.test.js        # lógica completa en tiempo virtual (273 comprobaciones)
node tests/ui.test.js         # interfaz en Chromium con Playwright (39 comprobaciones)
bash tests/firmware/run.sh    # el firmware real del ESP32 ejecutado en el PC (13 escenarios)
```

---

## 10. Qué se probó y qué falta

**Probado (en el PC):**
- Firmware del ESP32 ejecutado tal cual (13 escenarios, `tests/firmware`): basal de 10 s y fórmulas; calidad (plana, nula, ruidosa, deriva, saturada, datos cortados, forzar, cancelar); estados sostenidos e histéresis; ráfagas cortas; ≤ 4 en los primeros 5 min y ≤ 3 por ciclo en **75 min y 3 h**; intervalos mínimos; ventanas sin solapar; nunca varias preguntas al empezar un ciclo; estado prolongado sin saturar; sin página no se gasta el cupo; pausa (el tiempo no avanza); respuestas correctas/incorrectas e idempotencia; manual; expiración; umbral aprobado, rechazado, restaurado y con identificador repetido; cursor de revisiones y JSON válido; modo supervisado vs control automático; ventanas que cruzan una transición.
- Lógica de simulación y análisis (`tests/sim.test.js`): todos los casos de tu lista de pruebas, incluidas propuesta, datos insuficientes, relación invertida, sin relación, fisioterapeuta con 45 % de error y sesión de 3 h.
- Interfaz en Chromium (`tests/ui.test.js`): flujo completo, texto exacto del aviso, aviso que no bloquea, aprobar/restaurar, desconexión/reconexión, registro guardado.
- Sintaxis del firmware con `g++` (formatos `printf` incluidos).

**Pendiente / no probado:**
- **No se compiló con el Arduino IDE** ni se cargó en el ESP32 real (no hay toolchain aquí). Puede haber algún error de compilación específico de las placas.
- **No se probó el WiFi real:** las 5 rutas nuevas, el tamaño de `/estado` (≈ 3–5 KB) y la latencia con el ESP32 de verdad, ni Chrome con la restricción de «red local» (si usas GitHub Pages en vez del archivo local).
- **No se probó con señal EMG real ni con niños.** Los umbrales de calidad (CV 0.30/0.60, deriva 25/50 %, etc.), la estabilidad de ventana (35 %) y los criterios de propuesta (AUC 0.75, 4 observaciones, +0.10…) son **supuestos razonables a calibrar con datos reales**. Con niños con parálisis cerebral el RMS en reposo puede ser mucho más variable que en la simulación.
- No se probó la fatiga/ritmo de la fisioterapeuta con los avisos en una terapia real (¿4 en 5 min es demasiado?).
- El ESP32 **no guarda nada en memoria permanente**: al reiniciarse pierde calibración, umbrales y eventos (a propósito: una calibración depende de la colocación de los electrodos).

**Decisiones que tomé y que debes validar:**
1. En modo supervisado **se desactiva el premio** de los actuadores (estrellas lentas, luz azul, sin sonido). Es la lectura estricta de «no usar el umbral para el control automático».
2. La hipótesis por defecto es **RMS alto ↔ cabeza erguida**, configurable.
3. Las observaciones independientes son ventanas separadas ≥ 60 s de la misma postura (no episodios). Una ventana estable dura 3 s, pero varias ventanas del mismo estado largo cuentan como observaciones distintas: es optimista; por eso el control automático exige además ≥ 5 por postura y exactitud ≥ 0.85, y siempre lo decide la fisioterapeuta.
4. Si la respuesta llega tarde y el niño ya cambió, el aviso lo advierte, pero la respuesta se asocia al evento original.
