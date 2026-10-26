# nano

Firmware for Nano, a cute OLED desk companion on an ESP32 (`nano.ino`), by Mohamed Thahir ([@thahirsprojects](https://github.com/thahirsprojects)).

- Android app: [nano-companion-android](https://github.com/thahirsprojects/nano-companion-android)
- 3D-printed pocket case with battery and kickstand: [`case/`](case/)

## Eye styles (E command / double-tap)
0 Normal · 1 Happy · 2 Wow · 3 Angry · 4 Buddy (8-bit keychain face) ·
5 Grin · 6 Robot · 7 Drowsy. Outside Awake hours and overnight Nano always
switches to its own tired/sleeping eyes, whichever style is picked.

## Touch gestures on Nano (no phone needed)
- Tap the eyes: clock. Double-tap: next eye style. Hold: pet Nano.
- Keep holding past the pet (~2.5 s): toggle mute on Nano itself.
- Hold the clock screen (~1.2 s): torch -- fills the screen white as a tiny flashlight, capped at a minute.
- Tap any temporary screen (water, notification, timer, find) to dismiss it early.

## Focus timer
`X|<minutes>` (0 cancels) shows a countdown with a shrinking bar and chimes
when it reaches zero; capped at 3 hours. Triggered from the app's Home tab.

## Reliability
- A dead OLED still sounds a buzzer SOS pattern instead of failing silently.
- Settings only write to flash when they actually change, and skip writing
  entirely if the flash storage failed to open.
- The screen dims automatically during sleep hours, and the eyes/clock
  drift by a pixel or two every few minutes to reduce OLED burn-in.
- A heap watchdog restarts the board if memory gets critically low after
  long uptime, instead of crashing unpredictably.
- The touch sensor is debounced (two consistent readings before a tap or
  release registers) to ignore electrical noise.
