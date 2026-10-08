#!/bin/bash
# Para el splash y deja tty1 limpio para SDL
systemctl stop boot-splash.service 2>/dev/null || true
printf '\033[2J\033[H\033[?25l' > /dev/tty1
