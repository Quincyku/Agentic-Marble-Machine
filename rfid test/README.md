# RFID Test

A test rig for the marble machine. A passive UHF RFID tag goes in a marble, a reader sits next to the bowl, and a browser page shows the message assigned to whichever tag is in range. The same page measures read range, to help work out the marble's geometry.

## Hardware

- [M5Stack UHF-RFID Unit (JRD-4035)](https://shop.m5stack.com/products/uhf-rfid-unit-jrd-4035)
- [UHF 915 MHz ceramic RFID tags](https://rfidinc.com/product/uhf-915-mhz-ceramic-rfid-tags/)
- Seeed Studio XIAO ESP32-S3

## Files

| File | Purpose |
|---|---|
| `rfid_reader/rfid_reader.ino` | Scans for tags and reports every read over USB |
| `marble_range_test.html` | Browser page for tag messages, registration and range tests |
| `reader_check/reader_check.ino` | Connection test to run if the reader doesn't respond |

## Wiring

| Reader wire | XIAO pin |
|---|---|
| Red (5V) | `VBUS` / `5V` |
| Black (GND) | `GND` |
| White (TXD) | `D7` (RX) |
| Yellow (RXD) | `D6` (TX) |

Don't connect the red wire to `3V3`. `VBUS` only has 5V when the board is powered over USB.

The sketches also include pin settings for the XIAO ESP32-C3, a classic ESP32 dev board and a Pro Micro. The wiring for each is at the top of `rfid_reader.ino`. Those boards have only been compile-tested with this setup, except the ESP32 dev board, which never got a reply from the reader.

## Setup

1. Open `rfid_reader/rfid_reader.ino` in the Arduino IDE.
2. Select **XIAO_ESP32S3** as the board and upload the sketch. If the upload fails, hold `BOOT` while plugging in USB, then try again.
3. Close the Arduino Serial Monitor, since it blocks the port.
4. Open `marble_range_test.html` in Chrome or Edge. Safari doesn't support Web Serial.
5. Click **Connect reader** and choose the `usbmodem` port.

## Using the page

### Tag messages

Bring a registered tag near the reader and its message fills the top of the page. The message clears after 3 seconds without a read, and you can change that time on the page.

To register a tag:

1. Hold one new tag near the reader, keeping other unregistered tags away.
2. Type a message in the **New tag found** box.
3. Press **Register**.

Tags and messages are stored in the browser's local storage, so they don't carry over to another browser or computer. The page starts with one tag already registered: `E2000020090C005615701854`, "An email from Hiroshi".

The page keeps its own tag list. The `REGISTERED_TAGS` list in `rfid_reader.ino` only affects the text shown in the Arduino Serial Monitor.

### Range tests

The reader can't measure distance, so measure it with a ruler:

1. Enter the distance and a note describing the setup, such as the marble material and tag orientation.
2. Press **Measure** and hold the tag still for the test period.
3. Repeat at the next distance. The results can be downloaded as a CSV.

Test one tag at a time, because results add up the reads from all registered tags in range.

Tips for useful results:

- Look for the distance where reads per second stay steady, not the furthest single read.
- Test several orientations, since a marble can land any way up.
- Test with the tag inside a real marble, and in the real bowl.
- Hold the tag with something non-conductive, because your hand can affect the signal.

## Findings so far

Measured with the tag held close to the reader, in open air:

| Scan interval | Region | Reads in 20 s |
|---|---|---|
| 100 ms | US | 16 |
| 400 ms | US | 2 |
| 100 ms | Code 1 | 18 |
| 40 ms (current) | US | 37 |

- Only about 7% of scans catch the tag, so a message can take a moment to appear.
- Signal strength swung from about -33 to -52 dBm between reads, so a single reading is a poor guide to distance.
- Near/far detection in the sketch isn't calibrated. `NEAR_RSSI_DBM` is still a starting guess.
- These tags are sold as metal-mountable, so they may behave differently on or near metal.

The page's tag messages and range tests have been checked with simulated reads. Using the page with the real reader in Chrome is still to be confirmed.

## Troubleshooting

If the reader doesn't respond, upload `reader_check/reader_check.ino` and open the Serial Monitor at 115200 baud. It checks the signal lines, then tries both pin orders at several baud rates and prints what it finds. If it reports no reply, check the black and red wires first, and make sure the board's pins are soldered or held firmly.

Reader commands are sent directly over serial, because M5Stack's Arduino library doesn't fit on a Pro Micro. The frames are based on the [M5Unit-UHF-RFID library](https://github.com/m5stack/M5Unit-UHF-RFID).
