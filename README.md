# Bulletin v2.1

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


## v2.1 features

- **Search News**: type a query, get merged results from HN (Algolia) and
  Reddit - every headline shows source, author, and date.
- **Full article** (OK on a headline): the story is converted to plain
  text by the r.jina.ai reader proxy and shown in a scrollable pager
  (Up/Down to scroll, Back to return).
- **Photos** (OK inside an article): the first in-article image is
  resized server-side by wsrv.nl to 112x52 JPEG (~2KB), decoded on-device
  with tjpgd, and Bayer-dithered to 1-bit for the Flipper screen.

Notes: article/photo features depend on the free r.jina.ai and wsrv.nl
proxies (rate-limited but keyless). Reddit may occasionally refuse
non-browser clients; HN results still come through - failures per source
are skipped, not fatal.


## Why search/articles go through r.jina.ai (v2.2)

The FlipperHTTP ESP32 firmware injects a `{}` body into every GET
request. Strict CDNs and APIs (Fastly/Reddit, Algolia, jina's own GET
endpoint) reject a GET with a body as **HTTP 400**; Google's Firebase
tolerates it, which is why the Daily Edition always worked. Instead of
reflashing the board, search and article fetches are sent as **POST**
to `https://r.jina.ai/` with `{"url": <target>}` - POST carries a real
payload, so nothing is injected, and jina performs a clean GET to the
target server on our behalf.

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
