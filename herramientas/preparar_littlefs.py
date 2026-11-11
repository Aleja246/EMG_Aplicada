#!/usr/bin/env python3
"""Prepara la pagina de ASTRA para el atril:
  - firmware/esp32_atril_ap/pagina_web.h : la pagina DENTRO del firmware (v8.3). Con este archivo
    junto al .ino no hace falta subir LittleFS y la pagina siempre es la de esa version.
  - firmware/esp32_atril_ap/data/        : la misma pagina para LittleFS (se usa solo si falta
    pagina_web.h), con data/version.txt.

Copia index.html, css/, js/ y fonts/ de la raiz del repositorio a
firmware/esp32_atril_ap/data/, comprimidos con gzip (el ESP32 los manda tal
cual con "Content-Encoding: gzip": pesan ~4 veces menos y caben en su RAM).
Revisa que la pagina no use nada de internet (CDN, fuentes externas).

Uso (desde la raiz del repositorio):  python3 herramientas/preparar_littlefs.py
La carpeta data/ ya viene preparada en el repositorio: solo hay que volver a
correr esto si cambias la pagina (index.html, css o js).
"""
import gzip
import os
import re
import shutil
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(RAIZ, 'firmware', 'esp32_atril_ap', 'data')

html = open(os.path.join(RAIZ, 'index.html'), encoding='utf-8').read()
refs = re.findall(r'(?:src|href)="([^"]+)"', html)
externos = [r for r in refs if re.match(r'^(https?:)?//', r)]
if externos:
    sys.exit('La pagina usa recursos de internet (no funcionarian sin internet): %s' % externos)
locales = [r for r in refs if not r.startswith(('data:', '#', 'mailto:'))]
css = open(os.path.join(RAIZ, 'css', 'style.css'), encoding='utf-8').read()
for u in re.findall(r'url\(([^)]+)\)', css):
    u = u.strip('\'"')
    if re.match(r'^(https?:)?//', u):
        sys.exit('El CSS usa un recurso de internet: %s' % u)
    if not u.startswith('data:'):
        locales.append(os.path.normpath(os.path.join('css', u)).replace(os.sep, '/'))
archivos = ['index.html'] + sorted(set(locales))

m = re.search(r"VERSION_PAGINA\s*=\s*'([^']+)'", open(os.path.join(RAIZ, 'js', 'version.js'), encoding='utf-8').read())
if not m:
    sys.exit('No encuentro EMG.VERSION_PAGINA en js/version.js')
VERSION = m.group(1)
contenido = []                                   # (ruta servida, bytes, gz)

if os.path.isdir(DATA):
    shutil.rmtree(DATA)
total = 0
for a in archivos:
    origen = os.path.join(RAIZ, a)
    if not os.path.isfile(origen):
        sys.exit('No existe %s (lo pide la pagina)' % a)
    destino = os.path.join(DATA, a)
    os.makedirs(os.path.dirname(destino), exist_ok=True)
    datos = open(origen, 'rb').read()
    if a.endswith('.woff2'):                   # ya viene comprimida
        open(destino, 'wb').write(datos)
        n = len(datos)
        contenido.append(('/' + a, datos, False))
    else:
        z = gzip.compress(datos, compresslevel=9, mtime=0)
        open(destino + '.gz', 'wb').write(z)
        n = len(z)
        contenido.append(('/' + a, z, True))
    total += n
    print('%-24s %7d -> %6d bytes' % (a, len(datos), n))
open(os.path.join(DATA, 'version.txt'), 'w').write(VERSION + '\n')
print('Total en LittleFS: %.1f KB (%d archivos) en %s, version %s' % (total / 1024, len(archivos), os.path.relpath(DATA, RAIZ), VERSION))

# La misma pagina dentro del firmware (constantes en la flash del programa)
H = os.path.join(os.path.dirname(DATA), 'pagina_web.h')
with open(H, 'w') as f:
    f.write('// ASTRA: la pagina web DENTRO del firmware del atril. Generado por herramientas/preparar_littlefs.py\n')
    f.write('// a partir de index.html, css/, js/ y fonts/. No lo edites a mano: cambia la pagina y vuelve a generarlo.\n')
    f.write('// Tiene que estar en la MISMA carpeta que esp32_atril_ap.ino.\n#pragma once\n#include <stdint.h>\n\n')
    f.write('const char PAGINA_VERSION[] = "%s";\n\n' % VERSION)
    for i, (ruta, b, gz) in enumerate(contenido):
        f.write('static const uint8_t PW_%d[%d] = {  // %s%s\n' % (i, len(b), ruta, ' (gzip)' if gz else ''))
        for k in range(0, len(b), 24):
            f.write('  ' + ','.join('0x%02x' % x for x in b[k:k + 24]) + ',\n')
        f.write('};\n')
    f.write('\nstruct PaginaInterna { const char *ruta; const uint8_t *datos; uint32_t largo; bool gz; };\n')
    f.write('static const PaginaInterna PAGINA_INTERNA[] = {\n')
    for i, (ruta, b, gz) in enumerate(contenido):
        f.write('  {"%s", PW_%d, %d, %s},\n' % (ruta, i, len(b), 'true' if gz else 'false'))
    f.write('};\nstatic const int N_PAGINA_INTERNA = %d;\n' % len(contenido))
print('Pagina dentro del firmware: %s (%.0f KB de texto, %.1f KB en la flash)' % (os.path.relpath(H, RAIZ), os.path.getsize(H) / 1024, total / 1024))
if total > 150 * 1024:
    print('AVISO: la pagina pesa mas de 150 KB: el ESP32 leera de la flash lo que no quepa en su RAM.')
