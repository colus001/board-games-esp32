# ESP32-S3 Board Games

Initial target board: Sunton ESP32-4848S040C, 480x480 capacitive touch.

## Current Prototype

This project currently runs a local two-player chess prototype:

- Shows an 8x8 board on the 480x480 display
- Uses LVGL through `esp32-smartdisplay`
- Touch a piece to select it
- Highlights legal destination squares
- Enforces basic piece movement rules
- Alternates white/black turns

Not implemented yet:

- Check/checkmate detection
- Castling
- En passant
- Pawn promotion

## Build And Upload

Install PlatformIO, then run:

```bash
pio run
pio run --target upload
pio device monitor
```

This project vendors a corrected `esp32-4848S040CIY1` board definition because
the tested panel reports GT911 touch resolution as `1085x600`.
