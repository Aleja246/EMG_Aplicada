# Cómo se generó el firmware del atril

El atril v8 (`firmware/esp32_atril_ap/esp32_atril_ap.ino`) **no se escribió a mano**: se genera a partir de tu
firmware original para que la lógica del atril quede exactamente igual.

| Archivo | Qué es |
|---|---|
| `atril_original_v5_sonido.ino` | Tu firmware del atril (v5 con sonido), sin cambios |
| `v7transformar.py` | Adaptaciones del v7.1: mensajes también a la página, bloques, activaciones, música y volumen desde la página |
| `v8transformar.py` | Reutiliza esas adaptaciones y cambia solo la parte de red: red ASTRA, página, WebSocket y señal del Arduino |

Cada reemplazo comprueba que el texto original exista exactamente una vez; si tu código cambiara, el script se
detiene en lugar de generar algo distinto.

Para volver a generarlo (desde la raíz del repositorio):

```bash
python3 herramientas/firmware/v8transformar.py herramientas/firmware/atril_original_v5_sonido.ino firmware/esp32_atril_ap/esp32_atril_ap.ino
```

Si modificas tu firmware original (por ejemplo, otra velocidad), cambia `atril_original_v5_sonido.ino` y vuelve a
correr el comando. Así tu cambio pasa al v8 sin tocar la parte de red.
