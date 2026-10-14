#!/bin/sh
# Re-attach already-plugged USB serial devices after CT 109 starts.
[ "$2" = "post-start" ] && systemctl restart 'esphome-usb@*.service' --all 2>/dev/null
exit 0
