# CLAUDE.md

Guidance for Claude Code when working in this repository.

## What this is

VanLiveConnect is Arduino firmware for ESP8266 / ESP32 boards. It reads the VAN comfort bus of early-2000s
Peugeot / Citroën cars, hosts a Wi-Fi access point plus web server, and streams decoded vehicle data over a
WebSocket to a browser page that emulates the car's original Multi-Function Display (MFD).

Despite the parent folder name, this is **not** a React or Node project. There is no `package.json`, no npm, and
no test suite. The languages are C++ (Arduino `.ino` files) and a jQuery-based web UI embedded as string
literals.

## Building

Build with Arduino IDE 1.8.x or `arduino-cli`. The CI matrix in `.github/workflows/compile.yml` is the
authoritative reference for boards, board options, and library versions.

Typical ESP8266 (Wemos D1 mini) compile:

```bash
arduino-cli core install esp8266:esp8266 --additional-urls https://arduino.esp8266.com/stable/package_esp8266com_index.json
arduino-cli lib install VanBus "ESP Async TCP" "ESP Async WebServer"
arduino-cli compile --fqbn esp8266:esp8266:d1_mini --board-options xtal=160,ssl=basic,mmu=3232,non32xfer=fast,eesz=4M1M,ip=hb2n --warnings all ./VanLiveConnect
```

ESP32 (TTGO T7 Mini32) uses `esp32:esp32:ttgo-t7-v13-mini32` with libraries `"Async TCP"` and
`"ESP Async WebServer"`. Optional flags via
`--build-property compiler.cpp.extra_flags="-DSERVE_FROM_LITTLEFS"` (or `-DSERVE_FROM_SPIFFS`).

The `VanLiveConnect/ESP8266_IDE.*`, `ESP32_IDE.*`, `ESP32S2_IDE.*` scripts launch the Arduino IDE with the
right board options preset.

Required libraries: `VanBus` (>= 0.2.4, by the same author), ESP32Async forks of AsyncTCP / ESPAsyncTCP and
ESPAsyncWebServer. Older ESP32 cores (<= 1.0.6) need the dvarrel forks and `USE_OLD_ESP_ASYNC_WEB_SERVER`.
`VanLiveConnect/Patches/` holds patched copies of some library and core files (notably read-only SPIFFS,
because default SPIFFS writes cause VAN CRC errors).

There are no automated tests. "Verified" means it compiles cleanly under `--warnings all` for the CI matrix.
Runtime behaviour can only be checked on hardware or via the serial console debug defines.

## Layout

All firmware lives in `VanLiveConnect/`. Arduino concatenates every `.ino` in the folder into one translation
unit, so functions are declared across files via forward declarations at the top of each `.ino`.

| File | Role |
|---|---|
| `VanLiveConnect.ino` | Entry point. `setup()` brings up sleep, Wi-Fi, captive DNS, OTA, web server, WebSocket, VAN receiver, IR. `loop()` polls subsystems, receives one VAN packet per iteration, drops low-priority packets under queue pressure, pushes runtime stats every 5 s. |
| `Config.h` | All user-tunable settings: AP vs station mode, SSID/password, IP, sleep timeout, wake pin, IR pins, filesystem choice, debug defines. Edit here, not elsewhere. |
| `VanIden.h` | VAN packet identifier constants (`*_IDEN`). |
| `PacketToJson.ino` | Core (~5700 lines). One parser per IDEN turning raw bytes into JSON, with per-IDEN previous-data buffers for duplicate suppression. Dispatch table `IdenHandler_t`. |
| `OriginalMfd.ino` | State machine mirroring the real MFD: which small/large screen is showing, popups, trip computer cycling. |
| `PacketFilter.ino` | Selects which IDENs get printed for the serial debug defines. |
| `WebServer.ino` | ESPAsyncWebServer on port 80. Serves from PROGMEM or SPIFFS/LittleFS, ETags, HTTP 429 on low memory, selective Android `generate_204` handling. |
| `WebSocket.ino` | `/ws` endpoint, max two clients, 30-slot retransmit queue for "important" JSON, parses client→server messages (`in_menu:`, `mfd_language:`, `mfd_popup_showing:` …). |
| `IRrecv.ino` | Decodes the MFD infrared remote so menu navigation (never on the bus) reaches the browser. |
| `Sleep.ino`, `Wifi.ino`, `Esp.ino`, `Eeprom.ino`, `DateTime.ino`, `BasicOTA.ino` | Small support modules. |
| `Notifications.c/.h` | Warning/info message tables in EN, FR, DE, ES, IT, NL. Warnings end with `!`. |
| `MFD.html.ino`, `MFD.js.ino`, `CarInfo.css.ino`, `fa-all.css.ino`, `*.woff.ino`, `jquery-3.5.1.min.js.ino` | Web UI embedded as `PROGMEM` raw string literals. `MFD.js.ino` (~7200 lines) holds all display logic. |
| `data/` | Gzipped copies of the same web assets for SPIFFS/LittleFS serving. |

| `Display.ino` | Optional on-board 2.8" ILI9341 TFT + XPT2046 touch (branch `feature/s3-display`, ESP32-S3). Compiled only with `-DUSE_TFT_DISPLAY`. Taps the JSON stream via `DisplayOnJson()` called from `SendJsonOnWebSocket()`; three touch-cycled screens (instruments, audio, trip) plus notification popup. Pins and TFT_eSPI defines: `Config.h` "TFT display" section and `extras/Scripts/flash_s3_display.ps1`. |

`extras/` has schematics, screenshots, Android setup images, and IDE helper scripts.
`extras/Scripts/flash_s3_display.ps1` builds/uploads the ESP32-S3 display variant (TFT_eSPI is configured
through compiler defines, so this script is the single source of truth for the TFT pins). `README.md` is extensive
and is the primary user documentation.

## Data flow

VAN bus → transceiver → ESP GPIO → `VanBusRx` → `ParseVanPacketToJson()` → `SendJsonOnWebSocket()` →
browser `processJsonObject()` / `writeToDom()`.

JSON messages are `{"event": "display", "data": {...}}`. Keys in `data` map directly to HTML element IDs in
`MFD.html.ino`; `writeToDom` sets text, or for `style` / `class` suffixes toggles visibility. Adding a new
data field usually means: parser in `PacketToJson.ino` emits the key, `MFD.html.ino` gets an element with that
ID, and (if behaviour is needed) `MFD.js.ino` handles it in `handleItemChange`.

## Conventions and gotchas

- **Memory is tight** (ESP8266 has ~40 KB usable heap). Keep string constants in `PROGMEM` / `PSTR()` /
  `F()`, use `snprintf_P`, avoid `String` in hot paths. The `JSON_BUFFER_SIZE` is 4096 and shared.
- **Web assets are duplicated.** Changing HTML/JS/CSS means editing the `.ino` raw string *and* regenerating
  the matching `.gz` in `data/`. Keep them in sync.
- **Keep both platforms compiling.** Guard platform differences with `#ifdef ARDUINO_ARCH_ESP32` (and
  `CONFIG_IDF_TARGET_ESP32S2` for S2). Check against the ESP core version macros already used in the code
  (`ESP_ARDUINO_VERSION`, `VAN_BUS_VERSION_INT`).
- **Style:** 4-space indent, Allman braces, `} // if` / `} // function` closing comments, `PascalCase`
  functions, `camelCase` variables, `UPPER_SNAKE` macros. Match surrounding code.
- **Debug output** goes to `Serial` at 115200 and is gated by the `DEBUG_*` / `PRINT_*` defines in
  `Config.h`. Don't leave unconditional `Serial.print` in hot paths; Wi-Fi is sensitive to timing.
- **Behaviour fidelity matters.** Much of `OriginalMfd.ino` and `MFD.js.ino` exists to reproduce quirks of
  the real MFD (screen cycling, popup timing, sat nav list greying). Don't "simplify" these without
  understanding the original behaviour described in comments and `CHANGES.txt`.
- Version lives in `VanLiveConnectVersion.h` and `CHANGES.txt`; bump both together.
- Only tested on one vehicle (2003 Peugeot 406, type C MFD). Packet layouts for other models may differ.

## Web redesign branch (`feature/web-redesign`)

Visual restyle of the browser MFD in a modern head-unit look. Rules kept: every element id/gid, screen,
popup and script function of the original is unchanged; only layout, colours and decoration differ.
- Header bar (0..70 px) replaces the old bottom status strip; both panels moved down to top:70. Header tabs
  are decorative and follow `body[data-screen]`, which `changeLargeScreenTo()` now sets.
- Fuel and coolant are arc gauges: the `.gauge` element still receives the ESP's `scaleX(n)` transform,
  `processJsonObject()` mirrors it into CSS variable `--pct`, and CSS overrides the transform.
- Numeric text values are mirrored into `--val` (used by the audio-settings slider styling).
- Palette entries in `setColorTheme()` gained `--panel-fill` / `--panel-fill-2`; all new CSS derives from the
  palette variables so the blue/orange/gold and light/dark themes keep working.
- The bundled Font Awesome woff is a subset: only icons already used by the original page render.
- Preview without hardware: extract the raw strings from the `.ino` files to a folder, serve it statically and
  inject data in the console with `writeToDom({...})`.
