/*
  ESP32-S3 N16R8 → USB HID touchscreen + local profile editor.
  EXPERIMENTAL INPUT PROTOTYPE, NOT A PUBG-COMPATIBLE CHEAT OR AUTOMATIC FIRE DETECTOR.

  BOOT (GPIO0): one test swipe while released normally.
  GPIO4 -> momentary button -> GND: swipe while held (up to maxTravel or 2 s).
  Wi-Fi AP: S3-TouchProfiles; password: TouchS3demo26; http://192.168.4.1
  ESP32 Arduino core 3.3.12; USBMode=default (USB-OTG TinyUSB).
*/
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include "USB.h"
#include "USBHID.h"

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "This example targets ESP32-S3 only"
#endif
#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0
#error "Use ESP32-S3 USB-OTG (TinyUSB) mode (USBMode=default)"
#endif

static constexpr uint8_t PIN_TEST = 0;
static constexpr uint8_t PIN_TRIGGER = 4;
static constexpr uint16_t HID_MAX = 4095;
static const char *AP_SSID = "S3-TouchProfiles";
static const char *AP_PASSWORD = "TouchS3demo26";
static const char *gunNames[] = {"M416", "AUG", "AKM", "M762"};
static const char *scopeNames[] = {"Red dot", "2x", "3x", "4x", "6x"};
static constexpr int GUNS = 4, SCOPES = 5;

// Illustrative rates, NOT measured for PUBG, Pad 7 Pro, or gyroscope.
uint8_t speeds[GUNS][SCOPES] = {
  { 8, 7, 6, 5, 3 },
  { 7, 6, 5, 4, 3 },
  {12,10, 8, 7, 4 },
  {14,12,10, 8, 5 }
};
uint8_t activeGun = 0, activeScope = 0;
uint16_t originX = 2048, originY = 1800, maxTravel = 800;

Preferences prefs;
WebServer server(80);
USBHID hid;

// A single-finger USB HID digitizer, 7-byte input report (no report ID):
// flags (tip & in range), contact ID, X16LE, Y16LE, contact count.
// Logical coordinates 0..4095 are NOT physical screen pixels.
static const uint8_t touchDescriptor[] = {
  0x05, 0x0D,             // Usage Page (Digitizers)
  0x09, 0x04,             // Usage (Touch Screen)
  0xA1, 0x01,             // Collection (Application)
    0x09, 0x22,           // Usage (Finger)
    0xA1, 0x02,           // Collection (Logical)
      0x09, 0x42,         // Tip Switch
      0x09, 0x32,         // In Range
      0x15, 0x00, 0x25, 0x01, // Logical 0..1
      0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
      0x75, 0x06, 0x95, 0x01, 0x81, 0x03, // padding
      0x09, 0x51,         // Contact ID
      0x15, 0x00, 0x25, 0x01,
      0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
      0x05, 0x01,         // Generic Desktop
      0x09, 0x30,         // X
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x0F,
      0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
      0x09, 0x31,         // Y
      0x16, 0x00, 0x00, 0x26, 0xFF, 0x0F,
      0x75, 0x10, 0x95, 0x01, 0x81, 0x02,
    0xC0,
    0x05, 0x0D, 0x09, 0x54, // Contact Count
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
  0xC0
};

class TouchDevice : public USBHIDDevice {
public:
  TouchDevice() { hid.addDevice(this, sizeof(touchDescriptor)); }
  uint16_t _onGetDescriptor(uint8_t *buffer) override {
    memcpy(buffer, touchDescriptor, sizeof(touchDescriptor));
    return sizeof(touchDescriptor);
  }
  bool send(bool down, uint16_t x, uint16_t y) {
    x = (uint16_t)constrain((int)x, 0, (int)HID_MAX);
    y = (uint16_t)constrain((int)y, 0, (int)HID_MAX);
    uint8_t data[7] = {
      (uint8_t)(down ? 3 : 0), 0,
      (uint8_t)(x & 0xFF), (uint8_t)(x >> 8),
      (uint8_t)(y & 0xFF), (uint8_t)(y >> 8),
      (uint8_t)(down ? 1 : 0)
    };
    return hid.SendReport(0, data, sizeof(data), 20);
  }
};
TouchDevice touch;

void sendReport(bool down, int x, int y) {
  // Don't block the web server indefinitely when Android has not enumerated HID.
  if (hid.ready()) {
    touch.send(down, (uint16_t)constrain(x, 0, 4095),
                       (uint16_t)constrain(y, 0, 4095));
  }
}

void loadSettings() {
  prefs.begin("touchprofiles", false);
  uint8_t saved[GUNS][SCOPES];
  if (prefs.getBytesLength("rates") == sizeof(saved)) {
    prefs.getBytes("rates", saved, sizeof(saved));
    for (int g=0; g<GUNS; ++g)
      for (int s=0; s<SCOPES; ++s)
        speeds[g][s] = (uint8_t)constrain((int)saved[g][s], 0, 60);
  }
  activeGun = (uint8_t)constrain((int)prefs.getUChar("gun", 0), 0, GUNS-1);
  activeScope = (uint8_t)constrain((int)prefs.getUChar("scope", 0), 0, SCOPES-1);
  originX = (uint16_t)constrain((int)prefs.getUShort("x", 2048), 500, 3595);
  originY = (uint16_t)constrain((int)prefs.getUShort("y", 1800), 500, 3095);
  maxTravel = (uint16_t)constrain((int)prefs.getUShort("travel", 800), 100, 1000);
}

String page() {
  String s;
  s.reserve(6800);
  s += F("<!doctype html><html lang='ru'><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>ESP32-S3: Touch Profiles</title><style>"
         "body{font-family:system-ui,sans-serif;max-width:750px;margin:auto;padding:18px;background:#121a25;color:#e7edf8}"
         "label{display:block;margin:12px 0 4px}input,select,button{font:inherit;min-height:42px;border-radius:8px;padding:5px;background:#fff;color:#142031}"
         "input[type=number]{width:65px;max-width:100%;box-sizing:border-box}"
         "table{border-collapse:collapse;width:100%}th,td{padding:6px 3px;border-bottom:1px solid #3a495c;text-align:center}"
         "th:first-child{text-align:left} .wrap{overflow-x:auto}button{background:#a6d4fc;border:0;padding:12px 25px;margin-top:18px}"
         "p,small{color:#b7c9d9} .cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px}"
         "</style><h2>ESP32-S3 USB Touch</h2>"
         "<p>Profiles are stored inside the ESP32-S3. This does not detect the game's fire button.</p>"
         "<form method='POST' action='/save'><div class='cols'><div><label>Active weapon</label><select name='gun'>");
  for (int g=0; g<GUNS; ++g) {
    s += "<option value='"+String(g)+"'"+(g==activeGun ? " selected" : "")+">"+gunNames[g]+"</option>";
  }
  s += F("</select></div><div><label>Active scope</label><select name='scope'>");
  for (int c=0; c<SCOPES; ++c) {
    s += "<option value='"+String(c)+"'"+(c==activeScope ? " selected" : "")+">"+scopeNames[c]+"</option>";
  }
  s += F("</select></div></div><p>Input speed: HID coordinate units per ~20 ms. Zero disables movement.</p><div class='wrap'><table><tr><th>Weapon</th>");
  for (int c=0; c<SCOPES; ++c) s += "<th>"+String(scopeNames[c])+"</th>";
  s += F("</tr>");
  for (int g=0; g<GUNS; ++g) {
    s += "<tr><th>"+String(gunNames[g])+"</th>";
    for (int c=0; c<SCOPES; ++c) {
      s += "<td><input aria-label='"+String(gunNames[g])+" "+scopeNames[c]+"' name='p_"+String(g)+"_"+String(c)+"' type='number' min='0' max='60' value='"+String(speeds[g][c])+"'></td>";
    }
    s += "</tr>";
  }
  s += F("</table></div><div class='cols'>"
         "<div><label>Start X (0..4095)</label><input name='x' type='number' min='500' max='3595' value='");
  s += String(originX);
  s += F("'></div><div><label>Start Y (0..4095)</label><input name='y' type='number' min='500' max='3095' value='");
  s += String(originY);
  s += F("'></div><div><label>Maximum travel</label><input name='travel' type='number' min='100' max='1000' value='");
  s += String(maxTravel);
  s += F("'></div></div><button type='submit'>Save to ESP32-S3</button></form>"
         "<p><small>BOOT = single short test line. GPIO4 button held to GND = experimental continuous line, at most 2 seconds."
         " Test in a drawing app, not in PUBG. External touchscreen recognition is not guaranteed.</small></p></html>");
  return s;
}

int formInt(const String &name, int fallback, int lo, int hi) {
  if (!server.hasArg(name)) return fallback;
  String v = server.arg(name);
  // Reject blank and malformed values instead of silently interpreting as zero.
  if (!v.length()) return fallback;
  for (size_t i=0; i<v.length(); ++i)
    if (!isDigit((unsigned char)v[i])) return fallback;
  return constrain(v.toInt(), lo, hi);
}

void handleSave() {
  activeGun = (uint8_t)formInt("gun", activeGun, 0, GUNS-1);
  activeScope = (uint8_t)formInt("scope", activeScope, 0, SCOPES-1);
  originX = (uint16_t)formInt("x", originX, 500, 3595);
  originY = (uint16_t)formInt("y", originY, 500, 3095);
  maxTravel = (uint16_t)formInt("travel", maxTravel, 100, 1000);
  for (int g=0; g<GUNS; ++g)
    for (int c=0; c<SCOPES; ++c)
      speeds[g][c] = (uint8_t)formInt("p_"+String(g)+"_"+String(c), speeds[g][c], 0, 60);
  prefs.putBytes("rates", speeds, sizeof(speeds));
  prefs.putUChar("gun", activeGun);
  prefs.putUChar("scope", activeScope);
  prefs.putUShort("x", originX);
  prefs.putUShort("y", originY);
  prefs.putUShort("travel", maxTravel);
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "Saved");
}

void singleTestSwipe() {
  const int x=originX, y=originY;
  sendReport(true, x, y);
  delay(35);
  for (int i=1; i<=20; ++i) {
    sendReport(true, x, y + (i*300/20));
    delay(18);
  }
  sendReport(false, x, y+300);
}

bool swiping = false;
bool lockUntilRelease = false;
uint32_t startedAt = 0, lastStep = 0;
int currentY = 0;

void beginSwipe(uint32_t now) {
  swiping = true;
  lockUntilRelease = false;
  startedAt = lastStep = now;
  currentY = originY;
  sendReport(true, originX, currentY);
}

void endSwipe() {
  if (swiping) sendReport(false, originX, currentY);
  swiping = false;
}

void setup() {
  pinMode(PIN_TEST, INPUT_PULLUP);
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  loadSettings();
  hid.begin();
  USB.begin();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  server.on("/", HTTP_GET, [] { server.send(200,"text/html; charset=utf-8", page()); });
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
  const uint32_t now=millis();

  // Single line in a drawing app on BOOT press; avoid retrigger until released.
  static bool testArmed = true;
  if (digitalRead(PIN_TEST)==HIGH) testArmed=true;
  if (testArmed && digitalRead(PIN_TEST)==LOW && !swiping) {
    delay(25);
    if (digitalRead(PIN_TEST)==LOW) {
      testArmed=false;
      singleTestSwipe();
    }
  }

  const bool triggerDown = digitalRead(PIN_TRIGGER)==LOW;
  if (!triggerDown) {
    endSwipe();
    lockUntilRelease=false;
  } else if (!swiping && !lockUntilRelease) {
    beginSwipe(now);
  } else if (swiping && now-lastStep>=20) {
    lastStep=now;
    uint8_t step = speeds[activeGun][activeScope];
    // No synthetic scrolling when a speed is configured as zero.
    currentY += step;
    if (currentY > originY + maxTravel) currentY=originY+maxTravel;
    if (currentY > 4095) currentY=4095;
    if (step) sendReport(true, originX, currentY);
  }
  if (swiping && (now-startedAt>=2000 || currentY >= originY + maxTravel)) {
    endSwipe();
    lockUntilRelease=true;
  }
  delay(2);
}
