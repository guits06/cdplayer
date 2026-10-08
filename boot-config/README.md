# Boot Splash Config

Archivos para la boot animation del CDPlayer en Raspberry Pi.

## Instalación

```bash
sudo cp boot-splash.sh boot-splash-clear.sh /usr/local/bin/
sudo chmod +x /usr/local/bin/boot-splash.sh /usr/local/bin/boot-splash-clear.sh
sudo cp boot-splash.service boot-splash-clear.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable boot-splash.service boot-splash-clear.service
```

Añadir a `/boot/firmware/cmdline.txt` (al final, en la misma línea):
```
quiet loglevel=3 systemd.show_status=0 vt.global_cursor_default=0 logo.nologo
```

Crear `/etc/systemd/journald.conf.d/no-console.conf`:
```ini
[Journal]
ForwardToConsole=no
MaxLevelConsole=emerg
```
