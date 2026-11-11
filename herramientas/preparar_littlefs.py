#!/usr/bin/env python3
"""Prepara la carpeta data/ del atril (LittleFS) con la pagina de ASTRA.

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
    else:
        z = gzip.compress(datos, compresslevel=9, mtime=0)
        open(destino + '.gz', 'wb').write(z)
        n = len(z)
    total += n
    print('%-24s %7d -> %6d bytes' % (a, len(datos), n))
print('Total en LittleFS: %.1f KB (%d archivos) en %s' % (total / 1024, len(archivos), os.path.relpath(DATA, RAIZ)))
if total > 150 * 1024:
    print('AVISO: la pagina pesa mas de 150 KB: el ESP32 leera de la flash lo que no quepa en su RAM.')
