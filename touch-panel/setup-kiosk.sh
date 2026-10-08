#!/bin/bash
set -e

echo "=== Instalando paquetes necesarios para el kiosk (Cage + Cog + wvkbd) ==="
apt-get update
apt-get install -y cage cog wvkbd || apt-get install -y cage cog

echo "=== Instalando servicios systemd del panel ==="
cp /home/guille/panel/panel.service /etc/systemd/system/
cp /home/guille/panel/panel-kiosk.service /etc/systemd/system/

systemctl daemon-reload
systemctl enable panel.service
systemctl restart panel.service

echo "=== Estado del servicio backend panel ==="
systemctl status panel.service --no-pager

echo ""
echo "=== Instalación de backend completada ==="
echo "Para arrancar el kiosk gráfico en pantalla execute:"
echo "  systemctl start panel-kiosk"
