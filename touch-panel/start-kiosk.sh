#!/bin/bash
export XDG_RUNTIME_DIR=/run/user/0
export XCURSOR_THEME=transparent
export XCURSOR_SIZE=24
export WLR_NO_HARDWARE_CURSORS=1
export GTK_CURSOR_THEME_NAME=transparent
export GTK_CURSOR_THEME_SIZE=24
export COG_HIDE_CURSOR=1
export WPE_HIDE_CURSOR=1
export WPE_WL_NO_CURSOR=1

mkdir -p /run/user/0

exec /usr/bin/cage -d -- /usr/bin/cog http://localhost:8080
