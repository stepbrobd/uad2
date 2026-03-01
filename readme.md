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

## Overview

This is a Linux kernel ALSA/PCIe driver for the **Universal Audio Apollo Solo
Thunderbolt** audio interface, reverse-engineered from the macOS kext
`com.uaudio.driver.UAD2System` (arm64e, v11.7.0 build 2).

### Reverse Engineering Sources

| File                                | Description                                          |
| ----------------------------------- | ---------------------------------------------------- |
| `bin/aarch64-darwin/uad2.kext`      | Raw arm64e kext binary (primary RE source)           |
| `bin/aarch64-darwin/uad2.kext.dump` | Disassembly via `otool -v -s __TEXT_EXEC __text`     |
| `bin/x86_64-darwin/uad2.kext`       | x86_64 kext binary (fully stripped, not used for RE) |
| `bin/x86_64-darwin/uad2.kext.dump`  | x86_64 disassembly (not used)                        |

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
| PCIe Link             | Gen1 x1, MSI capable                                  |
| Thunderbolt Tunnelled | Yes (Intel JHL8440 controller)                        |
| Thunderbolt Vendor    | Universal Audio, Inc. (TB vendor ID `0x1176`)         |
| TB Device Model       | "Apollo Solo" (model ID `0x0B`, revision 1, ROM v32)  |
| ioreg IOName          | `pci1a00,2`                                           |
| PCI BDF               | `45:0:0`                                              |

The PCI ID table in the driver is extensible for other UA Thunderbolt devices
(Apollo Twin, x4, x6, etc.).

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

- Linux kernel headers (currently 6.19.4)
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

The module compiles cleanly with zero warnings against kernel 6.19.4.

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

| Offset   | Register              | Access | Description                                                                                     |
| -------- | --------------------- | ------ | ----------------------------------------------------------------------------------------------- |
| `0x2218` | Device ID / Handshake | R/W    | Read to verify device; write back (echo handshake). Also polled after clock change (2s timeout) |

### Audio Transport Registers (`CPcieAudioExtension`)

| Offset   | Register                      | Access | Description                                                                                                                                                                                                 |
| -------- | ----------------------------- | ------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x2240` | Buffer Frame Size             | W      | Write `bufferFrameSize - 1` (hardware mask)                                                                                                                                                                 |
| `0x2244` | DMA Position Counter          | R/W    | Read: current frame position (wrapping counter, not byte offset). Write `0` to clear. If `pos > bufferFrameSize`, clamp to 0                                                                                |
| `0x2248` | Transport Control             | R/W    | State machine: `0x000`=stop/reset, `0x001`=armed/prepared, `0x00F`=running (normal), `0x20F`=running (extended, device variants `0xA`/`0x9`). Read status bits: bit5=running, bit7=overflow, bit8=underflow |
| `0x224C` | Playback Monitor Config       | W      | Write `(totalPlayChans - 1) \| 0x100`                                                                                                                                                                       |
| `0x2250` | Playback Channel Count        | W      | Total playback channels                                                                                                                                                                                     |
| `0x2254` | Poll Status                   | R      | Poll for DMA ready (compare vs `irq_period`)                                                                                                                                                                |
| `0x2258` | Interrupt Period              | W      | IRQ period in frames                                                                                                                                                                                        |
| `0x225C` | Record Channel Count          | W      | Record channels (+1 if monitor bit set)                                                                                                                                                                     |
| `0x2260` | Stream Enable / Clock Trigger | W      | `0x1`=start, `0x10`=stop/disconnect, `0x4`=clock change                                                                                                                                                     |

### DMA Engine 1 (Dual-Engine Devices Only)

| Offset   | Register               | Access | Description |
| -------- | ---------------------- | ------ | ----------- |
| `0x2264` | DMA1 Interrupt Control | W      | Write `0x0` |
| `0x2268` | DMA1 Status            | W      | Write `0x0` |

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

| Offset                               | Description                                                                    |
| ------------------------------------ | ------------------------------------------------------------------------------ |
| `(channel_base_index << 2) + 0xC1A4` | Record IO descriptors — 72 DWORDs (`0x120` bytes) of record channel config     |
| `(channel_base_index << 2) + 0xC2C4` | Playback IO descriptors — 72 DWORDs (`0x120` bytes) of playback channel config |

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

| Parameter                  | Value               |
| -------------------------- | ------------------- |
| Poll Register              | `ring_base + 0x1A4` |
| Ready Value                | `0xA8CAED0F`        |
| Poll Interval              | 300 ms (`0x12C`)    |
| Max Attempts (DSP type 0)  | 100 (30s timeout)   |
| Max Attempts (DSP type >0) | 10 (3s timeout)     |

## Initialization Sequence

From `CPcieDevice::ProgramRegisters` @ `0xDF48`, exact order:

1. **Read** `BAR+0x2218` (device ID)
2. **Verify** == `expected_device_id` (stored at `CPcieDevice+0xCC8`)
3. **Write** `BAR+0x2218 = device_id` (echo handshake)
4. **`CPcieIntrManager::ProgramRegisters`:**
   - Write `BAR+0x2204 = 0x0`
   - Write `BAR+0x2208 = 0x0`
   - (if dual DMA) Write `BAR+0x2264 = 0x0`, `BAR+0x2268 = 0x0`
5. **`CPcieIntrManager::ResetDMAEngines`:**
   - Compute `ctrl = dual ? 0x1FE00 : 0x1E00`
   - Write `BAR+0x2200 = ctrl` (twice)
   - Read `BAR+0x2200` (readback verify)
   - Save shadow register
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

| Bit  | Event                       | Action                                                                                                                                                                                                                   |
| ---- | --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| 5    | Connect ack                 | Signal `connectEvent` (`this+0x2838`)                                                                                                                                                                                    |
| 21   | Channel config (first pass) | Lock `this+0x10`, copy 10 DWORDs from `BAR+0xC000` to `this+0x24`, copy up to `0x5F` DWORDs from `BAR+(idx<<2)+0xC000` to `this+0x4C`, unlock, parse channel names. **Forces bits 0+1 on** (`orr w20, w20, #0x00400003`) |
| 0    | Playback IO ready           | Lock, copy 72 DWORDs from `BAR+0xC2C4` to `this+0x2E8`, unlock, parse IO names, call `_recomputeBufferFrameSize()`, `Callback(this, 0x66, 0, context)`                                                                   |
| 1    | Record IO ready             | Lock, copy 72 DWORDs from `BAR+0xC1A4` to `this+0x1C8`, unlock, parse IO names, call `_recomputeBufferFrameSize()`, `Callback(this, 0x65, 0, context)`                                                                   |
| 22   | Rate change                 | Read `BAR+0xC05C` and `BAR+0xC054`, `callback(0x67)`                                                                                                                                                                     |
| 4    | DMA ready                   | Read `BAR+0xC054/0xC058/0xC05C`, `callback(0x64)`                                                                                                                                                                        |
| 6    | Error                       | Log only                                                                                                                                                                                                                 |
| 21   | (second pass)               | Signal `notifyEvent` (`this+0x2830`) — **wakes `Connect()`**                                                                                                                                                             |
| 7    | End buffer                  | Signal `endBufferEvent` (`this+0x2840`)                                                                                                                                                                                  |
| 0\|1 | Combined IO ready           | `callback(0x6E)`                                                                                                                                                                                                         |

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
bufferFrameSize = floor_pow2(0x400000 / (max(play_ch, rec_ch) * 4))
```

Capped at `0x2000` (8192).

**Fields read:** `this+0x1D4` (record channels), `this+0x2F8` (playback
channels) **Field written:** `this+0x287C` (bufferFrameSize)

**For Apollo Solo** (42 play, 32 rec):

```
0x400000 / (42 * 4) = 24966 → floor_pow2 = 16384 → capped to 8192
```

## Transport Sequence

### PrepareTransport (from `pcm_prepare`)

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

### StartTransport (@ line 71904)

1. Check `transport_state == 1` (prepared)
2. Check `is_connected`
3. Lock `hw_lock`
4. Write `BAR+0x2248 = 0x00F` (normal) or `0x20F` (variant `0xA`/`0x9` extended)
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

| Vector ID | Decimal | Callback                 | Priority | Description                           |
| --------- | ------- | ------------------------ | -------- | ------------------------------------- |
| `0x28`    | 40      | `_notifyIntrCallback`    | 0        | Reads `BAR+0xC008`, dispatches events |
| `0x46`    | 70      | `_periodicTimerCallback` | —        | Periodic timer interrupt              |
| `0x47`    | 71      | `_endBufferCallback`     | 1        | End-of-buffer notification            |

### Vector Enable/Disable (`CPcieIntrManager`)

**EnableVector** (`vec`, `arm`) @ `0x13CD4` (line 12349):

1. `slot = IntrManager[(vec << 2) + 0x4D0]`
2. `bit_mask = 1 << slot`
3. `IntrManager+0x20 |= bit_mask`
4. If `arm`: write `bit_mask` to `BAR+0x2208` (arm DMA0)
5. Write full shadow to `BAR+0x2204`
6. If dual-DMA: write high bits to `BAR+0x2264/0x2268`

**DisableVector** (`vec`) @ `0x13F34` (line 12502):

1. `slot = IntrManager[(vec << 2) + 0x4D0]`
2. `bit_mask = 1 << slot`
3. `IntrManager+0x20 &= ~bit_mask`
4. Write shadow to `BAR+0x2204`
5. If dual-DMA: write to `BAR+0x2268`

### Vector Lifecycle

| Vector | Enabled                                                     | Disabled                 |
| ------ | ----------------------------------------------------------- | ------------------------ |
| `0x28` | `ProgramRegisters`                                          | `Shutdown`               |
| `0x46` | `PrepareTransport` (only if `periodic_timer_interval != 0`) | —                        |
| `0x47` | End of `PrepareTransport` (after DMA ready poll)            | Start of `StopTransport` |

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

| Offset   | Field                        | Description                      |
| -------- | ---------------------------- | -------------------------------- |
| `+0x00`  | lock object pointer          | PAC-authenticated vtable         |
| `+0x08`  | BAR base pointer             |                                  |
| `+0x18`  | `use_lock` flag              | `bool`                           |
| `+0x4BC` | `dma_ctrl_shadow`            | Shadow register for `BAR+0x2200` |
| `+0x4C0` | `has_second_engine` flag     |                                  |
| `+0x4C4` | `has_second_dma_engine` flag |                                  |

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

| Offset     | Field                                  | Description                                   |
| ---------- | -------------------------------------- | --------------------------------------------- |
| `+0x08`    | reference count / instance ID          |                                               |
| `+0x10`    | `hw_lock`                              | Spinlock (`vtable+0x10`=Lock, `+0x18`=Unlock) |
| `+0x18`    | `CPcieIntrManager*` pointer            |                                               |
| `+0x20`    | `is_connected` flag                    | `u32`                                         |
| `+0x24`    | `channel_base_index`                   | `u32`, = 10 for Apollo Solo                   |
| `+0x28`    | secondary field                        | = 380 (`0x17C`)                               |
| `+0xA1`    | `sample_rate_code`                     | `u8`                                          |
| `+0xA8`    | rate change value                      | From `BAR+0xC05C`                             |
| `+0x01C8`  | record IO descriptor copy area         | 72 DWORDs from `BAR+0xC1A4`                   |
| `+0x01D4`  | `record_channel_count`                 |                                               |
| `+0x02E8`  | playback IO descriptor copy area       | 72 DWORDs from `BAR+0xC2C4`                   |
| `+0x02F8`  | `playback_channel_count`               |                                               |
| `+0x2828`  | mutex                                  | `CUAOS::CreateMutex`                          |
| `+0x2830`  | `notifyEvent`                          | `CUAOS::CreateEvent`, auto-reset              |
| `+0x2838`  | `connectEvent`                         | `CUAOS::CreateEvent`, auto-reset              |
| `+0x2840`  | `endBufferEvent`                       | `CUAOS::CreateEvent`, auto-reset              |
| `+0x2848`  | BAR base pointer                       | Set in `MapHardware`                          |
| `+0x2858`  | notification callback function pointer |                                               |
| `+0x2860`  | notification callback context          |                                               |
| `+0x2868`  | end-buffer callback function pointer   |                                               |
| `+0x2870`  | end-buffer callback context            |                                               |
| `+0x2878`  | `transport_state`                      | 0=uninit, 1=prepared, 2=running               |
| `+0x287C`  | `bufferFrameSize`                      | Computed by `_recomputeBufferFrameSize`       |
| `+0x2880`  | `interrupt_period`                     | Frames                                        |
| `+0x2888`  | `hw_spinlock` #2                       |                                               |
| `+0x2890`  | notification spinlock                  | Guards `_handleNotificationInterrupt`         |
| `+0x2898`  | notification active flag               | Must be non-zero to process events            |
| `+0x28A0`  | DMA Buffer A pointer                   | Playback, 4 MB — set in `MapHardware`         |
| `+0x28A8`  | DMA Buffer B pointer                   | Capture, 4 MB — set in `MapHardware`          |
| `+0x28B0`  | `device_variant_id` / constant `0xC`   | Set in `Connect`                              |
| `+0x28B4`  | `current_sample_rate`                  | Enum                                          |
| `+0x28B8`  | `diagnostic_flags`                     | bit1,2 = monitor enable                       |
| `+0x28BC`  | `diagnostic_status`                    |                                               |
| `+0x28C0`  | `periodic_timer_interval`              | Frames                                        |
| `+0x28C8`  | firmware base address                  | 64-bit, from `BAR+0x30/0x34`                  |
| `+0x28D0`  | `UAD2DeviceType`                       |                                               |
| `+0x28D8`  | task/DMA tag                           | From `MapHardware` arg                        |
| `+0x28E0`  | flag: enable playback monitor read     |                                               |
| `+0x28E8`  | DMA mapper / IOMapper object           |                                               |
| `+0x2EF4`  | extended mode flag                     | For variant `0x9`                             |
| `+0x22EF0` | large-offset DMA address storage       |                                               |

## Driver Changelog

### v2026.301.0 (Current)

- Date-based versioning adopted
- Two complete rounds of code review with all critical/important issues fixed
- R1 (Critical): Added `snd_card_disconnect()` early in `uad2_remove()` to
  prevent ALSA ops racing with teardown
- R2 (Critical): Added `disconnecting` flag + `READ_ONCE` guard in
  `uad2_read32`/`uad2_write32` (MMIO becomes no-op on removal). IRQ handler
  checks for `0xFFFFFFFF` sentinel (hot-unplug detection)
- R3/I4 (Critical): `uad2_clock_source_put()` rejects changes with `-EBUSY` when
  transport is running
- R4: `uad2_start_transport()` validates `transport_state == 1` and `connected`
  before writing hardware
- R5: `uad2_pcm_prepare()` skips `uad2_set_sample_rate()` if rate unchanged
- R6: DMA-ready poll reduced from 200 iterations to 3 (matching kext's ~2
  retries)
- R12: Removed unnecessary `{}` block scope for firmware base address read
- R14: `period_bytes_min` increased from 256 to 4096
- R17: PCI ID table sentinel `{ 0 }` → `{ }`

### v0.6.0

- Removed redundant `.ioctl = snd_pcm_lib_ioctl` from `snd_pcm_ops`
- Added `sync_stop` callback: `uad2_pcm_sync_stop()` calls `synchronize_irq()`
  to drain in-flight IRQ handlers before stream close (prevents UAF)
- Fixed IRQ handler race: `playback_ss`/`capture_ss` now protected by
  `dev->lock` spinlock in open/close, snapshotted under lock in
  `uad2_irq_handler()`
- Debug instrumentation: upgraded `dev_dbg` → `dev_info` for first-boot
  debugging (notification handler status, connect ack, channel config, channel
  counts, ring capacity, DSP ring base, DMA ready poll, transport prepare
  summary, per-channel connect outcome)
- Channel mapping decode and logging: added `uad2_channel_type_names[]` table
  (15 entries), `IO_DESC_CHAN_OFFSET` constant, `uad2_log_channel_map()`
  function that reads uint16 descriptors from firmware and logs decoded
  type/sub-index per channel. Called after successful `uad2_audio_connect()` for
  both playback and record directions
- Fixed use-after-free in `uad2_remove()`: reordered `free_irq()` before
  `snd_card_free()`, freed DMA buffers while dev is still alive
- Fixed race conditions: `transport_state` and `connected` flag now use
  `READ_ONCE`/`WRITE_ONCE` for proper data-race annotation; `transport_state`
  writes moved under `dev->lock` where possible
- Fixed race on `play_channels`/`rec_channels`: writes in notification handler
  now use `WRITE_ONCE`, reads in `pcm_prepare` use `READ_ONCE`
- Interrupt vector enable/disable now self-locking: `uad2_enable_vector()` and
  `uad2_disable_vector()` take `dev->lock` internally;
  `__uad2_enable_vector_locked()`/`__uad2_disable_vector_locked()` for callers
  already holding the lock
- Fixed `pcm_prepare` clobbering device-global `buffer_frames`: now uses local
  variables instead of writing back to `dev->buffer_frames`
- Fixed ring capacity clamp logic: values > `0x400` now capped to `0x400`
  instead of incorrectly clamped to 0
- Added `uad2_shutdown()` call in probe error path after partial `hw_program` to
  clean up partially-initialized hardware state
- Fixed pointer format leak: `%px` → BAR-relative offsets in DSP ring log,
  removed virtual address logging from SG buffer allocation
- Removed redundant `memset` after `dma_alloc_coherent` (returns zeroed memory)
- Reordered `pci_set_master()` after `dma_set_mask_and_coherent()` in probe
- `MODULE_LICENSE` changed from `"GPL v2"` to canonical `"GPL"`

### v0.5.0

- Renamed from `apollo_solo` → `uad2` throughout (generic for all UA Thunderbolt
  devices)
- PCI ID table extensible for multiple devices (Apollo Twin, x4, x6, etc.)

### v0.4.0

- Proper interrupt vector management: `uad2_enable_vector()` /
  `uad2_disable_vector()` with 64-bit enable shadow bitmask (mirrors
  `IntrManager+0x20`)
- Vector lifecycle: `0x28` enabled in `ProgramRegisters` / disabled in
  `Shutdown`, `0x46` enabled in `PrepareTransport` if periodic timer configured,
  `0x47` enabled after DMA ready poll / disabled at start of `StopTransport`
- Full Shutdown sequence (`uad2_shutdown`): stops transport, clears doorbell,
  disables notification vector, disconnects — replaces ad-hoc teardown in
  `remove()`
- Transport state tracking (0=uninit, 1=prepared, 2=running)
- `notify_lock` spinlock protecting notification handler (mirrors kext
  `this+0x2890`)
- Locking in `PrepareTransport`/`StartTransport`/`StopTransport` (`hw_lock`
  under spinlock)
- ALSA mixer controls: Clock Source (Internal/S/PDIF) and Current Sample Rate
  (read-only)
- Clock source configurable via mixer; applied in `_setSampleClock` register

### v0.3.0

- Full register map including SG tables (`0x8000–0xBFFF`), firmware base
  (`0x30/0x34`), notification status (`0xC008`), and all firmware mailbox
  registers
- Two 4 MB DMA buffer allocation (scatter-gather source for playback + capture)
- SG table programming: 1024-entry loop writing 64-bit phys addrs to
  `BAR+0x8000/0xA000`
- Firmware base address read from `BAR+0x30/0x34`
- Global interrupt enable (`BAR+0x0014 = 0xFFFFFFFF`)
- Full Connect handshake with proper doorbell address, `notifyEvent` via Linux
  completion, manual notification polling on timeout
- Notification interrupt handler with full event dispatch
- Dynamic buffer frame size computation: `floor_pow2(4MB / (max_ch * 4))`, cap
  8192
- ALSA PCM buffer mapped directly to 4 MB HW DMA buffer (zero-copy)
- `PrepareTransport` with DMA ready poll (`BAR+0x2254`)
- Playback monitor config (`BAR+0x224C`)
- `channel_base_index = 10` (confirmed from binary data segment @ `0x6018`)
- Transport start/stop (`0xF` / `0x0` to `BAR+0x2248`)
- Proper `remove()` with disconnect (`0x10`), secondary counter clear, interrupt
  disable
- Corrected format: S32_LE, 42 output / 32 input channels

## Known Limitations

- **Single MSI vector mode** — hardware has 3 vectors; all currently handled on
  vector 0
- **Channel count readback** from IO descriptors uses fixed offsets (may need
  tuning)
- **No ALSA mixer controls** for monitor routing (clock source selection IS
  implemented)
- **No firmware version verification**
- **No power management** / suspend-resume support
- **No PCIe error handlers** (`pci_error_handlers`)
- **No Thunderbolt hot-unplug safety** — device may disappear mid-DMA (partial
  mitigation via `disconnecting` flag and `0xFFFFFFFF` sentinel check)
- **Channel mapping predicted but NOT verified** on live hardware
- **`uad2_audio_connect()` doesn't fail** on individual channel timeouts
  (deferred — needs hardware)
- **Excessive `dev_info` logging** in notification handler (intentionally left
  for first-boot debugging)

## Testing Plan

### Priority Order

#### 1. Build and Fix Compilation Errors

- Run `make` against the running kernel
- Fix any real compilation errors (not the clang/GCC LSP false positives)
- The driver used `snd_pcm_lib_ioctl` which was removed in kernel 5.18+; if
  building against >= 5.18, remove the `.ioctl` line from `snd_pcm_ops`
- Verify all includes resolve and types are correct

#### 2. Channel Mapping (Critical for Audio Output)

The driver programs `REG_PLAYBACK_CHAN_CNT` with `dev->play_channels` (42) and
`REG_RECORD_CHAN_CNT` with `dev->rec_channels` (32), meaning the DMA buffer is
always 42-channel interleaved for playback.

**Problems:**

- Most apps want stereo. Writing 2ch to a 42ch interleaved buffer means audio
  lands in channels 0–1, but we don't know if those map to the monitor outputs
- The driver currently does NOT adjust channel counts based on ALSA
  `runtime->channels`

**Fix options:**

1. Decode the channel-to-output mapping from the kext/ioreg data (the macOS
   `IOAudioStream` objects have `StartingChannelID` fields)
2. As a first test, write a 42ch S32_LE buffer with audio in different channel
   pairs to discover which pair produces output
3. Test whether the hardware can operate with fewer channels (write a smaller
   count to `REG_PLAYBACK_CHAN_CNT`)

#### 3. Interrupt Delivery Verification

- Load the module and check `/proc/interrupts` for the MSI vector
- Verify that the notification handler fires during Connect
- If Connect times out on all 20 channels, the interrupt path is broken
- Fallback: the manual polling in the Connect loop should still work

#### 4. DMA Buffer Sanity

- After probe, read back SG table entries from `BAR+0x8000` to verify they match
  allocated DMA addresses
- Read `BAR+0x2244` (position counter) — should be 0 before transport starts
- After starting playback, verify position counter advances

#### 5. Runtime Testing Sequence

```sh
# a) Load module
sudo insmod uad2.ko
# Check dmesg for probe success
dmesg | tail -50

# b) Verify card appears
cat /proc/asound/cards

# c) Verify PCM device
aplay -l

# d) DMA smoke test (silence, verify no kernel panics)
aplay -D hw:X,0 -f S32_LE -c 42 -r 48000 /dev/zero

# e) Generate test tone on specific channel pairs to find monitor outputs
# f) Test with real audio via plughw
```

#### 6. Hardening (After Basic Audio Works)

- Add error recovery in Connect (currently continues on timeout)
- Add PCIe error handling (`pci_error_handlers`)
- Consider Thunderbolt hot-unplug safety
- Test suspend/resume if relevant

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
| `_notifyIntrCallback`                               | (schedules deferred) | —           | Done (v0.4.0) |
| `_endBufferCallback`                                | —                    | —           | Done (v0.4.0) |
| `_periodicTimerCallback`                            | —                    | —           | Done (v0.4.0) |
| `_requestCallback`                                  | `0x4D6F8`            | —           | Done (v0.4.0) |

### Potential Future Analysis

- `CDeviceManager::_setSampleClock` @ line 40218 (higher-level clock
  orchestration)
- Multi-vector MSI setup (register multiple MSI vectors for `0x28`, `0x46`,
  `0x47`)
- Monitor mixer routing (`BAR+0x224C` and related registers)
- Power management / Thunderbolt hot-plug handling

## Project Files

| File                                | Description                                                                                                                                 |
| ----------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- |
| `uad2.c`                            | Main driver source (~2149 lines) — PCI probe/remove, ALSA PCM ops, interrupt handling, DMA, firmware connect, transport, mixer, channel map |
| `Makefile`                          | Standard out-of-tree kernel module Makefile (uses `KDIR` from env)                                                                          |
| `Kbuild`                            | `obj-m := uad2.o`                                                                                                                           |
| `flake.nix`                         | Nix flake providing build environment                                                                                                       |
| `.clang-format`                     | Linux kernel coding style                                                                                                                   |
| `compile_commands.json`             | LSP compilation database (generated via `bear -- make`)                                                                                     |
| `bin/aarch64-darwin/uad2.kext`      | arm64e kext binary (primary RE source)                                                                                                      |
| `bin/aarch64-darwin/uad2.kext.dump` | arm64e disassembly                                                                                                                          |
| `bin/x86_64-darwin/uad2.kext`       | x86_64 kext binary (stripped, unused)                                                                                                       |
| `bin/x86_64-darwin/uad2.kext.dump`  | x86_64 disassembly (unused)                                                                                                                 |

## License

This driver is licensed under **GPL** (`MODULE_LICENSE("GPL")`).
