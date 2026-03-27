/*
 * UAD2 - Universal Audio Thunderbolt PCIe Audio Driver
 *
 * Reverse engineered from com.uaudio.driver.UAD2System (macOS kext)
 * arm64e binary, version 11.7.0 build 2 (Apr 24 2025)
 *
 * Register map derived from:
 *   CPcieDevice::ProgramRegisters           @ 0xdf48
 *   CPcieIntrManager::ProgramRegisters      @ 0x133f8
 *   CPcieIntrManager::ResetDMAEngines       @ 0x134b8
 *   CPcieIntrManager::EnableDMA             @ 0x13a4c
 *   CPcieDSP::ProgramRegisters              @ 0x1124c
 *   CPcieDSP::_waitFor469ToStart            @ 0x1152c
 *   CPcieRingBuffer::ProgramRegisters       @ 0x14c48
 *   CPcieAudioExtension::MapHardware        @ 0x4ba0c  (line 70355)
 *   CPcieAudioExtension::ProgramRegisters   @ 0x4bac0  (line 70401)
 *   CPcieAudioExtension::Connect            @ 0x4be58  (line 70632)
 *   CPcieAudioExtension::_handleNotificationInterrupt @ 0x4c154 (line 70824)
 *   CPcieAudioExtension::PrepareTransport   @ (line 71623)
 *   CPcieAudioExtension::StartTransport     @ (line 71904)
 *   CPcieAudioExtension::StopTransport      @ (line 71987)
 *   CPcieAudioExtension::TransportPosition  @ (line 72057)
 *   CPcieAudioExtension::_setSampleClock    @ (line 72712)
 *   CPcieAudioExtension::_recomputeBufferFrameSize @ 0x4d9e0 (line 72414)
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/hrtimer.h>
#include <linux/hrtimer_types.h>
#include <linux/log2.h>
#include <linux/string.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/initval.h>

/* ============================================================
 * PCI identity
 * All Apollo Thunderbolt devices share vendor 0x1A00, device 0x0002.
 * Device model is determined by serial prefix (BAR0+0x20..0x2C),
 * not by PCI subsystem ID.
 * ============================================================ */
#define UA_VENDOR_ID 0x1a00
#define UA_DEVICE_ID 0x0002

/* ============================================================
 * Device type IDs (from open-apollo ua_apollo.h, reconstructed
 * from kext CPcieDevice::Name() and _deviceTypeFromSerialNumber())
 *
 * v2 devices: type from ext_caps register (0x2234) bits[25:20]-1,
 *             then refined via serial number prefix lookup.
 * ============================================================ */
#define UA_DEV_APOLLO_X6 0x1E
#define UA_DEV_APOLLO_X4 0x1F
#define UA_DEV_APOLLO_X8P 0x20
#define UA_DEV_APOLLO_X16 0x21
#define UA_DEV_APOLLO_X8 0x22
#define UA_DEV_APOLLO_TWIN_X 0x23
#define UA_DEV_APOLLO_SOLO 0x27
#define UA_DEV_ARROW 0x28
#define UA_DEV_APOLLO_X16D 0x2A
#define UA_DEV_APOLLO_X6_GEN2 0x35
#define UA_DEV_APOLLO_X4_GEN2 0x36
#define UA_DEV_APOLLO_X8_GEN2 0x37
#define UA_DEV_APOLLO_X8P_GEN2 0x38
#define UA_DEV_APOLLO_X16_GEN2 0x39
#define UA_DEV_APOLLO_TWIN_X_GEN2 0x3A

/* Serial prefix register: BAR0+0x20..0x2C, 16-byte ASCII string.
 * First 4 digits (at offset +4 into the string) identify the model.
 * NOTE: serial prefix detection is unreliable on some models (e.g.
 * Apollo Solo serial coincidentally contains "2032" → false x16D match).
 * PCI subsystem ID is used as primary detection where known. */
#define UA_REG_SERIAL_BASE 0x0020
#define UA_REG_SERIAL_LEN 16

/* PCI subsystem IDs for known models.
 * These are constant per model and more reliable than serial prefix.
 * Only models confirmed on real hardware or from open-apollo are listed. */
#define UA_SUBSYS_SOLO 0x000F /* Apollo Solo (confirmed via ioreg) */
#define UA_SUBSYS_X4_QUAD 0x0011 /* Apollo x4 Quad (from open-apollo) */

/* FPGA revision register — bit 31 distinguishes v1/v2 firmware */
#define REG_FPGA_REV 0x2218
/* Extended capabilities (v2 firmware): bits[25:20] = device family,
 * bits[15:8] = DSP count */
#define REG_EXT_CAPS 0x2234

/* ============================================================
 * BAR register offsets  (all 32-bit MMIO, BAR 0 = 64 KB window)
 * ============================================================ */

/* --- Global interrupt control --- */
#define REG_INTR_ENABLE 0x0014 /* WRITE 0xFFFFFFFF to unmask */

/* --- Firmware base address (read after SG table programming) --- */
#define REG_FW_BASE_LO 0x0030 /* R: firmware base low 32 bits */
#define REG_FW_BASE_HI 0x0034 /* R: firmware base high 32 bits */

/* --- DMA master control (CPcieIntrManager) --- */
#define REG_DMA_MASTER_CTRL \
	0x2200 /* R/W bitmask: per-DSP channel enable
                                         * EnableDMA sets bit (1 << (dsp_index+1))
                                         * bit 0 reserved (global master?)
                                         * Uses software shadow, never read from HW */
#define DMA_CTRL_MASTER_ENABLE 0x0001 /* bit 0: global master enable */
#define DMA_RESET_SINGLE 0x1e00 /* bits[12:9]: single engine reset */
#define DMA_RESET_DUAL 0x1fe00 /* bits[16:9]: dual engine reset */
/* Masks to clear DMA channel enable bits (preserving bit 0): */
#define DMA_SHADOW_MASK_SINGLE 0xffffffe1 /* clears bits[4:1] */
#define DMA_SHADOW_MASK_DUAL 0xfffffe01 /* clears bits[8:1] */

/* --- DMA engine 0 (CPcieIntrManager::ProgramRegisters) --- */
#define REG_DMA0_INTR_CTRL 0x2204 /* WRITE 0x0 to disable/clear */
#define REG_DMA0_STATUS 0x2208 /* WRITE 0xFFFFFFFF to arm; WRITE 0x0 to clear */

/* --- Device identification (CPcieDevice::ProgramRegisters) --- */
#define REG_DEVICE_ID \
	0x2218 /* READ to verify device identity
                                         * Also polled after clock change (2s timeout) */
#define REG_DEVICE_HANDSHAKE \
	0x2220 /* WRITE 0x0 after reading device ID
                                         * CPcieDevice::WriteRegEjj always writes 0 here */

/* --- Audio transport registers (CPcieAudioExtension) --- */
#define REG_BUFFER_FRAME_SIZE 0x2240 /* W: bufferFrameSize - 1 */
#define REG_DMA_POSITION \
	0x2244 /* R: current frame position (wrapping counter)
                                         * W: 0 to clear */
#define REG_TRANSPORT_CTL \
	0x2248 /* R/W: transport state machine
                                         * 0x000 = stop
                                         * 0x001 = armed/prepared
                                         * 0x00F = running (normal)
                                         * 0x20F = running (extended, variant 0xA/0x9)
                                         * Read status: bit5=running, bit7=overflow,
                                         *              bit8=underflow */
#define REG_PLAYBACK_MON_CFG \
	0x224C /* W: (totalPlayChans-1) | 0x100 (enable+mask) */
#define REG_PLAYBACK_CHAN_CNT 0x2250 /* W: total playback channels */
#define REG_POLL_STATUS \
	0x2254 /* R: poll for DMA ready (compare vs irq_period) */
#define REG_IRQ_PERIOD 0x2258 /* W: interrupt period in frames */
#define REG_RECORD_CHAN_CNT 0x225C /* W: record channel count */
#define REG_STREAM_ENABLE \
	0x2260 /* W: 0x1 = start stream, 0x10 = stop stream
                                         * Also: 0x4 = clock change trigger */

/* --- DMA engine 1 (dual-engine devices only) ---
 * Note: relative to DMA0, the roles of +0 and +4 are swapped:
 *   0x2264 = arm/status (like DMA0's 0x2208)
 *   0x2268 = enable ctrl (like DMA0's 0x2204)
 */
#define REG_DMA1_STATUS 0x2264 /* WRITE bitmask to arm; read pending */
#define REG_DMA1_INTR_CTRL 0x2268 /* WRITE enable shadow */

/* --- Buffer and timer configuration --- */
#define REG_BUFFER_SIZE_KB 0x226C /* W: (totalPlayChans * (bufSz-1)) >> 10 */
#define REG_PERIODIC_TIMER 0x2270 /* W: periodic timer interval in frames */

/* --- Playback monitor status --- */
#define REG_PLAYBACK_MON_STAT 0x22C0 /* R: playback monitor status */
#define REG_SECONDARY_CTL 0x22C4 /* W: 0 on Shutdown */

/* --- DSP Mixer registers (BAR0 + 0x3800 window) ---
 *
 * The mixer engine uses a sequence-counter handshake to update settings:
 *   1. Read MIXER_SEQ_RD (wait for DSP idle: RD == WR)
 *   2. Write paired (value, mask) words to MIXER_SETTING[index]
 *   3. Increment and write MIXER_SEQ_WR
 *   4. DSP reads all 38 settings atomically per SEQ_WR advance
 *
 * 38 settings × 8 bytes each (two 32-bit words per setting).
 * From open-apollo ua_apollo.h / ua_core.c.
 * ============================================================ */
#define REG_MIXER_BASE 0x3800
#define REG_MIXER_SEQ_WR 0x3808 /* Mixer sequence counter (host → DSP) */
#define REG_MIXER_SEQ_RD 0x380C /* Mixer sequence counter (DSP readback) */
#define REG_MIXER_RB_STATUS 0x3810 /* Readback: 1=ready, write 0 to re-arm */
#define REG_MIXER_RB_DATA 0x3814 /* Readback: 40 consecutive u32 words */
#define MIXER_SETTING_STRIDE 8 /* 2 × u32 per setting */
#define MIXER_BATCH_COUNT 38 /* Settings flushed per batch */
#define MIXER_RB_WORDS 40 /* Readback data words */
#define DSP_SERVICE_INTERVAL_MS 10 /* readback drain + mixer flush rate */

/* --- Scatter-Gather DMA tables (ProgramRegisters @ 0x4bac0) ---
 *
 * Buffer A (playback): BAR+0x8000..0x9FFF = 1024 entries × 8 bytes
 *   Each entry: [low32 phys_addr] [high32 phys_addr] for one 4KB page
 *
 * Buffer B (capture):  BAR+0xA000..0xBFFF = same layout
 *   Computed as BufferA_SG_offset + 0x2000
 *
 * Loop: sg_offset = 0x8000; sg_offset += 8; until sg_offset == 0xA000
 *       dma_offset = 0; dma_offset += 0x1000
 * ============================================================ */
#define REG_SG_TABLE_A_BASE 0x8000 /* Playback SG table start */
#define REG_SG_TABLE_A_END 0xA000 /* Playback SG table end (exclusive) */
#define REG_SG_TABLE_B_OFFSET 0x2000 /* Capture SG table = A + 0x2000 */
#define SG_ENTRY_SIZE 8 /* bytes per SG entry */
#define SG_NUM_ENTRIES 1024 /* entries per table */
#define SG_PAGE_SIZE 0x1000 /* 4KB per page */
#define SG_BUFFER_SIZE 0x400000 /* 4MB total DMA buffer */

/* --- Sample clock (CPcieAudioExtension::_setSampleClock) --- */
#define REG_SAMPLE_CLOCK \
	0xC04C /* W: clock_source | (rate_enum << 8)
                                         * Actual offset: BAR + (dev_idx*4) + 0xC04C
                                         * For Apollo Solo (dev_idx from this+0x24) */

/* --- Firmware mailbox (notification/doorbell) --- */
#define REG_FW_DOORBELL \
	0xC004 /* W: 0x0ACEFACE as connect command
                                         * Addr: BAR + (channel_base_index << 2) + 0xC004 */
#define REG_FW_NOTIFY_STATUS \
	0xC008 /* R/W: notification bitmask register
                                         * Addr: BAR + (channel_base_index << 2) + 0xC008
                                         * Write 0 to acknowledge/clear */
#define DMA_DESC_MAGIC 0x0aceface

/* --- Bank-shifted notification registers (add channel_base_index<<2) ---
 * From open-apollo ua_apollo.h.  These contain DSP state info that
 * the firmware expects the host to read after connect and on events. */
#define REG_NOTIF_RATE_INFO 0xC054 /* rate info readback */
#define REG_NOTIF_XPORT_INFO 0xC058 /* transport info readback */
#define REG_NOTIF_CLOCK_INFO 0xC05C /* clock source info readback */

/* --- Firmware notification status bits (BAR+0xC008 bitmask) --- */
#define NOTIFY_PLAYBACK_IO BIT(0) /* Playback IO descriptors ready */
#define NOTIFY_RECORD_IO BIT(1) /* Record IO descriptors ready */
#define NOTIFY_DMA_READY BIT(4) /* DMA engine ready */
#define NOTIFY_CONNECT_ACK BIT(5) /* Connect handshake acknowledged */
#define NOTIFY_ERROR BIT(6) /* Firmware error */
#define NOTIFY_END_BUFFER BIT(7) /* End of buffer */
#define NOTIFY_CHAN_CONFIG BIT(21) /* Channel configuration update */
#define NOTIFY_RATE_CHANGE BIT(22) /* Sample rate change ack */

/* --- Playback/Record IO descriptor areas --- */
#define REG_RECORD_IO_DESC 0xC1A4 /* R: record IO descriptors (72 DWORDs) */
#define REG_PLAYBACK_IO_DESC 0xC2C4 /* R: playback IO descriptors (72 DWORDs) */
#define IO_DESC_DWORDS 72 /* 0x120 bytes = 288 bytes */

/* --- Channel config area --- */
#define REG_CHAN_CONFIG_BASE 0xC000 /* R: 10 DWORDs of channel config */
#define CHAN_CONFIG_DWORDS 10

/* IO descriptor channel type offset: uint16 descriptors start at +0x18
 * within the IO descriptor block (after header fields).
 * Each uint16: high byte = type index, low byte = sub-index */

/* ============================================================
 * DSP ring buffer registers (relative to ring_base)
 *
 * For DSP index n:
 *   n < 4:  ring_base = BAR + 0x2000 + (n * 0x80)
 *   n >= 4: ring_base = BAR + 0x5e00 + (n * 0x80)
 * Second ring at ring_base + 0x40
 *
 * CPcieRingBuffer::ProgramRegisters @ 0x14c48:
 *   4 descriptor slots at ring_base+0x00..0x1C (8 bytes each)
 *   Each slot: low 32-bit phys addr, high 32-bit phys addr
 *   Pages: 0x0000, 0x1000, 0x2000, 0x3000 (4 x 4KB pages)
 *   All written and verified via read-back
 * ============================================================ */
#define DSP_RING_BASE_LOW 0x2000
#define DSP_RING_BASE_HIGH 0x5e00
#define DSP_RING_STRIDE 0x80
#define DSP_RING2_OFFSET 0x40

/* Ring buffer register offsets (relative to ring_base) */
#define DSP_RING_DESC0_LO 0x00 /* W/R: descriptor 0 phys addr low 32 */
#define DSP_RING_DESC0_HI 0x04 /* W/R: descriptor 0 phys addr high 32 */
#define DSP_RING_DESC1_LO 0x08 /* descriptor 1 */
#define DSP_RING_DESC1_HI 0x0C
#define DSP_RING_DESC2_LO 0x10 /* descriptor 2 */
#define DSP_RING_DESC2_HI 0x14
#define DSP_RING_DESC3_LO 0x18 /* descriptor 3 */
#define DSP_RING_DESC3_HI 0x1C
#define DSP_RING_DESC_COUNT 0x20 /* W: ring descriptor count = ring_size */
#define DSP_RING_SIZE_REG 0x24 /* W: ring size register = ring_size */
#define DSP_RING_CAPACITY 0x28 /* R: hardware ring capacity (cap at 0x400) */
#define DSP_READY_POLL_OFF 0x1a4 /* R: DSP ready poll; wait for DSP_READY_SIG */

#define DSP_RING_DESC_SLOTS 4 /* 4 descriptor slots per ring */
#define DSP_RING_PAGE_SIZE 0x1000 /* 4 KB per descriptor page */
#define DSP_READY_SIG 0xa8caed0f
#define DSP_POLL_INTERVAL_MS 300
#define DSP_POLL_MAX_PRIMARY 100 /* DSP type 0: 30s timeout */
#define DSP_POLL_MAX_SECONDARY 10 /* others: 3s timeout */

/* ============================================================
 * UAD2SampleRate enum (1-based, from _setSampleClock analysis)
 * ============================================================ */
#define UA_RATE_44100 1
#define UA_RATE_48000 2
#define UA_RATE_88200 3
#define UA_RATE_96000 4
#define UA_RATE_176400 5
#define UA_RATE_192000 6

/* ============================================================
 * Audio engine parameters
 * ============================================================ */
#define UAD2_MAX_BUFFER_FRAMES 8192 /* max from _recomputeBufferFrameSize cap */
#define UAD2_SAMPLE_OFFSET 16
#define UAD2_BYTES_PER_SAMPLE 4 /* 24-bit in 32-bit container */
#define UAD2_MAX_DSPS 8
#define UAD2_MAX_CHANNELS 64

/* channel_base_index = bank_shift for notification register addressing.
 * All Apollo Thunderbolt devices use 0x0A (verified on Solo, x4). */
#define UAD2_CHANNEL_BASE_IDX 10

/* ============================================================
 * Serial prefix → device type lookup table
 * From open-apollo ua_core.c (extracted from kext
 * _deviceTypeFromSerialNumber() data at offset 0x3E840)
 * ============================================================ */
struct ua_serial_entry {
	char prefix[5]; /* 4-char ASCII serial prefix + NUL */
	u32 device_type;
};

static const struct ua_serial_entry ua_serial_table[] = {
	{ "2005", UA_DEV_APOLLO_X4 },
	{ "2016", UA_DEV_APOLLO_X6 },
	{ "2017", UA_DEV_APOLLO_X8 },
	{ "2018", UA_DEV_APOLLO_X16 },
	{ "2019", UA_DEV_APOLLO_X8P },
	{ "2020", UA_DEV_APOLLO_TWIN_X },
	{ "2024", UA_DEV_APOLLO_SOLO },
	{ "2025", UA_DEV_ARROW },
	{ "2032", UA_DEV_APOLLO_X16D },
	{ "2073", UA_DEV_APOLLO_X6_GEN2 },
	{ "2082", UA_DEV_APOLLO_X6_GEN2 },
	{ "2086", UA_DEV_APOLLO_X8_GEN2 },
	{ "2087", UA_DEV_APOLLO_X8P_GEN2 },
	{ "2088", UA_DEV_APOLLO_X16_GEN2 },
	{ "2089", UA_DEV_APOLLO_TWIN_X_GEN2 },
	{ "2092", UA_DEV_APOLLO_X4_GEN2 },
};

/* ============================================================
 * Per-model channel counts and capabilities
 * From open-apollo ua_audio.c (macOS IOKit IOAudioEngine properties)
 *
 * These are PCIe DMA channel counts, not physical I/O count.
 * ============================================================ */
struct ua_model_info {
	u32 device_type;
	unsigned int play_ch;
	unsigned int rec_ch;
	unsigned int num_preamps;
	unsigned int num_hiz;
};

static const struct ua_model_info ua_models[] = {
	/* Solo/Arrow: open-apollo has 3/2 but those are physical I/O counts.
	 * DMA channel counts from macOS ioreg: play=42 rec=32 for Solo.
	 * Arrow likely same (same chipset). Firmware IO descriptors will
	 * override these after connect if different. */
	{ UA_DEV_APOLLO_SOLO, 42, 32, 1, 0 },
	{ UA_DEV_ARROW, 42, 32, 1, 0 },
	{ UA_DEV_APOLLO_TWIN_X, 8, 8, 2, 2 },
	{ UA_DEV_APOLLO_TWIN_X_GEN2, 8, 8, 2, 2 },
	{ UA_DEV_APOLLO_X4, 24, 22, 4, 2 },
	{ UA_DEV_APOLLO_X4_GEN2, 24, 22, 4, 2 },
	{ UA_DEV_APOLLO_X6, 24, 22, 4, 2 },
	{ UA_DEV_APOLLO_X6_GEN2, 24, 22, 4, 2 },
	{ UA_DEV_APOLLO_X8, 26, 26, 4, 2 },
	{ UA_DEV_APOLLO_X8_GEN2, 26, 26, 4, 2 },
	{ UA_DEV_APOLLO_X8P, 26, 26, 8, 2 },
	{ UA_DEV_APOLLO_X8P_GEN2, 26, 26, 8, 2 },
	{ UA_DEV_APOLLO_X16, 34, 34, 8, 2 },
	{ UA_DEV_APOLLO_X16_GEN2, 34, 34, 8, 2 },
	{ UA_DEV_APOLLO_X16D, 34, 34, 0, 0 },
};

static const char *ua_device_name(u32 device_type)
{
	switch (device_type) {
	case UA_DEV_APOLLO_X4:
		return "Apollo x4";
	case UA_DEV_APOLLO_X6:
		return "Apollo x6";
	case UA_DEV_APOLLO_X8:
		return "Apollo x8";
	case UA_DEV_APOLLO_X8P:
		return "Apollo x8p";
	case UA_DEV_APOLLO_X16:
		return "Apollo x16";
	case UA_DEV_APOLLO_X16D:
		return "Apollo x16D";
	case UA_DEV_APOLLO_TWIN_X:
		return "Apollo Twin X";
	case UA_DEV_APOLLO_SOLO:
		return "Apollo Solo";
	case UA_DEV_ARROW:
		return "Arrow";
	case UA_DEV_APOLLO_X4_GEN2:
		return "Apollo x4 Gen 2";
	case UA_DEV_APOLLO_X6_GEN2:
		return "Apollo x6 Gen 2";
	case UA_DEV_APOLLO_X8_GEN2:
		return "Apollo x8 Gen 2";
	case UA_DEV_APOLLO_X8P_GEN2:
		return "Apollo x8p Gen 2";
	case UA_DEV_APOLLO_X16_GEN2:
		return "Apollo x16 Gen 2";
	case UA_DEV_APOLLO_TWIN_X_GEN2:
		return "Apollo Twin X Gen 2";
	default:
		return "Unknown Apollo";
	}
}

/* Connect loop: 20 channels with 10 retries each */
#define UAD2_CONNECT_CHANNELS 20
#define UAD2_CONNECT_RETRIES 10
#define UAD2_CONNECT_WAIT_MS 100

/* IRQ period lookup table from kext data segment @ 0x6020.
 * _setSampleClock loads this value into this+0x2880, which
 * PrepareTransport then writes to REG_IRQ_PERIOD (0x2258).
 * The firmware uses this to determine the DMA notification interval. */
static unsigned int uad2_irq_period_for_rate(unsigned int rate)
{
	switch (rate) {
	case 44100:
	case 48000:
		return 8;
	case 88200:
	case 96000:
		return 16;
	case 176400:
	case 192000:
		return 32;
	default:
		return 8; /* safe fallback: base rate value */
	}
}

/* Periodic timer interval lookup table from kext CUAD2AudioMixer::StartTransport.
 *
 * This determines the interrupt rate for audio processing.  The kext's mixer
 * calls SetPeriodicTimerIntervalInFrames(N-1, 0) where N comes from a rate-
 * indexed lookup table at kext data segment @ 0x5D20:
 *   44100/48000   → 256 frames → value 255
 *   88200/96000   → 512 frames → value 511
 *   176400/192000 → 1024 frames → value 1023
 *
 * This value is written to BAR+0x2270 (REG_PERIODIC_TIMER) and triggers
 * interrupt vector 0x46 (_periodicTimerCallback), which is the ACTUAL audio
 * clock that drives the kext's ProcessAudio and period advancement.
 *
 * NOTE: Vector 0x47 (end-of-buffer, _endBufferCallback) passes callback ID
 * 0x6c to the mixer, which ignores it.  Only vector 0x46 (periodic timer,
 * callback ID 0x6d) triggers ProcessAudio.  The Linux driver must use the
 * periodic timer interrupt to drive snd_pcm_period_elapsed(). */
static unsigned int uad2_periodic_timer_for_rate(unsigned int rate)
{
	switch (rate) {
	case 44100:
	case 48000:
		return 255; /* 256 - 1 */
	case 88200:
	case 96000:
		return 511; /* 512 - 1 */
	case 176400:
	case 192000:
		return 1023; /* 1024 - 1 */
	default:
		return 255;
	}
}

/* ============================================================
 * Interrupt vector enable bitmask management
 *
 * CPcieIntrManager::EnableVector @ 0x13cd4 (line 12349):
 *   1. Lookup: slot = IntrManager[(vector_id << 2) + 0x4D0]
 *   2. bit_mask = 1 << slot
 *   3. IntrManager+0x20 |= bit_mask (enable shadow, 64-bit)
 *   4. If arm_flag: write bit_mask to BAR+0x2208 (arm DMA0)
 *   5. Write full enable shadow to BAR+0x2204 (DMA0 interrupt ctrl)
 *   6. If dual-DMA: write to BAR+0x2264/0x2268
 *
 * CPcieIntrManager::DisableVector @ 0x13f34 (line 12502):
 *   1. slot = IntrManager[(vector_id << 2) + 0x4D0]
 *   2. bit_mask = 1 << slot
 *   3. IntrManager+0x20 &= ~bit_mask
 *   4. Write new enable shadow to BAR+0x2204
 *   5. If dual-DMA: write to BAR+0x2268
 *
 * Vector assignments (from CPcieDSP::Initialize @ 0x10d60):
 *   base = dsp_index * 5  (for Apollo Solo, dsp_index=0 → base=0)
 *   vec+0 = ring0 callback    (not enabled by AudioExtension)
 *   vec+1 = ring1 callback    (not enabled by AudioExtension)
 *   vec+2 = error callback    (enabled immediately)
 *   vec+3 = error callback    (enabled immediately)
 *   vec+4 = error callback    (enabled immediately)
 *
 * CPcieAudioExtension registers (from ProgramRegisters/Initialize):
 *   0x28 = notification interrupt → _notifyIntrCallback
 *   0x46 = periodic timer         → _periodicTimerCallback
 *   0x47 = end-of-buffer          → _endBufferCallback
 *
 * For Linux: we don't maintain the full vector→slot lookup table.
 * Instead we maintain a 64-bit enable shadow and compute bit positions
 * based on the known vector IDs.  The slot lookup at +0x4D0 in the kext
 * is a dense remapping; for our three vectors we use the slot values
 * directly.  We arm on enable (write bitmask to 0x2208).
 * ============================================================ */

/* ============================================================
 * Driver private state
 * ============================================================ */
struct uad2_dev {
	struct pci_dev *pci;
	struct snd_card *card;
	struct snd_pcm *pcm;

	void __iomem *bar; /* 64 KB MMIO window */
	resource_size_t bar_len;

	spinlock_t lock; /* protects HW register access
	                                         * (mirrors kext hw_lock at this+0x10) */
	spinlock_t notify_lock; /* protects notification handler
	                                         * (mirrors kext spinlock at this+0x2890) */

	u32 expected_device_id;
	u32 device_type; /* UA_DEV_APOLLO_* enum */
	u32 fpga_rev; /* cached FPGA revision from 0x2218 */
	bool fw_v2; /* v2 firmware (bit 31 of fpga_rev set) */
	bool has_extended_irq; /* uses DMA1 regs for upper 32 slots */
	bool disconnecting; /* set during remove, guards MMIO access */
	unsigned int dsp_count;
	u32 channel_base_index; /* 10 (0x0A) for all Apollo TB devices */

	/* Shadow register for DMA master control (never read from HW) */
	u32 dma_ctrl_shadow;

	/* Shadow register for interrupt enable bitmask
	 * (mirrors IntrManager+0x20 in kext) */
	u64 intr_enable_shadow;

	/* Accumulated active interrupt bits from hardirq top-half,
	 * consumed by threaded bottom-half.  Protected by dev->lock. */
	u64 irq_active;

	/* Ring buffer DMA allocations (4 x 4KB pages per ring) */
	dma_addr_t ring_dma_addr[DSP_RING_DESC_SLOTS];
	void *ring_dma_buf[DSP_RING_DESC_SLOTS];

	/* Two 4MB DMA buffers (playback + capture scatter-gather source)
	 * Allocated as contiguous DMA memory, page addresses written to
	 * BAR+0x8000-0x9FFF (playback) and BAR+0xA000-0xBFFF (capture).
	 * ALSA PCM buffers are mapped directly into these. */
	dma_addr_t sg_dma_addr[2]; /* [0]=playback, [1]=capture */
	void *sg_dma_buf[2];
	size_t sg_buf_size; /* 0x400000 = 4MB */

	/* Firmware base address (read from BAR+0x30/0x34 after SG programming) */
	u64 fw_base_addr;

	/* Audio parameters (computed from channel config or defaults) */
	unsigned int buffer_frames; /* from _recomputeBufferFrameSize */
	unsigned int irq_period_frames;
	unsigned int periodic_timer_interval; /* from kext this+0x28C0 */
	unsigned int play_channels;
	unsigned int rec_channels;
	unsigned int clock_source; /* 0=internal, 1=SPDIF */
	unsigned int current_rate; /* current sample rate in Hz */

	/* Transport state (mirrors kext this+0x2878):
	 * 0=uninit, 1=prepared, 2=running */
	int transport_state;

	/* Number of active streams (playback + capture).
	 * The hardware has a single shared transport for both directions.
	 * We reference-count active streams so StopTransport only stops
	 * the hardware when the LAST stream stops.  Protected by dev->lock. */
	int streams_running;

	/* Per-stream running flags.  Used by the IRQ thread to decide
	 * which substreams should receive snd_pcm_period_elapsed().
	 * Set in trigger(START), cleared in trigger(STOP).
	 * Protected by dev->lock. */
	bool playback_running;
	bool capture_running;

	/* Number of substreams that have been prepared (pcm_prepare called
	 * but hw_free not yet called).  Used to prevent hw_free on one
	 * substream from tearing down the transport that the other substream
	 * still depends on.  Protected by dev->lock. */
	int streams_prepared;

	/* Number of substreams currently open (pcm_open called but
	 * pcm_close not yet called).  The hardware transport is only
	 * fully torn down when this reaches 0 — NOT on hw_free.
	 * This prevents the pathological cycle where PipeWire's rapid
	 * stop→hw_free→prepare→start reconfiguration briefly drives
	 * streams_prepared to 0, tearing down a transport that was about
	 * to be re-prepared immediately.  Following the snd-dice/snd-bebob
	 * pattern: transport lifecycle is tied to open/close, not to
	 * hw_params/hw_free.  Protected by dev->lock. */
	int open_count;

	/* Notification interrupt handling */
	struct completion notify_event; /* signals Connect() on bit 21 */
	struct completion connect_event; /* signals on bit 5 (connect ack) */
	struct completion rate_event; /* signals _setSampleClock on bit 5 */
	u32 notify_status; /* last notification bitmask */
	bool connected;

	/* IO descriptor copies from firmware (72 DWORDs each).
	 * Kext copies the full block on NOTIFY_PLAYBACK_IO / NOTIFY_RECORD_IO.
	 * Channel count is at DWORD[4] (offset +0x10 from desc base). */
	u32 play_io_desc[IO_DESC_DWORDS];
	u32 rec_io_desc[IO_DESC_DWORDS];

	/* Channel config area copy (10 DWORDs from BAR+0xC000) */
	u32 chan_config[CHAN_CONFIG_DWORDS];

	/* Mixer batch write state (from open-apollo CPcieDeviceMixer protocol).
	 * The DSP reads ALL 38 settings atomically on each SEQ_WR advance.
	 * We maintain cached val/mask arrays and flush the full batch. */
	u32 mixer_val[MIXER_BATCH_COUNT];
	u32 mixer_mask[MIXER_BATCH_COUNT];
	u32 mixer_seq_wr;
	bool mixer_dirty;
	bool mixer_ready;
	u32 mixer_rb_data[MIXER_RB_WORDS];

	/* PCIe setup state */
	bool pcie_setup_done;

	/* DSP service loop — periodic readback drain + mixer flush.
	 * The Apollo DSP firmware needs periodic "servicing" from the host.
	 * Without this, the DSP halts and mixer settings are never processed.
	 * Runs at 10 Hz via delayed_work. */
	struct delayed_work dsp_service_work;
	bool dsp_service_running;
	unsigned int dsp_service_count;

	/* PCM substream pointers for IRQ handler (accessed under lock) */
	struct snd_pcm_substream *playback_ss;
	struct snd_pcm_substream *capture_ss;

	/* Per-direction DMA position offsets for hw_ptr alignment.
	 * When pcm_prepare is called while the transport is already running,
	 * ALSA resets hw_ptr to 0 but the hardware DMA position counter is
	 * at some arbitrary value.  We snapshot the hardware position at
	 * prepare time and subtract it in .pointer() so that ALSA sees
	 * positions starting from 0.
	 *
	 * MUST be per-direction: if playback starts first (offset=0) and
	 * capture opens later (offset=N), a shared offset would break
	 * playback's pointer tracking → ALSA returns -EIO.
	 *
	 * Set to 0 on cold-start prepare (transport was stopped), set to
	 * current REG_DMA_POSITION on hot re-prepare. */
	u32 play_pos_offset;
	u32 rec_pos_offset;

	/* Period elapsed polling timer.
	 * The Apollo's hardware periodic timer IRQ (vector 0x46) is
	 * unreliable — fires once then stops.  We use an hrtimer to
	 * poll SAMPLE_POS and call snd_pcm_period_elapsed() at ~1ms.
	 * Same approach as USB audio drivers (snd-usb-audio). */
	struct hrtimer period_timer;
	bool period_timer_running;
	snd_pcm_uframes_t last_play_period;
	snd_pcm_uframes_t last_rec_period;
};

/* Forward declarations */
static unsigned int uad2_compute_buffer_frames(unsigned int play_ch,
					       unsigned int rec_ch);
static void uad2_period_timer_stop(struct uad2_dev *dev);

/* ============================================================
 * Low-level register accessors
 * Mirrors CUAOS::ReadReg / CUAOS::WriteReg
 *
 * When the Thunderbolt device is hot-unplugged, MMIO reads return
 * 0xFFFFFFFF.  The disconnecting flag is set early in uad2_remove()
 * so we can skip MMIO to a disappeared device.  In the IRQ handler
 * path we also check for the 0xFFFFFFFF sentinel.
 * ============================================================ */
static inline u32 uad2_read32(struct uad2_dev *dev, u32 offset)
{
	if (unlikely(READ_ONCE(dev->disconnecting)))
		return 0xFFFFFFFF;
	return ioread32(dev->bar + offset);
}

static inline void uad2_write32(struct uad2_dev *dev, u32 offset, u32 val)
{
	if (unlikely(READ_ONCE(dev->disconnecting)))
		return;
	iowrite32(val, dev->bar + offset);
}

/* Compute the firmware mailbox register address with channel_base_index */
static inline u32 uad2_fw_reg(struct uad2_dev *dev, u32 base_offset)
{
	return (dev->channel_base_index << 2) + base_offset;
}

/* ============================================================
 * Mixer setting register offset (3-range formula)
 *
 * From open-apollo ua_apollo.h ua_mixer_setting_reg().
 * Settings live in 3 non-contiguous ranges within the 0x3800 window:
 *   Settings  0-15: base = 0x3800 + 0xB4 + index * 8
 *   Settings 16-31: base = 0x3800 + 0xBC + index * 8  (+8 gap)
 *   Settings 32-37: base = 0x3800 + 0xC0 + index * 8  (+4 more gap)
 * ============================================================ */
static inline u32 uad2_mixer_setting_reg(unsigned int index)
{
	if (index <= 15)
		return REG_MIXER_BASE + 0xB4 + index * MIXER_SETTING_STRIDE;
	else if (index <= 31)
		return REG_MIXER_BASE + 0xBC + index * MIXER_SETTING_STRIDE;
	else
		return REG_MIXER_BASE + 0xC0 + index * MIXER_SETTING_STRIDE;
}

/* ============================================================
 * Mixer batch write protocol
 *
 * The DSP reads ALL 38 settings atomically on each SEQ_WR advance.
 * Ported from open-apollo ua_core.c ua_mixer_flush_settings().
 *
 * Protocol:
 *   1. Maintain cached val/mask arrays for all 38 settings
 *   2. On setting change: merge into cache, mark dirty
 *   3. On service tick: if dirty AND SEQ_RD == cached SEQ_WR,
 *      write ALL 38 settings to SRAM, then bump SEQ_WR once
 *
 * Word encoding per setting:
 *   wordA = (mask[15:0] << 16) | val[15:0]
 *   wordB = (mask[31:16] << 16) | val[31:16]
 * ============================================================ */
static void __maybe_unused uad2_mixer_write_setting(struct uad2_dev *dev,
						    unsigned int index,
						    u32 value, u32 mask)
{
	if (index >= MIXER_BATCH_COUNT)
		return;
	if (!dev->mixer_ready)
		return;

	/* Merge into cache (read-modify-write with mask) */
	dev->mixer_val[index] = (dev->mixer_val[index] & ~mask) |
				(value & mask);
	dev->mixer_mask[index] |= mask;
	dev->mixer_dirty = true;
}

static void uad2_mixer_flush_settings(struct uad2_dev *dev)
{
	u32 seq_rd, word_a, word_b, reg;
	int i;

	if (!dev->mixer_ready || !dev->mixer_dirty)
		return;

	/* Check DSP idle: SEQ_RD must match our cached SEQ_WR */
	seq_rd = uad2_read32(dev, REG_MIXER_SEQ_RD);
	if (seq_rd != dev->mixer_seq_wr)
		return; /* DSP still processing previous batch */

	/* Write ALL 38 settings to SRAM registers.
	 * Skip settings with no mask — preserves firmware defaults.
	 * Val AND mask persist across flushes (DSP reads both on
	 * every SEQ bump).  Do NOT clear mask after writing. */
	for (i = 0; i < MIXER_BATCH_COUNT; i++) {
		if (!dev->mixer_mask[i])
			continue;
		reg = uad2_mixer_setting_reg(i);
		word_a = ((dev->mixer_mask[i] & 0xFFFF) << 16) |
			 (dev->mixer_val[i] & 0xFFFF);
		word_b = (((dev->mixer_mask[i] >> 16) & 0xFFFF) << 16) |
			 ((dev->mixer_val[i] >> 16) & 0xFFFF);
		uad2_write32(dev, reg, word_a);
		uad2_write32(dev, reg + 4, word_b);
	}

	/* Single SEQ_WR bump for entire batch */
	dev->mixer_seq_wr++;
	uad2_write32(dev, REG_MIXER_SEQ_WR, dev->mixer_seq_wr);
	dev->mixer_dirty = false;
}

/* Initialize mixer batch protocol after connect.
 * Sync SEQ counter, zero cache, activate capture routing.
 * From open-apollo ua_audio.c:1009-1072. */
static void uad2_mixer_init(struct uad2_dev *dev)
{
	u32 seq_rd;
	int s;

	seq_rd = uad2_read32(dev, REG_MIXER_SEQ_RD);
	dev->mixer_seq_wr = seq_rd;
	uad2_write32(dev, REG_MIXER_SEQ_WR, seq_rd);

	memset(dev->mixer_val, 0, sizeof(dev->mixer_val));
	memset(dev->mixer_mask, 0, sizeof(dev->mixer_mask));
	dev->mixer_dirty = false;
	dev->mixer_ready = true;

	/* Activate DSP capture routing.
	 * Settings[1-37] = 0x20, mask=0xFF enables type 0x01 capture.
	 * Setting[35] = 0x05, mask=0x1F is the channel config count.
	 * Skip setting[2] — monitor core (volume/mute/source/dim).
	 * Writing it corrupts standalone monitor operation. */
	for (s = 1; s < MIXER_BATCH_COUNT; s++) {
		if (s == 2)
			continue; /* skip monitor core */
		dev->mixer_val[s] = 0x20;
		dev->mixer_mask[s] = 0xFF;
	}
	dev->mixer_val[35] = 0x05;
	dev->mixer_mask[35] = 0x1F;
	dev->mixer_dirty = true;

	dev_info(&dev->pci->dev, "mixer init (SEQ=%u, capture routing armed)\n",
		 seq_rd);
}

/* ============================================================
 * DSP service loop — periodic readback drain + mixer flush
 *
 * The Apollo DSP firmware needs periodic "servicing" from the host to
 * stay alive.  On macOS, the kext calls CDSPResourceManager::Service()
 * ~103/sec.  Without this, the DSP halts: front panel freezes, mixer
 * settings are written but never processed (SEQ_RD never advances).
 *
 * The readback drain cycle (read status→read data→write 0 to re-arm)
 * acts as the host liveness signal.  We run this at 100 Hz via
 * delayed_work.
 *
 * Ported from open-apollo ua_audio.c:434-539.
 * ============================================================ */
static void uad2_dsp_service_handler(struct work_struct *work)
{
	struct uad2_dev *dev = container_of(to_delayed_work(work),
					    struct uad2_dev, dsp_service_work);
	u32 rb_status, notif;
	u32 notify_reg = uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS);
	unsigned int i;

	if (READ_ONCE(dev->disconnecting) || !dev->dsp_service_running)
		return;

	/* Poll and clear notification status register.
	 * If the DSP fires notifications and the host never clears them,
	 * the DSP may stall.  This register is NOT write-1-to-clear;
	 * write 0 to clear all bits. */
	notif = uad2_read32(dev, notify_reg);
	if (notif == 0xFFFFFFFF) {
		/* Device gone (Thunderbolt hot-unplug) */
		dev->dsp_service_running = false;
		return;
	}
	if (notif)
		uad2_write32(dev, notify_reg, 0x0);

	/* Drain readback data (40 words from 0x3814, re-arm by writing
	 * 0 to 0x3810).  This is the host liveness signal. */
	rb_status = uad2_read32(dev, REG_MIXER_RB_STATUS);
	if (rb_status == 1) {
		for (i = 0; i < MIXER_RB_WORDS; i++)
			dev->mixer_rb_data[i] =
				uad2_read32(dev, REG_MIXER_RB_DATA + i * 4);
		uad2_write32(dev, REG_MIXER_RB_STATUS, 0);
	}

	/* Flush pending mixer settings (after initial stabilization) */
	if (dev->dsp_service_count >= 5)
		uad2_mixer_flush_settings(dev);

	dev->dsp_service_count++;

	/* Reschedule */
	if (dev->dsp_service_running)
		schedule_delayed_work(
			&dev->dsp_service_work,
			msecs_to_jiffies(DSP_SERVICE_INTERVAL_MS));
}

static void uad2_dsp_service_start(struct uad2_dev *dev)
{
	if (dev->dsp_service_running)
		return;

	dev_info(&dev->pci->dev, "starting DSP service loop (%u ms)\n",
		 DSP_SERVICE_INTERVAL_MS);

	dev->dsp_service_running = true;
	dev->dsp_service_count = 0;
	schedule_delayed_work(&dev->dsp_service_work,
			      msecs_to_jiffies(DSP_SERVICE_INTERVAL_MS));
}

static void uad2_dsp_service_stop(struct uad2_dev *dev)
{
	if (!dev->dsp_service_running)
		return;

	dev->dsp_service_running = false;
	cancel_delayed_work_sync(&dev->dsp_service_work);
}

/* ============================================================
 * Device detection — read serial prefix, look up model, set channels
 *
 * All Apollo Thunderbolt devices share PCI vendor 0x1A00, device 0x0002.
 * The model is identified by a 4-digit ASCII serial prefix read from
 * BAR0+0x20..0x2C.  The FPGA revision register (0x2218) bit 31
 * distinguishes v1 (clear) from v2 (set) firmware; v2 devices use
 * the extended capabilities register (0x2234) for DSP count.
 *
 * Ported from open-apollo ua_core.c ua_read_serial_type() and
 * ua_detect_capabilities().
 * ============================================================ */
static void uad2_detect_device(struct uad2_dev *dev)
{
	char serial[UA_REG_SERIAL_LEN + 1];
	u32 regs[4];
	u32 ext_caps;
	u16 subsys;
	int i;

	/* Cache FPGA revision and detect firmware version */
	dev->fpga_rev = uad2_read32(dev, REG_FPGA_REV);

	/* Try PCI subsystem ID first — most reliable for known models.
	 * Serial prefix detection is unreliable on some models (see
	 * Apollo Solo false-match to x16D). */
	subsys = dev->pci->subsystem_device;
	switch (subsys) {
	case UA_SUBSYS_SOLO:
		dev->device_type = UA_DEV_APOLLO_SOLO;
		dev_info(&dev->pci->dev, "subsystem 0x%04x → Apollo Solo\n",
			 subsys);
		break;
	case UA_SUBSYS_X4_QUAD:
		dev->device_type = UA_DEV_APOLLO_X4;
		dev_info(&dev->pci->dev, "subsystem 0x%04x → Apollo x4\n",
			 subsys);
		break;
	default:
		dev->device_type = 0; /* will try serial prefix below */
		break;
	}

	if ((s32)dev->fpga_rev >= 0) {
		/* v1 firmware: device type from FPGA rev bits[31:28] - 1 */
		dev->fw_v2 = false;
		dev->device_type = (dev->fpga_rev >> 28) - 1;
		dev->dsp_count = 1; /* v1 default, refine from subsys ID */
	} else {
		/* v2 firmware: DSP count from ext_caps */
		dev->fw_v2 = true;
		ext_caps = uad2_read32(dev, REG_EXT_CAPS);
		dev->dsp_count = (ext_caps >> 8) & 0xFF;
		if (dev->dsp_count > UAD2_MAX_DSPS)
			dev->dsp_count = UAD2_MAX_DSPS;

		dev_info(&dev->pci->dev, "ext_caps=0x%08x dsp=%u\n", ext_caps,
			 dev->dsp_count);

		/* Read serial for logging */
		for (i = 0; i < 4; i++)
			regs[i] = uad2_read32(dev, UA_REG_SERIAL_BASE + i * 4);
		memcpy(serial, regs, UA_REG_SERIAL_LEN);
		serial[UA_REG_SERIAL_LEN] = '\0';
		dev_info(&dev->pci->dev, "serial: %.16s\n", serial);

		/* Serial prefix lookup only if subsystem ID didn't match.
		 * Serial prefix is unreliable on some models (Apollo Solo
		 * serial contains "2032" at offset 4 → false x16D match). */
		if (!dev->device_type) {
			for (i = 0; i < (int)ARRAY_SIZE(ua_serial_table); i++) {
				if (!strncmp(serial + 4,
					     ua_serial_table[i].prefix, 4)) {
					dev->device_type =
						ua_serial_table[i].device_type;
					dev_info(&dev->pci->dev,
						 "serial prefix → %s\n",
						 ua_device_name(
							 dev->device_type));
					break;
				}
			}
		}
	}

	/* Look up channel counts from model table */
	for (i = 0; i < (int)ARRAY_SIZE(ua_models); i++) {
		if (ua_models[i].device_type == dev->device_type) {
			dev->play_channels = ua_models[i].play_ch;
			dev->rec_channels = ua_models[i].rec_ch;
			goto found;
		}
	}

	/* Fallback: safe stereo default for unknown models */
	dev_warn(&dev->pci->dev,
		 "unknown device type 0x%02x, using safe defaults\n",
		 dev->device_type);
	dev->play_channels = 3;
	dev->rec_channels = 2;

found:
	dev->buffer_frames = uad2_compute_buffer_frames(dev->play_channels,
							dev->rec_channels);
	dev_info(&dev->pci->dev,
		 "%s detected: type=0x%02x fw_%s dsp=%u "
		 "play=%uch rec=%uch buf=%u\n",
		 ua_device_name(dev->device_type), dev->device_type,
		 dev->fw_v2 ? "v2" : "v1", dev->dsp_count, dev->play_channels,
		 dev->rec_channels, dev->buffer_frames);
}

/* ============================================================
 * PCIe setup: disable ASPM, increase completion timeouts
 *
 * The Thunderbolt bridge chain may put the PCIe link to sleep via
 * ASPM (Active State Power Management).  When the link is in L1,
 * MMIO reads stall for up to 10 seconds (PCIe completion timeout).
 * The DSP service loop reads BAR0 every 10ms — if the link sleeps,
 * these reads stall and can cascade into a system freeze.
 *
 * This walks the bridge chain from the device up to the root port,
 * disabling ASPM (LNKCTL bits[1:0]=0) and setting completion
 * timeout to range D (65-210ms) on each hop.
 *
 * Ported from open-apollo ua_audio.c ua_pcie_setup().
 * Called once from probe after BAR mapping.
 * ============================================================ */
static void uad2_pcie_setup(struct uad2_dev *dev)
{
	int pos;
	u16 devctl, devsta, lnkctl, lnksta;
	struct pci_dev *bridge;

	if (dev->pcie_setup_done)
		return;

	/* Configure the endpoint device */
	pos = pci_find_capability(dev->pci, PCI_CAP_ID_EXP);
	if (pos) {
		pci_read_config_word(dev->pci, pos + PCI_EXP_DEVCTL, &devctl);
		pci_read_config_word(dev->pci, pos + PCI_EXP_DEVSTA, &devsta);
		pci_read_config_word(dev->pci, pos + PCI_EXP_LNKCTL, &lnkctl);
		pci_read_config_word(dev->pci, pos + PCI_EXP_LNKSTA, &lnksta);

		dev_info(&dev->pci->dev,
			 "PCIe: LnkCtl=0x%04x LnkSta=0x%04x Gen%u x%u\n",
			 lnkctl, lnksta, lnksta & 0xF, (lnksta >> 4) & 0x3F);

		/* Disable ASPM on device (bits[1:0] of LNKCTL) */
		if (lnkctl & 3) {
			dev_info(&dev->pci->dev,
				 "Disabling device ASPM (was 0x%x)\n",
				 lnkctl & 3);
			lnkctl &= ~3;
			pci_write_config_word(dev->pci, pos + PCI_EXP_LNKCTL,
					      lnkctl);
		}

		/* Clear pending PCIe error status */
		if (devsta & 0xF) {
			pci_write_config_word(dev->pci, pos + PCI_EXP_DEVSTA,
					      devsta);
		}

		/* Set completion timeout to range D (65-210ms) */
		{
			u16 devctl2;

			pci_read_config_word(dev->pci, pos + PCI_EXP_DEVCTL2,
					     &devctl2);
			if ((devctl2 & 0xF) != 0x6) {
				devctl2 = (devctl2 & ~0xF) | 0x6;
				pci_write_config_word(dev->pci,
						      pos + PCI_EXP_DEVCTL2,
						      devctl2);
			}
		}
	}

	/* Walk upstream bridges and disable ASPM on each hop */
	bridge = dev->pci->bus->self;
	while (bridge) {
		pos = pci_find_capability(bridge, PCI_CAP_ID_EXP);
		if (pos) {
			u16 devctl2;

			pci_read_config_word(bridge, pos + PCI_EXP_LNKCTL,
					     &lnkctl);
			pci_read_config_word(bridge, pos + PCI_EXP_DEVCTL2,
					     &devctl2);

			/* Disable ASPM */
			if (lnkctl & 3) {
				lnkctl &= ~3;
				pci_write_config_word(
					bridge, pos + PCI_EXP_LNKCTL, lnkctl);
				dev_info(&dev->pci->dev,
					 "Bridge %s: ASPM disabled\n",
					 pci_name(bridge));
			}

			/* Set completion timeout to range D */
			if ((devctl2 & 0xF) != 0x6) {
				devctl2 = (devctl2 & ~0xF) | 0x6;
				pci_write_config_word(
					bridge, pos + PCI_EXP_DEVCTL2, devctl2);
			}
		}
		if (!bridge->bus || !bridge->bus->self)
			break;
		bridge = bridge->bus->self;
	}

	dev->pcie_setup_done = true;
	dev_info(&dev->pci->dev, "PCIe ASPM/timeout setup complete\n");
}

/* ============================================================
 * Sample rate helpers
 * ============================================================ */
static u8 uad2_rate_to_enum(unsigned int rate)
{
	switch (rate) {
	case 44100:
		return UA_RATE_44100;
	case 48000:
		return UA_RATE_48000;
	case 88200:
		return UA_RATE_88200;
	case 96000:
		return UA_RATE_96000;
	case 176400:
		return UA_RATE_176400;
	case 192000:
		return UA_RATE_192000;
	default:
		return UA_RATE_48000;
	}
}

/*
 * CPcieAudioExtension::_setSampleClock @ line 72712 (arm64 0x4DE70)
 *
 * Kext flow (confirmed in both arm64 and x86_64):
 *   1. Compute clock_val = clock_source | (rate_enum << 8)
 *   2. Write clock_val to BAR + (fpga_index * 4) + 0xC04C
 *   3. Reset rate_event (CUAOS::Event at this+0x2838)
 *   4. Write 0x4 to REG_STREAM_ENABLE (trigger clock change)
 *   5. WaitWithTimeout(2000ms) on rate_event
 *   6. On signal: read REG_DEVICE_ID as sanity check
 *   7. On timeout: return 0 (kext treats timeout as success)
 *
 * The rate_event is signaled by bit 5 in the notification ISR
 * (_handleNotificationInterrupt → rate_event->Signal()).
 *
 * The caller (SetSampleClock @ 0x4DD08) retries once if _setSampleClock
 * returns -38 (signaled + device ID valid).  We simplify: wait for
 * completion, return 0 on success or timeout (matching kext behavior).
 */
static int uad2_set_sample_rate(struct uad2_dev *dev, unsigned int rate)
{
	u32 clock_val;
	u8 rate_enum;
	u32 reg_offset;
	unsigned long ret;

	rate_enum = uad2_rate_to_enum(rate);
	clock_val = dev->clock_source | ((u32)rate_enum << 8);

	/* Cache clock value (kext stores at this+0x98) */
	reg_offset = uad2_fw_reg(dev, REG_SAMPLE_CLOCK);

	/* Write to sample clock register: BAR + (channel_base_index << 2) + 0xC04C */
	uad2_write32(dev, reg_offset, clock_val);

	/* Reset completion before triggering (kext: rate_event->Reset()) */
	reinit_completion(&dev->rate_event);

	/* Trigger clock change */
	uad2_write32(dev, REG_STREAM_ENABLE, 0x4);

	/* Wait for firmware ack via bit 5 notification interrupt.
	 * Kext uses WaitWithTimeout(2000) = 2 seconds. */
	ret = wait_for_completion_timeout(&dev->rate_event,
					  msecs_to_jiffies(2000));

	if (ret > 0) {
		/* Completion signaled — read device ID as sanity check
		 * (kext does this but doesn't use the result to block) */
		u32 dev_id = uad2_read32(dev, REG_DEVICE_ID);

		if (dev_id != dev->expected_device_id)
			dev_warn(&dev->pci->dev,
				 "Post-clock device ID mismatch: 0x%08x\n",
				 dev_id);

		dev->current_rate = rate;
		return 0;
	}

	/* Timeout — kext returns 0 (success) on timeout, so we do too.
	 * The hardware may still complete the change asynchronously. */
	dev_warn(&dev->pci->dev,
		 "Sample rate change timeout (rate=%u), continuing\n", rate);
	dev->current_rate = rate;
	return 0;
}

/* ============================================================
 * _recomputeBufferFrameSize equivalent @ 0x4d9e0 (line 72414)
 *
 * Formula: floor_pow2(0x400000 / (max(play_ch, rec_ch) * 4) / 2)
 * Capped at 0x2000 (8192 frames).
 *
 * Kext at 0x4DA1C: ubfx x8, x8, #1, #31 — divides by 2 BEFORE CLZ.
 *
 * For Apollo Solo with 42 play / 32 rec:
 *   0x400000 / (42 * 4) = 24966, /2 = 12483 → floor_pow2 = 8192
 * ============================================================ */
static unsigned int uad2_compute_buffer_frames(unsigned int play_ch,
					       unsigned int rec_ch)
{
	unsigned int max_ch = max(play_ch, rec_ch);
	unsigned int raw_frames;

	if (max_ch == 0)
		max_ch = 1;

	raw_frames = SG_BUFFER_SIZE / (max_ch * UAD2_BYTES_PER_SAMPLE);

	/* Kext divides by 2 here (ubfx x8, x8, #1, #31 at 0x4DA1C) */
	raw_frames /= 2;

	/* Round down to power of 2 */
	if (raw_frames == 0)
		return UAD2_MAX_BUFFER_FRAMES; /* fallback */
	raw_frames = rounddown_pow_of_two(raw_frames);

	/* Cap at 8192 */
	if (raw_frames > UAD2_MAX_BUFFER_FRAMES)
		raw_frames = UAD2_MAX_BUFFER_FRAMES;

	return raw_frames;
}

/* ============================================================
 * Interrupt vector enable/disable
 *
 * The kext maintains a vector_to_slot[72] lookup table at
 * IntrManager+0x4D0 that maps logical vector IDs to bit slot indices
 * within the u64 intr_enable_shadow:
 *
 *   vector 0x28 (notify)   → slot 32   (bit 0 of DMA1)
 *   vector 0x46 (periodic) → slot 62   (bit 30 of DMA1)
 *   vector 0x47 (endbuf)   → slot 63   (bit 31 of DMA1)
 *
 * Shadow bits  0-31 → DMA0 registers (0x2204 ctrl, 0x2208 status)
 * Shadow bits 32-63 → DMA1 registers (0x2268 ctrl, 0x2264 status)
 *
 * For Apollo Solo all three vectors map to slots > 31, so
 * has_extended_irq must be true and DMA1 registers must be accessed.
 *
 * EnableVector(slot, arm):
 *   shadow |= (1 << slot)
 *   if arm: write (1 << slot) to status register (re-arm)
 *   write shadow to ctrl register (enable mask)
 *
 * DisableVector(slot):
 *   shadow &= ~(1 << slot)
 *   write shadow to ctrl register
 * ============================================================ */
/* Internal: caller must hold dev->lock */
static void __uad2_enable_vector_locked(struct uad2_dev *dev, unsigned int slot,
					bool arm)
{
	u64 bit = 1ULL << slot;

	dev->intr_enable_shadow |= bit;

	if (arm)
		uad2_write32(dev, REG_DMA0_STATUS, (u32)bit);

	uad2_write32(dev, REG_DMA0_INTR_CTRL, (u32)dev->intr_enable_shadow);

	if (dev->has_extended_irq) {
		if (arm)
			uad2_write32(dev, REG_DMA1_STATUS, (u32)(bit >> 32));
		uad2_write32(dev, REG_DMA1_INTR_CTRL,
			     (u32)(dev->intr_enable_shadow >> 32));
	}
}

/* Internal: caller must hold dev->lock */
static void __uad2_disable_vector_locked(struct uad2_dev *dev,
					 unsigned int slot)
{
	u64 bit = 1ULL << slot;

	dev->intr_enable_shadow &= ~bit;

	uad2_write32(dev, REG_DMA0_INTR_CTRL, (u32)dev->intr_enable_shadow);

	if (dev->has_extended_irq)
		uad2_write32(dev, REG_DMA1_INTR_CTRL,
			     (u32)(dev->intr_enable_shadow >> 32));
}

static void uad2_enable_vector(struct uad2_dev *dev, unsigned int slot,
			       bool arm)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	__uad2_enable_vector_locked(dev, slot, arm);
	spin_unlock_irqrestore(&dev->lock, flags);
}

static void uad2_disable_vector(struct uad2_dev *dev, unsigned int slot)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	__uad2_disable_vector_locked(dev, slot);
	spin_unlock_irqrestore(&dev->lock, flags);
}

/* Interrupt slot indices — mapped from kext vector_to_slot[] table:
 *   vector 0x28 → slot 32,  vector 0x46 → slot 62,  vector 0x47 → slot 63 */
#define INTR_SLOT_NOTIFY 32 /* notification interrupt (vector 0x28) */
#define INTR_SLOT_PERIODIC 62 /* periodic timer interrupt (vector 0x46) */
#define INTR_SLOT_ENDBUF 63 /* end-of-buffer interrupt (vector 0x47) */

/* ============================================================
 * CPcieIntrManager::ProgramRegisters equivalent
 * Clears DMA interrupt and status registers.
 * Called only during probe (sequential init), no locking needed.
 * ============================================================ */
static void uad2_intr_program(struct uad2_dev *dev)
{
	uad2_write32(dev, REG_DMA0_INTR_CTRL, 0x0);
	uad2_write32(dev, REG_DMA0_STATUS, 0x0);

	if (dev->has_extended_irq) {
		uad2_write32(dev, REG_DMA1_STATUS, 0x0);
		uad2_write32(dev, REG_DMA1_INTR_CTRL, 0x0);
	}

	dev->intr_enable_shadow = 0;
}

/* ============================================================
 * CPcieIntrManager::ResetDMAEngines equivalent @ 0x134b8
 *
 * Reset pulse sequence from kext:
 *   1. AND shadow with mask to clear DMA channel enable bits
 *      (preserving bit 0 = global master enable)
 *   2. Write shadow | reset_bits  — asserts reset
 *   3. Write shadow alone        — deasserts reset
 *   4. Read-back flush
 *
 * Shadow is initialized to DMA_CTRL_MASTER_ENABLE (0x1) by
 * CPcieIntrManager::Initialize (dump line 11571: str w8,[x19,#0x4bc])
 * ============================================================ */
static void uad2_dma_reset(struct uad2_dev *dev)
{
	u32 reset_bits, mask;

	if (dev->has_extended_irq) {
		reset_bits = DMA_RESET_DUAL;
		mask = DMA_SHADOW_MASK_DUAL;
	} else {
		reset_bits = DMA_RESET_SINGLE;
		mask = DMA_SHADOW_MASK_SINGLE;
	}

	/* Clear channel enable bits in shadow, keep bit 0 (master enable) */
	dev->dma_ctrl_shadow &= mask;

	/* Assert reset: shadow | reset_bits */
	uad2_write32(dev, REG_DMA_MASTER_CTRL,
		     dev->dma_ctrl_shadow | reset_bits);

	/* Deassert reset: shadow alone */
	uad2_write32(dev, REG_DMA_MASTER_CTRL, dev->dma_ctrl_shadow);

	/* Read-back flush */
	uad2_read32(dev, REG_DMA_MASTER_CTRL);
}

/* ============================================================
 * CPcieIntrManager::EnableDMA equivalent @ 0x13a4c
 *
 * Sets bit (1 << (dsp_index + 1)) in shadow register, writes to
 * REG_DMA_MASTER_CTRL.  Bit 0 is reserved (global master enable).
 * dsp_index must be < 64.
 * ============================================================ */
static void uad2_enable_dma(struct uad2_dev *dev, unsigned int dsp_index)
{
	u32 bit;

	if (WARN_ON(dsp_index >= 64))
		return;

	bit = 1u << (dsp_index + 1);
	dev->dma_ctrl_shadow |= bit;
	uad2_write32(dev, REG_DMA_MASTER_CTRL, dev->dma_ctrl_shadow);
}

/* ============================================================
 * CPcieDSP::_waitFor469ToStart equivalent @ 0x1152c
 *
 * Polls dsp_poll_base+0x1A4 waiting for DSP firmware to boot.
 * dsp_poll_base is a SEPARATE address from ring_base:
 *   ring_base     = BAR + {0x2000|0x5e00} + dsp_index*0x80  (DMA rings)
 *   dsp_poll_base = BAR + (dsp_index > 3 ? 0x2000 : 0) + dsp_index*0x800
 *
 * For DSP 0: poll_base = BAR+0x000, polls BAR+0x1A4
 * For DSP 1: poll_base = BAR+0x800, polls BAR+0x9A4
 *
 * Condition: called when dsp_type == 1 (from GetCapabilities)
 *   and this+0x18 == 0 (first-time init flag).
 *   dsp_index == 0 gets 100 attempts, others get 10.
 *
 * CORRECTED LOGIC (from careful arm64+x86_64 trace):
 * The kext loop treats BOTH 0 AND DSP_READY_SIG as "not ready yet" —
 * it keeps polling on either.  The loop only exits early when the value
 * is non-zero AND not equal to DSP_READY_SIG.  After the loop ends
 * (either by early exit or counter exhaustion), the final success check
 * is `val & 1` — bit 0 must be set for success.
 *
 * This suggests the DSP boot sequence is:
 *   0x00000000 (not started) → 0xa8caed0f (booting) → final_val (ready)
 * where final_val has bit 0 set.
 * ============================================================ */
static int uad2_dsp_wait_ready(struct uad2_dev *dev,
			       void __iomem *dsp_poll_base, int dsp_index)
{
	int max_attempts = (dsp_index == 0) ? DSP_POLL_MAX_PRIMARY :
					      DSP_POLL_MAX_SECONDARY;
	int i;
	u32 val = 0;

	for (i = 0; i < max_attempts; i++) {
		val = ioread32(dsp_poll_base + DSP_READY_POLL_OFF);

		/* Kext exits loop immediately on non-zero, non-DSP_READY_SIG.
		 * Both 0 and DSP_READY_SIG mean "still booting". */
		if (val != 0 && val != DSP_READY_SIG)
			break;

		msleep(DSP_POLL_INTERVAL_MS);
	}

	/* Kext final success check: bit 0 of last read value must be set.
	 * arm64: tbnz w20, #0x0, <success>
	 * x86_64: testb $0x1, %r13b; jne <success> */
	if (val & 1)
		return 0;

	dev_err(&dev->pci->dev,
		"DSP %d failed to start (timeout after %dms, val=0x%08x)\n",
		dsp_index, max_attempts * DSP_POLL_INTERVAL_MS, val);
	return -ETIMEDOUT;
}

/* ============================================================
 * CPcieRingBuffer::ProgramRegisters equivalent @ 0x14c48
 *
 * Programs one ring buffer with 4 DMA descriptor pages:
 *   1. Read hardware ring capacity from ring_base+0x28, cap at 0x400
 *   2. Write ring_size to ring_base+0x24 (ring size register)
 *   3. Write ring_size to ring_base+0x20 (descriptor count register)
 *   4. For each of 4 descriptor slots:
 *      - Get physical address of page (i * 0x1000)
 *      - Write low 32 bits to ring_base + (i * 8)
 *      - Write high 32 bits to ring_base + (i * 8) + 4
 *      - Read back both and verify match
 * ============================================================ */
static int uad2_ring_program(struct uad2_dev *dev, void __iomem *ring_base,
			     dma_addr_t dma_addr, int ring_idx)
{
	u32 ring_size;
	int i;

	/* Read hardware ring capacity.
	 * Kext zeros ring_size when >= 0x400 (likely error detection). */
	ring_size = ioread32(ring_base + DSP_RING_CAPACITY);
	if (ring_size >= 0x400) {
		dev_warn(
			&dev->pci->dev,
			"Ring %d capacity 0x%x >= 0x400, zeroing (kext behavior)\n",
			ring_idx, ring_size);
		ring_size = 0;
	}

	/* Program ring size and descriptor count */
	iowrite32(ring_size, ring_base + DSP_RING_SIZE_REG);
	iowrite32(ring_size, ring_base + DSP_RING_DESC_COUNT);

	/* Program 4 DMA descriptor slots with 4KB-aligned physical pages */
	for (i = 0; i < DSP_RING_DESC_SLOTS; i++) {
		dma_addr_t page_addr = dma_addr + (i * DSP_RING_PAGE_SIZE);
		u32 lo = lower_32_bits(page_addr);
		u32 hi = upper_32_bits(page_addr);
		u32 rb_lo, rb_hi;

		/* Assert page alignment */
		if (WARN_ON(page_addr & 0xFFF))
			return -EINVAL;

		/* Write low then high 32-bit physical address */
		iowrite32(lo, ring_base + (i * 8));
		iowrite32(hi, ring_base + (i * 8) + 4);

		/* Read-back verification (exact kext behavior) */
		rb_hi = ioread32(ring_base + (i * 8) + 4);
		rb_lo = ioread32(ring_base + (i * 8));

		if (rb_lo != lo || rb_hi != hi) {
			dev_err(&dev->pci->dev,
				"Ring %d desc %d readback mismatch: "
				"wrote %08x:%08x read %08x:%08x\n",
				ring_idx, i, hi, lo, rb_hi, rb_lo);
			return -EIO;
		}
	}

	return 0;
}

/* ============================================================
 * CPcieDSP::ProgramRegisters equivalent @ 0x1124c
 * Sets up command/response ring buffer addresses for one DSP
 * ============================================================ */
static int uad2_dsp_program(struct uad2_dev *dev, int dsp_index)
{
	void __iomem *ring_base;
	void __iomem *ring2_base;
	void __iomem *dsp_poll_base;
	dma_addr_t ring_dma;
	int err;

	/* Compute ring base within BAR (for DMA ring buffer programming) */
	if (dsp_index < 4)
		ring_base = dev->bar + DSP_RING_BASE_LOW +
			    (dsp_index * DSP_RING_STRIDE);
	else
		ring_base = dev->bar + DSP_RING_BASE_HIGH +
			    (dsp_index * DSP_RING_STRIDE);

	ring2_base = ring_base + DSP_RING2_OFFSET;

	/* Compute DSP poll base (SEPARATE from ring_base):
	 *   For dsp_index < 4:  BAR + dsp_index * 0x800
	 *   For dsp_index >= 4: BAR + 0x2000 + dsp_index * 0x800 */
	if (dsp_index < 4)
		dsp_poll_base = dev->bar + (dsp_index * 0x800);
	else
		dsp_poll_base = dev->bar + 0x2000 + (dsp_index * 0x800);

	/* Wait for DSP firmware.
	 * Kext condition: dsp_type == 1 && this+0x18 == 0.
	 * For Apollo Solo (1 DSP, dsp_index=0), dsp_type from GetCapabilities
	 * is expected to be 1.  We always wait on first init. */
	err = uad2_dsp_wait_ready(dev, dsp_poll_base, dsp_index);
	if (err)
		return err;

	/*
	 * Allocate DMA pages for ring descriptors (4 x 4KB = 16KB per ring).
	 * In the kext, these are IOBufferMemoryDescriptor pages obtained via
	 * getPhysicalSegment(offset) where offset = i * 0x1000.
	 */
	if (!dev->ring_dma_buf[0]) {
		dev->ring_dma_buf[0] = dma_alloc_coherent(
			&dev->pci->dev,
			DSP_RING_DESC_SLOTS * DSP_RING_PAGE_SIZE,
			&dev->ring_dma_addr[0], GFP_KERNEL);
		if (!dev->ring_dma_buf[0])
			return -ENOMEM;
	}
	ring_dma = dev->ring_dma_addr[0];

	/* Program ring 0 */
	err = uad2_ring_program(dev, ring_base, ring_dma, 0);
	if (err)
		return err;

	/* Program ring 1 (second ring at +0x40) */
	err = uad2_ring_program(dev, ring2_base, ring_dma, 1);
	if (err)
		return err;

	/* Enable DMA for this DSP (CPcieIntrManager::EnableDMA) */
	uad2_enable_dma(dev, dsp_index);

	/* Enable DSP interrupt vectors (CPcieDSP::Initialize @ 0x10CC0).
	 * For dsp_index N, the kext enables vectors N*5+2, N*5+3, N*5+4
	 * which handle message FIFO service and error callbacks.
	 * Vectors 0-39 use identity mapping (slot = vector ID). */
	uad2_enable_vector(dev, dsp_index * 5 + 2, true);
	uad2_enable_vector(dev, dsp_index * 5 + 3, true);
	uad2_enable_vector(dev, dsp_index * 5 + 4, true);

	return 0;
}

/* ============================================================
 * Allocate the two 4MB DMA buffers for audio streaming.
 *
 * MapHardware @ 0x4ba0c (line 70355):
 *   CUAOS::AllocDMABuffer(0x400000, 2, 0, task)  -- called twice
 *   Stored at this+0x28a0 (playback) and this+0x28a8 (capture)
 * ============================================================ */
static int uad2_alloc_sg_buffers(struct uad2_dev *dev)
{
	int i;

	dev->sg_buf_size = SG_BUFFER_SIZE;

	for (i = 0; i < 2; i++) {
		dev->sg_dma_buf[i] =
			dma_alloc_coherent(&dev->pci->dev, dev->sg_buf_size,
					   &dev->sg_dma_addr[i], GFP_KERNEL);
		if (!dev->sg_dma_buf[i]) {
			dev_err(&dev->pci->dev,
				"Failed to allocate 4MB DMA buffer %d\n", i);
			/* Free already-allocated buffer */
			if (i > 0 && dev->sg_dma_buf[0]) {
				dma_free_coherent(&dev->pci->dev,
						  dev->sg_buf_size,
						  dev->sg_dma_buf[0],
						  dev->sg_dma_addr[0]);
				dev->sg_dma_buf[0] = NULL;
			}
			return -ENOMEM;
		}
		/* dma_alloc_coherent returns zeroed memory */
	}

	return 0;
}

static void uad2_free_sg_buffers(struct uad2_dev *dev)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (dev->sg_dma_buf[i]) {
			dma_free_coherent(&dev->pci->dev, dev->sg_buf_size,
					  dev->sg_dma_buf[i],
					  dev->sg_dma_addr[i]);
			dev->sg_dma_buf[i] = NULL;
		}
	}
}

/* ============================================================
 * CPcieAudioExtension::ProgramRegisters equivalent @ 0x4bac0
 *
 * Full sequence (decoded from lines 70401-70631):
 * 1. Check and clear transport status (BAR+0x2248)
 * 2. Program scatter-gather tables:
 *    - 1024 iterations (sg_offset 0x8000..0x9FF8, step 8)
 *    - Each iter: write 64-bit phys addr for playback page to BAR+sg_offset
 *    - Each iter: write 64-bit phys addr for capture page to BAR+sg_offset+0x2000
 * 3. Read firmware base from BAR+0x30/0x34
 * 4. Enable interrupt vector 0x28
 * ============================================================ */
static int uad2_audio_ext_program(struct uad2_dev *dev)
{
	u32 status;
	u32 sg_offset;
	u32 dma_offset;
	dma_addr_t play_page, cap_page;

	/* Verify SG buffers are allocated */
	if (!dev->sg_dma_buf[0] || !dev->sg_dma_buf[1]) {
		dev_err(&dev->pci->dev, "SG DMA buffers not allocated\n");
		return -EINVAL;
	}

	/* Phase 1: Check and clear transport status (exact kext sequence) */
	status = uad2_read32(dev, REG_TRANSPORT_CTL);
	if (status & BIT(5)) {
		/* Read-to-clear the pending status latch */
		uad2_read32(dev, REG_DMA_POSITION);
	}
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);

	/* Phase 2: Program scatter-gather tables
	 * Loop: sg_offset = 0x8000, dma_offset = 0
	 *        sg_offset += 8,    dma_offset += 0x1000
	 *        until sg_offset == 0xA000 (1024 iterations)
	 */
	sg_offset = REG_SG_TABLE_A_BASE;
	dma_offset = 0;

	while (sg_offset != REG_SG_TABLE_A_END) {
		/* Buffer A (playback): write to BAR + sg_offset */
		play_page = dev->sg_dma_addr[0] + dma_offset;
		uad2_write32(dev, sg_offset, lower_32_bits(play_page));
		uad2_write32(dev, sg_offset + 4, upper_32_bits(play_page));

		/* Buffer B (capture): write to BAR + sg_offset + 0x2000 */
		cap_page = dev->sg_dma_addr[1] + dma_offset;
		uad2_write32(dev, sg_offset + REG_SG_TABLE_B_OFFSET,
			     lower_32_bits(cap_page));
		uad2_write32(dev, sg_offset + REG_SG_TABLE_B_OFFSET + 4,
			     upper_32_bits(cap_page));

		dma_offset += SG_PAGE_SIZE;
		sg_offset += SG_ENTRY_SIZE;
	}

	/* Phase 3: Read firmware base address (BAR+0x30 lo, BAR+0x34 hi) */
	dev->fw_base_addr = ((u64)uad2_read32(dev, REG_FW_BASE_HI) << 32) |
			    uad2_read32(dev, REG_FW_BASE_LO);
	/* Phase 4: Enable interrupt vector 0x28 (notification interrupt)
	 *
	 * In the kext, this calls CPcieIntrManager::EnableVector(0x28, 1).
	 * This arms the vector in the DMA interrupt controller and updates
	 * the enable shadow bitmask. */
	uad2_write32(dev, REG_INTR_ENABLE, 0xFFFFFFFF);
	uad2_enable_vector(dev, INTR_SLOT_NOTIFY, true);

	return 0;
}

/* ============================================================
 * _handleNotificationInterrupt equivalent @ 0x4c154 (line 70824)
 *
 * Reads notification bitmask from BAR+(channel_base_index<<2)+0xC008,
 * dispatches events, then writes 0 to clear.
 *
 * Full event dispatch (from analysis of lines 70824-71446):
 *   bit 5:  Connect ack     → signal connect_event
 *   bit 21: Channel config  → copy config from BAR+0xC000, force bits 0+1
 *   bit 0:  Playback ready  → copy playback IO desc from BAR+0xC2C4
 *   bit 1:  Record ready    → copy record IO desc from BAR+0xC1A4
 *   bit 22: Rate change     → read BAR+0xC05C/0xC054
 *   bit 4:  DMA ready       → read BAR+0xC054/0xC058/0xC05C
 *   bit 6:  Error           → log only
 *   bit 21: (second pass)   → signal notify_event (wakes Connect())
 *   bit 7:  End buffer      → (unused for now)
 *   bit 0|1: combined       → (callback)
 *   Finally: write 0 to clear notification register
 * ============================================================ */
static void uad2_handle_notification(struct uad2_dev *dev)
{
	u32 status;
	u32 notify_reg = uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS);
	unsigned long flags;

	spin_lock_irqsave(&dev->notify_lock, flags);

	/* Check connected flag (mirrors kext this+0x2898 check) */
	if (!READ_ONCE(dev->connected)) {
		spin_unlock_irqrestore(&dev->notify_lock, flags);
		return;
	}

	status = uad2_read32(dev, notify_reg);
	if (!status) {
		spin_unlock_irqrestore(&dev->notify_lock, flags);
		return;
	}

	dev->notify_status = status;

	/* Bit 5: Connect/rate-change ack.
	 * The kext has two separate CUAOS::Event objects both signaled
	 * by bit 5: connect_event (this+0x2830) used during Connect(),
	 * and rate_event (this+0x2838) used during _setSampleClock().
	 * We signal both completions here — only the one being waited
	 * on will actually wake a thread. */
	if (status & NOTIFY_CONNECT_ACK) {
		complete(&dev->connect_event);
		complete(&dev->rate_event);
	}

	/* Bit 21: Channel config — kext copies 10 DWORDs from BAR+0xC000,
	 * then forces bits 0, 1, AND 22 on (ORs 0x400003) to trigger
	 * IO descriptor re-reads and rate change handling. */
	if (status & NOTIFY_CHAN_CONFIG) {
		int i;

		/* Copy 10 DWORDs of channel config from BAR+0xC000 */
		for (i = 0; i < CHAN_CONFIG_DWORDS; i++)
			dev->chan_config[i] = uad2_read32(
				dev, REG_CHAN_CONFIG_BASE + (i * 4));

		/* Force playback + record IO ready + rate change processing */
		status |= NOTIFY_PLAYBACK_IO | NOTIFY_RECORD_IO |
			  NOTIFY_RATE_CHANGE;
	}

	/* Bit 0: Playback IO ready — kext copies 72 DWORDs from BAR+0xC2C4 */
	if (status & NOTIFY_PLAYBACK_IO) {
		u32 play_ch;
		int i;

		/* Full IO descriptor copy (72 DWORDs = 288 bytes).
		 * Matches kext behavior of copying the entire block. */
		for (i = 0; i < IO_DESC_DWORDS; i++)
			dev->play_io_desc[i] = uad2_read32(
				dev, REG_PLAYBACK_IO_DESC + (i * 4));

		/* Channel count is at DWORD[4] (offset +0x10) */
		play_ch = dev->play_io_desc[4];
		if (play_ch > 0 && play_ch <= 128)
			WRITE_ONCE(dev->play_channels, play_ch);
	}

	/* Bit 1: Record IO ready — kext copies 72 DWORDs from BAR+0xC1A4 */
	if (status & NOTIFY_RECORD_IO) {
		u32 rec_ch;
		int i;

		/* Full IO descriptor copy */
		for (i = 0; i < IO_DESC_DWORDS; i++)
			dev->rec_io_desc[i] =
				uad2_read32(dev, REG_RECORD_IO_DESC + (i * 4));

		/* Channel count is at DWORD[4] */
		rec_ch = dev->rec_io_desc[4];
		if (rec_ch > 0 && rec_ch <= 128)
			WRITE_ONCE(dev->rec_channels, rec_ch);
	}

	/* Bit 22: Rate change — read rate and clock info.
	 * The firmware expects the host to read these registers as
	 * acknowledgment.  From open-apollo ua_audio.c:976-983. */
	if (status & NOTIFY_RATE_CHANGE) {
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
	}

	/* Bit 4: DMA ready — read transport and clock info */
	if (status & NOTIFY_DMA_READY) {
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_XPORT_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
	}

	/* Bit 6: Error */
	if (status & NOTIFY_ERROR) {
		dev_warn(&dev->pci->dev, "Firmware error notification\n");
	}

	/* Bit 21 (second pass): Signal notify_event — wakes Connect() */
	if (status & NOTIFY_CHAN_CONFIG) {
		complete(&dev->notify_event);
	}

	/* Clear the notification register (write-to-clear) */
	uad2_write32(dev, notify_reg, 0x0);

	spin_unlock_irqrestore(&dev->notify_lock, flags);
}

/* ============================================================
 * CPcieAudioExtension::Connect equivalent @ 0x4be58 (line 70632)
 *
 * Full handshake sequence decoded from lines 70632-70823:
 *
 * The kext rings the same doorbell up to 20 times but EXITS on the
 * first successful firmware response.  The doorbell address
 * (BAR + channel_base_index*4 + 0xC004) is constant — the 20-iteration
 * outer loop is a retry mechanism, not per-channel initialization.
 *
 * Per iteration:
 *   a. Write 0x0ACEFACE to BAR+(channel_base_index<<2)+0xC004
 *   b. Write 0x1 to BAR+0x2260 (stream enable doorbell)
 *   c. Wait on notify_event (100ms timeout, up to 10 retries)
 *   d. If timeout: manually call _handleNotificationInterrupt()
 *   e. If success: goto done (early exit)
 *
 * If all 20 attempts fail: is_connected stays false, return -92.
 * If any attempt succeeds: is_connected = true, return 0.
 *
 * The firmware responds by setting bits 5+21 in the notification
 * register, which the interrupt handler processes:
 *   bit 5 → signals connect_event
 *   bit 21 → copies channel config, signals notify_event
 * Connect() waits on notify_event (this+0x2830 in kext).
 * ============================================================ */
static int uad2_audio_connect(struct uad2_dev *dev)
{
	u32 doorbell_reg = uad2_fw_reg(dev, REG_FW_DOORBELL);
	int chan, retry;
	unsigned long timeout_jiffies;
	unsigned long flags;
	long ret;

	/* Set connected = true BEFORE the doorbell loop.
	 * The kext sets this+0x2898 (is_connected) = 1 at this point.
	 * The notification handler checks this flag and returns early
	 * if false, so it MUST be true for the connect handshake to
	 * receive firmware responses via the IRQ-driven path. */
	WRITE_ONCE(dev->connected, true);

	/* The kext's Connect loop rings the same doorbell up to 20 times
	 * (with 10 retries each) but EXITS on the first successful firmware
	 * response.  It is NOT iterating over 20 separate channels — the
	 * doorbell address (BAR + channel_base_index*4 + 0xC004) is the
	 * same every iteration.  The 20-channel loop is a retry mechanism,
	 * not a per-channel initialization.  If we reach chan==20 without
	 * any success, the kext considers the connection failed. */
	for (chan = 0; chan < UAD2_CONNECT_CHANNELS; chan++) {
		/* Reset completion before each doorbell */
		reinit_completion(&dev->notify_event);

		/* Kext holds hw_lock (this+0x10) around doorbell + stream
		 * enable writes to prevent concurrent register access */
		spin_lock_irqsave(&dev->lock, flags);

		/* Write DMA descriptor magic (doorbell command) */
		uad2_write32(dev, doorbell_reg, DMA_DESC_MAGIC);

		/* Write stream enable (doorbell to firmware) */
		uad2_write32(dev, REG_STREAM_ENABLE, 0x1);

		spin_unlock_irqrestore(&dev->lock, flags);

		/* Wait for firmware response via notification interrupt.
		 * Kext uses intr_timer->wait(100) with up to 10 retries.
		 * In between retries, it manually polls the notification
		 * register via _handleNotificationInterrupt(). */
		timeout_jiffies = msecs_to_jiffies(UAD2_CONNECT_WAIT_MS);

		for (retry = 0; retry < UAD2_CONNECT_RETRIES; retry++) {
			ret = wait_for_completion_timeout(&dev->notify_event,
							  timeout_jiffies);
			if (ret > 0)
				goto connect_done;

			/* Timeout: manually poll notification register
			 * (mirrors kext behavior of calling
			 *  _handleNotificationInterrupt on timeout) */
			uad2_handle_notification(dev);

			/* Check if notification arrived during manual poll */
			if (try_wait_for_completion(&dev->notify_event))
				goto connect_done;
		}
	}

	/* All 20 attempts exhausted without a single firmware response */
	dev_err(&dev->pci->dev,
		"Connect failed: no response after %d attempts x %d retries\n",
		UAD2_CONNECT_CHANNELS, UAD2_CONNECT_RETRIES);
	WRITE_ONCE(dev->connected, false);
	return -ETIMEDOUT;

connect_done:
	dev_info(&dev->pci->dev, "Connect succeeded on attempt %d (retry %d)\n",
		 chan, retry);

	/* Recompute buffer frame size (channels set by uad2_detect_device()
	 * in probe, or updated from IO descriptor notifications above) */
	dev->buffer_frames = uad2_compute_buffer_frames(dev->play_channels,
							dev->rec_channels);

	dev_info(&dev->pci->dev,
		 "Audio connected: play=%u rec=%u buffer_frames=%u\n",
		 dev->play_channels, dev->rec_channels, dev->buffer_frames);

	/* Synthetic notification reads — the kext's notification handler
	 * synthetically injects bits 0+1+22 after connect (ORs 0x400003)
	 * to force the host to read IO descriptors and rate/clock info.
	 * The firmware state machine expects these reads as acknowledgment
	 * before advancing to active audio routing.
	 * From open-apollo ua_audio.c:946-993. */
	{
		u32 rate_info =
			uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
		u32 clock_info = uad2_read32(
			dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
		u32 xport_info = uad2_read32(
			dev, uad2_fw_reg(dev, REG_NOTIF_XPORT_INFO));

		dev_info(
			&dev->pci->dev,
			"post-connect: rate=0x%08x clock=0x%08x xport=0x%08x\n",
			rate_info, clock_info, xport_info);
	}

	/* Initialize mixer batch protocol and arm capture routing */
	uad2_mixer_init(dev);

	/* Start periodic DSP service loop (readback drain + mixer flush) */
	uad2_dsp_service_start(dev);

	return 0;
}

/* ============================================================
 * CPcieDevice::ProgramRegisters equivalent @ 0xdf48
 * Top-level hardware init sequence -- exact order from RE
 * ============================================================ */
static int uad2_hw_program(struct uad2_dev *dev)
{
	u32 device_id;
	int i, err;

	/* 1. Read and verify device ID register */
	device_id = uad2_read32(dev, REG_DEVICE_ID);
	if (device_id != dev->expected_device_id) {
		dev_err(&dev->pci->dev,
			"Device ID mismatch: got 0x%08x expected 0x%08x\n",
			device_id, dev->expected_device_id);
		return -ENODEV;
	}

	/* 2. Acknowledge device ID (kext WriteRegEjj: always writes 0 to 0x2220) */
	uad2_write32(dev, REG_DEVICE_HANDSHAKE, 0);

	/* 3. Program interrupt controller registers */
	uad2_intr_program(dev);

	/* 4. Reset DMA engines */
	uad2_dma_reset(dev);

	/* 5. Arm DMA interrupt */
	uad2_write32(dev, REG_DMA0_STATUS, 0xffffffff);

	/* 6. Program each DSP (Apollo Solo: 1 DSP, index 0) */
	for (i = 0; i < dev->dsp_count; i++) {
		err = uad2_dsp_program(dev, i);
		if (err) {
			dev_err(&dev->pci->dev, "DSP %d init failed: %d\n", i,
				err);
			return err;
		}
	}

	/* 7. Program audio extension: SG tables + firmware base + interrupts */
	err = uad2_audio_ext_program(dev);
	if (err) {
		dev_err(&dev->pci->dev, "Audio extension init failed: %d\n",
			err);
		return err;
	}

	/* 8. Firmware Connect handshake (20-channel doorbell) */
	err = uad2_audio_connect(dev);
	if (err) {
		dev_err(&dev->pci->dev, "Audio connect failed: %d\n", err);
		return err;
	}

	return 0;
}

/* ============================================================
 * CPcieAudioExtension::PrepareTransport equivalent
 *
 * Programs all audio transport registers before starting DMA.
 * Must be called from pcm_prepare with runtime parameters.
 *
 * Full sequence from lines 71623-71903 (refined with new analysis):
 *   1.  Check transport_state != 2 (not already running)
 *   2.  Check is_connected == 1
 *   3.  Validate bufferFrameSize < 0x10000
 *   4.  Lock hw_lock
 *   5.  Write BAR+0x2240 = bufferFrameSize - 1
 *   6.  Write BAR+0x226C = (actualPlayChans * (bufSz-1)) >> 10
 *   7.  Write BAR+0x2244 = 0  (clear position)
 *   8.  If periodic_timer_interval != 0:
 *         Write BAR+0x2270 = periodic_timer_interval
 *         EnableVector(0x46, 1)  — enable periodic timer interrupt
 *   9.  Write BAR+0x2244 = 0  (clear again)
 *   10. Read  BAR+0x2244       (flush)
 *   11. Write BAR+0x2248 = 0   (stop/reset)
 *   12. Write BAR+0x2258 = irqPeriod
 *   13. Write BAR+0x2250 = actualPlayChans
 *   14. Write BAR+0x225C = actualRecChans
 *   15. Write BAR+0x2248 = 1   (arm)
 *   16. Read  BAR+0x22C0       (monitor flush)
 *   17. If diagnostic_flags bit1: Write BAR+0x224C = (actualPlayChans-1)|0x100
 *   18. Unlock hw_lock
 *   19. Set transport_state = 1 (prepared)
 *   20. Poll  BAR+0x2254 until == irqPeriod (DMA ready, max 2 retries)
 *   21. EnableVector(0x47, 1)  — enable end-of-buffer interrupt
 * ============================================================ */
static void uad2_stop_transport(struct uad2_dev *dev);

static int uad2_prepare_transport(struct uad2_dev *dev,
				  unsigned int buffer_frames,
				  unsigned int irq_period_frames,
				  unsigned int play_channels,
				  unsigned int rec_channels)
{
	u32 buf_size_kb;
	unsigned long flags;
	int i;

	/* Validate: must be connected */
	if (!READ_ONCE(dev->connected)) {
		dev_err(&dev->pci->dev, "PrepareTransport: not connected\n");
		return -ENODEV;
	}

	/* Safety net: if transport is running, stop it before re-preparing.
	 * This path should not be reached during normal ALSA operation
	 * because uad2_pcm_prepare() uses the fast-path (position offset)
	 * when transport_state >= 1.  Kept for direct callers like sample
	 * rate change while running. */
	if (READ_ONCE(dev->transport_state) == 2)
		uad2_stop_transport(dev);
	if (buffer_frames >= 0x10000) {
		dev_err(&dev->pci->dev,
			"PrepareTransport: buffer_frames 0x%x out of range\n",
			buffer_frames);
		return -EINVAL;
	}

	spin_lock_irqsave(&dev->lock, flags);

	/* 5. Buffer frame size (hardware uses size-1 as mask) */
	uad2_write32(dev, REG_BUFFER_FRAME_SIZE, buffer_frames - 1);

	/* 6. Buffer size in KB */
	buf_size_kb = (play_channels * (buffer_frames - 1)) >> 10;
	uad2_write32(dev, REG_BUFFER_SIZE_KB, buf_size_kb);

	/* 7. Clear DMA position counter */
	uad2_write32(dev, REG_DMA_POSITION, 0);
	dev->play_pos_offset = 0;
	dev->rec_pos_offset = 0;

	/* 8. Periodic timer (if configured) */
	if (dev->periodic_timer_interval) {
		uad2_write32(dev, REG_PERIODIC_TIMER,
			     dev->periodic_timer_interval);
		__uad2_enable_vector_locked(dev, INTR_SLOT_PERIODIC, true);
	}

	/* 9-10. Clear position again + read-back flush */
	uad2_write32(dev, REG_DMA_POSITION, 0);
	uad2_read32(dev, REG_DMA_POSITION);

	/* 11. Stop/reset transport */
	uad2_write32(dev, REG_TRANSPORT_CTL, 0);

	/* 12. Interrupt period */
	uad2_write32(dev, REG_IRQ_PERIOD, irq_period_frames);

	/* 13. Playback channel count */
	uad2_write32(dev, REG_PLAYBACK_CHAN_CNT, play_channels);

	/* 14. Record channel count */
	uad2_write32(dev, REG_RECORD_CHAN_CNT, rec_channels);

	/* 15. Arm transport (prepared state) */
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x1);

	/* 16. Read playback monitor status (fence read) */
	uad2_read32(dev, REG_PLAYBACK_MON_STAT);

	/* 17. P2P_ROUTE — tells FPGA how to route audio between DSP and DMA.
	 * Open-apollo writes this for all models AFTER DMA enable + fence.
	 * Value: 0x100 | (play_channels - 1).
	 * Previous code skipped this; open-apollo proved it's needed. */
	uad2_write32(dev, REG_PLAYBACK_MON_CFG, 0x100 | (play_channels - 1));

	spin_unlock_irqrestore(&dev->lock, flags);

	/* 19. Mark transport as prepared */
	WRITE_ONCE(dev->transport_state, 1);

	/* 20. Poll BAR+0x2254 until DMA is ready (compare vs irq_period)
	 * Kext does max 2 iterations with 1ms sleep between */
	for (i = 0; i < 3; i++) {
		u32 poll_val = uad2_read32(dev, REG_POLL_STATUS);

		if (poll_val == irq_period_frames)
			break;
		usleep_range(1000, 2000);
	}

	/* 21. Enable end-of-buffer interrupt vector */
	uad2_enable_vector(dev, INTR_SLOT_ENDBUF, true);

	return 0;
}

/* ============================================================
 * CPcieAudioExtension::StartTransport equivalent @ line 71904
 *
 * Sequence (from disasm lines 71904-71986):
 *   1. Check transport_state == 1 (prepared)
 *   2. Check is_connected
 *   3. Lock hw_lock
 *   4. Check device variant (this+0x28B0):
 *      - variant 0xA → always use 0x20F
 *      - variant 0x9 → use 0x20F if extended_mode_flag (this+0x22EF4) is zero (cbz)
 *      - otherwise   → use 0xF
 *   5. Write BAR+0x2248 = start_value
 *   6. Unlock hw_lock
 *   7. Set transport_state = 2 (running)
 *
 * v2 firmware devices use extended mode (0x20F, BIT 9 = EXT_MODE).
 * v1 firmware uses normal start (0xF).
 * ============================================================ */
static void uad2_start_transport(struct uad2_dev *dev)
{
	unsigned long flags;
	u32 start_val;

	if (READ_ONCE(dev->transport_state) == 2)
		return;
	if (READ_ONCE(dev->transport_state) != 1) {
		dev_warn(&dev->pci->dev,
			 "StartTransport: not prepared (state=%d)\n",
			 READ_ONCE(dev->transport_state));
		return;
	}
	if (!READ_ONCE(dev->connected)) {
		dev_warn(&dev->pci->dev, "StartTransport: not connected\n");
		return;
	}

	/* v2 firmware (all current Apollo TB devices) requires extended
	 * mode bit 9 set.  Without it, DSP processing may not activate.
	 * From open-apollo: 0x20F = DMA+play+rec+IRQ+extended mode. */
	start_val = dev->fw_v2 ? 0x20F : 0xF;

	spin_lock_irqsave(&dev->lock, flags);
	uad2_write32(dev, REG_TRANSPORT_CTL, start_val);
	WRITE_ONCE(dev->transport_state, 2);
	spin_unlock_irqrestore(&dev->lock, flags);
}

/* ============================================================
 * CPcieAudioExtension::StopTransport equivalent @ line 71987
 *
 * Sequence (from disasm lines 71987-72056):
 *   1. Check is_connected
 *   2. DisableVector(0x47)  — disable end-of-buffer interrupt
 *   3. Lock hw_lock
 *   4. Write BAR+0x2248 = 0 (stop transport)
 *   5. Unlock hw_lock
 *   6. Set transport_state = 0
 * ============================================================ */
static void uad2_stop_transport(struct uad2_dev *dev)
{
	unsigned long flags;

	/* Disable end-of-buffer interrupt before stopping (kext behavior) */
	uad2_disable_vector(dev, INTR_SLOT_ENDBUF);

	/* Disable periodic timer interrupt — no more period-elapsed callbacks
	 * should fire after the transport is stopped. */
	uad2_disable_vector(dev, INTR_SLOT_PERIODIC);

	spin_lock_irqsave(&dev->lock, flags);
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);
	WRITE_ONCE(dev->transport_state, 0);
	spin_unlock_irqrestore(&dev->lock, flags);
}

/* ============================================================
 * CPcieAudioExtension::Shutdown equivalent @ 0x4cb0c (line 71447)
 *
 * Full sequence (decoded from lines 71447-71537):
 *   1. Lock hw_lock
 *   2. Write BAR+0x2248 = 0  (stop transport)
 *   3. Write BAR+(channel_base_index<<2)+0xC004 = 0  (clear doorbell)
 *   4. Read  BAR+0x2248       (flush / status read)
 *   5. Write BAR+0x22C4 = 0   (clear secondary counter)
 *   6. Unlock hw_lock
 *   7. DisableVector(0x28)    (disable notification interrupt)
 *   8. Tail-call _disconnect():
 *      a. Write BAR+0x2260 = 0x10  (disconnect command)
 *      b. Set is_connected = 0
 * ============================================================ */
static void uad2_shutdown(struct uad2_dev *dev)
{
	unsigned long flags;

	/* Stop polling timer and DSP service loop */
	uad2_period_timer_stop(dev);
	uad2_dsp_service_stop(dev);
	dev->mixer_ready = false;

	/* 1-6: Stop transport and clear doorbell under lock */
	spin_lock_irqsave(&dev->lock, flags);

	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);
	uad2_write32(dev, uad2_fw_reg(dev, REG_FW_DOORBELL), 0x0);
	uad2_read32(dev, REG_TRANSPORT_CTL); /* flush */
	uad2_write32(dev, REG_SECONDARY_CTL, 0x0);

	spin_unlock_irqrestore(&dev->lock, flags);

	/* 7: Disable notification interrupt */
	uad2_disable_vector(dev, INTR_SLOT_NOTIFY);

	/* 8: Disconnect */
	uad2_write32(dev, REG_STREAM_ENABLE, 0x10);
	WRITE_ONCE(dev->connected, false);
	WRITE_ONCE(dev->transport_state, 0);
	WRITE_ONCE(dev->streams_running, 0);
	WRITE_ONCE(dev->streams_prepared, 0);
	WRITE_ONCE(dev->open_count, 0);
}

/* ============================================================
 * ALSA PCM hardware definitions
 *
 * From ioreg IOAudioEngine (confirmed on live hardware):
 *   Format: 32-bit container, 24-bit depth, signed int, LE
 *   (IOAudioStreamByteOrder=1 on macOS = little-endian)
 *
 * Buffer structure: interleaved, all channels × buffer_frames × 4 bytes
 * The ALSA PCM buffer is mapped directly into the 4MB hardware DMA buffer.
 * ============================================================ */
/* The hardware DMA is completely rigid:
 *   - Buffer = 8192 frames (always, computed by _recomputeBufferFrameSize)
 *   - Periodic timer interval varies by rate:
 *     256 frames @ 44.1/48kHz, 512 @ 88.2/96kHz, 1024 @ 176.4/192kHz
 *   - All channels interleaved, S32_LE, fixed channel count
 *
 * The periodic timer (vector 0x46) fires snd_pcm_period_elapsed() at
 * the interval above.  ALSA's period size MUST match this interval for
 * correct buffer position accounting.
 *
 * Since the period varies by sample rate but ALSA constraints are set
 * at open time (before the rate is known), we allow a range of period
 * sizes that covers all supported rates.  A constraint rule in pcm_open
 * ties the exact period to the selected rate.
 *
 * Period sizes: 256 (1x rates), 512 (2x rates), 1024 (4x rates)
 * Periods:      32, 16, 8 respectively (8192 / period_size)
 *
 * Playback: 256 frames × 42ch × 4B = 43008 bytes/period (min)
 *           1024 frames × 42ch × 4B = 172032 bytes/period (max)
 * Capture:  256 frames × 32ch × 4B = 32768 bytes/period (min)
 *           1024 frames × 32ch × 4B = 131072 bytes/period (max)
 *
 * SNDRV_PCM_INFO_BATCH: The hardware DMA position register only updates
 * at period boundaries (every 256 frames at 48kHz = 5.33ms).  Between
 * interrupts, .pointer() returns a stale position.  PipeWire's rate
 * estimator and ALSA's internal hw_ptr tracking need to know this to
 * avoid declaring spurious XRUNs when the position appears to jump by
 * a full period at once.  The BATCH flag tells them the position is
 * updated in coarse batches, not smoothly.
 */
#define UAD2_PERIOD_FRAMES_1X 256 /* period at 44.1/48 kHz */
#define UAD2_PERIOD_FRAMES_2X 512 /* period at 88.2/96 kHz */
#define UAD2_PERIOD_FRAMES_4X 1024 /* period at 176.4/192 kHz */

static const struct snd_pcm_hardware uad2_pcm_hw_playback = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_JOINT_DUPLEX | SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S32_LE, /* 24-in-32 LE, MSB-justified */
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = UAD2_MAX_CHANNELS,
	.buffer_bytes_max = UAD2_MAX_BUFFER_FRAMES * UAD2_MAX_CHANNELS *
			    UAD2_BYTES_PER_SAMPLE,
	.period_bytes_min = UAD2_PERIOD_FRAMES_1X * 2 * UAD2_BYTES_PER_SAMPLE,
	.period_bytes_max = UAD2_PERIOD_FRAMES_4X * UAD2_MAX_CHANNELS *
			    UAD2_BYTES_PER_SAMPLE,
	.periods_min = UAD2_MAX_BUFFER_FRAMES / UAD2_PERIOD_FRAMES_4X,
	.periods_max = UAD2_MAX_BUFFER_FRAMES / UAD2_PERIOD_FRAMES_1X,
};

static const struct snd_pcm_hardware uad2_pcm_hw_capture = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER |
		SNDRV_PCM_INFO_JOINT_DUPLEX | SNDRV_PCM_INFO_BATCH,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = UAD2_MAX_CHANNELS,
	.buffer_bytes_max = UAD2_MAX_BUFFER_FRAMES * UAD2_MAX_CHANNELS *
			    UAD2_BYTES_PER_SAMPLE,
	.period_bytes_min = UAD2_PERIOD_FRAMES_1X * 2 * UAD2_BYTES_PER_SAMPLE,
	.period_bytes_max = UAD2_PERIOD_FRAMES_4X * UAD2_MAX_CHANNELS *
			    UAD2_BYTES_PER_SAMPLE,
	.periods_min = UAD2_MAX_BUFFER_FRAMES / UAD2_PERIOD_FRAMES_4X,
	.periods_max = UAD2_MAX_BUFFER_FRAMES / UAD2_PERIOD_FRAMES_1X,
};

/* ============================================================
 * PCM operations
 *
 * Key design: the ALSA DMA buffer is mapped directly into
 * the hardware's 4MB scatter-gather buffer.  ALSA's
 * snd_pcm_lib_malloc_pages is NOT used; instead, we set
 * runtime->dma_area/dma_addr/dma_bytes to point into our
 * pre-allocated 4MB buffer.  This avoids a copy path.
 * ============================================================ */

/*
 * Constraint rule: tie ALSA period_size to sample rate.
 *
 * The hardware's periodic timer fires at rate-dependent intervals:
 *   44100/48000 Hz   → every 256 frames
 *   88200/96000 Hz   → every 512 frames
 *   176400/192000 Hz → every 1024 frames
 *
 * ALSA must use the matching period_size so that snd_pcm_period_elapsed()
 * accounting stays in sync with the hardware interrupt cadence.
 */
static const unsigned int uad2_rate_list[] = {
	44100, 48000, 88200, 96000, 176400, 192000,
};
static const unsigned int uad2_period_for_rate[] = {
	256, 256, 512, 512, 1024, 1024,
};

static int uad2_rule_period_size(struct snd_pcm_hw_params *params,
				 struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *period =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	struct snd_interval *rate =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval new_period;
	unsigned int min_p = UINT_MAX, max_p = 0;
	int i;

	/* Find the range of valid period sizes for the current rate range */
	for (i = 0; i < ARRAY_SIZE(uad2_rate_list); i++) {
		if (uad2_rate_list[i] >= rate->min &&
		    uad2_rate_list[i] <= rate->max) {
			if (uad2_period_for_rate[i] < min_p)
				min_p = uad2_period_for_rate[i];
			if (uad2_period_for_rate[i] > max_p)
				max_p = uad2_period_for_rate[i];
		}
	}

	if (min_p > max_p)
		return -EINVAL;

	snd_interval_any(&new_period);
	new_period.min = min_p;
	new_period.max = max_p;

	return snd_interval_refine(period, &new_period);
}

/*
 * Constraint rule: buffer_size must equal the hardware buffer (8192 frames).
 *
 * The hardware DMA uses a fixed 8192-frame ring buffer. The DMA position
 * counter (REG_DMA_POSITION) wraps at 8192, not at ALSA's buffer_size.
 * If ALSA picks a smaller buffer_size, the pointer callback returns values
 * beyond buffer_size, causing -EIO errors.
 */
static int uad2_rule_buffer_size(struct snd_pcm_hw_params *params,
				 struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *buffer =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);
	struct snd_interval fixed;

	snd_interval_any(&fixed);
	fixed.min = UAD2_MAX_BUFFER_FRAMES;
	fixed.max = UAD2_MAX_BUFFER_FRAMES;

	return snd_interval_refine(buffer, &fixed);
}

/*
 * Constraint rule: periods = 8192 / period_size.
 *
 * Since buffer_size is fixed at 8192 and period_size depends on rate,
 * the number of periods must adjust accordingly:
 *   44100/48000 Hz  → period=256  → periods=32
 *   88200/96000 Hz  → period=512  → periods=16
 *   176400/192000 Hz → period=1024 → periods=8
 */
static int uad2_rule_periods(struct snd_pcm_hw_params *params,
			     struct snd_pcm_hw_rule *rule)
{
	struct snd_interval *periods =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIODS);
	struct snd_interval *period_size =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
	struct snd_interval new_periods;
	unsigned int min_periods, max_periods;

	/* periods = buffer_frames / period_size
	 * When period_size is large, periods is small and vice versa */
	min_periods = UAD2_MAX_BUFFER_FRAMES / period_size->max;
	max_periods = UAD2_MAX_BUFFER_FRAMES / period_size->min;

	snd_interval_any(&new_periods);
	new_periods.min = min_periods;
	new_periods.max = max_periods;

	return snd_interval_refine(periods, &new_periods);
}

static int uad2_pcm_open(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int err;

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ss->runtime->hw = uad2_pcm_hw_playback;
		/* Set actual channel count from device detection */
		ss->runtime->hw.channels_min = dev->play_channels;
		ss->runtime->hw.channels_max = dev->play_channels;
		ss->runtime->hw.buffer_bytes_max = UAD2_MAX_BUFFER_FRAMES *
						   dev->play_channels *
						   UAD2_BYTES_PER_SAMPLE;
		spin_lock_irqsave(&dev->lock, flags);
		dev->playback_ss = ss;
		dev->open_count++;
		spin_unlock_irqrestore(&dev->lock, flags);
	} else {
		ss->runtime->hw = uad2_pcm_hw_capture;
		ss->runtime->hw.channels_min = dev->rec_channels;
		ss->runtime->hw.channels_max = dev->rec_channels;
		ss->runtime->hw.buffer_bytes_max = UAD2_MAX_BUFFER_FRAMES *
						   dev->rec_channels *
						   UAD2_BYTES_PER_SAMPLE;
		spin_lock_irqsave(&dev->lock, flags);
		dev->capture_ss = ss;
		dev->open_count++;
		spin_unlock_irqrestore(&dev->lock, flags);
	}

	/* Tell ALSA that playback and capture share a clock source.
	 * This sets matching sync IDs so userspace (PipeWire/PulseAudio)
	 * knows the streams are phase-locked. */
	snd_pcm_set_sync(ss);

	/* Add constraint rule: period_size must match the hardware's
	 * periodic timer interval for the selected sample rate.
	 * This prevents ALSA from choosing a period_size that doesn't
	 * match the actual interrupt cadence. */
	err = snd_pcm_hw_rule_add(ss->runtime, 0,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE,
				  uad2_rule_period_size, NULL,
				  SNDRV_PCM_HW_PARAM_RATE, -1);
	if (err < 0)
		return err;

	/* Add constraint rule: buffer_size must be exactly 8192 frames.
	 * The hardware DMA ring buffer is fixed at 8192 frames and the
	 * position counter wraps at that boundary. */
	err = snd_pcm_hw_rule_add(ss->runtime, 0,
				  SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				  uad2_rule_buffer_size, NULL, -1);
	if (err < 0)
		return err;

	/* Add constraint rule: periods = 8192 / period_size, since both
	 * buffer_size and period_size are fixed by hardware. */
	err = snd_pcm_hw_rule_add(ss->runtime, 0, SNDRV_PCM_HW_PARAM_PERIODS,
				  uad2_rule_periods, NULL,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
	if (err < 0)
		return err;

	return 0;
}

static int uad2_pcm_close(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;
	int open_count;

	spin_lock_irqsave(&dev->lock, flags);
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dev->playback_ss = NULL;
	else
		dev->capture_ss = NULL;
	dev->open_count--;
	if (dev->open_count < 0)
		dev->open_count = 0;
	open_count = dev->open_count;
	spin_unlock_irqrestore(&dev->lock, flags);

	/* Following the snd-dice/snd-bebob pattern: only tear down the
	 * shared transport when ALL substreams have been closed.
	 *
	 * This is the key fix for the PipeWire restart loop.  Previously,
	 * hw_free would tear down the transport whenever streams_prepared
	 * and streams_running both reached 0.  During PipeWire graph
	 * reconfiguration (e.g., opening a second app), there's a brief
	 * window where PipeWire stops and hw_free's both streams before
	 * immediately re-preparing them.  That brief tear-down caused:
	 *   1. Full DMA re-initialization (slow, 30-40s delays)
	 *   2. Stale buffer data replayed ("DJ looping" effect)
	 *
	 * Now the transport stays alive as long as any substream is open.
	 * PipeWire keeps substreams open during reconfiguration, so the
	 * transport persists through stop→hw_free→prepare→start cycles. */
	if (open_count == 0) {
		int state = READ_ONCE(dev->transport_state);

		if (state == 2) {
			uad2_stop_transport(dev);
		} else if (state == 1) {
			WRITE_ONCE(dev->transport_state, 0);
		}
	}

	return 0;
}

/*
 * hw_params: Map the ALSA PCM buffer directly into our pre-allocated
 * 4MB DMA buffer.  No separate DMA allocation needed since the hardware
 * scatter-gather tables already point to this memory.
 */
static int uad2_pcm_hw_params(struct snd_pcm_substream *ss,
			      struct snd_pcm_hw_params *params)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	int buf_idx = (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0 : 1;
	size_t buf_bytes = params_buffer_bytes(params);

	/* Ensure requested buffer fits within our 4MB DMA buffer */
	if (buf_bytes > dev->sg_buf_size) {
		dev_err(&dev->pci->dev,
			"Requested buffer %zu exceeds HW buffer %zu\n",
			buf_bytes, dev->sg_buf_size);
		return -EINVAL;
	}

	/* Point ALSA runtime directly at our pre-allocated DMA buffer */
	runtime->dma_area = dev->sg_dma_buf[buf_idx];
	runtime->dma_addr = dev->sg_dma_addr[buf_idx];
	runtime->dma_bytes = buf_bytes;

	return 0;
}

static int uad2_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	unsigned long flags;

	/* Don't actually free — the DMA buffer belongs to the device.
	 * Just clear the runtime pointers. */
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;

	/* Decrement the prepared-streams reference count if this substream
	 * was counted (pcm_prepare was called).  The private_data flag is
	 * set by pcm_prepare and cleared here to handle the case where
	 * pcm_prepare is called multiple times without intervening hw_free.
	 *
	 * NOTE: We do NOT tear down the transport here.  Following the
	 * snd-dice/snd-bebob pattern, transport lifecycle is tied to
	 * pcm_open/pcm_close (via open_count), not to hw_params/hw_free.
	 * This prevents the pathological PipeWire reconfiguration cycle
	 * where a brief hw_free→prepare sequence would cause a full
	 * transport teardown and re-initialization. */
	spin_lock_irqsave(&dev->lock, flags);
	if (runtime->private_data) {
		runtime->private_data = NULL;
		dev->streams_prepared--;
		if (dev->streams_prepared < 0)
			dev->streams_prepared = 0;
	}
	spin_unlock_irqrestore(&dev->lock, flags);

	return 0;
}

/*
 * pcm_prepare: program sample rate + transport registers
 *
 * Implements the combined sequence of:
 *   _setSampleClock()  -- set hardware clock
 *   PrepareTransport() -- program buffer/channel/interrupt registers
 *
 * IMPORTANT: The hardware has a single shared transport for both playback
 * and capture.  The DMA buffer layout is fixed at:
 *   buffer_frames × max(play_channels, rec_channels) × 4 bytes
 * and is always fully interleaved.  Transport registers must be programmed
 * with the hardware's buffer_frames (8192 for Apollo Solo), NOT the ALSA
 * runtime's buffer_size (which is in frames from ALSA's perspective and
 * can be much smaller, e.g. 96 frames, causing the hardware to malfunction).
 *
 * When both streams are open, the second stream's prepare must NOT
 * tear down a running transport.  If transport is already prepared
 * or running with compatible parameters, we skip re-preparation.
 */
static int uad2_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int buffer_frames, irq_period_frames;
	unsigned long flags;
	int err;

	/* Rate change detection: if the requested rate differs from the
	 * current rate AND the transport is running, we must stop the
	 * transport, change the clock, and do a full re-prepare with
	 * rate-correct parameters (IRQ period, periodic timer).
	 *
	 * Previously this just called set_sample_rate() without stopping
	 * the transport, leaving IRQ period and periodic timer at stale
	 * values.  The firmware zeros the internal mixer on rate change,
	 * so a full restart is required anyway.
	 *
	 * Discovery: open-apollo does a full disconnect/reconnect cycle.
	 * We do the minimum: stop transport → set rate → cold prepare. */
	if (dev->current_rate != rt->rate) {
		if (READ_ONCE(dev->transport_state) >= 1) {
			dev_info(&dev->pci->dev,
				 "pcm_prepare: rate change %u → %u, "
				 "stopping transport for re-prepare\n",
				 dev->current_rate, rt->rate);
			uad2_stop_transport(dev);
		}

		err = uad2_set_sample_rate(dev, rt->rate);
		if (err)
			dev_warn(
				&dev->pci->dev,
				"Sample rate set to %u may not have completed\n",
				rt->rate);
		/* Fall through to cold-start path (transport_state == 0) */
	}

	/* Track that this substream has been prepared. */
	if (!rt->private_data) {
		spin_lock_irqsave(&dev->lock, flags);
		dev->streams_prepared++;
		spin_unlock_irqrestore(&dev->lock, flags);
		rt->private_data = dev; /* mark as counted */
	}

	/* Hardware buffer frame size (fixed per model) */
	buffer_frames = dev->buffer_frames;

	/* IRQ period and periodic timer from rate-based lookup tables */
	irq_period_frames = uad2_irq_period_for_rate(rt->rate);
	dev->irq_period_frames = irq_period_frames;
	dev->periodic_timer_interval = uad2_periodic_timer_for_rate(rt->rate);

	/* Transport re-preparation strategy:
	 *
	 * Same-rate re-prepare (transport already running): use the
	 * position-offset approach.  Snapshot DMA position so .pointer()
	 * returns 0-relative values matching ALSA's reset hw_ptr.
	 *
	 * Rate change or cold-start (transport_state == 0): do the full
	 * prepare_transport sequence which resets DMA position to 0. */
	if (READ_ONCE(dev->transport_state) >= 1) {
		/* Transport already running at correct rate — fast path.
		 * Set per-direction offset so this stream's .pointer()
		 * returns 0-relative without affecting the other direction. */
		u32 cur_pos = uad2_read32(dev, REG_DMA_POSITION);

		if (cur_pos >= dev->buffer_frames)
			cur_pos = 0;
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dev->play_pos_offset = cur_pos;
		else
			dev->rec_pos_offset = cur_pos;
		return 0;
	}

	/* Cold start (or post-rate-change) — full transport preparation.
	 * Always program the full hardware channel counts — the DMA
	 * layout is fixed at all channels interleaved. */
	dev->play_pos_offset = 0;
	dev->rec_pos_offset = 0;
	/* Prepare transport only — do NOT start it here.
	 * ALSA expects transport to start in trigger(START).
	 * Starting here causes the DMA position to advance before
	 * ALSA's hw_ptr is initialized, resulting in XRUN on first read.
	 * The post-transport clock write also moves to trigger. */
	return uad2_prepare_transport(dev, buffer_frames, irq_period_frames,
				      READ_ONCE(dev->play_channels),
				      READ_ONCE(dev->rec_channels));
}

/* ============================================================
 * Period elapsed polling timer (hrtimer)
 *
 * The Apollo's hardware periodic timer IRQ (vector 0x46) is unreliable
 * on most models — fires once then stops.  We use an hrtimer at ~1ms
 * to poll REG_DMA_POSITION and call snd_pcm_period_elapsed() when a
 * period boundary is crossed.  Same approach as snd-usb-audio and
 * open-apollo's period_timer.
 * ============================================================ */
#define PERIOD_TIMER_NS 1000000 /* 1 ms polling interval */

static enum hrtimer_restart uad2_period_timer_fn(struct hrtimer *timer)
{
	struct uad2_dev *dev =
		container_of(timer, struct uad2_dev, period_timer);
	struct snd_pcm_substream *play_ss, *cap_ss;
	bool play_run, cap_run;
	unsigned long flags;
	u32 pos;
	snd_pcm_uframes_t cur_period;

	if (!dev->period_timer_running || READ_ONCE(dev->disconnecting))
		return HRTIMER_NORESTART;

	pos = uad2_read32(dev, REG_DMA_POSITION);
	if (pos == 0xFFFFFFFF) {
		dev->period_timer_running = false;
		return HRTIMER_NORESTART;
	}
	if (pos >= dev->buffer_frames)
		pos = 0;

	spin_lock_irqsave(&dev->lock, flags);
	play_ss = dev->playback_ss;
	cap_ss = dev->capture_ss;
	play_run = dev->playback_running;
	cap_run = dev->capture_running;
	spin_unlock_irqrestore(&dev->lock, flags);

	/* Check playback period boundary */
	if (play_ss && play_run) {
		u32 play_off = READ_ONCE(dev->play_pos_offset);
		u32 adj = (pos - play_off + dev->buffer_frames) %
			  dev->buffer_frames;
		snd_pcm_uframes_t period_size =
			play_ss->runtime ? play_ss->runtime->period_size : 256;

		cur_period = (period_size > 0) ? adj / period_size : 0;
		if (cur_period != dev->last_play_period) {
			dev->last_play_period = cur_period;
			snd_pcm_period_elapsed(play_ss);
		}
	}

	/* Check capture period boundary */
	if (cap_ss && cap_run) {
		u32 rec_off = READ_ONCE(dev->rec_pos_offset);
		u32 adj = (pos - rec_off + dev->buffer_frames) %
			  dev->buffer_frames;
		snd_pcm_uframes_t period_size =
			cap_ss->runtime ? cap_ss->runtime->period_size : 256;

		cur_period = (period_size > 0) ? adj / period_size : 0;
		if (cur_period != dev->last_rec_period) {
			dev->last_rec_period = cur_period;
			snd_pcm_period_elapsed(cap_ss);
		}
	}

	hrtimer_forward_now(timer, ns_to_ktime(PERIOD_TIMER_NS));
	return HRTIMER_RESTART;
}

static void uad2_period_timer_start(struct uad2_dev *dev)
{
	if (dev->period_timer_running)
		return;

	dev->period_timer_running = true;
	dev->last_play_period = 0;
	dev->last_rec_period = 0;
	hrtimer_start(&dev->period_timer, ns_to_ktime(PERIOD_TIMER_NS),
		      HRTIMER_MODE_REL);
}

static void uad2_period_timer_stop(struct uad2_dev *dev)
{
	if (!dev->period_timer_running)
		return;

	dev->period_timer_running = false;
	hrtimer_cancel(&dev->period_timer);
}

static int uad2_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock_irqsave(&dev->lock, flags);
		dev->streams_running++;
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dev->playback_running = true;
		else
			dev->capture_running = true;
		spin_unlock_irqrestore(&dev->lock, flags);
		uad2_start_transport(dev);

		/* Post-transport clock write: source=0xC enables DSP active
		 * processing.  Only do this on the FIRST stream start
		 * (streams_running==1), not on subsequent piggybacks.
		 * Open-apollo: clock write after transport start activates
		 * the mixer DSP for capture routing. */
		if (READ_ONCE(dev->streams_running) == 1) {
			u32 clock_cfg =
				0xC | ((u32)uad2_rate_to_enum(dev->current_rate)
				       << 8);
			uad2_write32(dev, uad2_fw_reg(dev, REG_SAMPLE_CLOCK),
				     clock_cfg);
			uad2_write32(dev, REG_STREAM_ENABLE, 0x4);
		}

		/* Start period-elapsed polling timer — the hardware
		 * periodic timer IRQ is unreliable on Apollo devices. */
		uad2_period_timer_start(dev);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		spin_lock_irqsave(&dev->lock, flags);
		dev->streams_running--;
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dev->playback_running = false;
		else
			dev->capture_running = false;
		if (dev->streams_running < 0)
			dev->streams_running = 0;
		spin_unlock_irqrestore(&dev->lock, flags);

		/* NOTE: We do NOT stop the hardware transport here.
		 *
		 * Following the snd-dice/snd-bebob pattern, the DMA
		 * transport runs continuously once started and only stops
		 * when the last substream closes (pcm_close).  This is
		 * critical for PipeWire which rapidly cycles through
		 * stop→hw_free→prepare→start during graph reconfiguration.
		 *
		 * Stopping the transport here caused:
		 *   1. transport_state reset to 0
		 *   2. Next pcm_prepare does full DMA re-init (slow)
		 *   3. Audio loops (stale DMA buffer replayed)
		 *   4. 30-40 second delays
		 *
		 * With the transport left running:
		 *   - The periodic timer IRQ keeps firing but skips
		 *     period_elapsed (playback_running/capture_running
		 *     are both false)
		 *   - The DMA writes harmlessly to our 4MB buffers
		 *   - When PipeWire triggers START again, it joins the
		 *     already-running transport instantly (no re-init)
		 *   - pcm_prepare snapshots the DMA position as an offset
		 *     so .pointer() reports 0-relative positions matching
		 *     ALSA's hw_ptr=0 reset
		 *
		 * Transport teardown happens in pcm_close when open_count
		 * reaches 0, or in uad2_shutdown/uad2_remove. */

		/* Stop polling timer when no streams are running */
		if (dev->streams_running == 0)
			uad2_period_timer_stop(dev);
		return 0;
	}
	return -EINVAL;
}

/*
 * pcm_pointer: read DMA position counter
 *
 * CPcieAudioExtension::TransportPosition @ line 72057:
 *   pos = ReadReg(BAR + 0x2244)   -- frame counter, NOT byte offset
 *   if (pos > bufferFrameSize) pos = 0
 *   return pos
 */
static snd_pcm_uframes_t uad2_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	u32 pos, offset;

	if (unlikely(READ_ONCE(dev->disconnecting)))
		return SNDRV_PCM_POS_XRUN;

	pos = uad2_read32(dev, REG_DMA_POSITION);

	/* The hardware position counter is in hardware frames (0 to
	 * buffer_frames-1).  Since we force ALSA channels to match the
	 * hardware channel count, runtime->buffer_size == dev->buffer_frames.
	 * Clamp against the hardware value for safety. */
	if (pos >= dev->buffer_frames)
		pos = 0;

	/* Apply per-direction position offset from pcm_prepare.
	 * When prepare is called while the transport is already running,
	 * we snapshot the DMA position at prepare time.  Subtracting it
	 * here makes ALSA see positions starting from 0, consistent with
	 * the hw_ptr=0 reset that ALSA performs internally.
	 *
	 * Per-direction offsets prevent playback/capture from clobbering
	 * each other's tracking when one opens after the other. */
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		offset = READ_ONCE(dev->play_pos_offset);
	else
		offset = READ_ONCE(dev->rec_pos_offset);
	pos = (pos - offset + dev->buffer_frames) % dev->buffer_frames;

	return pos;
}

/*
 * sync_stop: ensure no IRQ handler is accessing this substream.
 *
 * Called by ALSA core between trigger(STOP) and close() to guarantee
 * that snd_pcm_period_elapsed() won't reference a freed substream.
 * We synchronize against our MSI IRQ to drain any in-flight handler.
 */
static int uad2_pcm_sync_stop(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);

	synchronize_irq(pci_irq_vector(dev->pci, 0));
	return 0;
}

/*
 * mmap: map our pre-allocated DMA buffer into userspace.
 *
 * We use dma_alloc_coherent() for our 4MB buffers, which returns memory
 * that cannot be mapped via the default ALSA mmap fault handler (it uses
 * virt_to_page() which doesn't work on vmalloc/DMA-coherent addresses).
 * Instead, we provide a custom mmap callback using dma_mmap_coherent().
 */
static int uad2_pcm_mmap(struct snd_pcm_substream *ss,
			 struct vm_area_struct *vma)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *runtime = ss->runtime;
	int buf_idx = (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) ? 0 : 1;

	return dma_mmap_coherent(&dev->pci->dev, vma, dev->sg_dma_buf[buf_idx],
				 dev->sg_dma_addr[buf_idx], runtime->dma_bytes);
}

static const struct snd_pcm_ops uad2_pcm_ops = {
	.open = uad2_pcm_open,
	.close = uad2_pcm_close,
	.hw_params = uad2_pcm_hw_params,
	.hw_free = uad2_pcm_hw_free,
	.prepare = uad2_pcm_prepare,
	.trigger = uad2_pcm_trigger,
	.sync_stop = uad2_pcm_sync_stop,
	.pointer = uad2_pcm_pointer,
	.mmap = uad2_pcm_mmap,
};

/* ============================================================
 * Interrupt handler — split into hardirq top half + threaded bottom half.
 *
 * Mirrors CPcieIntrManager::ServiceInterrupt @ 0x14330.
 *
 * Kext flow:
 *   1. Read pending_lo from REG_DMA0_STATUS (0x2208)
 *   2. If has_extended_irq: read pending_hi from REG_DMA1_STATUS (0x2264)
 *   3. Combine into u64 pending, AND with intr_enable_shadow → active
 *   4. Re-arm: write active bits back to 0x2208 (and 0x2264 if extended)
 *   5. Dispatch callbacks per active bit
 *
 * Slot-to-vector mapping (from kext vector_to_slot[] table):
 *   slot 32 (bit 0 of DMA1) = vector 0x28 → notification
 *   slot 62 (bit 30 of DMA1) = vector 0x46 → periodic timer
 *   slot 63 (bit 31 of DMA1) = vector 0x47 → end-of-buffer
 *
 * With Linux MSI (single vector mode), all interrupts arrive on
 * vector 0, so we dispatch based on the pending bitmask.
 *
 * We use a threaded IRQ for uad2_handle_notification() (which needs
 * process context due to the reentrancy hazard with the connect loop's
 * manual poll) and for re-arming the one-shot periodic timer vector.
 *
 * snd_pcm_period_elapsed() is called directly from the hardirq top
 * half for minimum latency — this eliminates the variable thread
 * scheduling delay that caused ALSA to see multi-period position
 * jumps (manifesting as audible skips in PipeWire).  The function is
 * hardirq-safe (uses snd_pcm_stream_lock_irqsave() internally).
 * ============================================================ */

/*
 * Hardirq top half: read and ack interrupt status, wake thread.
 * Must be minimal — no sleeping, no complex dispatch.
 */
static irqreturn_t uad2_irq_hard(int irq, void *data)
{
	struct uad2_dev *dev = data;
	u32 pending_lo, pending_hi = 0;
	u64 pending, active, one_shot;

	/* Step 1: Read pending interrupt bitmask from DMA status registers */
	pending_lo = uad2_read32(dev, REG_DMA0_STATUS);

	/* Hot-unplug detection: MMIO reads return all-ones */
	if (pending_lo == 0xFFFFFFFF)
		return IRQ_NONE;

	if (dev->has_extended_irq) {
		pending_hi = uad2_read32(dev, REG_DMA1_STATUS);
		if (pending_hi == 0xFFFFFFFF)
			pending_hi = 0; /* treat hot-unplug as no pending */
	}

	pending = (u64)pending_hi << 32 | pending_lo;

	/* Step 2: Under a single spinlock, filter active bits, disable
	 * one-shot vectors in the enable shadow, and stash active bits
	 * for the threaded handler.
	 *
	 * The kext's ServiceInterrupt:
	 *   active = pending & enableShadow
	 *   enableShadow &= ~oneShotActive
	 *   write enableShadow to DMA0/DMA1 ENABLE registers
	 *   deferredPending |= active
	 *
	 * One-shot vectors (periodic timer slot 62, end-of-buffer slot 63)
	 * must be disabled in the ENABLE registers HERE in the hardirq,
	 * BEFORE we ack the status bits.  Otherwise a new event could fire
	 * immediately after ack and before the threaded handler re-arms,
	 * causing spurious/duplicate interrupts. */
	spin_lock(&dev->lock);

	active = pending & dev->intr_enable_shadow;
	if (!active) {
		spin_unlock(&dev->lock);
		return IRQ_NONE;
	}

	/* Disable one-shot vectors (periodic + end-of-buffer) in shadow.
	 * The threaded handler re-arms periodic via EnableVector(0x46, 1).
	 * End-of-buffer is re-armed by prepare_transport. */
	one_shot = active &
		   (BIT_ULL(INTR_SLOT_PERIODIC) | BIT_ULL(INTR_SLOT_ENDBUF));
	if (one_shot) {
		dev->intr_enable_shadow &= ~one_shot;
		uad2_write32(dev, REG_DMA0_INTR_CTRL,
			     (u32)dev->intr_enable_shadow);
		if (dev->has_extended_irq)
			uad2_write32(dev, REG_DMA1_INTR_CTRL,
				     (u32)(dev->intr_enable_shadow >> 32));
	}

	dev->irq_active |= active;

	spin_unlock(&dev->lock);

	/* Step 3: Ack only the ACTIVE (enabled) bits to status registers.
	 *
	 * The kext filters pending through the enable shadow early on
	 * (active = pending & enableShadow) and only writes active bits
	 * back to the status registers — never raw pending.  This avoids
	 * acking interrupts we didn't enable, which could confuse the
	 * firmware or clear events meant for other purposes.
	 *
	 * With MSI, un-acked but disabled bits remain pending in the
	 * device's internal state.  If a slot is later enabled, the
	 * pending bit will become active on the next read — correct
	 * level-triggered-like behavior within the device's interrupt
	 * controller. */
	uad2_write32(dev, REG_DMA0_STATUS, (u32)active);
	if (dev->has_extended_irq && (u32)(active >> 32))
		uad2_write32(dev, REG_DMA1_STATUS, (u32)(active >> 32));

	/* Periodic timer interrupt (slot 62 = vector 0x46): call
	 * snd_pcm_period_elapsed() directly from hardirq context.
	 *
	 * snd_pcm_period_elapsed() is hardirq-safe — it uses
	 * snd_pcm_stream_lock_irqsave() internally.  Many ALSA drivers
	 * (HDA, USB-audio, etc.) call it from hardirq.
	 *
	 * Moving this out of the threaded handler eliminates variable
	 * thread scheduling latency that caused ALSA to see multi-period
	 * position jumps, which PipeWire manifested as audible skips.
	 *
	 * IMPORTANT: We must NOT hold dev->lock when calling
	 * snd_pcm_period_elapsed(), because trigger() is called by ALSA
	 * with the stream lock held and then takes dev->lock internally.
	 * Lock ordering must be: stream_lock → dev->lock.  We already
	 * released dev->lock above, so we're safe here. */
	if (active & BIT_ULL(INTR_SLOT_PERIODIC)) {
		struct snd_pcm_substream *play_ss, *cap_ss;
		bool play_run, cap_run;

		spin_lock(&dev->lock);
		play_ss = dev->playback_ss;
		cap_ss = dev->capture_ss;
		play_run = dev->playback_running;
		cap_run = dev->capture_running;
		spin_unlock(&dev->lock);

		if (play_ss && play_run)
			snd_pcm_period_elapsed(play_ss);
		if (cap_ss && cap_run)
			snd_pcm_period_elapsed(cap_ss);
	}

	return IRQ_WAKE_THREAD;
}

/*
 * Threaded bottom half: dispatch deferred interrupt events.
 * Runs in process context — needed for uad2_handle_notification()
 * and re-arming the one-shot periodic timer vector.
 *
 * snd_pcm_period_elapsed() is now called directly from the hardirq
 * top half for lowest latency — see uad2_irq_hard().
 */
static irqreturn_t uad2_irq_thread(int irq, void *data)
{
	struct uad2_dev *dev = data;
	u64 active;

	/* Consume accumulated active bits */
	spin_lock_irq(&dev->lock);
	active = dev->irq_active;
	dev->irq_active = 0;
	spin_unlock_irq(&dev->lock);

	if (!active)
		return IRQ_NONE;

	/* Notification interrupt (slot 32 = vector 0x28) */
	if (active & BIT_ULL(INTR_SLOT_NOTIFY))
		uad2_handle_notification(dev);

	/* Periodic timer interrupt (slot 62 = vector 0x46).
	 *
	 * Re-arm the one-shot periodic timer vector.  The hardirq
	 * disabled this vector in intr_enable_shadow (kext one-shot
	 * behavior).  We must re-enable it here so the next period's
	 * interrupt fires.  This matches the kext's deferred handler
	 * calling ResetPeriodicTimerInterrupt → EnableVector(0x46, 1).
	 *
	 * Re-arm must happen regardless of streams_running — the
	 * transport may still be active and we need to keep receiving
	 * timer interrupts until stop_transport disables the vector. */
	if (active & BIT_ULL(INTR_SLOT_PERIODIC)) {
		if (READ_ONCE(dev->transport_state) == 2)
			uad2_enable_vector(dev, INTR_SLOT_PERIODIC, true);
	}

	/* End-of-buffer interrupt (slot 63 = vector 0x47).
	 * The kext mixer ignores this (callback ID 0x6c != 0x6d).
	 * We keep it enabled as a transport lifecycle signal but do NOT
	 * fire snd_pcm_period_elapsed from it. */
	/* (nothing to do for end-of-buffer) */

	return IRQ_HANDLED;
}

/* ============================================================
 * ALSA Mixer Controls
 *
 * Clock Source: selects the sync reference for the audio clock.
 * The _setSampleClock register at BAR+(channel_base_index<<2)+0xC04C
 * takes clock_source in the low byte, rate_enum << 8 in the next byte.
 *
 * Known clock source values (from kext analysis):
 *   0 = Internal
 *   1 = S/PDIF (optical/coaxial input)
 *
 * Sample Rate: read-only informational control showing current rate.
 * ============================================================ */

/* Clock source enumeration */
static const char *const uad2_clock_source_names[] = { "Internal", "S/PDIF" };
#define UAD2_NUM_CLOCK_SOURCES ARRAY_SIZE(uad2_clock_source_names)

static int uad2_clock_source_info(struct snd_kcontrol *kctl,
				  struct snd_ctl_elem_info *info)
{
	return snd_ctl_enum_info(info, 1, UAD2_NUM_CLOCK_SOURCES,
				 uad2_clock_source_names);
}

static int uad2_clock_source_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	val->value.enumerated.item[0] = dev->clock_source;
	return 0;
}

static int uad2_clock_source_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int new_source = val->value.enumerated.item[0];

	if (new_source >= UAD2_NUM_CLOCK_SOURCES)
		return -EINVAL;
	if (new_source == dev->clock_source)
		return 0;

	/* Reject clock source changes while transport is running —
	 * changing the clock mid-stream could corrupt DMA state. */
	if (READ_ONCE(dev->transport_state) == 2)
		return -EBUSY;

	dev->clock_source = new_source;

	/* Re-apply clock setting if we have a current rate */
	if (dev->current_rate)
		uad2_set_sample_rate(dev, dev->current_rate);

	return 1; /* value changed */
}

static const struct snd_kcontrol_new uad2_clock_source_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Clock Source",
	.info = uad2_clock_source_info,
	.get = uad2_clock_source_get,
	.put = uad2_clock_source_put,
};

/* Sample rate enum control (read-write).
 * Allows changing the sample rate via amixer/ALSA control interface.
 * Maps to the same set_sample_rate function used by pcm_prepare. */
static const char *const uad2_rate_names[] = {
	"44100", "48000", "88200", "96000", "176400", "192000",
};
static const unsigned int uad2_rate_values[] = {
	44100, 48000, 88200, 96000, 176400, 192000,
};
#define UAD2_NUM_RATES ARRAY_SIZE(uad2_rate_names)

static int uad2_sample_rate_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *info)
{
	return snd_ctl_enum_info(info, 1, UAD2_NUM_RATES, uad2_rate_names);
}

static int uad2_sample_rate_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int rate = dev->current_rate ? dev->current_rate : 48000;
	int i;

	for (i = 0; i < (int)UAD2_NUM_RATES; i++) {
		if (uad2_rate_values[i] == rate) {
			val->value.enumerated.item[0] = i;
			return 0;
		}
	}
	val->value.enumerated.item[0] = 1; /* default: 48000 */
	return 0;
}

static int uad2_sample_rate_put(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int idx = val->value.enumerated.item[0];
	unsigned int rate;

	if (idx >= UAD2_NUM_RATES)
		return -EINVAL;

	rate = uad2_rate_values[idx];
	if (rate == dev->current_rate)
		return 0;

	uad2_set_sample_rate(dev, rate);
	return 1; /* value changed */
}

static const struct snd_kcontrol_new uad2_sample_rate_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.name = "Sample Rate",
	.info = uad2_sample_rate_info,
	.get = uad2_sample_rate_get,
	.put = uad2_sample_rate_put,
};

static int uad2_create_mixer(struct uad2_dev *dev)
{
	struct snd_card *card = dev->card;
	int err;

	err = snd_ctl_add(card, snd_ctl_new1(&uad2_clock_source_ctl, dev));
	if (err < 0)
		return err;

	err = snd_ctl_add(card, snd_ctl_new1(&uad2_sample_rate_ctl, dev));
	if (err < 0)
		return err;

	return 0;
}

/* ============================================================
 * PCI probe
 * ============================================================ */
static int uad2_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	struct uad2_dev *dev;
	struct snd_card *card;
	int err;

	err = snd_card_new(&pci->dev, SNDRV_DEFAULT_IDX1, SNDRV_DEFAULT_STR1,
			   THIS_MODULE, sizeof(*dev), &card);
	if (err < 0)
		return err;

	dev = card->private_data;
	dev->pci = pci;
	dev->card = card;
	spin_lock_init(&dev->lock);
	spin_lock_init(&dev->notify_lock);
	init_completion(&dev->notify_event);
	init_completion(&dev->connect_event);
	init_completion(&dev->rate_event);
	INIT_DELAYED_WORK(&dev->dsp_service_work, uad2_dsp_service_handler);
	hrtimer_setup(&dev->period_timer, uad2_period_timer_fn, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	dev->transport_state = 0;
	dev->periodic_timer_interval = 0;
	dev->intr_enable_shadow = 0;

	/* PCI setup */
	err = pci_enable_device(pci);
	if (err)
		goto err_free_card;

	err = pci_request_regions(pci, "uad2");
	if (err)
		goto err_disable_pci;

	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
		if (err)
			goto err_release_regions;
	}

	pci_set_master(pci);

	/* Map 64KB BAR 0 (confirmed: phys 0x1203000000, len 65536) */
	dev->bar = pci_iomap(pci, 0, 0);
	dev->bar_len = pci_resource_len(pci, 0);
	if (!dev->bar) {
		dev_err(&pci->dev, "Failed to map BAR 0\n");
		err = -ENOMEM;
		goto err_release_regions;
	}

	dev_info(&pci->dev, "BAR 0 mapped: %pa len=%pa\n",
		 &pci->resource[0].start, &dev->bar_len);

	/*
	 * Snapshot device ID register at probe time.
	 * Used as expected value in uad2_hw_program() handshake
	 * and polled after clock changes.
	 */
	dev->expected_device_id = uad2_read32(dev, REG_DEVICE_ID);
	dev_info(&pci->dev, "Device ID register: 0x%08x\n",
		 dev->expected_device_id);

	/* Device detection: read serial prefix, determine model, set channels.
	 * All Apollo TB devices use channel_base_index=10 (bank_shift=0x0A). */
	dev->channel_base_index = UAD2_CHANNEL_BASE_IDX;
	dev->dma_ctrl_shadow = DMA_CTRL_MASTER_ENABLE;
	dev->clock_source = 0; /* internal */
	dev->current_rate = 48000;

	uad2_detect_device(dev);
	uad2_pcie_setup(dev);

	dev->has_extended_irq = dev->fw_v2; /* v2 FW uses DMA1 regs */

	/* Allocate the two 4MB DMA buffers for scatter-gather */
	err = uad2_alloc_sg_buffers(dev);
	if (err)
		goto err_unmap_bar;

	/* MSI interrupt */
	err = pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (err < 0)
		goto err_free_sg;

	err = request_threaded_irq(pci_irq_vector(pci, 0), uad2_irq_hard,
				   uad2_irq_thread, IRQF_SHARED, "uad2", dev);
	if (err)
		goto err_free_irq_vectors;

	/* Tell ALSA core our IRQ so snd_card_disconnect() can synchronize
	 * against in-flight hardirq handlers before transitioning PCM
	 * substreams to DISCONNECTED state.  Without this, there is a race
	 * window where the hardirq calls snd_pcm_period_elapsed() on a
	 * substream that has just been moved to DISCONNECTED. */
	card->sync_irq = pci_irq_vector(pci, 0);

	/* Hardware initialization (full sequence):
	 *   1. CPcieDevice::ProgramRegisters (device ID, DMA reset, DSP init)
	 *   2. CPcieAudioExtension::ProgramRegisters (SG tables, FW base, IRQ)
	 *   3. CPcieAudioExtension::Connect (20-channel firmware handshake)
	 */
	err = uad2_hw_program(dev);
	if (err) {
		dev_err(&pci->dev, "Hardware init failed: %d\n", err);
		/* Shutdown any partially-initialized hardware state
		 * (SG tables, interrupt vectors, DMA may be active) */
		uad2_shutdown(dev);
		goto err_free_irq;
	}

	/* ALSA card registration */
	strscpy(card->driver, "uad2", sizeof(card->driver));
	snprintf(card->shortname, sizeof(card->shortname), "UA %s",
		 ua_device_name(dev->device_type));
	snprintf(card->longname, sizeof(card->longname),
		 "Universal Audio %s Thunderbolt",
		 ua_device_name(dev->device_type));

	err = snd_pcm_new(card, "UAD2", 0, 1, 1, &dev->pcm);
	if (err < 0)
		goto err_free_irq;

	dev->pcm->private_data = dev;
	dev->pcm->info_flags = SNDRV_PCM_INFO_JOINT_DUPLEX;
	snd_pcm_set_ops(dev->pcm, SNDRV_PCM_STREAM_PLAYBACK, &uad2_pcm_ops);
	snd_pcm_set_ops(dev->pcm, SNDRV_PCM_STREAM_CAPTURE, &uad2_pcm_ops);

	/* No snd_pcm_lib_preallocate_pages_for_all — we use our own
	 * pre-allocated 4MB DMA buffers mapped in hw_params. */

	/* Create mixer controls (clock source, sample rate readout) */
	err = uad2_create_mixer(dev);
	if (err < 0)
		goto err_free_irq;

	err = snd_card_register(card);
	if (err < 0)
		goto err_free_irq;

	pci_set_drvdata(pci, dev);
	dev_info(&pci->dev,
		 "%s initialized — play=%uch rec=%uch buf=%u frames\n",
		 ua_device_name(dev->device_type), dev->play_channels,
		 dev->rec_channels, dev->buffer_frames);
	return 0;

err_free_irq:
	free_irq(pci_irq_vector(pci, 0), dev);
err_free_irq_vectors:
	pci_free_irq_vectors(pci);
err_free_sg:
	uad2_free_sg_buffers(dev);
err_unmap_bar:
	pci_iounmap(pci, dev->bar);
err_release_regions:
	pci_release_regions(pci);
err_disable_pci:
	pci_disable_device(pci);
err_free_card:
	snd_card_free(card);
	return err;
}

static void uad2_remove(struct pci_dev *pci)
{
	struct uad2_dev *dev = pci_get_drvdata(pci);

	/* Mark card as disconnected first — this causes all subsequent
	 * ALSA operations (PCM open/prepare/trigger/pointer) to return
	 * errors immediately, preventing races where userspace still has
	 * the device open while we tear down hardware state. */
	snd_card_disconnect(dev->card);

	/* Full shutdown sequence (mirrors kext CPcieAudioExtension::Shutdown):
	 * stops transport, clears doorbell, disables notification vector,
	 * sends disconnect command to firmware.
	 * Must happen BEFORE setting disconnecting flag so MMIO writes
	 * actually reach the hardware. */
	uad2_shutdown(dev);

	/* Disable global interrupt enable */
	uad2_write32(dev, REG_INTR_ENABLE, 0x0);

	/* Set disconnecting flag to guard MMIO access — after this point,
	 * uad2_read32/uad2_write32 become no-ops so the IRQ handler and
	 * any remaining ALSA callbacks won't touch disappeared hardware. */
	WRITE_ONCE(dev->disconnecting, true);

	/* Free IRQ before snd_card_free() — the card free will release
	 * dev (it's card->private_data), so we must stop the IRQ handler
	 * first to prevent use-after-free. */
	free_irq(pci_irq_vector(pci, 0), dev);
	pci_free_irq_vectors(pci);

	/* Free DMA buffers while dev is still alive */
	uad2_free_sg_buffers(dev);
	if (dev->ring_dma_buf[0]) {
		dma_free_coherent(&pci->dev,
				  DSP_RING_DESC_SLOTS * DSP_RING_PAGE_SIZE,
				  dev->ring_dma_buf[0], dev->ring_dma_addr[0]);
	}

	pci_iounmap(pci, dev->bar);

	/* snd_card_free() frees dev (card->private_data) — must be last
	 * operation that references dev */
	snd_card_free(dev->card);

	pci_release_regions(pci);
	pci_disable_device(pci);
}

static const struct pci_device_id uad2_pci_ids[] = {
	/* All Apollo Thunderbolt devices share the same PCI vendor/device ID.
	 * The actual model is determined by serial prefix detection in probe.
	 * Supports: Solo, Arrow, Twin X, x4, x6, x8, x8p, x16, x16D,
	 * and all Gen 2 variants. */
	{ PCI_DEVICE(UA_VENDOR_ID, UA_DEVICE_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, uad2_pci_ids);

/* ============================================================
 * Power management
 *
 * On suspend, shut down the hardware and disable PCI.
 * On resume, re-run the full initialization sequence.
 * Without these, the device is left in undefined state across
 * system sleep, causing kernel warnings and broken audio on wake.
 * ============================================================ */
static int uad2_suspend(struct device *d)
{
	struct pci_dev *pci = to_pci_dev(d);
	struct uad2_dev *dev = pci_get_drvdata(pci);

	if (dev->card)
		snd_power_change_state(dev->card, SNDRV_CTL_POWER_D3hot);

	uad2_shutdown(dev);

	pci_save_state(pci);
	pci_disable_device(pci);

	return 0;
}

static int uad2_resume(struct device *d)
{
	struct pci_dev *pci = to_pci_dev(d);
	struct uad2_dev *dev = pci_get_drvdata(pci);
	int err;

	pci_restore_state(pci);
	err = pci_enable_device(pci);
	if (err) {
		dev_err(&pci->dev, "Resume: pci_enable_device failed: %d\n",
			err);
		return err;
	}
	pci_set_master(pci);

	err = uad2_hw_program(dev);
	if (err) {
		dev_err(&pci->dev, "Resume: hw_program failed: %d\n", err);
		return err;
	}

	if (dev->card)
		snd_power_change_state(dev->card, SNDRV_CTL_POWER_D0);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(uad2_pm_ops, uad2_suspend, uad2_resume);

/* ============================================================
 * PCI error recovery (AER / Thunderbolt link-down)
 *
 * When the Thunderbolt controller detects a link-down event, the PCI
 * subsystem may invoke AER recovery BEFORE calling .remove().  Without
 * an error handler, the default behavior is undefined.  We immediately
 * gate MMIO access so the IRQ handler and any in-flight ALSA callbacks
 * won't touch disappeared hardware.
 * ============================================================ */
static pci_ers_result_t uad2_error_detected(struct pci_dev *pci,
					    pci_channel_state_t state)
{
	struct uad2_dev *dev = pci_get_drvdata(pci);

	dev_err(&pci->dev, "PCI error detected (state=%d)\n", state);
	WRITE_ONCE(dev->disconnecting, true);

	if (state == pci_channel_io_perm_failure)
		return PCI_ERS_RESULT_DISCONNECT;

	return PCI_ERS_RESULT_NEED_RESET;
}

static const struct pci_error_handlers uad2_err_handler = {
	.error_detected = uad2_error_detected,
};

static struct pci_driver uad2_driver = {
	.name = "uad2",
	.id_table = uad2_pci_ids,
	.probe = uad2_probe,
	.remove = uad2_remove,
	.driver.pm = pm_sleep_ptr(&uad2_pm_ops),
	.err_handler = &uad2_err_handler,
};

module_pci_driver(uad2_driver);

MODULE_AUTHOR("Claude Opus 4.6 (Anthropic)");
MODULE_DESCRIPTION("Universal Audio UAD2 Thunderbolt audio driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("2026.302.4");
