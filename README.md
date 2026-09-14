# ps4-900u-host

An **ESP32 exploit host for the PS4 on firmware 9.00**. It runs a small Wi-Fi
access point + web server that serves the **PsFree** webkit exploit to the PS4
browser and lets you upload and launch payloads (GoldHEN, etc.). It also proxies
the PS4's NTP requests so **GoldHEN's time sync works without internet**.

Built with the Arduino-ESP32 core for **ESP32-S2 / ESP32-S3** boards (USB-drive
emulation) and, with a wired USB setup, plain **ESP32**.

> Based on [stooged/ESP32-Server-900u](https://github.com/stooged/ESP32-Server-900u),
> with PsFree, an on-board NTP forwarder and a config editor.

## Features

* **PsFree webkit exploit** served from flash — point the PS4 browser at the
  host and it loads the exploit, then a payload from SPIFFS.
* **Payload manager in the web UI** — upload / list / run `.bin` payloads
  (GoldHEN and friends) stored on the ESP32's SPIFFS.
* **NTP forwarder** — intercepts GoldHEN's hard-coded NTP (time1.google.com,
  `216.239.35.0:123`) via an lwIP DNAT rule and answers it from the ESP32, so the
  PS4 clock syncs with no internet. Up to 4 configurable upstream NTP servers,
  queried concurrently (fastest reply wins).
* **Config editor** (`/config.html`) — AP SSID/password/IP, Wi-Fi STA
  connection, hostname, USB wait / sleep, **CPU frequency** and **low TX power**
  (power/heat saving), the NTP servers + on/off, and a diagnostic logger toggle.
* **Diagnostic log** — optional BOOT/NTP events to `/log.txt` on SPIFFS.
* **Brownout-threshold tweak** — lowers the ESP32-S2 analog BOD level to reduce
  spurious brownout resets on marginal USB power.
* **Built-in web file manager** and firmware updater.

## Hardware

| Board | Notes |
|-------|-------|
| ESP32-S2 / ESP32-S3 | Uses USB-OTG drive emulation (the exploit chain can present a USB mass-storage device). Recommended. |
| ESP32 (classic) | Set `USBCONTROL true` and wire the USB data lines to `usbPin` (default GPIO 4). |

A board with **4 MB flash** is recommended (the embedded pages + exploit +
SPIFFS payloads). ESP32-S2/S3 with **PSRAM** is ideal.

## Building

This is an **Arduino sketch** — open
`ESP32_Server_900u_HTTP_SPIFFS/ESP32_Server_900u_HTTP_SPIFFS.ino` in the Arduino
IDE.

### Requirements

* **Arduino IDE 1.8.x / 2.x**.
* **ESP32 core** (Boards Manager → *esp32* by Espressif) — built and tested
  against **2.0.5**.
* Libraries (Library Manager):
  * **ESPAsyncWebServer**
  * **AsyncTCP**

### Board settings

* Board: your ESP32-S2/S3 (or ESP32) board.
* Partition scheme: one with enough app space for the sketch (the embedded page
  and exploit arrays are large) — e.g. *"Huge APP"* / a 4 MB scheme.
* PSRAM: enable if your board has it.

### ESP32 core patch — required for the NTP forwarder

The stock ESP32 core compiles lwIP's NAPT support **out**
(`CONFIG_LWIP_IPV4_NAPT` is unset), so the NTP forwarder — which DNATs
GoldHEN's hard-coded NTP server (`216.239.35.0:123`) to the ESP32 — **does not
work on an unmodified core**.

A ready-to-apply patch is included: **[`docs/lwip-napt.patch`](docs/lwip-napt.patch)**.
It patches the lwIP source (a narrow DNAT receive hook so *only* UDP packets to
`216.239.35.0:123` are rewritten, plus the reply-source rewrite and a faster SNTP
failover) against **ESP-IDF v4.4**. It has two hunks, marked in the file:

1. apply in the ESP-IDF repo root (`components/lwip/Kconfig`,
   `components/lwip/port/esp32/include/lwipopts.h`)
2. apply inside `components/lwip/lwip` (the lwip submodule)

Then build the core with these sdkconfig flags:

```
CONFIG_LWIP_IPV4_NAPT=y
CONFIG_LWIP_IP_FORWARD=n
CONFIG_LWIP_L2_TO_L3_COPY=y
```

and replace the core's
`~/.arduino15/packages/esp32/hardware/esp32/<ver>/tools/sdk/esp32s2/lib/liblwip.a`
(and mirror the two flags in the SDK's `sdkconfig.h`).

Without this patch the sketch still **builds and serves the exploit + payloads**,
but the PS4 clock won't sync — the sketch's `ip_napt_enable(...)` call is
guarded by `#if IP_NAPT` and is simply compiled out on a stock core.

Step-by-step rebuild + install instructions: **[`docs/build-core.md`](docs/build-core.md)**.

### Uploading payloads

The exploit chain is embedded in `Loader.h`; the **payload** (e.g.
`goldhen.bin`) lives on SPIFFS. Put your `.bin` files in the sketch's `data/`
folder and use **Tools → ESP32 Sketch Data Upload**, or connect to the AP and
upload them from the web **File Uploader**.

## Usage

1. Flash the sketch. The board brings up an access point:
   * **SSID:** `PS4`
   * **Password:** `900cracker`
   * Web UI: `http://ps4.local/` (or the AP IP, default `10.1.1.1`)
   All of these are editable in the config page; settings persist in
   `/config.ini` on SPIFFS.
2. On the PS4 (9.00), open the **User's Guide / browser** and go to the host's
   URL. The exploit loads, then the selected payload.
3. Manage payloads and settings from the web UI (`/admin.html`, `/config.html`).

### `config.ini` keys

`AP_SSID`, `AP_PASS`, `WEBSERVER_IP`, `WEBSERVER_PORT`, `SUBNET_MASK`,
`WIFI_SSID`, `WIFI_PASS`, `WIFI_HOST`, `USEAP`, `CONWIFI`, `USBWAIT`,
`ESPSLEEP`, `SLEEPTIME`, `CPU_FREQ`, `LOW_TXPOWER`, `NTP_SERVER1..4`,
`USE_NTP`, `USE_LOG`.

## Credits

* **[stooged/ESP32-Server-900u](https://github.com/stooged/ESP32-Server-900u)** —
  the original ESP32 PS4 9.00 host this is based on.
* **[PsFree](https://github.com/kmeps4/PSFree)** — the webkit exploit.
* **[GoldHEN](https://github.com/GoldHEN/GoldHEN)** — the payload used on the PS4.
* **[ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)** by
  me-no-dev (and AsyncTCP).

## Disclaimer

For **educational / homebrew use only**, on **your own hardware**. No pirated
content is included. The authors are not responsible for any damage or misuse.
