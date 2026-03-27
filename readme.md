# UAD2 — Linux Kernel Driver for Universal Audio Thunderbolt Interfaces

> [!WARNING]
> **AI-Generated Code and Research**
>
> This driver, including all source code, reverse engineering analysis, register
> maps, and documentation, was written entirely by **Claude Opus 4.6**
> (Anthropic). The code has **not been reviewed by Universal Audio** and is
> based solely on reverse engineering of the macOS kext binary. Use at your own
> risk. This driver may damage your hardware, corrupt audio, cause kernel
> panics, or behave unpredictably.

> [!NOTE]
> From a human (only this part and some Nix related code is written by me) and
> all the other stuff is written by Claude. Tested on a UA Apollo Solo with a
> NixOS laptop. Playback, capture (with condenser mic + 48V phantom), rate
> switching (48k/96k), and full duplex all work. PipeWire integration verified.
> ALSA mixer controls for monitor volume/mute and preamp gain/phantom/pad are
> available via `alsamixer`, `pavucontrol`, or `amixer`.

## Acknowledgments

This driver incorporates findings and code patterns from
**[open-apollo](https://github.com/rolotrealanis98/open-apollo)** by
rolotrealanis98, a GPL-2.0 open-source Linux driver for Apollo Thunderbolt
interfaces built through clean-room reverse engineering. Key contributions taken
from open-apollo:

- Serial prefix device detection and per-model channel count tables
- v1/v2 firmware detection via FPGA revision register
- DSP mixer batch write protocol (38-setting cache, SEQ handshake, flush)
- Capture routing activation (settings[1-37]=0x20/0xFF)
- DSP service loop (readback drain + mixer flush as host liveness signal)
- Post-transport clock write for DSP active processing mode
- Extended mode transport start (0x20F) for v2 firmware
- P2P_ROUTE write in PrepareTransport
- Bank-shifted notification register reads (rate_info, xport_info, clock_info)

## Supported Devices

All Apollo Thunderbolt 3/4 devices share PCI vendor `0x1A00`, device `0x0002`.
The driver detects the model at probe time via PCI subsystem ID (preferred) or
serial prefix (BAR0+0x20..0x2C) as fallback.

| Device              | Type ID | Play Ch | Rec Ch | Preamps | Status                                     |
| ------------------- | ------- | ------- | ------ | ------- | ------------------------------------------ |
| Apollo Solo         | `0x27`  | 42      | 32     | 1       | Tested (play, capture, rate switch, mixer) |
| Arrow               | `0x28`  | 42      | 32     | 1       | Untested                                   |
| Apollo Twin X       | `0x23`  | 8       | 8      | 2       | Untested                                   |
| Apollo Twin X Gen 2 | `0x3A`  | 8       | 8      | 2       | Untested                                   |
| Apollo x4           | `0x1F`  | 24      | 22     | 4       | Untested                                   |
| Apollo x4 Gen 2     | `0x36`  | 24      | 22     | 4       | Untested                                   |
| Apollo x6           | `0x1E`  | 24      | 22     | 4       | Untested                                   |
| Apollo x6 Gen 2     | `0x35`  | 24      | 22     | 4       | Untested                                   |
| Apollo x8           | `0x22`  | 26      | 26     | 4       | Untested                                   |
| Apollo x8 Gen 2     | `0x37`  | 26      | 26     | 4       | Untested                                   |
| Apollo x8p          | `0x20`  | 26      | 26     | 8       | Untested                                   |
| Apollo x8p Gen 2    | `0x38`  | 26      | 26     | 8       | Untested                                   |
| Apollo x16          | `0x21`  | 34      | 34     | 8       | Untested                                   |
| Apollo x16 Gen 2    | `0x39`  | 34      | 34     | 8       | Untested                                   |
| Apollo x16D         | `0x2A`  | 34      | 34     | 0       | Untested                                   |

Thunderbolt 2 devices (original Apollo Twin, Apollo 8, Apollo 16, Duo, Quad) are
**not supported** — Linux generally does not enumerate TB2 PCIe devices.

## Changelog

### 2026-03-28: ALSA mixer controls for monitor + preamp

Added 12 ALSA mixer controls — works with `alsamixer`, `pavucontrol`, `wpctl`,
`amixer` out of the box:

- **Monitor**: Playback Volume (0-192), Playback Switch (mute), Dim Switch,
  Source (MIX/CUE1/CUE2)
- **Preamp** (per channel): Capture Volume (-144..+65 dB), 48V Phantom Power,
  Pad, Low Cut, Phase Invert, Input Select (Mic/Line)

All controls write through the DSP mixer batch protocol — no chardev, no daemon,
no userspace tools required. Changes are flushed to hardware within 10ms via the
DSP service loop.

### 2026-03-27: Multi-device support + bug fixes + capture

**Multi-device:**

- Serial prefix detection for all 15 Apollo TB models with per-model channel
  count tables. PCI ID table opened to match all devices.
- PCI subsystem ID used as primary device detection (serial prefix unreliable on
  some models — Apollo Solo serial coincidentally matches x16D prefix).
- Dynamic ALSA card name from detected model ("UA Apollo Solo").

**Bug fixes:**

- **Fix: sample rate switching** — stop transport before rate change, full
  re-prepare with rate-correct parameters (IRQ period, periodic timer), then
  post-transport clock write to enable DSP active processing.
- **Fix: extended transport mode** — v2 firmware devices now use `0x20F` start
  value (BIT 9 = EXT_MODE). Previously hardcoded to `0xF`.
- **Fix: capture I/O errors** — three bugs prevented capture from working:
  1. Shared `dma_pos_offset` between play/rec caused pointer corruption (split
     into per-direction offsets)
  2. Transport started in pcm_prepare instead of trigger (caused XRUN before
     first read)
  3. Hardware periodic timer IRQ (vector 0x46) never fires on Apollo Solo
     (replaced with hrtimer at 1ms polling — same approach as snd-usb-audio)
- **Fix: device detection** — PCI subsystem ID preferred over serial prefix
  (Apollo Solo serial "2032" falsely matched x16D)
- **Fix: PCIe ASPM** — disable Active State Power Management on the Thunderbolt
  bridge chain to prevent link sleep during continuous MMIO reads (DSP service
  loop). Without this, PCIe completion timeouts can cascade into system freeze.

**New features:**

- Mixer batch protocol: 38-setting val/mask cache with SEQ handshake
- DSP service loop: 100Hz delayed_work for readback drain + mixer flush
- Notification reads: rate_info, xport_info, clock_info after connect
- Sample Rate ALSA control: read-write enum with all 6 rates (44.1k–192k)
- PCIe ASPM disable + completion timeout increase on TB bridge chain

**Verified on live Apollo Solo hardware:**

- Playback: 42ch S32_LE at 48kHz and 96kHz
- Capture: condenser mic with +48V phantom → -42 dBFS on Ch0 (12MB captured)
- Rate switching: 48k → 96k → 48k without errors
- Full duplex: simultaneous playback + capture
- PipeWire: PCM RUNNING with 6.4ms latency, stable hw_ptr
- DSP service loop: 46K+ iterations without crash
- ASPM: upstream TB bridge ASPM disabled at probe time

**Known limitations:**

- PipeWire capture returns zeros (needs UCM2/WirePlumber channel mapping config
  to map hardware Ch0 → mic input; direct ALSA capture via `arecord -D hw:N`
  works fine)
- No DSP plugin chain (disabled in both this driver and open-apollo — clobbers
  capture routing)
- No bus coefficient control (fader/pan/send levels for internal DSP mixer
  routing — requires ring buffer command protocol)
- Preamp/monitor controls write to DSP mixer settings but hardware response has
  not been verified for all parameters on all models

## Overview

This is a Linux kernel ALSA/PCIe driver for **Universal Audio Apollo
Thunderbolt** audio interfaces, reverse-engineered from the macOS kext
`com.uaudio.driver.UAD2System` (arm64e, v11.7.0 build 2).

### Reverse Engineering Sources

| File                                | Description                                       |
| ----------------------------------- | ------------------------------------------------- |
| `bin/aarch64-darwin/uad2.kext`      | Raw arm64e kext binary (primary RE source)        |
| `bin/aarch64-darwin/uad2.kext.dump` | Disassembly via `otool -v -s __TEXT_EXEC __text`  |
| `bin/x86_64-darwin/uad2.kext`       | x86_64 kext binary (2017 symbols incl. 1228 text) |
| `bin/x86_64-darwin/uad2.kext.dump`  | x86_64 disassembly (cross-reference for arm64 RE) |

### macOS Driver Stack

```
IOPCIDevice
  → com_uaudio_driver_UAD2Pcie2
    → com_uaudio_driver_UAD2System
      → com_uaudio_driver_UAD2AudioEngine
        → com_uaudio_driver_UAD2AudioStream
```

## Hardware Identity

Confirmed via `ioreg` on live hardware:

| Property              | Value                                                 |
| --------------------- | ----------------------------------------------------- |
| PCI Vendor ID         | `0x1A00`                                              |
| PCI Device ID         | `0x0002`                                              |
| PCI Subsystem ID      | `0x000F`                                              |
| PCI Class Code        | `0x048000` (Multimedia > Other)                       |
| BAR 0                 | 64 KB MMIO window (phys `0x1203000000`, length 65536) |
| PCIe Link             | Gen1 x2 (2.5 GT/s), MSI capable                       |
| Thunderbolt Tunnelled | Yes (Intel JHL8440 controller)                        |
| Thunderbolt Vendor    | Universal Audio, Inc. (TB vendor ID `0x1176`)         |
| TB Device Model       | "Apollo Solo" (model ID `0x0B`, revision 1, ROM v32)  |
| ioreg IOName          | `pci1a00,2`                                           |
| PCI BDF               | `45:0:0`                                              |

The PCI ID table matches all UA Thunderbolt devices (`0x1A00:0x0002`). The model
is determined at probe time via PCI subsystem ID or serial prefix detection.

## Audio Engine Specifications

Confirmed from `ioreg` IOAudioEngine + IOAudioStream on macOS:

| Property                   | Value                                                                  |
| -------------------------- | ---------------------------------------------------------------------- |
| Sample Rates               | 44100, 48000, 88200, 96000, 176400, 192000 Hz                          |
| Buffer Size                | 8192 frames (`IOAudioEngineNumSampleFramesPerBuffer`), 16-frame offset |
| Format                     | 32-bit container, 24-bit depth, signed integer, **little-endian**      |
| Byte Order                 | 1 (MSB-justified LPCM)                                                 |
| Numeric Representation     | `sint` (`0x73696E74`)                                                  |
| Sample Format              | `lpcm` (`0x6C70636D`)                                                  |
| Input (Record) Channels    | 32 — MIC/LINE/HIZ, ADAT, S/PDIF, VIRTUAL, MON, AUX                     |
| Output (Playback) Channels | 42 — MON, LINE, ADAT, S/PDIF, VIRTUAL, CUE, RESERVED                   |
| Default Sample Rate        | 48000 Hz                                                               |
| DMA Layout                 | Interleaved                                                            |

## Building

### Prerequisites

The project uses a **Nix flake** (`flake.nix`) that provides:

- Linux kernel headers (6.19+; uses `hrtimer_setup()` API introduced in 6.8)
- `bear` (for `compile_commands.json` generation)
- `clang-tools` (for LSP)
- `gnumake`

### Build Commands

```sh
# Enter the Nix devshell (or use direnv)
nix develop

# Build the module
make

# Generate compile_commands.json for LSP
bear -- make
```

The module compiles cleanly with zero warnings against kernel 6.19+.

> **Note:** LSP errors about `-mpreferred-stack-boundary=3`,
> `-mindirect-branch=thunk-extern`, etc. are **false positives** — clang does
> not understand GCC-specific kernel flags from `compile_commands.json`. These
> can be safely ignored.

### Loading

```sh
sudo insmod uad2.ko
```

## Register Map

All registers are BAR-relative offsets, 32-bit MMIO.

### Global / Device

| Offset   | Register                     | Access | Description                                   |
| -------- | ---------------------------- | ------ | --------------------------------------------- |
| `0x0014` | Interrupt Enable             | W      | Write `0xFFFFFFFF` to unmask all interrupts   |
| `0x0030` | Firmware Base Address (low)  | R      | Low 32 bits of 64-bit firmware base address   |
| `0x0034` | Firmware Base Address (high) | R      | High 32 bits; read after SG table programming |

### DMA Master Control (`CPcieIntrManager`)

| Offset   | Register               | Access | Description                                                                                                                                                                                                                                        |
| -------- | ---------------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x2200` | DMA Master Control     | R/W    | Bitmask, shadow register pattern. `ResetDMAEngines` writes: single=`0x1E00`, dual=`0x1FE00` (written twice + readback). `EnableDMA`: sets bit `(1 << (dsp_index + 1))`; bit 0 reserved. Software shadow at `IntrManager+0x4BC`, never read from HW |
| `0x2204` | DMA0 Interrupt Control | W      | Write `0x0` to clear/disable                                                                                                                                                                                                                       |
| `0x2208` | DMA0 Status / IRQ Arm  | W      | Write `0xFFFFFFFF` to arm; write `0x0` to clear                                                                                                                                                                                                    |

### Device Identification

| Offset   | Register            | Access | Description                                                                                |
| -------- | ------------------- | ------ | ------------------------------------------------------------------------------------------ |
| `0x2218` | Device ID           | R      | Read to verify device identity. Also polled after clock change (2s timeout)                |
| `0x2220` | Device ID Handshake | W      | Write `0` after reading device ID (kext `WriteRegEjj` hardcodes offset 0x2220 and value 0) |

### Audio Transport Registers (`CPcieAudioExtension`)

| Offset   | Register                      | Access | Description                                                                                                                                                                                                 |
| -------- | ----------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x2240` | Buffer Frame Size             | W      | Write `bufferFrameSize - 1` (hardware mask)                                                                                                                                                                 |
| `0x2244` | DMA Position Counter          | R/W    | Read: current frame position (wrapping counter, not byte offset). Write `0` to clear. If `pos > bufferFrameSize`, clamp to 0                                                                                |
| `0x2248` | Transport Control             | R/W    | State machine: `0x000`=stop/reset, `0x001`=armed/prepared, `0x00F`=running (normal), `0x20F`=running (extended, device variants `0xA`/`0x9`). Read status bits: bit5=running, bit7=overflow, bit8=underflow |
| `0x224C` | Playback Monitor Config       | W      | Write `(totalPlayChans - 1) \| 0x100` — **only when `diagnostic_flags & 0x2`** (Apollo Solo: skipped, flags=1)                                                                                              |
| `0x2250` | Playback Channel Count        | W      | Total playback channels                                                                                                                                                                                     |
| `0x2254` | Poll Status                   | R      | Poll for DMA ready (compare vs `irq_period`). Read-only; firmware populates after arm.                                                                                                                      |
| `0x2258` | Interrupt Period              | W      | IRQ period value from rate lookup table: 8 (44.1/48k), 16 (88.2/96k), 32 (176.4/192k). **Not** buffer_frames/N.                                                                                             |
| `0x225C` | Record Channel Count          | W      | Record channels (+1 if monitor bit set)                                                                                                                                                                     |
| `0x2260` | Stream Enable / Clock Trigger | W      | `0x1`=start, `0x10`=stop/disconnect, `0x4`=clock change                                                                                                                                                     |

### DMA Engine 1 (Dual-Engine Devices Only)

Note: register roles are swapped relative to DMA0 (confirmed from kext
`CPcieIntrManager` cross-check):

| Offset   | Register               | Access | Description                                       |
| -------- | ---------------------- | ------ | ------------------------------------------------- |
| `0x2264` | DMA1 Status / IRQ Arm  | R/W    | Read: pending bits; Write: arm (like DMA0 0x2208) |
| `0x2268` | DMA1 Interrupt Control | W      | Write enable shadow (like DMA0 0x2204)            |

### Buffer / Timer

| Offset   | Register                | Access | Description                                  |
| -------- | ----------------------- | ------ | -------------------------------------------- |
| `0x226C` | Buffer Size in KB       | W      | Write `(totalPlayChans * (bufSz - 1)) >> 10` |
| `0x2270` | Periodic Timer Interval | W      | Timer interval in frames                     |

### Monitor

| Offset   | Register                  | Access | Description                                          |
| -------- | ------------------------- | ------ | ---------------------------------------------------- |
| `0x22C0` | Playback Monitor Status   | R      | Flush/status in PrepareTransport + TransportPosition |
| `0x22C4` | Secondary Counter Control | W      | Write `0` on Shutdown                                |

### Scatter-Gather DMA Tables (`CPcieAudioExtension::ProgramRegisters` @ `0x4BAC0`)

| Range            | Description                                                                                                                                                                              |
| ---------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x8000..0x9FFF` | **Buffer A (Playback)** SG table — 1024 entries x 8 bytes = 8192 bytes. Entry format: `[low32 phys_addr][high32 phys_addr]` per 4 KB page. Covers 1024 x 4 KB = 4 MB playback DMA buffer |
| `0xA000..0xBFFF` | **Buffer B (Capture)** SG table — same layout, offset = Buffer A + `0x2000`. Covers 4 MB capture DMA buffer                                                                              |

**SG Programming Loop** (confirmed from disassembly lines 70514–70569):

```c
sg_offset = 0x8000; dma_offset = 0;
do {
    WriteReg(BAR + sg_offset,          physA_lo);  // playback page low
    WriteReg(BAR + sg_offset + 4,      physA_hi);  // playback page high
    WriteReg(BAR + sg_offset + 0x2000, physB_lo);  // capture page low
    WriteReg(BAR + sg_offset + 0x2004, physB_hi);  // capture page high
    dma_offset += 0x1000;
    sg_offset += 8;
} while (sg_offset != 0xA000);
```

### Sample Clock (`CPcieAudioExtension::_setSampleClock`)

| Offset                              | Register              | Access | Description                                                                                                                                               |
| ----------------------------------- | --------------------- | ------ | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `(channel_base_index * 4) + 0xC04C` | Sample Clock Register | W      | Write `clock_source \| (rate_enum << 8)`. `clock_source` 0 = internal. After write: write `0x4` to `BAR+0x2260` (trigger), poll `BAR+0x2218` (2s timeout) |

**UAD2SampleRate enum** (1-based):

| Enum | Rate      |
| ---- | --------- |
| 1    | 44100 Hz  |
| 2    | 48000 Hz  |
| 3    | 88200 Hz  |
| 4    | 96000 Hz  |
| 5    | 176400 Hz |
| 6    | 192000 Hz |

### Firmware Mailbox (`0xC000` Range, Per-Channel via `channel_base_index`)

| Offset                               | Register            | Access | Description                                                                     |
| ------------------------------------ | ------------------- | ------ | ------------------------------------------------------------------------------- |
| `0xC000`                             | Channel Config Area | R      | 10 DWORDs of channel config                                                     |
| `(channel_base_index << 2) + 0xC004` | Doorbell Register   | W      | Write `0x0ACEFACE` as connect command                                           |
| `(channel_base_index << 2) + 0xC008` | Notification Status | R/W    | Read: 32-bit bitmask of pending events. Write `0`: clear/acknowledge all events |

**Notification Status Bits:**

| Bit | Event                                            |
| --- | ------------------------------------------------ |
| 0   | Playback IO ready                                |
| 1   | Record IO ready                                  |
| 4   | DMA ready                                        |
| 5   | Connect ack                                      |
| 6   | Error                                            |
| 7   | End buffer                                       |
| 21  | Channel config (also forces bits 0+1 in handler) |
| 22  | Rate change                                      |

### Playback/Record IO Descriptor Areas

These use **flat BAR offsets** — no `channel_base_index` shift applied
(confirmed by cross-check against kext disassembly):

| Offset   | Description                                                                    |
| -------- | ------------------------------------------------------------------------------ |
| `0xC1A4` | Record IO descriptors — 72 DWORDs (`0x120` bytes) of record channel config     |
| `0xC2C4` | Playback IO descriptors — 72 DWORDs (`0x120` bytes) of playback channel config |

## Channel Base Index

Constant loaded from binary data segment at virtual address `0x6018` during
`Initialize`.

| Device      | `channel_base_index` | Secondary Field (`this+0x28`) |
| ----------- | -------------------- | ----------------------------- |
| Apollo Solo | **10** (`0x0A`)      | 380 (`0x017C`)                |

Used for all firmware mailbox register address calculations:

```
actual_offset = (10 << 2) + base_offset = 0x28 + base_offset
```

## DSP Ring Buffer Registers

From `CPcieRingBuffer::ProgramRegisters` @ `0x14C48`:

### Base Address Calculation

```
if (dsp_index < 4):  ring_base = BAR + 0x2000 + (dsp_index * 0x80)
if (dsp_index >= 4): ring_base = BAR + 0x5E00 + (dsp_index * 0x80)
```

Second ring (ring2) at `ring_base + 0x40`.

**Apollo Solo** has 1 DSP (index 0):

- `ring0_base = BAR + 0x2000`
- `ring1_base = BAR + 0x2040`

### Per-Ring Register Layout

Offsets relative to `ring_base`:

| Offset   | Register                      | Access | Description                      |
| -------- | ----------------------------- | ------ | -------------------------------- |
| `+0x00`  | Descriptor 0 phys addr (low)  | W/R    | 64-bit DMA address, low 32 bits  |
| `+0x04`  | Descriptor 0 phys addr (high) | W/R    | 64-bit DMA address, high 32 bits |
| `+0x08`  | Descriptor 1 phys addr (low)  | W/R    |                                  |
| `+0x0C`  | Descriptor 1 phys addr (high) | W/R    |                                  |
| `+0x10`  | Descriptor 2 phys addr (low)  | W/R    |                                  |
| `+0x14`  | Descriptor 2 phys addr (high) | W/R    |                                  |
| `+0x18`  | Descriptor 3 phys addr (low)  | W/R    |                                  |
| `+0x1C`  | Descriptor 3 phys addr (high) | W/R    |                                  |
| `+0x20`  | Descriptor Count              | W      | Write `ring_size`                |
| `+0x24`  | Ring Size                     | W      | Write `ring_size`                |
| `+0x28`  | Hardware Ring Capacity        | R      | Cap value at `0x400`             |
| `+0x1A4` | DSP Ready Poll                | R      | Wait for `0xA8CAED0F`            |

### Programming Sequence

1. Read `ring_base+0x28` → `ring_size` (if >= `0x400`, cap to `0x400`)
2. Write `ring_base+0x24 = ring_size`
3. Write `ring_base+0x20 = ring_size`
4. For `i = 0..3`:
   - `phys_addr = dma_alloc->getPhysicalSegment(i * 0x1000)` (4 KB page aligned)
   - Write `ring_base + (i * 8) = phys_addr[31:0]`
   - Write `ring_base + (i * 8) + 4 = phys_addr[63:32]`
   - Read back both, verify match (return `-2` on mismatch)

## DSP Boot Wait

`_waitFor469ToStart` @ `0x1152C`:

The DSP poll address uses a **separate base** from the ring buffer base:

```
ring_base     = BAR + {0x2000|0x5e00} + dsp_index*0x80   (DMA rings)
dsp_poll_base = BAR + (dsp_index > 3 ? 0x2000 : 0) + dsp_index*0x800  (status)
```

For DSP 0: `ring_base = BAR+0x2000`, `poll_base = BAR+0x000`, poll address =
`BAR+0x1A4`

| Parameter                  | Value               |
| -------------------------- | ------------------- |
| Poll Register              | `poll_base + 0x1A4` |
| Ready Value                | `0xA8CAED0F`        |
| Poll Interval              | 300 ms (`0x12C`)    |
| Max Attempts (DSP type 0)  | 100 (30s timeout)   |
| Max Attempts (DSP type >0) | 10 (3s timeout)     |

## Initialization Sequence

From `CPcieDevice::ProgramRegisters` @ `0xDF48`, exact order:

1. **Read** `BAR+0x2218` (device ID)
2. **Verify** == `expected_device_id` (stored at `CPcieDevice+0xCC8`)
3. **Write** `BAR+0x2220 = 0` (handshake ack — kext `WriteRegEjj` always writes
   0 to 0x2220, ignoring its arguments)
4. **`CPcieIntrManager::ProgramRegisters`:**
   - Write `BAR+0x2204 = 0x0`
   - Write `BAR+0x2208 = 0x0`
   - (if dual DMA) Write `BAR+0x2264 = 0x0`, `BAR+0x2268 = 0x0`
5. **`CPcieIntrManager::ResetDMAEngines`:**
   - Clear DMA channel bits in shadow (preserve bit 0 = master enable)
   - Compute `reset_bits`: single=`0x1E00`, dual=`0x1FE00`
   - Write `BAR+0x2200 = shadow | reset_bits` (assert reset)
   - Write `BAR+0x2200 = shadow` (deassert reset — pulse pattern)
   - Read `BAR+0x2200` (readback flush)
6. **Write** `BAR+0x2208 = 0xFFFFFFFF` (arm interrupt)
7. **For each DSP** (Apollo Solo: 1 DSP):
   - `CPcieDSP::ProgramRegisters`:
     - Compute `ring_base`
     - Call `_waitFor469ToStart` (DSP type 0 only)
     - `CPcieRingBuffer::ProgramRegisters` x2 (ring0, ring1):
       - Read `ring_base+0x28` → `ring_size` (cap at `0x400`)
       - Write `ring_base+0x24 = ring_size`
       - Write `ring_base+0x20 = ring_size`
       - 4-iteration loop: write 64-bit DMA phys addrs to `ring_base+0x00..0x1C`
     - `CPcieIntrManager::EnableDMA(dsp_index)`:
       - `bit = 1 << (dsp_index + 1)`
       - `shadow |= bit`
       - Write `BAR+0x2200 = shadow`
8. **`CPcieAudioExtension::ProgramRegisters`** (@ `0x4BAC0`, lines 70401–70631):
   - Read `BAR+0x2248`, if bit5: Read `BAR+0x2244` (ack)
   - Write `BAR+0x2248 = 0x0` (clear)
   - Program SG tables: 1024-entry loop writing `BAR+0x8000..0xBFFF`
   - Read `BAR+0x30/0x34` → firmware base (64-bit, stored at `this+0x28C8`)
   - `EnableVector(0x28, 1)` → enable notification interrupt
9. **`CPcieAudioExtension::Connect`** (@ `0x4BE58`, lines 70632–70823):
   - Reset `notifyEvent`
   - 20-channel doorbell loop: write `0x0ACEFACE`, write `0x1` to `BAR+0x2260`
   - Wait on `notifyEvent` (100 ms x 10 retries per channel)
   - Firmware responds: bit 5 (connect ack), bit 21 (channel config)
   - Bit 21 forces bits 0+1 → copies IO descriptors, recomputes buffer size
   - `notifyEvent` signaled by second bit 21 check → wakes `Connect()`

## Notification Interrupt Handler

`_handleNotificationInterrupt` @ `0x4C154` (lines 70824–71446):

### Full Dispatch Chain

1. Acquire lock on `this+0x2890` (notification spinlock)
2. Check `this+0x2898` (connected flag); if 0, unlock and return
3. Read `BAR+(channel_base_index<<2)+0xC008` → notification bitmask
4. **Dispatch:**

| Bit  | Event                       | Action                                                                                                                                                                                                                                                                       |
| ---- | --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 5    | Connect ack                 | Signal `connectEvent` (`this+0x2838`)                                                                                                                                                                                                                                        |
| 21   | Channel config (first pass) | Lock `this+0x10`, write 10 DWORDs from `this+0x24` to `BAR+0xC000` (cached config → device), copy up to `0x5F` DWORDs from `BAR+(idx<<2)+0xC000` to `this+0x4C` (device → cache), unlock, parse channel names. **Forces bits 0, 1, and 22 on** (`orr w20, w20, #0x00400003`) |
| 0    | Playback IO ready           | Lock, copy 72 DWORDs from `BAR+0xC2C4` to `this+0x2E8`, unlock, parse IO names, call `_recomputeBufferFrameSize()`, `Callback(this, 0x66, 0, context)`                                                                                                                       |
| 1    | Record IO ready             | Lock, copy 72 DWORDs from `BAR+0xC1A4` to `this+0x1C8`, unlock, parse IO names, call `_recomputeBufferFrameSize()`, `Callback(this, 0x65, 0, context)`                                                                                                                       |
| 22   | Rate change                 | Read `BAR+0xC05C` and `BAR+0xC054`, `callback(0x67)`                                                                                                                                                                                                                         |
| 4    | DMA ready                   | Read `BAR+0xC054/0xC058/0xC05C`, `callback(0x64)`                                                                                                                                                                                                                            |
| 6    | Error                       | Log only                                                                                                                                                                                                                                                                     |
| 21   | (second pass)               | Signal `notifyEvent` (`this+0x2830`) — **wakes `Connect()`**                                                                                                                                                                                                                 |
| 7    | End buffer                  | Signal `endBufferEvent` (`this+0x2840`)                                                                                                                                                                                                                                      |
| 0\|1 | Combined IO ready           | `callback(0x6E)`                                                                                                                                                                                                                                                             |

5. Write `0` to `BAR+(channel_base_index<<2)+0xC008` (clear/ack)
6. Unlock notification spinlock

### Callback Routing

`_notifyIntrCallback` does NOT call the handler directly — it schedules via
`CUAOS::ScheduleRequest` (workqueue/deferred context). `_requestCallback` @
`0x4D6F8` is the deferred entry point that calls `_handleNotificationInterrupt`.
The handler uses spinlock at `this+0x2890` and checks the connected flag at
`this+0x2898`.

## Event Objects

`CPcieAudioExtension` event objects:

| Offset        | Name             | Created With                  | Signaled By         | Waited On By                    |
| ------------- | ---------------- | ----------------------------- | ------------------- | ------------------------------- |
| `this+0x2830` | `notifyEvent`    | `CUAOS::CreateEvent(name, 1)` | Bit 21 second pass  | `Connect()` with 100 ms timeout |
| `this+0x2838` | `connectEvent`   | `CUAOS::CreateEvent(name, 1)` | Bit 5 (connect ack) |                                 |
| `this+0x2840` | `endBufferEvent` | `CUAOS::CreateEvent(name, 1)` | Bit 7 (end buffer)  |                                 |

Event vtable: `+0x08`=Destroy, `+0x10`=Signal, `+0x18`=Wait(timeout),
`+0x28`=Reset

## Connect Sequence

`CPcieAudioExtension::Connect` @ `0x4BE58` (detailed):

1. Acquire mutex at `this+0x2828`
2. Reset `notifyEvent` (`this+0x2830`) via `vtable+0x28`
3. For `chan = 0..19`:
   - Lock HW spinlock (`this+0x2888`)
   - Write `BAR + (10 << 2) + 0xC004 = 0x0ACEFACE` (doorbell)
   - Write `BAR + 0x2260 = 0x1` (stream enable)
   - Unlock HW spinlock
   - `notifyEvent->Wait(100ms)`, up to 10 retries
   - On timeout: call `_handleNotificationInterrupt()` manually
4. After loop: check `this+0x20` (connected flag), validate `sample_rate_code`
5. On success: set `is_connected=1`, `buffer_count=12`
6. On failure: return `-92` (`kIOReturnNotReady`)
7. Disconnect: Write `BAR+0x2260 = 0x10`

## Buffer Frame Size Computation

`_recomputeBufferFrameSize` @ `0x4D9E0`:

```
bufferFrameSize = floor_pow2(0x400000 / (max(play_ch, rec_ch) * 4) / 2)
```

Capped at `0x2000` (8192). The `/2` step is the `ubfx x8, x8, #1, #31`
instruction at `0x4DA1C` in the arm64 kext, which performs an unsigned right
shift by 1 before the CLZ (count-leading-zeros) that implements floor_pow2.

**Fields read:** `this+0x1D4` (record channels), `this+0x2F8` (playback
channels) **Field written:** `this+0x287C` (bufferFrameSize)

**For Apollo Solo** (42 play, 32 rec):

```
0x400000 / (42 * 4) = 24966, / 2 = 12483 → floor_pow2 = 8192 → capped to 8192
```

## Transport Sequence

The hardware has a **single shared transport** for both playback and capture
directions. In macOS, CoreAudio's IOAudioEngine model starts/stops all streams
atomically through a mixer layer, so the kext never encounters independent
stream lifecycle. In ALSA, PipeWire opens playback and capture as independent
streams, so the Linux driver must coordinate them.

### Linux Driver Stream Lifecycle

The driver follows the `snd-dice`/`snd-bebob` pattern for shared-transport
management. The key principle is: **transport lifecycle is tied to
`pcm_open`/`pcm_close`, not to `hw_params`/`hw_free` or `trigger`**.

#### Reference Counters

| Counter            | Incremented      | Decremented     | Purpose                                      |
| ------------------ | ---------------- | --------------- | -------------------------------------------- |
| `open_count`       | `pcm_open`       | `pcm_close`     | Tracks open substreams; transport stops at 0 |
| `streams_prepared` | `pcm_prepare`    | `hw_free`       | Tracks prepared substreams                   |
| `streams_running`  | `trigger(START)` | `trigger(STOP)` | Tracks started substreams                    |

#### `pcm_open`

- Increments `open_count`.
- Sets ALSA hardware constraints (period size, buffer size = 8192 frames).
- Calls `snd_pcm_set_sync()` to signal shared clock to userspace.
- Adds `SNDRV_PCM_INFO_JOINT_DUPLEX` flag.

#### `pcm_prepare`

- Sets sample rate via `_setSampleClock` sequence.
- Increments `streams_prepared` (once per substream, tracked via
  `runtime->private_data` flag).
- **If `transport_state >= 1`** (already prepared or running): skips
  re-preparation entirely. Snapshots the current DMA position as
  `dma_pos_offset` so that `.pointer()` returns 0-relative positions matching
  ALSA's reset `hw_ptr=0`.
- **If `transport_state == 0`**: does full `PrepareTransport` sequence (programs
  all transport registers, SG tables, enables periodic timer interrupt).
- Always uses `dev->buffer_frames` (8192, hardware-computed) and
  `irq_period_frames` from the kext's rate-based lookup table: 8 at 44.1/48 kHz,
  16 at 88.2/96 kHz, 32 at 176.4/192 kHz.

#### `trigger(START)`

- Increments `streams_running`, sets per-stream `playback_running` or
  `capture_running` flag.
- Calls `StartTransport`. If `transport_state == 2` (already running), this is a
  no-op — the stream piggybacks on the existing transport.

#### `trigger(STOP)`

- Decrements `streams_running`, clears per-stream running flag.
- **Does NOT stop the hardware transport.** The DMA keeps running, writing
  harmlessly to the pre-allocated 4MB buffers. The periodic timer IRQ continues
  firing but skips `snd_pcm_period_elapsed()` for non-running streams.

This is critical for PipeWire, which rapidly cycles through `STOP` -> `hw_free`
-> `prepare` -> `START` during audio graph reconfiguration (e.g., when a new
application starts playing audio). Stopping the transport here would cause full
DMA re-initialization (30-40 second delays) and stale buffer replay.

#### `hw_free`

- Clears ALSA runtime DMA pointers.
- Decrements `streams_prepared` if this substream was prepared.
- Does NOT tear down the transport.

#### `pcm_close`

- Decrements `open_count`.
- **When `open_count` reaches 0**: calls `StopTransport` (if running) or resets
  `transport_state` to 0 (if prepared but not running). This is the only place
  during normal operation where the transport is torn down.

#### Interrupt Handling

The periodic timer (vector `0x46`, slot 62) drives audio processing:

- **Hardirq**: reads pending interrupt bits from DMA0/DMA1 STATUS registers,
  filters through `intr_enable_shadow`, ACKs only active bits. For the periodic
  timer vector: disables the one-shot vector in the enable shadow (kext
  fidelity), then calls `snd_pcm_period_elapsed()` directly for each running
  stream. Lock ordering: `dev->lock` is released before calling
  `snd_pcm_period_elapsed()` (which internally acquires the stream group lock).
- **Threaded handler**: re-arms vector `0x46` by calling `EnableVector` (the
  hardware disables this vector on each fire; see ServiceInterrupt analysis
  below). Dispatches deferred notification interrupts.

### PrepareTransport (kext sequence)

1. Check `transport_state != 2` (not already running)
2. Check `is_connected == 1`
3. Validate `bufferFrameSize < 0x10000`
4. Lock `hw_lock` (`this+0x10`)
5. Write `BAR+0x2240 = bufferFrameSize - 1`
6. Write `BAR+0x226C = (totalPlayChans * (bufSz - 1)) >> 10`
7. Write `BAR+0x2244 = 0` (clear position)
8. If `periodic_timer_interval != 0`:
   - Write `BAR+0x2270 = periodic_timer_interval`
   - `EnableVector(0x46, 1)` (enable periodic timer interrupt)
9. Write `BAR+0x2244 = 0` (clear again)
10. Read `BAR+0x2244` (flush)
11. Write `BAR+0x2248 = 0` (stop/reset)
12. Write `BAR+0x2258 = irqPeriod`
13. Write `BAR+0x2250 = totalPlayChans`
14. Write `BAR+0x225C = recChans`
15. Write `BAR+0x2248 = 1` (arm)
16. Read `BAR+0x22C0` (monitor flush)
17. If `diagnostic_flags` bit1: Write
    `BAR+0x224C = (totalPlayChans - 1) | 0x100`
18. Unlock `hw_lock`
19. Set `transport_state = 1` (prepared)
20. Poll `BAR+0x2254` until `== irqPeriod` (DMA ready, max ~2 retries with 1 ms
    sleep)
21. `EnableVector(0x47, 1)` (enable end-of-buffer interrupt)

### IRQ Period Lookup Table

The `irqPeriod` value written to `BAR+0x2258` in step 12 above comes from a
rate-based lookup table in the kext data segment (@ `0x6020` in the ARM64
binary). `_setSampleClock` loads the value into `this+0x2880`, and
`PrepareTransport` writes it to the register. The firmware uses this to
determine the DMA notification interval.

| Sample Rate | IRQ Period Value |
| ----------- | ---------------- |
| 44100 Hz    | 8                |
| 48000 Hz    | 8                |
| 88200 Hz    | 16               |
| 96000 Hz    | 16               |
| 176400 Hz   | 32               |
| 192000 Hz   | 32               |

**Critical:** The firmware expects these specific small values, NOT computed
values like `buffer_frames / 8` (= 1024). Writing 1024 causes the DMA engine to
never start — `REG_POLL_STATUS` (`0x2254`) stays at 0, and the transport never
enters the running state.

### StartTransport (@ line 71904)

1. Check `transport_state == 1` (prepared)
2. Check `is_connected`
3. Lock `hw_lock`
4. Write `BAR+0x2248 = 0x00F` (normal) or `0x20F` (variant `0xA` always; variant
   `0x9` if `extended_mode_flag` is zero)
5. Unlock `hw_lock`
6. Set `transport_state = 2` (running)

### StopTransport (@ line 71987)

1. Check `is_connected`
2. `DisableVector(0x47)` (disable end-of-buffer interrupt **FIRST**)
3. Lock `hw_lock`
4. Write `BAR+0x2248 = 0x0`
5. Unlock `hw_lock`
6. Set `transport_state = 0`

## Shutdown Sequence

`CPcieAudioExtension::Shutdown` @ `0x4CB0C` (line 71447), decoded from lines
71447–71537:

1. Lock `hw_lock` (`this+0x10`)
2. Write `BAR+0x2248 = 0` (stop transport)
3. Write `BAR+(channel_base_index<<2)+0xC004 = 0` (clear doorbell)
4. Read `BAR+0x2248` (flush / status read)
5. Write `BAR+0x22C4 = 0` (clear secondary counter)
6. Unlock `hw_lock`
7. `DisableVector(0x28)` (disable notification interrupt)
8. Tail-call `_disconnect()`:
   - Write `BAR+0x2260 = 0x10` (disconnect command)
   - Set `is_connected = 0`

## Sample Clock Change

`_setSampleClock` @ line 72712:

1. Combine: `clock_val = clock_source | (rate_enum << 8)`
2. Write `BAR + (channel_base_index * 4) + 0xC04C = clock_val`
3. Write `BAR + 0x2260 = 0x4` (trigger clock change)
4. Poll `BAR + 0x2218` for ack (timeout 2000 ms)

## MSI Interrupt Vectors

Registered in `Initialize`:

| Vector ID | Decimal | Callback                 | Priority | Slot | Description                           |
| --------- | ------- | ------------------------ | -------- | ---- | ------------------------------------- |
| `0x28`    | 40      | `_notifyIntrCallback`    | 0        | 32   | Reads `BAR+0xC008`, dispatches events |
| `0x46`    | 70      | `_periodicTimerCallback` | —        | 62   | Periodic timer interrupt              |
| `0x47`    | 71      | `_endBufferCallback`     | 1        | 63   | End-of-buffer notification            |

### Vector-to-Slot Indirection Table

The kext maintains a `vector_to_slot[72]` array at `IntrManager+0x4D0` that maps
logical vector IDs to bit slot positions within a u64 `intr_enable_shadow`:

- Slots 0–31 → DMA0 registers (`0x2204` ctrl, `0x2208` status)
- Slots 32–63 → DMA1 registers (`0x2268` ctrl, `0x2264` status)

For Apollo Solo, **all three audio vectors map to slots > 31**, so
`has_extended_irq` must be `true` and DMA1 registers are always accessed.

### Vector Enable/Disable (`CPcieIntrManager`)

**EnableVector** (`vec`, `arm`) @ `0x13CD4` (line 12349):

1. `slot = vector_to_slot[vec]` (from `IntrManager+0x4D0`)
2. `bit_mask = 1ULL << slot`
3. `intr_enable_shadow |= bit_mask`
4. Determine register pair: slot < 32 → DMA0 (0x2204/0x2208); slot >= 32 → DMA1
   (0x2268/0x2264)
5. If `arm`: write `(u32)(bit_mask >> shift)` to status register (re-arm)
6. Write `(u32)(intr_enable_shadow >> shift)` to ctrl register (enable mask)

**DisableVector** (`vec`) @ `0x13F34` (line 12502):

1. `slot = vector_to_slot[vec]`
2. `bit_mask = 1ULL << slot`
3. `intr_enable_shadow &= ~bit_mask`
4. Write updated shadow to ctrl register(s)

### Vector Lifecycle

| Slot | Vector | Enabled                                                     | Disabled                 |
| ---- | ------ | ----------------------------------------------------------- | ------------------------ |
| 32   | `0x28` | `ProgramRegisters`                                          | `Shutdown`               |
| 62   | `0x46` | `PrepareTransport` (only if `periodic_timer_interval != 0`) | —                        |
| 63   | `0x47` | End of `PrepareTransport` (after DMA ready poll)            | Start of `StopTransport` |

Additionally, DSP interrupt vectors `0..4` (for DSP 0: slots 2, 3, 4) are
enabled immediately after DSP programming in `CPcieDSP::Initialize`.

### ServiceInterrupt Flow (`CPcieIntrManager` @ `0x14330`)

Complete interrupt dispatch flow from kext analysis:

1. Read DMA0 status (`0x2208`) → `pending_lo`
2. If `has_extended_irq`: Read DMA1 status (`0x2264`) → `pending_hi`
   - `0xFFFFFFFF` = hot-unplug → treat as 0
3. Combine: `pending = pending_lo | (pending_hi << 32)`
4. `active = pending & intr_enable_shadow`
5. If `active == 0`: return (not our interrupt)
6. `priority_active = active & priority_mask`
7. If bit 63 set: immediate dispatch `callback[63]` (vector 0x47, end-of-buffer)
8. If bit 62 set: immediate dispatch `callback[62]` (vector 0x46, periodic
   timer)
9. Re-arm priority vectors: write active bits to DMA0/DMA1 status
10. `deferred = active & ~priority_mask` → accumulate into `deferred_pending`
11. One-shot disable: clear matching bits from `intr_enable_shadow`
12. Re-arm deferred: write to DMA0/DMA1 status

## Channel Type Name Table

From kext binary @ `0x60368`:

| Index | Name         |
| ----- | ------------ |
| 0     | MIC/LINE/HIZ |
| 1     | MIC/LINE     |
| 2     | AUX          |
| 3     | LINE         |
| 4     | ADAT         |
| 5     | S/PDIF       |
| 6     | AES/EBU      |
| 7     | VIRTUAL      |
| 8     | MADI         |
| 9     | MON          |
| 10    | CUE          |
| 11    | TALKBACK     |
| 12    | RESERVED     |
| 13    | DANTE        |
| 14    | UNKNOWN      |

### IO Descriptor Per-Channel Format

`uint16` at `descriptor_base + 0x18 + ch * 2`:

- **High byte** = type index (into table above)
- **Low byte** = sub-index within that type

### Naming Conventions (from `_parseIONames`)

- **Stereo types** (MON, S/PDIF, AES/EBU, AUX, CUE): pair via `(sub + 1) / 2`
- **L/R suffix**: even sub-index = L, odd = R

### Predicted Channel Mapping

> [!NOTE]
> These mappings are predicted from kext analysis and **have NOT been verified
> on live hardware**.

**Playback (42 channels):**

| Channels | Type                                            |
| -------- | ----------------------------------------------- |
| 0–1      | MON L/R (headphone/monitor — main audio output) |
| 2–3      | LINE L/R                                        |
| 4–11     | ADAT 1–8                                        |
| 12–13    | S/PDIF L/R                                      |
| 14–23    | VIRTUAL 1–10                                    |
| 24–27    | CUE 1–4                                         |
| 28–41    | RESERVED                                        |

**Record (32 channels):**

| Channels | Type                                               |
| -------- | -------------------------------------------------- |
| 0–1      | MIC/LINE/HIZ L/R (front panel inputs)              |
| 2–31     | Remaining channels TBD from live hardware readback |

## Object Layouts

### `CPcieDevice` (arm64e offsets)

| Offset   | Field                              | Description                             |
| -------- | ---------------------------------- | --------------------------------------- |
| `+0xC50` | `dsp_count`                        | `u32`                                   |
| `+0xC70` | `m_pRegisterWindowBaseAddr`        | BAR pointer                             |
| `+0xC80` | `CPcieDSP*` array                  | 8 bytes each, up to `APOLLO_MAX_DSPS=8` |
| `+0xCC8` | `expected_device_id`               | `u32`                                   |
| `+0xCD0` | `CPcieIntrManager*`                |                                         |
| `+0xCD8` | (second intr manager / bus object) |                                         |
| `+0xCE0` | (zeroed at Initialize)             |                                         |
| `+0xCF8` | `CPcieAudioExtension*`             |                                         |
| `+0xD58` | spinlock                           | `CUAOS::CreateSpinLock`                 |
| `+0xD60` | `0x100` (lock capacity)            |                                         |
| `+0xD68` | `0` (lock state)                   |                                         |

### `CPcieIntrManager`

Full object layout (0x5F0 bytes total, from arm64 analysis):

| Offset   | Size  | Field                      | Description                             |
| -------- | ----- | -------------------------- | --------------------------------------- |
| `+0x00`  | 8     | lock object pointer        | PAC-authenticated vtable                |
| `+0x08`  | 8     | BAR base pointer           |                                         |
| `+0x10`  | 8     | parent context             |                                         |
| `+0x18`  | 4     | `interrupts_enabled` flag  | `bool`                                  |
| `+0x20`  | 8     | `intr_enable_shadow`       | u64 bitmask (slots 0-63)                |
| `+0x28`  | 8     | `priority_mask`            | u64 — priority dispatch slots           |
| `+0x30`  | 8     | `deferred_pending`         | u64 — accumulated deferred slots        |
| `+0x38`  | 0x240 | `callback_ptrs[72]`        | Function pointers per slot              |
| `+0x278` | 0x240 | `callback_ctx[72]`         | Context pointers per slot               |
| `+0x4B8` | 4     | (padding/reserved)         |                                         |
| `+0x4BC` | 4     | `dma_ctrl_shadow`          | Shadow register for `BAR+0x2200`        |
| `+0x4C0` | 4     | `hw_version / is_extended` |                                         |
| `+0x4C4` | 4     | `has_extended_irq`         | Uses DMA1 registers for upper 32 slots  |
| `+0x4C8` | 4     | `oneshot_mask_lo`          |                                         |
| `+0x4CC` | 4     | `num_slots`                | 25 default, 64 extended                 |
| `+0x4D0` | 0x120 | `vector_to_slot[72]`       | Maps logical vector ID → bit slot index |

### `CPcieRingBuffer`

| Offset  | Field                    | Description                                                |
| ------- | ------------------------ | ---------------------------------------------------------- |
| `+0x00` | DMA alloc object pointer | `IOBufferMemoryDescriptor`, `vtable[6]=getPhysicalSegment` |
| `+0x10` | BAR base for this ring   | `reg_base`                                                 |
| `+0x34` | `hw_capacity`            | From hardware                                              |
| `+0x38` | `ring_size`              |                                                            |
| `+0x3C` | `ring_pos`               | Initialized same as `ring_size`                            |
| `+0x40` | `channel_id`             | Flag, controls log tag                                     |

### `CPcieAudioExtension` (Comprehensive)

| Offset     | Field                                  | Description                                                                                                                                |
| ---------- | -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `+0x08`    | reference count / instance ID          |                                                                                                                                            |
| `+0x10`    | `hw_lock`                              | Spinlock (`vtable+0x10`=Lock, `+0x18`=Unlock)                                                                                              |
| `+0x18`    | `CPcieIntrManager*` pointer            |                                                                                                                                            |
| `+0x20`    | `is_connected` flag                    | `u32`                                                                                                                                      |
| `+0x24`    | `channel_base_index`                   | `u32`, = 10 for Apollo Solo                                                                                                                |
| `+0x28`    | secondary field                        | = 380 (`0x17C`)                                                                                                                            |
| `+0xA1`    | `sample_rate_code`                     | `u8`                                                                                                                                       |
| `+0xA8`    | rate change value                      | From `BAR+0xC05C`                                                                                                                          |
| `+0x01C8`  | record IO descriptor copy area         | 72 DWORDs from `BAR+0xC1A4`                                                                                                                |
| `+0x01D4`  | `record_channel_count`                 |                                                                                                                                            |
| `+0x02E8`  | playback IO descriptor copy area       | 72 DWORDs from `BAR+0xC2C4`                                                                                                                |
| `+0x02F8`  | `playback_channel_count`               |                                                                                                                                            |
| `+0x2828`  | mutex                                  | `CUAOS::CreateMutex`                                                                                                                       |
| `+0x2830`  | `notifyEvent`                          | `CUAOS::CreateEvent`, auto-reset                                                                                                           |
| `+0x2838`  | `connectEvent`                         | `CUAOS::CreateEvent`, auto-reset                                                                                                           |
| `+0x2840`  | `endBufferEvent`                       | `CUAOS::CreateEvent`, auto-reset                                                                                                           |
| `+0x2848`  | BAR base pointer                       | Set in `MapHardware`                                                                                                                       |
| `+0x2858`  | notification callback function pointer |                                                                                                                                            |
| `+0x2860`  | notification callback context          |                                                                                                                                            |
| `+0x2868`  | end-buffer callback function pointer   |                                                                                                                                            |
| `+0x2870`  | end-buffer callback context            |                                                                                                                                            |
| `+0x2878`  | `transport_state`                      | 0=uninit, 1=prepared, 2=running                                                                                                            |
| `+0x287C`  | `bufferFrameSize`                      | Computed by `_recomputeBufferFrameSize`                                                                                                    |
| `+0x2880`  | `interrupt_period`                     | Rate-based: 8/16/32 for 1x/2x/4x rates                                                                                                     |
| `+0x2888`  | `hw_spinlock` #2                       |                                                                                                                                            |
| `+0x2890`  | notification spinlock                  | Guards `_handleNotificationInterrupt`                                                                                                      |
| `+0x2898`  | notification active flag               | Must be non-zero to process events                                                                                                         |
| `+0x28A0`  | DMA Buffer A pointer                   | Playback, 4 MB — set in `MapHardware`                                                                                                      |
| `+0x28A8`  | DMA Buffer B pointer                   | Capture, 4 MB — set in `MapHardware`                                                                                                       |
| `+0x28B0`  | `device_variant_id` / constant `0xC`   | Set in `Connect`                                                                                                                           |
| `+0x28B4`  | `current_sample_rate`                  | Enum                                                                                                                                       |
| `+0x28B8`  | `diagnostic_flags`                     | Init=1 for all devices. bit1: enable `0x224C` write; bit2: monitor channel adjust. Apollo Solo: flags=1, so `0x224C` write is **skipped**. |
| `+0x28BC`  | `diagnostic_status`                    |                                                                                                                                            |
| `+0x28C0`  | `periodic_timer_interval`              | Frames                                                                                                                                     |
| `+0x28C8`  | firmware base address                  | 64-bit, from `BAR+0x30/0x34`                                                                                                               |
| `+0x28D0`  | `UAD2DeviceType`                       |                                                                                                                                            |
| `+0x28D8`  | task/DMA tag                           | From `MapHardware` arg                                                                                                                     |
| `+0x28E0`  | flag: enable playback monitor read     |                                                                                                                                            |
| `+0x28E8`  | DMA mapper / IOMapper object           |                                                                                                                                            |
| `+0x22EF4` | extended mode flag                     | For variant `0x9` (constructed via `mov #0x2ef4; movk #0x2, lsl #16`)                                                                      |
| `+0x22EF0` | large-offset DMA address storage       |                                                                                                                                            |

## Known Limitations

- **Single MSI vector mode** — hardware has 3 vectors; all currently handled on
  vector 0
- **Channel count readback** from IO descriptors uses fixed offsets (may need
  tuning)
- **No ALSA mixer controls** for monitor routing (clock source selection IS
  implemented)
- **No firmware version verification**
- **Notification handlers partially implemented** — the kext dispatches
  notification bits 4 (DMA ready, callback 0x64), 7 (end buffer event), 22 (rate
  change readback, callback 0x67), and a combined 0|1 handler (callback 0x6E).
  The Linux driver omits all four — it only handles bits 0, 1, 5, 6, and 21.
  This is intentional: the driver does not need DMA-ready or end-buffer
  signaling, rate change is not enabled, and the combined IO callback is
  redundant with individual bit 0/1 handling.
- **Channel mapping predicted but NOT verified** on live hardware
- **No rate change while running** — sample rate switching has been
  reverse-engineered and tested (see "Sample Rate Switching" below) but is not
  enabled because it resets the device's internal DSP mixer routing. A userspace
  mixer application is required to restore mixer coefficients after a rate
  change; see "Future Work".
- **No DSP plugin support** — the mixer/DSP processing layer from the macOS kext
  is not implemented
- **No userspace mixer application** — the Apollo Solo's internal monitor
  mixer/routing is configured entirely by the macOS UAD Console app via a
  register interface at BAR+0x3800. Without a Linux equivalent, the mixer state
  can only be set by the Mac. See "Future Work".
- **No capture testing** — only playback has been tested; capture streams are
  exposed but untested
- **`speaker-test` crashes at 42 channels** — `speaker-test` allocates channel
  map arrays for up to 16 channels only, causing SIGSEGV at channel 16+. This is
  a bug in `speaker-test`, not the driver. Use PipeWire or direct `aplay`
  instead.

### DMA Position Behavior

The hardware DMA position register (`BAR+0x2244`) only updates at period
boundaries (every 256 frames at 48 kHz = 5.33 ms), not smoothly between
interrupts. The driver reports `SNDRV_PCM_INFO_BATCH` to inform ALSA/PipeWire of
this coarse position update behavior. Without this flag, PipeWire's rate
estimator sees discontinuities when the position jumps by 256 frames at once,
causing false XRUNs.

### Position-Offset Approach

When a running stream is re-prepared (e.g., PipeWire graph reconfiguration), the
driver does NOT stop and restart the DMA transport. Instead, it snapshots the
current DMA position as `dma_pos_offset` at prepare time, and the `.pointer()`
callback returns `(hw_pos - dma_pos_offset) % buffer_frames`. This gives ALSA a
0-relative position matching its reset `hw_ptr=0`, while the hardware transport
continues uninterrupted. Stopping and re-preparing the transport is
fundamentally broken on this hardware — `REG_POLL_STATUS` never converges after
a stop+re-prepare cycle.

### Thunderbolt Hot-Unplug Safety

The driver handles surprise removal (Thunderbolt cable pull) safely through
three mechanisms:

1. **`card->sync_irq`** — set to the MSI vector after `request_threaded_irq` in
   probe. This tells `snd_card_disconnect()` to synchronize against in-flight
   hardirq handlers before transitioning PCM substreams to DISCONNECTED state,
   closing a race where the hardirq could call `snd_pcm_period_elapsed()` on a
   substream that was just moved to DISCONNECTED.

2. **PCI error handlers** (`pci_error_handlers`) — an `.error_detected` callback
   immediately sets `dev->disconnecting = true` when the PCI subsystem reports
   an AER or Thunderbolt link-down event, gating all MMIO access before
   `.remove()` is called.

3. **`SNDRV_PCM_POS_XRUN`** — returned from `.pointer()` when the device is
   disconnecting, providing a semantically correct error signal to ALSA instead
   of returning stale or garbage position values.

The remove path performs an ordered teardown: `snd_card_disconnect()` →
`uad2_shutdown()` (firmware disconnect) → disable global interrupt enable → set
`disconnecting` flag → `free_irq` → free DMA buffers → `pci_iounmap` →
`snd_card_free` → release PCI resources. Verified via debug prints during
physical Thunderbolt cable pulls: all 5 phases complete in ~2-3 ms with clean
re-probe on reconnect.

## Testing Status

### Completed

- **Build**: compiles cleanly with zero warnings against kernel 6.19+
- **Module load**: `insmod uad2.ko` succeeds, card appears in
  `/proc/asound/cards`, PCM device visible in `aplay -l`
- **Firmware connect**: 20-channel doorbell sequence completes, IO descriptors
  read successfully, channel counts detected (42 play / 32 rec)
- **Interrupt delivery**: MSI vector 0 fires, notification handler dispatches
  connect/IO/DMA events correctly
- **DMA / SG tables**: playback and capture 4MB DMA buffers allocated, SG table
  entries written to BAR+0x8000/0xA000
- **Playback via PipeWire**: audio plays through browser and desktop apps.
  Multiple simultaneous applications work (PipeWire mixes to the 42ch hardware
  stream). Transport survives PipeWire graph reconfiguration without audible
  glitches. Zero XRUNs during sustained playback.
- **Multichannel duplex**: PipeWire "multichannel duplex" profile opens both
  42ch playback + 32ch capture simultaneously without issues
- **Direct ALSA playback**: `aplay -D hw:X,0 -f S32_LE -c 42 -r 48000` works
- **PipeWire profiles**: "multichannel output", "multichannel duplex", and "pro
  audio" profiles all functional

- **Thunderbolt hot-unplug/replug**: physical cable pull during idle and during
  active playback both produce clean teardown (all 5 remove phases complete in
  ~2-3 ms), no kernel oops, clean re-probe on reconnect

### Not Yet Tested

- **DSP plugins** — not implemented; both this driver and open-apollo have the
  plugin chain disabled (clobbers capture routing)
- **Suspend/resume** — stubs exist but untested on live hardware
- **Multi-device** (multiple Apollo units simultaneously)

### Sample Rate Switching

Rate switching is **fully implemented and working**. The driver stops the
transport, fires the firmware clock handshake, then does a full re-prepare with
rate-correct parameters and a post-transport clock write for DSP activation.

Tested: 48kHz → 96kHz → 48kHz via both `amixer` control and direct ALSA
(`aplay -D hw:N -r RATE`).

| Speed Class | Rates          | IRQ Period | Periodic Timer | Period Frames |
| ----------- | -------------- | ---------- | -------------- | ------------- |
| 1x          | 44100, 48000   | 8          | 255            | 256           |
| 2x          | 88200, 96000   | 16         | 511            | 512           |
| 4x          | 176400, 192000 | 32         | 1023           | 1024          |

## Future Work

### Completed (Previously Future)

- ~~Mixer controls~~ — 12 ALSA kcontrols for monitor + preamp (volume, mute,
  dim, gain, phantom, pad, lowcut, phase, mic/line, source)
- ~~Sample rate switching~~ — fully working with transport stop/re-prepare cycle
- ~~Capture~~ — working with hrtimer polling, condenser mic verified at -42 dBFS

### Remaining

**Chardev + ioctl interface** — `/dev/ua_apolloN` for advanced userspace control
(bus coefficients, raw register access, firmware loading). Required for a
standalone CLI/GUI mixer tool.

**Bus coefficient control** — fader/pan/send levels for DSP internal mixer
routing. Requires ring buffer command protocol (`SetMixerBusParam`).

**UCM2/WirePlumber configs** — PipeWire channel mapping so that capture shows as
named inputs (Mic 1, Line In, etc.) instead of raw 32-channel device. Can port
from open-apollo `configs/ucm2/` and `configs/wireplumber/`.

**Hardware readback parsing** — the 40-word readback from `BAR+0x3814` contains
preamp flags, monitor state, and gain values. Parsing these would let the driver
reflect actual hardware state in ALSA kcontrols.

#### Hardware Mixer Register Map (`CPcieDeviceMixer`)

The mixer uses 38 settings (indices 0x00–0x25), each occupying 8 bytes (two
32-bit registers). The register block is at `BAR+0x3800`:

**Control registers:**

| Offset  | R/W | Description                                         |
| ------- | --- | --------------------------------------------------- |
| `+0x08` | W   | Version/commit — write incremented counter to apply |
| `+0x0C` | R   | Firmware version counter (must match before write)  |
| `+0x10` | R/W | Readback ready (1=data available, write 0 to ack)   |
| `+0x14` | R   | Readback data base — 40 × uint32 metering values    |

**Mixer setting register offsets (relative to BAR+0x3800):**

| Index Range | Formula            | BAR+0x3800 Offsets     |
| ----------- | ------------------ | ---------------------- |
| 0x00–0x0F   | `index * 8 + 0xB4` | 0x00B4–0x0130 (8 gaps) |
| 0x10–0x1F   | `index * 8 + 0xBC` | 0x013C–0x01B8 (8 gaps) |
| 0x20–0x25   | `index * 8 + 0xC0` | 0x01C0–0x01EC          |

There are 8-byte gaps between ranges (at 0x0134 and 0x01B8).

**Per-setting data format (8 bytes = two 32-bit writes):**

```
Register at offset+0:  [31:16] = group_bits[15:0]  | [15:0] = coeff_word_0
Register at offset+4:  [31:16] = group_bits[31:16] | [15:0] = coeff_word_1
```

Where `group_bits` is a 32-bit bitmask of active/dirty bits and the coefficient
is split into two 16-bit halves.

**Commit protocol:**

1. Read firmware version from `+0x0C`
2. Verify it matches the driver's cached version
3. Write all 38 settings
4. Increment version, write to `+0x08` (acts as a doorbell/commit)

The firmware will not process settings if the version counters are out of sync.
The `GetReadback` metering path (40 values at `+0x14` through `+0xB0`) also
opportunistically flushes deferred settings.

#### Kext Mixer Functions (Analyzed)

| Function                                 | Dump Line | Description                                 |
| ---------------------------------------- | --------- | ------------------------------------------- |
| `CPcieDeviceMixer::Initialize`           | 68750     | Sets BAR base, syncs version counter        |
| `CPcieDeviceMixer::_writeSetting`        | 69047     | Writes one setting to hardware              |
| `CPcieDeviceMixer::_flushCachedSettings` | 69013     | Writes all 38 settings + commit             |
| `CPcieDeviceMixer::WriteSetting`         | 69222     | Public API: update cache + write + commit   |
| `CPcieDeviceMixer::GetReadback`          | 68921     | Reads 40 metering values + deferred flush   |
| `CUAD2DeviceMixer::SetOtherParam`        | 67693     | Maps param IDs → setting indices + bitmasks |
| `CUAD2DeviceMixer::AckSampleRateChange`  | 65455     | DMA command 0x1A0002 to firmware            |
| `CMessenger::DispatchSetMixerParam`      | 52910     | Userspace → kernel mixer param dispatch     |
| `CMessenger::DispatchSetMixerBusParam`   | 52794     | Batch mixer bus param dispatch              |

#### Current Implementation

The driver implements approach #2 — ALSA kcontrols that write directly to DSP
mixer settings via the batch protocol. Monitor params route to setting[2] with
per-param bitmasks. Preamp params route to setting[param_id + 7]. The DSP
service loop flushes all pending changes every 10ms.

Sample rate switching is fully working — the driver stops the transport, sets
the clock, and does a full re-prepare with correct parameters.

## Disassembly Reference

All analysis uses `bin/aarch64-darwin/uad2.kext.dump`, generated with:

```sh
otool -v -s __TEXT_EXEC __text bin/aarch64-darwin/uad2.kext > bin/aarch64-darwin/uad2.kext.dump
```

### Analyzed Functions

| Function                                            | Address              | Disasm Line | Status        |
| --------------------------------------------------- | -------------------- | ----------- | ------------- |
| `CPcieRingBuffer::ProgramRegisters`                 | `0x14C48`            | 13354       | Done          |
| `CPcieIntrManager::EnableDMA`                       | `0x13A4C`            | —           | Done          |
| `CPcieIntrManager::EnableVector`                    | `0x13CD4`            | 12349       | Done (v0.4.0) |
| `CPcieIntrManager::DisableVector`                   | `0x13F34`            | 12502       | Done (v0.4.0) |
| `CPcieAudioExtension::MapHardware`                  | `0x4BA0C`            | 70355       | Done (v0.3.0) |
| `CPcieAudioExtension::ProgramRegisters`             | `0x4BAC0`            | 70401       | Done (v0.3.0) |
| `CPcieAudioExtension::Connect`                      | `0x4BE58`            | 70632       | Done          |
| `CPcieAudioExtension::_handleNotificationInterrupt` | `0x4C154`            | 70824       | Done (v0.3.0) |
| `CPcieAudioExtension::Shutdown`                     | `0x4CB0C`            | 71447       | Done (v0.4.0) |
| `CPcieAudioExtension::_disconnect`                  | (tail-called)        | —           | Done (v0.4.0) |
| `CPcieAudioExtension::PrepareTransport`             | —                    | 71623       | Done (v0.4.0) |
| `CPcieAudioExtension::StartTransport`               | —                    | 71904       | Done (v0.4.0) |
| `CPcieAudioExtension::StopTransport`                | —                    | 71987       | Done (v0.4.0) |
| `CPcieAudioExtension::TransportPosition`            | —                    | 72057       | Done          |
| `CPcieAudioExtension::_recomputeBufferFrameSize`    | `0x4D9E0`            | 72414       | Done (v0.3.0) |
| `CPcieAudioExtension::_setSampleClock`              | —                    | 72712       | Done          |
| `CPcieDeviceMixer::Initialize`                      | `0x4A188`            | 68750       | Done          |
| `CPcieDeviceMixer::_writeSetting`                   | `0x4A610`            | 69047       | Done          |
| `CPcieDeviceMixer::_flushCachedSettings`            | `0x4A58C`            | 69013       | Done          |
| `CPcieDeviceMixer::WriteSetting`                    | —                    | 69222       | Done          |
| `CPcieDeviceMixer::GetReadback`                     | `0x4A420`            | 68921       | Done          |
| `CUAD2DeviceMixer::AckSampleRateChange`             | `0x46E74`            | 65455       | Done          |
| `CDeviceManager::_setSampleClock`                   | `0x2E8EC`            | 40218       | Done          |
| `CDeviceManager::SetAudioClock`                     | `0x2ECE8`            | 40475       | Done          |
| `CDeviceManager::_updateRoutingTablesForEvent`      | `0x2949C`            | 34773       | Done          |
| `CMessenger::DispatchSetAudioSampleRate`            | `0x3DAC0`            | 55867       | Done          |
| `CMessenger::DispatchSetMixerParam`                 | —                    | 52910       | Done          |
| `CMessenger::DispatchSetMixerBusParam`              | —                    | 52794       | Done          |
| `CUAD2AudioMixer::HandleNotification`               | `0x1A220`            | 18968       | Done          |
| `CUAD2AudioMixer::StartTransport`                   | `0x1B30C`            | 20059       | Done          |
| `CUAD2AudioMixer::StopTransport`                    | `0x1ABE4`            | 19598       | Done          |
| `_notifyIntrCallback`                               | (schedules deferred) | —           | Done (v0.4.0) |
| `_endBufferCallback`                                | —                    | —           | Done (v0.4.0) |
| `_periodicTimerCallback`                            | —                    | —           | Done (v0.4.0) |
| `_requestCallback`                                  | `0x4D6F8`            | —           | Done (v0.4.0) |

### Potential Future Analysis

- Multi-vector MSI setup (register multiple MSI vectors for `0x28`, `0x46`,
  `0x47`)
- `SetOtherParam` switch table — full mapping of param IDs 0x01–0x8A to mixer
  setting indices and bitmasks (required for userspace mixer implementation)
- `SetMixerBusParam` — bus/channel/group/value parameter format
- `CUAD2AudioMixer::HandleNotification` — notification dispatch table for IDs
  0x67–0x6F (partially analyzed: 0x68 = rate change ack, 0x6E/0x6F = routing
  table rebuild)

## Project Files

| File                                | Description                                                                                                                                 |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `uad2.c`                            | Main driver source (~2900 lines) — PCI probe/remove, ALSA PCM ops, interrupt handling, DMA, firmware connect, transport, mixer, channel map |
| `Makefile`                          | Standard out-of-tree kernel module Makefile (uses `KDIR` from env)                                                                          |
| `Kbuild`                            | `obj-m := uad2.o`                                                                                                                           |
| `flake.nix`                         | Nix flake providing build environment                                                                                                       |
| `.clang-format`                     | Linux kernel coding style                                                                                                                   |
| `compile_commands.json`             | LSP compilation database (generated via `bear -- make`)                                                                                     |
| `bin/aarch64-darwin/uad2.kext`      | arm64e kext binary (primary RE source)                                                                                                      |
| `bin/aarch64-darwin/uad2.kext.dump` | arm64e disassembly                                                                                                                          |
| `bin/x86_64-darwin/uad2.kext`       | x86_64 kext binary (2017 symbols incl. 1228 defined text with mangled C++ names)                                                            |
| `bin/x86_64-darwin/uad2.kext.dump`  | x86_64 disassembly (65,463 lines, cross-reference source)                                                                                   |

## License

I retain absolutely zero copyright and there will be absolutely zero warranty.
Use at your own risk.
