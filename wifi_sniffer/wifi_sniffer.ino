// WiFi Probe Sniffer for ESP32 - fingerprint edition
// Captures 802.11 probe requests and null data frames.
// Devices are fingerprinted by their probe request content (IE structure),
// which stays stable even when phones randomize their MAC addresses.
// So "the same phone" is tracked as one fingerprint with N MACs.
//
// MQTT topics:
//   presence/wifi_probes   - raw MAC list, every 15s (powers the counter sensor)
//   presence/wifi_devices  - fingerprint-level device list, every 15s
//   presence/wifi_new      - one event per newly-seen fingerprint
//   presence/wifi_loiter   - fingerprint loitering (many MACs or long span)
//
// Requirements:
//   - ESP32 devkit (dedicated - single radio, cannot also run ESPHome BLE)
//   - Arduino IDE with esp32 core
//   - Library: PubSubClient by Nick O'Leary
//
// Edit the config block below before flashing.

#include <WiFi.h>
#include <PubSubClient.h>
#include <stdarg.h>
#include "esp_wifi.h"

// ---------------------- CONFIG ----------------------
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"
#define MQTT_HOST        "192.168.1.10"   // IP of the machine running Home Assistant
#define MQTT_PORT        1883

#define MQTT_TOPIC_PROBES   "presence/wifi_probes"
#define MQTT_TOPIC_DEVICES  "presence/wifi_devices"
#define MQTT_TOPIC_NEW      "presence/wifi_new"
#define MQTT_TOPIC_LOITER   "presence/wifi_loiter"
// -----------------------------------------------------

#define SEEN_WINDOW_MS       300000   // keep raw MACs for 5 minutes
#define DEVICE_WINDOW_MS     300000   // keep fingerprints for 5 minutes
#define PRUNE_INTERVAL_MS    30000
#define PUBLISH_INTERVAL_MS  15000
#define MAX_PROBES           64
#define MAX_DEVICES          64

#define MIN_ALERTS_GAP_MS    1000     // min spacing between wifi_new publishes
#define LOITER_MAC_COUNT     3        // >=N distinct MACs for one fingerprint = loitering
#define LOITER_SPAN_MS       1800000  // or fingerprint seen for more than 30 minutes
#define SEQ_CONTIGUOUS_DELTA 100      // seq gap accepted as "same radio" confirmation

struct Probe {
  uint8_t mac[6];
  uint32_t lastSeen;
  int8_t rssi;
};

struct Device {
  uint32_t fingerprint;
  uint8_t mac[6];
  int8_t rssi;
  uint16_t seq;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint16_t macCount;
  bool seqContiguous;
  bool loiterReported;
  volatile bool pendingNew;
  volatile bool pendingLoiter;
};

static Probe probes[MAX_PROBES];
static Device devices[MAX_DEVICES];
static uint32_t lastPrune = 0;
static uint32_t lastPublish = 0;
static uint32_t lastNewPublish = 0;

WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------------- raw MAC tracking ----------------

void addProbe(const uint8_t *mac, int8_t rssi) {
  uint32_t now = millis();
  for (int i = 0; i < MAX_PROBES; i++) {
    if (memcmp(probes[i].mac, mac, 6) == 0) {
      probes[i].lastSeen = now;
      probes[i].rssi = rssi;
      return;
    }
  }
  for (int i = 0; i < MAX_PROBES; i++) {
    if (probes[i].lastSeen == 0) {
      memcpy(probes[i].mac, mac, 6);
      probes[i].lastSeen = now;
      probes[i].rssi = rssi;
      return;
    }
  }
}

void pruneProbes() {
  uint32_t now = millis();
  uint32_t cutoff = (now > SEEN_WINDOW_MS) ? now - SEEN_WINDOW_MS : 0;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (probes[i].lastSeen != 0 && probes[i].lastSeen < cutoff) {
      memset(&probes[i], 0, sizeof(Probe));
    }
  }
}

// ---------------- fingerprint tracking ----------------

// Hash the TLV structure (tag, length, payload) of all IEs in a probe request.
// This captures the vendor IE set/order that stays stable per device+OS.
uint32_t computeFingerprint(const uint8_t *frame, uint32_t frameLen) {
  uint32_t h = 2166136261u;
  uint32_t off = 24; // 802.11 header
  while (off + 2 <= frameLen) {
    uint8_t tag = frame[off];
    uint8_t len = frame[off + 1];
    if (off + 2 + len > frameLen) break;
    h ^= tag;      h *= 16777619u;
    h ^= len;      h *= 16777619u;
    for (uint8_t i = 0; i < len; i++) {
      h ^= frame[off + 2 + i];
      h *= 16777619u;
    }
    off += 2 + len;
  }
  return h;
}

uint16_t seqDelta(uint16_t a, uint16_t b) {
  return (a <= b) ? (b - a) : (uint16_t)(65536 - a + b);
}

void updateDevice(uint32_t fp, const uint8_t *mac, uint16_t seq, int8_t rssi) {
  uint32_t now = millis();
  Device *d = NULL;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].fingerprint == fp && devices[i].firstSeen != 0) { d = &devices[i]; break; }
  }
  bool isNew = false;
  if (d == NULL) {
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (devices[i].firstSeen == 0) { d = &devices[i]; break; }
    }
    if (d == NULL) return; // table full
    memset(d, 0, sizeof(Device));
    d->fingerprint = fp;
    d->firstSeen = now;
    memcpy(d->mac, mac, 6);
    d->macCount = 1;
    isNew = true;
  } else if (memcmp(d->mac, mac, 6) != 0) {
    if (d->macCount > 0 && seqDelta(d->seq, seq) <= SEQ_CONTIGUOUS_DELTA) {
      d->seqContiguous = true; // same radio confirmed despite MAC rotation
    }
    d->macCount++;
    memcpy(d->mac, mac, 6);
  }
  d->rssi = rssi;
  d->seq = seq;
  d->lastSeen = now;
  if (isNew) {
    d->pendingNew = true; // one alert per fingerprint (until reboot)
  }
  uint32_t span = now - d->firstSeen;
  bool loiter = (d->macCount >= LOITER_MAC_COUNT) || (span > LOITER_SPAN_MS);
  if (loiter && !d->loiterReported) {
    d->loiterReported = true;
    d->pendingLoiter = true;
  }
}

void pruneDevices() {
  uint32_t now = millis();
  uint32_t cutoff = (now > DEVICE_WINDOW_MS) ? now - DEVICE_WINDOW_MS : 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (devices[i].firstSeen != 0 && devices[i].lastSeen < cutoff) {
      memset(&devices[i], 0, sizeof(Device));
    }
  }
}

// ---------------- radio callback ----------------

void IRAM_ATTR snifferCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
  uint32_t frameLen = pkt->rx_ctrl.sig_len;
  if (frameLen < 24) return;

  const uint8_t *frame = pkt->payload;
  uint8_t frameCtrl = frame[0];
  uint8_t frameType = (frameCtrl >> 2) & 0x03;
  uint8_t subtype = (frameCtrl >> 4) & 0x0F;

  bool isProbe = (frameType == 0 && subtype == 0x04);
  bool isNull = (frameType == 2 && (subtype == 0x04 || subtype == 0x05));
  if (!isProbe && !isNull) return;

  const uint8_t *sa = &frame[10];
  uint16_t seq = (frame[22] | (frame[23] << 8)) >> 4;
  int8_t rssi = pkt->rx_ctrl.rssi;

  addProbe(sa, rssi);

  if (isProbe) {
    uint32_t fp = computeFingerprint(frame, frameLen);
    if (fp == 0) fp = 1; // avoid reserved value
    updateDevice(fp, sa, seq, rssi);
  }
}

// ---------------- publishing ----------------

// Bounds-safe append: never writes past the buffer, returns the new position.
int appendFmt(char *buf, int cap, int pos, const char *fmt, ...) {
  if (pos >= cap) return cap;
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(buf + pos, cap - pos, fmt, args);
  va_end(args);
  if (n < 0) return pos;
  return (n > cap - pos) ? cap : pos + n;
}

int appendMac(char *buf, int cap, int pos, const uint8_t *mac) {
  return appendFmt(buf, cap, pos, "%02x:%02x:%02x:%02x:%02x:%02x",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void publishProbes() {
  char buf[4096];
  int len = 0;
  uint32_t now = millis();
  uint32_t cutoff = (now > SEEN_WINDOW_MS) ? now - SEEN_WINDOW_MS : 0;

  len = appendFmt(buf, sizeof(buf), len, "{\"devices\":[");
  bool first = true;
  for (int i = 0; i < MAX_PROBES; i++) {
    if (probes[i].lastSeen != 0 && probes[i].lastSeen >= cutoff) {
      if (!first) len = appendFmt(buf, sizeof(buf), len, ",");
      first = false;
      len = appendFmt(buf, sizeof(buf), len, "{\"mac\":\"");
      len = appendMac(buf, sizeof(buf), len, probes[i].mac);
      len = appendFmt(buf, sizeof(buf), len, "\",\"rssi\":%d}", probes[i].rssi);
    }
  }
  len = appendFmt(buf, sizeof(buf), len, "]}");
  mqtt.publish(MQTT_TOPIC_PROBES, buf, len);
}

void publishDevices() {
  char buf[4096];
  int len = 0;
  uint32_t now = millis();
  uint32_t cutoff = (now > DEVICE_WINDOW_MS) ? now - DEVICE_WINDOW_MS : 0;

  len = appendFmt(buf, sizeof(buf), len, "{\"devices\":[");
  bool first = true;
  for (int i = 0; i < MAX_DEVICES; i++) {
    Device &d = devices[i];
    if (d.firstSeen != 0 && d.lastSeen >= cutoff) {
      if (!first) len = appendFmt(buf, sizeof(buf), len, ",");
      first = false;
      len = appendFmt(buf, sizeof(buf), len, "{\"fingerprint\":\"%08lx\",\"mac\":\"",
                      (unsigned long)d.fingerprint);
      len = appendMac(buf, sizeof(buf), len, d.mac);
      len = appendFmt(buf, sizeof(buf), len,
                      "\",\"rssi\":%d,\"macs\":%d,\"span\":%lu}",
                      d.rssi, d.macCount, (unsigned long)(d.lastSeen - d.firstSeen) / 1000);
    }
  }
  len = appendFmt(buf, sizeof(buf), len, "]}");
  mqtt.publish(MQTT_TOPIC_DEVICES, buf, len);
}

bool publishNew(Device *d) {
  char buf[256];
  int len = appendFmt(buf, sizeof(buf), 0, "{\"fingerprint\":\"%08lx\",\"mac\":\"",
                      (unsigned long)d->fingerprint);
  len = appendMac(buf, sizeof(buf), len, d->mac);
  len = appendFmt(buf, sizeof(buf), len, "\",\"rssi\":%d}", d->rssi);
  return mqtt.publish(MQTT_TOPIC_NEW, buf, len);
}

bool publishLoiter(Device *d) {
  char buf[512];
  int len = appendFmt(buf, sizeof(buf), 0, "{\"fingerprint\":\"%08lx\",\"mac\":\"",
                      (unsigned long)d->fingerprint);
  len = appendMac(buf, sizeof(buf), len, d->mac);
  len = appendFmt(buf, sizeof(buf), len,
                  "\",\"rssi\":%d,\"macs\":%d,\"span\":%lu,\"seq_contiguous\":%s}",
                  d->rssi, d->macCount,
                  (unsigned long)(d->lastSeen - d->firstSeen) / 1000,
                  d->seqContiguous ? "true" : "false");
  return mqtt.publish(MQTT_TOPIC_LOITER, buf, len);
}

// ---------------- setup / loop ----------------

void connectMQTT() {
  while (!mqtt.connected()) {
    if (mqtt.connect("wifi-sniffer")) {
      Serial.println("MQTT connected");
    } else {
      Serial.print("MQTT connect failed, rc=");
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");

  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCb);

  Serial.println("Sniffing started");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }
  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  uint32_t now = millis();

  for (int i = 0; i < MAX_DEVICES; i++) {
    Device &d = devices[i];
    if (d.firstSeen == 0) continue;
    if (d.pendingNew) {
      d.pendingNew = false;
      if (now - lastNewPublish >= MIN_ALERTS_GAP_MS) {
        lastNewPublish = now;
        if (!publishNew(&d)) d.pendingNew = true; // retry if MQTT was down
      } else {
        d.pendingNew = true; // retry next loop
      }
    }
    if (d.pendingLoiter) {
      d.pendingLoiter = false;
      if (!publishLoiter(&d)) d.pendingLoiter = true;
    }
  }

  if (now - lastPrune >= PRUNE_INTERVAL_MS) {
    lastPrune = now;
    pruneProbes();
    pruneDevices();
  }
  if (now - lastPublish >= PUBLISH_INTERVAL_MS) {
    lastPublish = now;
    publishProbes();
    publishDevices();
  }
}
