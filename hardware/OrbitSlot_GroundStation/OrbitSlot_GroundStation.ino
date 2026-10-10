/*
  OrbitSlot hardware ground station (ESP32)
  -----------------------------------------
  Mirrors one ground station from the OrbitSlot dashboard over USB serial
  and lets you inject faults by handling the board.

  Parts: ESP32 dev board, SSD1306 128x64 I2C OLED, MPU6050,
         1 green LED + 1 red LED (each with a 220 ohm resistor), 1 buzzer (active or passive).

  Wiring (ESP32 DevKit):
    OLED VCC -> 3V3     OLED GND -> GND    OLED SDA -> GPIO21   OLED SCL -> GPIO22
    MPU  VCC -> 3V3     MPU  GND -> GND    MPU  SDA -> GPIO21   MPU  SCL -> GPIO22
    Green LED long leg (+) -> 220 ohm -> GPIO26 (D26),  short leg (-) -> GND
    Red   LED long leg (+) -> 220 ohm -> GPIO25 (D25),  short leg (-) -> GND
    Buzzer (+) -> GPIO27 (D27),  Buzzer (-) -> GND
  (OLED and MPU6050 share the same I2C bus: OLED at 0x3C, MPU6050 at 0x68.)

  What the LEDs mean:
    GREEN steady         -> downlink in progress (medium or low priority data)
    GREEN fast flicker   -> downlink in progress with HIGH priority data
    GREEN short blink    -> idle but connected to the dashboard (heartbeat every 2 s)
    RED steady           -> station OFFLINE (board turned face-down, or failure in dashboard)
    RED fast blink (3 s) -> a high-priority deadline was just missed
    RED double flash     -> urgent requests are waiting to be delivered
    GREEN + RED alternate slowly -> not connected to the dashboard yet

  What the buzzer does (same as before):
    1 short chirp        -> a downlink starts
    3 beeps              -> new urgent requests arrived
    1 long low tone      -> a high-priority deadline was missed
    2 falling tones      -> station went offline
    3 rising tones       -> station back online
    At power-on: both LEDs light, then 3 beeps (self-test)

  Libraries (Arduino Library Manager):
    Adafruit SSD1306, Adafruit GFX Library, ArduinoJson (v7)

  Protocol: newline-delimited JSON at 115200 baud.
    Dashboard -> board: {"t":"10:42","st":"Svalbard","sch":"OrbitSlot","m":"LINK|IDLE|OFF",
                         "sat":"SAT-03","pri":0,"pct":62,"next":"SAT-07 10:55","el":42,"urg":3,"miss":1}
    Board -> dashboard: {"ev":"hello"} {"ev":"imu","el":38} {"ev":"down"} {"ev":"up"} {"ev":"shake"}

  Gestures:
    Board face-down for 1.2 s  -> station knocked offline (dashboard replans)
    Board face-up for 0.8 s    -> station restored
    Sharp shake                -> 6 urgent requests sent to the dashboard
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>

#define GREEN_LED   26
#define RED_LED     25
#define BUZZER_PIN  27
#define MPU_ADDR    0x68
#define OLED_ADDR   0x3C

Adafruit_SSD1306 oled(128, 64, &Wire, -1);

struct State {
  char t[8]     = "--:--";
  char st[20]   = "Waiting";
  char sch[16]  = "OrbitSlot";
  char mode[6]  = "WAIT";
  char sat[16]  = "";
  int  pri      = -1;
  int  pct      = 0;
  char next[24] = "";
  int  el       = -1;
  int  urg      = 0;
  int  miss     = 0;
} S;

String line;
unsigned long lastRx = 0, lastImuSend = 0, lastDraw = 0, lastShake = 0;
unsigned long faceDownSince = 0, faceUpSince = 0, missFlashUntil = 0;
bool reportedDown = false, mpuOk = false, firstMsg = true;
int prevUrg = 0, prevMiss = 0;
char prevMode[6] = "WAIT";
float ax = 0, ay = 0, az = 1;
int antEl = 90;

/* ---------------- MPU6050 (raw registers, no extra library) ---------------- */
bool mpuInit() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); Wire.write(0x00);           // wake up
  if (Wire.endTransmission() != 0) return false;
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C); Wire.write(0x00);           // accelerometer +-2 g
  return Wire.endTransmission() == 0;
}

bool mpuRead() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, 6) < 6) return false;
  int16_t x = (Wire.read() << 8) | Wire.read();
  int16_t y = (Wire.read() << 8) | Wire.read();
  int16_t z = (Wire.read() << 8) | Wire.read();
  ax = x / 16384.0; ay = y / 16384.0; az = z / 16384.0;
  return true;
}

/* ---------------- Buzzer patterns ---------------- */
void beep(int freq, int ms) { tone(BUZZER_PIN, freq, ms); delay(ms + 30); }
void chirpLink()   { beep(2200, 50); }
void alarmUrgent() { for (int i = 0; i < 3; i++) beep(2600, 90); }
void alarmMiss()   { beep(420, 450); }
void alarmFail()   { beep(500, 180); beep(350, 300); }
void chirpRestore(){ beep(1200, 70); beep(1800, 70); beep(2400, 90); }

/* ---------------- Serial input from the dashboard ---------------- */
void handleMessage(const String &msg) {
  JsonDocument doc;
  if (deserializeJson(doc, msg)) return;
  strlcpy(S.t,    doc["t"]    | "--:--",     sizeof(S.t));
  strlcpy(S.st,   doc["st"]   | "?",         sizeof(S.st));
  strlcpy(S.sch,  doc["sch"]  | "OrbitSlot", sizeof(S.sch));
  strlcpy(S.mode, doc["m"]    | "IDLE",      sizeof(S.mode));
  strlcpy(S.sat,  doc["sat"]  | "",          sizeof(S.sat));
  strlcpy(S.next, doc["next"] | "",          sizeof(S.next));
  S.pri  = doc["pri"]  | -1;
  S.pct  = doc["pct"]  | 0;
  S.el   = doc["el"]   | -1;
  S.urg  = doc["urg"]  | 0;
  S.miss = doc["miss"] | 0;
  lastRx = millis();

  if (firstMsg) {                       // don't alarm on the first snapshot
    firstMsg = false; prevUrg = S.urg; prevMiss = S.miss; strlcpy(prevMode, S.mode, sizeof(prevMode));
    return;
  }
  if (strcmp(S.mode, prevMode) != 0) {
    if (!strcmp(S.mode, "LINK")) chirpLink();
    else if (!strcmp(S.mode, "OFF")) alarmFail();
    else if (!strcmp(prevMode, "OFF")) chirpRestore();
    strlcpy(prevMode, S.mode, sizeof(prevMode));
  }
  if (S.urg > prevUrg) alarmUrgent();
  if (S.miss > prevMiss) { alarmMiss(); missFlashUntil = millis() + 3000; }
  prevUrg = S.urg; prevMiss = S.miss;
}

void readSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { if (line.length()) handleMessage(line); line = ""; }
    else if (c != '\r' && line.length() < 512) line += c;
  }
}

/* ---------------- Gestures from the MPU6050 ---------------- */
void readGestures() {
  if (!mpuOk || !mpuRead()) return;
  unsigned long now = millis();
  float mag = sqrt(ax * ax + ay * ay + az * az);

  // Antenna elevation: board flat and face-up = pointing straight up (90 deg)
  float tilt = acos(constrain(az / max(mag, 0.01f), -1.0f, 1.0f)) * 180.0 / PI;
  antEl = constrain((int)(90 - tilt), 0, 90);

  // Shake: sharp spike well above 1 g
  if (mag > 2.0 && now - lastShake > 3000) {
    lastShake = now;
    Serial.println("{\"ev\":\"shake\"}");
  }

  // Face-down knocks the station offline; face-up restores it
  if (az < -0.6) { if (!faceDownSince) faceDownSince = now; faceUpSince = 0; }
  else if (az > 0.6) { if (!faceUpSince) faceUpSince = now; faceDownSince = 0; }
  else { faceDownSince = 0; faceUpSince = 0; }

  if (!reportedDown && faceDownSince && now - faceDownSince > 1200) {
    reportedDown = true; Serial.println("{\"ev\":\"down\"}");
  }
  if (reportedDown && faceUpSince && now - faceUpSince > 800) {
    reportedDown = false; Serial.println("{\"ev\":\"up\"}");
  }

  if (now - lastImuSend > 250) {
    lastImuSend = now;
    Serial.print("{\"ev\":\"imu\",\"el\":"); Serial.print(antEl); Serial.println("}");
  }
}

/* ---------------- LEDs ---------------- */
void updateLeds() {
  unsigned long now = millis();
  bool linked = !firstMsg && now - lastRx < 3000;
  bool green = false, red = false;

  if (!linked) {
    // Not connected yet: green and red alternate slowly
    bool phase = (now / 700) % 2;
    green = phase; red = !phase;
  } else {
    // GREEN: downlink status
    if (!strcmp(S.mode, "LINK")) green = (S.pri == 0) ? ((now / 120) % 2) : true;
    else if (!strcmp(S.mode, "IDLE")) green = (now % 2000) < 80;      // heartbeat blink

    // RED: problems, in order of importance
    if (now < missFlashUntil) red = (now / 100) % 2;                  // deadline just missed
    else if (!strcmp(S.mode, "OFF")) red = true;                      // station offline
    else if (S.urg > 0) { unsigned long p = now % 2000; red = p < 90 || (p > 220 && p < 310); }  // urgent waiting
  }
  digitalWrite(GREEN_LED, green ? HIGH : LOW);
  digitalWrite(RED_LED, red ? HIGH : LOW);
}

/* ---------------- OLED ---------------- */
void drawScreen() {
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);

  bool linked = millis() - lastRx < 3000 && !firstMsg;
  oled.setCursor(0, 0);  oled.print("OrbitSlot");
  oled.setCursor(68, 0); oled.print(S.t); oled.print(" UTC");
  oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  if (!linked) {
    oled.setCursor(0, 20); oled.print("Waiting for dashboard");
    oled.setCursor(0, 34); oled.print("Open page, press");
    oled.setCursor(0, 44); oled.print("Connect ESP32");
    oled.setCursor(0, 56); oled.print("Antenna EL "); oled.print(antEl);
    oled.display();
    return;
  }

  // Station name + mode badge
  oled.setCursor(0, 14);
  char name[13]; strlcpy(name, S.st, sizeof(name)); oled.print(name);
  int bx = 98;
  if (!strcmp(S.mode, "LINK") || !strcmp(S.mode, "OFF")) {
    oled.fillRect(bx - 2, 13, 32, 10, SSD1306_WHITE);
    oled.setTextColor(SSD1306_BLACK);
  }
  oled.setCursor(bx, 14); oled.print(S.mode);
  oled.setTextColor(SSD1306_WHITE);

  if (!strcmp(S.mode, "LINK")) {
    char sat[8]; strlcpy(sat, S.sat, sizeof(sat));          // fits size-2 text
    oled.setTextSize(2); oled.setCursor(0, 25); oled.print(sat);
    oled.setTextSize(1); oled.setCursor(92, 29);
    oled.print(S.pri == 0 ? "HIGH" : S.pri == 1 ? "MED" : "LOW");
    oled.drawRect(0, 43, 100, 6, SSD1306_WHITE);
    oled.fillRect(0, 43, constrain(S.pct, 0, 100), 6, SSD1306_WHITE);
    oled.setCursor(104, 42); oled.print(S.pct); oled.print("%");
  } else if (!strcmp(S.mode, "OFF")) {
    oled.setTextSize(2); oled.setCursor(0, 25); oled.print("OFFLINE");
    oled.setTextSize(1); oled.setCursor(0, 43); oled.print("Flip board up");
  } else {
    oled.setCursor(0, 27); oled.print("Idle. Next:");
    char nx[22]; strlcpy(nx, S.next, sizeof(nx));
    oled.setCursor(0, 38); oled.print(nx);
  }

  oled.setCursor(0, 55);
  oled.print("EL "); if (S.el < 0) oled.print("--"); else oled.print(S.el);
  oled.print("/"); oled.print(antEl);
  oled.print(" U"); oled.print(S.urg);
  oled.print(" M"); oled.print(S.miss);
  oled.display();
}

/* ---------------- Power-on self-test ---------------- */
void selfTest() {
  digitalWrite(GREEN_LED, HIGH); digitalWrite(RED_LED, HIGH);
  for (int i = 0; i < 3; i++) beep(2000, 80);       // 3 beeps
  delay(200);
  digitalWrite(GREEN_LED, LOW); digitalWrite(RED_LED, LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    // OLED missing: keep running so LEDs, buzzer and MPU still work
  }
  oled.clearDisplay(); oled.display();
  mpuOk = mpuInit();
  selfTest();
  Serial.println("{\"ev\":\"hello\"}");
}

void loop() {
  readSerial();
  readGestures();
  updateLeds();
  if (millis() - lastDraw > 100) { lastDraw = millis(); drawScreen(); }
}
