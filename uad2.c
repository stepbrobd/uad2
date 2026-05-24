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

/* All Apollo Thunderbolt devices share this vendor/device ID;
 * model identified later by serial prefix and subsystem ID. */
#define UA_VENDOR_ID 0x1a00
#define UA_DEVICE_ID 0x0002

/* Device type IDs from open-apollo ua_apollo.h, reconstructed from
 * kext CPcieDevice::Name() and _deviceTypeFromSerialNumber(). */
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

/* Serial prefix at BAR0+0x20..0x2C: 16-byte ASCII string, model digits
 * at offset +4.  Apollo Solo's serial contains "2032" which falsely
 * matches x16D, so PCI subsystem ID is preferred for known models. */
#define UA_REG_SERIAL_BASE 0x0020
#define UA_REG_SERIAL_LEN 16

/* PCI subsystem IDs (constant per model, more reliable than serial prefix) */
#define UA_SUBSYS_SOLO 0x000F /* Apollo Solo (confirmed via ioreg) */
#define UA_SUBSYS_X4_QUAD 0x0011 /* Apollo x4 Quad (from open-apollo) */

/* FPGA revision; bit 31 distinguishes v1 (clear) from v2 (set) firmware */
#define REG_FPGA_REV 0x2218
/* Extended capabilities (v2 firmware): bits[25:20] = family, bits[15:8] = DSP count */
#define REG_EXT_CAPS 0x2234

/* BAR register offsets (all 32-bit MMIO, BAR 0 = 64 KB window) */

#define REG_INTR_ENABLE 0x0014 /* W 0xFFFFFFFF to unmask */

#define REG_FW_BASE_LO 0x0030 /* R: firmware base low 32 */
#define REG_FW_BASE_HI 0x0034 /* R: firmware base high 32 */

/* DMA master control (CPcieIntrManager).  Software shadow, never read
 * from HW.  EnableDMA sets bit (1 << (dsp_index+1)); bit 0 reserved. */
#define REG_DMA_MASTER_CTRL 0x2200
#define DMA_CTRL_MASTER_ENABLE 0x0001 /* bit 0: global master enable */
#define DMA_RESET_SINGLE 0x1e00 /* bits[12:9]: single engine reset */
#define DMA_RESET_DUAL 0x1fe00 /* bits[16:9]: dual engine reset */
#define DMA_SHADOW_MASK_SINGLE 0xffffffe1 /* clears bits[4:1] */
#define DMA_SHADOW_MASK_DUAL 0xfffffe01 /* clears bits[8:1] */

/* DMA engine 0 (CPcieIntrManager::ProgramRegisters) */
#define REG_DMA0_INTR_CTRL 0x2204 /* W 0x0 to disable/clear */
#define REG_DMA0_STATUS 0x2208 /* W 0xFFFFFFFF to arm; W 0x0 to clear */

/* Device identification (CPcieDevice::ProgramRegisters) */
#define REG_DEVICE_ID 0x2218 /* R to verify; polled after clock change */
#define REG_DEVICE_HANDSHAKE 0x2220 /* W 0x0 after reading device ID */

/* Audio transport registers (CPcieAudioExtension).
 * REG_TRANSPORT_CTL values: 0x000 stop, 0x001 armed, 0x00F running,
 * 0x20F running extended (variant 0xA/0x9).  Read status: bit5=running,
 * bit7=overflow, bit8=underflow. */
#define REG_BUFFER_FRAME_SIZE 0x2240 /* W: bufferFrameSize - 1 */
#define REG_DMA_POSITION 0x2244 /* R: frame position (wrapping); W 0 clears */
#define REG_TRANSPORT_CTL 0x2248
#define REG_PLAYBACK_MON_CFG 0x224C /* W: (totalPlayChans-1) | 0x100 */
#define REG_PLAYBACK_CHAN_CNT 0x2250 /* W: total playback channels */
#define REG_POLL_STATUS 0x2254 /* R: poll for DMA ready vs irq_period */
#define REG_IRQ_PERIOD 0x2258 /* W: interrupt period in frames */
#define REG_RECORD_CHAN_CNT 0x225C /* W: record channel count */
#define REG_STREAM_ENABLE 0x2260 /* W: 0x1 start, 0x10 stop, 0x4 clock change */

/* DMA engine 1 (dual-engine devices only).
 * Roles of +0 and +4 are swapped vs DMA0:
 *   0x2264 = arm/status (like DMA0's 0x2208)
 *   0x2268 = enable ctrl (like DMA0's 0x2204) */
#define REG_DMA1_STATUS 0x2264 /* W bitmask to arm; R pending */
#define REG_DMA1_INTR_CTRL 0x2268 /* W enable shadow */

#define REG_BUFFER_SIZE_KB 0x226C /* W: (totalPlayChans * (bufSz-1)) >> 10 */
#define REG_PERIODIC_TIMER 0x2270 /* W: periodic timer interval in frames */

#define REG_PLAYBACK_MON_STAT 0x22C0 /* R: playback monitor status */
#define REG_SECONDARY_CTL 0x22C4 /* W: 0 on Shutdown */

/* DSP Mixer registers (BAR0 + 0x3800 window).
 * Sequence-counter handshake: read SEQ_RD (wait DSP idle: RD == WR),
 * write paired (value, mask) words to MIXER_SETTING[index], bump SEQ_WR.
 * DSP then reads all 38 settings atomically.  From open-apollo. */
#define REG_MIXER_BASE 0x3800
#define REG_MIXER_SEQ_WR 0x3808 /* host writes; DSP reads */
#define REG_MIXER_SEQ_RD 0x380C /* DSP writes; host reads */
#define REG_MIXER_RB_STATUS 0x3810 /* readback: 1=ready, W 0 to re-arm */
#define REG_MIXER_RB_DATA 0x3814 /* readback: 40 consecutive u32 words */
#define MIXER_SETTING_STRIDE 8 /* two u32 per setting */
#define MIXER_BATCH_COUNT 38
#define MIXER_RB_WORDS 40
#define DSP_SERVICE_INTERVAL_MS 10 /* readback drain + mixer flush rate */

/* Scatter-Gather DMA tables (ProgramRegisters @ 0x4bac0).
 * Buffer A (playback): BAR+0x8000..0x9FFF = 1024 entries of 8 bytes
 * (low32 phys, high32 phys) for one 4KB page each.
 * Buffer B (capture):  BAR+0xA000..0xBFFF = same layout. */
#define REG_SG_TABLE_A_BASE 0x8000
#define REG_SG_TABLE_A_END 0xA000
#define REG_SG_TABLE_B_OFFSET 0x2000 /* Capture SG table = A + 0x2000 */
#define SG_ENTRY_SIZE 8
#define SG_NUM_ENTRIES 1024
#define SG_PAGE_SIZE 0x1000
#define SG_BUFFER_SIZE 0x400000 /* 4 MB total DMA buffer */

/* Sample clock (CPcieAudioExtension::_setSampleClock).
 * W: clock_source | (rate_enum << 8) at BAR + (dev_idx*4) + 0xC04C. */
#define REG_SAMPLE_CLOCK 0xC04C

/* Firmware mailbox (notification/doorbell).
 * Address = BAR + (channel_base_index << 2) + offset. */
#define REG_FW_DOORBELL 0xC004 /* W: 0x0ACEFACE as connect command */
#define REG_FW_NOTIFY_STATUS \
	0xC008 /* R/W: notification bitmask; W 0 to clear */
#define DMA_DESC_MAGIC 0x0aceface

/* Bank-shifted notification readback registers (add channel_base_index<<2).
 * Firmware expects host reads after connect and on events. */
#define REG_NOTIF_RATE_INFO 0xC054
#define REG_NOTIF_XPORT_INFO 0xC058
#define REG_NOTIF_CLOCK_INFO 0xC05C

/* Firmware notification status bits (BAR+0xC008 bitmask) */
#define NOTIFY_PLAYBACK_IO BIT(0)
#define NOTIFY_RECORD_IO BIT(1)
#define NOTIFY_DMA_READY BIT(4)
#define NOTIFY_CONNECT_ACK BIT(5)
#define NOTIFY_ERROR BIT(6)
#define NOTIFY_END_BUFFER BIT(7)
#define NOTIFY_CHAN_CONFIG BIT(21)
#define NOTIFY_RATE_CHANGE BIT(22)

/* Playback/Record IO descriptor areas (72 DWORDs = 0x120 bytes each) */
#define REG_RECORD_IO_DESC 0xC1A4
#define REG_PLAYBACK_IO_DESC 0xC2C4
#define IO_DESC_DWORDS 72

#define REG_CHAN_CONFIG_BASE 0xC000 /* R: 10 DWORDs of channel config */
#define CHAN_CONFIG_DWORDS 10

/* DSP ring buffer registers (relative to ring_base).
 * For DSP index n: ring_base = BAR + (n<4 ? 0x2000 : 0x5e00) + n*0x80.
 * Second ring at ring_base + 0x40.
 * From CPcieRingBuffer::ProgramRegisters @ 0x14c48: 4 descriptor slots
 * at +0x00..0x1C (8 bytes each, low/high 32-bit phys), pages
 * 0x0000/0x1000/0x2000/0x3000.  All writes verified via read-back. */
#define DSP_RING_BASE_LOW 0x2000
#define DSP_RING_BASE_HIGH 0x5e00
#define DSP_RING_STRIDE 0x80
#define DSP_RING2_OFFSET 0x40

#define DSP_RING_DESC0_LO 0x00
#define DSP_RING_DESC0_HI 0x04
#define DSP_RING_DESC1_LO 0x08
#define DSP_RING_DESC1_HI 0x0C
#define DSP_RING_DESC2_LO 0x10
#define DSP_RING_DESC2_HI 0x14
#define DSP_RING_DESC3_LO 0x18
#define DSP_RING_DESC3_HI 0x1C
#define DSP_RING_DESC_COUNT 0x20 /* W: descriptor count = ring_size */
#define DSP_RING_SIZE_REG 0x24 /* W: ring size */
#define DSP_RING_CAPACITY 0x28 /* R: HW ring capacity (cap at 0x400) */
#define DSP_READY_POLL_OFF 0x1a4 /* R: DSP ready poll, wait for DSP_READY_SIG */

#define DSP_RING_DESC_SLOTS 4
#define DSP_RING_PAGE_SIZE 0x1000 /* 4 KB per descriptor page */
#define DSP_READY_SIG 0xa8caed0f
#define DSP_POLL_INTERVAL_MS 300
#define DSP_POLL_MAX_PRIMARY 100 /* DSP type 0: 30s timeout */
#define DSP_POLL_MAX_SECONDARY 10 /* others: 3s timeout */

/* UAD2SampleRate enum (1-based, from _setSampleClock analysis) */
#define UA_RATE_44100 1
#define UA_RATE_48000 2
#define UA_RATE_88200 3
#define UA_RATE_96000 4
#define UA_RATE_176400 5
#define UA_RATE_192000 6

/* Audio engine parameters */
#define UAD2_MAX_BUFFER_FRAMES 8192 /* max from _recomputeBufferFrameSize cap */
#define UAD2_SAMPLE_OFFSET 16
#define UAD2_BYTES_PER_SAMPLE 4 /* 24-bit in 32-bit container */
#define UAD2_MAX_DSPS 8
#define UAD2_MAX_CHANNELS 64

/* channel_base_index = bank_shift for notification register addressing.
 * All Apollo Thunderbolt devices use 0x0A (verified on Solo, x4). */
#define UAD2_CHANNEL_BASE_IDX 10

/* Serial prefix to device type lookup, from open-apollo ua_core.c
 * (extracted from kext _deviceTypeFromSerialNumber() data @ 0x3E840) */
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

/* Per-model channel counts and capabilities, from open-apollo ua_audio.c
 * (macOS IOKit IOAudioEngine properties).  These are PCIe DMA channel
 * counts, not physical I/O count. */
struct ua_model_info {
	u32 device_type;
	unsigned int play_ch;
	unsigned int rec_ch;
	unsigned int num_preamps;
	unsigned int num_hiz;
};

static const struct ua_model_info ua_models[] = {
	/* Solo/Arrow: open-apollo lists 3/2 (physical I/O); macOS ioreg
	 * shows DMA play=42 rec=32 for Solo.  Arrow assumed same (same
	 * chipset).  Firmware IO descriptors override after connect. */
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

/* IRQ period lookup, kext data segment @ 0x6020.  _setSampleClock loads
 * this into this+0x2880; PrepareTransport writes to REG_IRQ_PERIOD. */
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

/* Periodic timer interval from kext CUAD2AudioMixer::StartTransport.
 * Drives vector 0x46 (_periodicTimerCallback), the actual audio clock
 * that triggers ProcessAudio.  Vector 0x47 (_endBufferCallback) passes
 * callback ID 0x6c which the mixer ignores, so the Linux driver must
 * use the periodic timer interrupt for snd_pcm_period_elapsed(). */
static unsigned int uad2_periodic_timer_for_rate(unsigned int rate)
{
	switch (rate) {
	case 44100:
	case 48000:
		return 255;
	case 88200:
	case 96000:
		return 511;
	case 176400:
	case 192000:
		return 1023;
	default:
		return 255;
	}
}

/* Interrupt vector enable bitmask management.
 *
 * CPcieIntrManager::EnableVector @ 0x13cd4 (line 12349):
 *   slot = IntrManager[(vector_id << 2) + 0x4D0]
 *   shadow |= (1 << slot)
 *   if arm: write (1 << slot) to BAR+0x2208 (DMA0 status)
 *   write shadow to BAR+0x2204 (DMA0 enable ctrl)
 *   if dual-DMA: write to BAR+0x2264/0x2268
 *
 * Vector assignments from CPcieDSP::Initialize @ 0x10d60:
 *   base = dsp_index * 5  (Apollo Solo dsp_index=0 yields base=0)
 *   vec+0,+1 = ring callbacks  (not enabled by AudioExtension)
 *   vec+2,+3,+4 = error callbacks (enabled immediately)
 *
 * CPcieAudioExtension vectors:
 *   0x28 notification, 0x46 periodic timer, 0x47 end-of-buffer.
 *
 * Linux skips the kext's vector_to_slot[] indirection and uses slot
 * values directly for these three known vectors. */

/* Driver private state */
struct uad2_dev {
	struct pci_dev *pci;
	struct snd_card *card;
	struct snd_pcm *pcm;

	void __iomem *bar; /* 64 KB MMIO window */
	resource_size_t bar_len;

	spinlock_t lock; /* HW register access (mirrors kext hw_lock @+0x10) */
	spinlock_t
		notify_lock; /* notification handler (kext spinlock @+0x2890) */

	u32 expected_device_id;
	u32 device_type; /* UA_DEV_APOLLO_* enum */
	u32 fpga_rev; /* cached FPGA revision from 0x2218 */
	bool fw_v2; /* v2 firmware (bit 31 of fpga_rev set) */
	bool has_extended_irq; /* uses DMA1 regs for upper 32 slots */
	bool disconnecting; /* set during remove, guards MMIO access */
	unsigned int dsp_count;
	u32 channel_base_index; /* 10 (0x0A) for all Apollo TB devices */

	u32 dma_ctrl_shadow; /* DMA master control shadow (never read from HW) */
	u64 intr_enable_shadow; /* interrupt enable bitmask (kext IntrManager+0x20) */

	/* Active interrupt bits accumulated from hardirq top-half,
	 * consumed by threaded bottom-half.  Protected by dev->lock. */
	u64 irq_active;

	/* Ring buffer DMA allocations (4 x 4KB pages per ring) */
	dma_addr_t ring_dma_addr[DSP_RING_DESC_SLOTS];
	void *ring_dma_buf[DSP_RING_DESC_SLOTS];

	/* Two 4MB DMA buffers (playback + capture scatter-gather source).
	 * Page addresses are written to BAR+0x8000..0x9FFF (playback) and
	 * BAR+0xA000..0xBFFF (capture).  ALSA PCM buffers map directly here. */
	dma_addr_t sg_dma_addr[2]; /* [0]=playback, [1]=capture */
	void *sg_dma_buf[2];
	size_t sg_buf_size; /* 0x400000 = 4MB */

	u64 fw_base_addr; /* read from BAR+0x30/0x34 after SG programming */

	/* Audio parameters (computed from channel config or defaults) */
	unsigned int buffer_frames; /* from _recomputeBufferFrameSize */
	unsigned int irq_period_frames;
	unsigned int periodic_timer_interval; /* kext this+0x28C0 */
	unsigned int play_channels;
	unsigned int rec_channels;
	unsigned int clock_source; /* 0=internal, 1=SPDIF */
	unsigned int current_rate; /* current sample rate in Hz */

	/* Transport state: 0=uninit, 1=prepared, 2=running (kext this+0x2878) */
	int transport_state;

	/* The hardware has one shared transport for both directions.  We
	 * reference-count active streams so StopTransport only fires when
	 * the LAST stream stops.  Protected by dev->lock. */
	int streams_running;

	/* IRQ thread uses these to decide which substream gets
	 * snd_pcm_period_elapsed().  Protected by dev->lock. */
	bool playback_running;
	bool capture_running;

	/* Substreams that have called pcm_prepare but not yet hw_free.
	 * Per-direction flags are required because a single shared flag
	 * on snd_pcm_runtime is unreliable under JOINT_DUPLEX with
	 * overlapping prepare/hw_free across the two directions.
	 * Protected by dev->lock. */
	int streams_prepared;
	bool play_prepared;
	bool rec_prepared;

	/* Substreams open (between pcm_open and pcm_close).  Transport
	 * tears down only when this reaches 0, NOT on hw_free.  Following
	 * snd-dice/snd-bebob: lifecycle tied to open/close, not hw_params.
	 * Without this PipeWire's rapid stop, hw_free, prepare, start
	 * reconfiguration would briefly drive streams_prepared to 0 and
	 * destroy a transport about to be re-prepared.  Protected by
	 * dev->lock. */
	int open_count;

	/* Notification interrupt handling */
	struct completion notify_event; /* signals Connect() on bit 21 */
	struct completion connect_event; /* signals on bit 5 (connect ack) */
	/* Last notification bitmask read by the handler.  Set under
	 * notify_lock, polled by uad2_set_sample_rate as a fallback because
	 * Apollo's TB-tunneled MSI delivery is unreliable. */
	u32 notify_status;
	bool connected;

	/* Firmware IO descriptor copies (72 DWORDs each).  Channel count
	 * lives at DWORD[4] (offset +0x10). */
	u32 play_io_desc[IO_DESC_DWORDS];
	u32 rec_io_desc[IO_DESC_DWORDS];

	u32 chan_config[CHAN_CONFIG_DWORDS]; /* 10 DWORDs from BAR+0xC000 */

	/* Mixer batch write state (open-apollo CPcieDeviceMixer protocol).
	 * DSP reads all 38 settings atomically per SEQ_WR advance. */
	u32 mixer_val[MIXER_BATCH_COUNT];
	u32 mixer_mask[MIXER_BATCH_COUNT];
	u32 mixer_seq_wr;
	bool mixer_dirty;
	bool mixer_ready;
	u32 mixer_rb_data[MIXER_RB_WORDS];

	/* Monitor state cache, written to DSP mixer setting[2] */
	struct {
		int level; /* 0-192 raw (192 + dB*2; 0=-96dB, 192=0dB) */
		int mute; /* 0=unmuted, 2=muted (UA_MON_MUTE encoding) */
		bool dim;
		int source; /* 0=MIX, 1=CUE1, 2=CUE2 */
	} monitor;

	/* Preamp state cache, indexed by preamp channel */
	struct {
		int gain; /* dB value (-144..+65) */
		bool phantom; /* 48V phantom power */
		bool pad;
		bool lowcut;
		bool phase; /* true=inverted */
		bool mic_line; /* false=Mic, true=Line */
	} preamp[UAD2_MAX_DSPS];

	unsigned int num_preamps;

	bool pcie_setup_done;

	/* DSP service loop: periodic readback drain + mixer flush.  Without
	 * this the DSP firmware halts and mixer settings never apply.
	 * Runs at DSP_SERVICE_INTERVAL_MS via delayed_work. */
	struct delayed_work dsp_service_work;
	bool dsp_service_running;
	unsigned int dsp_service_count;

	/* PCM substream pointers for IRQ handler (accessed under lock) */
	struct snd_pcm_substream *playback_ss;
	struct snd_pcm_substream *capture_ss;

	/* Per-direction DMA position offsets for hw_ptr alignment.  When
	 * pcm_prepare runs while the transport is already going, ALSA
	 * resets hw_ptr to 0 but REG_DMA_POSITION sits at some arbitrary
	 * value.  We snapshot the HW position at prepare time and subtract
	 * it in .pointer() so ALSA sees positions starting from 0.
	 *
	 * MUST be per-direction.  A shared offset would break playback's
	 * pointer tracking if capture opens later (capture's offset would
	 * clobber playback's), causing ALSA to return -EIO. */
	u32 play_pos_offset;
	u32 rec_pos_offset;

	/* Period-elapsed polling timer.  The hardware periodic timer IRQ
	 * (vector 0x46) is unreliable on Apollo (fires once then stops).
	 * We poll REG_DMA_POSITION via hrtimer at ~1ms, same approach as
	 * snd-usb-audio.
	 *
	 * cached_period_frames is the period_size the streams were prepared
	 * with.  Cached so the hrtimer never dereferences snd_pcm_runtime
	 * (the ALSA core may free that struct between two ticks if hw_free
	 * races with the timer). */
	struct hrtimer period_timer;
	bool period_timer_running;
	snd_pcm_uframes_t cached_period_frames;
	snd_pcm_uframes_t last_play_period;
	snd_pcm_uframes_t last_rec_period;
};

/* Forward declarations */
static unsigned int uad2_compute_buffer_frames(unsigned int play_ch,
					       unsigned int rec_ch);
static void uad2_period_timer_stop(struct uad2_dev *dev);
static void uad2_handle_notification(struct uad2_dev *dev);

/* Low-level register accessors (mirror CUAOS::ReadReg / CUAOS::WriteReg).
 * Hot-unplug yields 0xFFFFFFFF MMIO reads.  The disconnecting flag is
 * set early in uad2_remove() so we can skip MMIO to a vanished device. */
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

/* Mixer setting register offset.  From open-apollo ua_apollo.h.
 * Settings live in 3 non-contiguous ranges within the 0x3800 window:
 *   0..15:  base 0xB4 + index*8
 *   16..31: base 0xBC + index*8 (8-byte gap)
 *   32..37: base 0xC0 + index*8 (extra 4-byte gap). */
static inline u32 uad2_mixer_setting_reg(unsigned int index)
{
	if (index <= 15)
		return REG_MIXER_BASE + 0xB4 + index * MIXER_SETTING_STRIDE;
	else if (index <= 31)
		return REG_MIXER_BASE + 0xBC + index * MIXER_SETTING_STRIDE;
	else
		return REG_MIXER_BASE + 0xC0 + index * MIXER_SETTING_STRIDE;
}

/* Mixer batch write protocol.  Ported from open-apollo ua_core.c.
 * Maintain cached val/mask arrays for all 38 settings; on each service
 * tick, if dirty AND SEQ_RD == cached SEQ_WR, write all 38 settings
 * to SRAM and bump SEQ_WR once.  The DSP reads them atomically.
 * Per-setting encoding: wordA = (mask[15:0]<<16) | val[15:0],
 * wordB = (mask[31:16]<<16) | val[31:16]. */
static void __maybe_unused uad2_mixer_write_setting(struct uad2_dev *dev,
						    unsigned int index,
						    u32 value, u32 mask)
{
	if (index >= MIXER_BATCH_COUNT)
		return;
	if (!dev->mixer_ready)
		return;

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

	seq_rd = uad2_read32(dev, REG_MIXER_SEQ_RD);
	if (seq_rd != dev->mixer_seq_wr)
		return; /* DSP still processing previous batch */

	/* Skip settings with no mask, preserving firmware defaults.
	 * Val and mask persist across flushes; do NOT clear mask. */
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

	dev->mixer_seq_wr++;
	uad2_write32(dev, REG_MIXER_SEQ_WR, dev->mixer_seq_wr);
	dev->mixer_dirty = false;
}

/* Initialize mixer batch protocol after connect.  From open-apollo
 * ua_audio.c:1009-1072.  Do NOT write any settings here: the device
 * persists all mixer/monitor/preamp/HP config across power cycles and
 * TB reconnect, and writing would clobber user-configured values. */
static void uad2_mixer_init(struct uad2_dev *dev)
{
	u32 seq_rd;

	seq_rd = uad2_read32(dev, REG_MIXER_SEQ_RD);
	dev->mixer_seq_wr = seq_rd;
	uad2_write32(dev, REG_MIXER_SEQ_WR, seq_rd);

	memset(dev->mixer_val, 0, sizeof(dev->mixer_val));
	memset(dev->mixer_mask, 0, sizeof(dev->mixer_mask));
	dev->mixer_dirty = false;
	dev->mixer_ready = true;

	dev_dbg(&dev->pci->dev,
		"mixer init (SEQ=%u, device config preserved)\n", seq_rd);
}

/* DSP service loop: periodic readback drain + mixer flush.
 *
 * The Apollo DSP firmware halts without periodic "servicing" from the
 * host.  The macOS kext drives CDSPResourceManager::Service() ~103/sec;
 * without it the front panel freezes and mixer SEQ_RD never advances.
 * The readback drain cycle (read status, read data, write 0 to re-arm)
 * acts as the host liveness signal.  Ported from open-apollo
 * ua_audio.c:434-539. */
static void uad2_dsp_service_handler(struct work_struct *work)
{
	struct uad2_dev *dev = container_of(to_delayed_work(work),
					    struct uad2_dev, dsp_service_work);
	u32 rb_status, notif;
	u32 notify_reg = uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS);
	unsigned int i;

	if (READ_ONCE(dev->disconnecting) || !dev->dsp_service_running)
		return;

	/* Notification register is NOT write-1-to-clear; write 0 to clear.
	 * The DSP may stall if its notifications are never acknowledged. */
	notif = uad2_read32(dev, notify_reg);
	if (notif == 0xFFFFFFFF) {
		/* Device gone (Thunderbolt hot-unplug) */
		dev->dsp_service_running = false;
		return;
	}
	if (notif)
		uad2_write32(dev, notify_reg, 0x0);

	/* Drain readback data and re-arm.  This is the host liveness signal. */
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

	if (dev->dsp_service_running)
		schedule_delayed_work(
			&dev->dsp_service_work,
			msecs_to_jiffies(DSP_SERVICE_INTERVAL_MS));
}

static void uad2_dsp_service_start(struct uad2_dev *dev)
{
	if (dev->dsp_service_running)
		return;

	dev_dbg(&dev->pci->dev, "starting DSP service loop (%u ms)\n",
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

/* Monitor parameter routing.  All write to setting[2] (monitor core).
 * From open-apollo ua_core.c ua_monitor_set_param() and ua_apollo.h. */
#define MON_PARAM_LEVEL 0x01 /* Monitor volume: setting[2] bits[7:0] */
#define MON_PARAM_MUTE 0x03 /* Mute: setting[2] bits[17:16] */
#define MON_PARAM_SOURCE 0x04 /* Source: setting[2] bits[19:18]+[29] */
#define MON_PARAM_DIM 0x44 /* Dim: setting[2] bit 31 */
#define MON_MUTE_ON 2
#define MON_MUTE_OFF 0

static void uad2_monitor_set_param(struct uad2_dev *dev, u32 param_id,
				   u32 value)
{
	u32 full_word;

	switch (param_id) {
	case MON_PARAM_LEVEL:
		dev->monitor.level = value & 0xFF;
		break;
	case MON_PARAM_MUTE:
		dev->monitor.mute = value;
		break;
	case MON_PARAM_SOURCE:
		dev->monitor.source = value;
		break;
	case MON_PARAM_DIM:
		dev->monitor.dim = !!value;
		break;
	default:
		return;
	}

	/* Rebuild the complete setting[2] word with full mask.  The DSP
	 * activates the monitor processing module only on a full word
	 * commit; partial-mask writes update fields but never trigger
	 * the module to start processing audio. */
	full_word = (dev->monitor.level & 0xFF); /* bits[7:0] */
	if (dev->monitor.mute == MON_MUTE_ON)
		full_word |= 0x00010000; /* bits[17:16]: mute encoding */
	/* bits[19:18] + bit[29]: source */
	full_word |= ((dev->monitor.source & 3) << 18) |
		     (((dev->monitor.source >> 2) & 1) << 29);
	if (dev->monitor.dim)
		full_word |= BIT(31);

	uad2_mixer_write_setting(dev, 2, full_word, 0xFFFFFFFF);
}

/* Preamp parameter routing: setting[param_id + 7], full 0xFFFFFFFF mask.
 * From open-apollo hardware.py and kext SetMixerParam ch_type=1. */
#define PREAMP_PARAM_MIC_LINE 0x00 /* 0=Mic, 1=Line */
#define PREAMP_PARAM_PAD 0x01
#define PREAMP_PARAM_48V 0x03
#define PREAMP_PARAM_LOWCUT 0x04
#define PREAMP_PARAM_PHASE 0x05 /* IEEE 754: +1.0f / -1.0f */
#define PREAMP_PARAM_GAIN_A 0x06 /* First gain param (dB value) */
#define PREAMP_SETTING_OFFSET 7 /* setting index = param_id + 7 */

#define PHASE_NORMAL 0x3F800000 /* IEEE 754 +1.0f */
#define PHASE_INVERT 0xBF800000 /* IEEE 754 -1.0f */

static void uad2_preamp_set_param(struct uad2_dev *dev, unsigned int ch,
				  u32 param_id, u32 value)
{
	unsigned int setting_idx;

	if (ch >= dev->num_preamps)
		return;

	setting_idx = param_id + PREAMP_SETTING_OFFSET;
	if (setting_idx >= MIXER_BATCH_COUNT)
		return;

	switch (param_id) {
	case PREAMP_PARAM_MIC_LINE:
		dev->preamp[ch].mic_line = !!value;
		break;
	case PREAMP_PARAM_PAD:
		dev->preamp[ch].pad = !!value;
		break;
	case PREAMP_PARAM_48V:
		dev->preamp[ch].phantom = !!value;
		break;
	case PREAMP_PARAM_LOWCUT:
		dev->preamp[ch].lowcut = !!value;
		break;
	case PREAMP_PARAM_PHASE:
		dev->preamp[ch].phase = !!value;
		value = value ? PHASE_INVERT : PHASE_NORMAL;
		break;
	case PREAMP_PARAM_GAIN_A:
		dev->preamp[ch].gain = (int)value;
		break;
	default:
		return;
	}

	uad2_mixer_write_setting(dev, setting_idx, value, 0xFFFFFFFF);
}

/* Device detection: read serial prefix, look up model, set channels.
 * Ported from open-apollo ua_core.c ua_read_serial_type() and
 * ua_detect_capabilities(). */
static void uad2_detect_device(struct uad2_dev *dev)
{
	char serial[UA_REG_SERIAL_LEN + 1];
	u32 regs[4];
	u32 ext_caps;
	u16 subsys;
	int i;

	dev->fpga_rev = uad2_read32(dev, REG_FPGA_REV);

	/* Subsystem ID is the most reliable identifier for known models;
	 * serial prefix detection mis-matches some models (Apollo Solo
	 * contains "2032" which collides with x16D). */
	subsys = dev->pci->subsystem_device;
	switch (subsys) {
	case UA_SUBSYS_SOLO:
		dev->device_type = UA_DEV_APOLLO_SOLO;
		dev_dbg(&dev->pci->dev, "subsystem 0x%04x is Apollo Solo\n",
			subsys);
		break;
	case UA_SUBSYS_X4_QUAD:
		dev->device_type = UA_DEV_APOLLO_X4;
		dev_dbg(&dev->pci->dev, "subsystem 0x%04x is Apollo x4\n",
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

		dev_dbg(&dev->pci->dev, "ext_caps=0x%08x dsp=%u\n", ext_caps,
			dev->dsp_count);

		for (i = 0; i < 4; i++)
			regs[i] = uad2_read32(dev, UA_REG_SERIAL_BASE + i * 4);
		memcpy(serial, regs, UA_REG_SERIAL_LEN);
		serial[UA_REG_SERIAL_LEN] = '\0';
		dev_dbg(&dev->pci->dev, "serial: %.16s\n", serial);

		/* Fall back to serial prefix only when subsystem ID missed */
		if (!dev->device_type) {
			for (i = 0; i < (int)ARRAY_SIZE(ua_serial_table); i++) {
				if (!strncmp(serial + 4,
					     ua_serial_table[i].prefix, 4)) {
					dev->device_type =
						ua_serial_table[i].device_type;
					dev_dbg(&dev->pci->dev,
						"serial prefix is %s\n",
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
			dev->num_preamps = ua_models[i].num_preamps;
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

/* PCIe setup: disable ASPM, increase completion timeouts.
 *
 * The Thunderbolt bridge chain may park the PCIe link in L1 via ASPM.
 * MMIO reads then stall for up to 10 seconds (PCIe completion timeout).
 * Since the DSP service loop reads BAR0 every 10ms, a sleeping link
 * cascades into a system freeze.
 *
 * Walks the bridge chain from device to root port, clearing LNKCTL
 * bits[1:0] and setting completion timeout to range D (65-210ms).
 * Ported from open-apollo ua_audio.c ua_pcie_setup().  Called once
 * from probe after BAR mapping. */
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

		dev_dbg(&dev->pci->dev,
			"PCIe: LnkCtl=0x%04x LnkSta=0x%04x Gen%u x%u\n", lnkctl,
			lnksta, lnksta & 0xF, (lnksta >> 4) & 0x3F);

		/* Disable ASPM on device (bits[1:0] of LNKCTL) */
		if (lnkctl & 3) {
			dev_dbg(&dev->pci->dev,
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
				dev_dbg(&dev->pci->dev,
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
	dev_dbg(&dev->pci->dev, "PCIe ASPM/timeout setup complete\n");
}

/* Sample rate helpers */
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

/* CPcieAudioExtension::_setSampleClock @ line 72712 (arm64 0x4DE70).
 *
 * Kext flow: compute clock_val = clock_source | (rate_enum << 8), write
 * to BAR + (channel_base_index * 4) + 0xC04C, write 0x4 to
 * REG_STREAM_ENABLE, then wait 2000 ms on a completion signaled by the
 * notification ISR (bit 5).
 *
 * Apollo's Thunderbolt-tunneled PCIe drops MSIs (verified empirically:
 * /proc/interrupts shows ~7 IRQs per session even during continuous
 * playback).  Waiting on the IRQ-signaled completion therefore times
 * out almost every time, so we poll the notification register directly
 * every 10 ms for the rate-change ack (bits 22, 4 or 5), exactly like
 * open-apollo's ua_audio_set_clock (ua_audio.c:1835). */
#define UAD2_RATE_ACK_BITS \
	(NOTIFY_RATE_CHANGE | NOTIFY_DMA_READY | NOTIFY_CONNECT_ACK)
#define UAD2_RATE_POLL_TIMEOUT_MS 2000
#define UAD2_RATE_POLL_INTERVAL_MS 10

static int uad2_set_sample_rate(struct uad2_dev *dev, unsigned int rate)
{
	u32 clock_val, reg_offset, notify_reg;
	int waited_ms;
	u8 rate_enum;
	u32 status;

	rate_enum = uad2_rate_to_enum(rate);
	clock_val = dev->clock_source | ((u32)rate_enum << 8);
	reg_offset = uad2_fw_reg(dev, REG_SAMPLE_CLOCK);
	notify_reg = uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS);

	/* Drain any pending notification bits left from a previous
	 * operation, then clear the cached status, so the poll below
	 * cannot mistake a stale ack for our new doorbell's response. */
	uad2_handle_notification(dev);
	WRITE_ONCE(dev->notify_status, 0);

	uad2_write32(dev, reg_offset, clock_val);
	uad2_write32(dev, REG_STREAM_ENABLE, 0x4);

	for (waited_ms = 0; waited_ms < UAD2_RATE_POLL_TIMEOUT_MS;
	     waited_ms += UAD2_RATE_POLL_INTERVAL_MS) {
		/* uad2_handle_notification takes notify_lock, processes
		 * any pending bits, then clears the register.  Calling
		 * it here drains the same bits the IRQ thread would,
		 * so the IRQ thread (if it ever runs) just sees zero
		 * and returns. */
		uad2_handle_notification(dev);
		status = READ_ONCE(dev->notify_status);
		if (status == 0xFFFFFFFF)
			return -ENODEV;
		if (status & UAD2_RATE_ACK_BITS)
			break;
		msleep(UAD2_RATE_POLL_INTERVAL_MS);
	}

	if (!(status & UAD2_RATE_ACK_BITS)) {
		dev_warn(
			&dev->pci->dev,
			"Sample rate change ack missing after %d ms (rate=%u), continuing\n",
			waited_ms, rate);
	} else {
		u32 dev_id = uad2_read32(dev, REG_DEVICE_ID);

		if (dev_id != dev->expected_device_id)
			dev_warn(&dev->pci->dev,
				 "Post-clock device ID mismatch: 0x%08x\n",
				 dev_id);
		dev_dbg(&dev->pci->dev,
			"rate ack 0x%08x after %d ms (rate=%u)\n", status,
			waited_ms, rate);
	}

	/* Suppress the stale notify_status snapshot so the next caller
	 * doesn't see an old ack from this rate change. */
	WRITE_ONCE(dev->notify_status, 0);

	dev->current_rate = rate;
	dev->irq_period_frames = uad2_irq_period_for_rate(rate);
	dev->periodic_timer_interval = uad2_periodic_timer_for_rate(rate);
	return 0;
}

/* _recomputeBufferFrameSize equivalent @ 0x4d9e0 (line 72414).
 * floor_pow2(0x400000 / (max(play_ch, rec_ch) * 4) / 2), cap 0x2000.
 * Kext at 0x4DA1C uses ubfx x8, x8, #1, #31 (divides by 2 before CLZ).
 * Apollo Solo 42 play / 32 rec yields 0x400000/(42*4)=24966, /2=12483,
 * floor_pow2 = 8192. */
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

/* Interrupt vector enable/disable.
 *
 * Kext keeps a vector_to_slot[72] lookup at IntrManager+0x4D0 that maps
 * logical vector IDs to bit slot indices in the u64 intr_enable_shadow.
 * Slot assignments used here:
 *   vector 0x28 (notify)   = slot 32 (bit 0 of DMA1)
 *   vector 0x46 (periodic) = slot 62 (bit 30 of DMA1)
 *   vector 0x47 (endbuf)   = slot 63 (bit 31 of DMA1)
 *
 * Shadow bits 0-31 drive DMA0 registers (0x2204 ctrl, 0x2208 status);
 * bits 32-63 drive DMA1 registers (0x2268 ctrl, 0x2264 status).  All
 * three vectors on Apollo Solo map to slot > 31, so has_extended_irq
 * must be set and DMA1 registers used. */
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

/* Interrupt slot indices from kext vector_to_slot[].
 * vector 0x28 = slot 32, 0x46 = slot 62, 0x47 = slot 63. */
#define INTR_SLOT_NOTIFY 32 /* notification interrupt (vector 0x28) */
#define INTR_SLOT_PERIODIC 62 /* periodic timer interrupt (vector 0x46) */
#define INTR_SLOT_ENDBUF 63 /* end-of-buffer interrupt (vector 0x47) */

/* CPcieIntrManager::ProgramRegisters equivalent.  Clears DMA interrupt
 * and status registers.  Called only from probe; no locking needed. */
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

/* CPcieIntrManager::ResetDMAEngines equivalent @ 0x134b8.
 *
 * Reset pulse: mask shadow to clear DMA channel enable bits (keep bit 0,
 * the global master enable), write shadow | reset_bits to assert reset,
 * write shadow alone to deassert, then read-back to flush.  Shadow is
 * initialized to DMA_CTRL_MASTER_ENABLE (0x1) by
 * CPcieIntrManager::Initialize (dump line 11571: str w8,[x19,#0x4bc]). */
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

/* CPcieIntrManager::EnableDMA equivalent @ 0x13a4c.
 * Sets bit (1 << (dsp_index + 1)) in shadow.  Bit 0 reserved
 * (global master enable).  dsp_index must be < 64. */
static void uad2_enable_dma(struct uad2_dev *dev, unsigned int dsp_index)
{
	u32 bit;

	if (WARN_ON(dsp_index >= 64))
		return;

	bit = 1u << (dsp_index + 1);
	dev->dma_ctrl_shadow |= bit;
	uad2_write32(dev, REG_DMA_MASTER_CTRL, dev->dma_ctrl_shadow);
}

/* CPcieDSP::_waitFor469ToStart equivalent @ 0x1152c.
 *
 * Polls dsp_poll_base+0x1A4 for DSP firmware boot.  dsp_poll_base is
 * a SEPARATE address from ring_base:
 *   ring_base     = BAR + (n<4 ? 0x2000 : 0x5e00) + n*0x80 (DMA rings)
 *   dsp_poll_base = BAR + (n>3 ? 0x2000 : 0) + n*0x800
 * DSP 0 polls BAR+0x1A4; DSP 1 polls BAR+0x9A4.
 *
 * Called when dsp_type == 1 (from GetCapabilities) and this+0x18 == 0
 * (first-time init flag).  DSP 0 gets 100 attempts, others 10.
 *
 * Kext loop treats both 0 AND DSP_READY_SIG as "still booting"; it
 * exits early only when val is non-zero AND not DSP_READY_SIG.  Final
 * success check is `val & 1`, so the DSP boot sequence runs
 * 0x00000000 (not started), 0xa8caed0f (booting), final_val (ready)
 * where final_val has bit 0 set. */
static int uad2_dsp_wait_ready(struct uad2_dev *dev,
			       void __iomem *dsp_poll_base, int dsp_index)
{
	int max_attempts = (dsp_index == 0) ? DSP_POLL_MAX_PRIMARY :
					      DSP_POLL_MAX_SECONDARY;
	int i;
	u32 val = 0;

	for (i = 0; i < max_attempts; i++) {
		val = ioread32(dsp_poll_base + DSP_READY_POLL_OFF);
		if (val != 0 && val != DSP_READY_SIG)
			break;
		msleep(DSP_POLL_INTERVAL_MS);
	}

	/* Final success check: bit 0 must be set.
	 * arm64: tbnz w20, #0x0, <success>
	 * x86_64: testb $0x1, %r13b; jne <success> */
	if (val & 1)
		return 0;

	dev_err(&dev->pci->dev,
		"DSP %d failed to start (timeout after %dms, val=0x%08x)\n",
		dsp_index, max_attempts * DSP_POLL_INTERVAL_MS, val);
	return -ETIMEDOUT;
}

/* CPcieRingBuffer::ProgramRegisters equivalent @ 0x14c48.
 *
 * Programs one ring buffer with 4 DMA descriptor pages: read ring
 * capacity (cap at 0x400), write ring_size to size and count regs,
 * then for each of 4 descriptor slots write low/high 32-bit phys
 * addresses and read back to verify. */
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

/* CPcieDSP::ProgramRegisters equivalent @ 0x1124c.  Sets up command/
 * response ring buffer addresses for one DSP. */
static int uad2_dsp_program(struct uad2_dev *dev, int dsp_index)
{
	void __iomem *ring_base;
	void __iomem *ring2_base;
	void __iomem *dsp_poll_base;
	dma_addr_t ring_dma;
	int err;

	if (dsp_index < 4)
		ring_base = dev->bar + DSP_RING_BASE_LOW +
			    (dsp_index * DSP_RING_STRIDE);
	else
		ring_base = dev->bar + DSP_RING_BASE_HIGH +
			    (dsp_index * DSP_RING_STRIDE);

	ring2_base = ring_base + DSP_RING2_OFFSET;

	/* DSP poll base is a SEPARATE address from ring_base */
	if (dsp_index < 4)
		dsp_poll_base = dev->bar + (dsp_index * 0x800);
	else
		dsp_poll_base = dev->bar + 0x2000 + (dsp_index * 0x800);

	err = uad2_dsp_wait_ready(dev, dsp_poll_base, dsp_index);
	if (err)
		return err;

	/* DMA pages for ring descriptors (4 x 4KB = 16KB per ring).  Kext
	 * uses IOBufferMemoryDescriptor pages from getPhysicalSegment(i*0x1000). */
	if (!dev->ring_dma_buf[0]) {
		dev->ring_dma_buf[0] = dma_alloc_coherent(
			&dev->pci->dev,
			DSP_RING_DESC_SLOTS * DSP_RING_PAGE_SIZE,
			&dev->ring_dma_addr[0], GFP_KERNEL);
		if (!dev->ring_dma_buf[0])
			return -ENOMEM;
	}
	ring_dma = dev->ring_dma_addr[0];

	err = uad2_ring_program(dev, ring_base, ring_dma, 0);
	if (err)
		return err;

	err = uad2_ring_program(dev, ring2_base, ring_dma, 1);
	if (err)
		return err;

	uad2_enable_dma(dev, dsp_index);

	/* Enable DSP message FIFO service + error callbacks
	 * (CPcieDSP::Initialize @ 0x10CC0).  Vectors 0-39 use identity
	 * mapping (slot = vector ID). */
	uad2_enable_vector(dev, dsp_index * 5 + 2, true);
	uad2_enable_vector(dev, dsp_index * 5 + 3, true);
	uad2_enable_vector(dev, dsp_index * 5 + 4, true);

	return 0;
}

/* Two 4MB DMA buffers for audio streaming.  MapHardware @ 0x4ba0c
 * (line 70355) calls CUAOS::AllocDMABuffer(0x400000, 2, 0, task) twice
 * and stores them at this+0x28a0 (playback) and this+0x28a8 (capture). */
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
			if (i > 0 && dev->sg_dma_buf[0]) {
				dma_free_coherent(&dev->pci->dev,
						  dev->sg_buf_size,
						  dev->sg_dma_buf[0],
						  dev->sg_dma_addr[0]);
				dev->sg_dma_buf[0] = NULL;
			}
			return -ENOMEM;
		}
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

/* CPcieAudioExtension::ProgramRegisters equivalent @ 0x4bac0.
 *
 * Decoded from kext lines 70401-70631: clear transport status, program
 * scatter-gather tables (1024 iterations, sg_offset 0x8000..0x9FF8 step
 * 8, writing 64-bit phys addrs for playback and capture pages), read
 * firmware base from BAR+0x30/0x34, then enable interrupt vector 0x28. */
static int uad2_audio_ext_program(struct uad2_dev *dev)
{
	u32 status;
	u32 sg_offset;
	u32 dma_offset;
	dma_addr_t play_page, cap_page;

	if (!dev->sg_dma_buf[0] || !dev->sg_dma_buf[1]) {
		dev_err(&dev->pci->dev, "SG DMA buffers not allocated\n");
		return -EINVAL;
	}

	/* Phase 1: clear transport status (exact kext sequence) */
	status = uad2_read32(dev, REG_TRANSPORT_CTL);
	if (status & BIT(5))
		uad2_read32(dev, REG_DMA_POSITION); /* read-to-clear latch */
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x0);

	/* Phase 2: program scatter-gather tables */
	sg_offset = REG_SG_TABLE_A_BASE;
	dma_offset = 0;

	while (sg_offset != REG_SG_TABLE_A_END) {
		play_page = dev->sg_dma_addr[0] + dma_offset;
		uad2_write32(dev, sg_offset, lower_32_bits(play_page));
		uad2_write32(dev, sg_offset + 4, upper_32_bits(play_page));

		cap_page = dev->sg_dma_addr[1] + dma_offset;
		uad2_write32(dev, sg_offset + REG_SG_TABLE_B_OFFSET,
			     lower_32_bits(cap_page));
		uad2_write32(dev, sg_offset + REG_SG_TABLE_B_OFFSET + 4,
			     upper_32_bits(cap_page));

		dma_offset += SG_PAGE_SIZE;
		sg_offset += SG_ENTRY_SIZE;
	}

	/* Phase 3: read firmware base (BAR+0x30 lo, BAR+0x34 hi) */
	dev->fw_base_addr = ((u64)uad2_read32(dev, REG_FW_BASE_HI) << 32) |
			    uad2_read32(dev, REG_FW_BASE_LO);

	/* Phase 4: arm notification interrupt (kext EnableVector(0x28, 1)) */
	uad2_write32(dev, REG_INTR_ENABLE, 0xFFFFFFFF);
	uad2_enable_vector(dev, INTR_SLOT_NOTIFY, true);

	return 0;
}

/* _handleNotificationInterrupt equivalent @ 0x4c154 (line 70824).
 *
 * Reads notification bitmask from BAR+(channel_base_index<<2)+0xC008,
 * dispatches events, then writes 0 to clear.  Event dispatch from
 * kext lines 70824-71446:
 *   bit 5  CONNECT_ACK    signals connect_event
 *   bit 21 CHAN_CONFIG    copies config from BAR+0xC000, forces bits 0+1+22
 *   bit 0  PLAYBACK_IO    copies 72 DWORDs from BAR+0xC2C4
 *   bit 1  RECORD_IO      copies 72 DWORDs from BAR+0xC1A4
 *   bit 22 RATE_CHANGE    reads BAR+0xC05C/0xC054
 *   bit 4  DMA_READY      reads BAR+0xC054/0xC058/0xC05C
 *   bit 6  ERROR          logged
 *   bit 21 (second pass)  signals notify_event (wakes Connect())
 *   bit 7  END_BUFFER     unused
 * Finally write 0 to clear the notification register. */
static void uad2_handle_notification(struct uad2_dev *dev)
{
	u32 status;
	u32 notify_reg = uad2_fw_reg(dev, REG_FW_NOTIFY_STATUS);
	unsigned long flags;

	spin_lock_irqsave(&dev->notify_lock, flags);

	/* connected mirror of kext this+0x2898 check */
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

	/* CONNECT_ACK fires on connect.  The rate-change path polls
	 * notify_status instead of waiting because TB-tunneled MSI is
	 * unreliable.  See uad2_set_sample_rate. */
	if (status & NOTIFY_CONNECT_ACK)
		complete(&dev->connect_event);

	/* CHAN_CONFIG: kext ORs 0x400003 to force IO descriptor re-reads
	 * and rate change handling after copying the config block. */
	if (status & NOTIFY_CHAN_CONFIG) {
		int i;

		for (i = 0; i < CHAN_CONFIG_DWORDS; i++)
			dev->chan_config[i] = uad2_read32(
				dev, REG_CHAN_CONFIG_BASE + (i * 4));

		status |= NOTIFY_PLAYBACK_IO | NOTIFY_RECORD_IO |
			  NOTIFY_RATE_CHANGE;
	}

	if (status & NOTIFY_PLAYBACK_IO) {
		u32 play_ch;
		int i;

		for (i = 0; i < IO_DESC_DWORDS; i++)
			dev->play_io_desc[i] = uad2_read32(
				dev, REG_PLAYBACK_IO_DESC + (i * 4));

		/* Channel count is at DWORD[4] (offset +0x10) */
		play_ch = dev->play_io_desc[4];
		if (play_ch > 0 && play_ch <= 128)
			WRITE_ONCE(dev->play_channels, play_ch);
	}

	if (status & NOTIFY_RECORD_IO) {
		u32 rec_ch;
		int i;

		for (i = 0; i < IO_DESC_DWORDS; i++)
			dev->rec_io_desc[i] =
				uad2_read32(dev, REG_RECORD_IO_DESC + (i * 4));

		rec_ch = dev->rec_io_desc[4];
		if (rec_ch > 0 && rec_ch <= 128)
			WRITE_ONCE(dev->rec_channels, rec_ch);
	}

	/* Firmware expects ack reads on rate change and DMA ready */
	if (status & NOTIFY_RATE_CHANGE) {
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
	}

	if (status & NOTIFY_DMA_READY) {
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_XPORT_INFO));
		uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
	}

	if (status & NOTIFY_ERROR)
		dev_warn(&dev->pci->dev, "Firmware error notification\n");

	/* CHAN_CONFIG second pass wakes Connect() */
	if (status & NOTIFY_CHAN_CONFIG)
		complete(&dev->notify_event);

	uad2_write32(dev, notify_reg, 0x0);

	spin_unlock_irqrestore(&dev->notify_lock, flags);
}

/* CPcieAudioExtension::Connect equivalent @ 0x4be58 (line 70632).
 *
 * The kext rings the same doorbell up to 20 times and exits on the
 * first firmware response.  The 20-iteration loop is a retry mechanism,
 * not per-channel init: the doorbell address
 * (BAR + channel_base_index*4 + 0xC004) is constant.  Per iteration:
 * write magic to doorbell, write 0x1 to REG_STREAM_ENABLE, wait on
 * notify_event with a 100ms timeout (up to 10 retries) and manually
 * call uad2_handle_notification on timeout.  The firmware responds by
 * setting bits 5 + 21 in the notification register: bit 5 signals
 * connect_event, bit 21 copies channel config and signals notify_event
 * (the bit the handshake actually waits on). */
static int uad2_audio_handshake(struct uad2_dev *dev)
{
	u32 doorbell_reg = uad2_fw_reg(dev, REG_FW_DOORBELL);
	unsigned long timeout_jiffies = msecs_to_jiffies(UAD2_CONNECT_WAIT_MS);
	unsigned long flags;
	int chan, retry;
	long ret;

	/* connected must be true BEFORE the doorbell so the notification
	 * handler does not bail out and miss the firmware response. */
	WRITE_ONCE(dev->connected, true);

	for (chan = 0; chan < UAD2_CONNECT_CHANNELS; chan++) {
		reinit_completion(&dev->notify_event);

		spin_lock_irqsave(&dev->lock, flags);
		uad2_write32(dev, doorbell_reg, DMA_DESC_MAGIC);
		uad2_write32(dev, REG_STREAM_ENABLE, 0x1);
		spin_unlock_irqrestore(&dev->lock, flags);

		for (retry = 0; retry < UAD2_CONNECT_RETRIES; retry++) {
			ret = wait_for_completion_timeout(&dev->notify_event,
							  timeout_jiffies);
			if (ret > 0)
				goto done;

			/* Kext: manually poll the notification register on
			 * timeout (calls _handleNotificationInterrupt). */
			uad2_handle_notification(dev);

			if (try_wait_for_completion(&dev->notify_event))
				goto done;
		}
	}

	WRITE_ONCE(dev->connected, false);
	return -ETIMEDOUT;

done:
	dev_dbg(&dev->pci->dev, "audio handshake ok on attempt %d (retry %d)\n",
		chan, retry);
	return 0;
}

/* Synthetic post-handshake reads.  The kext's notification handler
 * synthetically injects bits 0+1+22 after connect (ORs 0x400003) to
 * force the host to read IO descriptors and rate/clock info.  The
 * firmware state machine expects these reads as acknowledgment before
 * advancing to active audio routing.  From open-apollo ua_audio.c:946. */
static void uad2_audio_post_connect_drain(struct uad2_dev *dev)
{
	u32 rate_info, clock_info, xport_info;

	rate_info = uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_RATE_INFO));
	clock_info = uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_CLOCK_INFO));
	xport_info = uad2_read32(dev, uad2_fw_reg(dev, REG_NOTIF_XPORT_INFO));

	dev_dbg(&dev->pci->dev,
		"post-connect: rate=0x%08x clock=0x%08x xport=0x%08x\n",
		rate_info, clock_info, xport_info);
}

/* First-time connect from probe: doorbell handshake, recompute buffer
 * frames from firmware IO descriptors, initialize mixer batch protocol,
 * start DSP service loop. */
static int uad2_audio_connect(struct uad2_dev *dev)
{
	int err;

	err = uad2_audio_handshake(dev);
	if (err) {
		dev_err(&dev->pci->dev,
			"Connect failed: no response after %d attempts x %d retries\n",
			UAD2_CONNECT_CHANNELS, UAD2_CONNECT_RETRIES);
		return err;
	}

	dev->buffer_frames = uad2_compute_buffer_frames(dev->play_channels,
							dev->rec_channels);
	dev_dbg(&dev->pci->dev,
		"Audio connected: play=%u rec=%u buffer_frames=%u\n",
		dev->play_channels, dev->rec_channels, dev->buffer_frames);

	uad2_audio_post_connect_drain(dev);
	uad2_mixer_init(dev);
	uad2_dsp_service_start(dev);

	return 0;
}

/* CPcieDevice::ProgramRegisters equivalent @ 0xdf48.
 * Top-level hardware init sequence; exact order from RE. */
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

/* CPcieAudioExtension::PrepareTransport equivalent (kext lines
 * 71623-71903).  Programs all audio transport registers before DMA
 * starts.  Called from pcm_prepare with runtime parameters.  Sequence:
 *   - check state != 2, is_connected, bufferFrameSize < 0x10000
 *   - under hw_lock:
 *       0x2240 = bufferFrameSize - 1
 *       0x226C = (actualPlayChans * (bufSz-1)) >> 10
 *       0x2244 = 0 (clear position)
 *       if periodic_timer_interval: 0x2270 = interval, EnableVector(0x46,1)
 *       0x2244 = 0 again, then read-back flush
 *       0x2248 = 0 (stop/reset)
 *       0x2258 = irqPeriod
 *       0x2250 = actualPlayChans, 0x225C = actualRecChans
 *       0x2248 = 1 (arm), read 0x22C0 (monitor flush)
 *       if diagnostic_flags bit1: 0x224C = (actualPlayChans-1) | 0x100
 *   - transport_state = 1, poll 0x2254 until == irqPeriod (max 2 retries)
 *   - EnableVector(0x47, 1) for end-of-buffer interrupt. */
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

	uad2_write32(dev, REG_BUFFER_FRAME_SIZE, buffer_frames - 1);

	buf_size_kb = (play_channels * (buffer_frames - 1)) >> 10;
	uad2_write32(dev, REG_BUFFER_SIZE_KB, buf_size_kb);

	uad2_write32(dev, REG_DMA_POSITION, 0);
	dev->play_pos_offset = 0;
	dev->rec_pos_offset = 0;

	if (dev->periodic_timer_interval) {
		uad2_write32(dev, REG_PERIODIC_TIMER,
			     dev->periodic_timer_interval);
		__uad2_enable_vector_locked(dev, INTR_SLOT_PERIODIC, true);
	}

	/* Clear position again and read-back flush */
	uad2_write32(dev, REG_DMA_POSITION, 0);
	uad2_read32(dev, REG_DMA_POSITION);

	uad2_write32(dev, REG_TRANSPORT_CTL, 0); /* stop/reset */
	uad2_write32(dev, REG_IRQ_PERIOD, irq_period_frames);
	uad2_write32(dev, REG_PLAYBACK_CHAN_CNT, play_channels);
	uad2_write32(dev, REG_RECORD_CHAN_CNT, rec_channels);
	uad2_write32(dev, REG_TRANSPORT_CTL, 0x1); /* arm */
	uad2_read32(dev, REG_PLAYBACK_MON_STAT); /* fence */

	/* P2P_ROUTE tells the FPGA how to route audio between DSP and DMA.
	 * open-apollo writes this for all models after DMA enable + fence;
	 * previous code skipped it and was wrong. */
	uad2_write32(dev, REG_PLAYBACK_MON_CFG, 0x100 | (play_channels - 1));

	spin_unlock_irqrestore(&dev->lock, flags);

	WRITE_ONCE(dev->transport_state, 1);

	/* Poll until DMA is ready (kext does max 2 retries, 1ms sleeps) */
	for (i = 0; i < 3; i++) {
		u32 poll_val = uad2_read32(dev, REG_POLL_STATUS);

		if (poll_val == irq_period_frames)
			break;
		usleep_range(1000, 2000);
	}

	uad2_enable_vector(dev, INTR_SLOT_ENDBUF, true);

	return 0;
}

/* CPcieAudioExtension::StartTransport equivalent (kext lines 71904-71986).
 *
 * Verify state == 1 and is_connected, pick start_val by device variant
 * (this+0x28B0): variant 0xA always uses 0x20F; variant 0x9 uses 0x20F
 * when the extended_mode_flag at this+0x22EF4 is zero; everything else
 * uses 0xF.  Write start_val to REG_TRANSPORT_CTL and set state = 2.
 *
 * v2 firmware (all current Apollo TB devices) requires extended mode
 * (0x20F, BIT 9 = EXT_MODE), without which the DSP may not activate.
 * v1 firmware starts at 0xF.  From open-apollo: 0x20F = DMA + play +
 * rec + IRQ + extended mode. */
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

	start_val = dev->fw_v2 ? 0x20F : 0xF;

	spin_lock_irqsave(&dev->lock, flags);
	uad2_write32(dev, REG_TRANSPORT_CTL, start_val);
	WRITE_ONCE(dev->transport_state, 2);
	spin_unlock_irqrestore(&dev->lock, flags);
}

/* CPcieAudioExtension::StopTransport equivalent (kext lines 71987-72056).
 * Disables vector 0x47, writes 0 to REG_TRANSPORT_CTL under hw_lock,
 * sets state to 0.  We also disable the periodic timer vector here so
 * no more period-elapsed callbacks fire after the transport stops. */
static void uad2_stop_transport(struct uad2_dev *dev)
{
	unsigned long flags;

	uad2_disable_vector(dev, INTR_SLOT_ENDBUF);
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
	dev->play_prepared = false;
	dev->rec_prepared = false;
	dev->cached_period_frames = 0;
}

/* ============================================================
 * ALSA PCM hardware definitions
 *
 * From ioreg IOAudioEngine (confirmed on live hardware):
 *   Format: 32-bit container, 24-bit depth, signed int, LE
 *   (IOAudioStreamByteOrder=1 on macOS = little-endian)
 *
 * Buffer structure: interleaved, all channels x buffer_frames x 4 bytes
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
 * Playback: 256 frames x 42ch x 4B = 43008 bytes/period (min)
 *           1024 frames x 42ch x 4B = 172032 bytes/period (max)
 * Capture:  256 frames x 32ch x 4B = 32768 bytes/period (min)
 *           1024 frames x 32ch x 4B = 131072 bytes/period (max)
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
 *   44100/48000 Hz:   every 256 frames
 *   88200/96000 Hz:   every 512 frames
 *   176400/192000 Hz: every 1024 frames
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
 *   44100/48000 Hz:   period=256,  periods=32
 *   88200/96000 Hz:   period=512,  periods=16
 *   176400/192000 Hz: period=1024, periods=8
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
		goto err_undo_open;

	/* Add constraint rule: buffer_size must be exactly 8192 frames.
	 * The hardware DMA ring buffer is fixed at 8192 frames and the
	 * position counter wraps at that boundary. */
	err = snd_pcm_hw_rule_add(ss->runtime, 0,
				  SNDRV_PCM_HW_PARAM_BUFFER_SIZE,
				  uad2_rule_buffer_size, NULL, -1);
	if (err < 0)
		goto err_undo_open;

	/* Add constraint rule: periods = 8192 / period_size, since both
	 * buffer_size and period_size are fixed by hardware. */
	err = snd_pcm_hw_rule_add(ss->runtime, 0, SNDRV_PCM_HW_PARAM_PERIODS,
				  uad2_rule_periods, NULL,
				  SNDRV_PCM_HW_PARAM_PERIOD_SIZE, -1);
	if (err < 0)
		goto err_undo_open;

	return 0;

err_undo_open:
	/* hw_rule_add failed after we registered the substream pointer
	 * and bumped open_count.  Roll those back so pcm_close (which
	 * ALSA may not call on this error path) does not leave the
	 * transport pinned by a non-existent stream. */
	spin_lock_irqsave(&dev->lock, flags);
	if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
		dev->playback_ss = NULL;
	else
		dev->capture_ss = NULL;
	if (dev->open_count > 0)
		dev->open_count--;
	spin_unlock_irqrestore(&dev->lock, flags);
	return err;
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
	 * transport persists through stop, hw_free, prepare, start cycles. */
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

	/* Don't actually free the DMA buffer, it belongs to the device.
	 * Just clear the runtime pointers. */
	runtime->dma_area = NULL;
	runtime->dma_addr = 0;
	runtime->dma_bytes = 0;

	/* Decrement the prepared-streams reference count if pcm_prepare
	 * had counted this direction.  Uses per-direction flags
	 * (play_prepared / rec_prepared) so JOINT_DUPLEX overlap of
	 * prepare/hw_free across directions is accounted correctly.
	 *
	 * NOTE: We do NOT tear down the transport here.  Following the
	 * snd-dice/snd-bebob pattern, transport lifecycle is tied to
	 * pcm_open/pcm_close (via open_count), not to hw_params/hw_free.
	 * This prevents the pathological PipeWire reconfiguration cycle
	 * where a brief hw_free->prepare sequence would cause a full
	 * transport teardown and re-initialization. */
	spin_lock_irqsave(&dev->lock, flags);
	{
		bool *flag = (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
				     &dev->play_prepared :
				     &dev->rec_prepared;

		if (*flag) {
			*flag = false;
			if (dev->streams_prepared > 0)
				dev->streams_prepared--;
		}
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
 *   buffer_frames x max(play_channels, rec_channels) x 4 bytes
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
	 * The kext's _setSampleClock (0x4DE70) only writes the clock
	 * register and waits for the bit-5 ack. It does not reach
	 * StopTransport or PrepareTransport itself.  Those are driven
	 * by the IOAudioEngine layer above on macOS, which stops the
	 * engine, changes the clock, then restarts.  We mirror the same
	 * three-step sequence here. */
	if (dev->current_rate != rt->rate) {
		if (READ_ONCE(dev->transport_state) >= 1) {
			dev_dbg(&dev->pci->dev,
				"pcm_prepare: rate change %u -> %u, stopping transport for re-prepare\n",
				dev->current_rate, rt->rate);

			/* Clear running flags before stopping the transport
			 * so the hrtimer and IRQ handlers stop firing
			 * snd_pcm_period_elapsed on a substream whose
			 * underlying DMA is being torn down. */
			spin_lock_irqsave(&dev->lock, flags);
			dev->playback_running = false;
			dev->capture_running = false;
			spin_unlock_irqrestore(&dev->lock, flags);

			uad2_period_timer_stop(dev);
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

	/* Track that this direction has been prepared. Uses per-direction
	 * flags because a single shared flag broke under JOINT_DUPLEX. */
	{
		bool *flag = (ss->stream == SNDRV_PCM_STREAM_PLAYBACK) ?
				     &dev->play_prepared :
				     &dev->rec_prepared;

		spin_lock_irqsave(&dev->lock, flags);
		if (!*flag) {
			*flag = true;
			dev->streams_prepared++;
		}
		/* Cache period_size for the hrtimer so it never has to
		 * dereference snd_pcm_runtime (which the ALSA core can
		 * free under us between two timer ticks). */
		dev->cached_period_frames = rt->period_size;
		spin_unlock_irqrestore(&dev->lock, flags);
	}

	/* Hardware buffer frame size (fixed per model) */
	buffer_frames = dev->buffer_frames;

	/* IRQ period and periodic timer from rate-based lookup tables.
	 * uad2_set_sample_rate also updates these, but we recompute here
	 * in case pcm_prepare is reached without a rate change (cold start
	 * at probe-default 48000). */
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
		/* Transport already running at correct rate (fast path).
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

	/* Cold start (or post-rate-change): full transport preparation.
	 * Always program the full hardware channel counts because the
	 * DMA layout is fixed at all channels interleaved. */
	dev->play_pos_offset = 0;
	dev->rec_pos_offset = 0;
	/* Prepare transport only. Do NOT start it here: ALSA expects
	 * trigger(START) to start the transport. Starting here would
	 * cause the DMA position to advance before ALSA's hw_ptr is
	 * initialized, resulting in XRUN on first read. The post-transport
	 * clock write also lives in trigger. */
	return uad2_prepare_transport(dev, buffer_frames, irq_period_frames,
				      READ_ONCE(dev->play_channels),
				      READ_ONCE(dev->rec_channels));
}

/* ============================================================
 * Period elapsed polling timer (hrtimer)
 *
 * The Apollo's hardware periodic timer IRQ (vector 0x46) is unreliable
 * on most models: it fires once then stops. We use an hrtimer at ~1ms
 * to poll REG_DMA_POSITION and call snd_pcm_period_elapsed() when a
 * period boundary is crossed. Same approach as snd-usb-audio and
 * open-apollo's period_timer.
 * ============================================================ */
#define PERIOD_TIMER_NS 1000000 /* 1 ms polling interval */

static enum hrtimer_restart uad2_period_timer_fn(struct hrtimer *timer)
{
	struct uad2_dev *dev =
		container_of(timer, struct uad2_dev, period_timer);
	struct snd_pcm_substream *play_ss, *cap_ss;
	snd_pcm_uframes_t period_size, cur_period;
	bool play_run, cap_run;
	unsigned long flags;
	u32 pos;

	if (!READ_ONCE(dev->period_timer_running) ||
	    READ_ONCE(dev->disconnecting))
		return HRTIMER_NORESTART;

	pos = uad2_read32(dev, REG_DMA_POSITION);
	if (pos == 0xFFFFFFFF) {
		WRITE_ONCE(dev->period_timer_running, false);
		return HRTIMER_NORESTART;
	}
	if (pos >= dev->buffer_frames)
		pos = 0;

	/* Read all stream pointers and the cached period size under
	 * dev->lock.  Caching period_size here (rather than reading
	 * play_ss->runtime->period_size) avoids dereferencing the ALSA
	 * runtime struct, which the ALSA core can free under us between
	 * trigger(STOP) and hw_free. */
	spin_lock_irqsave(&dev->lock, flags);
	play_ss = dev->playback_ss;
	cap_ss = dev->capture_ss;
	play_run = dev->playback_running;
	cap_run = dev->capture_running;
	period_size = dev->cached_period_frames;
	spin_unlock_irqrestore(&dev->lock, flags);

	if (!period_size)
		goto out;

	if (play_ss && play_run) {
		u32 off = READ_ONCE(dev->play_pos_offset);
		u32 adj = (pos - off + dev->buffer_frames) % dev->buffer_frames;

		cur_period = adj / period_size;
		if (cur_period != dev->last_play_period) {
			dev->last_play_period = cur_period;
			snd_pcm_period_elapsed(play_ss);
		}
	}

	if (cap_ss && cap_run) {
		u32 off = READ_ONCE(dev->rec_pos_offset);
		u32 adj = (pos - off + dev->buffer_frames) % dev->buffer_frames;

		cur_period = adj / period_size;
		if (cur_period != dev->last_rec_period) {
			dev->last_rec_period = cur_period;
			snd_pcm_period_elapsed(cap_ss);
		}
	}

out:
	hrtimer_forward_now(timer, ns_to_ktime(PERIOD_TIMER_NS));
	return HRTIMER_RESTART;
}

static void uad2_period_timer_start(struct uad2_dev *dev)
{
	if (READ_ONCE(dev->period_timer_running))
		return;

	dev->last_play_period = 0;
	dev->last_rec_period = 0;
	WRITE_ONCE(dev->period_timer_running, true);
	hrtimer_start(&dev->period_timer, ns_to_ktime(PERIOD_TIMER_NS),
		      HRTIMER_MODE_REL);
}

/*
 * Stop and drain the period timer. CALLER MUST be in process context.
 * hrtimer_cancel() spins until the callback returns, so calling it
 * from the callback itself (or from any atomic context the callback
 * can call into) deadlocks. ALSA's snd_pcm_drain_done path triggers
 * SNDRV_PCM_TRIGGER_STOP from inside snd_pcm_period_elapsed(), which
 * is one such forbidden caller; that path must use the lighter
 * WRITE_ONCE(period_timer_running, false) instead and let the next
 * tick of the callback notice the flag and self-cancel.
 */
static void uad2_period_timer_stop(struct uad2_dev *dev)
{
	if (!READ_ONCE(dev->period_timer_running))
		return;

	WRITE_ONCE(dev->period_timer_running, false);
	hrtimer_cancel(&dev->period_timer);
}

static int uad2_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct uad2_dev *dev = snd_pcm_substream_chip(ss);
	unsigned long flags;
	bool first_stream;
	int running;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		spin_lock_irqsave(&dev->lock, flags);
		dev->streams_running++;
		first_stream = (dev->streams_running == 1);
		if (ss->stream == SNDRV_PCM_STREAM_PLAYBACK)
			dev->playback_running = true;
		else
			dev->capture_running = true;
		spin_unlock_irqrestore(&dev->lock, flags);
		uad2_start_transport(dev);

		/* Post-transport clock write: clock_source=0xC enables DSP
		 * active processing.  Only fire on the first stream of the
		 * transport, not on piggyback opens.  first_stream is
		 * captured under dev->lock so concurrent start/stop cannot
		 * see both as the "first" or both as a piggyback.
		 *
		 * The 0xC source override is required by the firmware
		 * regardless of dev->clock_source: open-apollo's RE shows
		 * the DSP only enters active processing mode for source 0xC
		 * (internal).  The user-visible Clock Source enum still
		 * shows the cached dev->clock_source. */
		if (first_stream) {
			u32 clock_cfg =
				0xC | ((u32)uad2_rate_to_enum(dev->current_rate)
				       << 8);
			uad2_write32(dev, uad2_fw_reg(dev, REG_SAMPLE_CLOCK),
				     clock_cfg);
			uad2_write32(dev, REG_STREAM_ENABLE, 0x4);
		}

		/* Start period-elapsed polling timer. The hardware
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
		running = dev->streams_running;
		spin_unlock_irqrestore(&dev->lock, flags);

		/* NOTE: We do NOT stop the hardware transport here.
		 *
		 * Following the snd-dice/snd-bebob pattern, the DMA
		 * transport runs continuously once started and only stops
		 * when the last substream closes (pcm_close).  This is
		 * critical for PipeWire which rapidly cycles through
		 * stop, hw_free, prepare, start during graph reconfiguration.
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

		/* Stop the polling timer when no streams are running.
		 * We MUST NOT call uad2_period_timer_stop() here.
		 * trigger() can be invoked from inside our hrtimer
		 * callback (snd_pcm_period_elapsed -> snd_pcm_drain_done
		 * -> snd_pcm_do_stop), and hrtimer_cancel() spins waiting
		 * for the callback to finish, deadlocking against itself.
		 * Just clear the flag; the next tick (within 1 ms) sees
		 * it and returns HRTIMER_NORESTART, taking the timer off
		 * the wheel naturally.
		 *
		 * The running snapshot was captured under dev->lock above
		 * so a concurrent trigger(START) cannot make us clear the
		 * flag while another stream is still running. */
		if (running == 0)
			WRITE_ONCE(dev->period_timer_running, false);
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
 * Interrupt handler. Split into hardirq top half + threaded bottom
 * half. Mirrors CPcieIntrManager::ServiceInterrupt @ 0x14330.
 *
 * Kext flow:
 *   1. Read pending_lo from REG_DMA0_STATUS (0x2208)
 *   2. If has_extended_irq: read pending_hi from REG_DMA1_STATUS (0x2264)
 *   3. Combine into u64 pending, AND with intr_enable_shadow for active
 *   4. Re-arm: write active bits back to 0x2208 (and 0x2264 if extended)
 *   5. Dispatch callbacks per active bit
 *
 * Slot-to-vector mapping (from kext vector_to_slot[] table):
 *   slot 32 (bit 0 of DMA1)  = vector 0x28 (notification)
 *   slot 62 (bit 30 of DMA1) = vector 0x46 (periodic timer)
 *   slot 63 (bit 31 of DMA1) = vector 0x47 (end-of-buffer)
 *
 * With Linux MSI (single vector mode), all interrupts arrive on
 * vector 0, so we dispatch based on the pending bitmask.
 *
 * We use a threaded IRQ for uad2_handle_notification() (which needs
 * process context due to the reentrancy hazard with the connect loop's
 * manual poll) and for re-arming the one-shot periodic timer vector.
 *
 * snd_pcm_period_elapsed() is called directly from the hardirq top
 * half for minimum latency. This eliminates the variable thread
 * scheduling delay that caused ALSA to see multi-period position
 * jumps (manifesting as audible skips in PipeWire). The function is
 * hardirq-safe (uses snd_pcm_stream_lock_irqsave() internally).
 * ============================================================ */

/*
 * Hardirq top half: read and ack interrupt status, wake thread.
 * Must be minimal: no sleeping, no complex dispatch.
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
	 * back to the status registers, never raw pending. This avoids
	 * acking interrupts we didn't enable, which could confuse the
	 * firmware or clear events meant for other purposes.
	 *
	 * With MSI, un-acked but disabled bits remain pending in the
	 * device's internal state. If a slot is later enabled, the
	 * pending bit will become active on the next read (correct
	 * level-triggered-like behavior within the device's interrupt
	 * controller). */
	uad2_write32(dev, REG_DMA0_STATUS, (u32)active);
	if (dev->has_extended_irq && (u32)(active >> 32))
		uad2_write32(dev, REG_DMA1_STATUS, (u32)(active >> 32));

	/* Periodic timer interrupt (slot 62 = vector 0x46): call
	 * snd_pcm_period_elapsed() directly from hardirq context.
	 *
	 * snd_pcm_period_elapsed() is hardirq-safe and uses
	 * snd_pcm_stream_lock_irqsave() internally. Many ALSA drivers
	 * (HDA, USB-audio, etc.) call it from hardirq.
	 *
	 * Moving this out of the threaded handler eliminates variable
	 * thread scheduling latency that caused ALSA to see multi-period
	 * position jumps, which PipeWire manifested as audible skips.
	 *
	 * IMPORTANT: We must NOT hold dev->lock when calling
	 * snd_pcm_period_elapsed(), because trigger() is called by ALSA
	 * with the stream lock held and then takes dev->lock internally.
	 * Lock ordering: stream_lock then dev->lock. We already released
	 * dev->lock above, so we're safe here. */
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
 * Runs in process context (needed for uad2_handle_notification()
 * and re-arming the one-shot periodic timer vector).
 *
 * snd_pcm_period_elapsed() is now called directly from the hardirq
 * top half for lowest latency (see uad2_irq_hard()).
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
	 * Re-arm the one-shot periodic timer vector. The hardirq
	 * disabled this vector in intr_enable_shadow (kext one-shot
	 * behavior). We must re-enable it here so the next period's
	 * interrupt fires. This matches the kext's deferred handler
	 * calling ResetPeriodicTimerInterrupt then EnableVector(0x46, 1).
	 *
	 * Re-arm must happen regardless of streams_running because the
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

	/* Reject clock source changes while transport is running.
	 * Changing the clock mid-stream could corrupt DMA state. */
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

	/* Reject mid-transport rate changes: PrepareTransport() programs
	 * the IRQ period and periodic timer based on the rate, and the
	 * userspace PCM stream is bound to a fixed period_size matching
	 * the rate.  Changing the rate while DMA is running would leave
	 * those values stale and break position tracking.  pcm_prepare()
	 * is the only correct rate-change entry point during streaming. */
	if (READ_ONCE(dev->transport_state) >= 1)
		return -EBUSY;

	uad2_set_sample_rate(dev, rate);
	return 1;
}

static const struct snd_kcontrol_new uad2_sample_rate_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_CARD,
	.name = "Sample Rate",
	.info = uad2_sample_rate_info,
	.get = uad2_sample_rate_get,
	.put = uad2_sample_rate_put,
};

/* ============================================================
 * Monitor ALSA kcontrols
 * ============================================================ */

/* Monitor Playback Volume: 0-192 raw (192 + dB*2) */
static int uad2_monitor_vol_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 192;
	info->value.integer.step = 1;
	return 0;
}

static int uad2_monitor_vol_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	val->value.integer.value[0] = dev->monitor.level;
	return 0;
}

static int uad2_monitor_vol_put(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	int new_val = val->value.integer.value[0];

	if (new_val < 0 || new_val > 192)
		return -EINVAL;
	if (new_val == dev->monitor.level)
		return 0;

	uad2_monitor_set_param(dev, MON_PARAM_LEVEL, new_val);
	return 1;
}

static const struct snd_kcontrol_new uad2_monitor_vol_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Monitor Playback Volume",
	.info = uad2_monitor_vol_info,
	.get = uad2_monitor_vol_get,
	.put = uad2_monitor_vol_put,
};

/* Monitor Playback Switch (mute): BOOLEAN, 1=unmuted 0=muted */
static int uad2_monitor_mute_info(struct snd_kcontrol *kctl,
				  struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	info->count = 1;
	info->value.integer.min = 0;
	info->value.integer.max = 1;
	return 0;
}

static int uad2_monitor_mute_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	/* ALSA convention: 1=on (playing), 0=muted */
	val->value.integer.value[0] = (dev->monitor.mute == MON_MUTE_OFF) ? 1 :
									    0;
	return 0;
}

static int uad2_monitor_mute_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	int hw_mute = val->value.integer.value[0] ? MON_MUTE_OFF : MON_MUTE_ON;

	if (hw_mute == dev->monitor.mute)
		return 0;

	uad2_monitor_set_param(dev, MON_PARAM_MUTE, hw_mute);
	return 1;
}

static const struct snd_kcontrol_new uad2_monitor_mute_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Monitor Playback Switch",
	.info = uad2_monitor_mute_info,
	.get = uad2_monitor_mute_get,
	.put = uad2_monitor_mute_put,
};

/* Monitor Dim Switch: BOOLEAN */
static int uad2_monitor_dim_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	val->value.integer.value[0] = dev->monitor.dim ? 1 : 0;
	return 0;
}

static int uad2_monitor_dim_put(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	bool new_dim = !!val->value.integer.value[0];

	if (new_dim == dev->monitor.dim)
		return 0;

	uad2_monitor_set_param(dev, MON_PARAM_DIM, new_dim ? 1 : 0);
	return 1;
}

static const struct snd_kcontrol_new uad2_monitor_dim_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Monitor Dim Switch",
	.info = uad2_monitor_mute_info, /* reuse boolean info */
	.get = uad2_monitor_dim_get,
	.put = uad2_monitor_dim_put,
};

/* Monitor Source: ENUM "MIX", "CUE 1", "CUE 2" */
static const char *const uad2_monitor_source_names[] = { "MIX", "CUE 1",
							 "CUE 2" };

static int uad2_monitor_source_info(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_info *info)
{
	return snd_ctl_enum_info(info, 1, 3, uad2_monitor_source_names);
}

static int uad2_monitor_source_get(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);

	val->value.enumerated.item[0] = dev->monitor.source;
	return 0;
}

static int uad2_monitor_source_put(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int idx = val->value.enumerated.item[0];

	if (idx > 2)
		return -EINVAL;
	if ((int)idx == dev->monitor.source)
		return 0;

	uad2_monitor_set_param(dev, MON_PARAM_SOURCE, idx);
	return 1;
}

static const struct snd_kcontrol_new uad2_monitor_source_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Monitor Source",
	.info = uad2_monitor_source_info,
	.get = uad2_monitor_source_get,
	.put = uad2_monitor_source_put,
};

/* ============================================================
 * Preamp ALSA kcontrols (per-channel, dynamic registration)
 *
 * private_value packs channel index and param_id:
 *   bits[7:0]  = channel index
 *   bits[15:8] = param_id
 * ============================================================ */
#define UA_CTL_PACK(ch, param) (((param) << 8) | (ch))
#define UA_CTL_CH(pv) ((pv) & 0xFF)
#define UA_CTL_PARAM(pv) (((pv) >> 8) & 0xFF)

/* Preamp Capture Volume: INTEGER -144..+65 dB */
static int uad2_preamp_gain_info(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 1;
	info->value.integer.min = -144;
	info->value.integer.max = 65;
	info->value.integer.step = 1;
	return 0;
}

static int uad2_preamp_gain_get(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);

	val->value.integer.value[0] = dev->preamp[ch].gain;
	return 0;
}

static int uad2_preamp_gain_put(struct snd_kcontrol *kctl,
				struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);
	int new_gain = val->value.integer.value[0];

	if (new_gain < -144 || new_gain > 65)
		return -EINVAL;
	if (new_gain == dev->preamp[ch].gain)
		return 0;

	uad2_preamp_set_param(dev, ch, PREAMP_PARAM_GAIN_A, (u32)new_gain);
	return 1;
}

/* Preamp boolean switch: generic for pad/48V/lowcut/phase */
static int uad2_preamp_switch_get(struct snd_kcontrol *kctl,
				  struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);
	unsigned int param = UA_CTL_PARAM(kctl->private_value);

	switch (param) {
	case PREAMP_PARAM_48V:
		val->value.integer.value[0] = dev->preamp[ch].phantom;
		break;
	case PREAMP_PARAM_PAD:
		val->value.integer.value[0] = dev->preamp[ch].pad;
		break;
	case PREAMP_PARAM_LOWCUT:
		val->value.integer.value[0] = dev->preamp[ch].lowcut;
		break;
	case PREAMP_PARAM_PHASE:
		val->value.integer.value[0] = dev->preamp[ch].phase;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int uad2_preamp_switch_put(struct snd_kcontrol *kctl,
				  struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);
	unsigned int param = UA_CTL_PARAM(kctl->private_value);
	bool new_val = !!val->value.integer.value[0];

	uad2_preamp_set_param(dev, ch, param, new_val ? 1 : 0);
	return 1;
}

/* Preamp Input Select: ENUM "Mic", "Line" */
static const char *const uad2_input_select_names[] = { "Mic", "Line" };

static int uad2_input_select_info(struct snd_kcontrol *kctl,
				  struct snd_ctl_elem_info *info)
{
	return snd_ctl_enum_info(info, 1, 2, uad2_input_select_names);
}

static int uad2_input_select_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);

	val->value.enumerated.item[0] = dev->preamp[ch].mic_line ? 1 : 0;
	return 0;
}

static int uad2_input_select_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *val)
{
	struct uad2_dev *dev = snd_kcontrol_chip(kctl);
	unsigned int ch = UA_CTL_CH(kctl->private_value);
	unsigned int idx = val->value.enumerated.item[0];

	if (idx > 1)
		return -EINVAL;

	uad2_preamp_set_param(dev, ch, PREAMP_PARAM_MIC_LINE, idx);
	return 1;
}

static int uad2_create_mixer(struct uad2_dev *dev)
{
	struct snd_card *card = dev->card;
	struct snd_kcontrol_new ctl;
	unsigned int ch;
	int err;

	err = snd_ctl_add(card, snd_ctl_new1(&uad2_clock_source_ctl, dev));
	if (err < 0)
		return err;

	err = snd_ctl_add(card, snd_ctl_new1(&uad2_sample_rate_ctl, dev));
	if (err < 0)
		return err;

	/* Monitor controls */
	err = snd_ctl_add(card, snd_ctl_new1(&uad2_monitor_vol_ctl, dev));
	if (err < 0)
		return err;
	err = snd_ctl_add(card, snd_ctl_new1(&uad2_monitor_mute_ctl, dev));
	if (err < 0)
		return err;
	err = snd_ctl_add(card, snd_ctl_new1(&uad2_monitor_dim_ctl, dev));
	if (err < 0)
		return err;
	err = snd_ctl_add(card, snd_ctl_new1(&uad2_monitor_source_ctl, dev));
	if (err < 0)
		return err;

	/* Per-channel preamp controls */
	for (ch = 0; ch < dev->num_preamps; ch++) {
		char name[44];

		/* Capture Volume (gain) */
		memset(&ctl, 0, sizeof(ctl));
		snprintf(name, sizeof(name), "Mic %u Capture Volume", ch + 1);
		ctl.iface = SNDRV_CTL_ELEM_IFACE_MIXER;
		ctl.name = name;
		ctl.info = uad2_preamp_gain_info;
		ctl.get = uad2_preamp_gain_get;
		ctl.put = uad2_preamp_gain_put;
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_GAIN_A);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;

		/* 48V Phantom Power Switch */
		snprintf(name, sizeof(name), "Mic %u 48V Phantom Power Switch",
			 ch + 1);
		ctl.info = uad2_monitor_mute_info; /* boolean */
		ctl.get = uad2_preamp_switch_get;
		ctl.put = uad2_preamp_switch_put;
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_48V);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;

		/* Pad Switch */
		snprintf(name, sizeof(name), "Mic %u Pad Switch", ch + 1);
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_PAD);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;

		/* Low Cut Switch */
		snprintf(name, sizeof(name), "Mic %u Low Cut Switch", ch + 1);
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_LOWCUT);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;

		/* Phase Invert Switch */
		snprintf(name, sizeof(name), "Mic %u Phase Invert Switch",
			 ch + 1);
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_PHASE);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;

		/* Input Select (Mic/Line) */
		snprintf(name, sizeof(name), "Mic %u Input Select", ch + 1);
		ctl.info = uad2_input_select_info;
		ctl.get = uad2_input_select_get;
		ctl.put = uad2_input_select_put;
		ctl.private_value = UA_CTL_PACK(ch, PREAMP_PARAM_MIC_LINE);
		err = snd_ctl_add(card, snd_ctl_new1(&ctl, dev));
		if (err < 0)
			return err;
	}

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

	dev_dbg(&pci->dev, "BAR 0 mapped: %pa len=%pa\n",
		&pci->resource[0].start, &dev->bar_len);

	/*
	 * Snapshot device ID register at probe time.
	 * Used as expected value in uad2_hw_program() handshake
	 * and polled after clock changes.
	 */
	dev->expected_device_id = uad2_read32(dev, REG_DEVICE_ID);
	dev_dbg(&pci->dev, "Device ID register: 0x%08x\n",
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

	/* No snd_pcm_lib_preallocate_pages_for_all needed; we use our
	 * own pre-allocated 4MB DMA buffers mapped in hw_params. */

	/* Create mixer controls (clock source, sample rate readout) */
	err = uad2_create_mixer(dev);
	if (err < 0)
		goto err_free_irq;

	err = snd_card_register(card);
	if (err < 0)
		goto err_free_irq;

	pci_set_drvdata(pci, dev);
	dev_info(&pci->dev,
		 "%s initialized: play=%uch rec=%uch buf=%u frames\n",
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

	/* Mark card as disconnected first. All subsequent ALSA operations
	 * (PCM open/prepare/trigger/pointer) then return errors
	 * immediately, preventing races where userspace still has the
	 * device open while we tear down hardware state. */
	snd_card_disconnect(dev->card);

	/* Full shutdown sequence (mirrors kext CPcieAudioExtension::Shutdown):
	 * stops transport, clears doorbell, disables notification vector,
	 * sends disconnect command to firmware. Must happen BEFORE setting
	 * disconnecting flag so MMIO writes actually reach the hardware. */
	uad2_shutdown(dev);

	/* Disable global interrupt enable */
	uad2_write32(dev, REG_INTR_ENABLE, 0x0);

	/* Set disconnecting flag to guard MMIO access. After this point,
	 * uad2_read32/uad2_write32 become no-ops so the IRQ handler and
	 * any remaining ALSA callbacks won't touch disappeared hardware. */
	WRITE_ONCE(dev->disconnecting, true);

	/* Free IRQ before snd_card_free(). The card free will release
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

	/* snd_card_free() frees dev (card->private_data). Must be the
	 * last operation that references dev. */
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
