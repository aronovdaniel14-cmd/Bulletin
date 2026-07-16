# Bulletin

Minimalist daily news ticker for Flipper Zero + WiFi Dev Board running
[FlipperHTTP](https://github.com/jblanked/FlipperHTTP) firmware.

Fetches the Hacker News front page (Algolia API, no key needed) over the
ESP32 and renders it as a clean paginated edition on the Flipper screen —
one headline per page, left/right to flip through.

## Requirements

- Flipper Zero (tested against Momentum mntm-012, API 87.1 — the prebuilt
  `dist/bulletin.fap` targets that; rebuild with `ufbt` for OFW/Unleashed)
- WiFi Dev Board (ESP32-S2) flashed with FlipperHTTP firmware
- Board attached to the Flipper GPIO header (standard USART pins 13/14)

## Install

Copy `dist/bulletin.fap` to `SD Card/apps/GPIO/` (qFlipper or SD card),
or from the source folder just run:

```
ufbt launch
```

## Usage

1. **Set WiFi SSID** and **Set WiFi Password** — saved locally to
   `apps_data/bulletin/wifi.cfg` so you only type them once.
2. **Send WiFi to Board** — pushes credentials to the ESP32 (it stores
   them and auto-connects on boot from then on).
3. **Read News** — fetches the latest edition and opens the reader.
   Left/Right = previous/next headline, Back = menu.

If you already configured WiFi via another FlipperHTTP app (FlipWiFi,
FlipStore, etc.), skip straight to Read News.

## Changing the feed

Edit `BULLETIN_FEED_URL` in `bulletin.c`. Any HTTPS endpoint returning
JSON with `"title":"..."` fields works. Examples:

- HN search by topic:
  `https://hn.algolia.com/api/v1/search?query=ai&tags=story&hitsPerPage=15&attributesToRetrieve=title&attributesToHighlight=none`
- Your own backend that pre-summarizes headlines (pairs nicely with an
  edition-based news backend serving other clients too).

Then rebuild: `ufbt`.

## How it works

- The response is streamed straight to SD
  (`apps_data/bulletin/latest.json`) via FlipperHTTP's save-to-file mode,
  so large responses never hit the 2KB UART line buffer.
- A lightweight scanner extracts `"title"` values (handles JSON escapes,
  strips non-ASCII) — no full JSON parser needed on-device.
- Fetching runs in a worker thread with a loading spinner; the UI never
  blocks.
- Last edition stays cached on SD after fetch.
