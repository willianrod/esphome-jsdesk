# USB serial passthrough into an unprivileged Proxmox LXC

This applies if the ESPHome dashboard runs in an **unprivileged** LXC container and
you want to flash boards over USB from it. Plugging and unplugging the board while
the container runs works as expected.

`lxc-device add` and plain device bind-mounts don't work well for unprivileged
containers with hot-plugged devices. This setup instead:

1. allows the USB-serial device majors (188 = ttyUSB, 166 = ttyACM) in the
   container's cgroup
2. bind-mounts a host directory, `/dev/esphome-usb`, into the container
3. on plug-in, a udev rule starts a systemd unit. The unit creates the device node
   in that directory, owned by the container's root, and symlinks it to
   `/dev/ttyUSBn` inside the container
4. on unplug, the unit stops and removes both
5. a Proxmox hookscript re-creates the links whenever the container starts

The examples use CT ID **109**. Change it in `esphome-usb`, the service
description and the config below.

## Files (in [`../proxmox/`](../proxmox/))

| File                       | Install to                                  |
|----------------------------|---------------------------------------------|
| `99-esphome-usb.rules`     | `/etc/udev/rules.d/`                        |
| `esphome-usb@.service`     | `/etc/systemd/system/`                      |
| `esphome-usb`              | `/usr/local/bin/` (chmod +x)                |
| `esphome-usb.conf`         | `/etc/tmpfiles.d/`                          |
| `esphome-usb-hook.sh`      | `/var/lib/vz/snippets/` (chmod +x)          |

In `99-esphome-usb.rules`, set the serial of any USB-serial device that must
**not** go to this container, such as a Zigbee coordinator used by another CT.
If there isn't one, delete the `ENV{ID_SERIAL_SHORT}!=...` condition. To find a
device's serial:

```sh
udevadm info /dev/ttyUSB0 | grep ID_SERIAL_SHORT
```

`100000:100020` is the host UID and GID of root and of the `dialout` group (GID 20)
inside a default unprivileged container. Adjust them if your ID mapping is
different.

## Container config

Add to `/etc/pve/lxc/<CTID>.conf`:

```
lxc.cgroup2.devices.allow: c 188:* rwm
lxc.cgroup2.devices.allow: c 166:* rwm
lxc.mount.entry: /dev/esphome-usb dev/esphome-usb none bind,optional,create=dir
hookscript: local:snippets/esphome-usb-hook.sh
```

## Enable

```sh
systemd-tmpfiles --create /etc/tmpfiles.d/esphome-usb.conf
systemctl daemon-reload
udevadm control --reload
udevadm trigger --subsystem-match=tty --action=add
pct reboot <CTID>
```

Check it from inside the container with `ls -l /dev/ttyUSB*`, then flash with
`esphome run desk.yaml --device /dev/ttyUSB0`.

If no `/dev/ttyUSB*` appears on the host at all, check the cable. Charge-only
micro-USB cables are a common cause.
