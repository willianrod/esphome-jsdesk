# JSDRIVE "P R T 5 G" bus protocol

Observed on a Q200-23 control box with a CT08-JS touch panel. Other JSDRIVE / JSM
boxes are similar, but byte details can differ (see "Variations").

## Physical layer

| Line | Direction (Q200-23)       | Level | Purpose                                       |
|------|---------------------------|-------|-----------------------------------------------|
| P    | panel → box               | 5 V logic | KEY1 / "power". **High alone moves the desk** on the Q200-23. Not needed for control |
| R    | panel → box (box **R**eceives) | 5 V UART | Button frames                                 |
| T    | box → panel (box **T**ransmits) | 5 V UART | Display frames                                |
| 5    | box → panel               | +5 V  | Panel supply                                  |
| G    | –                         | GND   | Ground                                        |

UART: **9600 baud, 8N1**, idle high.

Some boxes swap the meaning of R and T. If you see no `5A` frames, swap them.

## Panel → box (every ~16 ms)

```
A5 00 <buttons> <0xFF - buttons> FF
```

The panel sends this continuously, even when nothing is pressed (`A5 00 00 FF FF`).
The box only answers while it's being polled. Without a panel, the ESP sends the
idle frame itself to keep the height readings coming.

| Button | Code   | Confirmed on the Q200-23 |
|--------|--------|--------------------------|
| M      | `0x01` | yes. Hold ~3 s to unlock |
| M1     | `0x02` | no                       |
| M2     | `0x04` | no                       |
| M3     | `0x08` | no                       |
| M4     | `0x10` | no                       |
| Up     | `0x20` | yes                      |
| Down   | `0x40` | yes                      |

The desk moves for as long as Up or Down keeps being sent, and stops when the
frames go back to `00`.

## Box → panel (reply to each poll)

```
5A <d1> <d2> <d3> <status> <checksum>        6 bytes (Q200-23, status = 0x10)
5A <d1> <d2> <d3> <checksum>                 5 bytes (some other boxes)
```

- `checksum = (d1 + d2 + d3 [+ status]) & 0xFF`
- `d1..d3` are the panel's three 7-segment digits, with segment bits `0bPGFEDCBA`.
- Bit `0x80` on `d2` is the decimal point, so `0x06 0xE6 0x3F` reads `14.0`.

The component works out the frame length by itself from the first valid frames.

| Digit | Byte | Digit | Byte |
|-------|------|-------|------|
| 0     | `3F` | 5     | `6D` |
| 1     | `06` | 6     | `7D` |
| 2     | `5B` | 7     | `07` |
| 3     | `4F` | 8     | `7F` |
| 4     | `66` (`67` on some boxes) | 9 | `6F` |

Example: `5A 7D E6 3F 10 A0` reads **`64.0`** (7D+E6+3F+10 = 0x1A0, checksum A0).

Other displays:

| Display | Bytes      | Meaning                                                       |
|---------|------------|---------------------------------------------------------------|
| `---`   | `40 40 40` | Locked. Hold M (`0x01`) for ~3 s to unlock                    |
| `E04`   | `79 3F 66` | Appears in the idle cycle on the Q200-23; meaning unknown     |
| blank   | `00 00 00` | Display off between cycles                                    |

## Lock behaviour

After some idle time the box locks and reports `---`. While it's locked, Up and
Down are ignored. Holding M for about 3 s unlocks it, and the height comes back.
The firmware only sends M while it sees `---`, and starts the requested move once
a valid height frame arrives. It gives up after 6 s.

## Variations seen in other projects

- 5-byte box frames with no status byte.
- Status byte `0x01` instead of `0x10`.
- `0x67` instead of `0x66` for the digit 4.
- Boxes where the panel must hold P/KEY1 high while moving. That's what the
  `key_on_move` option is for. **Don't enable it on a Q200-23.**
