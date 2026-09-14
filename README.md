# Custom firmware for the SLZB-06MU Zigbee bridge (ESP-IDF, no Arduino)

English · [Русский](README.ru.md)

A replacement for SLZB-OS on an SLZB-06MU (ESP32-S3 + W5500 + EFR32MG21).
It does one thing: hands the Zigbee NCP on UART2 to the network over TCP 6638,
the way zigbee-herdsman expects it.

## Why

The stock firmware dropped the Zigbee network roughly fifteen times a day.
Disassembly showed the task serving the radio stops reading the UART until the
low four bits of an event group hold one specific value. Meanwhile the NCP keeps
transmitting: bytes pile up in the driver buffer, arrive in one burst, and by
then the NCP has run out its ASH timeouts and tears the link down.

## What is done differently

| | SLZB-OS | here |
|---|---|---|
| Reading the UART | suspended until an event group matches an exact value | unconditional, every pass of the loop |
| Flow control | `HW Flow: off` — RTS wired as a static GPIO, CTS input disabled | hardware RTS on: if we fall behind, the NCP is throttled in hardware |
| Blocking calls | present, without timeouts | none: `poll` with a 5 ms timeout, non-blocking socket and UART |
| Task watchdog | tasks not subscribed | every own task subscribed, panic on hang |
| A stalled TCP client | can hold the radio hostage | a new connection always evicts the old one |
| Diagnostics | log into a ring buffer | byte/drop/overflow counters in `/status`, console report every 30 s |

The hardware flow control line is the interesting part: RTS and CTS are wired on
the board, the stock firmware just never uses them — it says so itself in its own
log (`starting ZB1 serial on: 115200 baud. HW Flow: off`). So the brake existed
in hardware all along and was never applied.

Everything a bridge does not need — BLE, Wi-Fi, MQTT, add-ons, a megabyte and a
half of web UI — is not built at all: the image is 462 KB against 4 MB.


## Features

Everything is configured through the web UI and kept in NVS — not a single
address, password or port is baked into the firmware.

- **Radio-to-network bridge**: TCP port, UART baud rate and flow control
  (off / RTS / RTS+CTS) are all changeable without a rebuild.
- **USB mode**: the radio is handed to USB-CDC instead of the network, so the
  bridge can be plugged straight into a machine running Z2M. The console is
  taken over by data in that mode, so the log only survives via syslog.
- **Networking**: DHCP or a static address, hostname, DNS, custom MAC if needed.
  Wi-Fi comes up **only as a fallback** when the Ethernet link drops; with no
  network at all, an access point appears after a minute so the bridge can be
  configured without opening the case. mDNS and time sync included.
- **Flashing the radio through the bridge**: a `.gbl` file is uploaded from the
  browser into the Gecko Bootloader over XMODEM. The bridge never reaches out to
  the internet for it, unlike the stock firmware which pulled images from the
  vendor's server.
- **Updating the bridge itself** over the network, into the spare OTA slot.
- **MQTT**: the firmware publishes its own state with HA discovery — uptime,
  client presence, disconnects, buffer overflows, bytes both ways, chip
  temperature. No external collector needed.
- **Syslog** over UDP, so the log outlives a reboot on someone else's machine.
- **LEDs** can be switched off entirely or only at night, on a schedule.
- **Button**: short press resets the radio, 5 seconds reboots the bridge,
  15 seconds restores factory settings.

Deliberately not carried over from the stock firmware: hardware this board does
not have (second radio and CC1101, 4G, Z-Wave, Thread/OTBR), anything that takes
the bridge out to the internet (vendor auto-updates, VPN), and features of the
larger models in the series — BLE proxy, buzzer melodies, Ambilight.

## Hardware

Pinout lives in `main/board.h`. It was read off a running bridge over JTAG (GPIO
matrix + IO_MUX) and cross-checked against SMLIGHT's own board definition:
`github.com/smlight-tech/slzb-esphome`, `hw_defs/06xu/r1_73.yaml`.

- W5500 on SPI3: SCLK 42, MOSI 39, MISO 41, CS 2, INT 38, RST 40
- EFR32MG21 (NCP) on UART2: TX 17, RX 18, RTS 14, CTS 15, RST 21, boot 16
- LED 46 and 45, button 0, RJ45 backlight 1 — all inverted
- flash 16 MB DIO 80 MHz, PSRAM quad 80 MHz (taken from the stock bootloader header)

The MAC address is not hardcoded: it is derived from the base eFuse address with
`esp_derive_local_mac()`, exactly the way the stock Arduino firmware did it. The
bridge therefore keeps the address it had, and the DHCP reservation keeps handing
out the same IP. To force a different one, drop a `main/local_config.h` next to
the sources with `#define ETH_MAC_OVERRIDE { ... }` — it stays out of the repo.

## Build and flash

```sh
./build.sh                      # builds in docker, espressif/idf:release-v5.4
./flash.sh /dev/ttyACM0         # dumps the flash into backup/, then writes
./ota.sh <bridge-ip>            # later updates go over the network
./restore.sh backup/flash-<date>.bin # put the stock SLZB-OS back
```

`flash.sh` takes a full 16 MB dump first: that is the only way to get the stock
firmware back together with its settings and Zigbee network keys.

## What the bridge exposes

- TCP `:6638` — the NCP stream (`tcp://<bridge>:6638` in Zigbee2MQTT)
- HTTP `:80` — a status page, `/status` (JSON), `/update` (OTA),
  `/ncp/reset`, `/reboot`
- the button: a short press resets the NCP, a five-second hold reboots the bridge
- the console goes to the built-in USB-JTAG, UART0 is left alone

## Status

Running on a live bridge since 2026-09-14: Ethernet comes up, Z2M talks to the
NCP, and OTA over the network is confirmed (the bridge rebooted itself into the
`ota_1` slot, verified through otadata).

CTS is off by default: enable it only after confirming the radio was built with
hardware flow control, otherwise transmission stalls for good. The feature set
above compiles but the live bridge still runs the previous build — it will be
flashed once verified.

## License

MIT.
