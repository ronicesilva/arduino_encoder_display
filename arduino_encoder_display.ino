/*
  WELL RECORD - Cable metering system for artesian well inspection
  E38S6G5-360B-G24N optical encoder (360 PPR) coupled to a 6.5cm reel
  + 4x20 LCD display (I2C serial module) + serial communication with Python app
  Arduino Uno

  Features:
    - Calculates cable length in centimeters (accounts for forward/backward)
    - LCD shows: fixed title, current length, and uptime (HH:MM:SS)
    - Physical button resets the counter
    - Serial protocol for the Python app:
        * Arduino -> PC: sends "CM:<value>;PULSES:<value>;TIME:<HH:MM:SS>"
          every 200ms, continuously (not only when it changes)
        * PC -> Arduino: "RESET" command (followed by \n) resets the counter
          remotely, same as the physical button

  Wiring:

  4x20 LCD with I2C serial module (PCF8574 backpack):
    SDA -> A4
    SCL -> A5
    VCC -> 5V
    GND -> GND
    (contrast is adjusted with the trimmer on the I2C module itself)
    (I2C address is usually 0x27 or 0x3F - see lcdI2cAddress below)

  E38S6G5-360B-G24N encoder (coupled to the 6.5cm diameter reel):
    A   -> Pin 8
    B   -> Pin 9
    +   -> 5V (check your model's supply voltage)
    GND -> GND

  Reset button:
    Signal -> Pin 7 (logic level 1 = pressed)
    GND    -> GND (via external pull-down resistor)

  IMPORTANT about the encoder and button pins:
  Pins 8, 9, and 7 do not support attachInterrupt() on the Arduino Uno
  (only pins 2 and 3 support native external interrupts). The encoder and
  button use Pin Change Interrupts (PCINT) instead:
    - Encoder (pins 8/9) -> PCINT0 group
    - Button (pin 7)     -> PCINT2 group

  Required library: "LiquidCrystal I2C" (Frank de Brabander), installable
  via Arduino IDE > Tools > Manage Libraries.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ---------- LCD (I2C) ----------
// SDA = A4, SCL = A5 (fixed by hardware on the Arduino Uno)
const uint8_t lcdI2cAddress = 0x27; // try 0x3F if the display stays blank
LiquidCrystal_I2C lcd(lcdI2cAddress, 20, 4); // 20 columns, 4 rows

// ---------- Fixed message shown on line 1 of the LCD ----------
const char display_fixed_msg[] = "* WELL RECORD *";

// ---------- Encoder pins ----------
const int encoderPinA = 8;
const int encoderPinB = 9;

// ---------- Reset button pin ----------
const int resetButtonPin = 7;

// ---------- Reel mechanical parameters ----------
const float reelDiameterCm = 6.5;
const float circumferenceCm = reelDiameterCm * PI; // ~20.4203 cm/revolution
const int pulsesPerRevolution = 360;
const int quadratureFactor = 4; // full quadrature decoding = 1440 pulses/revolution
const float cmPerPulse = circumferenceCm / (pulsesPerRevolution * quadratureFactor);

// ---------- Variables shared with interrupts ----------
volatile long pulseCounter = 0;
volatile uint8_t lastEncoderState = 0;
volatile bool dataChanged = true;

volatile unsigned long lastButtonTime = 0;
const unsigned long buttonDebounceMs = 200;

// ---------- Periodic send control for the Python app ----------
unsigned long lastSerialSendTime = 0;
const unsigned long serialIntervalMs = 200; // sends status every 200ms

// ---------- Display cache (avoids redrawing the LCD unnecessarily) ----------
char previousCmLine[21] = "";
char previousTimeLine[21] = "";

void setup() {
  pinMode(encoderPinA, INPUT_PULLUP);
  pinMode(encoderPinB, INPUT_PULLUP);
  pinMode(resetButtonPin, INPUT); // active-high signal - uses external pull-down

  Serial.begin(115200);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print(display_fixed_msg);

  // Store the initial state of both encoder channels
  lastEncoderState = (digitalRead(encoderPinA) << 1) | digitalRead(encoderPinB);

  // Enable pin change interrupt for PCINT0 group (pins 8-13) - encoder
  PCICR |= (1 << PCIE0);
  PCMSK0 |= (1 << PCINT0);  // pin 8 = PCINT0
  PCMSK0 |= (1 << PCINT1);  // pin 9 = PCINT1

  // Enable pin change interrupt for PCINT2 group (pins 0-7) - button
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT23); // pin 7 = PCINT23
}

void loop() {
  processSerialCommands();

  unsigned long now = millis();
  bool timeToSend = (now - lastSerialSendTime) >= serialIntervalMs;

  if (dataChanged || timeToSend) {
    dataChanged = false;
    if (timeToSend) lastSerialSendTime = now;
    updateDisplayAndSerial(timeToSend);
  }
}

// ---------- Reads commands coming from the Python app over serial ----------
void processSerialCommands() {
  while (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.equalsIgnoreCase("RESET")) {
      noInterrupts();
      pulseCounter = 0;
      interrupts();
      dataChanged = true;
      Serial.println("OK:RESET");
    }
  }
}

// ---------- Pin change interrupt (pins 8 and 9) - encoder ----------
ISR(PCINT0_vect) {
  uint8_t a = digitalRead(encoderPinA);
  uint8_t b = digitalRead(encoderPinB);
  uint8_t currentState = (a << 1) | b;

  uint8_t transition = (lastEncoderState << 2) | currentState;

  switch (transition) {
    case 0b0001:
    case 0b0111:
    case 0b1110:
    case 0b1000:
      pulseCounter++;
      break;

    case 0b0010:
    case 0b1011:
    case 0b1101:
    case 0b0100:
      pulseCounter--;
      break;

    // other combinations = noise/invalid reading, ignore
  }

  lastEncoderState = currentState;
  dataChanged = true;
}

// ---------- Pin change interrupt (pin 7) - reset button ----------
ISR(PCINT2_vect) {
  // Active-high signal (1) = button pressed
  if (digitalRead(resetButtonPin) == HIGH) {
    unsigned long now = millis();
    if (now - lastButtonTime > buttonDebounceMs) {
      pulseCounter = 0;
      dataChanged = true;
      lastButtonTime = now;
    }
  }
}

// ---------- Updates the LCD and sends status over serial ----------
void updateDisplayAndSerial(bool sendSerial) {
  long pulses;
  noInterrupts();
  pulses = pulseCounter;
  interrupts();

  float cm = pulses * cmPerPulse;

  unsigned long totalSeconds = millis() / 1000;
  int hours = (totalSeconds / 3600) % 100; // 2-digit display limit
  int minutes = (totalSeconds / 60) % 60;
  int seconds = totalSeconds % 60;

  char timeBuf[9]; // "00:00:00"
  sprintf(timeBuf, "%02d:%02d:%02d", hours, minutes, seconds);

  // ---------- Line 3 (index 2): cable length in meters + centimeters ----------
  bool isNegative = cm < 0;
  float absCm = fabs(cm);
  int meters = (int)(absCm / 100.0);
  float remainderCm = absCm - (meters * 100.0);

  char remainderStr[8];
  dtostrf(remainderCm, 4, 1, remainderStr); // e.g. "34.5"

  char cmLineBuf[21];
  snprintf(cmLineBuf, sizeof(cmLineBuf), "%s%dm %scm",
           isNegative ? "-" : "", meters, remainderStr);
  // Pad the rest of the line with spaces to erase leftover old text
  int len = strlen(cmLineBuf);
  for (int i = len; i < 20; i++) cmLineBuf[i] = ' ';
  cmLineBuf[20] = '\0';

  if (strcmp(cmLineBuf, previousCmLine) != 0) {
    strcpy(previousCmLine, cmLineBuf);
    lcd.setCursor(0, 2);
    lcd.print(cmLineBuf);
  }

  // ---------- Line 4 (index 3): uptime, right-aligned ----------
  char timeLineBuf[21];
  memset(timeLineBuf, ' ', 20);
  timeLineBuf[20] = '\0';
  int startCol = 20 - 8; // "00:00:00" is 8 characters
  memcpy(&timeLineBuf[startCol], timeBuf, 8);

  if (strcmp(timeLineBuf, previousTimeLine) != 0) {
    strcpy(previousTimeLine, timeLineBuf);
    lcd.setCursor(0, 3);
    lcd.print(timeLineBuf);
  }

  // ---------- Periodic send to the Python app ----------
  if (sendSerial) {
    Serial.print("CM:");
    Serial.print(cm, 2);
    Serial.print(";PULSES:");
    Serial.print(pulses);
    Serial.print(";TIME:");
    Serial.println(timeBuf);
  }
}
