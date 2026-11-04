# Bloque 5 · Pruebas, video y seguridad

## Protocolo (cada integrante, mínimo 3)

Antes de empezar: limpien la piel y coloquen los electrodos igual que se pondrán en los niños (anoten la colocación). Abran el monitor serie del ESP32 (115200) y la página en http://192.168.4.1.

| # | Prueba | Cómo | Se anota |
|---|---|---|---|
| P1 | Calibración de reposo | Cabeza apoyada en la mano de un compañero, 15 s (**Reposo · 15 s**) | media, SD, variación (%), umbral alto y bajo |
| P2 | Detección | 10 levantamientos: subir la cabeza, sostener ~1 s y relajar; uno cada ~5 s. Otro integrante los cuenta a mano | levantamientos hechos vs. contados por el ESP32 |
| P3 | Falsos en reposo | 1 min con la cabeza apoyada | conteos falsos (meta: 0) |
| P4 | Falsos por movimiento | 30 s: un compañero mueve la cabeza (si se puede, con el ATLAS caminando). Repetir después de **Pasiva · 10 s** | falsos antes / después de la pasiva |
| P5 | Latencia | Video a 60 cuadros/s (o cámara lenta) con la cabeza y la tira de luz en cuadro. Cuadros entre el inicio del movimiento y el cambio de luz ÷ 60 | latencia de luz y de sonido (meta: < 0.5 s) |
| P6 | Ronda | 5 levantamientos: las estrellas suben 5 escalones, celebran y bajan a cero | ¿completa la ronda? |
| P7 | Invitación | Quedarse quieto 8 s: azul que respira, "¡Arriba!", las estrellas bajan; luego levantar | tiempo de respuesta que muestra la página |
| P8 | Seguridad | Desconectar el cable del opto (y presionar el PARO si está instalado) | ¿todo se detiene en < 0.5 s? ¿reanuda? |
| P9 | Página | Dejarla abierta 5 min durante P2-P7 | ¿se trabó? ¿el contador coincide con el monitor serie? |

Al final, **Terminar** descarga el CSV de cada integrante: guárdenlo como evidencia.

### Tabla de resultados

| Integrante | Reposo (media ± SD) | k | Umbral alto / bajo | Hechos | Contados | Falsos reposo 1 min | Falsos movimiento (antes / después pasiva) | Latencia luz (s) | Latencia sonido (s) | Amplitud media (%) | Duración media (s) | Notas |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | | | | 10 | | | / | | | | | |
| 2 | | | | 10 | | | / | | | | | |
| 3 | | | | 10 | | | / | | | | | |
| **Total** | | | | **30** | | | | | | | | |

**Sensibilidad** = contados ÷ hechos. **Falsos por minuto** = falsos ÷ minutos de reposo. Si hay falsos, anoten con qué k desaparecen.

## Video (la señal real dispara la salida, sin "manual bypass")

1. Una toma de toda la cadena: electrodos → Arduino → opto → ESP32 → móvil y página.
2. La página abierta desde **http://192.168.4.1** con "Arduino: 20 líneas/s" (sin la franja de MODO SIMULADO).
3. Calibración de reposo: aparecen los dos umbrales en la gráfica.
4. Levantamientos: cada uno cruza el umbral alto, cambia la luz, suena el ding y sube el contador.
5. Cinco levantamientos: las estrellas llegan arriba, celebración, bajan a cero.
6. Quieto 8 s: invitación (azul que respira y "¡Arriba!").
7. Seguridad: se desconecta el opto (o se presiona el PARO) y todo se detiene.
8. **Terminar** y el CSV abierto en Excel.

Díganlo en el video: la página no tiene ningún botón para encender la luz, el sonido o los motores; solo la señal EMG los dispara.

## Explicación de seguridad

- **Aislamiento eléctrico**: la persona solo está conectada al lado del Arduino. El opto PC817 separa ese lado del ESP32, los motores y su fuente. Durante las pruebas, el Arduino va con batería (power bank); no lo conecten a una computadora enchufada a la pared mientras tenga electrodos puestos (si necesitan el monitor serie, que la laptop esté con batería).
- **Luz sin destellos (epilepsia, frecuente en niños con PC)**: máximo un cambio de color por segundo con transición suave de ~0.4 s, la invitación "respira" a un ciclo cada 4 s y el brillo no pasa de 50 %. La guía WCAG 2.3.1 limita los destellos a 3 por segundo; aquí no hay destellos.
- **Sonido**: canción a volumen bajo y efectos cortos. Nunca se lanza un anuncio encima de otro (eso detiene el DFPlayer).
- **Motores**: 28BYJ-48 con reductor, ~10 rpm y poca fuerza. Las estrellas son ligeras y cuelgan fuera del alcance. El microswitch marca el tope; si no se encuentra en 1.3 recorridos, o si sigue presionado lejos del tope, las estrellas se desactivan solas. Las bobinas se apagan cuando no se mueven (no se calientan).
- **Paro de emergencia** (IO34, `USAR_PARO = true` cuando esté conectado): detiene todo al instante (bobinas apagadas, luz apagada, música en pausa). Para seguir hay que soltarlo **y** pulsar Reanudar (o BOOT); las estrellas vuelven a buscar el tope antes de moverse.
- **Sin señal**: si pasan más de 500 ms sin datos del Arduino, todo se detiene; nunca se actúa con datos viejos.
- **Un solo cerebro**: la página no mueve nada directamente; manda órdenes que el ESP32 revisa. Si la página se cierra, la terapia sigue de forma segura.
- **Datos**: el CSV queda en la tableta; el nombre solo se usa para el archivo. No se suben datos de pacientes a internet ni a GitHub.
- **Limitaciones** (preséntenlas): la duración es el tiempo sobre el umbral, no el tiempo con la cabeza arriba; la amplitud se normaliza a una tarea de referencia (no a una contracción máxima), y cambia si cambia la colocación de los electrodos. El sistema no sustituye a la terapeuta: siempre hay supervisión.
