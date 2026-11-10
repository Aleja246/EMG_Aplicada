#!/bin/sh
# ASTRA - abre el servidor de la pagina (Mac / Linux). Deja esta ventana abierta.
cd "$(dirname "$0")"
(sleep 1; open http://localhost:8080 2>/dev/null || xdg-open http://localhost:8080 2>/dev/null) &
node servidor.js
