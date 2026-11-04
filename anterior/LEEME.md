# Versión anterior (ya no se usa)

Aquí quedó la primera versión: el Arduino mandaba la señal cruda a 2 kHz por USB
(`emg_stream/`) y la página (`index.html`, `css/`, `js/`) hacía todo el procesamiento
en el navegador con Web Serial. `GUIA.md` la explica.

El sistema actual sigue la regla de **un solo cerebro** (ver el `README.md` principal):
el Arduino solo mide (`arduino/emg_rms`), el ESP32 decide todo (`esp32/astra`) y la
página solo muestra datos y manda botones.

`DIGITALIZARR.ino` es el código base original (notch de 60 Hz + RMS); `emg_rms.ino` parte de él.
