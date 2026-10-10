// AS5600 encoder read test for the smartknob_driver board.
// Pins from mcu.kicad_sch: encoder I2C SCL IO1, SDA IO2.
#include <Arduino.h>
#include <Wire.h>

static constexpr int PIN_ENC_SCL = 1;
static constexpr int PIN_ENC_SDA = 2;
static constexpr int PIN_DRV_NSLEEP = 34;
static constexpr int PIN_DRV_DRVOFF = 35;

static constexpr uint8_t AS5600_ADDR = 0x36;
static constexpr uint8_t REG_RAW_ANGLE = 0x0C;
static constexpr uint8_t REG_ANGLE = 0x0E;
static constexpr uint8_t REG_STATUS = 0x0B;
static constexpr uint8_t REG_AGC = 0x1A;
static constexpr uint8_t REG_MAGNITUDE = 0x1B;

static bool readRegs(uint8_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AS5600_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool read12(uint8_t reg, uint16_t &out) {
  uint8_t b[2];
  if (!readRegs(reg, b, 2)) return false;
  out = ((uint16_t)(b[0] & 0x0F) << 8) | b[1];
  return true;
}

static void scanBus() {
  Serial.println("I2C scan:");
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", addr, addr == AS5600_ADDR ? " (AS5600)" : "");
      found++;
    }
  }
  if (!found) Serial.println("  no devices found - check J3 cable orientation / 3V3 / pull-ups");
}

void setup() {
  // Keep the DRV8316 asleep and its outputs Hi-Z (matches the board's pull resistors).
  pinMode(PIN_DRV_NSLEEP, OUTPUT);
  digitalWrite(PIN_DRV_NSLEEP, LOW);
  pinMode(PIN_DRV_DRVOFF, OUTPUT);
  digitalWrite(PIN_DRV_DRVOFF, HIGH);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);
  delay(500);
  Serial.println("\nsmartknob_driver AS5600 encoder test");

  Wire.begin(PIN_ENC_SDA, PIN_ENC_SCL, 400000);
  scanBus();
}

void loop() {
  static uint32_t lastScan = 0;
  uint8_t status, agc, mag[2];
  uint16_t raw, angle;

  bool ok = readRegs(REG_STATUS, &status, 1) && readRegs(REG_AGC, &agc, 1) &&
            readRegs(REG_MAGNITUDE, mag, 2) && read12(REG_RAW_ANGLE, raw) && read12(REG_ANGLE, angle);
  if (!ok) {
    Serial.println("AS5600 read failed (no ACK at 0x36)");
    if (millis() - lastScan > 5000) {
      scanBus();
      lastScan = millis();
    }
    delay(500);
    return;
  }

  uint16_t magnitude = ((uint16_t)(mag[0] & 0x0F) << 8) | mag[1];
  const char *magnet = (status & 0x20) ? ((status & 0x10) ? "WEAK" : (status & 0x08) ? "STRONG" : "OK") : "NONE";
  Serial.printf("raw=%4u  angle=%7.2f deg  magnet=%-6s agc=%3u  mag=%4u  status=0x%02X\n", raw,
                raw * 360.0f / 4096.0f, magnet, agc, magnitude, status);
  delay(100);
}
