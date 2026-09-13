# Smart Remote+ for CYD

*Smart Remote+* as a standalone Wi-Fi remote for LG webOS TVs, running on the £10 **ESP32-2432S028**
("Cheap Yellow Display"): a 2.8" touchscreen with an ESP32 behind it. No hub, no
phone, no app on the TV. Pair once, then tap.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Support on Ko-fi](https://img.shields.io/badge/Support-Ko--fi-ff5e5b.svg)](https://ko-fi.com/garybbgames)

![Remote page](docs/remote.png)

It is the hardware version of the app: the same webOS protocol, the same
key artwork, and the same pairing manifest, so pairings made with
[lgtvremote-cli](https://github.com/griches/lgtvremote-cli) can be imported
unchanged.

## Features

- **Power** that knows the difference between standby and off: asks the TV for
  its real power state, then sends turn-off or Wake-on-LAN accordingly.
- **Volume, mute, play/pause, screen off, d-pad, back, home, menu, exit.** The
  mute tile follows the TV, so it stays right when you use the real remote.
- **App launcher** with bundled artwork for the popular streaming apps.
- **Input switching** with the names you gave the inputs on the TV
  (PS5, Xbox, PC…) drawn on the tiles.
- **Build your own remote.** Pages and tiles are a small JSON layout, edited in
  the browser on the device's own web page, with a live preview of the screen.
- **Multiple TVs**, with on-device discovery and PIN pairing. Nothing to
  configure on a computer.
- **On-device Wi-Fi setup** with a network scan and an on-screen keyboard, so
  a pre-built firmware binary is enough.
- **Over-the-air updates** from PlatformIO or by uploading a `.bin` in the browser.
- Backlight dims after a minute idle and wakes on touch.

| Apps | Extras and inputs | TVs |
|---|---|---|
| ![](docs/apps.png) | ![](docs/inputs.png) | ![](docs/tvs.png) |

| Pairing | Wi-Fi setup | Keyboard |
|---|---|---|
| ![](docs/pairing.png) | ![](docs/wifi.png) | ![](docs/keyboard.png) |

## Hardware

Any **ESP32-2432S028** board. They are sold under many names; look for the
2.8" 240x320 ILI9341 display with a resistive touch panel, an ESP32-WROOM and a
CH340 USB-serial chip. Both the micro-USB and the dual-USB variants work.

Nothing else is needed. A 3D-printed case is a nice touch; there are many on
Printables and Thingiverse for this board.

## Quick start

1. Install [PlatformIO Core](https://platformio.org/install/cli). On a Mac:
   `brew install platformio`.
2. Clone this repository and plug the board in over USB.
3. Build and flash:

   ```sh
   pio run -t upload
   ```

4. On first boot the board scans for Wi-Fi networks. Tap yours, type the
   password, tap **Join**.
5. Tap the status bar (bottom left) to open the TV list, tap **Scan for TVs**,
   tap your TV, and type the PIN the TV shows. Done.

The web page is at **http://lgremote.local** (or the IP shown in the serial
log). From there you can rearrange the remote, watch the screen live, and
update the firmware.

### Optional: compile in your Wi-Fi

If you would rather not type a password on a 2.8" keyboard, copy
`include/secrets.example.h` to `include/secrets.h` and fill it in. It is used
only to seed the device on first boot and is ignored by git.

### Optional: import pairings from lgtvremote-cli

If you already use the CLI, its pairings can be baked into the firmware as a
one-off seed so no re-pairing is needed:

```sh
python3 tools/gen_config.py      # reads ~/.config/lgtvremote/devices.json
pio run -t upload
```

This writes `include/tv_config.h` (ignored by git; it contains client keys).
Once the board has booted with it, the TVs live in the device's own flash and
the header is no longer consulted.

## Building your own remote

Open **http://lgremote.local**. Each page is a 4x3 grid; each tile has a
*kind*, an *argument*, and optionally an icon and label.

| Kind | Argument | What it does |
|---|---|---|
| `power` | | Power toggle with real power-state detection |
| `button` | `UP` `DOWN` `LEFT` `RIGHT` `ENTER` `BACK` `HOME` `MENU` `EXIT` … | Presses a remote key |
| `app` | a webOS app id, e.g. `netflix` | Launches the app; uses bundled art if there is any |
| `input` | an input id, e.g. `HDMI_2` | Switches input; shows the TV's own name for it |
| `ssap` | any `ssap://` URI, plus optional JSON payload | Sends a raw SSAP request |
| `mute` `playpause` `screen` | | Two-state tiles whose icon follows the TV |
| `volume_up` `volume_down` | | Volume, with auto-repeat while held |

Tiles without artwork show their label on a blank key. `repeat` makes any tile
auto-repeat while held. The same JSON is available at `/api/layout` if you
prefer to script it, and `/api/layout/default` returns the factory layout.

![Web editor](docs/web.png)

### Adding artwork

Icons are 64x64 RGB565 arrays compiled into the firmware and referenced by
name. `tools/gen_assets.py` produces them from 144px PNGs. To add an app,
put its id in the `APPS` list there, drop `<id>.png` in the source folder it
points at (set `LGTV_PLUGIN_IMGS` to override the path), run the script and
rebuild. The generated `src/assets.cpp` is committed, so you only need the
script when changing the icon set.

## Updating

- From PlatformIO over Wi-Fi: `pio run -t upload --upload-port lgremote.local`
- From the browser: build with `pio run`, then upload
  `.pio/build/cyd/firmware.bin` on the web page.
- Over USB: `pio run -t upload` as usual.

## How it talks to the TV

webOS exposes a JSON-over-WebSocket API (SSAP) on port 3001 (TLS, self-signed)
with a plain fallback on 3000 for old firmware. The board registers with a
manifest identical to the one used by lgtvremote-cli and Smart Remote+, which
is what makes client keys interchangeable between them. Remote
keys go over a second "pointer input" socket the TV hands out on request.
Wake-on-LAN uses the MAC addresses the TV reports after pairing, and a few
settings writes use the well-known `createAlert` workaround for `luna://`
calls. Discovery is SSDP for `urn:lge-com:service:webos-second-screen:1`.

All of this runs on the ESP32's second core so a TV that is off (and makes
TCP connects block) never stalls the touch UI.

## Developer notes

The board exposes a small command set on the serial port (460800 baud):

| Command | Effect |
|---|---|
| `S` | Dump the framebuffer (used by `tools/cyd.py`) |
| `t X Y` | Simulate a tap |
| `p N` | Jump to page N |
| `b NAME` | Send a remote key |
| `v` `w` `k` | Open the TV list / Wi-Fi list / keyboard |
| `scan`, `pair IP`, `pin N` | Drive discovery and pairing |
| `wifi SSID PASS`, `layout reset`, `h` | Set Wi-Fi, reset the layout, print free heap |

`tools/cyd.py` runs a sequence of those in one serial session and can save
screenshots, which is how every image in this README was captured:

```sh
python3 tools/cyd.py "p 1" "shot apps.png"
```

Opening the serial port resets the board (CH340 auto-reset), so the tool
waits for it to boot before sending anything.

### Display quirks

- Colours inverted (cyan where red should be)? Your panel is the other CYD
  variant: remove `-D TFT_INVERSION_ON=1` from `platformio.ini`.
- Touch offset? Adjust `RAW_MIN` / `RAW_MAX` at the top of `src/main.cpp`.
- Pixel read-back from this panel is shifted by one byte; the screenshot code
  compensates, which is why it isn't a plain `readRect`.

## Related

- [lgtvremote-cli](https://github.com/griches/lgtvremote-cli), the Python CLI
  whose pairing manifest and protocol this project shares.
- *Smart Remote+*, whose actions and artwork this remote reproduces.

## Support

If this saved you a remote or an afternoon, you can buy me a coffee at
**[ko-fi.com/garybbgames](https://ko-fi.com/garybbgames)**. Stars on the repo
and pull requests are just as welcome.

## Licence

MIT. App logos belong to their respective owners and are included for
identification only.
