// ===================================================================
//  NANO (BLE version) - a cute OLED desk companion you can carry
//  - No Wi-Fi needed. The phone talks to Nano over Bluetooth LE.
//  - Daytime cute eyes, evening tired eyes, night sleep ("Zzz")
//  - ONE TAP    : clock ("Assalamu Alaikum" style clock + date)
//  - DOUBLE TAP : change the eye style
//  - HOLD       : pet Nano -> happy eyes, hearts, cute words
//  - Idle fun   : occasional wink / yawn / heart / cross-eye / roll
//  - Every N min: "DRINK WATER" animation (20 s)
//  - 26 Nov     : birthday mode all day
//
//  PHONE -> NANO COMMANDS (write text to the one BLE characteristic):
//    T|<utc epoch seconds>|<tz offset seconds>    set the clock
//    N|AppName|full message                       show a notification
//    S|name|HH:MM start|HH:MM end|water minutes  change settings
//    M|title|artist|PLAYING/PAUSED/STOPPED        music metadata
//    W                                            test the water screen
//    E|<0-3>                                      eye style (normal/happy/surprised/angry)
//    F                                            find me: flash the screen
//  NANO -> PHONE (BLE notify on the same characteristic):
//    C|NEXT, C|PREV, C|PLAYPAUSE                  music controls
//    H|WATER                                      water reminder tapped away
//    E|<0-3>                                      eye style changed by double tap
//
//  WIRING (unchanged):
//    OLED VCC->3V3 GND->GND SCK->P22 SDA->P21
//    TTP223 VCC->3V3/5V GND->GND SIG->P4
// ===================================================================

#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <sys/time.h>
#include <time.h>

// ====== BLE IDs (the Android app must use the same two) ======
#define SERVICE_UUID "7a1c0001-5e0d-4b8e-9f3a-2d6c1b4a0001"
#define CHAR_UUID    "7a1c0002-5e0d-4b8e-9f3a-2d6c1b4a0001"

// ---- settings (defaults; changed from the phone, saved to flash) ----
String        buddyName    = "Nano";
int           actStart     = 9 * 60;        // 09:00 in minutes
int           actEnd       = 17 * 60 + 30;  // 17:30
int           waterMin     = 20;            // minutes between water nags
unsigned long waterIntervalMs = 20UL * 60UL * 1000UL;

// fixed
const int SLEEP_START = 23 * 60;   // 23:00
const int SLEEP_END   = 7 * 60;    // 07:00
const int BDAY_MONTH  = 11;        // 26 November
const int BDAY_DAY    = 26;

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
#define OLED_ADDR     0x3C
#define TOUCH_PIN     4
const unsigned long DOUBLE_TAP_MS = 400;
const unsigned long HOLD_MS       = 600;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Preferences prefs;

enum Mode { EYES, CLOCK, WATER, NOTIFY, NOTIFYFULL, MUSIC, FIND };
Mode mode = EYES;
unsigned long modeStart = 0;
String notifyApp = "", notifyFull = "";

// Compact music metadata. The OLED shows at most 3 title words and 2 artist words.
String musicTitle = "", musicArtist = "", musicState = "STOPPED";
unsigned long musicPausedAt = 0;
const unsigned long MUSIC_PAUSE_TIMEOUT_MS = 30000;   // paused music screen -> eyes
const unsigned long FIND_MS = 8000;

enum Mood { NORMAL, HAPPY, ANGRY, SURPRISED, TIRED, SLEEP };
int mood = NORMAL;
int activeMood = NORMAL;            // cute-hours expression; double tap changes it
const int MOOD_ORDER[] = { NORMAL, HAPPY, SURPRISED, ANGRY };   // index = eye style sent by the phone
int moodPick = 0;

float openL = 1, openR = 1, openCur = 1.0;
unsigned long nextBlink = 0, blinkStart = 0;
bool blinking = false;
float lookX = 0, lookY = 0, lookTX = 0, lookTY = 0;
unsigned long nextLook = 0;

enum Special { SP_NONE, SP_CROSS, SP_ROLL, SP_WINK, SP_YAWN, SP_HEART };
int special = SP_NONE;
unsigned long specialEnd = 0, specialStart = 0;
float rollAngle = 0;

// touch handling
bool lastTouch = false, pendingTap = false, petActive = false;
unsigned long lastTapTime = 0, pressStart = 0, petWordNext = 0;
String petWord = "";

unsigned long lastWater = 0;

// ---- BLE state ----
volatile bool phoneConnected = false;
bool wasConnected = false;
#define CMD_Q_SIZE 16                 // phone sends T, S, E and M together on connect
String cmdQ[CMD_Q_SIZE];              // queue: BLE task -> loop() (one producer, one consumer)
volatile int qHead = 0, qTail = 0;
BLECharacteristic* nanoCharacteristic = nullptr;
bool musicLongTriggered = false;

void pushCmd(const String &s) {
  int n = (qHead + 1) % CMD_Q_SIZE;
  if (n == qTail) return;             // queue full, drop
  cmdQ[qHead] = s;
  qHead = n;
}

const char* CUTE[] = {
  "You got this!", "Hi friend!", "Stay hydrated!",
  "You're awesome!", "Pet me more!", "Yay! :)", "Hehe~",
  "Best buddy!", "So cozy~"
};
const int CUTE_N = sizeof(CUTE) / sizeof(CUTE[0]);

// ===============================================================
void applyWater() { waterIntervalMs = (unsigned long)waterMin * 60000UL; }

void loadSettings() {
  prefs.begin("nano", false);
  buddyName = prefs.getString("name", "Nano");
  actStart  = prefs.getInt("as", 9 * 60);
  actEnd    = prefs.getInt("ae", 17 * 60 + 30);
  waterMin  = prefs.getInt("wm", 20);
  if (buddyName.length() == 0) buddyName = "Nano";
  if (actStart < 0 || actStart >= 24 * 60) actStart = 9 * 60;
  if (actEnd < 0 || actEnd >= 24 * 60) actEnd = 17 * 60 + 30;
  if (waterMin < 1 || waterMin > 240) waterMin = 20;
  applyWater();
}

// The phone resends settings on every connect; only write flash when they change.
void saveSettings() {
  if (prefs.getString("name", "") != buddyName) prefs.putString("name", buddyName);
  if (prefs.getInt("as", -1) != actStart) prefs.putInt("as", actStart);
  if (prefs.getInt("ae", -1) != actEnd) prefs.putInt("ae", actEnd);
  if (prefs.getInt("wm", -1) != waterMin) prefs.putInt("wm", waterMin);
}

void drawHeart(int cx, int cy, int s) {
  display.fillCircle(cx - s / 2, cy, s / 2, SSD1306_WHITE);
  display.fillCircle(cx + s / 2, cy, s / 2, SSD1306_WHITE);
  display.fillTriangle(cx - s, cy, cx + s, cy, cx, cy + s + s / 2, SSD1306_WHITE);
}

// ===============================================================
//  BLE
// ===============================================================
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) { phoneConnected = true; }
  void onDisconnect(BLEServer* s) {
    phoneConnected = false;
    BLEDevice::startAdvertising();    // be findable again right away
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) {
    auto v = c->getValue();
    String s;
    for (size_t i = 0; i < v.length(); i++) s += (char)v[i];
    if (s.length()) pushCmd(s);
  }
};

void startBLE() {
  BLEDevice::init("Nano");
  BLEDevice::setMTU(517);             // room for a full notification in one write
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService* svc = server->createService(SERVICE_UUID);
  nanoCharacteristic = svc->createCharacteristic(
      CHAR_UUID,
      BLECharacteristic::PROPERTY_WRITE |
      BLECharacteristic::PROPERTY_WRITE_NR |
      BLECharacteristic::PROPERTY_NOTIFY);
  nanoCharacteristic->addDescriptor(new BLE2902());
  nanoCharacteristic->setCallbacks(new RxCB());
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ===============================================================
void bootScreen() {
  for (int frame = 0; frame < 3; frame++) {
    display.clearDisplay();
    float op = (frame == 1) ? 0.15 : 1.0;
    int h = (int)(28 * op); if (h < 3) h = 3;
    display.fillRoundRect(40, 32 - h / 2, 20, h, 6, SSD1306_WHITE);
    display.fillRoundRect(68, 32 - h / 2, 20, h, 6, SSD1306_WHITE);
    display.display();
    delay(300);
  }
  display.clearDisplay();
  display.setTextSize(1); display.setCursor(30, 14); display.print("Hi, I am");
  int size = (buddyName.length() * 18 <= 128) ? 3 : 2;   // 8 letters do not fit at size 3
  display.setTextSize(size);
  int w = buddyName.length() * 6 * size;
  display.setCursor(max(0, (128 - w) / 2), 32);
  display.print(buddyName);
  display.display();
  delay(2000);
}

// ===============================================================
// Keep only the first few words; do not scroll long song names on the OLED.
String compactWords(String input, int maxWords, int maxChars) {
  input.replace("\r", " ");
  input.replace("\n", " ");
  input.trim();
  String out;
  int words = 0;
  bool inWord = false;
  for (unsigned int i = 0; i < input.length(); ++i) {
    char ch = input.charAt(i);
    bool space = (ch == ' ' || ch == '\t');
    if (space) {
      if (inWord) { inWord = false; }
      if (out.length() && out.charAt(out.length() - 1) != ' ') out += ' ';
      continue;
    }
    if (!inWord) {
      while (out.length() && out.charAt(out.length() - 1) == ' ') out.remove(out.length() - 1);
      if (words >= maxWords) break;
      if (out.length()) out += ' ';
      ++words;
      inWord = true;
    }
    if ((int)out.length() >= maxChars) break;
    out += ch;
  }
  out.trim();
  return out;
}

// The OLED font only has ASCII; drop any other UTF-8 bytes so they don't
// show up as random symbols.
String asciiOnly(const String &in) {
  String out;
  out.reserve(in.length());
  for (unsigned int i = 0; i < in.length(); ++i) {
    unsigned char ch = (unsigned char)in.charAt(i);
    if (ch >= 32 && ch < 127) out += (char)ch;
    else if (ch == '\t') out += ' ';
  }
  return out;
}

// Raw bytes work with both the 2.x (std::string) and 3.x (String) BLE libraries.
void sendToPhone(const String &payload) {
  if (!phoneConnected || nanoCharacteristic == nullptr) return;
  nanoCharacteristic->setValue((uint8_t*)payload.c_str(), payload.length());
  nanoCharacteristic->notify();
  Serial.print("BLE notify: ");
  Serial.println(payload);
}

void sendMediaControl(const char* action) {
  sendToPhone(String("C|") + action);
}

void drawMusic(unsigned long now) {
  display.clearDisplay();

  // Tiny playback indicator; no verbose labels or scrolling text.
  if (musicState == "PLAYING") {
    display.fillTriangle(7, 5, 7, 15, 15, 10, SSD1306_WHITE);
  } else {
    display.drawRect(7, 5, 3, 10, SSD1306_WHITE);
    display.drawRect(13, 5, 3, 10, SSD1306_WHITE);
  }
  display.drawFastHLine(22, 10, 98, SSD1306_WHITE);

  // Title: maximum 3 words. Artist: maximum 2 words.
  display.setTextSize(1);
  display.setCursor(4, 21);
  display.print(musicTitle.length() ? musicTitle : "No track info");
  display.setCursor(4, 35);
  if (musicArtist.length()) display.print(musicArtist);

  // Lightweight animated equalizer, drawn only while playing.
  const int baseY = 61;
  const int barX[] = {75, 84, 93, 102, 111, 120};
  for (int i = 0; i < 6; ++i) {
    int h;
    if (musicState == "PLAYING") {
      h = 3 + ((now / 95 + i * 3) % 9);
    } else {
      h = 3;
    }
    display.fillRect(barX[i], baseY - h, 4, h, SSD1306_WHITE);
  }
  display.display();
}

// ===============================================================
void setup() {
  Serial.begin(115200);
  pinMode(TOUCH_PIN, INPUT);
  randomSeed(esp_random());
  loadSettings();

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found!");
    while (true) delay(1000);
  }
  display.setTextColor(SSD1306_WHITE);
  bootScreen();

  startBLE();

  display.clearDisplay(); display.setTextSize(1);
  display.setCursor(16, 28); display.print("Waiting for phone");
  display.display();
  delay(1500);

  unsigned long now = millis();
  lastWater = now;
  nextBlink = now + random(2000, 5000);
  nextLook  = now + random(1500, 3500);
}

// ===============================================================
// Time comes from the phone (command T). Once set, the ESP32 keeps counting.
bool getNow(struct tm &ti) {
  time_t t = time(nullptr);
  if (t < 1577836800L) return false;          // not set yet (before 2020)
  localtime_r(&t, &ti);                        // phone already added the tz offset
  return true;
}
int minutesNow() { struct tm ti; if (!getNow(ti)) return -1; return ti.tm_hour * 60 + ti.tm_min; }
bool isBirthday() { struct tm ti; if (!getNow(ti)) return false; return (ti.tm_mon + 1 == BDAY_MONTH) && (ti.tm_mday == BDAY_DAY); }
bool isSleepHours() { int m = minutesNow(); if (m < 0) return false; return (m >= SLEEP_START || m < SLEEP_END); }
bool activeHours() {
  int m = minutesNow(); if (m < 0) return true;
  if (actStart <= actEnd) return (m >= actStart && m < actEnd);
  return (m >= actStart || m < actEnd);        // range crosses midnight
}

void setClock(long utc, long tz) {
  struct timeval tv;
  tv.tv_sec = utc + tz;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

int parseHHMM(String s) {
  int c = s.indexOf(':'); if (c < 0) return -1;
  return s.substring(0, c).toInt() * 60 + s.substring(c + 1).toInt();
}

// ===============================================================
void drawEye(int cx, int cy, float open, int m, bool isLeft) {
  int W = 34, H = 38, r = 10;
  if (m == SURPRISED) { W = 32; H = 42; r = 16; }
  if (m == TIRED)     { H = 26; r = 8; }
  int curH = (int)(H * open); if (curH < 3) curH = 3;
  int x = cx - W / 2, y = cy - curH / 2;
  int rr = min(r, curH / 2);
  display.fillRoundRect(x, y, W, curH, rr, SSD1306_WHITE);
  if (open < 0.6) return;
  if (m == HAPPY) {
    display.fillRoundRect(x - 1, y + curH / 2, W + 2, curH / 2 + 2, rr, SSD1306_BLACK);
  } else if (m == ANGRY) {
    if (isLeft) display.fillTriangle(x, y, x + W, y, x + W, y + 14, SSD1306_BLACK);
    else        display.fillTriangle(x, y, x + W, y, x, y + 14, SSD1306_BLACK);
  } else if (m == TIRED) {
    display.fillRect(x - 1, y - 1, W + 2, (int)(curH * 0.42), SSD1306_BLACK);
    if (isLeft) display.fillTriangle(x, y, x + 12, y, x, y + 12, SSD1306_BLACK);
    else        display.fillTriangle(x + W, y, x + W - 12, y, x + W, y + 12, SSD1306_BLACK);
  }
}

// small "No Phone" note at the bottom while Bluetooth is not connected
void phoneBadge() {
  if (phoneConnected) return;
  display.setTextSize(1);
  display.setCursor(40, 56);
  display.print("No Phone");
}

void drawSleeping(unsigned long now) {
  display.clearDisplay();
  display.fillRoundRect(21, 34, 30, 6, 3, SSD1306_WHITE);
  display.fillRoundRect(77, 34, 30, 6, 3, SSD1306_WHITE);
  float t = (now % 3000) / 3000.0;
  for (int i = 0; i < 3; i++) {
    float p = t - i * 0.18; if (p < 0) p += 1.0;
    int zx = 90 + i * 7 + (int)(p * 4);
    int zy = 30 - (int)(p * 22);
    display.setTextSize(i + 1); display.setCursor(zx, zy);
    if (p < 0.85) display.print("z");
  }
  phoneBadge();
  display.display();
}

void chooseMood(unsigned long now) {
  if (isSleepHours()) { mood = SLEEP; return; }
  if (!activeHours()) { mood = TIRED; return; }
  mood = activeMood;
}

void updateEyes(unsigned long now) {
  chooseMood(now);
  if (mood == SLEEP) { drawSleeping(now); return; }
  bool tired = (mood == TIRED);

  // blink
  if (!blinking && now >= nextBlink) { blinking = true; blinkStart = now; }
  if (blinking) {
    unsigned long e = now - blinkStart, dur = tired ? 170 : 110;
    if (e < dur)        openCur = 1.0 - (e / (float)dur) * 0.92;
    else if (e < dur*2) openCur = 0.08 + ((e - dur) / (float)dur) * 0.92;
    else { openCur = 1.0; blinking = false; nextBlink = now + (tired ? random(3500, 7000) : random(2000, 5000)); }
  }
  openL = openR = openCur;

  // start a fun move sometimes
  if (now >= nextLook && special == SP_NONE) {
    if (!tired && random(5) == 0) {
      int r = random(5);
      specialStart = now;
      if      (r == 0) { special = SP_CROSS; specialEnd = now + 1200; }
      else if (r == 1) { special = SP_ROLL;  specialEnd = now + 1100; rollAngle = 0; }
      else if (r == 2) { special = SP_WINK;  specialEnd = now + 700; }
      else if (r == 3) { special = SP_YAWN;  specialEnd = now + 1300; }
      else             { special = SP_HEART; specialEnd = now + 1200; }
      nextLook = now + 1600;
    } else {
      int range = tired ? 5 : 10; int dirs[] = { -range, 0, range };
      lookTX = dirs[random(3)];
      lookTY = tired ? 0 : (random(3) - 1) * 6;
      nextLook = now + (tired ? random(3000, 6000) : random(1300, 3200));
    }
  }
  if (special != SP_NONE && now >= specialEnd) special = SP_NONE;

  // apply the current special
  int cross = 0; bool heart = false;
  if      (special == SP_CROSS) cross = 8;
  else if (special == SP_ROLL)  { rollAngle += 0.35; lookX = 11 * cos(rollAngle); lookY = 8 * sin(rollAngle); }
  else if (special == SP_WINK)  openR = 0.12;
  else if (special == SP_YAWN)  { float p = (now - specialStart) / 1300.0;
                                  float yo = (p < 0.5) ? (1.0 - p * 1.6) : (0.2 + (p - 0.5) * 1.6);
                                  if (yo > 1) yo = 1;
                                  if (yo < 0.15) yo = 0.15;
                                  openL = openR = yo; }
  else if (special == SP_HEART) heart = true;

  if (special != SP_ROLL) { lookX += (lookTX - lookX) * 0.2; lookY += (lookTY - lookY) * 0.2; }

  display.clearDisplay();
  int cy = 32 + (int)lookY;
  drawEye(36 + (int)lookX + cross, cy, openL, mood, true);
  drawEye(92 + (int)lookX - cross, cy, openR, mood, false);
  if (heart) { int s = 4 + (int)(2 * sin((now % 600) / 600.0 * 6.28)); drawHeart(64, 14, s); }
  phoneBadge();
  display.display();
}

// ---- pet (hold) ----
void showPet(unsigned long now) {
  display.clearDisplay();
  drawEye(36, 30, 1.0, HAPPY, true);
  drawEye(92, 30, 1.0, HAPPY, false);
  for (int i = 0; i < 3; i++) {
    int hx = 24 + i * 40;
    int hy = 48 - (int)(((now / 15 + i * 200) % 600) / 600.0 * 38);
    drawHeart(hx, hy, 4);
  }
  display.setTextSize(1);
  display.setCursor((128 - (int)petWord.length() * 6) / 2, 54);
  display.print(petWord);
  display.display();
}

// ---- clock / water / birthday ----
void showClock() {
  display.clearDisplay();
  char hhmm[6] = "--:--", date[16] = "";
  struct tm ti; bool ok = getNow(ti);
  if (ok) {
    strftime(hhmm, sizeof(hhmm), "%H:%M", &ti);
    strftime(date, sizeof(date), "%a %d %b", &ti);
  }
  display.setTextSize(3); display.setCursor((128 - (int)strlen(hhmm) * 18) / 2, 16); display.print(hhmm);
  display.setTextSize(1);
  if (ok) { display.setCursor((128 - (int)strlen(date) * 6) / 2, 46); display.print(date); }
  else { display.setCursor(4, 46); display.print("waiting for phone..."); }
  display.display();
}

void showWater(unsigned long now) {
  float t = (now - modeStart) / 1000.0;
  display.clearDisplay();

  // --- a glass on the left with wavy water + rising bubbles ---
  int gx = 12, gy = 6, gw = 34, gh = 52;
  display.drawRoundRect(gx, gy, gw, gh, 4, SSD1306_WHITE);
  int surface = gy + 18;                       // water line
  for (int x = gx + 2; x < gx + gw - 2; x++) {
    int wy = surface + (int)(2.5 * sin(x * 0.5 + t * 4.0));
    display.drawFastVLine(x, wy, (gy + gh - 2) - wy, SSD1306_WHITE);
  }
  for (int b = 0; b < 3; b++) {                // bubbles (dark dots in the water)
    float bp = fmod(t * 0.6 + b * 0.33, 1.0);
    int bxp = gx + 8 + b * 9;
    int byp = (gy + gh - 4) - (int)(bp * (gh - 24));
    display.fillCircle(bxp, byp, 1, SSD1306_BLACK);
  }

  // --- a droplet falling into the glass ---
  float dp = fmod(t, 1.4) / 1.4;
  int dcx = gx + gw / 2, dy = gy - 10 + (int)(dp * 12);
  display.fillTriangle(dcx - 3, dy, dcx + 3, dy, dcx, dy - 5, SSD1306_WHITE);
  display.fillCircle(dcx, dy + 1, 3, SSD1306_WHITE);

  // --- text on the right ---
  display.setTextSize(2);
  display.setCursor(56, 12); display.print("DRINK");
  display.setCursor(56, 30); display.print("WATER");

  // --- countdown bar (how long the reminder stays) ---
  int total = 20000, rem = total - (int)(now - modeStart);
  if (rem < 0) rem = 0;
  int barX = 56, barW = 66;
  display.drawRect(barX, 52, barW, 6, SSD1306_WHITE);
  display.fillRect(barX, 52, (int)(barW * (rem / (float)total)), 6, SSD1306_WHITE);

  display.display();
}

// which app is this, in a tidy name?
String appFriendly(String n) {
  String l = n; l.toLowerCase();
  if (l.indexOf("whatsapp") >= 0) return "WhatsApp";
  if (l.indexOf("instagram") >= 0) return "Instagram";
  if (l.indexOf("gmail") >= 0 || l.indexOf("mail") >= 0) return "Gmail";
  if (l.indexOf("sms") >= 0 || l.indexOf("message") >= 0 || l.indexOf("mms") >= 0) return "Messages";
  if (l.indexOf("telegram") >= 0) return "Telegram";
  if (l.indexOf("facebook") >= 0) return "Facebook";
  if (l.indexOf("youtube") >= 0) return "YouTube";
  if (l.indexOf("phone") >= 0 || l.indexOf("dialer") >= 0 || l.indexOf("call") >= 0) return "Phone";
  return n;
}

// draw a simple monochrome logo centered at cx,cy (about 34px)
void drawAppLogo(String n, int cx, int cy) {
  String l = n; l.toLowerCase();
  if (l.indexOf("whatsapp") >= 0) {
    display.fillCircle(cx, cy, 17, SSD1306_WHITE);
    display.fillTriangle(cx - 12, cy + 10, cx - 18, cy + 18, cx - 6, cy + 15, SSD1306_WHITE);
    for (int i = 0; i <= 8; i++) display.fillCircle(cx - 6 + i * 12 / 8, cy + 6 - i * 12 / 8, 2, SSD1306_BLACK);
    display.fillCircle(cx - 7, cy + 7, 3, SSD1306_BLACK);
    display.fillCircle(cx + 7, cy - 7, 3, SSD1306_BLACK);
  } else if (l.indexOf("instagram") >= 0) {
    display.drawRoundRect(cx - 16, cy - 16, 32, 32, 9, SSD1306_WHITE);
    display.drawRoundRect(cx - 15, cy - 15, 30, 30, 8, SSD1306_WHITE);
    display.drawCircle(cx, cy, 7, SSD1306_WHITE);
    display.fillCircle(cx + 9, cy - 9, 2, SSD1306_WHITE);
  } else if (l.indexOf("gmail") >= 0 || l.indexOf("mail") >= 0) {
    display.drawRect(cx - 16, cy - 11, 32, 22, SSD1306_WHITE);
    display.drawLine(cx - 16, cy - 11, cx, cy + 2, SSD1306_WHITE);
    display.drawLine(cx + 16, cy - 11, cx, cy + 2, SSD1306_WHITE);
  } else if (l.indexOf("sms") >= 0 || l.indexOf("message") >= 0 || l.indexOf("mms") >= 0) {
    display.drawRoundRect(cx - 16, cy - 13, 32, 22, 6, SSD1306_WHITE);
    display.fillTriangle(cx - 6, cy + 8, cx + 2, cy + 8, cx - 10, cy + 15, SSD1306_WHITE);
    display.fillCircle(cx - 7, cy - 2, 1, SSD1306_WHITE);
    display.fillCircle(cx, cy - 2, 1, SSD1306_WHITE);
    display.fillCircle(cx + 7, cy - 2, 1, SSD1306_WHITE);
  } else if (l.indexOf("telegram") >= 0) {
    display.fillTriangle(cx - 16, cy - 2, cx + 15, cy - 13, cx + 1, cy + 14, SSD1306_WHITE);
    display.drawLine(cx + 15, cy - 13, cx - 3, cy + 4, SSD1306_BLACK);
  } else if (l.indexOf("facebook") >= 0) {
    display.fillRoundRect(cx - 16, cy - 16, 32, 32, 6, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK); display.setTextSize(3);
    display.setCursor(cx - 6, cy - 11); display.print("f");
    display.setTextColor(SSD1306_WHITE);
  } else if (l.indexOf("youtube") >= 0) {
    display.fillRoundRect(cx - 17, cy - 11, 34, 22, 6, SSD1306_WHITE);
    display.fillTriangle(cx - 5, cy - 6, cx - 5, cy + 6, cx + 7, cy, SSD1306_BLACK);
  } else if (l.indexOf("phone") >= 0 || l.indexOf("dialer") >= 0 || l.indexOf("call") >= 0) {
    for (int i = 0; i <= 8; i++) display.fillCircle(cx - 8 + i * 16 / 8, cy - 8 + i * 16 / 8, 3, SSD1306_WHITE);
    display.fillCircle(cx - 9, cy - 9, 4, SSD1306_WHITE);
    display.fillCircle(cx + 9, cy + 9, 4, SSD1306_WHITE);
  } else {                                   // unknown app: a rounded tile with its initial
    display.fillRoundRect(cx - 16, cy - 16, 32, 32, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK); display.setTextSize(3);
    char c = n.length() ? toupper(n.charAt(0)) : '?';
    display.setCursor(cx - 8, cy - 11); display.print(c);
    display.setTextColor(SSD1306_WHITE);
  }
}

// clean notification: app logo + app name, a quick pop-in, no scrolling
bool showNotify(unsigned long now) {
  unsigned long el = now - modeStart;
  display.clearDisplay();
  int cx = 64, cy = 22;

  if (el < 450) {                            // pop-in ring
    int rr = 6 + (int)(el / 450.0 * 26);
    display.drawCircle(cx, cy, rr, SSD1306_WHITE);
  }
  drawAppLogo(notifyApp, cx, cy);

  String fn = appFriendly(notifyApp);
  if (fn.length() > 20) fn = fn.substring(0, 20);
  display.setTextColor(SSD1306_WHITE);
  int tw = fn.length() * 12; int ty = 46;
  display.setTextSize(2);
  if (tw > 124) { display.setTextSize(1); tw = fn.length() * 6; ty = 50; }
  display.setCursor((128 - tw) / 2, ty);
  display.print(fn);

  display.display();
  return el > 5000;                          // show for 5s
}

// full message as small wrapped text (no scrolling)
void drawWrapped(String s, int x, int y, int maxLines) {
  display.setTextSize(1);
  int maxc = 21, line = 0;                    // ~21 chars per line at size 1
  s.trim();
  while (s.length() && line < maxLines) {
    String seg = s;
    if (seg.length() > maxc) {
      int cut = seg.lastIndexOf(' ', maxc);
      if (cut <= 0) cut = maxc;
      seg = s.substring(0, cut);
      s = s.substring(seg.length());
    } else {
      s = "";
    }
    s.trim();
    display.setCursor(x, y + line * 10);
    display.print(seg);
    line++;
  }
}

// tap-to-read: app name header + full message, small text, no scroll
bool showNotifyFull(unsigned long now) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(appFriendly(notifyApp));
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);
  drawWrapped(notifyFull, 0, 14, 5);         // up to 5 lines
  display.display();
  return (now - modeStart) > 12000;          // auto-close after 12s
}

// "find my Nano": big surprised eyes, flashing screen
bool showFind(unsigned long now) {
  unsigned long el = now - modeStart;
  display.clearDisplay();
  drawEye(36, 26, 1.0, SURPRISED, true);
  drawEye(92, 26, 1.0, SURPRISED, false);
  display.setTextSize(1);
  const char* msg = "Here I am!";
  display.setCursor((128 - (int)strlen(msg) * 6) / 2, 54);
  display.print(msg);
  setInverted((el / 300) % 2 == 1);
  display.display();
  if (el > FIND_MS) { setInverted(false); return true; }
  return false;
}

void showBirthday(unsigned long now) {
  float t = (now % 2000) / 2000.0;
  display.clearDisplay();
  for (int i = 0; i < 12; i++) display.drawPixel((i * 37) % 128, (int)((i * 23 + now / 20) % 64), SSD1306_WHITE);
  int bx = 44, by = 42, bw = 40, bh = 16;
  display.fillRoundRect(bx, by, bw, bh, 3, SSD1306_WHITE);
  display.fillRect(bx + 4, by - 6, bw - 8, 8, SSD1306_WHITE);
  for (int c = 0; c < 3; c++) {
    int candx = bx + 10 + c * 10;
    display.drawFastVLine(candx, by - 14, 8, SSD1306_WHITE);
    if (((int)(t * 3) + c) % 2 == 0) display.fillCircle(candx, by - 16, 2, SSD1306_WHITE);
  }
  if (t < 0.6) {
    display.setTextSize(1);
    const char* m1 = "Happy Birthday!";
    display.setCursor((128 - (int)strlen(m1) * 6) / 2, 2); display.print(m1);
    String m2 = buddyName + " loves you";
    display.setCursor((128 - (int)m2.length() * 6) / 2, 12); display.print(m2);
  }
  display.display();
}

// ===============================================================
//  Commands from the phone
// ===============================================================
// Where to go when a temporary screen (clock, notification, water...) ends.
Mode homeMode() {
  return (musicState == "PLAYING") ? MUSIC : EYES;
}

// Music only takes over idle screens, never a notification or reminder.
bool canShowMusic() {
  return mode == EYES || mode == CLOCK || mode == MUSIC;
}

void setEyeStyle(int pick) {
  if (pick < 0 || pick > 3) return;
  moodPick = pick;
  activeMood = MOOD_ORDER[pick];
}

bool screenInverted = false;
void setInverted(bool on) {
  if (on == screenInverted) return;
  screenInverted = on;
  display.invertDisplay(on);
}

void handleCommand(String c) {
  c.trim();
  if (c.length() < 1) return;
  Serial.print("CMD: "); Serial.println(c);
  char type = c.charAt(0);
  String rest = (c.length() > 2) ? c.substring(2) : "";

  if (type == 'T') {                              // T|utc|tz
    int bar = rest.indexOf('|');
    long utc = (bar < 0) ? rest.toInt() : rest.substring(0, bar).toInt();
    long tz  = (bar < 0) ? 19800 : rest.substring(bar + 1).toInt();
    if (utc > 1577836800L) setClock(utc, tz);

  } else if (type == 'N') {                       // N|App|message
    rest = asciiOnly(rest);
    int bar = rest.indexOf('|');
    if (bar >= 0) { notifyApp = rest.substring(0, bar); notifyFull = rest.substring(bar + 1); }
    else          { notifyApp = rest; notifyFull = rest; }
    notifyApp.trim(); notifyFull.trim();
    notifyFull = notifyFull.substring(0, 110);
    mode = NOTIFY; modeStart = millis(); pendingTap = false;

  } else if (type == 'S') {                       // S|name|HH:MM|HH:MM|minutes
    String parts[4]; int n = 0; String r = asciiOnly(rest);
    while (n < 4) {
      int b = r.indexOf('|');
      if (b < 0) { parts[n++] = r; break; }
      parts[n++] = r.substring(0, b);
      r = r.substring(b + 1);
    }
    if (n >= 1) { parts[0].trim(); if (parts[0].length()) buddyName = parts[0].substring(0, 8); }
    if (n >= 2) { int v = parseHHMM(parts[1]); if (v >= 0 && v < 24 * 60) actStart = v; }
    if (n >= 3) { int v = parseHHMM(parts[2]); if (v >= 0 && v < 24 * 60) actEnd = v; }
    if (n >= 4) { int w = parts[3].toInt(); if (w >= 1 && w <= 240) waterMin = w; }
    applyWater();
    saveSettings();

  } else if (type == 'M') {                       // M|title|artist|PLAYING/PAUSED/STOPPED
    String fields[3];
    int n = 0;
    String r = asciiOnly(rest);
    while (n < 3) {
      int b = r.indexOf('|');
      if (b < 0) { fields[n++] = r; break; }
      fields[n++] = r.substring(0, b);
      r = r.substring(b + 1);
    }
    String oldState = musicState;
    if (n >= 1) musicTitle = compactWords(fields[0], 3, 20);
    if (n >= 2) musicArtist = compactWords(fields[1], 2, 18);
    if (n >= 3) { fields[2].trim(); fields[2].toUpperCase(); musicState = fields[2]; }
    if (musicState != "PLAYING" && musicState != "PAUSED") musicState = "STOPPED";

    if (musicState == "STOPPED") {
      if (mode == MUSIC) mode = EYES;
    } else if (musicState == "PAUSED") {
      if (oldState != "PAUSED") musicPausedAt = millis();   // music screen times out later
    } else if (canShowMusic() && mode != MUSIC) {   // PLAYING: show it whenever Nano is idle
      mode = MUSIC;
      modeStart = millis();
      pendingTap = false;
      musicLongTriggered = false;
    }

  } else if (type == 'W') {                       // test water screen
    mode = WATER; modeStart = millis(); pendingTap = false;

  } else if (type == 'E') {                       // E|0..3 eye style
    if (rest.length()) setEyeStyle(rest.toInt());

  } else if (type == 'F') {                       // find my Nano
    mode = FIND; modeStart = millis(); pendingTap = false;
  }
}

// ===============================================================
void onSingleTap() { mode = CLOCK; modeStart = millis(); }
void onDoubleTap() {
  setEyeStyle((moodPick + 1) % 4);
  sendToPhone(String("E|") + moodPick);           // keep the app's picker in sync
}

void loop() {
  unsigned long now = millis();

  // commands that arrived over Bluetooth
  while (qTail != qHead) {
    String c = cmdQ[qTail];
    qTail = (qTail + 1) % CMD_Q_SIZE;
    handleCommand(c);
  }

  // phone went away: its music info is stale now
  bool connectedNow = phoneConnected;
  if (wasConnected && !connectedNow) {
    musicState = "STOPPED";
    if (mode == MUSIC) mode = EYES;
  }
  wasConnected = connectedNow;

  bool touch = digitalRead(TOUCH_PIN);
  bool rising = (touch && !lastTouch);
  bool falling = (!touch && lastTouch);
  if (rising) pressStart = now;

  // ---- MUSIC controls: hold = play/pause ----
  if (mode == MUSIC && touch && !musicLongTriggered &&
      (now - pressStart >= HOLD_MS)) {
    musicLongTriggered = true;
    pendingTap = false;
    sendMediaControl("PLAYPAUSE");
  }

  // ---- HOLD = pet (only from the eyes screen) ----
  if (mode == EYES && touch && !petActive && (now - pressStart >= HOLD_MS)) {
    petActive = true; pendingTap = false;
    petWord = CUTE[random(CUTE_N)]; petWordNext = now + 1500;
  }
  if (petActive) {
    if (!touch) { petActive = false; }           // let go -> back to eyes
    else {
      if (now >= petWordNext) { petWord = CUTE[random(CUTE_N)]; petWordNext = now + 1500; }
      showPet(now); lastTouch = touch; delay(30); return;
    }
  }

  // ---- taps register on release of a SHORT press ----
  if (falling && (now - pressStart < HOLD_MS) && !musicLongTriggered) {
    if (mode == MUSIC) {
      if (pendingTap && (now - lastTapTime <= DOUBLE_TAP_MS)) {
        pendingTap = false;
        sendMediaControl("PREV");
      } else {
        pendingTap = true;
        lastTapTime = now;
      }
    } else if (mode == NOTIFY) { mode = NOTIFYFULL; modeStart = now; pendingTap = false; }
    else if (mode == NOTIFYFULL) { notifyApp = ""; notifyFull = ""; mode = homeMode(); pendingTap = false; }
    else if (mode == WATER) {                     // tapped away = "I drank" -> phone counts it
      lastWater = now; mode = homeMode(); pendingTap = false;
      sendToPhone("H|WATER");
    }
    else if (mode != EYES) { mode = homeMode(); pendingTap = false; }
    else {
      if (pendingTap && (now - lastTapTime <= DOUBLE_TAP_MS)) { pendingTap = false; onDoubleTap(); }
      else { pendingTap = true; lastTapTime = now; }
    }
  }
  if (falling) musicLongTriggered = false;
  if (pendingTap && (now - lastTapTime > DOUBLE_TAP_MS)) {
    pendingTap = false;
    if (mode == MUSIC) sendMediaControl("NEXT");
    else onSingleTap();
  }
  lastTouch = touch;

  // ---- screens ----
  if (mode != FIND) setInverted(false);
  if (mode == MUSIC) {
    if (musicState == "PAUSED" && now - musicPausedAt > MUSIC_PAUSE_TIMEOUT_MS) mode = EYES;
    else { drawMusic(now); delay(35); return; }
  }
  if (mode == CLOCK)  { showClock(); if (now - modeStart > 5000) mode = homeMode(); delay(30); return; }
  if (mode == WATER)  { showWater(now); if (now - modeStart > 20000) { mode = homeMode(); lastWater = now; } delay(30); return; }
  if (mode == NOTIFY)     { if (showNotify(now)) mode = homeMode(); delay(30); return; }
  if (mode == NOTIFYFULL) { if (showNotifyFull(now)) { notifyApp = ""; notifyFull = ""; mode = homeMode(); } delay(30); return; }
  if (mode == FIND)       { if (showFind(now)) { setInverted(false); mode = homeMode(); } delay(30); return; }

  if (isBirthday()) { showBirthday(now); delay(30); return; }
  if (now - lastWater >= waterIntervalMs) {
    // no nagging while Nano is sleepy or asleep; restart the timer instead
    if (activeHours() && !isSleepHours()) { mode = WATER; modeStart = now; }
    else lastWater = now;
  }
  updateEyes(now);
  delay(30);
}
