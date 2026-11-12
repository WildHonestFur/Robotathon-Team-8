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
#define I2C_FREQ 100000
TwoWire I2C_0 = TwoWire(0);
APDS9960 apds = APDS9960(I2C_0, APDS9960_INT);

// Controller 
#define ONBOARD_LED_PIN 2 
#define IN1  16  
#define IN2  17  
#define IN3  18
#define IN4  19
extern ControllerPtr myControllers[BP32_MAX_GAMEPADS]; 
const int DEADZONE = 40;

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

void drive(int Lx, int Ly) {
    bool fwd   = (Ly >  DEADZONE);
    bool back  = (Ly < -DEADZONE);
    bool right = (Lx >  DEADZONE);
    bool left  = (Lx < -DEADZONE);

    if (fwd && left)        { leftMotor(0);  rightMotor(1);  }  // curve left
    else if (fwd && right)  { leftMotor(1);  rightMotor(0);  }  // curve right
    else if (back && left)  { leftMotor(0);  rightMotor(-1); }  // reverse curving left
    else if (back && right) { leftMotor(-1); rightMotor(0);  }  // reverse curving right
    else if (fwd)           { leftMotor(1);  rightMotor(1);  }  // straight forward
    else if (back)          { leftMotor(-1); rightMotor(-1); }  // straight backward
    else if (left)          { leftMotor(-1); rightMotor(1);  }  // pivot left in place
    else if (right)         { leftMotor(1);  rightMotor(-1); }  // pivot right in place
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

Colors getColors() {
    int r = 0, g = 0, b = 0, a = 0;
    while (!apds.colorAvailable()) { delay(5); }  // wait for sensor
    apds.readColor(r, g, b, a);
    Colors color = {r, g, b, a};
    return color;
}

void printColors(const Colors &c) {
    Serial.printf("R: %d  G: %d  B: %d  A: %d\n", c.r, c.g, c.b, c.a);
}

bool colorsMatch(const Colors &c1, const Colors &c2, int tolerance) {
    return abs(c1.r - c2.r) <= tolerance &&
           abs(c1.g - c2.g) <= tolerance &&
           abs(c1.b - c2.b) <= tolerance &&
           abs(c1.a - c2.a) <= tolerance;
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
const int COLOR_TOLERANCE = 8;

// One step of the seek. Call every loop while seeking.
// Returns true when seeking is finished (color matched or timed out).
bool goForwardUntil(const Colors &target, unsigned long startMs) {
    leftMotor(1);
    rightMotor(1);

    Colors current;
    if (tryGetColors(current) && colorsMatch(current, target, COLOR_TOLERANCE)) {
        leftMotor(0);
        rightMotor(0);
        Serial.println("Target color found");
        return true;
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

void setup() {
    Serial.begin(115200);

    BP32.setup(&onConnectedController, &onDisconnectedController);
    BP32.forgetBluetoothKeys(); 
    esp_log_level_set("gpio", ESP_LOG_ERROR); // Suppress info log spam from gpio_isr_service
    uni_bt_allowlist_set_enabled(true);

    /*qtr.setTypeAnalog(); // or setTypeAnalog()
    qtr.setSensorPins((const uint8_t[]) {32, 33, 36, 39, 34, 35, 26, 25}, 8); // the numbers in the bracket is for sensor 1 to 8, 8 is just the number of sensors used. They range from 0 to 1000, 0 being black and 1000 being white.

    for (uint8_t i = 0; i < 250; i++) {
        Console.printf("calibrating %d/250\n", i); // 250 is the number of calibrations recommended by manufacturer
        qtr.calibrate();
        delay(20);
    }*/

    I2C_0.begin(I2C_SDA, I2C_SCL, I2C_FREQ);
    apds.setInterruptPin(APDS9960_INT);
    apds.begin();

    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(IN3, OUTPUT);
    pinMode(IN4, OUTPUT);

}

void loop() {
    vTaskDelay(1); // Ensures WDT does not get triggered when no controller is connected
    BP32.update(); 
    for (auto myController : myControllers) { // Only execute code when controller is connected
        if (myController && myController->isConnected() && myController->hasData()) {        
            
            // Getting inputs
            int ly = myController->axisY();   // up is NEGATIVE
            int lx = -myController->axisX();   // right is POSITIVE
            int a = myController->a();
            int b = myController->b();
            int x = myController->x();
            int y = myController->y();
            int l1 = myController->l1();

            // Default Modes
            static Modes modes = {true, false, false};
            static Modes prevModes = {false, false, false};  // all false so the starting mode prints once
            static int prevY = 0;
            static int prevL1 = 0;
            static bool seeking = false;
            static unsigned long seekStart = 0;
            static Colors targetColor = {0, 0, 0, 0};
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
            }

            // Mode logic (runs every loop, no printing)
            if (modes.driveMode) {
                drive(ly, lx);
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
                if (seeking && goForwardUntil(targetColor, seekStart)) {
                    seeking = false;
                }
            } else if (seeking) {
                // Left color mode mid-seek: cancel and stop
                seeking = false;
                leftMotor(0);
                rightMotor(0);
            }

            if (modes.sensorMode) {
                // Sensor Function
            }

            prevModes = modes;  // remember for next loop
            prevY = y;
            prevL1 = l1;
            //dumpGamepad(myController); // Prints the gamepad state, delete or comment if don't need
    }
}
}