// SmartKnob firmware for the smartknob_driver board (ESP32-S3 + DRV8316CR + AS5600).
//
// The knob runs a generic haptic engine: an integer value in [lo, hi], one value per `step`
// degrees, optional detents and spring end-walls past lo/hi. The engine always runs here, so the
// feel never waits on the PC.
//
// Two ways to use it, each over USB or Bluetooth LE (paired in Windows Bluetooth settings):
//  * Host mode: the SmartKnob desktop app (SmartKnob/desktop) talks to the knob over the USB CDC
//    serial port or the BLE Nordic UART Service (same text protocol), pushes a haptic config per
//    mode and gets value changes back. It sets the PC volume exactly, so nothing has to be guessed.
//  * Standalone: with no app for HOST_TIMEOUT_MS the knob is a media-key volume knob (USB HID, or
//    BLE HID when USB isn't connected). HID
//    can only nudge the volume, so on connect it drives the PC to 0 % and then to
//    START_VOLUME_PCT, unless the app told it the real level shortly before going away.
//
// Haptics need motor power (VM). Without it the knob still works as a plain encoder.
//
// Protocol (lines of `CMD key=value ...`, 115200, '\n' terminated):
//   host -> knob  HELLO                         -> INFO name=SmartKnob fw=2 hap=<0|1>
//                 CFG lo= hi= val= step= det= wall=   (step deg, det volts, wall volts/deg)
//                 VAL v=<n>                     set the value without moving the knob
//                 PING [vol=<pct>]              keep-alive, -> PONG hap= vm=
//   knob -> host  VAL v=<n>                     value changed by turning the knob
// Human commands: s (status), d (toggle debug print), c (recalibrate), v<pct> (standalone volume).
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "USB.h"
#include "USBHIDConsumerControl.h"
#include <BLEDevice.h>
#include <BLEHIDDevice.h>
#include <BLE2902.h>
#include <BLESecurity.h>
#include <freertos/stream_buffer.h>

#if ARDUINO_USB_MODE != 0
#error "HID needs the native USB-OTG (TinyUSB) mode: build with -DARDUINO_USB_MODE=0"
#endif

// ---------------- Tuning ----------------
static constexpr int KNOB_DIR = 1;              // flip to -1 if clockwise turns the value down
static constexpr int VOL_STEP_PCT = 2;          // Windows moves 2 % per Volume Up/Down press
static constexpr int VOL_STEPS = 100 / VOL_STEP_PCT;
static constexpr int START_VOLUME_PCT = 20;     // standalone: level set after the connect-time sync
static constexpr float STEP_DEG = 5.0f;         // standalone: knob rotation per volume step
static constexpr float STEP_HYST = 0.15f;       // extra fraction of a step before changing (no chatter)
static constexpr float WALL_K = 0.08f;          // standalone end-stop spring, volts per degree past the end
static constexpr float WALL_KD = 0.004f;        // end-stop damping, volts per (deg/s)
static constexpr float DETENT_KMAX = 0.25f;     // detent spring stiffness cap, volts per degree
static constexpr float DETENT_KD = 0.002f;      // detent damping, volts per (deg/s)
static constexpr float VEL_ALPHA = 0.03f;       // velocity low-pass per loop (~7 ms at 4.7 kHz)
static constexpr float DAMP_DB_DPS = 30.0f;     // damping ignores speeds below this: one encoder LSB of
                                                // jitter reads as tens of deg/s and made the knob buzz at rest
static constexpr float UMAX_12V = 3.0f;         // max q-axis voltage with barrel-jack power
static constexpr float UMAX_LOW = 1.2f;         // max q-axis voltage when VM is low (e.g. USB-fed)
static constexpr float CAL_VOLTS = 2.0f;        // alignment voltage during calibration
static constexpr int POLE_PAIRS = 7;            // 14-magnet gimbal motor (measured)
static constexpr uint32_t KEY_INTERVAL_MS = 12; // spacing between HID key presses
static constexpr uint32_t USB_SETTLE_MS = 4000; // wait for the desktop app before a standalone sync
static constexpr uint32_t HOST_TIMEOUT_MS = 3000;
static constexpr uint32_t VAL_INTERVAL_MS = 8;  // min spacing of VAL reports to the host

// ---------------- Pins (mcu.kicad_sch) ----------------
static constexpr int PIN_INH[3] = {33, 21, 17};
static constexpr int PIN_INL[3] = {47, 18, 16};
static constexpr int PIN_DRV_CS = 12, PIN_DRV_SCLK = 13, PIN_DRV_SDI = 14, PIN_DRV_SDO = 15;
static constexpr int PIN_DRV_NSLEEP = 34, PIN_DRV_NFAULT = 48, PIN_DRV_DRVOFF = 35;
static constexpr int PIN_VM_SENSE = 7;
static constexpr int PIN_ENC_SCL = 1, PIN_ENC_SDA = 2;

static constexpr uint8_t REG_IC_STATUS = 0x00, REG_CTRL1 = 0x03, REG_CTRL2 = 0x04;
static constexpr uint8_t PWM_MODE_3X = 0b10;
// 20 kHz x 11 bit exceeds the LEDC divider range on this core; 10 bit works.
static constexpr int PWM_FREQ = 20000, PWM_BITS = 10, PWM_MAX = (1 << PWM_BITS) - 1;
static constexpr float TWO_PI_F = 6.28318531f, HALF_PI_F = 1.57079633f;
static constexpr float VM_MIN = 4.5f;  // DRV8316 minimum operating voltage

static SPIClass drvSpi(FSPI);
static const SPISettings drvSpiSettings(1000000, MSBFIRST, SPI_MODE1);
static USBHIDConsumerControl consumer;
static USBCDC Link;  // serial link to the desktop app (own object so USB can start in setup())

// ---------------- Bluetooth LE ----------------
// One connection, two services: HID consumer control (volume keys with no app) and the Nordic UART
// Service carrying the same text protocol as USB.
static const char *NUS_UUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char *NUS_RX_UUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // host -> knob
static const char *NUS_TX_UUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // knob -> host
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,  // Consumer Control application
    0x85, 0x01,                          // report ID 1
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
    0x09, 0xE9, 0x09, 0xEA, 0x81, 0x02,  // bit 0 Volume Up, bit 1 Volume Down
    0x95, 0x06, 0x81, 0x03,              // padding
    0xC0};
static BLECharacteristic *bleTx = nullptr, *bleKeys = nullptr;
static volatile bool bleConnected = false;
static StreamBufferHandle_t bleRx;  // bytes written by the host, filled on the Bluetooth task

// Everything the knob prints goes to USB and, line by line, to BLE.
struct DualOut : Print {
  char line[192];
  size_t n = 0;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *b, size_t len) override {
    Link.write(b, len);
    for (size_t i = 0; i < len; i++) {
      if (n < sizeof(line)) line[n++] = b[i];
      if (b[i] != '\n' && n < sizeof(line)) continue;
      // shortcut: 20-byte chunks fit the default MTU; use the negotiated MTU if throughput matters
      for (size_t o = 0; bleConnected && o < n; o += 20) {
        bleTx->setValue((uint8_t *)line + o, min(n - o, (size_t)20));
        bleTx->notify();
      }
      n = 0;
    }
    return len;
  }
};
static DualOut Out;

// ---------------- State ----------------
struct HapticConfig {
  int32_t lo, hi;
  float stepDeg;  // knob rotation per value
  float detentV;  // detent strength (0 = smooth)
  float wallK;    // end-wall spring, volts per degree
};
static const HapticConfig STANDALONE_CFG = {0, VOL_STEPS, STEP_DEG, 0.0f, WALL_K};

static float vm = 0, umax = 0;
static bool driveOn = false, calibrated = false;
static int sensorDir = -1;    // +1 if increasing electrical angle increases the encoder angle
static float elecOffset = 0;  // electrical angle at raw encoder 0
static int torqueSign = 1;    // set by the torque self-test

static uint16_t lastRaw = 0;
static int32_t encAccum = 0;
static int encFails = 0;
static float mechDeg = 0, velDps = 0;

static HapticConfig cfg = STANDALONE_CFG;
static int32_t value = 0;     // current detent / value
static int32_t anchorVal = 0; // value at anchorDeg
static float anchorDeg = 0;   // mechanical angle where anchorVal sits

enum SyncState { WAIT_USB, SETTLE, ZEROING, READY };
static volatile bool usbMounted = false;
static SyncState syncState = WAIT_USB;
static uint32_t syncT0 = 0, lastKeyMs = 0;
static int pendingKeys = 0;  // >0: Volume Up presses to send, <0: Volume Down

static bool hostMode = false;
static uint32_t lastHostMs = 0, lastValMs = 0;
static bool valueDirty = false;
static int hostVolPct = -1;  // PC volume last reported by the app (-1 = unknown)
static bool debug = false;
static float dbgEMin = 1e9f, dbgEMax = -1e9f, dbgUMin = 1e9f, dbgUMax = -1e9f;  // per debug-print window
static uint32_t dbgLoops = 0;

// ---------------- DRV8316 ----------------
static uint16_t drvXfer(uint16_t frame) {
  if (__builtin_popcount(frame) & 1) frame |= 0x0100;  // even parity
  drvSpi.beginTransaction(drvSpiSettings);
  digitalWrite(PIN_DRV_CS, LOW);
  uint16_t r = drvSpi.transfer16(frame);
  digitalWrite(PIN_DRV_CS, HIGH);
  drvSpi.endTransaction();
  delayMicroseconds(2);
  return r;
}
static uint8_t drvRead(uint8_t addr) { return drvXfer((uint16_t)((addr << 1) | 0x80) << 8) & 0xFF; }
static void drvWrite(uint8_t addr, uint8_t v) { drvXfer(((uint16_t)(addr << 1) << 8) | v); }

static float readVm() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_VM_SENSE);
  return mv / 8.0f / 1000.0f * 11.0f;  // 100k / 10k divider
}

static void setDuty(int k, float d) { ledcWrite(k, (uint32_t)(constrain(d, 0.0f, 1.0f) * PWM_MAX)); }

// Sinusoidal phase voltages centred on VM/2. theta = electrical angle (rad).
static void setVoltageVector(float theta, float volts) {
  for (int k = 0; k < 3; k++) setDuty(k, 0.5f + volts * cosf(theta - k * TWO_PI_F / 3.0f) / vm);
}

static void disableDrive() {
  digitalWrite(PIN_DRV_DRVOFF, HIGH);
  for (int k = 0; k < 3; k++) {
    digitalWrite(PIN_INL[k], LOW);
    ledcWrite(k, 0);
  }
  digitalWrite(PIN_DRV_NSLEEP, LOW);
  driveOn = false;
}

static bool enableDrive() {
  digitalWrite(PIN_DRV_NSLEEP, HIGH);
  delay(5);
  drvWrite(REG_CTRL1, 0x03);  // unlock registers
  if ((drvRead(REG_CTRL1) & 0x07) != 0x03) {
    Out.println("DRV8316 not responding on SPI");
    disableDrive();
    return false;
  }
  uint8_t c2 = drvRead(REG_CTRL2);
  drvWrite(REG_CTRL2, (c2 & ~0x07) | (PWM_MODE_3X << 1) | 0x01);  // 3x PWM, clear faults
  delay(1);
  if (((drvRead(REG_CTRL2) >> 1) & 0x03) != PWM_MODE_3X) {
    Out.println("DRV8316: could not set 3x PWM mode");
    disableDrive();
    return false;
  }
  setVoltageVector(0, 0);
  for (int k = 0; k < 3; k++) digitalWrite(PIN_INL[k], HIGH);  // 3x mode: INL = phase enable
  digitalWrite(PIN_DRV_DRVOFF, LOW);
  delay(2);
  if (!digitalRead(PIN_DRV_NFAULT)) {
    Out.printf("DRV8316 fault after enable (IC_STATUS=0x%02X)\n", drvRead(REG_IC_STATUS));
    disableDrive();
    return false;
  }
  driveOn = true;
  return true;
}

// ---------------- Encoder ----------------
static bool encReadRaw(uint16_t &raw) {
  Wire.beginTransmission(0x36);
  Wire.write(0x0C);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x36, (uint8_t)2) != 2) return false;
  raw = (Wire.read() & 0x0F) << 8;
  raw |= Wire.read();
  return true;
}

// Updates the unwrapped mechanical angle (degrees). Returns false on an I2C error.
static bool encUpdate() {
  uint16_t raw;
  if (!encReadRaw(raw)) {
    encFails++;
    return false;
  }
  int32_t d = (int32_t)raw - lastRaw;
  if (d > 2048) d -= 4096;
  if (d < -2048) d += 4096;
  encAccum += d;
  lastRaw = raw;
  encFails = 0;
  mechDeg = encAccum * (360.0f / 4096.0f);
  return true;
}

static float elecAngle() {
  float e = sensorDir * POLE_PAIRS * (lastRaw * (TWO_PI_F / 4096.0f)) + elecOffset;
  e = fmodf(e, TWO_PI_F);
  return e < 0 ? e + TWO_PI_F : e;
}

// Voltage-mode torque: q-axis voltage, positive = pushes the encoder angle up.
static void applyTorque(float uMech) {
  setVoltageVector(elecAngle() + HALF_PI_F, uMech * sensorDir * torqueSign);
}

static void holdAndSettle(float theta, float volts, uint32_t ms) {
  setVoltageVector(theta, volts);
  uint32_t t0 = millis();
  while (millis() - t0 < ms) {
    encUpdate();
    delay(2);
  }
}

// ---------------- Value tracking ----------------
// The current knob angle becomes the centre of the current value's detent.
static void reanchor() {
  anchorDeg = mechDeg;
  anchorVal = value;
}

static void setValue(int32_t v) {
  value = constrain(v, cfg.lo, cfg.hi);
  reanchor();
}

// Align the rotor to find the sensor direction and electrical zero, then check the torque sign.
static bool calibrate() {
  Out.println("calibrating motor (the knob will move ~50 deg)...");
  if (!driveOn && !enableDrive()) return false;
  calibrated = false;
  holdAndSettle(0, CAL_VOLTS, 400);
  float m0 = mechDeg;
  for (int i = 1; i <= 200; i++) {  // one electrical turn forward
    setVoltageVector(i * TWO_PI_F / 200.0f, CAL_VOLTS);
    encUpdate();
    delay(3);
  }
  holdAndSettle(0, CAL_VOLTS, 300);
  float moved = mechDeg - m0;
  if (fabsf(moved) < 20.0f) {
    Out.printf("calibration failed: rotor moved only %.1f deg (check motor power / wiring)\n", moved);
    setVoltageVector(0, 0);
    return false;
  }
  sensorDir = moved > 0 ? 1 : -1;
  float pp = 360.0f / fabsf(moved);
  if (fabsf(pp - POLE_PAIRS) > 1.0f) Out.printf("warning: measured %.2f pole pairs, using %d\n", pp, POLE_PAIRS);
  // Rotor d-axis now sits at electrical angle 0.
  elecOffset = -sensorDir * POLE_PAIRS * (lastRaw * (TWO_PI_F / 4096.0f));
  calibrated = true;

  // Torque sign check: a short positive push must increase the encoder angle.
  torqueSign = 1;
  float t0deg = mechDeg;
  uint32_t t0 = millis();
  while (millis() - t0 < 150 && fabsf(mechDeg - t0deg) < 30.0f) {
    encUpdate();
    applyTorque(1.5f);
  }
  setVoltageVector(0, 0);
  float d = mechDeg - t0deg;
  if (d < -2.0f) torqueSign = -1;
  delay(200);
  encUpdate();
  Out.printf("calibration ok: sensorDir=%+d torqueSign=%+d (test push moved %+.1f deg)\n", sensorDir, torqueSign, d);
  return true;
}

// ---------------- Standalone media-key volume ----------------
static bool linkUp() { return usbMounted || bleConnected; }

static void sendKey(bool up) {
  if (usbMounted) {
    consumer.press(up ? CONSUMER_CONTROL_VOLUME_INCREMENT : CONSUMER_CONTROL_VOLUME_DECREMENT);
    consumer.release();
    return;
  }
  uint8_t r = up ? 0x01 : 0x02;  // bits of HID_REPORT_MAP
  bleKeys->setValue(&r, 1);
  bleKeys->notify();
  r = 0;
  bleKeys->setValue(&r, 1);
  bleKeys->notify();
}

struct BleServerCb : BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleConnected = true; }
  void onDisconnect(BLEServer *s) override {
    bleConnected = false;
    s->startAdvertising();
  }
};
struct BleRxCb : BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) override {
    std::string v = c->getValue();
    xStreamBufferSend(bleRx, v.data(), v.size(), 0);
  }
};

static void bleBegin() {
  bleRx = xStreamBufferCreate(512, 1);
  BLEDevice::init("SmartKnob");
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCb);

  BLEHIDDevice *hid = new BLEHIDDevice(server);
  bleKeys = hid->inputReport(1);
  hid->manufacturer()->setValue("SmartKnob");
  hid->pnp(0x02, 0x303A, 0x4B4E, 0x0100);
  hid->hidInfo(0x00, 0x01);
  hid->reportMap((uint8_t *)HID_REPORT_MAP, sizeof(HID_REPORT_MAP));
  hid->startServices();
  hid->setBatteryLevel(100);

  BLEService *nus = server->createService(NUS_UUID);
  bleTx = nus->createCharacteristic(NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  bleTx->addDescriptor(new BLE2902());
  BLECharacteristic *rx =
      nus->createCharacteristic(NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRxCb);
  nus->start();

  BLESecurity *sec = new BLESecurity();  // Just Works bonding, which Windows needs for HID
  sec->setAuthenticationMode(ESP_LE_AUTH_BOND);
  sec->setCapability(ESP_IO_CAP_NONE);

  BLEAdvertising *adv = server->getAdvertising();
  adv->setAppearance(0x03C1);  // keyboard: listed by Windows "Add device"
  adv->addServiceUUID(hid->hidService()->getUUID());
  adv->start();
}

static void usbEvent(void *, esp_event_base_t base, int32_t id, void *) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (id == ARDUINO_USB_STARTED_EVENT) usbMounted = true;
  if (id == ARDUINO_USB_STOPPED_EVENT) usbMounted = false;
}

static void enterStandalone() {
  hostMode = false;
  cfg = STANDALONE_CFG;
  pendingKeys = 0;
  if (hostVolPct >= 0) {  // the app told us the real level: no need to re-sync
    setValue((hostVolPct + VOL_STEP_PCT / 2) / VOL_STEP_PCT);
    syncState = READY;
  } else {
    setValue(value);
    syncState = WAIT_USB;
  }
  Out.println("desktop app gone: standalone volume knob");
}

static void updateSync() {
  if (!linkUp()) {
    if (hostMode) enterStandalone();
    syncState = WAIT_USB;
    pendingKeys = 0;
    hostVolPct = -1;  // may be a different PC next time
    return;
  }
  if (hostMode) {
    if (millis() - lastHostMs > HOST_TIMEOUT_MS) enterStandalone();
    return;
  }
  switch (syncState) {
    case WAIT_USB:
      syncState = SETTLE;
      syncT0 = millis();
      break;
    case SETTLE:
      if (millis() - syncT0 > USB_SETTLE_MS) {
        pendingKeys = -(VOL_STEPS + 3);  // drive the PC to 0 %
        syncState = ZEROING;
        Out.println("syncing PC volume: 0 %, then start level");
      }
      break;
    case ZEROING:
      if (pendingKeys == 0) {
        setValue(START_VOLUME_PCT / VOL_STEP_PCT);
        pendingKeys = value;
        syncState = READY;
      }
      break;
    case READY: break;
  }
}

static void sendPendingKeys() {
  if (hostMode || !linkUp() || pendingKeys == 0 || millis() - lastKeyMs < KEY_INTERVAL_MS) return;
  lastKeyMs = millis();
  bool up = pendingKeys > 0;
  sendKey(up);
  pendingKeys += up ? -1 : 1;
}

// ---------------- Control loop ----------------
static uint32_t lastLoopUs = 0;
static float prevMech = 0;

static void onValueStep(int d) {
  if (hostMode)
    valueDirty = true;
  else
    pendingKeys += d;
}

static void controlStep() {
  uint32_t now = micros();
  float dt = (now - lastLoopUs) * 1e-6f;
  lastLoopUs = now;
  bool ok = encUpdate();
  if (dt > 0 && dt < 0.05f) velDps += ((mechDeg - prevMech) / dt - velDps) * VEL_ALPHA;
  prevMech = mechDeg;

  const float step = cfg.stepDeg;
  float p = KNOB_DIR * (mechDeg - anchorDeg);  // degrees, value-up positive
  float v = KNOB_DIR * velDps;
  float pv = v > DAMP_DB_DPS ? v - DAMP_DB_DPS : v < -DAMP_DB_DPS ? v + DAMP_DB_DPS : 0;  // for damping
  float e = p - (value - anchorVal) * step;    // offset from the current value's detent centre
  bool active = hostMode || syncState == READY;

  if (active) {
    const float sw = (0.5f + STEP_HYST) * step;
    while (e > sw && value < cfg.hi) {
      value++;
      e -= step;
      onValueStep(1);
    }
    while (e < -sw && value > cfg.lo) {
      value--;
      e += step;
      onValueStep(-1);
    }
  }

  if (!driveOn || !calibrated) return;
  if (!ok && encFails > 20) {  // lost the encoder: never drive blind
    setVoltageVector(0, 0);
    return;
  }
  float u = 0;
  if (active) {
    if ((value >= cfg.hi && e > 0) || (value <= cfg.lo && e < 0)) {
      u = -cfg.wallK * e - WALL_KD * pv;  // end wall: spring back to the end
    } else if (cfg.detentV > 0) {
      float k = min(cfg.detentV / (0.3f * step), DETENT_KMAX);
      u = constrain(-k * e, -cfg.detentV, cfg.detentV) - DETENT_KD * pv;
    }
  }
  u = constrain(u, -umax, umax);
  applyTorque(KNOB_DIR * u);
  if (debug) {
    dbgEMin = min(dbgEMin, e), dbgEMax = max(dbgEMax, e);
    dbgUMin = min(dbgUMin, u), dbgUMax = max(dbgUMax, u);
    dbgLoops++;
  }
}

// ---------------- Power / fault supervision ----------------
static uint32_t lastPowerCheck = 0;

static void superviseDrive() {
  if (millis() - lastPowerCheck < 500) return;
  lastPowerCheck = millis();
  vm = readVm();
  umax = vm >= 8.0f ? UMAX_12V : UMAX_LOW;
  umax = min(umax, 0.45f * vm);
  if (driveOn && !digitalRead(PIN_DRV_NFAULT)) {
    Out.printf("DRV8316 fault (IC_STATUS=0x%02X), haptics off\n", drvRead(REG_IC_STATUS));
    disableDrive();
    calibrated = false;
    return;
  }
  if (vm < VM_MIN) {
    if (driveOn) {
      Out.printf("motor power lost (VM=%.1f V), haptics off\n", vm);
      disableDrive();
      calibrated = false;
    }
    return;
  }
  if (!calibrated) {
    static uint32_t lastTry = 0;
    if (lastTry && millis() - lastTry < 5000) return;  // back off after a failed attempt
    lastTry = millis();
    Out.printf("motor power present (VM=%.1f V)\n", vm);
    if (!calibrate()) disableDrive();
    encUpdate();
    prevMech = mechDeg;
    reanchor();  // calibration moved the knob; keep the current value
  }
}

static bool hapticsOn() { return driveOn && calibrated; }

static void printStatus() {
  Out.printf("USB %s, BLE %s, %s, sync %d, value %ld [%ld..%ld] step %.1f det %.2f, %d pending keys, knob %.1f deg, "
                "VM %.1f V, haptics %s, encoder %s\n",
                usbMounted ? "mounted" : "not mounted", bleConnected ? "connected" : "advertising", hostMode ? "app connected" : "standalone", syncState,
                (long)value, (long)cfg.lo, (long)cfg.hi, cfg.stepDeg, cfg.detentV, pendingKeys, mechDeg, vm,
                hapticsOn() ? "on" : "off", encFails ? "ERROR" : "ok");
}

// ---------------- Serial protocol ----------------
// Finds `key=` in a `CMD k=v k=v` line. Returns false if absent.
static bool argF(const char *line, const char *key, float &out) {
  size_t kl = strlen(key);
  for (const char *s = strchr(line, ' '); s; s = strchr(s + 1, ' ')) {
    if (strncmp(s + 1, key, kl) == 0 && s[1 + kl] == '=') {
      out = atof(s + 2 + kl);
      return true;
    }
  }
  return false;
}
static bool argI(const char *line, const char *key, int32_t &out) {
  float f;
  if (!argF(line, key, f)) return false;
  out = (int32_t)lroundf(f);
  return true;
}

static void hostSeen() {
  lastHostMs = millis();
  if (hostMode) return;
  hostMode = true;
  pendingKeys = 0;
  syncState = READY;
  Out.println("desktop app connected");
}

static void handleLine(const char *line) {
  if (strncmp(line, "HELLO", 5) == 0) {
    hostSeen();
    Out.printf("INFO name=SmartKnob fw=2 hap=%d\n", hapticsOn());
  } else if (strncmp(line, "PING", 4) == 0) {
    hostSeen();
    int32_t pct;
    if (argI(line, "vol", pct)) hostVolPct = constrain(pct, 0, 100);
    Out.printf("PONG hap=%d vm=%.1f\n", hapticsOn(), vm);
  } else if (strncmp(line, "CFG", 3) == 0) {
    hostSeen();
    HapticConfig c = cfg;
    int32_t v = value;
    argI(line, "lo", c.lo);
    argI(line, "hi", c.hi);
    argI(line, "val", v);
    argF(line, "step", c.stepDeg);
    argF(line, "det", c.detentV);
    argF(line, "wall", c.wallK);
    if (c.hi < c.lo || c.stepDeg < 0.5f) {
      Out.println("ERR bad CFG");
      return;
    }
    c.detentV = constrain(c.detentV, 0.0f, UMAX_12V);
    c.wallK = constrain(c.wallK, 0.0f, 1.0f);
    cfg = c;
    setValue(v);
  } else if (strncmp(line, "VAL", 3) == 0) {
    hostSeen();
    int32_t v;
    if (argI(line, "v", v)) setValue(v);
  } else if (strcmp(line, "s") == 0) {
    printStatus();
  } else if (strcmp(line, "d") == 0) {
    debug = !debug;
  } else if (line[0] == 'o') {  // test: shift the reference so the knob appears <deg> further up
    anchorDeg -= KNOB_DIR * atof(line + 1);
  } else if (strcmp(line, "c") == 0) {
    calibrated = false;
    if (vm >= VM_MIN && calibrate()) reanchor();
  } else if (line[0] == 'v' && !hostMode) {
    int pct = constrain(atoi(line + 1), 0, 100);
    pendingKeys = 0;
    setValue((pct + VOL_STEP_PCT / 2) / VOL_STEP_PCT);
    Out.printf("tracked volume set to %ld %%\n", (long)value * VOL_STEP_PCT);
  } else {
    Out.println("commands: s, d, c, v<pct>");
  }
}

// Non-blocking line reader (readStringUntil would stall the control loop on a partial line).
// One buffer per transport so half-lines from USB and BLE never mix.
struct LineBuf {
  char buf[160];
  size_t len = 0;
  void feed(char c) {
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = 0;
        handleLine(buf);
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
};

static void pollSerial() {
  static LineBuf usb, ble;
  while (Link.available()) usb.feed(Link.read());
  char tmp[32];
  size_t n;
  while ((n = xStreamBufferReceive(bleRx, tmp, sizeof(tmp), 0)) > 0)
    for (size_t i = 0; i < n; i++) ble.feed(tmp[i]);
}

static void reportValue() {
  if (!hostMode || !valueDirty || millis() - lastValMs < VAL_INTERVAL_MS) return;
  lastValMs = millis();
  valueDirty = false;
  Out.printf("VAL v=%ld\n", (long)value);
}

void setup() {
  pinMode(PIN_DRV_NSLEEP, OUTPUT);
  pinMode(PIN_DRV_DRVOFF, OUTPUT);
  pinMode(PIN_DRV_NFAULT, INPUT);
  for (int k = 0; k < 3; k++) {
    pinMode(PIN_INL[k], OUTPUT);
    ledcSetup(k, PWM_FREQ, PWM_BITS);
    ledcAttachPin(PIN_INH[k], k);
  }
  disableDrive();
  pinMode(PIN_DRV_CS, OUTPUT);
  digitalWrite(PIN_DRV_CS, HIGH);
  drvSpi.begin(PIN_DRV_SCLK, PIN_DRV_SDO, PIN_DRV_SDI, -1);
  analogReadResolution(12);

  // Own PID so the desktop app can tell the knob from the chip's USB-Serial/JTAG flashing port
  // (303A:1001), which it never opens. Needs CDC_ON_BOOT=0 so USB isn't started before setup().
  USB.PID(0x4B4E);
  USB.productName("SmartKnob");
  USB.manufacturerName("SmartKnob");
  USB.onEvent(usbEvent);
  consumer.begin();
  Link.begin(115200);
  Link.setTxTimeoutMs(0);  // never block the control loop when no terminal is open
  USB.begin();
  bleBegin();

  Wire.begin(PIN_ENC_SDA, PIN_ENC_SCL, 400000);
  encUpdate();
  encAccum = 0;
  encUpdate();
  prevMech = mechDeg;
  reanchor();
  lastLoopUs = micros();
}

void loop() {
  superviseDrive();
  updateSync();
  controlStep();
  sendPendingKeys();
  pollSerial();
  reportValue();

  static uint32_t lastDbg = 0;
  if (debug && millis() - lastDbg > 100) {
    lastDbg = millis();
    Out.printf("knob %7.1f deg  vel %7.1f  value %ld  pending %d  e [%.2f %.2f]  u [%.2f %.2f]  loops %lu\n",
                KNOB_DIR * (mechDeg - anchorDeg), velDps, (long)value, pendingKeys, dbgEMin, dbgEMax, dbgUMin,
                dbgUMax, (unsigned long)dbgLoops);
    dbgEMin = dbgUMin = 1e9f, dbgEMax = dbgUMax = -1e9f, dbgLoops = 0;
  }
}
