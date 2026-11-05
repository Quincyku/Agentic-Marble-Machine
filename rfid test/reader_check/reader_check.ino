/*
  Connection test for an ESP32 and the M5Stack UHF-RFID Unit.

  Open the Serial Monitor at 115200 baud. Every few seconds this:
    1. checks the level on both reader signal pins with the serial port off
    2. checks whether the two pins are shorted together
    3. asks for the reader's version using both pin orders and common bauds
    4. prints any bytes that come back, and what the result means
*/

#include <driver/gpio.h>

#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3)
const int PIN_A = D7;  // RX on the XIAO
const int PIN_B = D6;  // TX on the XIAO
#else
const int PIN_A = 16;  // RX2 on an ESP32 dev board
const int PIN_B = 17;  // TX2 on an ESP32 dev board
#endif

const long BAUD_RATES[] = {115200, 57600, 38400, 19200, 9600};

const uint8_t VERSION_QUERY[] = {0xBB, 0x00, 0x03, 0x00, 0x01, 0x00, 0x04, 0x7E};

// Clears any leftover serial port setup so the reading reflects the wiring.
int readPin(int pin, int mode) {
  gpio_reset_pin((gpio_num_t)pin);
  pinMode(pin, mode);
  delay(10);
  return digitalRead(pin);
}

// HIGH under both pulls = driven high. LOW under both = driven low.
const char* describePin(int pin) {
  int withPullDown = readPin(pin, INPUT_PULLDOWN);
  int withPullUp = readPin(pin, INPUT_PULLUP);
  if (withPullDown && withPullUp) return "driven HIGH";
  if (!withPullDown && !withPullUp) return "driven LOW";
  return "not driven";
}

bool pinsAreShorted() {
  gpio_reset_pin((gpio_num_t)PIN_B);
  pinMode(PIN_B, OUTPUT);
  digitalWrite(PIN_B, LOW);
  bool followedLow = readPin(PIN_A, INPUT_PULLUP) == LOW;

  digitalWrite(PIN_B, HIGH);
  bool followedHigh = readPin(PIN_A, INPUT_PULLDOWN) == HIGH;

  gpio_reset_pin((gpio_num_t)PIN_B);
  return followedLow && followedHigh;
}

// Returns true if a complete-looking reply came back.
bool askForVersion(long baud, int rxPin, int txPin, bool& gotBytes) {
  Serial1.begin(baud, SERIAL_8N1, rxPin, txPin);
  delay(30);
  while (Serial1.available()) Serial1.read();

  Serial1.write(VERSION_QUERY, sizeof(VERSION_QUERY));
  Serial1.flush();

  int count = 0;
  uint8_t first = 0, last = 0;
  unsigned long start = millis();
  while (millis() - start < 300) {
    if (Serial1.available()) {
      uint8_t b = Serial1.read();
      if (count == 0) {
        first = b;
        Serial.print("  ");
        Serial.print(baud);
        Serial.print(" baud, RX=GPIO");
        Serial.print(rxPin);
        Serial.print(" TX=GPIO");
        Serial.print(txPin);
        Serial.print(": ");
      }
      last = b;
      count++;
      if (b < 0x10) Serial.print('0');
      Serial.print(b, HEX);
      Serial.print(' ');
    }
  }
  if (count > 0) {
    Serial.println();
    gotBytes = true;
  }

  Serial1.end();
  return count > 0 && first == 0xBB && last == 0x7E;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Reader connection test");
}

void loop() {
  Serial.println();
  Serial.println("-----------------------------");

  const char* levelA = describePin(PIN_A);
  const char* levelB = describePin(PIN_B);
  bool shorted = pinsAreShorted();
  Serial.print("RX pin (GPIO");
  Serial.print(PIN_A);
  Serial.print("): ");
  Serial.println(levelA);
  Serial.print("TX pin (GPIO");
  Serial.print(PIN_B);
  Serial.print("): ");
  Serial.println(levelB);
  Serial.print("Shorted together: ");
  Serial.println(shorted ? "YES" : "no");

  bool gotBytes = false;
  long workingBaud = 0;
  bool swapped = false;
  for (long baud : BAUD_RATES) {
    if (askForVersion(baud, PIN_A, PIN_B, gotBytes)) {
      workingBaud = baud;
      break;
    }
    if (askForVersion(baud, PIN_B, PIN_A, gotBytes)) {
      workingBaud = baud;
      swapped = true;
      break;
    }
  }

  Serial.print("Result: ");
  if (shorted) {
    Serial.println("the RX and TX pins are connected to each other. Check for a solder bridge or touching wires.");
  } else if (workingBaud != 0) {
    Serial.print("reader answered at ");
    Serial.print(workingBaud);
    Serial.println(swapped ? " baud, but TX/RX are swapped. Swap white and yellow." : " baud with the wiring correct.");
  } else if (gotBytes) {
    Serial.println("bytes arrived but not a valid reply. Send these numbers to Claude.");
  } else if (levelA[7] == 'H' && levelB[7] == 'H') {
    Serial.println("both lines are high but no reply. Check the black GND wire first, then 5V at the reader.");
  } else {
    Serial.println("no reply. At least one line is not driven high, so check white, yellow, red and black.");
  }

  delay(2000);
}
