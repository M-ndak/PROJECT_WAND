// Magic wand firmware - Arduino Uno R3
// MPU6050 (0x68) + TTP223 touch on D2 + digital IR proximity on D3
// No external libraries. Cooperative scheduler instead of an RTOS.
//
// Machine protocol (9600 baud, newline terminated) - used by wand_pc.py:
//   S,<0|1>                 sleep state (1 = awake)
//   G,<NAME>                gesture: SWISH_L SWISH_R FLICK_UP FLICK_DOWN
//                                    SHAKE TWIRL THRUST
//   P,<NAME>                proximity: NEAR, FAR
//
// Human log lines start with '#', e.g.  "# [GESTURE] SWISH_R"
// The PC script ignores them.

#include <Wire.h>

// ---------- Serial ----------
#define BAUD_RATE 9600

// ---------- Logging ----------
#define VERBOSE 1   // 1 = print human-readable events, 0 = protocol lines only

// ---------- IR proximity ----------
#define PROX_ACTIVE_LOW 1   // 0 = OUT is HIGH when an object is detected (your sensor)

// ---------- Pins ----------
const uint8_t TOUCH_PIN        = 2;
const uint8_t PROX_DIGITAL_PIN = 3;

// ---------- I2C addresses ----------
const uint8_t MPU_ADDR = 0x68;

// ---------- Axis sign fixes (flip to -1 if a gesture is mirrored) ----------
#define SIGN_YAW    1   // gz : left/right swish
#define SIGN_PITCH  1   // gy : up/down flick
#define SIGN_THRUST 1   // ax : forward thrust

// ---------- Timing (ms) ----------
const uint16_t IMU_PERIOD       = 10;
const uint16_t PROX_PERIOD      = 20;
const uint16_t TOUCH_DEBOUNCE   = 250;
const uint16_t GESTURE_COOLDOWN = 400;
const uint16_t SHAKE_WINDOW     = 400;
const uint16_t PENDING_DELAY    = 250;

// ---------- Gesture thresholds ----------
const int SWING_START_DPS = 250;   // start of a swing
const int SWING_END_DPS   = 80;    // swing finished
const int TWIRL_MIN_DPS   = 150;
const int TWIRL_ANGLE     = 300;   // degrees of roll to count as twirl
const int THRUST_G_RAW    = 16000; // ~2 g at +-4 g range (8192 LSB/g)
const int THRUST_MAX_ROT  = 150;   // dps, thrust must be mostly linear

// ---------- State ----------
bool awake = true;
bool lastTouch = false;
unsigned long lastTouchToggle = 0;
unsigned long tImu = 0, tProx = 0;
unsigned long lastGestureTime = 0;

// swing tracking
bool swinging = false;
uint8_t swingAxis = 0;      // 1 = pitch, 2 = yaw
int8_t swingSign = 0;
unsigned long swingStart = 0;

// pending (delayed) single gesture so SHAKE can override it
const char* pendingName = nullptr;
unsigned long pendingTime = 0;

// shake tracking
uint8_t lastAxis = 0;
int8_t lastSign = 0;
unsigned long lastSwingEnd = 0;
uint8_t shakeCount = 0;

// twirl tracking
float rollAccum = 0;

// proximity tracking
bool proxNear = false;
bool proxCandidate = false;
uint8_t proxCount = 0;

// error rate limiting
unsigned long lastImuErr = 0;

// ---------- Log helpers ----------
#if VERBOSE
  #define LOG_BEGIN(tag)  do { Serial.print(F("# [")); Serial.print(F(tag)); Serial.print(F("] ")); } while (0)
  #define LOG_END()       Serial.println()
  #define LOGF(tag, msg)  do { LOG_BEGIN(tag); Serial.print(F(msg)); LOG_END(); } while (0)
#else
  #define LOG_BEGIN(tag)  do { } while (0)
  #define LOG_END()       do { } while (0)
  #define LOGF(tag, msg)  do { } while (0)
#endif

// ---------- I2C helpers ----------
void writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

int readReg8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return -1;
  return Wire.read();
}

// ---------- MPU6050 ----------
void mpuWake() {
  writeReg(MPU_ADDR, 0x6B, 0x01);  // wake, PLL X gyro clock
  writeReg(MPU_ADDR, 0x1B, 0x08);  // gyro +-500 dps (65.5 LSB/dps)
  writeReg(MPU_ADDR, 0x1C, 0x08);  // accel +-4 g   (8192 LSB/g)
  writeReg(MPU_ADDR, 0x1A, 0x03);  // digital low-pass ~44 Hz
}

void mpuSleep() {
  writeReg(MPU_ADDR, 0x6B, 0x40);  // sleep bit
}

bool mpuRead(int16_t &ax, int16_t &ay, int16_t &az,
             int16_t &gx, int16_t &gy, int16_t &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (uint8_t)14) != 14) return false;
  ax = (Wire.read() << 8) | Wire.read();
  ay = (Wire.read() << 8) | Wire.read();
  az = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();  // temperature, unused
  gx = (Wire.read() << 8) | Wire.read();
  gy = (Wire.read() << 8) | Wire.read();
  gz = (Wire.read() << 8) | Wire.read();
  return true;
}

// ---------- Output ----------
void sendGesture(const char* name) {
  Serial.print(F("G,"));
  Serial.println(name);
  LOG_BEGIN("GESTURE");
#if VERBOSE
  Serial.print(name);
#endif
  LOG_END();
  lastGestureTime = millis();
}

void sendProx(const char* name) {
  Serial.print(F("P,"));
  Serial.println(name);
  LOG_BEGIN("PROX");
#if VERBOSE
  Serial.print(name);
#endif
  LOG_END();
}

void sendState() {
  Serial.print(F("S,"));
  Serial.println(awake ? 1 : 0);
}

// ---------- Sleep / wake ----------
void setAwake(bool a) {
  awake = a;
  if (awake) {
    mpuWake();
    swinging = false;
    pendingName = nullptr;
    shakeCount = 0;
    rollAccum = 0;
    proxNear = false;
    proxCandidate = false;
    proxCount = 0;
    LOGF("STATE", "AWAKE - sensors powered up");
  } else {
    mpuSleep();
    LOGF("STATE", "SLEEP - sensors powered down");
  }
  sendState();
}

void handleTouch() {
  bool t = digitalRead(TOUCH_PIN) == HIGH;
  unsigned long now = millis();
  if (t && !lastTouch) {
    if (now - lastTouchToggle > TOUCH_DEBOUNCE) {
      LOGF("TOUCH", "pressed -> toggling sleep state");
      lastTouchToggle = now;
      setAwake(!awake);
    } else {
      LOGF("TOUCH", "pressed (ignored, debounce)");
    }
  } else if (!t && lastTouch) {
    LOGF("TOUCH", "released");
  }
  lastTouch = t;
}

// ---------- Gesture engine ----------
const char* swingName(uint8_t axis, int8_t sign) {
  if (axis == 2) return (sign > 0) ? "SWISH_R" : "SWISH_L";
  return (sign > 0) ? "FLICK_UP" : "FLICK_DOWN";
}

void endSwing(unsigned long now) {
  swinging = false;
  unsigned long dur = now - swingStart;

#if VERBOSE
  LOG_BEGIN("SWING");
  Serial.print(F("end, "));
  Serial.print(swingName(swingAxis, swingSign));
  Serial.print(F("?, duration "));
  Serial.print(dur);
  Serial.print(F(" ms"));
  LOG_END();
#endif

  if (dur > 500) {
    LOGF("SWING", "rejected: too slow");
    return;
  }
  if (now - lastGestureTime < GESTURE_COOLDOWN) {
    LOGF("SWING", "rejected: cooldown");
    return;
  }

  // Opposite swing on same axis shortly after the last one => shake
  if (swingAxis == lastAxis && swingSign == -lastSign &&
      now - lastSwingEnd < SHAKE_WINDOW) {
    shakeCount++;
#if VERBOSE
    LOG_BEGIN("SHAKE");
    Serial.print(F("counter-swing #"));
    Serial.print(shakeCount);
    LOG_END();
#endif
  } else {
    shakeCount = 0;
  }

  if (shakeCount >= 2) {
    pendingName = nullptr;
    shakeCount = 0;
    sendGesture("SHAKE");
  } else {
    pendingName = swingName(swingAxis, swingSign);
    pendingTime = now;
    LOGF("SWING", "queued, waiting to see if it becomes a shake");
  }

  lastAxis = swingAxis;
  lastSign = swingSign;
  lastSwingEnd = now;
}

void processImu() {
  int16_t ax, ay, az, gx, gy, gz;
  unsigned long now = millis();

  if (!mpuRead(ax, ay, az, gx, gy, gz)) {
    if (now - lastImuErr > 1000) {
      lastImuErr = now;
      LOGF("ERROR", "MPU6050 read failed");
    }
    return;
  }

  int gxd = gx / 65;                    // roll  (dps)
  int gyd = (gy / 65) * SIGN_PITCH;     // pitch (dps)
  int gzd = (gz / 65) * SIGN_YAW;       // yaw   (dps)

  // Deliver pending single gesture if no counter-swing arrived
  if (pendingName && now - pendingTime > PENDING_DELAY) {
    sendGesture(pendingName);
    pendingName = nullptr;
  }

  if (now - lastGestureTime < GESTURE_COOLDOWN && !swinging) {
    rollAccum = 0;
    return;
  }

  // --- Twirl (roll around the wand axis) ---
  if (abs(gxd) > TWIRL_MIN_DPS) {
    rollAccum += gxd * (IMU_PERIOD / 1000.0f);
    if (abs(rollAccum) > TWIRL_ANGLE) {
      rollAccum = 0;
      swinging = false;
      pendingName = nullptr;
      sendGesture("TWIRL");
      return;
    }
  } else {
    rollAccum = 0;
  }

  // --- Thrust (linear jab along the wand) ---
  int axs = ax * SIGN_THRUST;
  if (axs > THRUST_G_RAW && abs(gyd) < THRUST_MAX_ROT && abs(gzd) < THRUST_MAX_ROT) {
    swinging = false;
    pendingName = nullptr;
    sendGesture("THRUST");
    return;
  }

  // --- Swing (yaw / pitch) ---
  int ap = abs(gyd), ayw = abs(gzd);
  int dom = max(ap, ayw);
  if (!swinging) {
    if (dom > SWING_START_DPS && abs(gxd) < dom) {
      swinging = true;
      swingStart = now;
      if (ayw >= ap) { swingAxis = 2; swingSign = (gzd > 0) ? 1 : -1; }
      else           { swingAxis = 1; swingSign = (gyd > 0) ? 1 : -1; }
#if VERBOSE
      LOG_BEGIN("SWING");
      Serial.print(F("start, axis "));
      Serial.print(swingAxis == 2 ? F("YAW") : F("PITCH"));
      Serial.print(F(", dir "));
      Serial.print(swingSign > 0 ? F("+") : F("-"));
      Serial.print(F(", peak "));
      Serial.print(dom);
      Serial.print(F(" dps"));
      LOG_END();
#endif
    }
  } else if (dom < SWING_END_DPS) {
    endSwing(now);
  } else if (now - swingStart > 600) {
    swinging = false;  // timeout
    LOGF("SWING", "timeout: swing lasted too long, cancelled");
  }
}

// ---------- IR proximity (digital) ----------
// Needs 2 consecutive identical reads (~40 ms) to change state.
void processProx() {
  bool detected = (digitalRead(PROX_DIGITAL_PIN) == (PROX_ACTIVE_LOW ? LOW : HIGH));
  if (detected == proxCandidate) {
    if (proxCount < 255) proxCount++;
  } else {
    proxCandidate = detected;
    proxCount = 1;
  }
  if (proxCount >= 2 && proxCandidate != proxNear) {
    proxNear = proxCandidate;
    sendProx(proxNear ? "NEAR" : "FAR");
  }
}

// ---------- Startup self-check ----------
void selfCheck() {
#if VERBOSE
  LOGF("BOOT", "Magic wand starting");

  if (i2cPresent(MPU_ADDR)) {
    int who = readReg8(MPU_ADDR, 0x75);
    LOG_BEGIN("BOOT");
    Serial.print(F("MPU6050 found, WHO_AM_I=0x"));
    Serial.print(who, HEX);
    LOG_END();
  } else {
    LOGF("BOOT", "MPU6050 NOT found - check wiring/address");
  }

  LOG_BEGIN("BOOT");
  Serial.print(F("Touch pad D"));
  Serial.print(TOUCH_PIN);
  Serial.print(F(" reads "));
  Serial.print(digitalRead(TOUCH_PIN) ? F("HIGH") : F("LOW"));
  LOG_END();

  LOG_BEGIN("BOOT");
  Serial.print(F("IR proximity: digital D"));
  Serial.print(PROX_DIGITAL_PIN);
  Serial.print(F(" reads "));
  Serial.print(digitalRead(PROX_DIGITAL_PIN) ? F("HIGH") : F("LOW"));
  LOG_END();
#endif
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(BAUD_RATE);
  pinMode(TOUCH_PIN, INPUT);
  pinMode(PROX_DIGITAL_PIN, INPUT);
  Wire.begin();
  Wire.setClock(400000);
  selfCheck();
  setAwake(true);
  LOGF("BOOT", "Ready. Touch pad toggles sleep/awake");
}

void loop() {
  unsigned long now = millis();

  handleTouch();

  if (!awake) return;

  if (now - tImu >= IMU_PERIOD) {
    tImu = now;
    processImu();
  }
  if (now - tProx >= PROX_PERIOD) {
    tProx = now;
    processProx();
  }
}