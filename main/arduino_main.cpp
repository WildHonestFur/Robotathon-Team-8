#include "sdkconfig.h"
#include <Arduino.h>

#define ONBOARD_LED_PIN 2 // defines the word "ONBOARD_LED_PIN" as the number 2 for readability. Note that pin 2 is directly attached to the onboard LED of the microcontroller.

void setup() {
    pinMode(ONBOARD_LED_PIN, OUTPUT); // configures pin 2 to be a GPIO output pin 
}

void loop() {
    digitalWrite(ONBOARD_LED_PIN, HIGH); // writes a digital high to pin 2
    delay(1000); // waits for 1000 milliseconds (1 second)
    digitalWrite(ONBOARD_LED_PIN, LOW); // writes a digital low to pin 2
    delay(1000);
}
