# wifi-sniffer

Presence detection and home-security notifications built on **Home Assistant**,
two **ESP32** boards, a **PIR sensor**, and **MQTT**.

Unlike plain device trackers, the WiFi sniffer fingerprints the *contents* of
probe requests (information elements), so it can follow a device even while its
MAC address rotates — turning a stream of random MACs into stable "devices" and
flagging anything that lingers near the house.

## Features

- **Who's home** — HA companion app (iOS/Android) + BLE scanning via an
  ESPHome Bluetooth proxy
- **Unexpected presence** — PIR motion sensor (HC-SR501) wired to ESP32 #1
- **Unknown devices nearby** — WiFi probe sniffer on ESP32 #2, publishes to MQTT
- **Loitering detection** — fingerprint tracking across rotating MACs, with
  802.11 sequence-number confirmation that multiple MACs are one radio
- **Automations** — arrival/departure alerts, auto arm/disarm when the house is
  empty, motion-while-away alert, unknown-device and loiter alerts
  (quiet hours supported)

## Architecture

```
Phones (HA companion app) ──┐
ESP32 #1: BLE proxy + PIR ──┼──► Home Assistant ──► push notifications
ESP32 #2: WiFi sniffer ─────┘        (Docker + Mosquitto MQTT)
```

MQTT topics published by the sniffer:

| Topic | Payload | Frequency |
|---|---|---|
| `presence/wifi_probes` | raw MAC list | every 15s |
| `presence/wifi_devices` | fingerprint-level device list | every 15s |
| `presence/wifi_new` | new fingerprint event | on first sight |
| `presence/wifi_loiter` | loitering fingerprint event | once per loiter transition |

## Access

- **Local browser** — http://localhost:8123 (on the host machine)
- **Companion apps** — require HTTPS. Tailscale Serve provides a real
  certificate: `tailscale serve --bg http://127.0.0.1:8123`, then point the app
  at `https://<machine>.<tailnet>.ts.net` (tailnet-only, works from anywhere).
  HA needs `use_x_forwarded_for` + `trusted_proxies` for the reverse proxy -
  see setup-notes for the gotcha with `.storage/http`.

## Hardware

| Item | Price | Notes |
|---|---|---|
| ESP32 devkit #1 | ~$5 | BLE proxy + PIR (ESPHome firmware in `esphome/`) |
| ESP32 devkit #2 | ~$5 | dedicated WiFi sniffer (single-radio constraint) |
| HC-SR501 PIR sensor | ~$2 | wired to GPIO4 |
| Always-on host | — | runs the Docker stack (Mac/Pi/mini PC) |

## Repo layout

```
.
├── docker-compose.yml            # Home Assistant + Mosquitto MQTT broker
├── mosquitto/config/mosquitto.conf
├── esphome/
│   ├── presence-sensor.yaml      # ESP32 #1: BLE proxy + PIR
│   └── secrets.example.yaml      # copy to secrets.yaml, fill in
├── wifi_sniffer/
│   ├── wifi_sniffer.ino          # ESP32 #2: WiFi probe sniffer (Arduino)
│   └── config.example.h          # copy to config.h, fill WiFi + MQTT host
├── home-assistant/               # HA config: automations, sensors, helpers
└── setup-notes.md                # full walkthrough
```

## Quick start

1. **Start the hub** — `docker compose up -d`, then open http://localhost:8123
2. **Flash ESP32 #1** — fill `esphome/secrets.yaml`, then
   `.venv/bin/esphome run esphome/presence-sensor.yaml`
3. **Pair phones** — install the HA companion app, connect it via the HTTPS
   URL (see Access), link the phone tracker to a Person, and set your notify
   services in `home-assistant/notify.yaml`
4. **Flash ESP32 #2** — copy `wifi_sniffer/config.example.h` to `config.h`,
   fill in WiFi + `MQTT_HOST`, upload with the Arduino IDE
5. **Teach it your devices** — add MACs/fingerprints to the *Known WiFi
   MACs / Fingerprints* helper

Full instructions, wiring diagrams, and tuning knobs: [setup-notes.md](setup-notes.md)

## Honest caveats

- Phones randomize WiFi MACs since 2020; fingerprinting groups them, but two
  identical models on the same OS can rarely merge into one fingerprint
- Sniffer events are a "something is nearby" signal, not identification —
  family phones are pinned down by the companion app instead
- This is a notification layer, not a certified alarm system
