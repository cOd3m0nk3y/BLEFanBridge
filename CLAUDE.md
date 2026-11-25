# BLEFanBridge: developer notes

An ESP32 firmware that lets Alexa (and a local web page) control FanLamp Pro BLE ceiling fans **locally**, with no cloud service:

```
Echo --WiFi--> ESP32 emulating a Philips Hue bridge (Espalexa) --BLE advertising--> fan
```

README.md is the user guide. This file holds the details needed to change the code safely. It was written while building the project with an AI coding assistant, so it uses that assistant's working style ("ask the user to…").

## Working rules
- **Verify on hardware.** Protocol details from specs or libraries are guesses until the real fan obeys the board. When something doesn't work, capture raw packets first (Learn diagnostics, `/sniff`), then change the code. Label button codes as *sniffed from the remote*, *from NicoIIT's tables*, or *confirmed by the fan*.
- **Don't change the packet encoding in `fanlamp.h` casually.** Run the host test after any change (see Tests).
- Arduino inserts its generated function prototypes before the first function in the `.ino` (`bleInUse()`). Declare every struct, enum and constant that a function signature uses above it. Avoid default arguments on functions that others call before their definition.
- **WiFi and Bluetooth share the radio.** Never call `WiFi.setSleep(false)`; it aborts once BT is enabled.
- The core frees BLE memory at boot unless `bleInUse()` returns true. The sketch overrides it; keep that.

## Hardware (owner's setup)
- **Board:** AI Thinker ESP32-CAM on an ESP32-CAM-MB USB base with a CH340 USB-serial chip (VID 0x1A86, PID 0x7523). ESP32 rev 3.1, 4 MB flash. The camera and SD card are unused.
- **Uploading:** the base doesn't always auto-reset into the bootloader. If an upload hangs at "Connecting…", ask the user to hold **IO0**, tap **RST**, then release IO0.
- **Status LED:** red LED on GPIO 33, lit when LOW. Leave GPIO 4 alone; it drives the bright white flash LED.
- **Fans:** two Crazypai 52" DC fans (6 speeds, light) on one wall switch, plus two TYX BLE remotes. Remote B (`0x00000094`) controls both fans, and remote A (`0x00003068`) controls one.

## Build / upload (arduino-cli)
- **FQBN:** `esp32:esp32:esp32cam` with `--board-options PartitionScheme=huge_app`; WiFi + BT + the web server doesn't fit the default partition. The core is `esp32:esp32` 3.3.x, and the sketch has an `#error` guard requiring Bluedroid.
- **Libraries:** WiFiManager (tzapu), ESP Async WebServer 3.12.1 and Async TCP 3.6.1 (ESP32Async). Espalexa is **vendored** in `src/Espalexa/` (see Alexa).
- **Commands:**
  - `arduino-cli compile --fqbn esp32:esp32:esp32cam --board-options PartitionScheme=huge_app .`
  - `arduino-cli upload -p COMx --fqbn esp32:esp32:esp32cam --board-options PartitionScheme=huge_app .`
  - `arduino-cli monitor -p COMx -c baudrate=115200 -c dtr=off -c rts=off`. Keep `dtr=off rts=off`: with them on, the MB base holds the chip in reset, so you get no output and RST does nothing.
  - Find the port with `arduino-cli board list` (the CH340).
- **Logging while the user tests:** `arduino-cli monitor` exits at once without a TTY. Instead, open the port from PowerShell (`System.IO.Ports.SerialPort`, DtrEnable/RtsEnable false) and append lines to a file, as a background task. Stop it before uploading, because it holds the port.

## Files
- `BLEFanBridge.ino`: WiFi (WiFiManager portal "FanBridge-Setup"), Espalexa devices, fan settings in NVS, the JSON/web endpoints, the BLE advertising queue and press-and-hold, and learn/sniff scanning (raw Bluedroid GAP API).
- `fanlamp.h`: the FanLamp packet encoder and decoder. The v2/v3 code is ported from NicoIIT/esphome-components (MIT); v1 too, extended for the TYX variant.
- `webui.h`: static pages (`/`, `/settings`), CSS, PWA manifest and SVG icon. All data comes from `/status`, `/config` and `/learnstatus`.
- `icons.h`: generated PNG icons (192 "any", 512 maskable, 180 apple-touch) as byte arrays; Chrome needs PNG manifest icons to install the page. They're rendered from the fan SVG with headless Edge, so regenerate them rather than hand-editing.
- `src/Espalexa/`: Espalexa 2.7.0 plus one patch (see Alexa).
- `tests/fanlamp_test.cpp`: host test for `fanlamp.h`. The Arduino build ignores `tests/`.

## Tests
- From the repo root, on Linux or WSL with g++ and libssl-dev:
  `cd tests && g++ -std=c++17 -Wall -I.. fanlamp_test.cpp -lcrypto -o /tmp/fanlamp_test && /tmp/fanlamp_test`
  (On the owner's Windows machine: `wsl -d Ubuntu-24.04 -e sh -c 'cd <repo>/tests && …'`.)
- App vectors that must stay byte-identical: id `0xD2135C22`, index 2, seed `0x2B53`. The hex starts after the 3-byte flags AD.
  - v3 pair, tx 117: `1B03F0082080B801E122C6F2D3A76744A49F67F6B678458B532BB91B`
  - v3 speed 2 of 6, tx 127: `1B03F0082080B80BE122C6F2D3A7675DA49F47F4B65B338B532B6390`
  - v3 off, tx 128: `1B03F0082080B8F4E122C6F2D3A7675DA49F47F6B6D9138B532BE608`
- The test also includes real remote captures that must decode and re-encode byte-identical: v2-format remotes, TYX v1 (speed and both remotes' light-on), and NicoIIT's v1 examples. It also checks rejection of bad CRCs and non-FanLamp packets.
- Check the page JavaScript after editing `webui.h`. Extract the `<script>` bodies and run `node --check` on them, because the C string pasting makes typos easy.

## Packet formats
- Every known variant is 31 bytes: an optional flags AD `02 01 <adFlag>`, then `<len> <adType> <2–5 byte header>` and a 24-byte body (see the comment at the top of `fanlamp.h`). `Format.proto` picks the body: 2 = v2/v3, 1 = v1.
  - App v3: flag 0x19, AD type 0x03, header `F0 08`, prefix `20 80 00`, devType 0x0400, AES-signed.
  - App v2: the same, but prefix `10 80 00` and unsigned.
  - v2-format physical remote: flag 0x02, AD type 0x16, prefix `10 00 56`, devType 0x0400 (0x0100 also seen), usually signed.
  - **v1** (NicoIIT `FanLampEncoderV1`): an 8-byte prefix `AA 98 43 AF 0B 46 46 46`, a 14-byte struct (cmd, group_index LE, args[3], tx, param1, src, r2, seed BE, crc16 BE), then crc2. The body is bit-reversed per byte and LFSR-whitened (seed 0x6F). The decoder spots v1 by that prefix in the last 24 bytes of any AD structure and reads the variant options from the packet: `V1_XOR1`, `V1_SEED_ZERO`, `V1_CRC1_ZERO`, a forced crc2, and the light-on crc nibble. It treats args[2] as pairArg3 only for xor1 variants.
- `Identity` = Format + 32-bit remote ID + group index. A fan sends as its learned Identity if it has one, otherwise as `{app v3/v2, its own random ID, index 0}`. The tx count wraps 255 → 1.

## TYX remotes (FanLamp v1 variant, found 2026-11-22)
- FCC ID 2BHLF-TYX0004; not in NicoIIT's list, and not on fccid.io. Header `F0 FF` in manufacturer data (AD type 0xFF), flags AD present with value 0x00, seed always 0, crc16 left 0, crc2 computed.
- **Each press is a burst numbered tx 1..5** that starts again at 1 on the next press, about 100 ms per packet. `sendCommand` copies this: 5 packets, tx 1..5, 200 ms each (`V1_BURST`, `V1_BURST_MIN_MS`). The fans accept that timing.
- **Light-on crc2 quirk:** for `0x10` only, some remotes compute crc2 as if the group-index nibble (low nibble of struct[2]) were XORed with a per-remote value n, while sending it unchanged. Remote B (the group remote for both fans) uses n = 2; remote A uses 0. The fans reject a light-on with the wrong crc2, which made light-on flaky until this was found. n is stored in the high nibble of `Format.v1` (`V1_LIGHT_CRC_MASK`), so the NVS blob didn't change. The decoder finds n by trying 1..15 when a light-on crc2 fails. Only a light-on press reveals n: Learn keeps a stored n when re-learning the same remote from another button, and Sniff (`noteLightCrc`) updates a learned fan when it hears that remote's light-on. **Learn TYX remotes from the light button.**
- **Press-and-hold** (brightness and color temperature): while held, the remote sends `0x80` in args[2], with tx counting up every ~100 ms (a 6 s hold reached tx 58). On release it repeats the last tx with `0x81` for ~150 ms. `holdButton`/`holdLoop` copy this at about 110 ms per tx. The page sends `hold=1` once, then `hold=2` keep-alives every 500 ms (these can't start a hold, so a late one can't restart the ramp), and `hold=0` on release. The board lets go by itself after 1.5 s without keep-alives, or after 10 s. A tap still sends tx 1..3.
- Button codes, with *confirmed* meaning the fan obeyed the board (2026-11-22/23):

  | Button | Code (cmd, args) | Board → fan |
  |---|---|---|
  | Speed 1–6 | `0x32`, arg0 = speed | 2 confirmed; the others not yet |
  | Fan off | `0x31`, arg0 0 | confirmed |
  | Forward / reverse | `0x15`, arg0 0 / 1 | not confirmed yet |
  | Light | `0x10` on / `0x11` off. The remote tracks the state and alternates | confirmed on both fans, as both remotes |
  | Brightness + / − | `0x04` / `0x05`, press-and-hold as above | confirmed |
  | Color temp + / − | `0x06` / `0x07`, press-and-hold | confirmed |
  | K (Warm / Neutral / Cool) | `0x21`, arg0 = cold, arg1 = warm: `00 FF` / `FF FF` / `FF 00`; the remote's K button cycles through them | confirmed |
  | Night | `0x23` | confirmed |
  | Breeze (wavy line) | `0x33`, arg0 2 | confirmed |
  | All off | `0x6F` | confirmed |

## Learn and sniff
- `GET /learn?fan=N` runs a 20 s passive scan (interval 0x50, window 0x30, duplicate filtering off). Advertising pauses and the command queue is held until the scan ends; a broadcast that gets cut off is re-queued.
- The GAP callback runs on the BT task. It only decodes and copies into portMUX-protected structs, with no String or heap use. `loop()` saves the first Ok packet's Identity and sets that fan's tx counter to the remote's tx.
- `GET /learnstatus` returns `{state: idle|listening|done|timeout|unsupported, fan, secs, msg}`. Everything is logged to Serial with a `[Learn]` prefix.
- Diagnostics at the end print up to 16 distinct raw packets that were strong (≥ −55 dBm), contained `F0 08`, or **looked like FanLamp but had a bad CRC**. That last group is how the TYX format and the light-on quirk were found.
- `GET /sniff?secs=N` (5–300, default 60) maps buttons. It logs each distinct decoded packet as `[Sniff] #n <ms> …`, plus how many times it repeated and when it was last heard, and saves nothing (except `noteLightCrc`). Ask the user to press one button every ~5 s, and to say the order.

## Web server and concurrency
- **Async server** (ESPAsyncWebServer). The core `WebServer` serves one connection at a time and waits up to 5 s (`HTTP_MAX_DATA_WAIT`, compiled into the core) on an idle connection. Browsers that preconnect spare sockets stall on that.
- Async handlers run on the AsyncTCP task. Every route is registered through `web()`, which takes `appLock` (a recursive mutex) and sets the global `rq` to the current request. `loop()` holds the same lock while it works. Keep all fan, queue, NVS and Serial work behind that lock, and never block inside a handler.
- `logRequest()` prints `[HTTP] <ip> <method> <url> (<user agent>)` for page and API requests, skipping the frequent polls. Use it to tell whether a browser reaches the board at all.

## Alexa (Espalexa)
- **Vendored** in `src/Espalexa/`: 2.7.0 plus upstream PR #235, which changes the uniqueid from `MAC:00:11-IDX` to `MAC-IDX-00:11`. With the released 2.7.0, Alexa merged the fan and light into one device: it showed the light's name and controlled the fan. Don't switch back to the Library Manager version until a release includes the fix. Changing the uniqueid format makes Alexa see new devices, so the old ones have to be deleted in the Alexa app.
- `ESPALEXA_ASYNC` mode on the shared server. The Alexa PUT body arrives either as the `body` param (form content type) or through Espalexa's `onRequestBody` (JSON); both are tested.
- Each fan is a dimmable "light" whose brightness % maps to speed (6 speeds: 16/33/50/66/83/100). The light is an on/off device. `ESPALEXA_MAXDEVICES` is 12 (6 fans + 6 lights).
- Devices are created at boot, so adding, removing, renaming or hiding a fan (or its light) restarts the board. Espalexa numbers devices in the order they're added, and the uniqueid includes that number, so hiding or removing an earlier fan renumbers the later ones (Alexa sees them as new).
- The link is one-way: the fan never answers, so the state shown in Alexa is best-effort.

## Fan settings (NVS namespace `fanbridge`)
- `nf` holds the fan count (0–6). For fan i:
  - `fn<i>`: name
  - `fl<i>`: light name
  - `fa<i>`: "Show in Alexa" (bool, default true)
  - `fb<i>`: a 33-byte blob holding the version byte (2), speeds, proto, learned flag, own ID, and the full learned Identity, including the v1 fields. The layout is in `packFan()`.
- Change the version byte if the layout changes. A blob that's missing or has the wrong version resets that fan to defaults.
- **Pairing:** power-cycle the fan and send the pair command within about 5 s. Each fan's own remote ID is random, created when the fan is added. NVS survives normal uploads but not a flash erase. Fans that share a wall switch all pair together; use Learn for those.

## Phones and installing the page
- **Android 17 Chrome** needs the OS **Nearby devices / local network** permission. Without it, every connection to a LAN address fails with `ERR_LOCAL_NETWORK_PERMISSION_MISSING` (visible in `chrome://net-export`), and Chrome shows `ERR_TOO_MANY_RETRIES`. Samsung Internet worked. This was probably also why the WiFiManager setup portal never loaded on the owner's phone.
- Chrome only *installs* PWAs from secure origins. On `http://`, use Add to home screen → Create shortcut, or enable chrome://flags "Insecure origins treated as secure" for `http://<board-ip>`.
- The WiFiManager setup portal still uses the core `WebServer`, so it may misbehave on phones; a laptop works.
