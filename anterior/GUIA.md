# Guía del proyecto

## 1. Primeros pasos con GitHub (para quien lo usa por primera vez)
Conceptos: **repositorio** = carpeta del proyecto en la nube · **commit** = foto guardada de los cambios · **branch (rama)** = línea de trabajo paralela · **pull request (PR)** = petición para integrar una rama en `main`.

Este trabajo está en la rama `claude/semg-web-interface-9g04cw`. Para publicarlo:
1. En GitHub abre el repo `Aleja246/EMG_Aplicada` → verás un aviso «Compare & pull request» → **Create pull request** → **Merge pull request**. Con eso `main` ya tiene la página.
2. **Settings → Pages → Build and deployment → Source: Deploy from a branch → Branch: `main`, carpeta `/ (root)` → Save.**
3. En ~1 minuto la página queda en `https://aleja246.github.io/EMG_Aplicada/` (HTTPS, necesario para Web Serial).
4. Para editar: descarga el repo (**Code → Download ZIP**, o usa GitHub Desktop), modifica archivos y haz commit. Cada cambio en `main` se publica solo.

Alternativa sin GitHub: doble clic en `index.html`. Funciona igual (los datos se guardan por navegador y por dirección, así que no se comparten entre «archivo local» y «GitHub Pages»; usa el respaldo JSON para moverlos).

## 2. Cómo se comunica el Arduino con la página
```
Electrodos → sensor sEMG → pin A1 (ADC 14 bits, 2000 Hz) → Arduino → USB (serial) → Navegador (Web Serial) → filtros/métricas en JavaScript → pantalla
```
- El Arduino **solo muestrea y envía** números crudos (`emg_stream.ino`), en paquetes de 20 muestras (10 ms) con contador y checksum.
- La página pide permiso para usar el puerto serial (**Conectar Arduino** → eliges el puerto; el navegador lo exige por seguridad, una vez por sesión).
- Requisitos: Chrome/Edge de escritorio (Firefox y Safari no soportan Web Serial), HTTPS o `localhost`/archivo local, y **Monitor Serie cerrado**.
- Todo el procesamiento (notch, pasa altas, envolvente, RMS/MAV) se hace en el navegador, igual que en tu código base. Así puedes cambiar filtros y umbrales sin re-flashear.
- Si tu placa no es de 14 bits, ajusta «Máximo del ADC» en Ajustes (1023 para 10 bits). La frecuencia de muestreo debe coincidir con el firmware (2000 Hz).

### Indicadores de conexión y alarmas
Arriba a la derecha: chip **verde con punto que pulsa** = llegan datos (muestra Hz medidos), ámbar = frecuencia anómala, rojo = sin datos o error. Un banner rojo (con pitido) o ámbar aparece si:
| Alarma | Causa probable |
|---|---|
| Sin datos / conexión perdida | cable USB, placa apagada, Monitor Serie abierto |
| Señal saturada (≥20 % de muestras en el límite del ADC) | electrodo/cable desconectado, sin referencia |
| Señal plana (sin ruido) | sensor sin alimentación, cable de señal suelto |
| Ruido de 60 Hz > 70 % de la energía | mal contacto, referencia floja, cables largos |
| Paquetes perdidos / frecuencia ≠ 2000 Hz | hub USB, ordenador saturado, firmware distinto |

Con **Simulación → «Simular falla…»** puedes ver cada alarma sin hardware.

## 3. Tiempo real y números
La gráfica (señal filtrada + envolvente) se redibuja a ~60 cuadros/s, sin retraso apreciable (el retardo propio de la envolvente a 5 Hz es ~100 ms). Los números (activaciones, tiempos, RMS) se refrescan 4 veces por segundo: es suficiente para leerlos y no satura la pantalla. Las métricas por activación (RMS, MAV) se fijan cuando la activación termina.

## 4. Calibración (10 s reposo · 10 s pasiva · 5 repeticiones activas)
| Fase | Qué se mide | Para qué |
|---|---|---|
| Reposo | media y desviación de la envolvente | umbral = reposo + 3·SD |
| Pasiva (el terapeuta mueve la cabeza) | pico de envolvente | cuantifica el **artefacto de transición**; se muestra como % de la activa |
| Activa (N repeticiones: ARRIBA / ABAJO) | RMS y envolvente en la parte estable (se omiten 0.7 s iniciales y 0.3 s finales de cada «arriba») | referencia para **RMS normalizado = RMS / RMS_activo × 100** |

**Supuestos a confirmar:** la duración de cada repetición activa (por defecto 4 s arriba + 4 s abajo; cambia en Ajustes) y el umbral mínimo (25 % de la envolvente activa).

### Tu observación sobre las transiciones (abajo→arriba y arriba→abajo)
El sistema las trata así: una activación solo es **válida** si la envolvente se mantiene sobre el umbral ≥ 1 s (Ajustes → «Duración mínima»); las ráfagas cortas de transición se descartan y se cuentan aparte («transiciones descartadas»). Si la transición abajo→arriba queda pegada al inicio de la contracción real, puedes usar «Ignorar primeros (s)» (p. ej. 0.5) para que no infle el RMS/MAV. Con datos reales de la fase pasiva decidiremos valores definitivos; también se podría detectar el artefacto por su forma (pico brusco) en lugar de solo por duración.

## 5. Métricas y progreso
Métricas solicitadas: número de activaciones cervicales válidas, tiempo activo válido total, duración de cada contracción, RMS normalizado, tiempo total de sesión.

**¿Mostrar RMS/MAV crudos?** Para el fisioterapeuta lo útil es: duración, % del tiempo con la cabeza arriba y RMS normalizado (esfuerzo relativo). Los valores crudos dependen de electrodos y ganancia y no son comparables entre días; por eso van ocultos por defecto y se activan con «Mostrar valores técnicos».

**Cómo cuantificar el progreso** (el objetivo: mantener la cabeza arriba más tiempo):
1. **% del tiempo de sesión con cabeza arriba** (tiempo activo válido / tiempo total) — métrica principal.
2. **Contracción más larga** y **duración media** — resistencia.
3. **N.º de activaciones** — sirve si crece el tiempo con pocas interrupciones, o decrece con mayor duración cada una.
4. **RMS normalizado** — esfuerzo relativo.

En **Historial** se grafica cualquiera de estas por sesión con línea de tendencia y la comparación «primeras 3 vs. últimas 3 sesiones». Precaución: la colocación de electrodos y la calibración varían entre sesiones; por eso la normalización se hace por sesión y los tiempos (1–2) son lo más confiable. Mantén sesiones de duración y posición similares y anótalas (campos «Posición» y «Músculo/electrodos»).

## 6. Datos que se guardan al registrar
Obligatorios: nombre(s), apellidos, fecha de nacimiento, GMFCS. Opcionales sugeridos: ID/expediente, sexo, diagnóstico/tipo de PC, edad gestacional (prematuridad → edad corregida), tutor/contacto, medicación/toxina botulínica/cirugías (afectan el tono), notas y consentimiento informado. Por sesión: músculo/lado de electrodos, posición del niño, fisioterapeuta y notas.

## 7. Guardado de datos
- **Historial automático** al pulsar «Finalizar terapia» (en el navegador).
- **Guardar datos en CSV** (durante la sesión): 2 archivos — `…_activaciones.csv` (una fila por activación) y `…_envolvente.csv` (envolvente a 50 Hz). Se abren en Excel (UTF-8 con BOM). Si tu Excel usa «;» como separador, usa Datos → Desde texto/CSV.
- **Historial → Exportar CSV** y **Ajustes → Respaldo JSON** (haz uno periódicamente: si se borra el caché del navegador, se pierden los datos).
- **Google Sheets (opcional):**
  1. Crea una hoja → Extensiones → Apps Script → pega `apps-script/Code.gs`.
  2. Implementar → Nueva implementación → Aplicación web → «Ejecutar como: yo», «Acceso: cualquiera» → copia la URL.
  3. Pégala en Ajustes → «URL de Google Sheets». Al finalizar cada sesión se agrega una fila en `Sesiones` y las filas en `Activaciones`.
  4. Por privacidad, por defecto se envía el expediente/ID y **no** el nombre. Son datos de menores: confirma con tu institución antes de usar la nube.
