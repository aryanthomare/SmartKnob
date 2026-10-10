// Open-loop motor / phase test for the smartknob_driver board (DRV8316CR + AS5600).
// Nothing moves until a command is received over serial:
//   r       run the full test (status, phase step, pole-pair sweep, spin fwd/rev)
//   s       DRV8316 / supply / encoder status
//   p       phase step test (hold A, B, C, A and report rotor position + current)
//   n       pole-pair sweep
//   f / b   spin open-loop forward / backward for 3 s
//   v <V>   set drive voltage amplitude (default 1.5 V, max 4 V)
//   x       stop and disable outputs
// Any key aborts a spin.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include "driver/gpio.h"
#include "soc/gpio_periph.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include "driver/adc.h"

// Pins from mcu.kicad_sch
static constexpr int PIN_INHA = 33, PIN_INLA = 47;
static constexpr int PIN_INHB = 21, PIN_INLB = 18;
static constexpr int PIN_INHC = 17, PIN_INLC = 16;
static constexpr int PIN_DRV_CS = 12, PIN_DRV_SCLK = 13, PIN_DRV_SDI = 14, PIN_DRV_SDO = 15;
static constexpr int PIN_DRV_NSLEEP = 34, PIN_DRV_NFAULT = 48, PIN_DRV_DRVOFF = 35;
static constexpr int PIN_VM_SENSE = 7;
static constexpr int PIN_ISENSE[3] = {8, 9, 10};
static constexpr int PIN_ENC_SCL = 1, PIN_ENC_SDA = 2;

static constexpr int PIN_INH[3] = {PIN_INHA, PIN_INHB, PIN_INHC};
static constexpr int PIN_INL[3] = {PIN_INLA, PIN_INLB, PIN_INLC};

// DRV8316 registers
static constexpr uint8_t REG_IC_STATUS = 0x00, REG_STATUS1 = 0x01, REG_STATUS2 = 0x02;
static constexpr uint8_t REG_CTRL1 = 0x03, REG_CTRL2 = 0x04;
static constexpr uint8_t PWM_MODE_3X = 0b10;

// 20 kHz x 11 bit exceeds the LEDC divider range on this core; 10 bit works.
static constexpr int PWM_FREQ = 20000, PWM_BITS = 10, PWM_MAX = (1 << PWM_BITS) - 1;
static constexpr float CSA_GAIN_V_PER_A = 0.15f;  // DRV8316 default CSA_GAIN
static constexpr float TWO_PI_F = 6.28318531f;

static SPIClass drvSpi(FSPI);
static const SPISettings drvSpiSettings(1000000, MSBFIRST, SPI_MODE1);

static float driveVolts = 1.5f;
static float vm = 0;
static bool enabled = false;
static int polePairs = 0;
static float isenseOffsetMv[3] = {0, 0, 0};
static uint32_t ledcSetupFreq[3] = {0, 0, 0};

// ---------------- DRV8316 SPI ----------------
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

static void printDrvStatus() {
  uint8_t ic = drvRead(REG_IC_STATUS), s1 = drvRead(REG_STATUS1), s2 = drvRead(REG_STATUS2);
  Serial.printf("DRV8316: IC_STATUS=0x%02X STATUS1=0x%02X STATUS2=0x%02X CTRL1=0x%02X CTRL2=0x%02X nFAULT=%s\n", ic, s1,
                s2, drvRead(REG_CTRL1), drvRead(REG_CTRL2), digitalRead(PIN_DRV_NFAULT) ? "high(ok)" : "LOW(fault)");
  if (ic & 0x01) {
    Serial.print("  faults:");
    if (ic & 0x02) Serial.print(" OT");
    if (ic & 0x04) Serial.print(" OVP");
    if (!(ic & 0x08)) Serial.print(" NPOR(power-on reset latched, cleared by CLR_FLT)");
    if (ic & 0x10) Serial.print(" OCP");
    if (ic & 0x20) Serial.print(" SPI_FLT");
    if (ic & 0x40) Serial.print(" BK_FLT");
    if (s2 & 0x08) Serial.print(" VCP_UV");
    Serial.println();
  }
}

// ---------------- Measurements ----------------
static float readVm() {
  uint32_t mv = 0;
  for (int i = 0; i < 16; i++) mv += analogReadMilliVolts(PIN_VM_SENSE);
  return mv / 16.0f / 1000.0f * 11.0f;  // 100k / 10k divider
}

static void readIsenseMv(float out[3]) {
  uint32_t acc[3] = {0, 0, 0};
  for (int n = 0; n < 64; n++)
    for (int k = 0; k < 3; k++) acc[k] += analogReadMilliVolts(PIN_ISENSE[k]);
  for (int k = 0; k < 3; k++) out[k] = acc[k] / 64.0f;
}

static uint16_t lastRaw = 0;
static int32_t encAccum = 0;
static bool encOk = false;

static bool encReadRaw(uint16_t &raw) {
  Wire.beginTransmission(0x36);
  Wire.write(0x0C);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)0x36, (uint8_t)2) != 2) return false;
  raw = ((Wire.read() & 0x0F) << 8);
  raw |= Wire.read();
  return true;
}

// Unwrapped mechanical angle in degrees (must be called faster than half a turn).
static float encUpdate() {
  uint16_t raw;
  if (encReadRaw(raw)) {
    int32_t d = (int32_t)raw - lastRaw;
    if (d > 2048) d -= 4096;
    if (d < -2048) d += 4096;
    encAccum += d;
    lastRaw = raw;
    encOk = true;
  } else {
    encOk = false;
  }
  return encAccum * 360.0f / 4096.0f;
}

// ---------------- Drive ----------------
static void setDuty(int k, float d) {
  d = constrain(d, 0.0f, 1.0f);
  ledcWrite(k, (uint32_t)(d * PWM_MAX));
}

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
  enabled = false;
}

static bool enableDrive() {
  if (!ledcSetupFreq[0] || !ledcSetupFreq[1] || !ledcSetupFreq[2]) {
    Serial.println("ABORT: LEDC PWM setup failed");
    return false;
  }
  vm = readVm();
  Serial.printf("VM = %.2f V\n", vm);
  if (vm < 8.0f || vm > 16.0f) {
    Serial.println("ABORT: VM not in 8-16 V. Is the barrel jack plugged in?");
    return false;
  }
  digitalWrite(PIN_DRV_NSLEEP, HIGH);
  delay(5);
  drvWrite(REG_CTRL1, 0x03);  // unlock registers
  if ((drvRead(REG_CTRL1) & 0x07) != 0x03) {
    Serial.printf("ABORT: DRV8316 SPI not responding (CTRL1=0x%02X)\n", drvRead(REG_CTRL1));
    return false;
  }
  uint8_t c2 = drvRead(REG_CTRL2);
  c2 = (c2 & ~0x07) | (PWM_MODE_3X << 1) | 0x01;  // 3x PWM, clear faults
  drvWrite(REG_CTRL2, c2);
  delay(1);
  if (((drvRead(REG_CTRL2) >> 1) & 0x03) != PWM_MODE_3X) {
    Serial.println("ABORT: could not set 3x PWM mode");
    return false;
  }

  // 3x mode: INHx = PWM, INLx = enable. Start with zero differential voltage.
  setVoltageVector(0, 0);
  for (int k = 0; k < 3; k++) digitalWrite(PIN_INL[k], HIGH);
  digitalWrite(PIN_DRV_DRVOFF, LOW);
  delay(2);
  // Zero-current offsets of the current sense amps (bridge active, zero differential voltage).
  readIsenseMv(isenseOffsetMv);
  Serial.printf("ISENSE offsets (zero vector): %.0f %.0f %.0f mV\n", isenseOffsetMv[0], isenseOffsetMv[1],
                isenseOffsetMv[2]);
  enabled = true;
  printDrvStatus();
  if (!digitalRead(PIN_DRV_NFAULT)) {
    Serial.println("ABORT: nFAULT low after enable");
    disableDrive();
    return false;
  }
  return true;
}

static bool faultCheck() {
  if (!digitalRead(PIN_DRV_NFAULT)) {
    disableDrive();
    Serial.println("FAULT: nFAULT went low, outputs disabled");
    printDrvStatus();
    return true;
  }
  return false;
}

// Hold a vector for ms while keeping the encoder unwrapped.
static float holdVector(float theta, uint32_t ms) {
  setVoltageVector(theta, driveVolts);
  uint32_t t0 = millis();
  float a = encUpdate();
  while (millis() - t0 < ms) {
    a = encUpdate();
    delay(5);
  }
  return a;
}

// ---------------- Tests ----------------
static void printStatus() {
  Serial.printf("VM = %.2f V, drive amplitude = %.2f V, pole pairs = %d\n", readVm(), driveVolts, polePairs);
  float a = encUpdate();
  Serial.printf("encoder: %s, angle (unwrapped) = %.2f deg, raw = %u\n", encOk ? "ok" : "READ FAILED", a, lastRaw);
  printDrvStatus();
}

static bool phaseStepTest() {
  Serial.printf("\n--- Phase step test (%.2f V) ---\n", driveVolts);
  if (!enableDrive()) return false;
  const char *names[4] = {"A", "B", "C", "A"};
  float pos[4];
  for (int i = 0; i < 4; i++) {
    pos[i] = holdVector(i * TWO_PI_F / 3.0f, 800);
    float mv[3];
    readIsenseMv(mv);
    Serial.printf("  raw ISENSE %.0f %.0f %.0f mV\n", mv[0], mv[1], mv[2]);
    Serial.printf("hold %s: rotor = %8.2f deg   approx I(A,B,C) = %+.2f %+.2f %+.2f A\n", names[i], pos[i],
                  (mv[0] - isenseOffsetMv[0]) / 1000.0f / CSA_GAIN_V_PER_A,
                  (mv[1] - isenseOffsetMv[1]) / 1000.0f / CSA_GAIN_V_PER_A,
                  (mv[2] - isenseOffsetMv[2]) / 1000.0f / CSA_GAIN_V_PER_A);
    if (faultCheck()) return false;
  }
  setVoltageVector(0, 0);
  float d1 = pos[1] - pos[0], d2 = pos[2] - pos[1], d3 = pos[3] - pos[2];
  Serial.printf("steps: A->B %+.2f, B->C %+.2f, C->A %+.2f deg (should be equal size and sign)\n", d1, d2, d3);
  float mean = (fabsf(d1) + fabsf(d2) + fabsf(d3)) / 3.0f;
  bool sameSign = (d1 > 0) == (d2 > 0) && (d2 > 0) == (d3 > 0);
  bool even = fabsf(fabsf(d1) - mean) < 0.35f * mean && fabsf(fabsf(d2) - mean) < 0.35f * mean &&
              fabsf(fabsf(d3) - mean) < 0.35f * mean;
  if (mean < 0.5f)
    Serial.println("RESULT: rotor did not move - check motor connector / VM / drive voltage");
  else if (sameSign && even)
    Serial.printf("RESULT: all 3 phases OK. A->B->C turns the encoder %s.\n", d1 > 0 ? "POSITIVE (angle increasing)"
                                                                                     : "NEGATIVE (angle decreasing)");
  else
    Serial.println("RESULT: uneven steps - a phase may be open, or friction/cogging is high (try v 2.5)");
  return true;
}

static bool polePairSweep() {
  Serial.println("\n--- Pole-pair sweep (4 electrical turns) ---");
  if (!enabled && !enableDrive()) return false;
  float start = holdVector(0, 500);
  const int steps = 4 * 360;
  for (int i = 1; i <= steps; i++) {
    setVoltageVector(i * TWO_PI_F / 360.0f, driveVolts);
    encUpdate();
    delay(3);
    if (i % 100 == 0 && faultCheck()) return false;
  }
  float end = holdVector(0, 300);
  float mech = end - start;
  setVoltageVector(0, 0);
  if (fabsf(mech) < 5) {
    Serial.printf("RESULT: rotor moved only %.2f deg - check wiring / voltage\n", mech);
    return false;
  }
  float pp = 4 * 360.0f / fabsf(mech);
  polePairs = (int)lroundf(pp);
  Serial.printf("4 electrical turns -> %+.2f mechanical deg => %.2f pole pairs (~%d, %d magnets)\n", mech, pp,
                polePairs, polePairs * 2);
  Serial.printf("RESULT: forward (A->B->C) = encoder %s\n", mech > 0 ? "POSITIVE" : "NEGATIVE");
  return true;
}

static bool spin(int dir, float revPerSec, uint32_t ms) {
  int pp = polePairs > 0 ? polePairs : 7;
  Serial.printf("\n--- Spin %s at %.2f rev/s for %lu ms (pp=%d) ---\n", dir > 0 ? "FORWARD (A->B->C)" : "BACKWARD (A->C->B)",
                revPerSec, (unsigned long)ms, pp);
  if (!enabled && !enableDrive()) return false;
  while (Serial.available()) Serial.read();
  float omega = dir * TWO_PI_F * pp * revPerSec;
  float theta = 0, mStart = 0;
  uint32_t t0 = micros(), tPrev = t0;
  bool measured = false;
  encUpdate();
  while ((micros() - t0) < ms * 1000UL) {
    uint32_t now = micros();
    theta += omega * (now - tPrev) * 1e-6f;
    tPrev = now;
    setVoltageVector(theta, driveVolts);
    float a = encUpdate();
    if (!measured && (now - t0) > 1000000UL) {
      mStart = a;
      measured = true;
    }
    if (Serial.available()) {
      Serial.println("aborted");
      break;
    }
    if (faultCheck()) return false;
    delayMicroseconds(500);
  }
  float elapsed = (micros() - t0) * 1e-6f - 1.0f;
  float mEnd = encUpdate();
  setVoltageVector(0, 0);
  if (elapsed > 0.2f) {
    float rps = (mEnd - mStart) / 360.0f / elapsed;
    Serial.printf("commanded %+.2f rev/s, encoder measured %+.2f rev/s\n", dir * revPerSec, rps);
    if (fabsf(rps) < 0.1f * revPerSec)
      Serial.println("RESULT: rotor not following (stalled) - try higher voltage (v 2.5) or check phases");
    else
      Serial.printf("RESULT: spinning, encoder %s\n", rps > 0 ? "POSITIVE" : "NEGATIVE");
  }
  return true;
}

// Pad-level loopback of the control pins, with the bridge held Hi-Z (DRVOFF high, INLx low).
static float highFraction(int pin) {
  uint32_t hi = 0;
  for (int i = 0; i < 20000; i++) hi += gpio_get_level((gpio_num_t)pin);
  return hi / 20000.0f;
}

static void gpioDiag() {
  Serial.println("\n--- GPIO loopback (bridge Hi-Z) ---");
  disableDrive();
  Serial.printf("ledcSetup returned %u %u %u Hz\n", ledcSetupFreq[0], ledcSetupFreq[1], ledcSetupFreq[2]);
  const int pins[] = {PIN_INHA, PIN_INHB, PIN_INHC, PIN_INLA, PIN_INLB, PIN_INLC, PIN_DRV_DRVOFF, PIN_DRV_NSLEEP};
  for (int p : pins) PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[p]);

  const float duties[] = {0.0f, 0.25f, 0.5f, 0.75f};
  for (float d : duties) {
    for (int k = 0; k < 3; k++) ledcWrite(k, (uint32_t)(d * PWM_MAX));
    delay(5);
    Serial.printf("INH set %.2f -> measured A %.2f  B %.2f  C %.2f\n", d, highFraction(PIN_INHA), highFraction(PIN_INHB),
                  highFraction(PIN_INHC));
  }
  for (int k = 0; k < 3; k++) {
    ledcWrite(k, PWM_MAX / 2);
    Serial.printf("LEDC ch%d: freq=%u duty=%u\n", k, ledcReadFreq(k), ledcRead(k));
  }
  for (int k = 0; k < 3; k++) ledcWrite(k, 0);

  // Plain GPIO drive of the INH pads, then re-attach LEDC.
  for (int k = 0; k < 3; k++) {
    ledcDetachPin(PIN_INH[k]);
    pinMode(PIN_INH[k], OUTPUT);
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[PIN_INH[k]]);
    digitalWrite(PIN_INH[k], HIGH);
    delay(1);
    int h = gpio_get_level((gpio_num_t)PIN_INH[k]);
    digitalWrite(PIN_INH[k], LOW);
    delay(1);
    int l = gpio_get_level((gpio_num_t)PIN_INH[k]);
    Serial.printf("INH%c (IO%d) as GPIO: write 1 -> %d, write 0 -> %d\n", 'A' + k, PIN_INH[k], h, l);
    ledcAttachPin(PIN_INH[k], k);
    ledcWrite(k, 0);
  }

  for (int k = 0; k < 3; k++) {
    digitalWrite(PIN_INL[k], HIGH);
    delay(1);
    int h = gpio_get_level((gpio_num_t)PIN_INL[k]);
    digitalWrite(PIN_INL[k], LOW);
    delay(1);
    int l = gpio_get_level((gpio_num_t)PIN_INL[k]);
    Serial.printf("INL%c (IO%d): write 1 -> %d, write 0 -> %d\n", 'A' + k, PIN_INL[k], h, l);
  }
  digitalWrite(PIN_DRV_DRVOFF, LOW);
  delay(1);
  int l = gpio_get_level((gpio_num_t)PIN_DRV_DRVOFF);
  digitalWrite(PIN_DRV_DRVOFF, HIGH);
  delay(1);
  Serial.printf("DRVOFF (IO35): write 0 -> %d, write 1 -> %d\n", l, gpio_get_level((gpio_num_t)PIN_DRV_DRVOFF));
  Serial.printf("nSLEEP (IO34): currently written %d, reads %d\n", enabled, gpio_get_level((gpio_num_t)PIN_DRV_NSLEEP));
}

// ---------------- Current-sense (SOx) diagnostics ----------------
static void printPinCfg(int pin) {
  uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[pin]);
  uint32_t outSel = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * pin) & 0x1FF;
  uint32_t oe = pin < 32 ? (REG_READ(GPIO_ENABLE_REG) >> pin) & 1 : (REG_READ(GPIO_ENABLE1_REG) >> (pin - 32)) & 1;
  Serial.printf("  IO%-2d IO_MUX=0x%04X MCU_SEL=%u FUN_IE=%u WPU=%u WPD=%u OUT_SEL=%u%s OE=%u\n", pin, mux,
                (mux >> 12) & 7, (mux >> 9) & 1, (mux >> 8) & 1, (mux >> 7) & 1, outSel,
                outSel == 256 ? "(gpio)" : "(PERIPHERAL)", oe);
}

static void rippleTest(float theta, const char *label) {
  setVoltageVector(theta, driveVolts);
  delay(300);
  Serial.printf("hold %s: ", label);
  for (int k = 0; k < 3; k++) {
    adc1_channel_t ch = (adc1_channel_t)digitalPinToAnalogChannel(PIN_ISENSE[k]);
    int mn = 4095, mx = 0;
    uint32_t acc = 0;
    for (int i = 0; i < 2000; i++) {
      int r = adc1_get_raw(ch);
      mn = min(mn, r);
      mx = max(mx, r);
      acc += r;
    }
    Serial.printf(" SO%c min/mean/max %4d/%4u/%4d  ", 'A' + k, mn, acc / 2000, mx);
  }
  Serial.println();
}

// Briefly drive a sense pad (weakest drive) then release and read back with the ADC.
// Distinguishes: shorted to GND at the pad / held low through R2x by the DRV / open net.
static void kickTest(const char *label) {
  Serial.printf("kick test [%s]: drive 1 -> readback, then ADC mV at ~0/1/5 ms after release\n", label);
  for (int k = 0; k < 3; k++) {
    int pin = PIN_ISENSE[k];
    for (int level = 1; level >= 0; level--) {
      pinMode(pin, OUTPUT);
      gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_0);
      PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[pin]);
      digitalWrite(pin, level);
      delayMicroseconds(200);
      int rb = gpio_get_level((gpio_num_t)pin);
      uint32_t t0 = micros();
      uint32_t v0 = analogReadMilliVolts(pin);  // switches pad back to analog (releases drive)
      uint32_t t1 = micros();
      delay(1);
      uint32_t v1 = analogReadMilliVolts(pin);
      delay(4);
      uint32_t v5 = analogReadMilliVolts(pin);
      Serial.printf("  SO%c (IO%d) drive %d: readback %d   after release %4u (%luus) %4u %4u mV\n", 'A' + k, pin,
                    level, rb, v0, (unsigned long)(t1 - t0), v1, v5);
    }
  }
}

static void senseDiag() {
  Serial.println("\n--- Current-sense diagnostic ---");
  disableDrive();
  digitalWrite(PIN_DRV_NSLEEP, LOW);
  delay(5);
  for (int k = 0; k < 3; k++) analogReadMilliVolts(PIN_ISENSE[k]);
  Serial.println("pin config:");
  for (int k = 0; k < 3; k++) printPinCfg(PIN_ISENSE[k]);
  kickTest("DRV asleep");

  digitalWrite(PIN_DRV_NSLEEP, HIGH);
  delay(5);
  Serial.printf("CTRL5 (CSA gain etc) = 0x%02X\n", drvRead(0x07));
  kickTest("DRV awake, bridge Hi-Z");

  if (!enableDrive()) return;
  kickTest("bridge on, zero vector");
  Serial.println("pin config (bridge on):");
  for (int k = 0; k < 3; k++) printPinCfg(PIN_ISENSE[k]);

  rippleTest(0, "A");
  rippleTest(TWO_PI_F / 3, "B");
  rippleTest(2 * TWO_PI_F / 3, "C");
  disableDrive();
  Serial.println("=== sense diag done, outputs disabled ===");
}

// Drive current through one winding pair at a time (third phase Hi-Z). The low phase's FET is
// always on, so its CSA sees the full current (SOB is unusable on these boards; rotor motion
// is the second indicator).
static void pairTest() {
  Serial.printf("\n--- Winding pair test (%.2f V across the pair) ---\n", driveVolts);
  if (!enableDrive()) return;
  const char names[] = "ABC";
  const int pairs[6][2] = {{0, 2}, {2, 0}, {1, 0}, {1, 2}, {0, 1}, {2, 1}};
  float d = driveVolts / vm;
  for (auto &p : pairs) {
    int hi = p[0], lo = p[1], off = 3 - hi - lo;
    for (int k = 0; k < 3; k++) {
      setDuty(k, 0);
      digitalWrite(PIN_INL[k], HIGH);
    }
    digitalWrite(PIN_INL[off], LOW);  // third phase Hi-Z
    delay(50);
    float a0 = encUpdate();
    setDuty(hi, d);
    uint32_t t0 = millis();
    float a1 = a0;
    while (millis() - t0 < 500) {
      a1 = encUpdate();
      delay(5);
    }
    float mv[3];
    readIsenseMv(mv);
    setDuty(hi, 0);
    if (faultCheck()) return;
    float amps = (mv[lo] - isenseOffsetMv[lo]) / 1000.0f / CSA_GAIN_V_PER_A;
    if (lo == 1)
      Serial.printf("  %c -> %c (%c off): current n/a (SOB)      rotor moved %+7.2f deg\n", names[hi], names[lo],
                    names[off], a1 - a0);
    else
      Serial.printf("  %c -> %c (%c off): I(SO%c) = %+.3f A   rotor moved %+7.2f deg\n", names[hi], names[lo],
                    names[off], names[lo], amps, a1 - a0);
    delay(200);
  }
  for (int k = 0; k < 3; k++) digitalWrite(PIN_INL[k], HIGH);
  setVoltageVector(0, 0);
  disableDrive();
  Serial.println("A pair that shows no current and no rotor motion has an open winding/path.");
}

static void runAll() {
  digitalWrite(PIN_DRV_NSLEEP, HIGH);  // wake so SPI status is valid
  delay(5);
  printStatus();
  if (phaseStepTest() && polePairSweep()) {
    spin(+1, 1.0f, 3000);
    spin(-1, 1.0f, 3000);
  }
  disableDrive();
  printDrvStatus();
  Serial.println("\n=== done, outputs disabled ===");
}

void setup() {
  // Driver asleep and outputs Hi-Z until a test is requested.
  pinMode(PIN_DRV_NSLEEP, OUTPUT);
  digitalWrite(PIN_DRV_NSLEEP, LOW);
  pinMode(PIN_DRV_DRVOFF, OUTPUT);
  digitalWrite(PIN_DRV_DRVOFF, HIGH);
  pinMode(PIN_DRV_NFAULT, INPUT);
  for (int k = 0; k < 3; k++) {
    pinMode(PIN_INL[k], OUTPUT);
    digitalWrite(PIN_INL[k], LOW);
    ledcSetupFreq[k] = ledcSetup(k, PWM_FREQ, PWM_BITS);
    ledcAttachPin(PIN_INH[k], k);
    ledcWrite(k, 0);
  }
  pinMode(PIN_DRV_CS, OUTPUT);
  digitalWrite(PIN_DRV_CS, HIGH);
  drvSpi.begin(PIN_DRV_SCLK, PIN_DRV_SDO, PIN_DRV_SDI, -1);
  analogReadResolution(12);

  Serial.begin(115200);
  Wire.begin(PIN_ENC_SDA, PIN_ENC_SCL, 400000);
  encUpdate();
  encAccum = 0;
  Serial.println("\nsmartknob_driver motor test ready. Commands: r s p n f b v x");
}

void loop() {
  if (!Serial.available()) {
    delay(10);
    return;
  }
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;
  char c = line[0];
  switch (c) {
    case 'r': runAll(); break;
    case 's':
      if (!enabled) {
        digitalWrite(PIN_DRV_NSLEEP, HIGH);
        delay(5);
      }
      printStatus();
      break;
    case 'p': phaseStepTest(); disableDrive(); break;
    case 'n': polePairSweep(); disableDrive(); break;
    case 'f': spin(+1, 1.0f, 3000); disableDrive(); break;
    case 'b': spin(-1, 1.0f, 3000); disableDrive(); break;
    case 'v': {
      float v = line.substring(1).toFloat();
      if (v > 0 && v <= 4.0f) driveVolts = v;
      Serial.printf("drive amplitude = %.2f V\n", driveVolts);
      break;
    }
    case 'g': gpioDiag(); break;
    case 'a': senseDiag(); break;
    case 'w': pairTest(); break;
    case 'x': disableDrive(); Serial.println("disabled"); break;
    default: Serial.println("commands: r s p n f b v<V> x"); break;
  }
}
