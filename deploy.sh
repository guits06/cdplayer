#!/bin/bash
set -e

# Cargar variables de entorno locales si existen
if [ -f "$(dirname "$0")/.env" ]; then
    source "$(dirname "$0")/.env"
fi

PI_HOST="${PI_HOST:-root@raspberrypi.local}"
DEST_DIR="${DEST_DIR:-/home/guille/cdplayer}"
PANEL_DIR="${PANEL_DIR:-/home/guille/panel}"

echo "=== Desplegando reproductor CD a Raspberry Pi ($PI_HOST) ==="

scp -r modules cdda_engine.c cdda_engine.h Makefile main.py rpi_audio_receiver.py app.js index.html style.css visualizer.html manifest-visualizer.json sw.js sw-visualizer.js $PI_HOST:$DEST_DIR/

echo "=== Compilando motor C en Raspberry Pi ==="
ssh $PI_HOST "cd $DEST_DIR && make clean && make"

echo "=== Compilando e instalando mods/módulos en Raspberry Pi ==="
ssh $PI_HOST "if [ -d $DEST_DIR/modules/cisco_voip ]; then cd $DEST_DIR/modules/cisco_voip && make && make install; fi"
ssh $PI_HOST "if [ -d $DEST_DIR/modules/study_mode ]; then cd $DEST_DIR/modules/study_mode && make clean && make; fi"

echo "=== Reiniciando servicio cdplayer ==="
ssh $PI_HOST "systemctl restart cdplayer"

echo "=== Verificando estado del servicio cdplayer ==="
ssh $PI_HOST "systemctl status cdplayer --no-pager"

echo "=== Actualizando cdpanel en Raspberry Pi ==="
scp touch-panel/cdpanel.cpp touch-panel/panel_plugin.h touch-panel/Makefile $PI_HOST:$PANEL_DIR/
ssh $PI_HOST "cd $PANEL_DIR && make clean && make && systemctl restart cdpanel"

echo "=== Despliegue completado con éxito ==="
