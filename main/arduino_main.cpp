#include <Arduino.h>
#include <Wire.h>
#include <Arduino_APDS9960.h>
#include "sdkconfig.h"
#include <Bluepad32.h>
#include <uni.h>
#include "controller_callbacks.h"
#include <QTRSensors.h>

// Color Sensor
#define APDS9960_INT 4     
#define I2C_SDA 21
#define I2C_SCL 22
#define I2C_FREQ 400000
TwoWire I2C_0 = TwoWire(0);
APDS9960 apds = APDS9960(I2C_0, APDS9960_INT);

// Controller 
#define ONBOARD_LED_PIN 2
#define LED_PIN 23   // external LED (on by default, off in line sensor mode)
#define IN1  16  
#define IN2  17  
#define IN3  18
#define IN4  19
extern ControllerPtr myControllers[BP32_MAX_GAMEPADS]; 
const int DEADZONE = 100;

void dumpGamepad(ControllerPtr ctl) {
    Console.printf(
        "DPAD: %d A: %d B: %d X: %d Y: %d LX: %d LY: %d RX: %d RY: %d L1: %d R1: %d L2: %d R2: %d\n",
        ctl->dpad(),        // D-pad
        ctl->a(),           // Letter buttons
        ctl->b(),
        ctl->x(),
        ctl->y(),
        ctl->axisX(),        // (-511 - 512) left X Axis
        ctl->axisY(),        // (-511 - 512) left Y axis
        ctl->axisRX(),       // (-511 - 512) right X axis
        ctl->axisRY(),       // (-511 - 512) right Y axis
        ctl->l1(),           // Bumpers
        ctl->r1(),
        ctl->l2(),
        ctl->r2()
    );
}

void leftMotor(int dir) {
    if (dir == 1)       { digitalWrite(IN1, HIGH);  digitalWrite(IN2, LOW); }
    else if (dir == -1) { digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);  }
    else                { digitalWrite(IN1, LOW);  digitalWrite(IN2, LOW);  }
}

void rightMotor(int dir) {
    if (dir == 1)       { digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);  }
    else if (dir == -1) { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); }
    else                { digitalWrite(IN3, LOW);  digitalWrite(IN4, LOW);  }
}

// throttle: left stick, forward is POSITIVE. turn: right stick, right is POSITIVE.
void drive(int throttle, int turn) {
    bool fwd   = (throttle >  DEADZONE);
    bool back  = (throttle < -DEADZONE);
    bool right = (turn >  DEADZONE);
    bool left  = (turn < -DEADZONE);

    // The motors are mirror-mounted: a wheel's "forward" is +1 on the left motor but -1 on the right motor.
    if (fwd && left)        { leftMotor(0);  rightMotor(-1); }  // curve left
    else if (fwd && right)  { leftMotor(1);  rightMotor(0);  }  // curve right
    else if (back && left)  { leftMotor(0);  rightMotor(1);  }  // reverse curving left
    else if (back && right) { leftMotor(-1); rightMotor(0);  }  // reverse curving right
    else if (fwd)           { leftMotor(1);  rightMotor(-1); }  // straight forward
    else if (back)          { leftMotor(-1); rightMotor(1);  }  // straight backward
    else if (left)          { leftMotor(-1); rightMotor(-1); }  // pivot left in place
    else if (right)         { leftMotor(1);  rightMotor(1);  }  // pivot right in place
    else                    { leftMotor(0);  rightMotor(0);  }  // stop
}

struct Modes {
    bool driveMode;
    bool colorMode;
    bool sensorMode;
};

Modes switchModes(int a, int b, int x, Modes m){
    if (a != 0) {
        m.driveMode = true;
        m.colorMode = false;
        m.sensorMode = false;
    }
    if (b != 0) {
        m.driveMode = false;
        m.colorMode = true;
        m.sensorMode = false;
    }
    if (x != 0) {
        m.driveMode = false;
        m.colorMode = false;
        m.sensorMode = true;
    }
    return m;
}

struct Colors {
    int r, g, b, a;
};

const int COLOR_MIN_TOL = 8;  // minimum allowed difference per channel (raw units)
bool apdsReady = false;

// begin() sets 16x gain; the library's register setters are private, so write CONTROL (0x8F) directly.
// 0x00=1x, 0x01=4x, 0x02=16x, 0x03=64x. Call right after a successful apds.begin().
void setColorGain64x() {
    I2C_0.beginTransmission(0x39);  // APDS9960 I2C address
    I2C_0.write(0x8F);
    I2C_0.write(0x03);
    I2C_0.endTransmission();
}

Colors getColors() {
    int r = 0, g = 0, b = 0, a = 0;
    if (!apdsReady) { apdsReady = apds.begin(); if (apdsReady) setColorGain64x(); }  // try again if init failed at boot
    unsigned long t0 = millis();
    while (!apds.colorAvailable()) {
        if (millis() - t0 > 500) { Serial.println("Color sensor not responding"); return {0, 0, 0, 0}; }
    }
    apds.readColor(r, g, b, a);
    Colors color = {r, g, b, a};
    return color;
}

void printColors(const Colors &c) {
    Serial.printf("R: %d  G: %d  B: %d  A: %d\n", c.r, c.g, c.b, c.a);
}

// Raw-value comparison. Each channel may differ by max(COLOR_MIN_TOL, fraction * target value),
// so bright colors get a proportionally wider margin and dark ones still get a sane floor.
bool channelMatch(int cur, int target, float fraction) {
    float tol = fmaxf((float)COLOR_MIN_TOL, fraction * (float)target);
    return fabsf((float)(cur - target)) <= tol;
}

bool colorsMatch(const Colors &c, const Colors &target, float fraction) {
    return channelMatch(c.r, target.r, fraction) &&
           channelMatch(c.g, target.g, fraction) &&
           channelMatch(c.b, target.b, fraction);
}

// Non-blocking read: returns false if no new reading is ready yet
bool tryGetColors(Colors &out) {
    if (!apds.colorAvailable()) return false;
    int r = 0, g = 0, b = 0, a = 0;
    apds.readColor(r, g, b, a);
    out = {r, g, b, a};
    return true;
}

const unsigned long SEEK_TIMEOUT_MS = 30000;  // give up after 30 seconds
const float COLOR_TOLERANCE = 0.07f;  // fraction of target value (loosen if it misses, tighten if false stops)
const int HITS_REQUIRED = 1;          // consecutive matching readings needed to stop

static unsigned long lastSeekStart = 0;

// Pulse timing (tune these): drive briefly, stop, let the sensor settle, then sample while stationary.
const unsigned long PULSE_DRIVE_MS  = 40;  // motors on per pulse (shorter = slower average speed)
const unsigned long PULSE_SETTLE_MS = 40;  // motors off before sampling (must cover coasting + >1 sensor integration, ~11 ms)

enum PulsePhase { PULSE_DRIVE, PULSE_SETTLE, PULSE_SAMPLE };

// One step of the seek. Call every loop while seeking.
// Pulses the motors: DRIVE -> SETTLE (stopped) -> SAMPLE (one clean reading) -> repeat.
// Returns true when seeking is finished (color matched or timed out).
bool goForwardUntil(const Colors &target, unsigned long startMs) {
    static PulsePhase phase = PULSE_DRIVE;
    static unsigned long phaseStart = 0;
    static int hits = 0;

    if (startMs != lastSeekStart) {  // new seek: reset
        hits = 0;
        lastSeekStart = startMs;
        phase = PULSE_DRIVE;
        phaseStart = millis();
    }

    unsigned long now = millis();

    switch (phase) {
        case PULSE_DRIVE:
            leftMotor(-1);
            rightMotor(1);
            if (now - phaseStart >= PULSE_DRIVE_MS) {
                leftMotor(0);
                rightMotor(0);
                phase = PULSE_SETTLE;
                phaseStart = now;
            }
            break;

        case PULSE_SETTLE:
            // Motors are off; wait so the robot stops and the sensor's in-flight (blurred) reading is flushed
            if (now - phaseStart >= PULSE_SETTLE_MS) {
                phase = PULSE_SAMPLE;
            }
            break;

        case PULSE_SAMPLE: {
            Colors current;
            if (tryGetColors(current)) {  // wait here until a fresh stationary reading is ready
                if (colorsMatch(current, target, COLOR_TOLERANCE)) hits++;
                else hits = 0;

                if (hits >= HITS_REQUIRED) {
                    hits = 0;
                    Serial.println("Target color found");
                    return true;  // motors already stopped
                }
                phase = PULSE_DRIVE;  // no match: pulse forward again
                phaseStart = now;
            }
            break;
        }
    }

    if (millis() - startMs > SEEK_TIMEOUT_MS) {
        leftMotor(0);
        rightMotor(0);
        Serial.println("Seek timed out");
        return true;
    }
    return false;
}



QTRSensors qtr;
uint16_t sensors[8];

// ---------------- Line following (pulse method) ----------------
bool blackLine = true;                         // true = follow a black line, false = follow a white line
const int LINE_THRESHOLD = 500;                // calibrated value above this counts as black
const unsigned long LINE_PULSE_MS = 15;        // how long the motors run each pulse (shorter = slower, more precise)
const unsigned long CORRECT_PULSE_MS = 5 ;     // pivot pulse for small corrections (both wheels, opposite directions). Needs to be long enough for BOTH motors to start
const unsigned long LEFT_EXTRA_MS  = 3;        // extra on-time for the left wheel (raise if the LEFT wheel is the one lagging)
const unsigned long RIGHT_EXTRA_MS = 3;        // extra on-time for the right wheel (raise if the RIGHT wheel is the one lagging)
const int TURN_PULSES      = 14;               // pivot pulses for a 90 degree turn. TUNE THIS: lower if it turns too far, raise if too little
const int TURN_PRE_PULSES  = 0;                // forward pulses before turning (centers the wheels over the corner)
const int TURN_POST_PULSES = 5;                // forward pulses after turning (drives off the corner so it isn't re-detected)
const int DEADBAND = 800;                     // ignore tiny offsets (1000 = one sensor width)
const unsigned long LINE_PAUSE_MS = 30;       // motors off between pulses (bot settles, sensor is read here)

// sensors[0] is the RIGHT edge of the bot, sensors[7] is the LEFT edge, sensors[3] and [4] are the middle.
// Returns true if sensor i is looking at the line (works for both black and white lines).
bool onLine(int i) {
    bool isBlack = sensors[i] > LINE_THRESHOLD;
    return blackLine ? isBlack : !isBlack;
}

// Physical motions (forward on this robot is left -1, right +1, same as the color seek)
void goStraight()  { leftMotor(-1); rightMotor(1);  }
void turnRight()   { leftMotor(-1); rightMotor(-1); }  // pivot right
void turnLeft()    { leftMotor(1);  rightMotor(1);  }  // pivot left

// Run whatever direction was just set for ms, then stop and pause so the next reading is steady.
// Each wheel gets its own on-time so a weaker/stickier wheel can be given a little longer.
void movePulse(unsigned long ms) {
    unsigned long leftMs  = ms + LEFT_EXTRA_MS;
    unsigned long rightMs = ms + RIGHT_EXTRA_MS;
    unsigned long start = millis();
    while (millis() - start < max(leftMs, rightMs)) {
        if (millis() - start >= leftMs)  leftMotor(0);   // left wheel's time is up
        if (millis() - start >= rightMs) rightMotor(0);  // right wheel's time is up
    }
    leftMotor(0);
    rightMotor(0);
    delay(LINE_PAUSE_MS);
}

// Fixed 90 degree corner: creep forward, pivot a set number of pulses, then drive forward off the corner.
// Blocking on purpose: the bot ignores the sensors until the whole turn is done, so it can't spin past 90.
void doTurn(bool right) {
    for (int i = 0; i < TURN_PRE_PULSES; i++) { goStraight(); movePulse(LINE_PULSE_MS); }   // get the wheels over the corner
    for (int i = 0; i < TURN_PULSES; i++) {
        if (right) turnRight(); else turnLeft();
        movePulse(LINE_PULSE_MS);
    }
    for (int i = 0; i < TURN_POST_PULSES; i++) { goStraight(); movePulse(LINE_PULSE_MS); } // move onto the new line
}

// One pulse: read sensors while stopped, pick a move, run it briefly, stop.
void followLinePulse() {
    // Fills sensors[] with 0-1000 (1000 = black) and returns the line position: 0 (right edge) to 7000 (left edge), 3500 = centered.
    // If the line is lost it returns 0 or 7000 (the side it was last seen on), so the bot turns back toward it.
    int position = blackLine ? qtr.readLineBlack(sensors) : qtr.readLineWhite(sensors);
    int error = position - 3500;  // positive = line is to the LEFT, negative = line is to the RIGHT

    bool allRight = onLine(0) && onLine(1) && onLine(2) && onLine(3);  // whole right half on the line
    bool allLeft  = onLine(4) && onLine(5) && onLine(6) && onLine(7);  // whole left half on the line
    bool middleOk = onLine(3) && onLine(4);                            // both middle sensors on the line

    if (allRight)                  { doTurn(true);  return; }   // 90 degree corner to the right
    if (allLeft)                   { doTurn(false); return; }   // 90 degree corner to the left

    if (middleOk)                  { goStraight(); movePulse(LINE_PULSE_MS); }  // centered, keep going
    else if (error > DEADBAND)     { turnLeft();   movePulse(CORRECT_PULSE_MS); } // line drifted left -> small pivot left
    else if (error < -DEADBAND)    { turnRight();  movePulse(CORRECT_PULSE_MS); } // line drifted right -> small pivot right
    else                           { goStraight(); movePulse(LINE_PULSE_MS); }    // nearly centered
}


void setup() {
    Serial.begin(115200);

    BP32.setup(&onConnectedController, &onDisconnectedController);
    BP32.forgetBluetoothKeys(); 
    esp_log_level_set("gpio", ESP_LOG_ERROR); // Suppress info log spam from gpio_isr_service
    uni_bt_allowlist_set_enabled(true);

    qtr.setTypeAnalog(); // or setTypeAnalog()
    qtr.setSensorPins((const uint8_t[]) {32, 33, 36, 39, 34, 35, 26, 25}, 8); // the numbers in the bracket is for sensor 1 to 8, 8 is just the number of sensors used. They range from 0 to 1000, 0 being black and 1000 being white.

    for (uint16_t i = 0; i < 250; i++) {
        Console.printf("calibrating %d/250\n", i); // 250 is the number of calibrations recommended by manufacturer
        qtr.calibrate();
        delay(20);
    }

    Serial.println("Calibration done. Min/max per sensor:");
    for (int i = 0; i < 8; i++) {
        Serial.printf("  S%d: min %u  max %u\n", i + 1, qtr.calibrationOn.minimum[i], qtr.calibrationOn.maximum[i]);
    }

    delay(500);  // let sensor power rail settle on battery
    I2C_0.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
    apds.setInterruptPin(APDS9960_INT);
    for (int i = 0; i < 10 && !apdsReady; i++) {
        apdsReady = apds.begin();
        if (!apdsReady) { Serial.println("APDS9960 init failed, retrying"); delay(200); }
    }
    if (apdsReady) setColorGain64x();
    Serial.println(apdsReady ? "Color sensor ready" : "Color sensor NOT ready");

    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT);
    pinMode(IN4, OUTPUT);

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);  // LED on by default

    Serial.println("Setup complete. Waiting for controller...");
}

// State shared between the controller-input block and the every-loop seek step
static Modes modes = {true, false, false};
static Modes prevModes = {false, false, false};  // all false so the starting mode prints once
static int prevY = 0;
static int prevL1 = 0;
static bool seeking = false;
static unsigned long seekStart = 0;
static Colors targetColor = {0, 0, 0, 0};

void loop() {
    vTaskDelay(1); // Ensures WDT does not get triggered when no controller is connected
    BP32.update();
    bool controllerConnected = false;
    for (auto myController : myControllers) { // Only execute code when controller is connected
        if (myController && myController->isConnected()) controllerConnected = true;
        if (myController && myController->isConnected() && myController->hasData()) {

            // Getting inputs
            int throttle = myController->axisY();   // left stick: raw up is NEGATIVE; drive()'s "back" branch is physically forward on this robot
            int turn = -myController->axisRX();     // right stick: raw right is POSITIVE; flipped because turning was reversed on this robot
            int a = myController->a();
            int b = myController->b();
            int x = myController->x();
            int y = myController->y();
            int l1 = myController->l1();

            // Update Modes
            modes = switchModes(a, b, x, modes);

            // Print only when a mode just turned on
            if (modes.driveMode && !prevModes.driveMode) {
                Serial.println("Drive Mode is on");
            }
            if (modes.colorMode && !prevModes.colorMode) {
                Serial.println("Color Mode is on");
            }
            if (modes.sensorMode && !prevModes.sensorMode) {
                Serial.println("Sensor Mode is on");
                digitalWrite(LED_PIN, LOW);   // LED off in line sensor mode
            }
            // Just left sensor mode: stop the motors, LED back on
            if (prevModes.sensorMode && !modes.sensorMode) {
                leftMotor(0);
                rightMotor(0);
                digitalWrite(LED_PIN, HIGH);
            }

            // Just left drive mode: stop the motors so a held stick doesn't leave them running
            if (prevModes.driveMode && !modes.driveMode) {
                leftMotor(0);
                rightMotor(0);
            }

            // Mode logic (runs every loop, no printing)
            if (modes.driveMode) {
                drive(throttle, turn);
            }

            if (modes.colorMode) {
                if (y != 0 && prevY == 0) {
                    targetColor = getColors();
                    Serial.println("Color is remembered");
                    printColors(targetColor);
                }

                // L1 (only in color mode) starts seeking; it keeps going on its own
                if (l1 != 0 && prevL1 == 0 && !seeking) {
                    seeking = true;
                    seekStart = millis();
                    Serial.println("Seeking started");
                }
                // (the seek step itself runs every loop, after this block)
            } else if (seeking) {
                // Left color mode mid-seek: cancel and stop
                seeking = false;
                leftMotor(0);
                rightMotor(0);
            }

            prevModes = modes;  // remember for next loop
            prevY = y;
            prevL1 = l1;
            //dumpGamepad(myController); // Prints the gamepad state, delete or comment if don't need
        }
    }

    // Failsafe: no controller connected -> motors off
    if (!controllerConnected) {
        leftMotor(0);
        rightMotor(0);
    }

    // Line following: one pulse per loop while in sensor mode
    if (modes.sensorMode && controllerConnected) {
        followLinePulse();
    }

    // Seek step: runs every loop (not only when a new controller packet arrives)
    if (seeking) {
        if (!controllerConnected || !modes.colorMode) {
            // Controller lost or left color mode: cancel and stop
            seeking = false;
            leftMotor(0);
            rightMotor(0);
        } else if (goForwardUntil(targetColor, seekStart)) {
            seeking = false;
        }
    }
}