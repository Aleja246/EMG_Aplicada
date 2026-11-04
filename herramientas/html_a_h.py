#!/usr/bin/env python3
"""Convierte esp32/astra/pagina.html en esp32/astra/pagina.h (la página que sirve el ESP32).

Uso, desde la carpeta del repositorio:
    python herramientas/html_a_h.py

Sin Python: abre pagina.h y reemplaza todo lo que hay entre la línea
R"PAGINA(  y  )PAGINA"  por el contenido de pagina.html.
"""
import pathlib
import sys

carpeta = pathlib.Path(__file__).resolve().parent.parent / 'esp32' / 'astra'
html = (carpeta / 'pagina.html').read_text(encoding='utf-8')
if ')PAGINA"' in html:
    sys.exit('Error: la página no puede contener el texto )PAGINA"')

h = ('// GENERADO desde pagina.html con herramientas/html_a_h.py: no lo edites a mano.\n'
     '#pragma once\n'
     'const char PAGINA[] PROGMEM = R"PAGINA(' + html + ')PAGINA";\n')
(carpeta / 'pagina.h').write_text(h, encoding='utf-8', newline='\n')
print('pagina.h listo:', len(html.encode('utf-8')), 'bytes')
