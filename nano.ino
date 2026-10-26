// ===================================================================
//  NANO (BLE version) - a cute OLED desk companion you can carry
//  Developed by Mohamed Thahir - https://github.com/thahirsprojects
//  Android app: https://github.com/thahirsprojects/nano-companion-android
//  - No Wi-Fi needed. The phone talks to Nano over Bluetooth LE.
//  - Daytime cute eyes, evening tired eyes, night sleep ("Zzz"), auto-dimmed at night
//  - ONE TAP      : clock (on the clock screen: battery, date)
//  - DOUBLE TAP   : change the eye style
//  - HOLD         : pet Nano -> happy eyes, hearts, cute words
//  - HOLD LONGER  : (from the eyes) toggle local mute, no phone needed
//  - HOLD (clock) : torch -- fills the screen white as a tiny flashlight
//  - Idle fun     : occasional wink / yawn / heart / cross-eye / roll
//  - Every N min  : "DRINK WATER" animation (20 s)
//  - 26 Nov       : birthday mode all day
//  - A dead OLED still sounds an SOS beep pattern so a fault is never silent
//
//  PHONE -> NANO COMMANDS (write text to the one BLE characteristic):
//    T|<utc epoch seconds>|<tz offset seconds>    set the clock
//    N|AppName|sender                             show a notification (who sent it)
//    S|name|HH:MM start|HH:MM end|water minutes  change settings
//    M|title|artist|PLAYING/PAUSED/STOPPED        music metadata
//    W                                            test the water screen
//    E|<0-7>                                      eye style, see MOOD_ORDER
//    F                                            find me: flash the screen
//    R|App|caller                                 incoming call: rings until Q or a tap
//    Q                                            call answered, declined or missed: stop ringing
//    B|on|volume 0-2|flags|ringtone 0-7           sound settings (see SF_* flags, RINGTONES)
//    P|<sound id>, P|r<ringtone>, P|s             preview a sound or a ringtone, stop it
//    Z|<minutes>                                  mute sounds for a while (0 = unmute)
//    D|0/1                                        phone Do Not Disturb (quiet while on)
//    L|<percent>|<charging 0/1>                   phone battery on the clock screen (-1 hides)
//    X|<minutes>                                  start a focus/countdown timer (0 cancels)
//  NANO -> PHONE (BLE notify on the same characteristic):
//    C|NEXT, C|PREV, C|PLAYPAUSE                  music controls
//    H|WATER                                      water reminder tapped away
//    E|<0-7>                                      eye style changed by double tap
//
//  EYE STYLES (E|n / MOOD_ORDER): 0 Normal 1 Happy 2 Wow 3 Angry 4 Buddy
//                                 5 Grin 6 Robot 7 Drowsy
//
//  WIRING:
//    OLED VCC->3V3 GND->GND SCK->P22 SDA->P21
//    TTP223 VCC->3V3/5V GND->GND SIG->P4
//    Passive buzzer (+)->P25 (-)->GND  (add a 220 ohm resistor or an NPN
//    transistor later, then set BUZZER_DIRECT to 0 for full volume)
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
#include <esp_arduino_version.h>
#include "driver/gpio.h"

// a buzzer note; declared up here so the IDE's auto-generated prototypes can use it
struct Note { uint16_t freq; uint16_t ms; };   // freq 0 = rest, ms 0 = end of tune

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

enum Mode { EYES, CLOCK, WATER, NOTIFY, MUSIC, FIND, TIMER };
Mode mode = EYES;
unsigned long modeStart = 0;

// Time on the current screen. modeStart is often set (millis()) after loop() read
// "now", so plain now - modeStart could wrap to a huge number and close the
// screen instantly (the sound played but nothing showed). Clamp to 0 instead.
unsigned long modeElapsed(unsigned long now) {
  return ((long)(now - modeStart) > 0) ? (now - modeStart) : 0;
}
String notifyApp = "", notifyFrom = "";       // app + who sent it (no message text)
bool notifyIsCall = false;             // the notification screen is an incoming call
bool callActive = false;               // a call is ringing (until Q, a tap or disconnect)
unsigned long callStart = 0;
const unsigned long CALL_MAX_MS = 180000;   // safety stop if "call ended" never arrives

// Compact music metadata. The OLED shows at most 3 title words and 2 artist words.
String musicTitle = "", musicArtist = "", musicState = "STOPPED";
unsigned long musicPausedAt = 0;
const unsigned long MUSIC_PAUSE_TIMEOUT_MS = 30000;   // paused music screen -> eyes
const unsigned long FIND_MS = 8000;
unsigned long timerTotalMs = 0;   // focus/countdown timer length (X command)

enum Mood { NORMAL, HAPPY, ANGRY, SURPRISED, TIRED, SLEEP, BUDDY, GRIN, ROBOT, DROWSY };
int mood = NORMAL;
int activeMood = BUDDY;             // cute-hours expression; double tap changes it
// index = eye style sent by the phone (E command / app picker)
const int MOOD_ORDER[] = { NORMAL, HAPPY, SURPRISED, ANGRY, BUDDY, GRIN, ROBOT, DROWSY };
const int MOOD_COUNT = sizeof(MOOD_ORDER) / sizeof(MOOD_ORDER[0]);
int moodPick = 4;                     // Buddy: the 8-bit keychain-buddy face

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

// Holding past the pet threshold toggles mute, without touching the phone.
const unsigned long MUTE_HOLD_MS = 2500;
bool muteToggledThisPress = false;

// Holding the clock screen turns it into a plain white "torch" screen.
const unsigned long TORCH_HOLD_MS = 1200;
bool torchActive = false;

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
//  BUZZER (passive buzzer: + -> P25, - -> GND)
// ===============================================================
#define BUZZER_PIN    25
// 1 = buzzer wired straight to the pin (no resistor/transistor yet): the pin runs
//     at its weakest drive strength and the sound is kept quiet to protect the ESP32.
// 0 = buzzer driven through a 220 ohm resistor or an NPN transistor: full volume.
#define BUZZER_DIRECT 1

enum Sound { SND_NOTIFY, SND_CALL, SND_WATER, SND_FIND, SND_HAPPY, SND_CLICK,
             SND_BOOT, SND_BIRTHDAY, SND_TIMER, SND_ERROR, SND_COUNT };

// sound setting flags (B command), one bit per kind of sound
const int SF_NOTIFY = 1, SF_CALL = 2, SF_WATER = 4, SF_FIND = 8, SF_TOUCH = 16,
          SF_BOOT = 32, SF_BIRTHDAY = 64, SF_QUIET_OFF_HOURS = 128, SF_TIMER = 256;
const int SF_DEFAULT = SF_NOTIFY | SF_CALL | SF_WATER | SF_FIND | SF_TOUCH | SF_BOOT | SF_BIRTHDAY | SF_TIMER;
const int SF_MASK = 0x1FF;   // every flag bit above, for sanitizing the B command

bool soundOn = true;
bool phoneDnd = false;                // phone is in Do Not Disturb (D command)
bool muteTimed = false;               // "mute for N minutes" from the app (Z command)
unsigned long muteUntil = 0;
int  phoneBattery = -1;               // phone battery % for the clock screen (-1 = hidden)
bool phoneCharging = false;
int  soundVolume = 1;                 // 0 low, 1 medium, 2 high
int  soundFlags = SF_DEFAULT;

// ---- ringtones for incoming calls (picked in the app, 5th field of the B command) ----
// Notes stay between ~1 and 3 kHz, where a small passive buzzer is loudest. Each one
// ends with a pause and loops until the call is answered, declined or missed.
#define BELL     {2349, 30}, {1865, 30}            // one cycle of the bell trill
#define BELL7    BELL, BELL, BELL, BELL, BELL, BELL, BELL
#define MAR(f)   {f, 95}, {0, 15}                  // short marimba-style note
#define E8(f)    {f, 115}, {0, 15}                 // eighth note with a tiny gap
#define Q4(f)    {f, 245}, {0, 15}                 // quarter note
#define S16(f)   {f, 125}, {0, 15}                 // sixteenth note (Fur Elise)
#define ODE(f)   {f, 205}, {0, 25}

// 0 Classic bell: old telephone trill
const Note RING_BELL[] = { BELL7, BELL7, {0, 220}, BELL7, BELL7, {0, 1600}, {0, 0} };
// 1 Gran Vals (Nokia-style): Tarrega, 1902, public domain
const Note RING_GRANVALS[] = {
  E8(2637), E8(2349), Q4(1480), Q4(1661),
  E8(2217), E8(1976), Q4(1175), Q4(1319),
  E8(1976), E8(1760), Q4(1109), Q4(1319),
  {1760, 520}, {0, 1000}, {0, 0} };
// 2 Marimba (iPhone-style): original bouncy arpeggios, C - F - G - C
const Note RING_MARIMBA[] = {
  MAR(1568), MAR(2093), MAR(2637), MAR(2093), MAR(1568), MAR(2093), MAR(2637), MAR(3136),
  MAR(1397), MAR(1760), MAR(2093), MAR(1760), MAR(1397), MAR(1760), MAR(2093), MAR(2794),
  MAR(1568), MAR(1976), MAR(2349), MAR(1976), MAR(1568), MAR(1976), MAR(2349), MAR(3136),
  {2093, 250}, {0, 20}, {1568, 110}, {0, 10}, {2093, 330}, {0, 900}, {0, 0} };
// 3 Digital: four beeps at the buzzer's loudest pitch
const Note RING_DIGITAL[] = { {2730, 110}, {0, 60}, {2730, 110}, {0, 60}, {2730, 110}, {0, 60},
                              {2730, 110}, {0, 900}, {0, 0} };
// 4 Fur Elise: Beethoven, public domain
const Note RING_ELISE[] = {
  S16(2637), S16(2489), S16(2637), S16(2489), S16(2637), S16(1976), S16(2349), S16(2093),
  {1760, 420}, {0, 140}, S16(1047), S16(1319), S16(1760), {1976, 420}, {0, 140},
  S16(1319), S16(1661), S16(1976), {2093, 420}, {0, 1000}, {0, 0} };
// 5 Westminster chime: clock-tower quarters, public domain
const Note RING_CHIME[] = {
  {1661, 300}, {0, 30}, {1480, 300}, {0, 30}, {1319, 300}, {0, 30}, {988, 650}, {0, 200},
  {1319, 300}, {0, 30}, {1661, 300}, {0, 30}, {1480, 300}, {0, 30}, {988, 650}, {0, 1200}, {0, 0} };
// 6 Arcade: original rising runs
const Note RING_ARCADE[] = {
  {1047, 45}, {1319, 45}, {1568, 45}, {2093, 45}, {0, 30},
  {1319, 45}, {1568, 45}, {2093, 45}, {2637, 45}, {0, 30},
  {1568, 45}, {2093, 45}, {2637, 45}, {3136, 90}, {0, 160},
  {1047, 45}, {1319, 45}, {1568, 45}, {2093, 45}, {0, 30},
  {1319, 45}, {1568, 45}, {2093, 45}, {2637, 45}, {0, 30},
  {1568, 45}, {2093, 45}, {2637, 45}, {3136, 90}, {0, 800}, {0, 0} };
// 7 Ode to Joy: Beethoven, public domain
const Note RING_ODE[] = {
  ODE(2637), ODE(2637), ODE(2794), ODE(3136), ODE(3136), ODE(2794), ODE(2637), ODE(2349),
  ODE(2093), ODE(2093), ODE(2349), ODE(2637), {2637, 320}, {0, 25}, {2349, 100}, {0, 15},
  {2349, 460}, {0, 900}, {0, 0} };

const Note* const RINGTONES[] = { RING_BELL, RING_GRANVALS, RING_MARIMBA, RING_DIGITAL,
                                  RING_ELISE, RING_CHIME, RING_ARCADE, RING_ODE };
const int RING_COUNT = sizeof(RINGTONES) / sizeof(RINGTONES[0]);
int ringtone = 0;

const Note TUNE_NOTIFY[]   = { {1568, 70}, {0, 40}, {2093, 110}, {0, 0} };
const Note TUNE_WATER[]    = { {1047, 110}, {1319, 110}, {1568, 110}, {2093, 220}, {0, 0} };
const Note TUNE_FIND[]     = { {2637, 90}, {0, 60}, {2637, 90}, {0, 60}, {2637, 90}, {0, 450}, {0, 0} };
const Note TUNE_HAPPY[]    = { {1319, 60}, {1568, 60}, {2093, 90}, {0, 0} };
const Note TUNE_CLICK[]    = { {2400, 25}, {0, 0} };
const Note TUNE_BOOT[]     = { {784, 90}, {1047, 90}, {1319, 90}, {1568, 160}, {0, 0} };
const Note TUNE_BIRTHDAY[] = { {1047, 180}, {1047, 90}, {1175, 270}, {1047, 270}, {1397, 270}, {1319, 540},
                               {0, 120},
                               {1047, 180}, {1047, 90}, {1175, 270}, {1047, 270}, {1568, 270}, {1397, 540},
                               {0, 0} };
// Focus-timer "time's up" chime
const Note TUNE_TIMER[]    = { {1976, 140}, {0, 60}, {2349, 140}, {0, 60}, {2793, 240}, {0, 900}, {0, 0} };
// Low, flat, unmistakably "something is wrong" pattern -- plays even if the
// screen has failed, so a dead OLED is not mistaken for a dead board.
const Note TUNE_ERROR[]    = { {1200, 120}, {0, 120}, {1200, 120}, {0, 120}, {1200, 120}, {0, 900}, {0, 0} };

// SND_CALL plays the chosen ringtone (see tuneFor)
const Note* const TUNES[SND_COUNT] = { TUNE_NOTIFY, RING_BELL, TUNE_WATER, TUNE_FIND,
                                       TUNE_HAPPY, TUNE_CLICK, TUNE_BOOT, TUNE_BIRTHDAY,
                                       TUNE_TIMER, TUNE_ERROR };
// SND_ERROR bypasses this entirely (see soundAllowed); the flag here is unused.
const int SOUND_FLAG[SND_COUNT]    = { SF_NOTIFY, SF_CALL, SF_WATER, SF_FIND,
                                       SF_TOUCH, SF_TOUCH, SF_BOOT, SF_BIRTHDAY,
                                       SF_TIMER, SF_FIND };

// PWM duty (10-bit) per volume; a passive buzzer is loudest at 50 % (512)
#if BUZZER_DIRECT
const uint32_t VOLUME_DUTY[3] = { 24, 64, 160 };
#else
const uint32_t VOLUME_DUTY[3] = { 48, 160, 512 };
#endif

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define BUZZ_TARGET BUZZER_PIN       // core 3.x: LEDC calls take the pin
#else
  #define BUZZ_CHANNEL 4
  #define BUZZ_TARGET BUZZ_CHANNEL     // core 2.x: LEDC calls take a channel
#endif

// ---- player: runs in its own FreeRTOS task so note timing stays exact while
//      loop() is busy drawing the screen. loop() only posts requests. ----
const unsigned long REPEAT_FOREVER = 0xFFFFFFFFUL;   // loop until stopSound()
portMUX_TYPE soundMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t soundTaskHandle = nullptr;
const Note* reqTune = nullptr;        // request from loop() to the sound task
int reqId = -1;
unsigned long reqRepeatMs = 0;
bool reqPending = false;
volatile int playingId = -1;          // sound being played (-1 = silent)

void soundTask(void*);

void buzzerBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 10);
#else
  ledcSetup(BUZZ_CHANNEL, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BUZZ_CHANNEL);
#endif
  ledcWrite(BUZZ_TARGET, 0);
#if BUZZER_DIRECT
  gpio_set_drive_capability((gpio_num_t)BUZZER_PIN, GPIO_DRIVE_CAP_0);
#endif
  xTaskCreate(soundTask, "sound", 3072, nullptr, 2, &soundTaskHandle);
}

void buzzerTone(uint16_t freq) {
  if (freq == 0) { ledcWrite(BUZZ_TARGET, 0); return; }
  ledcWriteTone(BUZZ_TARGET, freq);   // sets the frequency (10-bit, 50 % duty)
  ledcWrite(BUZZ_TARGET, VOLUME_DUTY[soundVolume]);
}

// hand a tune to the sound task (nullptr = silence); it starts right away
void requestTune(int id, const Note* t, unsigned long repeatMs) {
  portENTER_CRITICAL(&soundMux);
  reqTune = t;
  reqId = id;
  reqRepeatMs = repeatMs;
  reqPending = true;
  portEXIT_CRITICAL(&soundMux);
  if (soundTaskHandle) xTaskNotifyGive(soundTaskHandle);
}

void soundTask(void*) {
  const Note* t = nullptr;
  int idx = 0;
  bool repeat = false, forever = false;
  unsigned long until = 0;

  for (;;) {
    bool fresh = false;
    int id = -1;
    unsigned long repeatMs = 0;
    portENTER_CRITICAL(&soundMux);
    if (reqPending) {
      t = reqTune; id = reqId; repeatMs = reqRepeatMs;
      reqPending = false;
      fresh = true;
    }
    portEXIT_CRITICAL(&soundMux);

    if (fresh) {
      idx = 0;
      repeat = repeatMs != 0;
      forever = repeatMs == REPEAT_FOREVER;
      until = millis() + (forever ? 0 : repeatMs);
      playingId = t ? id : -1;
    }

    if (t == nullptr) {
      buzzerTone(0);
      playingId = -1;
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);    // sleep until the next request
      continue;
    }

    if (t[idx].ms == 0) {                         // end of the tune
      if (repeat && (forever || (long)(millis() - until) < 0)) idx = 0;
      else t = nullptr;
      continue;
    }

    buzzerTone(t[idx].freq);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(t[idx].ms));   // wakes early on a new request
    idx++;
  }
}

const Note* tuneFor(int id) {
  if (id == SND_CALL) return RINGTONES[ringtone];
  return TUNES[id];
}

void stopSound() { requestTune(-1, nullptr, 0); }

bool isPlaying(int id) { return playingId == id; }

bool soundAllowed(int id) {
  if (id == SND_ERROR) return true;               // a hardware fault always gets signalled
  if (!(soundFlags & SOUND_FLAG[id])) return false;
  if (id == SND_FIND) return true;                // finding Nano always beeps
  if (!soundOn) return false;
  if (phoneDnd) return false;
  if (muteTimed) {
    if ((long)(millis() - muteUntil) < 0) return false;
    muteTimed = false;                            // mute time is over
  }
  if (id == SND_CALL) return true;                // the phone is ringing: ring at any hour
  if (isSleepHours()) return false;
  if ((soundFlags & SF_QUIET_OFF_HOURS) && !activeHours()) return false;
  return true;
}

// repeatMs: 0 = once, REPEAT_FOREVER = until stopSound(); preview (from the app)
// ignores the settings
void playSound(int id, unsigned long repeatMs, bool preview) {
  if (id < 0 || id >= SND_COUNT) return;
  if (!preview && !soundAllowed(id)) return;
  if (id == SND_CLICK && playingId >= 0) return;  // a click never cuts another sound
  requestTune(id, tuneFor(id), repeatMs);
}

void previewRingtone(int n) {
  if (n < 0 || n >= RING_COUNT) return;
  requestTune(SND_CALL, RINGTONES[n], 0);
}

// ===============================================================
void applyWater() { waterIntervalMs = (unsigned long)waterMin * 60000UL; }

bool prefsOk = false;   // false = flash storage unavailable; run on in-memory defaults

void loadSettings() {
  prefsOk = prefs.begin("nano", false);
  if (!prefsOk) {
    Serial.println("Preferences (NVS) unavailable; settings will not be saved this session.");
  }
  buddyName = prefs.getString("name", "Nano");
  actStart  = prefs.getInt("as", 9 * 60);
  actEnd    = prefs.getInt("ae", 17 * 60 + 30);
  waterMin  = prefs.getInt("wm", 20);
  if (buddyName.length() == 0) buddyName = "Nano";
  if (actStart < 0 || actStart >= 24 * 60) actStart = 9 * 60;
  if (actEnd < 0 || actEnd >= 24 * 60) actEnd = 17 * 60 + 30;
  if (waterMin < 1 || waterMin > 240) waterMin = 20;
  soundOn     = prefs.getBool("so", true);
  soundVolume = prefs.getInt("sv", 1);
  soundFlags  = prefs.getInt("sf", SF_DEFAULT) & SF_MASK;
  if (soundVolume < 0 || soundVolume > 2) soundVolume = 1;
  ringtone    = prefs.getInt("rt", 0);
  if (ringtone < 0 || ringtone >= RING_COUNT) ringtone = 0;
  applyWater();
}

// The phone resends settings on every connect; only write flash when they change.
void saveSettings() {
  if (!prefsOk) return;    // NVS failed to open; keep running on defaults, do not crash
  if (prefs.getString("name", "") != buddyName) prefs.putString("name", buddyName);
  if (prefs.getInt("as", -1) != actStart) prefs.putInt("as", actStart);
  if (prefs.getInt("ae", -1) != actEnd) prefs.putInt("ae", actEnd);
  if (prefs.getInt("wm", -1) != waterMin) prefs.putInt("wm", waterMin);
  if (prefs.getBool("so", !soundOn) != soundOn) prefs.putBool("so", soundOn);
  if (prefs.getInt("sv", -1) != soundVolume) prefs.putInt("sv", soundVolume);
  if (prefs.getInt("sf", -1) != soundFlags) prefs.putInt("sf", soundFlags);
  if (prefs.getInt("rt", -1) != ringtone) prefs.putInt("rt", ringtone);
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

  // Start the buzzer before the screen, so a dead OLED can still be heard.
  buzzerBegin();

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED not found! Signalling the fault on the buzzer and stopping here.");
    playSound(SND_ERROR, REPEAT_FOREVER, true);   // preview=true: plays regardless of settings
    for (;;) delay(1000);                          // nothing more we can do without a screen
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
  playSound(SND_BOOT, 0, false);                // played by loop(), so it never blocks
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
// Eye styles without a mouth read best alone (Normal/Happy/Wow/Angry/Drowsy are
// icon-like glyphs); Buddy/Grin/Robot are paired with drawMouth() below so the
// whole face reads as one character rather than two floating eyes.
void drawEye(int cx, int cy, float open, int m, bool isLeft) {
  int W = 34, H = 38, r = 10;
  if (m == SURPRISED) { W = 32; H = 42; r = 16; }
  if (m == TIRED)     { H = 26; r = 8; }
  if (m == BUDDY)     { W = 40; H = 22; r = 10; }   // wide, glowing 8-bit eyes
  if (m == GRIN)      { W = 32; H = 32; r = 10; }   // a touch shorter: room for a big smile
  if (m == ROBOT)     { W = 26; H = 26; r = 4; }    // small, square, mechanical
  if (m == DROWSY)    { H = 24; r = 8; }            // heavy lids, any time of day
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
  } else if (m == TIRED || m == DROWSY) {
    display.fillRect(x - 1, y - 1, W + 2, (int)(curH * 0.42), SSD1306_BLACK);
    if (isLeft) display.fillTriangle(x, y, x + 12, y, x, y + 12, SSD1306_BLACK);
    else        display.fillTriangle(x + W, y, x + W - 12, y, x + W, y + 12, SSD1306_BLACK);
  } else if (m == BUDDY) {
    // sharp "cool" brow cut sloping down toward the middle
    int cut = curH * 6 / 10;
    if (isLeft) display.fillTriangle(x - 1, y - 1, x + W + 1, y - 1, x + W + 1, y + cut, SSD1306_BLACK);
    else        display.fillTriangle(x - 1, y - 1, x + W + 1, y - 1, x - 1, y + cut, SSD1306_BLACK);
    // one-pixel-art shine near the outer top corner
    display.fillRect(isLeft ? x + 5 : x + W - 9, y + 4, 4, 3, SSD1306_BLACK);
  } else if (m == GRIN) {
    // a bright sparkle near the top (a little excited, not angled like Buddy)
    display.fillCircle(isLeft ? x + 8 : x + W - 8, y + 7, 2, SSD1306_BLACK);
  } else if (m == ROBOT) {
    // a single square "sensor" pupil, centred in the eye
    display.fillRect(cx - 3, cy - 3, 6, 6, SSD1306_BLACK);
  }
}

// Mouths for the styles that have one; called once after both eyes are drawn.
void drawMouth(int m, int mx, int my) {
  if (m == BUDDY) {
    display.drawLine(mx - 4, my + 3, mx, my - 1, SSD1306_WHITE);
    display.drawLine(mx, my - 1, mx + 4, my + 3, SSD1306_WHITE);
    display.drawLine(mx - 4, my + 4, mx, my, SSD1306_WHITE);
    display.drawLine(mx, my, mx + 4, my + 4, SSD1306_WHITE);
  } else if (m == GRIN) {
    // a wide open smile with two small "teeth" gaps
    display.fillRoundRect(mx - 15, my - 3, 30, 11, 5, SSD1306_WHITE);
    display.fillRoundRect(mx - 12, my, 24, 6, 3, SSD1306_BLACK);
    display.fillRect(mx - 6, my, 2, 6, SSD1306_WHITE);
    display.fillRect(mx + 4, my, 2, 6, SSD1306_WHITE);
  } else if (m == ROBOT) {
    // a flat speaker-grille bar
    display.drawFastHLine(mx - 11, my, 22, SSD1306_WHITE);
    display.drawFastVLine(mx - 11, my - 2, 4, SSD1306_WHITE);
    display.drawFastVLine(mx,      my - 2, 4, SSD1306_WHITE);
    display.drawFastVLine(mx + 11, my - 2, 4, SSD1306_WHITE);
  }
}

// small "No Phone" note at the bottom while Bluetooth is not connected
void phoneBadge() {
  if (phoneConnected) return;
  display.setTextSize(1);
  display.setCursor(40, 56);
  display.print("No Phone");
}

// a quiet "Muted" note, bottom-left, so a silent Nano doesn't look broken
void statusBadge() {
  bool muted = !soundOn || phoneDnd ||
    (muteTimed && (long)(millis() - muteUntil) < 0);
  if (!muted) return;
  display.setTextSize(1);
  display.setCursor(2, 56);
  display.print("Muted");
}

// Slow, tiny, pseudo-random drift so the eyes and clock digits are not drawn
// on the exact same OLED pixels for hours on end (reduces burn-in risk).
int burnShiftX = 0, burnShiftY = 0;
unsigned long nextBurnShift = 0;
void updateBurnShift(unsigned long now) {
  if (now < nextBurnShift) return;
  nextBurnShift = now + 300000UL;   // a new offset every 5 minutes
  burnShiftX = random(-2, 3);
  burnShiftY = random(-1, 2);
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
  int cy = 32 + (int)lookY + burnShiftY;
  int bx = (int)lookX + burnShiftX;
  drawEye(36 + bx + cross, cy, openL, mood, true);
  drawEye(92 + bx - cross, cy, openR, mood, false);
  if (mood == BUDDY || mood == GRIN || mood == ROBOT) {
    drawMouth(mood, 64 + bx, cy + 18);
  }
  statusBadge();
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
  display.setTextSize(3);
  display.setCursor((128 - (int)strlen(hhmm) * 18) / 2 + burnShiftX, 16 + burnShiftY);
  display.print(hhmm);
  display.setTextSize(1);
  if (ok) { display.setCursor((128 - (int)strlen(date) * 6) / 2, 46); display.print(date); }
  else { display.setCursor(4, 46); display.print("waiting for phone..."); }
  if (phoneBattery >= 0) {                   // phone battery, top right
    String b = String("Phone ") + phoneBattery + "%" + (phoneCharging ? "+" : "");
    display.setCursor(128 - (int)b.length() * 6, 2); display.print(b);
  }
  statusBadge();
  display.display();
}

void showWater(unsigned long now) {
  float t = modeElapsed(now) / 1000.0;
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
  int total = 20000, rem = total - (int)modeElapsed(now);
  if (rem < 0) rem = 0;
  int barX = 56, barW = 66;
  display.drawRect(barX, 52, barW, 6, SSD1306_WHITE);
  display.fillRect(barX, 52, (int)(barW * (rem / (float)total)), 6, SSD1306_WHITE);

  display.display();
}

// focus/countdown timer: big MM:SS remaining + a shrinking bar underneath,
// in the same style as the water screen. Returns true once time is up (the
// caller plays the completion sound exactly once, outside this function).
bool showTimer(unsigned long now) {
  unsigned long el = modeElapsed(now);
  display.clearDisplay();

  long remMs = (long)timerTotalMs - (long)el;
  if (remMs < 0) remMs = 0;
  int remSec = remMs / 1000;

  display.setTextSize(1);
  const char* label = "FOCUS TIME";
  display.setCursor((128 - (int)strlen(label) * 6) / 2, 10);
  display.print(label);

  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", remSec / 60, remSec % 60);
  display.setTextSize(3);
  display.setCursor((128 - (int)strlen(buf) * 18) / 2, 24);
  display.print(buf);

  float frac = timerTotalMs ? (remMs / (float)timerTotalMs) : 0;
  int barX = 14, barW = 100;
  display.drawRect(barX, 54, barW, 6, SSD1306_WHITE);
  display.fillRect(barX, 54, (int)(barW * frac), 6, SSD1306_WHITE);

  display.display();
  return remMs <= 0;
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
  if (l.indexOf("battery") >= 0) return "Battery";
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
  } else if (l.indexOf("battery") >= 0) {     // phone battery low
    display.drawRect(cx - 15, cy - 9, 28, 18, SSD1306_WHITE);
    display.fillRect(cx + 13, cy - 4, 3, 8, SSD1306_WHITE);
    display.fillRect(cx - 12, cy - 6, 5, 12, SSD1306_WHITE);
  } else {                                   // unknown app: a rounded tile with its initial
    display.fillRoundRect(cx - 16, cy - 16, 32, 32, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK); display.setTextSize(3);
    char c = n.length() ? toupper(n.charAt(0)) : '?';
    display.setCursor(cx - 8, cy - 11); display.print(c);
    display.setTextColor(SSD1306_WHITE);
  }
}

// notification: app logo + who sent it, a quick pop-in, no message text
bool showNotify(unsigned long now) {
  unsigned long el = modeElapsed(now);
  display.clearDisplay();
  int cx = 64, cy = 22;

  if (el < 450) {                            // pop-in ring
    int rr = 6 + (int)(el / 450.0 * 26);
    display.drawCircle(cx, cy, rr, SSD1306_WHITE);
  }
  drawAppLogo(notifyApp, cx, cy);

  // who sent it (falls back to the app name, e.g. in private mode)
  String who = notifyFrom.length() ? notifyFrom : appFriendly(notifyApp);
  display.setTextColor(SSD1306_WHITE);
  int ty = 46;
  if (who.length() <= 10) {                  // big text fits up to 10 letters
    display.setTextSize(2);
  } else {
    display.setTextSize(1);
    ty = 50;
    if (who.length() > 21) who = who.substring(0, 19) + "..";
  }
  int tw = who.length() * (ty == 46 ? 12 : 6);
  display.setCursor((128 - tw) / 2, ty);
  display.print(who);

  display.display();
  return !notifyIsCall && el > 5000;           // 5 s; a call stays until it ends (endCall)
}

// "find my Nano": big surprised eyes, flashing screen
bool showFind(unsigned long now) {
  unsigned long el = modeElapsed(now);
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

// the call was answered, declined, missed or silenced: stop ringing, close the call screen
void endCall() {
  if (!callActive) return;
  callActive = false;
  if (isPlaying(SND_CALL)) stopSound();
  if (mode == NOTIFY && notifyIsCall) mode = homeMode();
  notifyIsCall = false;
}

// Music only takes over idle screens, never a notification or reminder.
bool canShowMusic() {
  return mode == EYES || mode == CLOCK || mode == MUSIC;
}

void setEyeStyle(int pick) {
  if (pick < 0 || pick >= MOOD_COUNT) return;
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

  } else if (type == 'N' || type == 'R') {        // N|App|sender (5 s)   R|App|caller = incoming call
    if (type == 'N' && callActive) return;        // keep showing the ringing call
    rest = asciiOnly(rest);
    int bar = rest.indexOf('|');
    if (bar >= 0) { notifyApp = rest.substring(0, bar); notifyFrom = rest.substring(bar + 1); }
    else          { notifyApp = rest; notifyFrom = ""; }
    notifyApp.trim(); notifyFrom.trim();
    notifyFrom = notifyFrom.substring(0, 40);
    notifyIsCall = (type == 'R');
    mode = NOTIFY; modeStart = millis(); pendingTap = false;
    if (type == 'R') {
      // Rings until the phone says the call was answered, declined or missed (Q),
      // or Nano is tapped. Another R while ringing only updates the caller name.
      if (!callActive) {
        callActive = true;
        callStart = millis();
        playSound(SND_CALL, REPEAT_FOREVER, false);
      }
    } else {
      playSound(SND_NOTIFY, 0, false);
    }

  } else if (type == 'Q') {                       // call answered/declined/ended: stop ringing
    endCall();

  } else if (type == 'Z') {                       // Z|minutes: mute for a while (0 = unmute)
    long minutes = rest.toInt();
    muteTimed = minutes > 0;
    muteUntil = millis() + (unsigned long)minutes * 60000UL;
    if (muteTimed && !isPlaying(SND_FIND)) stopSound();

  } else if (type == 'D') {                       // D|1 phone Do Not Disturb on, D|0 off
    phoneDnd = rest.toInt() != 0;
    if (phoneDnd && !isPlaying(SND_FIND)) stopSound();

  } else if (type == 'L') {                       // L|percent|charging  phone battery (-1 hides it)
    int bar = rest.indexOf('|');
    phoneBattery = (bar < 0 ? rest : rest.substring(0, bar)).toInt();
    if (phoneBattery > 100) phoneBattery = 100;
    if (phoneBattery < -1) phoneBattery = -1;        // anything but a real % hides the reading
    phoneCharging = bar >= 0 && rest.substring(bar + 1).toInt() != 0;

  } else if (type == 'B') {                       // B|on|volume 0-2|flags|ringtone
    String parts[4]; int n = 0; String r = rest;
    while (n < 4) {
      int b = r.indexOf('|');
      if (b < 0) { parts[n++] = r; break; }
      parts[n++] = r.substring(0, b);
      r = r.substring(b + 1);
    }
    if (n >= 1 && parts[0].length()) soundOn = parts[0].toInt() != 0;
    if (n >= 2) { int v = parts[1].toInt(); if (v >= 0 && v <= 2) soundVolume = v; }
    if (n >= 3 && parts[2].length()) soundFlags = parts[2].toInt() & SF_MASK;
    if (n >= 4 && parts[3].length()) { int t = parts[3].toInt(); if (t >= 0 && t < RING_COUNT) ringtone = t; }
    if (!soundOn && !isPlaying(SND_FIND)) stopSound();
    saveSettings();

  } else if (type == 'P') {                       // P|<sound id> or P|r<ringtone> preview from the app
    if (rest.startsWith("s")) { if (!callActive) stopSound(); }   // P|s: stop a preview
    else if (rest.startsWith("r")) previewRingtone(rest.substring(1).toInt());
    else if (rest.length()) playSound(rest.toInt(), 0, true);

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
    playSound(SND_WATER, 0, false);

  } else if (type == 'E') {                       // E|0..3 eye style
    if (rest.length()) setEyeStyle(rest.toInt());

  } else if (type == 'F') {                       // find my Nano
    mode = FIND; modeStart = millis(); pendingTap = false;
    playSound(SND_FIND, FIND_MS, false);

  } else if (type == 'X') {                       // X|minutes: focus/countdown timer, 0 cancels
    long mins = rest.toInt();
    if (mins <= 0) {
      if (mode == TIMER) mode = homeMode();
    } else {
      if (mins > 180) mins = 180;                 // 3 h cap; a stuck timer should never run forever
      timerTotalMs = (unsigned long)mins * 60000UL;
      mode = TIMER; modeStart = millis(); pendingTap = false;
    }

  } else {
    Serial.print("Unknown command type: '"); Serial.print(type); Serial.println("'");
  }
}

// ===============================================================
void onSingleTap() { mode = CLOCK; modeStart = millis(); }
void onDoubleTap() {
  setEyeStyle((moodPick + 1) % MOOD_COUNT);
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
  now = millis();                                // commands may have taken a moment

  // phone went away: its music info is stale now
  bool connectedNow = phoneConnected;
  if (wasConnected && !connectedNow) {
    endCall();                                   // can't hear "call ended" any more
    musicState = "STOPPED";
    if (mode == MUSIC) mode = EYES;
  }
  wasConnected = connectedNow;
  if (callActive && (long)(now - callStart) > (long)CALL_MAX_MS) endCall();

  updateBurnShift(now);

  // Softer screen at night, so Nano doesn't glow like a nightlight in a dark
  // room; back to normal contrast the moment sleep hours end.
  static bool displayDimmed = false;
  bool wantDim = isSleepHours();
  if (wantDim != displayDimmed) {
    displayDimmed = wantDim;
    display.dim(wantDim);
  }

  // Self-heal if the heap is getting dangerously fragmented from long uptime
  // (lots of short-lived Strings). A clean restart is safer than an eventual
  // crash; BLE simply re-advertises and the phone reconnects on its own.
  static unsigned long nextHeapCheck = 0;
  if (now >= nextHeapCheck) {
    nextHeapCheck = now + 60000UL;
    uint32_t freeHeap = ESP.getFreeHeap();
    Serial.print("Free heap: "); Serial.println(freeHeap);
    if (freeHeap < 12000) {
      Serial.println("Heap critically low; restarting to recover.");
      delay(50);
      ESP.restart();
    }
  }

  // Debounce the touch sensor: two consistent readings in a row (~60 ms)
  // before trusting a state change, so electrical noise can't fake a tap.
  static bool touchFiltered = false;
  static uint8_t touchSame = 0;
  bool touchRaw = digitalRead(TOUCH_PIN);
  if (touchRaw == touchFiltered) {
    touchSame = 0;
  } else if (++touchSame >= 2) {
    touchFiltered = touchRaw;
    touchSame = 0;
  }
  bool touch = touchFiltered;
  bool rising = (touch && !lastTouch);
  bool falling = (!touch && lastTouch);
  if (rising) { pressStart = now; muteToggledThisPress = false; }

  // ---- MUSIC controls: hold = play/pause ----
  if (mode == MUSIC && touch && !musicLongTriggered &&
      (now - pressStart >= HOLD_MS)) {
    musicLongTriggered = true;
    pendingTap = false;
    sendMediaControl("PLAYPAUSE");
  }

  // ---- HOLD on the clock = torch: fills the screen white, a little desk
  //      flashlight. Capped at a minute even if a finger rests on it. ----
  if (mode == CLOCK && touch && !torchActive && (now - pressStart >= TORCH_HOLD_MS)) {
    torchActive = true; pendingTap = false;
  }
  if (torchActive) {
    if (!touch || (now - pressStart) > TORCH_HOLD_MS + 60000UL) {
      torchActive = false;
    } else {
      display.clearDisplay();
      display.fillRect(0, 0, 128, 64, SSD1306_WHITE);
      display.display();
      lastTouch = touch; delay(30); return;
    }
  }

  // ---- HOLD = pet (only from the eyes screen); keep holding past the pet
  //      threshold to toggle Nano's local mute without needing the phone ----
  if (mode == EYES && touch && !petActive && (now - pressStart >= HOLD_MS)) {
    petActive = true; pendingTap = false;
    playSound(SND_HAPPY, 0, false);
    petWord = CUTE[random(CUTE_N)]; petWordNext = now + 1500;
  }
  if (petActive) {
    if (!touch) { petActive = false; }           // let go -> back to eyes
    else {
      if (!muteToggledThisPress && (now - pressStart >= MUTE_HOLD_MS)) {
        muteToggledThisPress = true;
        soundOn = !soundOn;
        saveSettings();
        if (!soundOn) stopSound();
        playSound(SND_CLICK, 0, true);            // feedback, bypassing the new mute state
      }
      if (muteToggledThisPress) {
        display.clearDisplay();
        display.setTextSize(2);
        String msg = soundOn ? "UNMUTED" : "MUTED";
        display.setCursor((128 - (int)msg.length() * 12) / 2, 24);
        display.print(msg);
        display.display();
      } else {
        if (now >= petWordNext) { petWord = CUTE[random(CUTE_N)]; petWordNext = now + 1500; }
        showPet(now);
      }
      lastTouch = touch; delay(30); return;
    }
  }

  // ---- taps register on release of a SHORT press ----
  if (falling && (now - pressStart < HOLD_MS) && !musicLongTriggered) {
    // a tap silences a ringing call (and closes it) or the find beeps; otherwise it clicks
    if (callActive) {
      endCall();
      pendingTap = false;
    } else {
      if (isPlaying(SND_FIND)) stopSound();
      else playSound(SND_CLICK, 0, false);

      if (mode == MUSIC) {
        if (pendingTap && (now - lastTapTime <= DOUBLE_TAP_MS)) {
          pendingTap = false;
          sendMediaControl("PREV");
        } else {
          pendingTap = true;
          lastTapTime = now;
        }
      } else if (mode == WATER) {                   // tapped away = "I drank" -> phone counts it
        lastWater = now; mode = homeMode(); pendingTap = false;
        sendToPhone("H|WATER");
      }
      else if (mode != EYES) { mode = homeMode(); pendingTap = false; }
      else {
        if (pendingTap && (now - lastTapTime <= DOUBLE_TAP_MS)) { pendingTap = false; onDoubleTap(); }
        else { pendingTap = true; lastTapTime = now; }
      }
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
  if (mode == CLOCK)  { showClock(); if (modeElapsed(now) > 5000) mode = homeMode(); delay(30); return; }
  if (mode == WATER)  { showWater(now); if (modeElapsed(now) > 20000) { mode = homeMode(); lastWater = now; } delay(30); return; }
  if (mode == NOTIFY)     { if (showNotify(now)) mode = homeMode(); delay(30); return; }
  if (mode == FIND)       { if (showFind(now)) { setInverted(false); if (isPlaying(SND_FIND)) stopSound(); mode = homeMode(); } delay(30); return; }
  if (mode == TIMER)      { if (showTimer(now)) { playSound(SND_TIMER, 0, false); mode = homeMode(); } delay(30); return; }

  if (isBirthday()) {
    static int tunePlayedDay = -1;                // sing once per birthday
    struct tm ti;
    if (getNow(ti) && ti.tm_yday != tunePlayedDay && soundAllowed(SND_BIRTHDAY) && playingId < 0) {
      tunePlayedDay = ti.tm_yday;                 // waits until sound is allowed (not at midnight)
      playSound(SND_BIRTHDAY, 0, false);
    }
    showBirthday(now); delay(30); return;
  }
  if (now - lastWater >= waterIntervalMs) {
    // no nagging while Nano is sleepy or asleep; restart the timer instead
    if (activeHours() && !isSleepHours()) { mode = WATER; modeStart = now; playSound(SND_WATER, 0, false); }
    else lastWater = now;
  }
  updateEyes(now);
  delay(30);
}
