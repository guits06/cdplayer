#!/bin/bash
exec > /dev/tty1 2>&1 < /dev/tty1
export TERM=linux
printf '\033[?25l'
printf '\033[2J\033[H'

BOLD='\033[1m'
DIM='\033[2m'
WHITE='\033[97m'
MAGENTA='\033[95m'
RESET='\033[0m'

for i in $(seq 1 12); do printf '\n'; done
printf '%46s'"${BOLD}${WHITE}CDPLAYER${RESET}\n"
printf '%48s'"${DIM}by guille${RESET}\n"
printf '%54s'"${MAGENTA}. . .${RESET}\n"

sleep infinity
