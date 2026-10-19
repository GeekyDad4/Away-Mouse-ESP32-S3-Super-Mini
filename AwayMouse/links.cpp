// links.cpp - USB (TinyUSB) and Bluetooth LE (HID over GATT) mouse links.
#include "links.h"
#include "config.h"

#include "USB.h"
#include "USBHIDMouse.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEHIDDevice.h>
#include <BLESecurity.h>
#include <BLEAdvertising.h>

namespace {

// ======================= USB =======================
USBHIDRelativeMouse usbMouse;
volatile bool usbSuspended = false;
volatile uint32_t usbLastChangeMs = 0;  // last mount/unmount/start event

void onUsbEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:  usbSuspended = false; usbLastChangeMs = millis(); break;
    case ARDUINO_USB_STOPPED_EVENT:  usbSuspended = false; usbLastChangeMs = millis(); break;
    case ARDUINO_USB_SUSPEND_EVENT:  usbSuspended = true;  break;
    case ARDUINO_USB_RESUME_EVENT:   usbSuspended = false; break;
    default: break;
  }
}

bool usbMounted() { return (bool)USB; }

// ======================= Bluetooth =======================
// Standard 3-button mouse with wheel, report ID 1:
// [buttons][x][y][wheel]
const uint8_t kBleReportMap[] = {
  0x05, 0x01,        // Usage Page (Generic Desktop)
  0x09, 0x02,        // Usage (Mouse)
  0xA1, 0x01,        // Collection (Application)
  0x85, 0x01,        //   Report ID (1)
  0x09, 0x01,        //   Usage (Pointer)
  0xA1, 0x00,        //   Collection (Physical)
  0x05, 0x09,        //     Usage Page (Buttons)
  0x19, 0x01,        //     Usage Minimum (1)
  0x29, 0x03,        //     Usage Maximum (3)
  0x15, 0x00,        //     Logical Minimum (0)
  0x25, 0x01,        //     Logical Maximum (1)
  0x95, 0x03,        //     Report Count (3)
  0x75, 0x01,        //     Report Size (1)
  0x81, 0x02,        //     Input (Data, Variable, Absolute)
  0x95, 0x01,        //     Report Count (1)
  0x75, 0x05,        //     Report Size (5)
  0x81, 0x03,        //     Input (Constant) padding
  0x05, 0x01,        //     Usage Page (Generic Desktop)
  0x09, 0x30,        //     Usage (X)
  0x09, 0x31,        //     Usage (Y)
  0x09, 0x38,        //     Usage (Wheel)
  0x15, 0x81,        //     Logical Minimum (-127)
  0x25, 0x7F,        //     Logical Maximum (127)
  0x75, 0x08,        //     Report Size (8)
  0x95, 0x03,        //     Report Count (3)
  0x81, 0x06,        //     Input (Data, Variable, Relative)
  0xC0,              //   End Collection
  0xC0               // End Collection
};

bool bleStarted = false;        // stack initialised
bool bleAdvertising = false;
volatile bool bleConnected = false;
volatile bool bleSubscribed = false;
volatile bool bleEncrypted = false;
volatile uint16_t bleConnHandle = 0xFFFF;
volatile bool bleLastNotifyOk = false;
volatile uint32_t bleReadySinceMs = 0;

BLEServer *bleServer = nullptr;
BLEHIDDevice *bleHid = nullptr;
BLECharacteristic *bleInput = nullptr;

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer *s, ble_gap_conn_desc *desc) override {
    bleConnected = true;
    bleConnHandle = desc->conn_handle;
    bleAdvertising = false;  // NimBLE stops advertising on connect
    // ask for a snappy connection interval: 7.5-15 ms, no latency, 4 s timeout
    s->requestConnParams(desc->conn_handle, 6, 12, 0, 400);
  }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *) override {
    bleConnected = false;
    bleSubscribed = false;
    bleEncrypted = false;
    bleConnHandle = 0xFFFF;
    bleReadySinceMs = 0;
  }
};

class InputCb : public BLECharacteristicCallbacks {
  void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *, uint16_t subValue) override {
    bleSubscribed = (subValue & 0x0001) != 0;
    bleReadySinceMs = millis();
  }
  void onStatus(BLECharacteristic *, Status s, uint32_t) override {
    bleLastNotifyOk = (s == SUCCESS_NOTIFY);
  }
};

class SecurityCb : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() override { return 0; }
  void onPassKeyNotify(uint32_t) override {}
  bool onConfirmPIN(uint32_t) override { return true; }
  bool onSecurityRequest() override { return true; }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    bleEncrypted = desc->sec_state.encrypted;
    bleReadySinceMs = millis();
  }
};

void bleInit() {
  if (bleStarted) return;
  BLEDevice::init(BLE_NAME);

  BLESecurity *sec = new BLESecurity();
  sec->setCapability(ESP_IO_CAP_NONE);          // "Just Works": no PIN
  sec->setAuthenticationMode(true, false, true); // bond, no MITM, secure connections
  BLEDevice::setSecurityCallbacks(new SecurityCb());

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new ServerCb());
  bleServer->advertiseOnDisconnect(false);      // we decide when to advertise

  bleHid = new BLEHIDDevice(bleServer);
  bleHid->manufacturer()->setValue(BLE_MANUFACTURER);
  bleHid->pnp(0x02, USB_VENDOR_ID, USB_PRODUCT_ID, 0x0110);
  bleHid->hidInfo(0x00, 0x02);                  // country 0, normally connectable
  bleHid->reportMap((uint8_t *)kBleReportMap, sizeof(kBleReportMap));
  bleInput = bleHid->inputReport(1);
  bleInput->setCallbacks(new InputCb());
  bleHid->setBatteryLevel(100);
  bleHid->startServices();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->setAppearance(0x03C2);                   // HID Mouse
  adv->addServiceUUID(bleHid->hidService()->getUUID());
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x0C);

  bleStarted = true;
}

void bleStartAdvertising() {
  bleInit();
  if (!bleAdvertising && !bleConnected) {
    BLEDevice::startAdvertising();
    bleAdvertising = true;
  }
}

void bleShutOff() {
  if (!bleStarted) return;
  if (bleAdvertising) {
    BLEDevice::getAdvertising()->stop();
    bleAdvertising = false;
  }
  if (bleConnected && bleConnHandle != 0xFFFF) {
    bleServer->disconnect(bleConnHandle);
  }
}

bool bleReady() {
  // connected, paired/encrypted, input reports turned on, and settled for a moment
  return bleConnected && bleEncrypted && bleSubscribed && (millis() - bleReadySinceMs) > 500;
}

uint32_t bootMs = 0;

}  // namespace

namespace links {

void begin() {
  bootMs = millis();
  usbLastChangeMs = bootMs;

  USB.VID(USB_VENDOR_ID);
  USB.PID(USB_PRODUCT_ID);
  USB.manufacturerName(USB_MANUFACTURER);
  USB.productName(USB_PRODUCT);
  USB.onEvent(onUsbEvent);
  usbMouse.begin();
  USB.begin();
}

void service() {
  if (usbMounted()) {
    // A computer enumerated us over USB: USB wins, Bluetooth goes quiet.
    if (bleAdvertising || bleConnected) bleShutOff();
    return;
  }
  // No USB host. Give enumeration a moment, then fall back to Bluetooth.
  if (millis() - usbLastChangeMs < USB_DETECT_MS) return;
  if (!bleConnected && !bleAdvertising) bleStartAdvertising();
}

Link active() {
  if (usbMounted()) {
    if (usbSuspended) return Link::NONE;  // computer asleep: don't wake it
    return Link::USB;
  }
  if (bleReady()) return Link::BLE;
  return Link::NONE;
}

bool send(int8_t dx, int8_t dy, int8_t wheel) {
  switch (active()) {
    case Link::USB: {
      hid_mouse_report_t r = {};
      r.buttons = 0;
      r.x = dx;
      r.y = dy;
      r.wheel = wheel;
      r.pan = 0;
      return usbMouse.sendReport(r);
    }
    case Link::BLE: {
      uint8_t r[4] = {0, (uint8_t)dx, (uint8_t)dy, (uint8_t)wheel};
      bleLastNotifyOk = false;
      bleInput->setValue(r, sizeof(r));
      bleInput->notify();
      return bleLastNotifyOk;
    }
    default:
      return false;
  }
}

uint32_t tickMs() {
  return usbMounted() ? USB_TICK_MS : BLE_TICK_MS;
}

const char *describe() {
  if (usbMounted()) return usbSuspended ? "USB (computer asleep)" : "USB";
  if (bleConnected) return (bleSubscribed && bleEncrypted) ? "Bluetooth" : "Bluetooth (setting up)";
  if (bleAdvertising) return "Bluetooth advertising";
  return "waiting for USB";
}

}  // namespace links
