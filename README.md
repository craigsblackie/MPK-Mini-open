# MPK mini mk1 Open Source Firmware & BLE MIDI Bridge

This repository contains the open-source firmware for the AKAI MPK mini mk1 (STM32F102R8T6) and the accompanying ESP32-C3 Bluetooth LE MIDI bridge. Together, they enable wireless MIDI connectivity for the keyboard while maintaining full compatibility with the original hardware features.

The repository is organised into two main parts:
- `stm32/`: The main keyboard firmware (STM32F102).
- `esp32/`: The Bluetooth LE MIDI bridge (ESP32-C3).

---

## 1. STM32 Firmware (Keyboard)

This firmware is a from-scratch replacement for the application in the AKAI MPK mini mk1.

### Features
- **Full Compatibility**: Maintains original USB MIDI identity, note/velocity formulas, and matrix scanning.
- **Extended Features**: Selectable velocity curves, SysEx editor support, and custom boot signature.
- **BLE Integration**: Simultaneously supports USB MIDI and bidirectional BLE MIDI via an ESP32-C3 bridge.
- **Arpeggiator**: Built-in arpeggiator with multiple modes and external MIDI clock support.

### Build and Flash
**Requirements**: `arm-none-eabi-gcc` and `make`.

**Build:**
```bash
cd stm32
make
```
Outputs:
- `stm32/build/mpk-mini-open.bin`: Application image for flashing.
- `stm32/build/mpk-mini-open.elf`: Debug image.

**Flash:**
If the original updater is present, flash only the application slot:
```bash
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg \
  -c "adapter speed 1000" -c "init" -c "reset halt" \
  -c "flash write_image erase stm32/build/mpk-mini-open.bin 0x08002000 bin" \
  -c "verify_image stm32/build/mpk-mini-open.bin 0x08002000 bin" \
  -c "reset run" -c "shutdown"
```

---

## 2. ESP32-C3 BLE MIDI Bridge

This project turns an **ESP32-C3 SuperMini (HW-466AB)** into a bidirectional BLE MIDI transport.

### Parts List
- **ESP32-C3 SuperMini** (HW-466AB or equivalent), USB-C.
- **Diode**: 1N5819 (Schottky) or 1N4007.
- **Capacitor (Optional)**: 100 µF electrolytic (10 V or higher) for power stability.

### Build and Flash
**Requirements**: ESP-IDF (v6.0.3).

**Build & Flash:**
```bash
cd esp32
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

### Bringing-up Order
1. **Flash the ESP32 alone** (unplugged from keyboard). It should advertise as `MPK mini Open`.
2. **Bench-wire the signals** (GND, TX, RX) with separate USB power for each board.
3. **Connect power** via the diode (see below) and verify USB MIDI from the keyboard still works.

### Editor Portal
Press the **BOOT** button on the ESP32-C3 to host a local WiFi access point:
- **SSID**: `MPK-mini-Open`
- **Password**: `mpkmini1`
- **Address**: `http://192.168.4.1/`

This allows you to edit your velocity curves and programs directly from your browser.

---

## 3. Hardware Setup & Wiring

### Signal Wiring (UART Link)
Connect the STM32 and ESP32-C3 directly via TTL (3.3V logic).

| MPK mini STM32 | ESP32-C3 SuperMini | Purpose |
|---|---|---|
| **PA9** (LQFP64 pin 42) | **GPIO4** | STM32 MIDI TX $\to$ ESP RX |
| **PA10** (LQFP64 pin 43) | **GPIO5** | ESP MIDI TX $\to$ STM32 RX |
| **GND** | **GND** | Common Reference |

*Note: On the AD07 board, PA9 and PA10 must be soldered directly to the 0.5 mm-pitch LQFP64 pins.*

### Power Wiring (Keyboard-Powered)
The ESP32-C3 is powered from the MPK's +5V rail via a diode to prevent power conflict when both are connected to USB.

| MPK mini | Component | ESP32-C3 SuperMini |
|---|---|---|
| **+5 V Net** | 1A Diode (Anode to MPK, Cathode to ESP) | **5V Pin** |
| **GND** | Direct Connection | **GND Pin** |

![Wiring Diagram](wiring.jpg)

**Important**: Do **NOT** feed the ESP32's `3V3` pin from the MPK's 3.3V rail. Use the `5V` pin to allow the ESP32's onboard regulator to handle the load.

