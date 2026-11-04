// sonido.h — DFPlayer Mini con comandos directos (como v2.3: nunca espera respuesta,
// así los motores no se detienen esperando al reproductor).
//
// En la microSD:
//   MP3/0001.mp3      canción de fondo (si hay más: 0002, 0003... y sube N_CANCIONES)
//   ADVERT/0001.mp3   "ding" corto
//   ADVERT/0002.mp3   felicitación
//   ADVERT/0003.mp3   invitación ("¡Arriba!" o su nombre)
//   ADVERT/0004.mp3   grabación de la familia
// Los "anuncios" (ADVERT) pausan la canción, suenan y la canción sigue sola.
// Trampas que esto ya evita:
//   1) El anuncio solo funciona mientras suena la canción -> anunciar() lo revisa.
//   2) Un anuncio encima de otro detiene todo -> no se lanza otro hasta que pase
//      DURACION_ANUNCIO_MS del anterior. Pon ahí la duración real de tus archivos.
#pragma once

const uint8_t VOLUMEN_INICIAL = 12;              // 0..30: canción de fondo a volumen bajo
const int     N_CANCIONES     = 1;               // canciones en MP3/ (cambia al terminar cada ronda)
enum Anuncio { DING = 1, FELICITACION = 2, INVITACION = 3, FAMILIA = 4 };
const unsigned long DURACION_ANUNCIO_MS[5] = {0, 1500, 4000, 2000, 6000};  // ms de cada ADVERT/000N
const unsigned long MARGEN_ANUNCIO_MS = 300;
const unsigned long ESPACIO_CMD_MS    = 150;     // el DFPlayer pierde comandos muy juntos
const unsigned long DF_ARRANQUE_MS    = 2000;    // tarda en leer la microSD al encender

enum EstadoMusica { MUSICA_APAGADA, MUSICA_SONANDO, MUSICA_PAUSADA };
EstadoMusica musica = MUSICA_APAGADA;
unsigned long anuncioLibreEn = 0;                // antes de esto no se lanza otro anuncio
uint8_t volumen = VOLUMEN_INICIAL;
int cancion = 1;

struct CmdDF { uint8_t cmd; uint16_t param; };
const int MAX_DF = 8;
CmdDF colaDF[MAX_DF];
int dfIni = 0, dfN = 0;
unsigned long dfUltimoEnvio = 0, dfListoEn = 0;
uint8_t dfRx[10];
int dfRxN = 0;

// Trama de 10 bytes: 7E FF 06 CMD 00 P_alto P_bajo CHK_alto CHK_bajo EF
void dfEnviar(uint8_t cmd, uint16_t param) {
  if (!USAR_SONIDO) return;
  uint8_t f[10] = {0x7E, 0xFF, 0x06, cmd, 0x00, (uint8_t)(param >> 8), (uint8_t)(param & 0xFF), 0, 0, 0xEF};
  uint16_t suma = 0;
  for (int i = 1; i < 7; i++) suma += f[i];
  uint16_t chk = (uint16_t)(0 - suma);
  f[7] = chk >> 8;
  f[8] = chk & 0xFF;
  Serial1.write(f, 10);
}

void dfEncolar(uint8_t cmd, uint16_t param) {
  if (dfN >= MAX_DF) return;
  colaDF[(dfIni + dfN) % MAX_DF] = {cmd, param};
  dfN++;
}

void sonidoIniciar() {
  Serial1.begin(9600, SERIAL_8N1, DF_RX, DF_TX);
  dfListoEn = millis() + DF_ARRANQUE_MS;
  dfEncolar(0x06, volumen);
}

void sonidoVolumen(uint8_t v) { volumen = v; dfEncolar(0x06, v); }

void musicaIniciar() {                           // MP3/000N en bucle
  dfEncolar(0x06, volumen);
  dfEncolar(0x12, cancion);
  dfEncolar(0x19, 0);                            // repetir la canción actual
  musica = MUSICA_SONANDO;
  anuncioLibreEn = millis() + 1200;              // que la canción arranque primero
}

void musicaPausar() {
  if (musica != MUSICA_SONANDO) return;
  dfEncolar(0x0E, 0);
  musica = MUSICA_PAUSADA;
}

void musicaReanudar() {
  if (musica != MUSICA_PAUSADA) return;
  dfEncolar(0x0D, 0);
  musica = MUSICA_SONANDO;
  anuncioLibreEn = millis() + 800;
}

void musicaDetener() {
  if (musica == MUSICA_APAGADA) return;
  dfEncolar(0x16, 0);
  musica = MUSICA_APAGADA;
}

void musicaSiguiente() {                         // al terminar cada ronda (si hay varias)
  if (N_CANCIONES < 2 || musica != MUSICA_SONANDO) return;
  cancion = cancion % N_CANCIONES + 1;
  dfEncolar(0x12, cancion);
  dfEncolar(0x19, 0);
  anuncioLibreEn = millis() + 1200;
}

bool anuncioLibre() {
  return USAR_SONIDO && musica == MUSICA_SONANDO && dfN == 0 &&
         (long)(millis() - anuncioLibreEn) >= 0;
}

// Lanza ADVERT/000n si se puede. Devuelve false si no sonó (no hay canción u otro suena).
bool anunciar(Anuncio n) {
  if (!anuncioLibre()) return false;
  dfEncolar(0x13, n);
  anuncioLibreEn = millis() + DURACION_ANUNCIO_MS[n] + MARGEN_ANUNCIO_MS;
  return true;
}

// Llamar en cada vuelta del loop.
void sonidoActualizar() {
  unsigned long ahora = millis();
  if (dfN && (long)(ahora - dfListoEn) >= 0 && ahora - dfUltimoEnvio >= ESPACIO_CMD_MS) {
    dfEnviar(colaDF[dfIni].cmd, colaDF[dfIni].param);
    dfIni = (dfIni + 1) % MAX_DF;
    dfN--;
    dfUltimoEnvio = ahora;
  }
  // Avisos del DFPlayer. 0x3D = terminó una pista: si era la canción, que vuelva a
  // empezar (respaldo por si el módulo no respeta el comando de repetir 0x19).
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    if (dfRxN == 0 && b != 0x7E) continue;
    dfRx[dfRxN++] = b;
    if (dfRxN < 10) continue;
    dfRxN = 0;
    if (dfRx[9] != 0xEF || dfRx[3] != 0x3D) continue;
    if (musica == MUSICA_SONANDO && (long)(ahora - anuncioLibreEn) > 500) {
      dfEncolar(0x12, cancion);
      dfEncolar(0x19, 0);
    }
  }
}
