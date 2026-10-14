# esphome-jsdesk

Make a JSDRIVE / JSM sit-stand desk smart with an ESP8266 and ESPHome.

The ESP plugs into the desk's control box in place of the touch panel. It reads the
height from the box and moves the desk up, down, or to an exact height. Home
Assistant picks it up as a cover, a target-height number, and a set of buttons.

Tested on:

| Part          | Model                                             |
|---------------|---------------------------------------------------|
| Control box   | Q200-23 (JSM / JSDRIVE), RJ45 port labelled `P R T 5 G` |
| Touch panel   | CT08-JS (HZZY-D board)                            |
| ESP board     | NodeMCU v2 (ESP8266, 4 MB)                        |
| ESPHome       | 2026.7.1                                          |

Other JSDRIVE boxes and panels that use the same `P R T 5 G` bus are likely to work.
Check the [protocol notes](docs/protocol.md) before you connect anything.

> **Warning:** on the Q200-23, driving the **P** line high makes the desk move by
> itself. The firmware keeps P low at all times. Add the 10 kΩ pull-down described
> below so P can't float while the ESP boots. Keep the desk clear while testing.

## What you get

| Entity         | Type          | Notes                                                    |
|----------------|---------------|----------------------------------------------------------|
| Height         | sensor (cm)   | Decoded from the box's display output                    |
| Desk           | cover         | Open = max height, close = min height, position = %      |
| Target height  | number        | Type a height in cm and the desk moves there and stops   |
| Up / Down / Stop | buttons     | Up and Down run until Stop, the limit, or 60 s           |
| Preset 1–4     | buttons       | The box's memory positions (untested on the Q200-23)     |
| Moving         | binary sensor |                                                          |
| Display        | text sensor   | Diagnostic: what the panel would show (`---`, `E04`, …)  |

The firmware also exposes two API actions, `press` (hold a raw button code
for a set time) and `stop`. In Home Assistant they appear as `esphome.desk_press` and
`esphome.desk_stop`. They're useful for exploring the protocol.

Safety:
- Any move stops after 60 s, or once the height hasn't changed for 2 s.
- Moves are refused while the box isn't reporting a height.
- Target heights are clamped to `min_height` and `max_height`.

## Wiring (standalone mode)

Unplug the touch panel and plug a straight RJ45 patch cable into the box instead.
These colours are for a **T568B** cable, read at the far end:

| Box label | RJ45 pin | T568B colour  | NodeMCU             | Notes                                          |
|-----------|----------|---------------|---------------------|------------------------------------------------|
| G         | 5        | white/blue    | GND                 | Common ground                                  |
| R         | 2        | orange        | D4 (GPIO2)          | **Box input**, ESP → box (UART1 TX)            |
| T         | 1        | white/orange  | D1 (GPIO5)          | **Box output**, box → ESP (RX)                 |
| P         | 4        | blue          | D5 (GPIO14), held low | KEY1. Driving it high moves the desk. Never leave it floating |
| 5         | 8        | brown         | VIN                 | +5 V from the box. Powers the ESP (tested), or leave it unconnected and use USB |
| –         | 3, 6, 7  | white/green, green, white/brown | –  | Unused                                         |

On this box, R and T are named from the **box's** side: R is what the box receives
and T is what it transmits. The other way round from what you might expect.

The pin numbers come from one Q200-23. Before connecting, check yours with a
multimeter: G is ground and 5 is +5 V.

Recommended parts (the prototype ran without them, but add them):
- **10 kΩ from P to GND.** It keeps the desk still while the ESP is booting or
  unplugged.
- **330 Ω in series on R and on P.** It protects both sides.
- **A 1 kΩ / 2.2 kΩ divider on T.** The box drives T at 5 V and the ESP8266 is a
  3.3 V part.
- **Power:** the box's 5 V line runs the NodeMCU through VIN with no problems,
  so no extra power supply is needed. Plugging in USB at the same time to flash
  is fine, because the NodeMCU v2 has a diode between USB and VIN.

```
 Box RJ45                          NodeMCU
 T (1) ──[1k]────●──────────────── D1 / GPIO5
                 └──[2k2]── GND
 R (2) ──[330]──────────────────── D4 / GPIO2
 P (4) ──────────●──[330]───────── D5 / GPIO14
                 └──[10k]── GND
 G (5) ─────────────────────────── GND
 5 (8) ─────────────────────────── VIN
```

## Install

1. Copy the repo into your ESPHome config directory:
   - `desk.yaml`
   - `desk/`
   - `components/`
   - `secrets.yaml.example`
2. Copy `secrets.yaml.example` to `secrets.yaml`, or add its keys to your existing
   `secrets.yaml`:
   - your Wi-Fi details
   - a new API encryption key (the command to generate one is in the file)
   - a fallback hotspot password
   - the IP the dashboard should use
3. Set `min_height` and `max_height` in `desk/standalone.yaml` to your desk's range.
   The default is 62–120 cm. Also change `min_value` and `max_value` of "Target
   height" in `desk/entities.yaml`.
4. Flash once over USB with `esphome run desk.yaml`. After that it updates over the air.
5. Adopt the device in Home Assistant through the ESPHome integration.

### Modes

`desk.yaml` includes one mode file. Swap the include to change mode.

| File                   | Panel     | ESP role                                                                 | Status      |
|------------------------|-----------|--------------------------------------------------------------------------|-------------|
| `desk/standalone.yaml` | unplugged | Takes the panel's place: polls the box every 16 ms and sends commands    | **In use, tested** |
| `desk/control.yaml`    | connected | Sits between panel and box on R and P and forwards the panel's frames. Any button on the panel takes control back | Untested    |
| `desk/sniff.yaml`      | connected | Listens to both directions only and drives nothing; for reverse-engineering | Used early on |

`control.yaml` and `sniff.yaml` need an RJ45 splitter or breakout so the panel and
the ESP can share the bus. The wiring for each mode is in the comments at the top
of its file. `entities.yaml` is for `standalone` and `control`; leave it out with
`sniff`.

## How it behaves

- **Moving to a height:** the ESP holds Up or Down until the height is within
  `stop_tolerance` (0.7 cm) of the target, then releases. The desk coasts about
  0.6 cm further and lands on the target.
- **Locking:** after some idle time the box locks and the display shows `---`. A
  real panel needs M held for about 3 s to unlock. The firmware holds M (code
  `0x01`) only while the display shows `---`, then starts the move as soon as a
  height appears:
  - from locked: about 3 s unlock, then the move
  - already unlocked: the command goes out at once
- **Start-up delay:** after a command the desk takes about 1–1.5 s before the
  height starts changing. That's the box's motor soft-start, not the ESP.

## Documentation

- [docs/protocol.md](docs/protocol.md): the bus protocol (frames, buttons, 7-segment
  decoding, lock state).

## Open questions

- Presets M1–M4 (`0x02`/`0x04`/`0x08`/`0x10`) are wired up but untested.
- `E04` shows up in the box's idle display cycle. It seems harmless, but the cause
  is unknown.
- The box's auto-lock might be configurable from the panel's hidden settings menu.
  Turning it off would remove the 3 s unlock.

## Credits

The protocol work builds on earlier community reverse-engineering of JSDRIVE / JSM
and Desktronic controllers, by kortschak, ssieb, veli and fonix232 among others.

## License

[MIT](LICENSE)
