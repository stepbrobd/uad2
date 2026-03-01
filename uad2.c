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
#include <linux/log2.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/control.h>
#include <sound/initval.h>

/* ============================================================
 * PCI identity (confirmed via ioreg on live hardware)
 * Bus 45:0:0, Thunderbolt-tunnelled PCIe Gen1 x1
 * ============================================================ */
#define UA_VENDOR_ID 0x1a00
#define UAD2_DEVICE_ID_SOLO 0x0002
#define UAD2_SUBSYS_ID_SOLO 0x000f

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
#define DMA_CTRL_SINGLE_ENGINE 0x1e00 /* bits[12:9] */
#define DMA_CTRL_DUAL_ENGINE 0x1fe00 /* bits[16:9] */

/* --- DMA engine 0 (CPcieIntrManager::ProgramRegisters) --- */
#define REG_DMA0_INTR_CTRL 0x2204 /* WRITE 0x0 to disable/clear */
#define REG_DMA0_STATUS 0x2208 /* WRITE 0xFFFFFFFF to arm; WRITE 0x0 to clear */

/* --- Device identification (CPcieDevice::ProgramRegisters) --- */
#define REG_DEVICE_ID \
	0x2218 /* READ to verify; WRITE back as echo handshake
                                         * Also polled after clock change (2s timeout) */

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

/* --- DMA engine 1 (dual-engine devices only) --- */
#define REG_DMA1_INTR_CTRL 0x2264
#define REG_DMA1_STATUS 0x2268

/* --- Buffer and timer configuration --- */
#define REG_BUFFER_SIZE_KB 0x226C /* W: (totalPlayChans * (bufSz-1)) >> 10 */
#define REG_PERIODIC_TIMER 0x2270 /* W: periodic timer interval in frames */

/* --- Playback monitor status --- */
#define REG_PLAYBACK_MON_STAT 0x22C0 /* R: playback monitor status */
#define REG_SECONDARY_CTL 0x22C4 /* W: 0 on Shutdown */

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
 * Audio engine parameters (from ioreg IOAudioEngine, confirmed live)
 *
 * IOAudioEngineNumSampleFramesPerBuffer = 8192
 * IOAudioEngineSampleOffset = 16
 * Current sample rate = 48000
 * Format: 32-bit container, 24-bit depth, signed int, little-endian
 *         (IOAudioStreamByteOrder=1, alignment=1 = MSB-justified)
 * Input: 32 channels, Output: 42 channels
 * ============================================================ */
#define UAD2_MAX_BUFFER_FRAMES 8192 /* max from _recomputeBufferFrameSize cap */
#define UAD2_SAMPLE_OFFSET 16
#define UAD2_OUT_CHANNELS 42 /* output channels (to device) */
#define UAD2_IN_CHANNELS 32 /* input channels (from device) */
#define UAD2_BYTES_PER_SAMPLE 4 /* 24-bit in 32-bit container */
#define UAD2_MAX_DSPS 8

/* channel_base_index: constant loaded from data segment @ 0x6018 in kext
 * Confirmed value = 10 (0x0A) for Apollo Solo */
#define UAD2_CHANNEL_BASE_IDX 10

/* Connect loop: 20 channels with 10 retries each */
#define UAD2_CONNECT_CHANNELS 20
#define UAD2_CONNECT_RETRIES 10
#define UAD2_CONNECT_WAIT_MS 100

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
	bool dual_dma;
	unsigned int dsp_count;
	u32 channel_base_index; /* 10 for Apollo Solo */

	/* Shadow register for DMA master control (never read from HW) */
	u32 dma_ctrl_shadow;

	/* Shadow register for interrupt enable bitmask
	 * (mirrors IntrManager+0x20 in kext) */
	u64 intr_enable_shadow;

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

	/* Notification interrupt handling */
	struct completion notify_event; /* signals Connect() on bit 21 */
	struct completion connect_event; /* signals on bit 5 (connect ack) */
	u32 notify_status; /* last notification bitmask */
	bool connected;

	/* PCM substream pointers for IRQ handler (accessed under lock) */
	struct snd_pcm_substream *playback_ss;
	struct snd_pcm_substream *capture_ss;
};

/* ============================================================
 * Low-level register accessors
 * Mirrors CUAOS::ReadReg / CUAOS::WriteReg
 * ============================================================ */
static inline u32 uad2_read32(struct uad2_dev *dev, u32 offset)
{
	return ioread32(dev->bar + offset);
}

static inline void uad2_write32(struct uad2_dev *dev, u32 offset, u32 val)
{
	iowrite32(val, dev->bar + offset);
}

/* Compute the firmware mailbox register address with channel_base_index */
static inline u32 uad2_fw_reg(struct uad2_dev *dev, u32 base_offset)
{
	return (dev->channel_base_index << 2) + base_offset;
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
 * CPcieAudioExtension::_setSampleClock @ line 72712
 *
 * Writes combined clock_source | (rate_enum << 8) to REG_SAMPLE_CLOCK,
 * then triggers with REG_STREAM_ENABLE = 0x4,
 * then polls REG_DEVICE_ID for ack (2s timeout).
 *
 * clock_source: 0 = internal
 */
static int uad2_set_sample_rate(struct uad2_dev *dev, unsigned int rate)
{
	u32 clock_val;
	u8 rate_enum;
	u32 reg_offset;
	int i;
	u32 val;

	rate_enum = uad2_rate_to_enum(rate);
	clock_val = dev->clock_source | ((u32)rate_enum << 8);

	/* Write to sample clock register: BAR + (channel_base_index << 2) + 0xC04C */
	reg_offset = uad2_fw_reg(dev, REG_SAMPLE_CLOCK);
	uad2_write32(dev, reg_offset, clock_val);

	/* Trigger clock change */
	uad2_write32(dev, REG_STREAM_ENABLE, 0x4);

	/* Poll REG_DEVICE_ID for acknowledgment (2s timeout, per kext 0x7D0 = 2000ms) */
	for (i = 0; i < 200; i++) {
		msleep(10);
		val = uad2_read32(dev, REG_DEVICE_ID);
		if (val == dev->expected_device_id) {
			dev->current_rate = rate;
			return 0;
		}
	}

	dev_warn(&dev->pci->dev, "Sample rate change timeout (rate=%u)\n",
		 rate);
	return -ETIMEDOUT;
}

/* ============================================================
 * _recomputeBufferFrameSize equivalent @ 0x4d9e0 (line 72414)
 *
 * Formula: floor_pow2(0x400000 / (max(play_ch, rec_ch) * 4))
 * Capped at 0x2000 (8192 frames).
 *
 * For Apollo Solo with 42 play / 32 rec:
 *   0x400000 / (42 * 4) = 24966 → floor_pow2 = 16384 → capped to 8192
 * ============================================================ */
static unsigned int uad2_compute_buffer_frames(unsigned int play_ch,
					       unsigned int rec_ch)
{
	unsigned int max_ch = max(play_ch, rec_ch);
	unsigned int raw_frames;

	if (max_ch == 0)
		max_ch = 1;

	raw_frames = SG_BUFFER_SIZE / (max_ch * UAD2_BYTES_PER_SAMPLE);

	/* Round down to power of 2 */
	raw_frames = rounddown_pow_of_two(raw_frames);

	/* Cap at 8192 */
	if (raw_frames > UAD2_MAX_BUFFER_FRAMES)
		raw_frames = UAD2_MAX_BUFFER_FRAMES;

	return raw_frames;
}

/* ============================================================
 * Interrupt vector enable/disable
 *
 * The kext maintains a lookup table at IntrManager+0x4D0 that maps
 * logical vector IDs to bit slot indices.  We don't replicate that
 * table; instead, we assume the slot index equals the vector ID
 * (which holds for the vectors we use: 0x28, 0x46, 0x47).
 *
 * EnableVector(vec, arm):
 *   shadow |= (1 << vec)
 *   if arm: write (1 << vec) to BAR+0x2208
 *   write shadow to BAR+0x2204
 *
 * DisableVector(vec):
 *   shadow &= ~(1 << vec)
 *   write shadow to BAR+0x2204
 * ============================================================ */
static void uad2_enable_vector(struct uad2_dev *dev, unsigned int vec, bool arm)
{
	u64 bit = 1ULL << vec;

	dev->intr_enable_shadow |= bit;

	if (arm)
		uad2_write32(dev, REG_DMA0_STATUS, (u32)bit);

	uad2_write32(dev, REG_DMA0_INTR_CTRL, (u32)dev->intr_enable_shadow);

	if (dev->dual_dma) {
		if (arm)
			uad2_write32(dev, REG_DMA1_INTR_CTRL, (u32)(bit >> 32));
		uad2_write32(dev, REG_DMA1_STATUS,
			     (u32)(dev->intr_enable_shadow >> 32));
	}
}

static void uad2_disable_vector(struct uad2_dev *dev, unsigned int vec)
{
	u64 bit = 1ULL << vec;

	dev->intr_enable_shadow &= ~bit;

	uad2_write32(dev, REG_DMA0_INTR_CTRL, (u32)dev->intr_enable_shadow);

	if (dev->dual_dma)
		uad2_write32(dev, REG_DMA1_STATUS,
			     (u32)(dev->intr_enable_shadow >> 32));
}

/* Interrupt vector IDs (from CPcieAudioExtension Initialize + kext analysis) */
#define INTR_VEC_NOTIFY 0x28 /* notification interrupt */
#define INTR_VEC_PERIODIC 0x46 /* periodic timer interrupt */
#define INTR_VEC_ENDBUF 0x47 /* end-of-buffer interrupt */

/* ============================================================
 * CPcieIntrManager::ProgramRegisters equivalent
 * Clears DMA interrupt and status registers
 * ============================================================ */
static void uad2_intr_program(struct uad2_dev *dev)
{
	uad2_write32(dev, REG_DMA0_INTR_CTRL, 0x0);
	uad2_write32(dev, REG_DMA0_STATUS, 0x0);

	if (dev->dual_dma) {
		uad2_write32(dev, REG_DMA1_INTR_CTRL, 0x0);
		uad2_write32(dev, REG_DMA1_STATUS, 0x0);
	}

	dev->intr_enable_shadow = 0;
}

/* ============================================================
 * CPcieIntrManager::ResetDMAEngines equivalent
 * Computes channel enable bitmask and programs DMA master control
 * ============================================================ */
static void uad2_dma_reset(struct uad2_dev *dev)
{
	u32 ctrl;

	if (dev->dual_dma)
		ctrl = DMA_CTRL_DUAL_ENGINE;
	else
		ctrl = DMA_CTRL_SINGLE_ENGINE;

	uad2_write32(dev, REG_DMA_MASTER_CTRL, ctrl);
	uad2_write32(dev, REG_DMA_MASTER_CTRL,
		     ctrl); /* written twice per kext */
	uad2_read32(dev, REG_DMA_MASTER_CTRL); /* read-back flush */

	dev->dma_ctrl_shadow = ctrl;
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
 * Polls DSP ring+0x1a4 until DSP_READY_SIG appears
 * ============================================================ */
static int uad2_dsp_wait_ready(struct uad2_dev *dev, void __iomem *ring_base,
			       int dsp_type)
{
	int max_attempts = (dsp_type == 0) ? DSP_POLL_MAX_PRIMARY :
					     DSP_POLL_MAX_SECONDARY;
	int i;
	u32 val;

	for (i = 0; i < max_attempts; i++) {
		val = ioread32(ring_base + DSP_READY_POLL_OFF);
		msleep(DSP_POLL_INTERVAL_MS);

		if (val == DSP_READY_SIG) {
			dev_info(&dev->pci->dev,
				 "DSP %d ready after %d polls (val=0x%08x)\n",
				 dsp_type, i + 1, val);
			return 0;
		}
		if (val != 0) {
			dev_warn(&dev->pci->dev,
				 "DSP %d unexpected ready value 0x%08x\n",
				 dsp_type, val);
			return 0;
		}
	}

	dev_err(&dev->pci->dev, "DSP %d failed to start (timeout after %dms)\n",
		dsp_type, max_attempts * DSP_POLL_INTERVAL_MS);
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

	/* Read hardware ring capacity, capped at 0x400 */
	ring_size = ioread32(ring_base + DSP_RING_CAPACITY);
	if (ring_size >= 0x400) {
		dev_warn(&dev->pci->dev,
			 "Ring %d capacity 0x%x out of range, clamping to 0\n",
			 ring_idx, ring_size);
		ring_size = 0;
	}

	dev_dbg(&dev->pci->dev, "Ring %d capacity=0x%x\n", ring_idx, ring_size);

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
	dma_addr_t ring_dma;
	int dsp_type = dsp_index;
	int err;

	/* Compute ring base within BAR */
	if (dsp_index < 4)
		ring_base = dev->bar + DSP_RING_BASE_LOW +
			    (dsp_index * DSP_RING_STRIDE);
	else
		ring_base = dev->bar + DSP_RING_BASE_HIGH +
			    (dsp_index * DSP_RING_STRIDE);

	ring2_base = ring_base + DSP_RING2_OFFSET;

	dev_dbg(&dev->pci->dev, "DSP %d ring_base=%px ring2=%px\n", dsp_index,
		ring_base, ring2_base);

	/* Wait for DSP firmware (type 0 only) */
	if (dsp_type == 0) {
		err = uad2_dsp_wait_ready(dev, ring_base, dsp_type);
		if (err)
			return err;
	}

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
		memset(dev->sg_dma_buf[i], 0, dev->sg_buf_size);
		dev_info(&dev->pci->dev,
			 "SG buffer %d: virt=%px dma=%pad size=0x%zx\n", i,
			 dev->sg_dma_buf[i], &dev->sg_dma_addr[i],
			 dev->sg_buf_size);
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

	dev_info(
		&dev->pci->dev,
		"Scatter-gather tables programmed: %u entries, play_dma=%pad cap_dma=%pad\n",
		SG_NUM_ENTRIES, &dev->sg_dma_addr[0], &dev->sg_dma_addr[1]);

	/* Phase 3: Read firmware base address (BAR+0x30 lo, BAR+0x34 hi) */
	{
		u32 fw_lo = uad2_read32(dev, REG_FW_BASE_LO);
		u32 fw_hi = uad2_read32(dev, REG_FW_BASE_HI);

		dev->fw_base_addr = ((u64)fw_hi << 32) | fw_lo;
		dev_info(&dev->pci->dev, "Firmware base address: 0x%016llx\n",
			 dev->fw_base_addr);
	}

	/* Phase 4: Enable interrupt vector 0x28 (notification interrupt)
	 *
	 * In the kext, this calls CPcieIntrManager::EnableVector(0x28, 1).
	 * This arms the vector in the DMA interrupt controller and updates
	 * the enable shadow bitmask. */
	uad2_write32(dev, REG_INTR_ENABLE, 0xFFFFFFFF);
	uad2_enable_vector(dev, INTR_VEC_NOTIFY, true);

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
	if (!dev->connected) {
		spin_unlock_irqrestore(&dev->notify_lock, flags);
		return;
	}

	status = uad2_read32(dev, notify_reg);
	if (!status) {
		spin_unlock_irqrestore(&dev->notify_lock, flags);
		return;
	}

	dev->notify_status = status;

	dev_dbg(&dev->pci->dev, "Notification status: 0x%08x\n", status);

	/* Bit 5: Connect ack */
	if (status & NOTIFY_CONNECT_ACK) {
		dev_dbg(&dev->pci->dev, "Connect ack received\n");
		complete(&dev->connect_event);
	}

	/* Bit 21: Channel config — in kext this copies 10 DWORDs from
	 * BAR+0xC000 and forces bits 0+1 on.  For the Linux driver we
	 * mostly care about the channel counts being set and the
	 * notify_event signal at the end. */
	if (status & NOTIFY_CHAN_CONFIG) {
		dev_dbg(&dev->pci->dev, "Channel config update\n");
		/* Force playback + record IO ready processing */
		status |= NOTIFY_PLAYBACK_IO | NOTIFY_RECORD_IO;
	}

	/* Bit 0: Playback IO ready — kext copies 72 DWORDs from BAR+0xC2C4 */
	if (status & NOTIFY_PLAYBACK_IO) {
		u32 play_ch;

		/* Read playback channel count from the IO descriptor area.
		 * In kext: this+0x2F8 = playbackChannelCount, read from
		 * the 5th DWORD of the playback IO descriptor block
		 * (BAR+0xC2C4 + 0x10 = BAR+0xC2D4 is offset for count field,
		 *  but the count is stored at this+0x2F8 after copy).
		 * We read it from the known offset: descriptor[4]. */
		play_ch = uad2_read32(
			dev, uad2_fw_reg(dev, REG_PLAYBACK_IO_DESC + 0x10));
		if (play_ch > 0 && play_ch <= 128) {
			dev->play_channels = play_ch;
			dev_dbg(&dev->pci->dev, "Playback channels: %u\n",
				play_ch);
		}
	}

	/* Bit 1: Record IO ready — kext copies 72 DWORDs from BAR+0xC1A4 */
	if (status & NOTIFY_RECORD_IO) {
		u32 rec_ch;

		rec_ch = uad2_read32(
			dev, uad2_fw_reg(dev, REG_RECORD_IO_DESC + 0x10));
		if (rec_ch > 0 && rec_ch <= 128) {
			dev->rec_channels = rec_ch;
			dev_dbg(&dev->pci->dev, "Record channels: %u\n",
				rec_ch);
		}
	}

	/* Bit 22: Rate change */
	if (status & NOTIFY_RATE_CHANGE) {
		dev_dbg(&dev->pci->dev, "Rate change notification\n");
	}

	/* Bit 4: DMA ready */
	if (status & NOTIFY_DMA_READY) {
		dev_dbg(&dev->pci->dev, "DMA ready notification\n");
	}

	/* Bit 6: Error */
	if (status & NOTIFY_ERROR) {
		dev_warn(&dev->pci->dev, "Firmware error notification\n");
	}

	/* Bit 21 (second pass): Signal notify_event — wakes Connect() */
	if (status & NOTIFY_CHAN_CONFIG) {
		complete(&dev->notify_event);
	}

	/* Bit 7: End buffer */
	if (status & NOTIFY_END_BUFFER) {
		dev_dbg(&dev->pci->dev, "End buffer notification\n");
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
 * 1. Reset notify_event
 * 2. For chan = 0..19:
 *    a. Write 0x0ACEFACE to BAR+(channel_base_index<<2)+0xC004
 *    b. Write 0x1 to BAR+0x2260 (stream enable doorbell)
 *    c. Wait on notify_event (100ms timeout, up to 10 retries)
 *    d. If timeout: manually call _handleNotificationInterrupt()
 * 3. After all 20 channels: check connected flag
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
	long ret;

	dev->connected = false;

	for (chan = 0; chan < UAD2_CONNECT_CHANNELS; chan++) {
		/* Reset completion before each doorbell */
		reinit_completion(&dev->notify_event);

		/* Write DMA descriptor magic (doorbell command) */
		uad2_write32(dev, doorbell_reg, DMA_DESC_MAGIC);

		/* Write stream enable (doorbell to firmware) */
		uad2_write32(dev, REG_STREAM_ENABLE, 0x1);

		/* Wait for firmware response via notification interrupt.
		 * Kext uses intr_timer->wait(100) with up to 10 retries.
		 * In between retries, it manually polls the notification
		 * register via _handleNotificationInterrupt(). */
		timeout_jiffies = msecs_to_jiffies(UAD2_CONNECT_WAIT_MS);

		for (retry = 0; retry < UAD2_CONNECT_RETRIES; retry++) {
			ret = wait_for_completion_timeout(&dev->notify_event,
							  timeout_jiffies);
			if (ret > 0)
				break; /* Got a response */

			/* Timeout: manually poll notification register
			 * (mirrors kext behavior of calling
			 *  _handleNotificationInterrupt on timeout) */
			uad2_handle_notification(dev);

			/* Check if notification arrived during manual poll */
			if (try_wait_for_completion(&dev->notify_event))
				break;
		}

		if (retry >= UAD2_CONNECT_RETRIES) {
			dev_warn(
				&dev->pci->dev,
				"Connect channel %d: no response after %d retries\n",
				chan, UAD2_CONNECT_RETRIES);
		}
	}

	/* After completing all 20 channels, compute buffer frame size */
	if (dev->play_channels == 0)
		dev->play_channels = UAD2_OUT_CHANNELS;
	if (dev->rec_channels == 0)
		dev->rec_channels = UAD2_IN_CHANNELS;

	dev->buffer_frames = uad2_compute_buffer_frames(dev->play_channels,
							dev->rec_channels);

	dev->connected = true;

	dev_info(&dev->pci->dev,
		 "Audio connected: play=%u rec=%u buffer_frames=%u\n",
		 dev->play_channels, dev->rec_channels, dev->buffer_frames);

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

	/* 2. Echo device ID back (handshake) */
	uad2_write32(dev, REG_DEVICE_ID, device_id);

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
static int uad2_prepare_transport(struct uad2_dev *dev,
				  unsigned int buffer_frames,
				  unsigned int irq_period_frames,
				  unsigned int play_channels,
				  unsigned int rec_channels)
{
	u32 buf_size_kb;
	unsigned long flags;
	int i;

	/* Validate: must be connected and not already running */
	if (!dev->connected) {
		dev_err(&dev->pci->dev, "PrepareTransport: not connected\n");
		return -ENODEV;
	}
	if (dev->transport_state == 2) {
		dev_warn(&dev->pci->dev,
			 "PrepareTransport: transport already running\n");
		return -EBUSY;
	}
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

	/* 8. Periodic timer (if configured) */
	if (dev->periodic_timer_interval) {
		uad2_write32(dev, REG_PERIODIC_TIMER,
			     dev->periodic_timer_interval);
		uad2_enable_vector(dev, INTR_VEC_PERIODIC, true);
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

	/* 16. Read playback monitor status (flush) */
	uad2_read32(dev, REG_PLAYBACK_MON_STAT);

	/* 17. Playback monitor config (always enable for now) */
	uad2_write32(dev, REG_PLAYBACK_MON_CFG, (play_channels - 1) | 0x100);

	spin_unlock_irqrestore(&dev->lock, flags);

	/* 19. Mark transport as prepared */
	dev->transport_state = 1;

	/* 20. Poll BAR+0x2254 until DMA is ready (compare vs irq_period)
	 * Kext does max 2 iterations with 1ms sleep between */
	for (i = 0; i < 200; i++) {
		u32 poll_val = uad2_read32(dev, REG_POLL_STATUS);

		if (poll_val == irq_period_frames) {
			dev_dbg(&dev->pci->dev, "DMA ready after %d polls\n",
				i + 1);
			break;
		}
		usleep_range(1000, 2000);
	}

	/* 21. Enable end-of-buffer interrupt vector */
	uad2_enable_vector(dev, INTR_VEC_ENDBUF, true);

	dev_dbg(&dev->pci->dev,
		"Transport prepared: buf=%u irq=%u play=%u rec=%u\n",
		buffer_frames, irq_period_frames, play_channels, rec_channels);

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
 *      - variant 0x9 → use 0x20F if extended_mode_flag (this+0x2EF4) set
 *      - otherwise   → use 0xF
 *   5. Write BAR+0x2248 = start_value
 *   6. Unlock hw_lock
 *   7. Set transport_state = 2 (running)
 *
 * Apollo Solo uses normal start (0xF).
 * ============================================================ */
static void uad2_start_transport(struct uad2_dev *dev)
{
	unsigned long flags;

	spin_lock_irqsave(&dev->lock, flags);
	uad2_write32(dev, REG_TRANSPORT_CTL, 0xF);
	spin_unlock_irqrestore(&dev->lock, flags);

	dev->transport_state = 2;
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

	/* Disable end-of-buffer interrupt before stopping */
	uad2_disable_vector(dev, INTR_VEC_ENDBUF);

	spin_lock_irqsave(&dev->lock, flags);
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);
	spin_unlock_irqrestore(&dev->lock, flags);

	dev->transport_state = 0;
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

	/* 1-6: Stop transport and clear doorbell under lock */
	spin_lock_irqsave(&dev->lock, flags);

	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);
	uad2_write32(dev, uad2_fw_reg(dev, REG_FW_DOORBELL), 0x0);
	uad2_read32(dev, REG_TRANSPORT_CTL); /* flush */
	uad2_write32(dev, REG_SECONDARY_CTL, 0x0);

	spin_unlock_irqrestore(&dev->lock, flags);

	/* 7: Disable notification interrupt */
	uad2_disable_vector(dev, INTR_VEC_NOTIFY);

	/* 8: Disconnect */
	uad2_write32(dev, REG_STREAM_ENABLE, 0x10);
	dev->connected = false;
	dev->transport_state = 0;

	dev_dbg(&dev->pci->dev, "Shutdown complete\n");
}

/* ============================================================
 * ALSA PCM hardware definitions
 *
 * The hardware operates on fixed 4MB DMA buffers.
 * Buffer frame size = 8192 (for 42ch playback at 48kHz).
 * Period = buffer_frames (single period = entire buffer, since
 * hardware uses a single interrupt per full buffer cycle).
 *
 * From ioreg IOAudioEngine (confirmed on live hardware):
 *   Format: 32-bit container, 24-bit depth, signed int, LE
 *   (IOAudioStreamByteOrder=1 on macOS = little-endian)
 *
 * Buffer structure: interleaved, all channels × buffer_frames × 4 bytes
 * The ALSA PCM buffer is mapped directly to the first portion of the
 * 4MB hardware DMA buffer.
 * ============================================================ */
static const struct snd_pcm_hardware uad2_pcm_hw_playback = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE, /* 24-in-32 LE, MSB-justified */
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = UAD2_OUT_CHANNELS,
	.buffer_bytes_max = SG_BUFFER_SIZE, /* 4MB max (entire HW buffer) */
	.period_bytes_min = 256,
	.period_bytes_max = SG_BUFFER_SIZE / 2,
	.periods_min = 2,
	.periods_max = 32,
};

static const struct snd_pcm_hardware uad2_pcm_hw_capture = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 |
		 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 |
		 SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000,
	.rate_min = 44100,
	.rate_max = 192000,
	.channels_min = 2,
	.channels_max = UAD2_IN_CHANNELS,
	.buffer_bytes_max = SG_BUFFER_SIZE,
	.period_bytes_min = 256,
	.period_bytes_max = SG_BUFFER_SIZE / 2,
	.periods_min = 2,
	.periods_max = 32,
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
static int uad2_pcm_open(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ss->runtime->hw = uad2_pcm_hw_playback;
		dev->playback_ss = ss;
	} else {
		ss->runtime->hw = uad2_pcm_hw_capture;
		dev->capture_ss = ss;
	}

	return 0;
}

static int uad2_pcm_close(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);

	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dev->playback_ss = NULL;
	else
		dev->capture_ss = NULL;

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

	dev_dbg(&dev->pci->dev, "hw_params: stream=%d buf_bytes=%zu dma=%pad\n",
		ss->stream, buf_bytes, &runtime->dma_addr);

	return 0;
}

static int uad2_pcm_hw_free(struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *runtime = ss->runtime;

	/* Don't actually free — the DMA buffer belongs to the device.
	 * Just clear the runtime pointers. */
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;

	return 0;
}

/*
 * pcm_prepare: program sample rate + transport registers
 *
 * Implements the combined sequence of:
 *   _setSampleClock()  -- set hardware clock
 *   PrepareTransport() -- program buffer/channel/interrupt registers
 */
static int uad2_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	int err;

	/* Set sample rate on hardware */
	err = uad2_set_sample_rate(dev, rt->rate);
	if (err)
		dev_warn(&dev->pci->dev,
			 "Sample rate set to %u may not have completed\n",
			 rt->rate);

	/* Use the hardware's computed buffer frame size,
	 * but also respect ALSA's requested buffer size */
	dev->buffer_frames =
		min((unsigned int)rt->buffer_size, dev->buffer_frames);
	dev->irq_period_frames = rt->period_size;

	/* Prepare transport with ALSA runtime parameters */
	err = uad2_prepare_transport(dev, dev->buffer_frames,
				     dev->irq_period_frames, dev->play_channels,
				     dev->rec_channels);

	return err;
}

static int uad2_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		uad2_start_transport(dev);
		return 0;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		uad2_stop_transport(dev);
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
	u32 pos;

	pos = uad2_read32(dev, REG_DMA_POSITION);

	/* Safety clamp (matches kext TransportPosition behavior) */
	if (pos >= ss->runtime->buffer_size)
		pos = 0;

	return pos;
}

static const struct snd_pcm_ops uad2_pcm_ops = {
	.open = uad2_pcm_open,
	.close = uad2_pcm_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = uad2_pcm_hw_params,
	.hw_free = uad2_pcm_hw_free,
	.prepare = uad2_pcm_prepare,
	.trigger = uad2_pcm_trigger,
	.pointer = uad2_pcm_pointer,
};

/* ============================================================
 * Interrupt handler
 *
 * The hardware has three MSI vectors (registered in kext Initialize):
 *   0x28 = notification interrupt → _notifyIntrCallback
 *   0x46 = periodic timer interrupt
 *   0x47 = end-of-buffer interrupt → _endBufferCallback
 *
 * With Linux MSI (single vector mode), all interrupts arrive on
 * vector 0.  We handle:
 *   1. Transport status (BAR+0x2248) for overflow/underflow
 *   2. Firmware notifications (BAR+(idx<<2)+0xC008) for connect/IO
 *   3. Period elapsed for ALSA
 * ============================================================ */
static irqreturn_t uad2_irq_handler(int irq, void *data)
{
	struct uad2_dev *dev = data;
	u32 transport_status;
	u32 notify_status;
	bool handled = false;

	/* Check transport status register */
	transport_status = uad2_read32(dev, REG_TRANSPORT_CTL);

	/* Check notification status register */
	notify_status =
		uad2_read32(dev, uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS));

	if (!transport_status && !notify_status)
		return IRQ_NONE;

	/* Handle firmware notifications (connect ack, channel config, etc.) */
	if (notify_status) {
		uad2_handle_notification(dev);
		handled = true;
	}

	/* Handle transport status */
	if (transport_status & BIT(5)) {
		/* Transport is running — check for errors */
		if (transport_status & BIT(7))
			dev_warn_ratelimited(&dev->pci->dev, "DMA overflow\n");
		if (transport_status & BIT(8))
			dev_warn_ratelimited(&dev->pci->dev, "DMA underflow\n");

		/* Notify ALSA of period elapsed */
		if (dev->playback_ss)
			snd_pcm_period_elapsed(dev->playback_ss);
		if (dev->capture_ss)
			snd_pcm_period_elapsed(dev->capture_ss);

		handled = true;
	}

	return handled ? IRQ_HANDLED : IRQ_NONE;
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

/* Sample rate read-only control (informational) */
static int uad2_sample_rate_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 44100;
	info->value.integer.max = 192000;
	info->value.integer.step = 0;
	return 0;
}

static int uad2_sample_rate_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	val->value.integer.value[0] = dev->current_rate ? dev->current_rate :
							  48000;
	return 0;
}

static const struct snd_kcontrol_new uad2_sample_rate_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.name = "Current Sample Rate",
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.info = uad2_sample_rate_info,
	.get = uad2_sample_rate_get,
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

	pci_set_master(pci);
	err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(64));
	if (err) {
		err = dma_set_mask_and_coherent(&pci->dev, DMA_BIT_MASK(32));
		if (err)
			goto err_release_regions;
	}

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

	/* Apollo Solo: 1 DSP, single DMA engine
	 * channel_base_index = 10 (from data segment @ 0x6018 in kext binary) */
	dev->dsp_count = 1;
	dev->dual_dma = false;
	dev->channel_base_index = UAD2_CHANNEL_BASE_IDX;
	dev->dma_ctrl_shadow = 0;
	dev->play_channels = UAD2_OUT_CHANNELS;
	dev->rec_channels = UAD2_IN_CHANNELS;
	dev->buffer_frames = UAD2_MAX_BUFFER_FRAMES;
	dev->clock_source = 0; /* internal */
	dev->current_rate = 48000;

	/* Allocate the two 4MB DMA buffers for scatter-gather */
	err = uad2_alloc_sg_buffers(dev);
	if (err)
		goto err_unmap_bar;

	/* MSI interrupt */
	err = pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (err < 0)
		goto err_free_sg;

	err = request_irq(pci_irq_vector(pci, 0), uad2_irq_handler, IRQF_SHARED,
			  "uad2", dev);
	if (err)
		goto err_free_irq_vectors;

	/* Hardware initialization (full sequence):
	 *   1. CPcieDevice::ProgramRegisters (device ID, DMA reset, DSP init)
	 *   2. CPcieAudioExtension::ProgramRegisters (SG tables, FW base, IRQ)
	 *   3. CPcieAudioExtension::Connect (20-channel firmware handshake)
	 */
	err = uad2_hw_program(dev);
	if (err) {
		dev_err(&pci->dev, "Hardware init failed: %d\n", err);
		goto err_free_irq;
	}

	/* ALSA card registration */
	strscpy(card->driver, "uad2", sizeof(card->driver));
	strscpy(card->shortname, "UA UAD2", sizeof(card->shortname));
	strscpy(card->longname, "Universal Audio UAD2 Thunderbolt",
		sizeof(card->longname));

	err = snd_pcm_new(card, "UAD2", 0, 1, 1, &dev->pcm);
	if (err < 0)
		goto err_free_irq;

	dev->pcm->private_data = dev;
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
	dev_info(
		&pci->dev,
		"UAD2 initialized (v0.5.0) — play=%uch rec=%uch buf=%u frames\n",
		dev->play_channels, dev->rec_channels, dev->buffer_frames);
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

	/* Full shutdown sequence (mirrors kext CPcieAudioExtension::Shutdown):
	 * stops transport, clears doorbell, disables notification vector,
	 * sends disconnect command to firmware */
	uad2_shutdown(dev);

	/* Disable global interrupt enable */
	uad2_write32(dev, REG_INTR_ENABLE, 0x0);

	snd_card_free(dev->card);
	free_irq(pci_irq_vector(pci, 0), dev);
	pci_free_irq_vectors(pci);

	/* Free DMA buffers */
	uad2_free_sg_buffers(dev);
	if (dev->ring_dma_buf[0]) {
		dma_free_coherent(&dev->pci->dev,
				  DSP_RING_DESC_SLOTS * DSP_RING_PAGE_SIZE,
				  dev->ring_dma_buf[0], dev->ring_dma_addr[0]);
	}

	pci_iounmap(pci, dev->bar);
	pci_release_regions(pci);
	pci_disable_device(pci);
}

static const struct pci_device_id uad2_pci_ids[] = {
	{ /* Apollo Solo */
	  PCI_DEVICE_SUB(UA_VENDOR_ID, UAD2_DEVICE_ID_SOLO, UA_VENDOR_ID,
			 UAD2_SUBSYS_ID_SOLO) },
	/* TODO: Add other UAD2 Thunderbolt devices here:
	 * Apollo Twin, Apollo x4, Apollo x6, Apollo x8, Apollo x8p,
	 * Apollo x16, Apollo Twin X, Apollo Solo USB, etc.
	 * All share vendor 0x1a00, device 0x0002; differ by subsystem ID. */
	{ 0 }
};
MODULE_DEVICE_TABLE(pci, uad2_pci_ids);

static struct pci_driver uad2_driver = {
	.name = "uad2",
	.id_table = uad2_pci_ids,
	.probe = uad2_probe,
	.remove = uad2_remove,
};

module_pci_driver(uad2_driver);

MODULE_AUTHOR("Yifei Sun");
MODULE_DESCRIPTION("Universal Audio UAD2 Thunderbolt audio driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION("0.5.0");
