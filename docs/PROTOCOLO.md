# Protocolo: Arduino → ESP32 → página

Son los tres mensajes que el equipo acordó. Con ellos cada quien puede trabajar por separado.

## 1. Arduino → ESP32 (cada 50 ms)

```
R,0.052\n
```
- `R,` + RMS de los últimos 200 ms en **mV**, sin 60 Hz ni armónicos; termina en salto de línea.
- Serial1 del UNO R4 (pin D1) → opto PC817 → IO35 del ESP32, **9600 baud**, 8N1.
- 20 líneas/s × 8 bytes ≈ 17 % del enlace.
- El ESP32 descarta líneas que no empiecen con `R,`, que no sean un número ≥ 0, o que lleguen cortadas (hueco de más de 50 ms a media línea).
- Más de **500 ms** sin líneas válidas = sin señal: todo se detiene.

## 2. ESP32 → página (WebSocket `ws://192.168.4.1/ws`)

### Dato: con cada muestra (cada 50 ms)
```json
{"rms":0.052,"alto":0.07,"bajo":0.055,"estado":"BASAL","n":6,"nivel":3,"t":42}
```
| Campo | Significado |
|---|---|
| `rms` | Último RMS del Arduino. `null` sin señal (llega cada 250 ms) |
| `alto`, `bajo` | Umbrales actuales (0 si no hay calibración de reposo) |
| `estado` | `ESPERA`, `CAL_REPOSO`, `CAL_PASIVA`, `CAL_REF`, `BASAL`, `ACTIVO`, `INVITACION`, `CELEBRA`, `SIN_SENAL`, `PARO` |
| `n` | Levantamientos contados en la sesión |
| `nivel` | Escalón de las estrellas en la ronda (0 a 5) |
| `t` | Segundos de terapia (agregado al acuerdo: el bloque 4 pide el tiempo) |

### Información: cada segundo y después de cada cambio
```json
{"tipo":"info","modo":"SESION","senal":1,"est":"OK","t":42,"n":6,"nivel":3,"rondas":1,
 "k":3.0,"cal":1,"media":0.0400,"sd":0.0100,"cv":25,"pasiva":0,"piso":0.0000,"alto":0.0700,"bajo":0.0550,
 "ref":0.2500,"refFuente":"calibracion","lpm":8.6,"amp":61,"dur":1.12,"resp":3.4,"nInv":2,"nResp":1,
 "inv":8,"bajar":1,"familia":0,"vol":12,"calRestan":0,"calTotal":0,"lps":20,"malas":0,"csv":1}
```
| Campo | Significado |
|---|---|
| `modo` | `ESPERA`, `CAL_REPOSO`, `CAL_PASIVA`, `CAL_REF`, `SESION`, `PARO` |
| `senal` | 1 = llegan datos del Arduino |
| `est` | Estrellas: `BUSCANDO_CERO`, `OK`, `SIN_TOPE`, `APAGADAS` |
| `k`, `cal`, `media`, `sd`, `cv` | Sensibilidad; 1 si hay calibración de reposo; reposo (media, SD y variación en %) |
| `pasiva`, `piso` | 1 si se hizo la pasiva; umbral mínimo que dejó |
| `ref`, `refFuente` | 100 % de la amplitud: `calibracion` o `sesion` (máximo de la sesión) |
| `lpm`, `amp`, `dur` | Levantamientos por minuto, amplitud media (% de `ref`), duración media (s) |
| `resp`, `nInv`, `nResp` | Respuesta media a la invitación (s, −1 si no hay), invitaciones, respondidas |
| `inv`, `bajar`, `familia`, `vol` | Ajustes actuales |
| `calRestan`, `calTotal` | Segundos que faltan / duración de la calibración en curso |
| `lps`, `malas` | Líneas por segundo del Arduino y líneas dañadas (diagnóstico) |
| `csv` | 1 si ya hay sesión para descargar |

### Mensaje para la terapeuta
```json
{"tipo":"msg","txt":"Reposo listo: media 0.0400, SD 0.0100. Umbral alto 0.0700, bajo 0.0550 (k = 3.0)."}
```

## 3. Página → ESP32

| Comando | Qué hace |
|---|---|
| `{"cmd":"cal_reposo"}` | Calibración de reposo, 15 s (obligatoria) |
| `{"cmd":"cal_pasiva"}` | Calibración pasiva, 10 s (opcional) |
| `{"cmd":"cal_ref"}` | Referencia, 45 s (opcional) |
| `{"cmd":"cancelar"}` | Cancela la calibración en curso |
| `{"cmd":"iniciar"}` | Empieza la terapia (pide reposo calibrado y señal) |
| `{"cmd":"terminar"}` | Termina la terapia |
| `{"cmd":"sens","k":3}` | Sensibilidad k (1.5 a 6) |
| `{"cmd":"inv","s":8}` | Segundos sin activación antes de la invitación (3 a 120) |
| `{"cmd":"bajar","on":1}` | 1/0: las estrellas bajan durante la invitación |
| `{"cmd":"familia","on":0}` | 1/0: en la cima suena la grabación de la familia en vez de la felicitación |
| `{"cmd":"vol","v":12}` | Volumen del DFPlayer (0 a 30) |
| `{"cmd":"reanudar"}` | Reanuda tras el paro (solo si el botón ya se soltó) |

El ESP32 revisa cada orden: si no corresponde (por ejemplo, iniciar sin calibrar), responde con un `msg` y no hace nada.

## 4. Resumen CSV

`GET http://192.168.4.1/resumen.csv?p=<paciente>&f=<fecha>` → archivo `astra_<paciente>_<fecha>.csv` (UTF-8, para Excel). Primero vienen los datos de la sesión y abajo una fila por activación: inicio, duración, RMS medio y pico, amplitud (% de la referencia) y respuesta a la invitación.
