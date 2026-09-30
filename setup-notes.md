# Presence-Based Home Security - Setup Notes

Presence detection + security notifications using Home Assistant, an ESP32
(BLE + PIR), and a second ESP32 (WiFi probe sniffer).

## Ready state (verified, nothing running)

- [x] All config files written + YAML-validated
- [x] arduino-cli 1.5.1 + esp32 core 3.3.11 + PubSubClient installed; `wifi_sniffer.ino` compiles clean
- [x] esphome 2026.7.4 in `.venv`; API key generated; ESPHome config validates
- [x] Docker images pre-pulled (Home Assistant + Mosquitto)
- [ ] Fill `esphome/secrets.yaml` WiFi SSID/password (flash-time only)
- [ ] Edit `MQTT_HOST` in `wifi_sniffer.ino` (Mac's LAN IP, flash-time only)

## File tree

```
wifi-sniffer/
├── docker-compose.yml            # Home Assistant + Mosquitto MQTT broker
├── mosquitto/config/mosquitto.conf
├── esphome/
│   ├── presence-sensor.yaml      # ESP32 #1: BLE proxy + PIR (ESPHome)
│   └── secrets.yaml              # fill in: wifi creds + API key
├── wifi_sniffer/
│   └── wifi_sniffer.ino          # ESP32 #2: WiFi probe sniffer (Arduino)
├── home-assistant/
│   ├── configuration.yaml        # includes all files below
│   ├── automations.yaml          # 7 automations (incl. unknown device + loiter)
│   ├── template.yaml             # "Someone Home" template binary sensor
│   ├── input_boolean.yaml        # Security Armed, Quiet Mode
│   ├── input_text.yaml           # Known WiFi MACs / Fingerprints
│   ├── mqtt.yaml                 # WiFi probe + distinct device counters
│   └── notify.yaml               # notify.everyone group (edit service names!)
└── setup-notes.md
```

## Shopping list

| Item | Price | Where |
|---|---|---|
| HC-SR501 PIR sensor (5-pack) | ~$5 | AliExpress/Amazon |
| 2nd ESP32 devkit (e.g. WROOM-32D) | ~$5 | same order |
| Jumper wires | ~$2 | same order |

## Step 1 - Start the hub (Home Assistant + MQTT)

Prereq: OrbStack / Docker running on your Mac (images already pre-pulled).

```bash
cd wifi-sniffer
docker compose up -d
```

- Home Assistant: http://localhost:8123 (create account on first visit)
- Mosquitto: localhost:1883, anonymous (LAN only - see Security notes)

## Step 2 - HA config + integrations

1. Copy the contents of `home-assistant/` into the HA config dir (the
   `./home-assistant` folder is already mounted as `/config` - files are there
   already).
2. Restart HA (Settings > System > Restart).
3. Add the **MQTT** integration: Settings > Devices & Services > Add
   Integration > MQTT > broker `mosquitto` (the Docker service name - NOT
   localhost), port `1883`, no auth.
4. Add the **ESPHome** integration (after Step 3 flashes the device).
5. Set your timezone/location: Settings > System > General.

## Step 3 - ESP32 #1: presence sensor (BLE + PIR)

ESPHome is already installed in `.venv`; the API key is already
generated in `esphome/secrets.yaml` (gitignored - on a fresh clone, copy
`esphome/secrets.example.yaml` to `secrets.yaml` first). Only the WiFi
SSID/password are missing.

1. Edit `esphome/secrets.yaml` and fill in your WiFi SSID/password.
2. Flash (device in USB), from `wifi-sniffer/`:
   ```bash
   .venv/bin/esphome run esphome/presence-sensor.yaml
   ```
   First build downloads the toolchain (~10 min, one-time).
3. HA auto-discovers it via the ESPHome integration.

### PIR wiring (when the sensor arrives)

```
HC-SR501        ESP32
---------       -----
VCC      ────  3V3
GND      ────  GND
OUT      ────  GPIO4
```

Trim the two potentiometers: left = sensitivity (start mid), right = delay
(rotate fully counter-clockwise for shortest pulse).

## Step 4 - Phones, persons, notifications

1. Install the HA Companion app (App Store / Play Store) on each phone; log in
   with the same HA account (or add each user).
2. Settings > People > Create a Person for each family member, link their
   phone tracker.
3. Edit `home-assistant/notify.yaml` - replace the two `mobile_app_*` service
   names with your actual ones (they appear under
   Settings > Devices & Services > your phone > Devices). Then restart HA.

## Step 5 - ESP32 #2: WiFi probe sniffer (with device fingerprinting)

1. Arduino IDE > Boards Manager > install "esp32 by Espressif".
2. Library Manager > install "PubSubClient by Nick O'Leary".
3. Open `wifi_sniffer/wifi_sniffer.ino`, edit the config block at the top
   (WiFi creds + `MQTT_HOST` = IP of this Mac).
4. Select your ESP32 board, upload, open Serial Monitor (115200) to verify
   "Sniffing started" and MQTT connected.

Compile-check from the CLI (already installed, verified builds clean):

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 --warnings all wifi_sniffer/wifi_sniffer.ino
```

### How it works

Phones randomize their MAC addresses, but the *content* of their probe
requests (the information elements) is stable per device. The sniffer hashes
that content into a **fingerprint** (8 hex digits) and tracks devices by it:

- one fingerprint with **N different MACs** = the same phone rotating MACs
- a fingerprint seen for a long time or with many MACs = someone **loitering**

MQTT topics:

| Topic | Payload | Frequency |
|---|---|---|
| `presence/wifi_probes` | raw MAC list | every 15s |
| `presence/wifi_devices` | fingerprint-level device list | every 15s |
| `presence/wifi_new` | new fingerprint event | on first sight |
| `presence/wifi_loiter` | loitering fingerprint event | once per loiter transition |

Firmware knobs at the top of the .ino (tune as needed):

| Define | Default | Meaning |
|---|---|---|
| `LOITER_MAC_COUNT` | 3 | distinct MACs for one fingerprint = loitering |
| `LOITER_SPAN_MS` | 1800000 (30 min) | fingerprint presence span = loitering |
| `SEQ_CONTIGUOUS_DELTA` | 100 | 802.11 sequence gap confirming same radio |
| `MIN_ALERTS_GAP_MS` | 1000 | min spacing between `wifi_new` publishes |

Verify in HA: the **WiFi Probe Devices** sensor counts raw probes, and
**WiFi Distinct Devices** counts fingerprints (devices, not MACs). Check raw
topics with a Mosquitto subscriber:

```bash
docker compose exec mosquitto mosquitto_sub -t 'presence/#' -v
```

## Step 6 - Known devices (MACs and fingerprints)

1. Check what the sniffer sees: Settings > Devices & Services > **WiFi
   Distinct Devices** sensor > attributes > `devices` list. Each entry has a
   `fingerprint`, current `mac`, `macs` count (MACs seen for it) and `span`.
2. Add your own devices as comma-separated values in the **Known WiFi MACs /
   Fingerprints** helper (Settings > Devices & Services > Helpers) - you can
   paste either a MAC or a fingerprint. Anything not on the list triggers the
   unknown-device alert; loitering devices get the loiter alert.

   Tip: `macs > 1` on one of *your* devices is normal - it just means the
   fingerprint logic correctly grouped that device's rotating MACs.

## Testing checklist

- [ ] `binary_sensor.someone_home` turns on when a person is home
- [ ] Arrival/departure notifications fire (with Quiet Mode off)
- [ ] Leave for 5+ min -> "Security armed" notification
- [ ] Walk in front of PIR while armed -> ALERT notification
- [ ] Toggle Security Armed manually via the HA dashboard
- [ ] Disable the sniffer's WiFi -> WiFi Probe Devices goes stale/offline
- [ ] Phone walks past the house -> "Unknown device nearby" notification
- [ ] Same phone loiters 30+ min -> "Device loitering" notification (once)
- [ ] Two identical phone models may share a fingerprint (rare, safe merge)

## Migrating to a Raspberry Pi later

- Stop: `docker compose down`
- Copy the whole `wifi-sniffer/` folder to the Pi
- `docker compose up -d` - identical stack. Change `MQTT_HOST` in the .ino to
  the Pi's IP (or a hostname) and re-flash the sniffer.

## Troubleshooting

| Problem | Fix |
|---|---|
| HA won't boot | Check `home-assistant/home-assistant.log` for YAML errors |
| Person states stay home | Check companion app location permission + notify service names |
| BLE detection flaky | Move ESP32 to a central spot; `power_save_mode: none` already set |
| Sniffer shows nothing | Confirm radio isn't blocked; check MQTT connect in serial log |
| Same phone triggers repeatedly | That's MAC rotation - fingerprinting groups them; re-flash if old firmware |
| Fingerprint keeps changing for one device | OS update changed probe IEs; re-add the new fingerprint to the known list |
| ESPHome compile slow | First build downloads the toolchain (~10 min, one-time) |

## Security notes

- MQTT is anonymous on the LAN - fine for home, but make sure port 1883 is
  NOT port-forwarded on your router.
- Phones randomize WiFi MACs since 2020, but probe content fingerprinting
  groups them into stable devices, so a loitering phone shows up as one
  fingerprint with many MACs. Identical devices on the same OS version can
  rarely merge into one fingerprint (safe direction for alerts).
- The sniffer identifies *device class*, not identity - family phones are
  pinned down by the companion app instead.
- This is a notification layer, not a real alarm. Consider an LD2410 mmWave
  radar ($12, ESPHome-native) if you want to detect stationary intruders.
