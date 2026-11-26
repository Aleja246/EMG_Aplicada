# ASTRA con dos canales (A0 + A1)

Esta rama es **la misma `firmware-v8.3-premio-sostenido`** (la que funciona) con el segundo canal integrado.
La calibración, la detección, el premio sostenido de 8 s, las estrellas, la luz, la música y la página
("Cabeza arriba" / "Cabeza abajo", tiempo y episodios) **no cambian**.

## Qué cambia

| Parte | Antes (1 canal) | Ahora (2 canales) |
|---|---|---|
| Arduino `arduino_emg_ap.ino` | v5.2-AP: mide A0 | **v5.3-AP-2C**: mide A0 y A1 con la misma cadena (centro, notch de 60…420 Hz, RMS de 100 ms) |
| Lo que va al atril por el opto | SinArm de A0 (`"640\n"`) | **Igual: solo A0** (`OPTO_CANALES = 1`, por defecto), para que la detección sea la de premio-sostenido. Con `OPTO_CANALES = 2` va el promedio de A0 y A1 |
| Atril `esp32_atril_ap.ino` | v8.3 | **v8.3-2C**: idéntico, solo reenvía también la señal cruda de A1 a la página |
| Página | 1 gráfica | **2 gráficas**: Canal 1 · A0 y Canal 2 · A1, cada una con su señal y su línea de activación |
| Serial Plotter | Bruto, Sin60, SinArm | lo mismo + Bruto_A1, Sin60_A1, SinArm_A1 y AlAtril (lo que recibe el atril) |

Hay que subir **los dos** sketches y volver a cargar `data/` en el LittleFS del atril (la página cambió).

## Opción: el promedio (`OPTO_CANALES = 2`)

Por defecto el atril recibe solo A0. El promedio solo conviene cuando los dos canales tienen la misma calidad de señal.

- Levantar y sostener la cabeza es un movimiento **simétrico**: los extensores del cuello de los **dos lados**
  se activan a la vez. Lo recomendado es registrar el **mismo músculo en ambos lados** (canales homólogos).
- Promediar dos canales homólogos mantiene la actividad que comparten (el esfuerzo de levantar la cabeza) y
  reduce lo que no comparten: ruido de un electrodo, de un cable o un movimiento de un solo lado
  (en teoría el ruido no correlacionado baja ~√2).
- Girar o inclinar la cabeza hacia un lado activa sobre todo **un** lado: con el promedio dispara menos
  que con un solo canal (o que con el máximo de los dos). Así hay menos premios falsos.
- El atril calibra con el reposo y umbrales **relativos**, así que funciona igual con el promedio.

Referencias generales: Hermens HJ et al., *Development of recommendations for SEMG sensors and sensor placement
procedures*, J Electromyogr Kinesiol 2000 (SENIAM); Merletti R, Parker P, *Electromyography: Physiology,
Engineering, and Noninvasive Applications*, 2004.

### Colocación recomendada

- **A0 = extensores cervicales derechos, A1 = izquierdos** (los dos iguales): electrodos sobre los paraespinales
  del cuello, a la altura de C4, unos 2 cm a cada lado de la columna, paralelos a ella (2 cm entre electrodos).
- **Un solo electrodo de referencia** para las dos placas Nova, en una zona sin músculo (apófisis de C7 o la clavícula).
  Las dos placas deben compartir referencia y alimentación; si cada una tiene su propia referencia aparece ruido.
- Cables cortos, juntos/trenzados y lejos del cargador.

## Protecciones

- **Lectura del ADC**: al cambiar de canal se hace una lectura de descarte. Sin ella, el condensador del ADC
  se queda con el voltaje del otro canal y un canal "se copia" en el otro y mete ruido (es lo que se vio en
  la rama `firmware-v8.3-dos-canales`, que leía A1 justo después de A0 sin descartar).
- **Canal 2 malo**: si A1 está saturado o plano (electrodo suelto, cable desconectado) 1 s, el atril recibe
  **solo A0** hasta que A1 esté bien 3 s seguidos. La página lo dice junto a la gráfica del canal 2 y en
  Ajustes → Diagnóstico ("Lo que recibe el atril"). Si esto pasa a media sesión, conviene **Recalibrar**.

## Usar el promedio

En `firmware/arduino_emg_ap/arduino_emg_ap.ino` cambia `OPTO_CANALES = 1` por `OPTO_CANALES = 2`. Con `1` (por defecto)
el atril recibe solo A0, exactamente como en v5.2-AP (la página sigue mostrando las dos señales).

## Música (microSD)

Carpeta `/01`: `001.mp3`–`006.mp3` dinámicas (esfuerzo) y `007.mp3`–`012.mp3` reposo, en orden aleatorio sin repetir.
El manejo del DFPlayer es el mismo de v8.3 premio sostenido; deben estar las 12 canciones.

## Para revisar en el Serial Plotter

En reposo `SinArm` y `SinArm_A1` deben ser parecidos y bajos; al levantar la cabeza **los dos** deben subir.
Si uno no se mueve, revisa sus electrodos; si uno tiene mucho más ruido en reposo, revisa la referencia y sus cables.
