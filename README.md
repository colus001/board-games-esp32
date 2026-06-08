# ESP32-S3 Board Games

Initial target board: Sunton ESP32-4848S040C, 480x480 capacitive touch.

## Current Prototype

This project currently runs a touch chess prototype:

- Shows an 8x8 board on the 480x480 display
- Uses LVGL through `esp32-smartdisplay`
- Touch a piece to select it
- Highlights legal destination squares
- Enforces piece movement, checkmate, and stalemate
- Alternates white/black turns
- Supports Lichess Board API random matchmaking after login

Not implemented yet:

- Castling
- En passant
- Full pawn promotion choice UI

## Lichess Login

The board does not ask users to type a long Lichess token. It uses a small
Cloudflare Worker pairing service:

1. Set up Wi-Fi on the board.
2. Tap `Lichess Login`.
3. Open the displayed pairing URL on a phone or computer.
4. Approve the Lichess `board:play` permission.
5. The board receives and stores the token automatically.

The ESP32 firmware needs `PAIRING_BASE_URL` in `include/secrets.h`:

```cpp
#pragma once

#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define LICHESS_TOKEN ""
#define PAIRING_BASE_URL "https://your-worker.example.com"
```

`LICHESS_TOKEN` can stay empty for normal users. It exists only as a developer
fallback.

## Cloudflare Worker Pairing Backend

The Worker lives in `worker/` and stores pairing sessions in Cloudflare KV with
short TTLs. Tokens are deleted from KV as soon as the board retrieves them.

Create a Lichess OAuth app, then configure the Worker:

```bash
cd worker
npx wrangler kv namespace create PAIRINGS
npx wrangler secret put LICHESS_CLIENT_ID
# Optional, only if your Lichess OAuth app uses a secret:
npx wrangler secret put LICHESS_CLIENT_SECRET
```

Update `worker/wrangler.jsonc`:

- Replace the KV namespace `id`
- Set `PUBLIC_BASE_URL` to the deployed Worker URL

Deploy:

```bash
npx wrangler deploy
```

Configure the Lichess OAuth redirect URI to:

```text
https://your-worker.example.com/oauth/callback
```

Required Lichess scope:

```text
board:play
```

## Build And Upload

Install PlatformIO, then run:

```bash
pio run
pio run --target upload
pio device monitor
```

This project vendors a corrected `esp32-4848S040CIY1` board definition because
the tested panel reports GT911 touch resolution as `1085x600`.
