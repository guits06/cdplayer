#!/bin/bash
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
# Cargar variables de entorno locales si existen
if [ -f "$SCRIPT_DIR/../.env" ]; then
    source "$SCRIPT_DIR/../.env"
fi

PI_HOST="${PI_HOST:-root@raspberrypi.local}"
DEST_DIR="${PANEL_DIR:-/home/guille/panel}"

echo "=== Desplegando Panel Táctil Nativo C++ (SDL2) a Raspberry Pi ($PI_HOST) ==="

ssh $PI_HOST "mkdir -p $DEST_DIR"

scp $SCRIPT_DIR/cdpanel.cpp $SCRIPT_DIR/Makefile $SCRIPT_DIR/cdpanel.service $PI_HOST:$DEST_DIR/

echo "=== Compilando cdpanel en Raspberry Pi ==="
ssh $PI_HOST "cd $DEST_DIR && make clean && make"

echo "=== Desactivando servicios quiosco antiguos (Cage + Cog + WPE WebKit) ==="
ssh $PI_HOST "systemctl stop panel-kiosk panel || true"
ssh $PI_HOST "systemctl disable panel-kiosk panel || true"

echo "=== Instalando e iniciando servicio nativo cdpanel ==="
ssh $PI_HOST "cp $DEST_DIR/cdpanel.service /etc/systemd/system/ && systemctl daemon-reload && systemctl enable cdpanel && systemctl restart cdpanel"

echo "=== Estado del servicio cdpanel ==="
ssh $PI_HOST "systemctl status cdpanel --no-pager"

echo "=== Despliegue del panel nativo C++ completado con éxito ==="
