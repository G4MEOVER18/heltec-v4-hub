// G4MEOVER Heltec WiFi LoRa 32 V4 — Flipper-Hub + interaktives Menue
// ---------------------------------------------------------------------------
// Der Flipper steuert dieses Board per GPIO-UART (Pin 13/14 -> GPIO47/48).
// ukfe_rf-Befehle werden verifiziert und per ESP-NOW an die Satelliten gefunkt
// (Hub); Satelliten-ACKs gehen zurueck an den Flipper. Zusaetzlich: freies
// LoRa-Terminal (SX1262), GPS (GNSS-Stecker) und ein Mehrseiten-OLED-Menue,
// bedient mit dem einen Boot-Button (GPIO0):
//   kurz = weiter/scrollen · lang = auswaehlen · 2x schnell = naechste Seite
//
// Nur fuer autorisierte Sicherheitstests auf eigenen Geraeten.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <SPI.h>
#include <U8g2lib.h>
#include <TinyGPS++.h>
#include <RadioLib.h>

extern "C" {
#include "ukfe_rf.h"
}

// ---- V4-Pins (kompatibel zu V3) ----
#define PIN_OLED_SDA   17
#define PIN_OLED_SCL   18
#define PIN_OLED_RST   21
#define PIN_VEXT       36
#define PIN_LED        35
#define PIN_BTN        0     // Boot/PRG-Taste (USER_SW), active-low
// SX1262 LoRa
#define PIN_LORA_NSS   8
#define PIN_LORA_SCK   9
#define PIN_LORA_MOSI  10
#define PIN_LORA_MISO  11
#define PIN_LORA_RST   12
#define PIN_LORA_BUSY  13
#define PIN_LORA_DIO1  14
// GPS / GNSS-Stecker (Serial2)
#define PIN_GPS_RX     39
#define PIN_GPS_TX     38
#define PIN_GPS_PPS    41
#define PIN_VGNSS_CTRL 34

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t UKFE_SECRET[UKFE_RF_SECRET_LEN] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
};

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, PIN_OLED_RST, PIN_OLED_SCL, PIN_OLED_SDA);
TinyGPSPlus gps;
HardwareSerial FlipperSerial(1);
HardwareSerial GpsSerial(2);
SPIClass loraSpi(HSPI);
SX1262 radio = new Module(PIN_LORA_NSS, PIN_LORA_DIO1, PIN_LORA_RST, PIN_LORA_BUSY, loraSpi);

// ---- Hub-Zustand ----
static uint32_t s_flipperCounter = 0;
static uint32_t s_rxFromFlipper = 0, s_relayed = 0, s_rejected = 0, s_rawBytes = 0;
static uint8_t  s_lastCmd = 0;
static uint32_t s_ackFromSat = 0, s_txCounter = 100;
static uint8_t  s_lastAckCmd = 0;
static volatile bool s_ackFlag = false;
static uint8_t  s_ackBuf[UKFE_RF_MAX_FRAME];
static volatile int s_ackLen = 0;
static uint8_t  s_buf[128];
static size_t   s_len = 0;

// ---- LoRa-Zustand ----
static bool     s_loraOk = false, s_loraRxOn = false;
static float    s_loraFreq = 868.0f;
static int      s_loraSf = 7;
static uint32_t s_loraTx = 0, s_loraRx = 0;
static float    s_loraLastRssi = 0;
static char     s_loraLast[24] = "";
static volatile bool s_loraFlag = false;
ICACHE_RAM_ATTR void onLoraDio1() { s_loraFlag = true; }

// ---- Menue-Zustand ----
enum Page { PG_STATUS, PG_GPS, PG_LORA, PG_SATS, PG_INFO, PG_COUNT };
static int s_page = PG_STATUS;
static int s_sel  = 0;               // Auswahl-Cursor je Seite
static const int SEL_MAX[PG_COUNT] = {1, 1, 4, 1, 1};  // waehlbare Eintraege/Seite
static char s_toast[22] = "";        // kurze Statusmeldung
static uint32_t s_toastT = 0;

static void toast(const char* t) { snprintf(s_toast, sizeof(s_toast), "%s", t); s_toastT = millis(); }

// ---------------------------------------------------------------------------
// ESP-NOW-Empfang (Satelliten-ACK puffern)
void on_espnow_recv(const uint8_t* mac, const uint8_t* data, int len) {
    (void)mac;
    if(s_ackFlag) return;
    if(len <= 0 || len > (int)UKFE_RF_MAX_FRAME) return;
    memcpy(s_ackBuf, data, len); s_ackLen = len; s_ackFlag = true;
}

// Eigenen ukfe_rf-Frame bauen + per ESP-NOW broadcasten (fuer Menue-Aktionen)
static void hub_broadcast(uint8_t cmd) {
    UkfeRfMessage m; ukfe_rf_make_simple(&m, (UkfeRfCmd)cmd); m.counter = ++s_txCounter;
    uint8_t f[UKFE_RF_MAX_FRAME];
    size_t n = ukfe_rf_build_frame(UKFE_SECRET, &m, f, sizeof(f));
    if(n && esp_now_send(BROADCAST_MAC, f, n) == ESP_OK) { s_relayed++; toast("Broadcast gesendet"); }
    else toast("Broadcast FAIL");
}

static void relay_frame(const uint8_t* frame, size_t real_len, const UkfeRfMessage* msg) {
    digitalWrite(PIN_LED, HIGH);
    s_lastCmd = msg->cmd;
    if(esp_now_send(BROADCAST_MAC, frame, real_len) == ESP_OK) s_relayed++;
    digitalWrite(PIN_LED, LOW);
}

static void scan_and_relay() {
    while(s_len >= UKFE_RF_HDR_OVERHEAD) {
        size_t i = 0; bool found = false;
        for(; i + 2 < s_len; i++) {
            uint8_t L = s_buf[i]; size_t rl = (size_t)L + 1;
            if(rl < UKFE_RF_HDR_OVERHEAD || rl > UKFE_RF_MAX_FRAME) continue;
            if(s_buf[i+1] != UKFE_RF_MAGIC || s_buf[i+2] != UKFE_RF_VERSION) continue;
            found = true; break;
        }
        if(!found) { if(s_len > 2) { memmove(s_buf, s_buf + (s_len-2), 2); s_len = 2; } return; }
        if(i > 0) { memmove(s_buf, s_buf + i, s_len - i); s_len -= i; }
        size_t real_len = (size_t)s_buf[0] + 1;
        if(s_len < real_len) return;
        UkfeRfMessage msg;
        if(ukfe_rf_parse_frame(UKFE_SECRET, s_buf, real_len, &msg, &s_flipperCounter)) {
            s_rxFromFlipper++; relay_frame(s_buf, real_len, &msg);
        } else s_rejected++;
        memmove(s_buf, s_buf + real_len, s_len - real_len); s_len -= real_len;
    }
}

// ---------------------------------------------------------------------------
// Button: kurz / lang / doppelt (nicht-blockierend)
enum BtnEv { EV_NONE, EV_SHORT, EV_LONG, EV_DOUBLE };
static int      btn_last = HIGH;
static uint32_t btn_press_t = 0, btn_pending_t = 0;
static bool     btn_long_fired = false, btn_pending = false;

static BtnEv poll_button() {
    int b = digitalRead(PIN_BTN);
    uint32_t now = millis();
    BtnEv ev = EV_NONE;
    if(b == LOW && btn_last == HIGH) { btn_press_t = now; btn_long_fired = false; }
    if(b == LOW && !btn_long_fired && (now - btn_press_t) >= 700) {
        ev = EV_LONG; btn_long_fired = true; btn_pending = false;
    }
    if(b == HIGH && btn_last == LOW) {
        uint32_t dur = now - btn_press_t;
        if(!btn_long_fired && dur >= 30) {
            if(btn_pending && (now - btn_pending_t) < 400) { ev = EV_DOUBLE; btn_pending = false; }
            else { btn_pending = true; btn_pending_t = now; }
        }
    }
    if(btn_pending && (now - btn_pending_t) > 400) { btn_pending = false; ev = EV_SHORT; }
    btn_last = b;
    return ev;
}

// ---------------------------------------------------------------------------
// LoRa-Terminal
static void lora_init() {
    loraSpi.begin(PIN_LORA_SCK, PIN_LORA_MISO, PIN_LORA_MOSI, PIN_LORA_NSS);
    int st = radio.begin(s_loraFreq, 125.0, s_loraSf, 5, 0x34, 10, 8);
    radio.setTCXO(1.8);
    radio.setDio2AsRfSwitch(true);
    s_loraOk = (st == RADIOLIB_ERR_NONE);
    if(s_loraOk) radio.setDio1Action(onLoraDio1);
}

static void lora_send_test() {
    if(!s_loraOk) { toast("LoRa nicht init"); return; }
    char msg[32]; snprintf(msg, sizeof(msg), "G4MEOVER LoRa #%lu", (unsigned long)++s_loraTx);
    radio.transmit(msg);
    if(s_loraRxOn) radio.startReceive();
    toast("LoRa TX gesendet");
}

static void lora_toggle_rx() {
    if(!s_loraOk) { toast("LoRa nicht init"); return; }
    s_loraRxOn = !s_loraRxOn;
    if(s_loraRxOn) { radio.startReceive(); toast("LoRa RX an"); }
    else { radio.standby(); toast("LoRa RX aus"); }
}

static void lora_poll() {
    if(!s_loraFlag) return;
    s_loraFlag = false;
    if(!s_loraRxOn) return;
    uint8_t data[64]; int n = radio.readData(data, sizeof(data) - 1);
    if(n >= 0) {
        int ln = radio.getPacketLength();
        s_loraRx++; s_loraLastRssi = radio.getRSSI();
        int cp = ln < 20 ? ln : 20; memcpy(s_loraLast, data, cp); s_loraLast[cp] = 0;
    }
    radio.startReceive();
}

// ---------------------------------------------------------------------------
// Seiten-Rendering
static const char* PAGE_NAME[PG_COUNT] = {"STATUS", "GPS", "LORA", "SATELLIT", "INFO"};

static void draw_header() {
    oled.drawStr(0, 8, "G4MEOVER V4");
    char p[16]; snprintf(p, sizeof(p), "%d/%d %s", s_page + 1, PG_COUNT, PAGE_NAME[s_page]);
    int w = oled.getStrWidth(p);
    oled.drawStr(128 - w, 8, p);
    oled.drawHLine(0, 11, 128);
}

static void row(int y, bool sel, const char* txt) {
    if(sel) { oled.drawBox(0, y - 8, 128, 10); oled.setDrawColor(0); }
    oled.drawStr(2, y, txt);
    if(sel) oled.setDrawColor(1);
}

static void render() {
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    draw_header();
    char l[26];

    if(s_page == PG_STATUS) {
        snprintf(l, sizeof(l), "rx:%lu  relay:%lu", (unsigned long)s_rxFromFlipper, (unsigned long)s_relayed);
        oled.drawStr(2, 24, l);
        snprintf(l, sizeof(l), "ack:%lu  rej:%lu", (unsigned long)s_ackFromSat, (unsigned long)s_rejected);
        oled.drawStr(2, 36, l);
        snprintf(l, sizeof(l), "cmd:0x%02X ESPNOW k%d", s_lastCmd, ESPNOW_CHANNEL);
        oled.drawStr(2, 48, l);
        row(60, s_sel == 0, "> STATUS-Broadcast senden");
    } else if(s_page == PG_GPS) {
        bool fix = gps.location.isValid() && gps.satellites.isValid();
        snprintf(l, sizeof(l), "Sats: %d  %s", fix ? gps.satellites.value() : 0, fix ? "FIX" : "kein Fix");
        oled.drawStr(2, 24, l);
        if(fix) {
            snprintf(l, sizeof(l), "%.5f", gps.location.lat()); oled.drawStr(2, 36, l);
            snprintf(l, sizeof(l), "%.5f", gps.location.lng()); oled.drawStr(2, 48, l);
        } else { oled.drawStr(2, 36, "Modul am 8-Pin-Stecker?"); }
        row(60, s_sel == 0, "> GPS-Power umschalten");
    } else if(s_page == PG_LORA) {
        snprintf(l, sizeof(l), "%.1fMHz SF%d %s", s_loraFreq, s_loraSf, s_loraOk ? (s_loraRxOn ? "RX" : "idle") : "ERR");
        oled.drawStr(2, 22, l);
        snprintf(l, sizeof(l), "TX:%lu RX:%lu %ddBm", (unsigned long)s_loraTx, (unsigned long)s_loraRx, (int)s_loraLastRssi);
        oled.drawStr(2, 32, l);
        row(43, s_sel == 0, "Test-Paket senden");
        row(52, s_sel == 1, s_loraRxOn ? "RX stoppen" : "RX starten");
        char f[24]; snprintf(f, sizeof(f), "Freq %.0f  |  SF %d", s_loraFreq, s_loraSf);
        row(62, s_sel == 2 || s_sel == 3, f);
    } else if(s_page == PG_SATS) {
        snprintf(l, sizeof(l), "ACKs gesamt: %lu", (unsigned long)s_ackFromSat);
        oled.drawStr(2, 26, l);
        snprintf(l, sizeof(l), "letzter resp: 0x%02X", s_lastAckCmd);
        oled.drawStr(2, 38, l);
        oled.drawStr(2, 50, "Broadcast -> alle Sat.");
        row(62, s_sel == 0, "> Ping an Satelliten");
    } else if(s_page == PG_INFO) {
        oled.drawStr(2, 24, WiFi.macAddress().c_str());
        snprintf(l, sizeof(l), "Uptime: %lus", millis() / 1000);
        oled.drawStr(2, 36, l);
        oled.drawStr(2, 48, "Hub+LoRa+GPS+ESP-NOW");
        row(60, s_sel == 0, "> nur autorisierte Tests");
    }

    // Toast (kurze Meldung, 1.5s)
    if(s_toast[0] && millis() - s_toastT < 1500) {
        int w = oled.getStrWidth(s_toast);
        oled.setDrawColor(0); oled.drawBox(64 - w/2 - 2, 14, w + 4, 11); oled.setDrawColor(1);
        oled.drawFrame(64 - w/2 - 2, 14, w + 4, 11);
        oled.drawStr(64 - w/2, 22, s_toast);
    }
    oled.sendBuffer();
}

// Lang-Druck: Aktion der aktuellen Auswahl ausfuehren
static void activate() {
    if(s_page == PG_STATUS && s_sel == 0) hub_broadcast(UkfeRfCmdStatus);
    else if(s_page == PG_GPS && s_sel == 0) {
        digitalWrite(PIN_VGNSS_CTRL, !digitalRead(PIN_VGNSS_CTRL)); toast("GPS-Power getoggelt");
    } else if(s_page == PG_LORA) {
        if(s_sel == 0) lora_send_test();
        else if(s_sel == 1) lora_toggle_rx();
        else if(s_sel == 2) { s_loraFreq += 1.0f; if(s_loraFreq > 928) s_loraFreq = 433; lora_init(); toast("Freq geaendert"); }
        else if(s_sel == 3) { s_loraSf++; if(s_loraSf > 12) s_loraSf = 7; lora_init(); toast("SF geaendert"); }
    } else if(s_page == PG_SATS && s_sel == 0) hub_broadcast(UkfeRfCmdStatus);
    else toast("—");
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
    pinMode(PIN_BTN, INPUT_PULLUP);
    pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);
    pinMode(PIN_VGNSS_CTRL, OUTPUT); digitalWrite(PIN_VGNSS_CTRL, HIGH);
    delay(60);

    oled.begin();
    oled.setFont(u8g2_font_6x10_tf);
    oled.clearBuffer(); oled.drawStr(0, 10, "G4MEOVER V4"); oled.drawStr(0, 26, "Init..."); oled.sendBuffer();

    FlipperSerial.begin(FLIPPER_BAUD, SERIAL_8N1, FLIPPER_RX, FLIPPER_TX);
    GpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    pinMode(PIN_GPS_PPS, INPUT);

    lora_init();  // SX1262 (frei fuers LoRa-Terminal, da Flipper ueber UART kommt)

    WiFi.mode(WIFI_STA); WiFi.disconnect();
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if(esp_now_init() == ESP_OK) {
        esp_now_register_recv_cb(on_espnow_recv);
        esp_now_peer_info_t bp = {}; memcpy(bp.peer_addr, BROADCAST_MAC, 6);
        bp.channel = ESPNOW_CHANNEL; bp.encrypt = false; esp_now_add_peer(&bp);
    }
    Serial.printf("\nG4MEOVER V4 bereit. LoRa %s, ESP-NOW k%d. MAC %s\n",
                  s_loraOk ? "ok" : "ERR", ESPNOW_CHANNEL, WiFi.macAddress().c_str());
    toast("Bereit");
}

static uint32_t s_lastRender = 0;

void loop() {
    // Flipper-UART + USB-CDC -> Frames relayen
    while(FlipperSerial.available() && s_len < sizeof(s_buf)) { s_buf[s_len++] = FlipperSerial.read(); s_rawBytes++; }
    while(Serial.available() && s_len < sizeof(s_buf)) { s_buf[s_len++] = Serial.read(); s_rawBytes++; }
    scan_and_relay();

    // Satelliten-ACK -> an Flipper + USB weiterreichen
    if(s_ackFlag) {
        int len = s_ackLen; uint8_t f[UKFE_RF_MAX_FRAME]; memcpy(f, s_ackBuf, len); s_ackFlag = false;
        size_t rl = (size_t)f[0] + 1; UkfeRfMessage msg;
        if(rl <= (size_t)len && ukfe_rf_parse_frame(UKFE_SECRET, f, rl, &msg, NULL)) {
            s_ackFromSat++; s_lastAckCmd = msg.cmd;
            FlipperSerial.write(f, rl); Serial.write(f, rl);
        }
    }

    while(GpsSerial.available()) gps.encode(GpsSerial.read());
    lora_poll();

    // Button-Gesten
    BtnEv ev = poll_button();
    if(ev == EV_DOUBLE) { s_page = (s_page + 1) % PG_COUNT; s_sel = 0; render(); }
    else if(ev == EV_SHORT) { s_sel = (s_sel + 1) % SEL_MAX[s_page]; render(); }
    else if(ev == EV_LONG) { activate(); render(); }

    // periodisch neu zeichnen (Status/GPS/Toast)
    if(millis() - s_lastRender > 700) { s_lastRender = millis(); render(); }
    delay(2);
}
