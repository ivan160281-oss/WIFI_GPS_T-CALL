# ALTGEO GSM tracker — LILYGO T-Call A7670

Client-device firmware: fully autonomous GPS + WiFi + BLE tracker with
real-time sync over the cellular network. No SD card, no user configuration,
no screen - plug in power (with a SIM card inserted) and it runs.

## What "fully autonomous" means here

- **No config file, no first-run setup.** The server address and shared sync
  password are compiled into the firmware itself (see `CONFIGURE BEFORE
  FLASHING` at the top of the `.ino`) - the person receiving the device
  never touches any settings.
- **No SD card.** Everything - the offline point queue, sequence counters -
  lives on the ESP32's own internal flash (LittleFS + NVS), per the
  "completely self-contained" hardware requirement.
- **The only per-device thing is the IMEI**, and that's already burned into
  the SIM/modem hardware - nothing to type in at flash time.

## Finding the device's IMEI (for registering it on your account)

The device **continuously broadcasts over BLE** as:

```
ALTGEO_IMEI_<15-digit IMEI>
```

Open Bluetooth settings on any nearby phone (no app, no pairing - this is a
broadcast-only advertisement) and it'll show up under exactly that name.
Copy the digits after `ALTGEO_IMEI_` into the **"Add device"** form on the
ALTGEO dashboard, selecting **IMEI** as the identifier type.

## How positioning works

Unlike the SD-card tracker (which has a dedicated GPS chip), GNSS on this
board comes from the A7670 modem itself - there's no separate GPS module to
wire up. Position, WiFi, and BLE are all gathered the same way as the other
tracker (GPS-independent positioning fills in wherever satellites can't
reach), just read over the modem's AT-command link instead of a dedicated
GPS UART.

- **GNSS**: `modem.getGPS()` (TinyGSM) - the modem's own integrated
  receiver, enabled once at boot (`modem.enableGPS()`).
- **WiFi**: passive scan only - never associates or connects to anything it
  sees, exactly like the SD-card tracker.
- **BLE**: passive scan for nearby fixed devices, interleaved with this
  device's own continuous IMEI advertisement. The ESP32 BLE radio supports
  advertising and scanning at the same time as a standard combined role -
  see "Not yet verified on real hardware" below for the one caveat on this.

## Real-time sync, no data loss on GSM dropout

Every captured point is first written to a small persistent queue on
internal flash (`/queue.jsonl`, LittleFS) - a separate timer then tries to
push queued points to the server (`POST /api/device/points`) every 20
seconds. A point is only ever removed from the local queue once the server
has explicitly acknowledged storing it, by sequence number (see the server's
own `db.ingest_realtime_points` for the matching half of this). If there's
no signal, or the request fails, the queue just keeps growing - nothing is
lost, it flushes automatically whenever connectivity returns. Sequence
numbers persist in NVS (`Preferences`) across reboots/power loss, which the
server's idempotency check relies on to safely re-accept a retried batch
without storing duplicates.

The queue is capped at ~1.5MB; if that's ever exceeded (implies a very long
stretch with no signal at all), the **oldest** points are dropped first -
same circular-buffer philosophy as the SD-card tracker.

## Hardware

LILYGO T-Call A7670 V1.0/V1.1 - ESP32 + SIMCom A7670 (4G Cat-1, integrated
GNSS). Pin assignments in the firmware are sourced directly from LilyGO's
own reference firmware (`LilyGO-T-A76XX/examples/*/utilities.h`):

| Signal | Pin |
|---|---|
| Modem TX/RX | GPIO 26 / 25 |
| Modem PWRKEY | GPIO 4 |
| Modem DTR | GPIO 14 |
| Modem RESET | GPIO 27 |
| Status LED | GPIO 12 |

**Double-check these against your specific board revision's silkscreen
before flashing a whole batch** - LilyGO has shipped more than one pin
layout under similar model names over time.

## Getting a .bin without installing anything locally

Same pattern as the SD-card tracker: `.github/workflows/build.yml` builds
automatically via GitHub Actions on every push, using stable ESP32 core
3.3.10 plus the TinyGSM/ArduinoHttpClient/ArduinoJson libraries.

1. Push this repo (keeping the folder structure) to a GitHub repo of your own
2. Open the **Actions** tab - the build starts automatically
3. Download `altgeo-gsm-tracker-bin` from the finished run's **Artifacts**
4. Flash the `.bin` - either via the ALTGEO web flasher (Tracker page →
   "Прошить устройство в браузере", Web Serial, no software install) or
   `esptool.py write_flash 0x10000 altgeo_gsm_tracker.ino.bin` locally

## Server-side API this firmware talks to

```
POST /api/device/points
Headers:
  X-Device-IMEI: <imei>
  X-Sync-Password: <matches the server's WIFIGPS_PASSWORD>
  Content-Type: application/json
Body:
  {"points": [{"seq": 1, "ts": "...", "lat": 55.7, "lon": 37.6,
               "status": "ok", "wifi": [...], "ble": [...]}, ...]}
Response:
  {"acked_seqs": [1, 2, 3], "device_id": 42}
```

The device is auto-registered on its very first successful contact
(unclaimed, same as chip-id devices) - claiming it to a real account happens
separately, on the dashboard, using the IMEI read off the BLE broadcast.

## Not yet verified on real hardware

This was written and structurally checked (brace/paren balance, every
function defined and called consistently) without access to a physical
T-Call A7670 board or an ESP32 toolchain to actually compile it - the same
constraint that applied to the SD-card tracker's firmware.

Two build issues turned up on the first two cloud-build attempts and are now
fixed:

1. TinyGSM's `#error "Please define GSM modem model"` even with the modem
   macro defined before `#include <TinyGsmClient.h>` - moved the modem/pin
   `#define`s into their own `utilities.h`, included first, matching
   LilyGO's own example structure. This turned out not to be the actual
   cause of the error (see #2) but is a genuine Arduino gotcha worth keeping
   fixed regardless - the sketch's own prototype-generation pass can insert
   generated code between top-level `#define`/`#include` lines in a `.ino`
   file.
2. **The real cause**: the same error persisted after #1, because the macro
   itself - `TINY_GSM_MODEM_A7670` - doesn't exist in the mainline TinyGSM
   library that `arduino-cli lib install "TinyGSM"` pulls in (confirmed by
   reading its actual `TinyGsmClient.h`: it `#ifdef`s a long list of SIMCom/
   uBlox/Quectel models, and plain "A7670" isn't one of them - only
   `TINY_GSM_MODEM_A7672X`, a related but different SIMCom modem, is).
   LilyGO's own official example for this exact board
   (`LilyGo-T-PCIE/examples/A7670/TinyGSM_Net_GNSS`) uses
   `TINY_GSM_MODEM_SIM7600` instead, with the comment "A7670 Compatible with
   SIM7600 AT instructions" - so that's what this firmware now uses too, not
   a guess but LilyGO's own validated choice for the same hardware.

Before flashing a whole batch of devices, specifically verify:

- **The PWRKEY pulse sequence and timing** - SIMCom modems are sensitive to
  the exact HIGH/LOW pulse order and duration, and sources disagree slightly
  between LilyGO's own examples for closely related boards. If the modem
  doesn't respond to `modem.init()`, this is the first thing to adjust.
- **Simultaneous BLE advertising + scanning** - supported in principle by
  the ESP32 BLE controller, but the exact behavior (scan quality while
  advertising, any need to briefly pause advertising during a scan window)
  can vary by arduino-esp32 core version. If BLE scan results look sparse
  compared to the SD-card tracker, try pausing advertising for the ~2s scan
  window and resuming it right after.
- **APN auto-detection** - `ALTGEO_APN` is left blank, which many SIMs/
  carriers accept and get auto-assigned a working APN by the network; if
  yours doesn't connect, fill in your carrier's APN explicitly.
- **GNSS and network-time behavior under SIM7600 compatibility mode** -
  LilyGO's own example exercises GNSS this same way (it's literally named
  `TinyGSM_Net_GNSS`), which is reassuring, but hasn't been cross-checked
  against this specific firmware's exact call sequence on real hardware.
- **`modem.getNetworkTime()` availability** - not all carriers/firmware
  revisions expose network time over AT commands; if it's
  unavailable, points fall back to a `uptime_<seconds>` timestamp, which
  the server will still store, just without a meaningful absolute time
  until this is confirmed working (or replaced with NTP-over-GPRS as a
  fallback).
