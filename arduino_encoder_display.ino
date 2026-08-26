/*
  WELL RECORD - Sistema de metragem de cabo para inspeção de poços artesianos
  Encoder óptico E38S6G5-360B-G24N (360 PPR) acoplado a carretel de 20cm
  + display LCD 4x20 (modo paralelo) + comunicação serial com app Python
  Arduino Uno

  Funcionalidades:
    - Calcula metragem do cabo em centímetros (considera frente/trás)
    - Mostra no LCD: título fixo, metragem atual e tempo ligado (HH:MM:SS)
    - Botão físico zera o contador
    - Protocolo serial para app Python:
        * Arduino -> PC: envia "CM:<valor>;PULSES:<valor>;TIME:<HH:MM:SS>"
          a cada 200ms, continuamente (não só quando muda)
        * PC -> Arduino: comando "RESET" (seguido de \n) zera o contador
          remotamente, igual ao botão físico

  Ligações:

  LCD 4x20 (modo paralelo - 4 bits):
    RS  -> Pino 12
    EN  -> Pino 11
    D4  -> Pino 5
    D5  -> Pino 4
    D6  -> Pino 3
    D7  -> Pino 2
    RW  -> GND
    VSS -> GND
    VDD -> 5V
    V0  -> GND (sem potenciômetro, contraste fixo no máximo)
    A (LED+) -> 5V (via resistor ~220 ohm, se o módulo não tiver embutido)
    K (LED-) -> GND

  Encoder E38S6G5-360B-G24N (acoplado ao carretel de 20cm de diâmetro):
    A   -> Pino 8
    B   -> Pino 9
    +   -> 5V (verificar tensão de alimentação do seu modelo)
    GND -> GND

  Botão de zerar o contador:
    Sinal -> Pino 7 (nível lógico 1 = pressionado)
    GND   -> GND (via resistor pull-down externo)

  IMPORTANTE sobre os pinos do encoder e do botão:
  Os pinos 8, 9 e 7 não suportam attachInterrupt() no Arduino Uno
  (só os pinos 2 e 3 suportam interrupção externa nativa, e aqui
  eles já estão ocupados pelo LCD). Por isso usamos Interrupção
  por Mudança de Pino (PCINT):
    - Encoder (pinos 8/9) -> grupo PCINT0
    - Botão (pino 7)      -> grupo PCINT2
*/

#include <LiquidCrystal.h>

// ---------- LCD pins ----------
const int rs = 12, en = 11, d4 = 5, d5 = 4, d6 = 3, d7 = 2;
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
const String display_fixed_msg = "* WELL RECORD *";

// ---------- Encoder pins ----------
const int encoderPinA = 8;
const int encoderPinB = 9;

// ---------- Reset button pin ----------
const int resetButtonPin = 7;

// ---------- Reel mechanical parameters ----------
const float reelDiameterCm = 20.0;
const float circumferenceCm = reelDiameterCm * PI; // ~62.83 cm/revolution
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

  lcd.begin(20, 4);
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
