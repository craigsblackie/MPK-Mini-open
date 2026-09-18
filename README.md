# Open MPK mini mk1 firmware

This directory contains a from-scratch replacement for the application in
the AKAI MPK mini mk1 (AD07, STM32F102R8T6). It runs on the attached board
and retains the unit's original 8 KiB updater at `0x08000000`.

## Implemented compatibility

- Stock USB identity and Audio/MIDI descriptor tree (`09e8:007c`), USB MIDI
  IN/OUT, and the board's PA8-controlled D+ pull-up. The integrated ESP32-C3
  build changes only `bMaxPower`, from 100 mA to 500 mA.
- The nine-column matrix, stock key/panel debounce depths, all 25 dual-contact
  velocity keys, and the original note/velocity formulas.
- Eight ADC/DMA knobs with per-program CC, low/high range and channel.
- Eight velocity pads, Bank A/B, Note/CC/Program Change modes, momentary and
  toggle behavior, and separate pad MIDI channel.
- Arp On/Off and modifier mappings, six modes (Up, Down, Inclusive,
  Exclusive, Order, Random), octave range, latch, internal/tap tempo, and
  external MIDI clock transport.
- Sustain, Program-select modifier, Tap Tempo, and Octave Down/Up including
  the both-buttons reset gesture.
- Both 74HC164 LED bytes: pad latches plus Bank A/B, CC, Program Change, Arp,
  Tap Tempo, and octave status. Invalid configuration also reproduces the
  stock three-cycle all-LED factory-reset indication.
- A custom boot signature runs two 16-light circular chases (the second
  accelerating) and a short all-light arrival flash before normal status.
- Simultaneous bidirectional BLE MIDI over an ESP32-C3 SuperMini via USART1
  PA9/PA10, while the normal USB MIDI connection remains active. The bridge's
  own firmware, wiring and keyboard-powered supply are in
  `../esp32-c3-ble-midi/`.
- Five 101-byte program records (scratch plus four persistent programs), the
  stock flash-page layout, validation rules, defaults, and current-program
  marker.

Beyond stock:

- Selectable velocity response curves, separately for the keys and the pads:
  Linear, Soft, Medium soft, Hard, Very hard, and Fixed. `Linear` is the
  default and is bit-identical to stock, so a device that has never been given
  a curve behaves exactly as before. They are stored in their own block in the
  program flash page, not in the 101-byte record -- every byte of that record
  is already allocated by the original layout, and a stock-editor program
  write would clobber anything hidden there. Read and written over SysEx with
  the added `'v'` command; `'v'` is unused by stock, so it cannot shadow a
  real command.
- Editor SysEx write/select/read/status/bootstrap and universal identity
  requests. Dumps for programs 0-4, bootstrap side effects, status, and the
  identity response have been compared byte-for-byte with the backed-up
  original firmware on the physical unit.
- The exact original updater remains installed. Its PROGRAM-at-power-on
  updater entry and application checksum validation therefore remain stock
  behavior, not a reimplementation.

The stock `` ` `` service command is accepted without a reply, matching its
externally observable behavior. Non-SysEx channel messages sent *to* the
controller are ignored; the original controller has no user-facing MIDI-thru
or sound-engine behavior for them. MIDI realtime clock/start/continue/stop is
consumed by the arp when external clock is selected.

See `../FIRMWARE_ANALYSIS.md` for the reverse-engineering evidence and field
maps.

## Build

```bash
make
```

Host-side tests for the parts that are pure logic:

```bash
cd test && make check
```

This checks every velocity curve is monotonic, stays inside 1..127, anchors at
127, and never mutes a note; that `Linear` passes all 127 inputs through
untouched; and that the settings block still fits the flash page it shares with
the program store.

This requires `arm-none-eabi-gcc`. The outputs are:

- `build/mpk-mini-open.elf` — symbols/debug image.
- `build/mpk-mini-open.bin` — a `0x5800`-byte application image for
  `0x08002000`; its last halfword is patched so the stock updater's additive
  checksum over `0x08002000..0x080077ff` is zero.

## Flash

The binary is deliberately application-only. It does not contain or
redistribute AKAI's updater.

If the original updater is already present at `0x08000000..0x08001fff`, flash
only the application slot:

```bash
openocd -f interface/cmsis-dap.cfg -f target/stm32f1x.cfg \
  -c "adapter speed 1000" -c init -c "reset halt" \
  -c "flash write_image erase build/mpk-mini-open.bin 0x08002000 bin" \
  -c "verify_image build/mpk-mini-open.bin 0x08002000 bin" \
  -c "reset run" -c shutdown
```

If address zero contains an older monolithic replacement, first restore the
first `0x2000` bytes from a personal backup of that same keyboard. Do not
flash `build/mpk-mini-open.bin` at address `0x08000000`; that would overwrite
the retained updater and put the vector table at the wrong address.

The program store begins at `0x08007800` and is intentionally outside the
application image.

## Hardware validation

The current implementation has been built warning-free and exercised on the
AD07 board through USB and SWD. Checks include enumeration/ALSA binding,
bidirectional SysEx, exact stock/open reply comparisons, program write and
power-cycle persistence, live ADC generations, idle matrix polarity, LED
GPIO configuration/state bytes, application checksum acceptance, and the
stock-updater-to-open-application handoff.
