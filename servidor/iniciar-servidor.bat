@echo off
REM ASTRA - abre el servidor de la pagina. Deja esta ventana abierta mientras uses la pagina.
cd /d "%~dp0"
where node >nul 2>nul || (echo Falta Node.js: instalalo desde https://nodejs.org y vuelve a abrir este archivo. & pause & exit /b)
start "" http://localhost:8080
node servidor.js
pause
