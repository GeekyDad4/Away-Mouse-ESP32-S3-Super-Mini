// config.h - everything you might want to tune. Edit, rebuild, reflash.
#pragma once

// ---------------- Timing ----------------
// After a computer connects, the first burst starts within this window (ms).
#define FIRST_MOVE_DELAY_MIN_MS   2000
#define FIRST_MOVE_DELAY_MAX_MS   5000

// Idle time between bursts (seconds). A random value in this range.
#define PAUSE_MIN_S               10
#define PAUSE_MAX_S               45      // keep under the shortest idle/lock timeout

// ---------------- Movement ----------------
#define MOVES_MIN                 2       // outbound strokes per burst
#define MOVES_MAX                 5
#define WANDER_PX                 150.0f  // max distance from the burst's start (mouse counts ~ pixels)
#define SPEED                     1.0f    // 1.0 normal, 0.5 slower, 2.0 faster
#define OVERSHOOT_CHANCE          0.30f   // chance a stroke overshoots then corrects
#define TREMOR_PX                 0.6f    // hand tremor amplitude

// ---------------- Scrolling ----------------
#define SCROLL_CHANCE             0.15f   // chance a burst includes a scroll down-and-back-up
#define SCROLL_NOTCHES_MIN        1
#define SCROLL_NOTCHES_MAX        4

// ---------------- Link selection ----------------
// How long to wait for a computer to enumerate the USB mouse before
// falling back to Bluetooth (ms). Also used after a USB host goes away.
#define USB_DETECT_MS             3000

// Report intervals (ms)
#define USB_TICK_MS               10
#define BLE_TICK_MS               15

// ---------------- Identity (what the computer sees) ----------------
#define USB_VENDOR_ID             0x303A
#define USB_PRODUCT_ID            0x4002
#define USB_MANUFACTURER          "Generic"
#define USB_PRODUCT               "USB Optical Mouse"
#define BLE_NAME                  "BT5.0 Mouse"
#define BLE_MANUFACTURER          "Generic"

// ---------------- Board ----------------
// Onboard WS2812 RGB LED (Super Mini: GPIO48).
//   green = moving
//   red -> blue fade = waiting; red right after a burst, blue just before the next
//   off = no computer connected
#define LED_PIN                   48
#define LED_BRIGHTNESS            24      // 0-255. Full brightness is very bright on these boards.
// This Super Mini's LED takes RGB order. Standard WS2812 parts use
// LED_COLOR_ORDER_GRB; switch back if green shows up red on another board.
#define LED_COLOR_ORDER           LED_COLOR_ORDER_RGB
