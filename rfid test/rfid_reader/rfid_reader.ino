/*
  UHF RFID tag checker for an ESP32 dev board or a Pro Micro (ATmega32U4, 5V)
  and an M5Stack UHF-RFID Unit (JRD-4035).

  Reports over USB serial (115200 baud):
    - whether each tag it sees is REGISTERED or UNREGISTERED
    - whether that tag is NEAR, FAR (seen, but weak signal), or GONE

  Seeed XIAO ESP32-C3 or ESP32-S3 wiring:
    Unit white  (TXD) -> D7 (RX)
    Unit yellow (RXD) -> D6 (TX)
    Unit red    (5V)  -> 5V
    Unit black  (GND) -> GND

  ESP32 dev board wiring:
    Unit white  (TXD) -> RX2 (GPIO16)
    Unit yellow (RXD) -> TX2 (GPIO17)
    Unit red    (5V)  -> VIN
    Unit black  (GND) -> GND

  Pro Micro wiring:
    Unit white  (TXD) -> Pro Micro RXI (pin 0)
    Unit yellow (RXD) -> 1k resistor -> Pro Micro TXO (pin 1)
                         2k resistor from yellow to GND (drops 5V to about 3.3V)
    Unit red    (5V)  -> Pro Micro VCC
    Unit black  (GND) -> Pro Micro GND

  First run: open the Serial Monitor, hold a tag to the reader, copy its EPC
  into REGISTERED_TAGS below, then set NEAR_RSSI_DBM from the RSSI you observe.
*/

// ---------------------------- Settings to edit ------------------------------

// EPCs of your registered tags, exactly as printed in the Serial Monitor.
// Upper/lower case does not matter. Add as many lines as you need.
const char* const REGISTERED_TAGS[] = {
  "E2000020090C005615701854",
};

// A tag counts as NEAR once its smoothed signal is at or above this level.
// This is a starting guess. Calibrate it with your tags and distances.
const int NEAR_RSSI_DBM = -55;

// The tag only drops back to FAR once it is this far below NEAR_RSSI_DBM.
// The gap stops the status from flickering around the threshold.
const int NEAR_HYSTERESIS_DB = 5;

// Transmit power in hundredths of a dBm: 2600 = 26 dBm. The Unit's range is
// 18-26 dBm. Lowering it shortens the distance at which tags can be seen at all.
const uint16_t TX_POWER_CENTI_DBM = 2600;

// Reader region: 0x02 = US 902-928 MHz, which matches the 915 MHz tags.
const uint8_t REGION_CODE = 0x02;

const uint16_t POLL_INTERVAL_MS = 40;   // how often to scan for tags
const uint16_t TAG_LOST_MS = 4000;      // no reads for this long = GONE
const uint16_t REPORT_INTERVAL_MS = 500;  // live RSSI line per visible tag
const bool PRINT_LIVE_RSSI = false;     // true = also print a status line twice a second
const bool PRINT_EVERY_READ = true;     // READ,<epc>,<rssi>,<registered> for marble_range_test.html

// How much each new reading moves the average (0-1). Lower = steadier, slower.
const float RSSI_SMOOTHING = 0.3;

// ---------------------------- Reader protocol -------------------------------
//
// Frame: BB | type | command | length (2 bytes) | parameters | checksum | 7E
// The checksum is the low byte of the sum of type through the last parameter.

const uint8_t FRAME_HEADER = 0xBB;
const uint8_t FRAME_END = 0x7E;

const uint8_t TYPE_COMMAND = 0x00;
const uint8_t TYPE_RESPONSE = 0x01;
const uint8_t TYPE_NOTICE = 0x02;

const uint8_t CMD_GET_VERSION = 0x03;
const uint8_t CMD_SET_REGION = 0x07;
const uint8_t CMD_GET_REGION = 0x08;
const uint8_t CMD_POLL_ONCE = 0x22;
const uint8_t CMD_SET_TX_POWER = 0xB6;
const uint8_t CMD_ERROR = 0xFF;

const uint8_t ERROR_NO_TAG = 0x15;

const uint8_t MAX_FRAME_PARAMS = 48;
const uint8_t MAX_EPC_BYTES = 16;  // the usual EPC is 12 bytes (96 bits)

// A tag notice carries RSSI (1 byte), PC (2), the EPC, then a tag CRC (2).
const uint8_t TAG_NOTICE_OVERHEAD = 5;

// One entry per tag currently in view. This has to stay above the first function
// so the Arduino IDE's generated function prototypes can see the type.
const uint8_t MAX_VISIBLE_TAGS = 8;

struct VisibleTag {
  bool inUse;
  bool registered;
  bool near;
  float rssi;
  unsigned long lastSeen;
  unsigned long lastReport;
  char epc[MAX_EPC_BYTES * 2 + 1];
};

VisibleTag tags[MAX_VISIBLE_TAGS];

uint8_t frameType;
uint8_t frameCommand;
uint8_t frameLength;
uint8_t frameParams[MAX_FRAME_PARAMS];

// Feeds waiting bytes through the parser. Returns true when a complete frame
// with a valid checksum is in the frame* variables. Bad frames are dropped.
bool readFrame() {
  static uint8_t position = 0;
  static uint8_t sum = 0;

  while (Serial1.available()) {
    uint8_t b = Serial1.read();

    if (position == 0) {
      if (b == FRAME_HEADER) position = 1;
      continue;
    }

    if (position < 5) {
      sum = (position == 1) ? b : sum + b;
      if (position == 1) frameType = b;
      if (position == 2) frameCommand = b;
      if (position == 3 && b != 0) { position = 0; continue; }  // too long
      if (position == 4) {
        if (b > MAX_FRAME_PARAMS) { position = 0; continue; }  // too long
        frameLength = b;
      }
      position++;
    } else if (position < 5 + frameLength) {
      frameParams[position - 5] = b;
      sum += b;
      position++;
    } else if (position == 5 + frameLength) {
      if (b != sum) { position = 0; continue; }  // corrupted
      position++;
    } else {
      position = 0;
      if (b == FRAME_END) return true;
    }
  }
  return false;
}

void sendCommand(uint8_t command, const uint8_t* params, uint8_t length) {
  uint8_t sum = TYPE_COMMAND + command + length;
  Serial1.write(FRAME_HEADER);
  Serial1.write(TYPE_COMMAND);
  Serial1.write(command);
  Serial1.write((uint8_t)0);
  Serial1.write(length);
  for (uint8_t i = 0; i < length; i++) {
    Serial1.write(params[i]);
    sum += params[i];
  }
  Serial1.write(sum);
  Serial1.write(FRAME_END);
}

// Sends a command and waits for its response, which is left in frame*.
bool sendAndWait(uint8_t command, const uint8_t* params, uint8_t length) {
  while (Serial1.available()) Serial1.read();
  sendCommand(command, params, length);

  unsigned long start = millis();
  while (millis() - start < 500) {
    if (readFrame() && frameType == TYPE_RESPONSE) {
      return frameCommand == command;
    }
  }
  return false;
}

void configureReader() {
  uint8_t versionQuery[] = {0x00};
  while (!sendAndWait(CMD_GET_VERSION, versionQuery, sizeof(versionQuery))) {
    Serial.println(F("No reply from reader. Check power and that TX/RX are not swapped."));
  }
  Serial.print(F("Reader version: "));
  for (uint8_t i = 1; i < frameLength; i++) Serial.print((char)frameParams[i]);
  Serial.println();

  uint8_t region[] = {REGION_CODE};
  if (!sendAndWait(CMD_SET_REGION, region, sizeof(region))) {
    Serial.println(F("WARNING: could not set region."));
  }
  if (sendAndWait(CMD_GET_REGION, NULL, 0)) {
    Serial.print(F("Region code now: "));
    Serial.println(frameParams[0]);
  }

  uint8_t power[] = {(uint8_t)(TX_POWER_CENTI_DBM >> 8), (uint8_t)(TX_POWER_CENTI_DBM & 0xFF)};
  if (!sendAndWait(CMD_SET_TX_POWER, power, sizeof(power))) {
    Serial.println(F("WARNING: could not set transmit power."));
  }
}

// ------------------------------ Tag tracking --------------------------------

bool isRegistered(const char* epc) {
  const uint8_t count = sizeof(REGISTERED_TAGS) / sizeof(REGISTERED_TAGS[0]);
  for (uint8_t i = 0; i < count; i++) {
    if (strcasecmp(epc, REGISTERED_TAGS[i]) == 0) return true;
  }
  return false;
}

void printTag(const VisibleTag& tag, const __FlashStringHelper* status) {
  Serial.print(tag.registered ? F("[REGISTERED]   ") : F("[UNREGISTERED] "));
  Serial.print(tag.epc);
  Serial.print(F("  "));
  Serial.print(status);
  Serial.print(F("  RSSI "));
  Serial.print(tag.rssi, 1);
  Serial.println(F(" dBm"));
}

void printStatus(const VisibleTag& tag) {
  printTag(tag, tag.near ? F("NEAR") : F("FAR"));
}

// Returns the slot tracking this EPC, claiming a free one for a new tag.
VisibleTag* findOrAddTag(const char* epc, bool& isNew) {
  VisibleTag* freeSlot = NULL;
  isNew = false;
  for (uint8_t i = 0; i < MAX_VISIBLE_TAGS; i++) {
    if (!tags[i].inUse) {
      if (freeSlot == NULL) freeSlot = &tags[i];
    } else if (strcmp(tags[i].epc, epc) == 0) {
      return &tags[i];
    }
  }
  if (freeSlot != NULL) {
    isNew = true;
    strcpy(freeSlot->epc, epc);
  }
  return freeSlot;
}

void handleTagNotice() {
  if (frameLength <= TAG_NOTICE_OVERHEAD) return;
  uint8_t epcLength = frameLength - TAG_NOTICE_OVERHEAD;
  if (epcLength > MAX_EPC_BYTES) return;

  // RSSI is a signed byte in dBm, e.g. 0xC9 = -55 dBm.
  int rssi = (int8_t)frameParams[0];

  char epc[MAX_EPC_BYTES * 2 + 1];
  const char hexDigits[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < epcLength; i++) {
    uint8_t value = frameParams[3 + i];
    epc[i * 2] = hexDigits[value >> 4];
    epc[i * 2 + 1] = hexDigits[value & 0x0F];
  }
  epc[epcLength * 2] = '\0';

  if (PRINT_EVERY_READ) {
    Serial.print(F("READ,"));
    Serial.print(epc);
    Serial.print(',');
    Serial.print(rssi);
    Serial.print(',');
    Serial.println(isRegistered(epc) ? 1 : 0);
  }

  bool isNew;
  VisibleTag* tag = findOrAddTag(epc, isNew);
  if (tag == NULL) return;  // more than MAX_VISIBLE_TAGS tags at once

  unsigned long now = millis();
  tag->lastSeen = now;

  if (isNew) {
    tag->inUse = true;
    tag->registered = isRegistered(epc);
    tag->rssi = rssi;
    tag->near = rssi >= NEAR_RSSI_DBM;
    tag->lastReport = now;
    printStatus(*tag);
    return;
  }

  tag->rssi += RSSI_SMOOTHING * (rssi - tag->rssi);

  bool wasNear = tag->near;
  if (tag->rssi >= NEAR_RSSI_DBM) {
    tag->near = true;
  } else if (tag->rssi < NEAR_RSSI_DBM - NEAR_HYSTERESIS_DB) {
    tag->near = false;
  }

  if (tag->near != wasNear) {
    printStatus(*tag);
    tag->lastReport = now;
  }
}

void updateTags() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_VISIBLE_TAGS; i++) {
    VisibleTag& tag = tags[i];
    if (!tag.inUse) continue;

    if (now - tag.lastSeen >= TAG_LOST_MS) {
      printTag(tag, F("GONE"));
      tag.inUse = false;
    } else if (PRINT_LIVE_RSSI && now - tag.lastReport >= REPORT_INTERVAL_MS) {
      printStatus(tag);
      tag.lastReport = now;
    }
  }
}

// -------------------------------- Main ---------------------------------------

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_XIAO_ESP32C3) || defined(ARDUINO_XIAO_ESP32S3)
  Serial1.begin(115200, SERIAL_8N1, D7, D6);  // RX, TX
#elif defined(ESP32)
  Serial1.begin(115200, SERIAL_8N1, 16, 17);  // RX2, TX2 on the board
#else
  Serial1.begin(115200);
#endif

  // Give the Serial Monitor a moment to connect, but run without one too.
  unsigned long start = millis();
  while (!Serial && millis() - start < 3000) {}

  configureReader();
  Serial.println(F("Scanning for tags..."));
}

void loop() {
  static unsigned long lastPoll = 0;

  unsigned long now = millis();
  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    sendCommand(CMD_POLL_ONCE, NULL, 0);
  }

  while (readFrame()) {
    if (frameType == TYPE_NOTICE && frameCommand == CMD_POLL_ONCE) {
      handleTagNotice();
    } else if (frameCommand == CMD_ERROR && frameParams[0] != ERROR_NO_TAG) {
      Serial.print(F("Reader error code: "));
      Serial.println(frameParams[0]);
    }
  }

  updateTags();
}
