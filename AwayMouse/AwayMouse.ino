// Away Mouse - ESP32-S3 Super Mini mouse jiggler
//
// Plug it into a computer and it's a USB mouse. Power it from a charger and
// it's a Bluetooth mouse ("BT5.0 Mouse", no PIN). As soon as a computer is
// connected it starts moving the cursor in human-like bursts that always
// return to where they started. The onboard LED is green while moving. While
// waiting it fades from red (just finished) to blue (about to move again), so
// the color shows roughly how long until the next burst. Off = no computer.
// No buttons, no Wi-Fi.
//
// Tuning lives in config.h.
//
// Build settings (Arduino IDE, board "ESP32S3 Dev Module", core 3.3.x):
//   USB Mode: USB-OTG (TinyUSB)      USB CDC On Boot: Disabled
//   Flash Size: 4MB                  Partition Scheme: Huge APP (3MB No OTA)
//   PSRAM: Disabled                  Flash Mode: DIO 80MHz

#include <Arduino.h>
#include "esp_random.h"
#include "config.h"
#include "links.h"
#include "motion.h"

#if !defined(CONFIG_TINYUSB_HID_ENABLED) || ARDUINO_USB_MODE
#error "Set Tools > USB Mode to 'USB-OTG (TinyUSB)'"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "Set Tools > USB CDC On Boot to 'Disabled' so the computer only sees a mouse"
#endif

static const MotionConfig kMotion = {
  MOVES_MIN, MOVES_MAX, WANDER_PX, SPEED, OVERSHOOT_CHANCE, TREMOR_PX,
  SCROLL_CHANCE, SCROLL_NOTCHES_MIN, SCROLL_NOTCHES_MAX,
};

// Bridges the motion engine to the real links and FreeRTOS timing.
class DeviceSink : public MouseSink {
public:
  bool send(int8_t dx, int8_t dy, int8_t wheel) override { return links::send(dx, dy, wheel); }
  // Only "alive" while the link the burst started on is still usable.
  bool alive() override { return link_ != Link::NONE && links::active() == link_; }
  void setLink(Link l) { link_ = l; }
  void waitTick() override {
    const TickType_t period = pdMS_TO_TICKS(tickMs());
    TickType_t now = xTaskGetTickCount();
    // if we fell far behind (or were paused), restart the cadence
    if ((TickType_t)(now - last_) > 4 * period) last_ = now;
    vTaskDelayUntil(&last_, period ? period : 1);
  }
  void resetTick() override { last_ = xTaskGetTickCount(); }
  void sleepMs(uint32_t ms) override { delay(ms); }
  uint32_t tickMs() override { return links::tickMs(); }
  uint32_t random32() override { return esp_random(); }

private:
  TickType_t last_ = 0;
  Link link_ = Link::NONE;
};

static DeviceSink sink;
static MotionEngine engine(kMotion, sink);

static Link lastLink = Link::NONE;
static uint32_t nextBurstMs = 0;
static uint32_t pauseStartMs = 0;   // when the current wait began

// ---------- LED ----------
static uint32_t ledLast = 0xFFFFFFFF;  // forces the first write

static void setLed(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c == ledLast) return;  // only talk to the LED when the color changes
  ledLast = c;
  rgbLedWriteOrdered(LED_PIN, LED_COLOR_ORDER, r, g, b);
}
static void ledOff()    { setLed(0, 0, 0); }
static void ledMoving() { setLed(0, LED_BRIGHTNESS, 0); }

// Red at the start of the wait, blue just before the next burst.
static void ledWaiting() {
  uint32_t total = nextBurstMs - pauseStartMs;
  uint32_t done = millis() - pauseStartMs;
  float f = total ? (float)done / (float)total : 1.0f;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  uint8_t blue = (uint8_t)lroundf(LED_BRIGHTNESS * f);
  uint8_t red = LED_BRIGHTNESS - blue;
  setLed(red, 0, blue);
}

static uint32_t randRange(uint32_t lo, uint32_t hi) {
  return hi <= lo ? lo : lo + esp_random() % (hi - lo + 1);
}

void setup() {
  // LED off until a computer connects.
  ledOff();

  // Debug log on the UART pins (TX = GPIO43), not on USB.
  Serial0.begin(115200);
  Serial0.println("\nAway Mouse starting");

  links::begin();
}

void loop() {
  links::service();

  Link link = links::active();
  if (link != lastLink) {
    Serial0.printf("link: %s\n", links::describe());
    if (link != Link::NONE) {
      // Freshly connected: start moving shortly.
      pauseStartMs = millis();
      nextBurstMs = pauseStartMs + randRange(FIRST_MOVE_DELAY_MIN_MS, FIRST_MOVE_DELAY_MAX_MS);
    }
    lastLink = link;
  }

  if (link == Link::NONE) {
    ledOff();
    delay(50);
    return;
  }

  if ((int32_t)(millis() - nextBurstMs) >= 0) {
    sink.setLink(link);
    ledMoving();
    bool ok = engine.runBurst();
    Serial0.printf("burst %s\n", ok ? "done" : "interrupted");
    pauseStartMs = millis();
    nextBurstMs = pauseStartMs + 1000UL * randRange(PAUSE_MIN_S, PAUSE_MAX_S);
    if (!ok) {
      ledOff();  // link dropped mid-burst; loop() re-evaluates next pass
      return;
    }
  }
  ledWaiting();
  delay(20);
}
