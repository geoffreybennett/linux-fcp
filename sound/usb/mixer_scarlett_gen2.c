// SPDX-License-Identifier: GPL-2.0
/*
 *   Focusrite Scarlett 6i6/18i8/18i20 Gen 2 and 2i2/4i4/8i6/18i8/18i20
 *   Gen 3 Driver for ALSA
 *
 *   Copyright (c) 2018-2020 by Geoffrey D. Bennett <g at b4.vu>
 *   Copyright (c) 2020 by Vladimir Sadovnikov <sadko4u at gmail.com>
 *
 *   Based on the Scarlett (Gen 1) Driver for ALSA:
 *
 *   Copyright (c) 2013 by Tobias Hoffmann
 *   Copyright (c) 2013 by Robin Gareus <robin at gareus.org>
 *   Copyright (c) 2002 by Takashi Iwai <tiwai at suse.de>
 *   Copyright (c) 2014 by Chris J Arges <chris.j.arges at canonical.com>
 *
 *   Many codes borrowed from audio.c by
 *     Alan Cox (alan at lxorguk.ukuu.org.uk)
 *     Thomas Sailer (sailer at ife.ee.ethz.ch)
 *
 *   Code cleanup:
 *   David Henningsson <david.henningsson at canonical.com>
 */

/* Mixer Interface for the Focusrite Scarlett 6i6/18i8/18i20 Gen 2 and
 * 2i2/4i4/8i6/18i8/18i20 Gen 3 audio interfaces. Based on the Gen 1
 * driver and rewritten.
 */

/* The protocol was reverse engineered by looking at the communication
 * between Focusrite Control 2.3.4 and the Focusrite(R) Scarlett 18i20
 * (firmware 1083) using usbmon in July-August 2018.
 *
 * Scarlett 18i8 Gen 2 support added in April 2019.
 *
 * Scarlett 6i6 Gen 2 support added in June 2019 (thanks to Martin
 * Wittmann for providing usbmon output and testing).
 *
 * Scarlett 4i4/8i6 Gen 3 support added in May 2020 (thanks to Laurent
 * Debricon for donating a 4i4 and to Fredrik Unger for providing 8i6
 * usbmon output and testing).
 *
 * Scarlett 18i8/18i20 Gen 3 support added in June 2020 (thanks to
 * Darren Jaeckel, Alex Sedlack, Clovis Lunel and Dirk Lattermann for
 * providing usbmon output, protocol traces and testing).
 *
 * Scarlett 2i2 Gen 3 support added in November 2020 (thanks to Alexander
 * Vorona for taking 2i2 USB protocol dumps).
 *
 *
 * This ALSA mixer gives access to (model-dependent):
 *  - input, output, mixer-matrix muxes
 *  - 18x10 mixer-matrix gain stages
 *  - gain/volume controls
 *  - level meters
 *  - line/inst level, pad, and air controls
 *  - enable/disable MSD mode
 *  - main/alt speaker switching
 *
 * <ditaa>
 *    /--------------\    18chn            20chn     /--------------\
 *    | Hardware  in +--+------\    /-------------+--+ ALSA PCM out |
 *    \--------------/  |      |    |             |  \--------------/
 *                      |      |    |    /-----\  |
 *                      |      |    |    |     |  |
 *                      |      v    v    v     |  |
 *                      |   +---------------+  |  |
 *                      |    \ Matrix  Mux /   |  |
 *                      |     +-----+-----+    |  |
 *                      |           |          |  |
 *                      |           |18chn     |  |
 *                      |           |          |  |
 *                      |           |     10chn|  |
 *                      |           v          |  |
 *                      |     +------------+   |  |
 *                      |     | Mixer      |   |  |
 *                      |     |     Matrix |   |  |
 *                      |     |            |   |  |
 *                      |     | 18x10 Gain |   |  |
 *                      |     |   stages   |   |  |
 *                      |     +-----+------+   |  |
 *                      |           |          |  |
 *                      |18chn      |10chn     |  |20chn
 *                      |           |          |  |
 *                      |           +----------/  |
 *                      |           |             |
 *                      v           v             v
 *                      ===========================
 *               +---------------+       +--—------------+
 *                \ Output  Mux /         \ Capture Mux /
 *                 +---+---+---+           +-----+-----+
 *                     |   |                     |
 *                10chn|   |                     |18chn
 *                     |   |                     |
 *  /--------------\   |   |                     |   /--------------\
 *  | S/PDIF, ADAT |<--/   |10chn                \-->| ALSA PCM in  |
 *  | Hardware out |       |                         \--------------/
 *  \--------------/       |
 *                         v
 *                  +-------------+    Software gain per channel.
 *                  | Master Gain |<-- 18i20 only: Switch per channel
 *                  +------+------+    to select HW or SW gain control.
 *                         |
 *                         |10chn
 *  /--------------\       |
 *  | Analogue     |<------/
 *  | Hardware out |
 *  \--------------/
 * </ditaa>
 *
 * Gen 3 devices have a Mass Storage Device (MSD) mode where a small
 * disk with registration and driver download information is presented
 * to the host. To access the full functionality of the device without
 * proprietary software, MSD mode can be disabled by:
 * - holding down the 48V button for five seconds while powering on
 *   the device, or
 * - using this driver and alsamixer to change the "MSD Mode" setting
 *   to Off, waiting two seconds, then power-cycling the device
 */

#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/moduleparam.h>

#include <sound/control.h>
#include <sound/tlv.h>

#include "usbaudio.h"
#include "mixer.h"
#include "helper.h"

#include "mixer_scarlett_gen2.h"

/* device_setup value to enable */
#define SCARLETT2_ENABLE 0x01

/* device_setup value to allow turning MSD mode back on */
#define SCARLETT2_MSD_ENABLE 0x02

/* some gui mixers can't handle negative ctl values */
#define SCARLETT2_VOLUME_BIAS 127

/* mixer range from -80dB to +6dB in 0.5dB steps */
#define SCARLETT2_MIXER_MIN_DB -80
#define SCARLETT2_MIXER_BIAS (-SCARLETT2_MIXER_MIN_DB * 2)
#define SCARLETT2_MIXER_MAX_DB 6
#define SCARLETT2_MIXER_MAX_VALUE \
	((SCARLETT2_MIXER_MAX_DB - SCARLETT2_MIXER_MIN_DB) * 2)

/* map from (dB + 80) * 2 to mixer value
 * for dB in 0 .. 172: int(8192 * pow(10, ((dB - 160) / 2 / 20)))
 */
static const u16 scarlett2_mixer_values[173] = {
	/* 8 items per row */
	0, 0, 0, 0, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	2, 2, 2, 2, 2, 2, 2, 3,
	3, 3, 3, 3, 4, 4, 4, 4,
	5, 5, 5, 6, 6, 6, 7, 7,
	8, 8, 9, 9, 10, 10, 11, 12,
	12, 13, 14, 15, 16, 17, 18, 19,
	20, 21, 23, 24, 25, 27, 29, 30,
	32, 34, 36, 38, 41, 43, 46, 48,
	51, 54, 57, 61, 65, 68, 73, 77,
	81, 86, 91, 97, 103, 109, 115, 122,
	129, 137, 145, 154, 163, 173, 183, 194,
	205, 217, 230, 244, 259, 274, 290, 307,
	326, 345, 365, 387, 410, 434, 460, 487,
	516, 547, 579, 614, 650, 689, 730, 773,
	819, 867, 919, 973, 1031, 1092, 1157, 1225,
	1298, 1375, 1456, 1543, 1634, 1731, 1833, 1942,
	2057, 2179, 2308, 2445, 2590, 2744, 2906, 3078,
	3261, 3454, 3659, 3876, 4105, 4349, 4606, 4879,
	5168, 5475, 5799, 6143, 6507, 6892, 7301, 7733,
	8192, 8677, 9191, 9736, 10313, 10924, 11571, 12257,
	12983, 13752, 14567, 15430, 16345
};

/* This is array of high parts of the 32-bit floating point values
 * which are matching the -80..+6 dB level with 0.5 dB step
 * The lowest value is encoded as -128.0f for compatibility with
 * the original software
 */
static const u16 scarlett2_sw_config_mixer_values[173] = {
	/* 8 items per row */
	0xc300, 0xc29f, 0xc29e, 0xc29d, 0xc29c, 0xc29b, 0xc29a, 0xc299,
	0xc298, 0xc297, 0xc296, 0xc295, 0xc294, 0xc293, 0xc292, 0xc291,
	0xc290, 0xc28f, 0xc28e, 0xc28d, 0xc28c, 0xc28b, 0xc28a, 0xc289,
	0xc288, 0xc287, 0xc286, 0xc285, 0xc284, 0xc283, 0xc282, 0xc281,
	0xc280, 0xc27e, 0xc27c, 0xc27a, 0xc278, 0xc276, 0xc274, 0xc272,
	0xc270, 0xc26e, 0xc26c, 0xc26a, 0xc268, 0xc266, 0xc264, 0xc262,
	0xc260, 0xc25e, 0xc25c, 0xc25a, 0xc258, 0xc256, 0xc254, 0xc252,
	0xc250, 0xc24e, 0xc24c, 0xc24a, 0xc248, 0xc246, 0xc244, 0xc242,
	0xc240, 0xc23e, 0xc23c, 0xc23a, 0xc238, 0xc236, 0xc234, 0xc232,
	0xc230, 0xc22e, 0xc22c, 0xc22a, 0xc228, 0xc226, 0xc224, 0xc222,
	0xc220, 0xc21e, 0xc21c, 0xc21a, 0xc218, 0xc216, 0xc214, 0xc212,
	0xc210, 0xc20e, 0xc20c, 0xc20a, 0xc208, 0xc206, 0xc204, 0xc202,
	0xc200, 0xc1fc, 0xc1f8, 0xc1f4, 0xc1f0, 0xc1ec, 0xc1e8, 0xc1e4,
	0xc1e0, 0xc1dc, 0xc1d8, 0xc1d4, 0xc1d0, 0xc1cc, 0xc1c8, 0xc1c4,
	0xc1c0, 0xc1bc, 0xc1b8, 0xc1b4, 0xc1b0, 0xc1ac, 0xc1a8, 0xc1a4,
	0xc1a0, 0xc19c, 0xc198, 0xc194, 0xc190, 0xc18c, 0xc188, 0xc184,
	0xc180, 0xc178, 0xc170, 0xc168, 0xc160, 0xc158, 0xc150, 0xc148,
	0xc140, 0xc138, 0xc130, 0xc128, 0xc120, 0xc118, 0xc110, 0xc108,
	0xc100, 0xc0f0, 0xc0e0, 0xc0d0, 0xc0c0, 0xc0b0, 0xc0a0, 0xc090,
	0xc080, 0xc060, 0xc040, 0xc020, 0xc000, 0xbfc0, 0xbf80, 0xbf00,
	0x0000, 0x3f00, 0x3f80, 0x3fc0, 0x4000, 0x4020, 0x4040, 0x4060,
	0x4080, 0x4090, 0x40a0, 0x40b0, 0x40c0
};


#define SCARLETT2_ANALOGUE_IN_MAX                8        /* Maximum number of analogue inputs */
#define SCARLETT2_ANALOGUE_OUT_MAX               10       /* Maximum number of analogue outputs */
#define SCARLETT2_ALL_IN_MAX                     42       /* Maximum number of all inputs */
#define SCARLETT2_ALL_OUT_MAX                    26       /* Maximum number of all outputs */
#define SCARLETT2_LEVEL_SWITCH_MAX               2        /* Maximum number of LINE/INST switches */
#define SCARLETT2_PAD_SWITCH_MAX                 8        /* Maximum number of PAD switches */
#define SCARLETT2_AIR_SWITCH_MAX                 8        /* Maximum number of AIR switches */
#define SCARLETT2_48V_SWITCH_MAX                 2        /* Maximum number of 48V switches */
#define SCARLETT2_BUTTON_MAX                     2        /* Hardware buttons on the 18i20 */
#define SCARLETT2_INPUT_MIX_MAX                  24       /* Maximum number of inputs to the mixer */
#define SCARLETT2_OUTPUT_MIX_MAX                 12       /* Maximum number of outputs from the mixer */
#define SCARLETT2_MUX_MAX                        77       /* The maximum possible MUX connection count */
#define SCARLETT2_NUM_METERS                     56       /* Number of meters: 18 inputs, 20 outputs, 18 matrix inputs (XX FIXME) */
#define SCARLETT2_IN_NAME_LEN                    12       /* Maximum length of the input name */
#define SCARLETT2_OUT_NAME_LEN                   12       /* Maximum length of the output name */
#define SCARLETT2_GAIN_HALO_LEVELS               3        /* Number of gain halo levels */
#define SCARLETT2_GAIN_HALO_LEDS_MAX             4        /* Maximum number of gain halo LEDs */

#define SCARLETT2_SW_CONFIG_BASE                 0xec

#define SCARLETT2_SW_CONFIG_PACKET_SIZE          1024     /* The maximum packet size used to transfer data */

#define SCARLETT2_SW_CONFIG_MIXER_INPUTS         30       /* 30 inputs per one mixer in config */
#define SCARLETT2_SW_CONFIG_MIXER_OUTPUTS        12       /* 12 outputs in config */
#define SCARLETT2_SW_CONFIG_OUTPUT_MAX           26       /* Maximum number of outputs */
#define SCARLETT2_SW_CONFIG_SIZE_OFFSET          0x08     /* 0xf4   - 0xec */
#define SCARLETT2_SW_CONFIG_STEREO_BITS_OFFSET   0x0c8    /* 0x1b4  - 0xec */
#define SCARLETT2_SW_CONFIG_VOLUMES_OFFSET       0x0d0    /* 0x1bc  - 0xec */
#define SCARLETT2_SW_CONFIG_MIXER_OFFSET         0xf04    /* 0xff0  - 0xec */

/* Hardware port types:
 * - None (no input to mux)
 * - Analogue I/O
 * - S/PDIF I/O
 * - ADAT I/O
 * - Mixer I/O
 * - PCM I/O
 */
enum {
	SCARLETT2_PORT_TYPE_ANALOGUE = 0,       /* Analogue input/output        */
	SCARLETT2_PORT_TYPE_SPDIF = 1,          /* S/PDIF input/oputput         */
	SCARLETT2_PORT_TYPE_ADAT = 2,           /* ADAT input/output            */
	SCARLETT2_PORT_TYPE_ADAT2 = 3,          /* ADAT2 input/output (mapping) */
	SCARLETT2_PORT_TYPE_MIX = 3,            /* Mixer input/output           */
	SCARLETT2_PORT_TYPE_PCM = 4,            /* PCM input/output             */
	SCARLETT2_PORT_TYPE_INT_MIC = 5,        /* Internal microphone          */
	SCARLETT2_PORT_TYPE_TALKBACK = 6,       /* Talkback source              */
	SCARLETT2_PORT_TYPE_COUNT = 7
};

enum {
	SCARLETT2_PORT_ID_NONE = 0,
	SCARLETT2_PORT_ID_ANALOGUE = 0x80,
	SCARLETT2_PORT_ID_SPDIF = 0x180,
	SCARLETT2_PORT_ID_ADAT = 0x200,
	SCARLETT2_PORT_ID_MIX = 0x300,
	SCARLETT2_PORT_ID_PCM = 0x600,
	SCARLETT2_PORT_ID_MASK  = 0x0f80,
	SCARLETT2_PORT_NUM_MASK = 0x007f
};

/* Count of total I/O and number available at each sample rate */
enum {
	SCARLETT2_PORT_IN = 0,
	SCARLETT2_PORT_OUT = 1,
	SCARLETT2_PORT_OUT_44 = 2,
	SCARLETT2_PORT_OUT_88 = 3,
	SCARLETT2_PORT_OUT_176 = 4,
	SCARLETT2_PORT_DIRECTIONS = 5,
};

/* Configuration parameters that can be read and written */
enum {
	SCARLETT2_CONFIG_BUTTONS = 0,
	SCARLETT2_CONFIG_LINE_OUT_VOLUME = 1,
	SCARLETT2_CONFIG_SW_HW_SWITCH = 2,
	SCARLETT2_CONFIG_LEVEL_SWITCH = 3,
	SCARLETT2_CONFIG_PAD_SWITCH = 4,
	SCARLETT2_CONFIG_AIR_SWITCH = 5,
	SCARLETT2_CONFIG_SPDIF_SWITCH = 6,
	SCARLETT2_CONFIG_48V_SWITCH = 7,
	SCARLETT2_CONFIG_MSD_SWITCH = 8,
	SCARLETT2_CONFIG_MAIN_ALT_SPEAKER_SWITCH = 9,
	SCARLETT2_CONFIG_SPEAKER_SWITCHING_SWITCH = 10,
	SCARLETT2_CONFIG_GAIN_HALO_ENABLE = 11,
	SCARLETT2_CONFIG_GAIN_HALO_LEDS = 12,
	SCARLETT2_CONFIG_GAIN_HALO_LEVELS = 13,
	SCARLETT2_CONFIG_MIX_TALKBACK = 14,
	SCARLETT2_CONFIG_RETAIN_48V = 15,
	SCARLETT2_CONFIG_MUTES = 16,
	SCARLETT2_CONFIG_COUNT = 17
};

static const char *const scarlett2_button_names[SCARLETT2_BUTTON_MAX] = {
	"Mute", "Dim"
};

struct scarlett2_port_name {
	s8 direction;  /* Direction of port - SCARLETT2_PORT_* */
	s8 type;  /* Type of port - SCARLETT2_PORT_TYPE_* */
	s8 index; /* Index of port */
	const char *name; /* The name of port */
};

struct scarlett2_sw_port_mapping {
	s8 direction; /* Direction of port - SCARLETT2_PORT_* */
	s8 type;      /* Type of port - SCARLETT2_PORT_TYPE_* */
	s8 index;     /* The start index of routed port */
	s8 count;     /* Number of ports */
};

/* Description of each hardware port type:
 * - id: hardware ID for this port type
 * - num: number of sources/destinations of this port type
 * - src_descr: printf format string for mux input selections
 * - src_num_offset: added to channel number for the fprintf
 * - dst_descr: printf format string for mixer controls
 */
struct scarlett2_ports {
	u16 id;
	int num[SCARLETT2_PORT_DIRECTIONS];
	const char * const src_descr;
	int src_num_offset;
	const char * const dst_descr;
	const u8 * const dst_remapping;
};

/* Location, size, and activation command number for the configuration
 * parameters
 */
struct scarlett2_config {
	u8 offset;
	u8 size;
	u8 activate;
};

/* The device descriptor structure */
struct scarlett2_device_info {
	u32 usb_id; /* USB device identifier */
	u8 line_out_hw_vol; /* line out hw volume is sw controlled */
	u8 button_count; /* number of buttons */
	u8 level_input_count; /* inputs with level selectable */
	u8 pad_input_count; /* inputs with pad selectable */
	u8 air_input_count; /* inputs with air selectable */
	u8 power_48v_count; /* 48V phantom power */
	u8 has_retain48v;  /* Retain 48V switch is present */
	u8 has_msd_mode; /* Gen 3 devices have an internal MSD mode switch */
	u8 has_speaker_switching; /* main/alt speaker switching */
	u8 has_talkback; /* 18i20 Gen 3 has 'talkback' feature */
	u8 has_mux; /* MUX (routing) is present */
	u8 has_mixer; /* Internal Mixer is present */
	u8 has_sw_config; /* Software configuration is present */
	u8 has_meters; /* Device has meters */
	u8 has_hw_volume; /* Has hardware volume control */
	u8 gain_halos_count; /* Number of gain halos */
	const struct scarlett2_port_name * const port_names; /* Special names of ports */
	const struct scarlett2_sw_port_mapping * const sw_port_mapping; /* Software port mapping */
	const u8 mux_size[SCARLETT2_PORT_DIRECTIONS]; /* The maximum number of elements per mux */
	struct scarlett2_ports ports[SCARLETT2_PORT_TYPE_COUNT];
	const struct scarlett2_config * const config;
};

struct scarlett2_sw_cfg_volume {
	__le16 volume;  /* volume */
	u8 changed;     /* change flag */
	u8 flags;       /* some flags? */
};

/* Software configuration of the scarlett device */
struct scarlett2_sw_cfg {
	__le16 all_size;                                                    /* +0x0000: overall size, 0x1990 for all devices */
	__le16 magic1;                                                      /* +0x0002: magic number: 0x3006 */
	__le32 version;                                                     /* +0x0004: probably version */
	__le16 szof;                                                        /* +0x0008: the overall size, 0x1984 for all devices  */
	__le16 __pad0;                                                      /* +0x000a: ???????? */
	u8 out_mux[SCARLETT2_SW_CONFIG_OUTPUT_MAX];                         /* +0x00f8: output routing */
	u8 __pad1[0x0066];                                                  /* +0x0112: ???????? */
	u8 mixer_in_mux[SCARLETT2_SW_CONFIG_MIXER_INPUTS];                  /* +0x008c: 'custom mix' input routing                    */
	u8 mixer_in_map[SCARLETT2_SW_CONFIG_MIXER_INPUTS];                  /* +0x00aa: 'custom mix' input mapping                    */
	__le32 stereo_sw;                                                   /* +0x01b4: stereo configuration for each port (bit mask) */
	__le32 mute_sw;                                                     /* +0x01b8: mute switches (bit mask) */
	struct scarlett2_sw_cfg_volume volume[SCARLETT2_ANALOGUE_OUT_MAX];  /* +0x01bc: Volume settings of each output */
	u8 __pad2[0x01dc];                                                  /* +0x01e4: ???????? */
	u8 in_alias[SCARLETT2_ALL_IN_MAX][SCARLETT2_IN_NAME_LEN];           /* +0x03c0: Symbolic names of inputs */
	u8 __pad3[0x0438];                                                  /* +0x0420: ???????? */
	u8 out_alias[SCARLETT2_ALL_OUT_MAX][SCARLETT2_OUT_NAME_LEN];        /* +0x09f0: Symbolic names of outputs */
	u8 __pad4[0x04c8];                                                  /* +0x0b28: ???????? */
	__le32 mixer[SCARLETT2_SW_CONFIG_MIXER_OUTPUTS][SCARLETT2_SW_CONFIG_MIXER_INPUTS];  /* +0x0ff0: Matrix mixer settings */
	u8 __pad5[0x01e0];                                                  /* +0x1590: ???????? */
	s8 mixer_pan[SCARLETT2_SW_CONFIG_MIXER_OUTPUTS][SCARLETT2_SW_CONFIG_MIXER_INPUTS];  /* +0x1684: Input pan settings for mixer */
	u8 __pad6[0x0078];                                                  /* +0x17ec: ???????? */
	__le32 mixer_mute[SCARLETT2_SW_CONFIG_MIXER_OUTPUTS];               /* +0x1950: Mute settings for mixer inputs */
	__le32 mixer_solo[SCARLETT2_SW_CONFIG_MIXER_OUTPUTS];               /* +0x1980: Solo settings for mixer inputs */
	u8 __pad7[0x004a];                                                  /* +0x19b0: ???????? */
	__le32 mixer_bind;                                                  /* +0x19fa: output to 'custom mix' routing bitmap */
	u8 __pad8[0x006e];                                                  /* +0x19fe: ???????? */
	__le32 checksum;                                                    /* +0x1a6c: checksum of the area */
} __packed;

struct scarlett2_mixer_data {
	struct usb_mixer_interface *mixer;
	struct mutex usb_mutex; /* prevent sending concurrent USB requests */
	struct mutex data_mutex; /* lock access to this data */
	struct delayed_work work;
	const struct scarlett2_device_info *info;
	__u8 interface; /* vendor-specific interface number */
	__u8 endpoint; /* interrupt endpoint address */
	__le16 maxpacketsize;
	__u8 interval;
	int num_inputs; /* Overall number of inputs */
	int num_outputs; /* Overall number of outputs */
	u16 scarlett2_seq;
	u8 vol_updated; /* Flag that indicates that volume has been updated */
	u8 line_ctl_updated; /* Flag that indicates that state of PAD, INST buttons have been updated */
	u8 speaker_updated; /* Flag that indicates that speaker/talkback has been updated */
	u8 master_vol;
	u8 vol[SCARLETT2_ANALOGUE_OUT_MAX];
	u8 mutes[SCARLETT2_ALL_OUT_MAX];                                  /* Mute switches for each output */
	u8 vol_sw_hw_switch[SCARLETT2_ANALOGUE_OUT_MAX];
	u8 level_switch[SCARLETT2_LEVEL_SWITCH_MAX];
	u8 pad_switch[SCARLETT2_PAD_SWITCH_MAX];
	u8 air_switch[SCARLETT2_AIR_SWITCH_MAX];
	u8 pow_switch[SCARLETT2_48V_SWITCH_MAX];
	u8 msd_switch;
	u8 retain48v_switch;
	u8 speaker_switch;
	u8 talkback_switch;
	u8 buttons[SCARLETT2_BUTTON_MAX];
	u8 ghalo_custom;                                                  /* Custom gain halos flag */
	u8 ghalo_leds[SCARLETT2_GAIN_HALO_LEDS_MAX];                      /* Color of each gain halo led */
	u8 ghalo_levels[SCARLETT2_GAIN_HALO_LEVELS];                      /* Gain halo colors for each level */

	struct snd_kcontrol *master_vol_ctl;
	struct snd_kcontrol *speaker_ctl;
	struct snd_kcontrol *talkback_ctl;
	struct snd_kcontrol *vol_ctls[SCARLETT2_ANALOGUE_OUT_MAX];
	struct snd_kcontrol *mute_ctls[SCARLETT2_ALL_OUT_MAX];
	struct snd_kcontrol *pad_ctls[SCARLETT2_PAD_SWITCH_MAX];
	struct snd_kcontrol *level_ctls[SCARLETT2_LEVEL_SWITCH_MAX];
	struct snd_kcontrol *pow_ctls[SCARLETT2_48V_SWITCH_MAX];
	struct snd_kcontrol *button_ctls[SCARLETT2_BUTTON_MAX];
	struct snd_kcontrol *mix_talkback_ctls[SCARLETT2_OUTPUT_MIX_MAX]; /* Talkback controls for each mix */
	s8 mux[SCARLETT2_MUX_MAX];                                        /* Routing of outputs */
	u8 mix[SCARLETT2_INPUT_MIX_MAX * SCARLETT2_OUTPUT_MIX_MAX];       /* Matrix mixer */
	u8 mix_talkback[SCARLETT2_OUTPUT_MIX_MAX];                        /* Talkback enable for mixer output */
	u8 mix_mutes[SCARLETT2_INPUT_MIX_MAX * SCARLETT2_OUTPUT_MIX_MAX]; /* Mixer input mutes */

	/* Software configuration */
	struct scarlett2_sw_cfg *sw_cfg;                                  /* Software configuration data */
};

/*
 * PRO-class device configuration (8i6, 18i8, 18i20)
 */
static const struct scarlett2_config scarlett2_pro_config_items[SCARLETT2_CONFIG_COUNT] = {
	[SCARLETT2_CONFIG_BUTTONS] =                   /* Mute/Dim Buttons */
		{ .offset = 0x31, .size = 1, .activate = 2 },

	[SCARLETT2_CONFIG_LINE_OUT_VOLUME] =           /* Line Out Volume */
		{ .offset = 0x34, .size = 2, .activate = 1 },

	[SCARLETT2_CONFIG_SW_HW_SWITCH] =              /* SW/HW Volume Switch */
		{ .offset = 0x66, .size = 1, .activate = 3 },

	[SCARLETT2_CONFIG_LEVEL_SWITCH] =              /* Inst Switch */
		{ .offset = 0x7c, .size = 1, .activate = 7 },

	[SCARLETT2_CONFIG_PAD_SWITCH] =                /* Pad Switch */
		{ .offset = 0x84, .size = 1, .activate = 8 },

	[SCARLETT2_CONFIG_AIR_SWITCH] =                /* Air Switch */
		{ .offset = 0x8c, .size = 1, .activate = 8 },

	[SCARLETT2_CONFIG_SPDIF_SWITCH] =              /* S/PDIF Source */
		{ .offset = 0x94, .size = 1, .activate = 6 },

	[SCARLETT2_CONFIG_48V_SWITCH] =                /* Phantom (48V) power */
		{ .offset = 0x9c, .size = 1, .activate = 8 },

	[SCARLETT2_CONFIG_MSD_SWITCH] =                /* MSD Mode */
		{ .offset = 0x9d, .size = 1, .activate = 6 },

	[SCARLETT2_CONFIG_MAIN_ALT_SPEAKER_SWITCH] =   /* Alternate Speaker and Talkback switches */
		{ .offset = 0x9f, .size = 1, .activate = 10 },

	[SCARLETT2_CONFIG_SPEAKER_SWITCHING_SWITCH] =  /* Speaker Switching */
		{ .offset = 0xa0, .size = 1, .activate = 10 },

	[SCARLETT2_CONFIG_GAIN_HALO_ENABLE] =          /* Gain Halo Enable flag: bit 1 enables immediate values for gain halo */
		{ .offset = 0xa1, .size = 1, .activate = 9  },

	[SCARLETT2_CONFIG_GAIN_HALO_LEDS] =            /* Gain Halo LED colors: 1 bit per each R,G,B component */
		{ .offset = 0xa2, .size = 1, .activate = 9  },

	[SCARLETT2_CONFIG_GAIN_HALO_LEVELS] =          /* Gain Halo Colors for corresponding levels: 1 byte per RGB in order: Clip, Pre-Clip, Good */
		{ .offset = 0xa6, .size = 1, .activate = 11  },

	[SCARLETT2_CONFIG_MIX_TALKBACK] =              /* Talkback enable flags for each output of internal mixer */
		{ .offset = 0xb0, .size = 2, .activate = 10 },

	[SCARLETT2_CONFIG_RETAIN_48V] =                /* Retain 48V switch */
		{ .offset = 0x9e, .size = 1, .activate = 0 },
	
	[SCARLETT2_CONFIG_MUTES] =                     /* Hardware mutes for each analog output */
		{ .offset = 0x5c, .size = 1, .activate = 1 }
};

/*
 * Configuration space for home segment devices like Scarlett 2i2
 */
static const struct scarlett2_config scarlett2_home_config_items[SCARLETT2_CONFIG_COUNT] = {
	[SCARLETT2_CONFIG_LEVEL_SWITCH] =              /* Inst Switch */
		{ .offset = 0x08, .size = 1, .activate = 7 },

	[SCARLETT2_CONFIG_AIR_SWITCH] =                /* Air Switch */
		{ .offset = 0x09, .size = 1, .activate = 8 },
};

/* proprietary request/response format */
struct scarlett2_usb_packet {
	__le32 cmd;
	__le16 size;
	__le16 seq;
	__le32 error;
	__le32 pad;
	u8 data[];
};

/*** Model-specific data ***/
static const struct scarlett2_port_name s6i6_gen2_ports[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Headphones 2 R" },
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s6i6_gen2_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 6  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 4  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 6  },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s6i6_gen2_info = {
	.usb_id = USB_ID(0x1235, 0x8203),
	
	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first two analogue inputs have an optional pad. */
	.pad_input_count = 2,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s6i6_gen2_ports,

	.sw_port_mapping = s6i6_gen2_sw_port_mapping,

	.mux_size = { 42, 42, 42, 42, 42 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 4, 4, 4, 4, 4 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 10, 18, 18, 18, 18 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 6, 6, 6, 6, 6 },
			.src_descr = "PCM In %d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_port_name s18i8_gen2_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 4, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 5, "Headphones 2 R" },
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s18i8_gen2_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 20 },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s18i8_gen2_info = {
	.usb_id = USB_ID(0x1235, 0x8204),

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first four analogue inputs have an optional pad. */
	.pad_input_count = 4,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s18i8_gen2_port_names,

	.sw_port_mapping = s18i8_gen2_sw_port_mapping,

	.mux_size = { 60, 60, 60, 56, 50 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 8, 6, 6, 6, 6 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_ADAT] = {
			.id = SCARLETT2_PORT_ID_ADAT,
			.num = { 8, 0, 0, 0, 0 },
			.src_descr = "ADAT In %d",
			.src_num_offset = 1,
			.dst_descr = "ADAT Out %d",
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 10, 18, 18, 18, 18 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 8, 18, 18, 14, 10 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_port_name s18i20_gen2_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 6, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 7, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 8, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 9, "Headphones 2 R" },
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s18i20_gen2_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 10 },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ADAT2,    0, 4  },

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT2,    0, 4  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 20 },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s18i20_gen2_info = {
	.usb_id = USB_ID(0x1235, 0x8201),

	/* The analogue line outputs on the 18i20 can be switched
	 * between software and hardware volume control
	 */
	.line_out_hw_vol = 1,

	/* Mute and dim buttons */
	.button_count = 2,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s18i20_gen2_port_names,

	.sw_port_mapping = s18i20_gen2_sw_port_mapping,

	.mux_size = { 77, 77, 77, 73, 46 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 8, 10, 10, 10, 10 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			/* S/PDIF outputs aren't available at 192KHz
			 * but are included in the USB mux I/O
			 * assignment message anyway
			 */
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_ADAT] = {
			.id = SCARLETT2_PORT_ID_ADAT,
			.num = { 8, 8, 8, 4, 0 },
			.src_descr = "ADAT In %d",
			.src_num_offset = 1,
			.dst_descr = "ADAT Out %d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 10, 18, 18, 18, 18 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 20, 18, 18, 14, 10 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_device_info s2i2_gen3_info = {
	.usb_id = USB_ID(0x1235, 0x8210),

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first two analogue inputs have an optional "air" feature. */
	.air_input_count = 2,

	/* One 48V phantom power switch */
	/* .power_48v_count = 1, */

	.config = scarlett2_home_config_items
};

static const struct scarlett2_port_name s4i4_gen3_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Headphones L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Headphones R" },
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s4i4_gen3_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 4  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_PCM,      4, 2  }, /* Loopback = { PCM-04, PCM-05 } ??? */

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 4  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 4  },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s4i4_gen3_info = {
	.usb_id = USB_ID(0x1235, 0x8212),

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first two analogue inputs have an optional pad. */
	.pad_input_count = 2,

	/* The first two analogue inputs have an optional "air" feature. */
	.air_input_count = 2,
	
	/* One 48V phantom power switch */
	.power_48v_count = 1,

	/* Gen 3 devices have an MSD mode */
	.has_msd_mode = 1,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s4i4_gen3_port_names,

	.sw_port_mapping = s4i4_gen3_sw_port_mapping,

	.mux_size = { 77, 77, 77, 73, 46 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 4, 4, 4, 4, 4 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 6, 8, 8, 8, 8 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 4, 6, 6, 6, 6 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_port_name s8i6_gen3_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Headphones 3 R" },
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s8i6_gen3_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 4  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_PCM,      4, 2  }, /* Loopback = { PCM-05, PCM-06 } ??? */

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 6  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 10 },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s8i6_gen3_info = {
	.usb_id = USB_ID(0x1235, 0x8213),

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first two analogue inputs have an optional pad. */
	.pad_input_count = 2,

	/* The first two analogue inputs have an optional "air" feature. */
	.air_input_count = 2,
	
	/* One 48V phantom power switch */
	.power_48v_count = 1,

	/* Gen 3 devices have an MSD mode */
	.has_msd_mode = 1,

	/* Has a 'Retain 48V' switch */
	.has_retain48v = 1,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s8i6_gen3_port_names,

	.sw_port_mapping = s8i6_gen3_sw_port_mapping,

	.mux_size = { 42, 42, 42, 42, 42 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 6, 4, 4, 4, 4 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 8, 8, 8, 8, 8 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 6, 10, 10, 10, 10 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_port_name s18i8_gen3_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Main Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Main Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 4, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 5, "Headphones 2 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 6, "Alt Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 7, "Alt Monitor R" },

	{ SCARLETT2_PORT_OUT,  SCARLETT2_PORT_TYPE_PCM, 10, "Loopback L" },
	{ SCARLETT2_PORT_OUT,  SCARLETT2_PORT_TYPE_PCM, 11, "Loopback R" },
	
	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s18i8_gen3_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_PCM,      10, 2 }, /* Loopback */

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 20 },

	{ -1, -1, -1, -1}
};

static const u8 s18i8_analogue_out_remapping[8] = { 0, 1, 6, 7, 2, 3, 4, 5 };

static const struct scarlett2_device_info s18i8_gen3_info = {
	.usb_id = USB_ID(0x1235, 0x8214),

	/* The analogue line outputs on the 18i8 can be switched
	 * between software and hardware volume control
	 */
	.line_out_hw_vol = 1,

	/* Virtual mute and dim buttons */
	.button_count = 2,

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first four analogue inputs have an optional pad. */
	.pad_input_count = 4,

	/* The first four analogue inputs have an optional "air" feature. */
	.air_input_count = 4,

	/* Two 48V phantom power switches */
	.power_48v_count = 2,

	/* Gen 3 devices have an MSD mode */
	.has_msd_mode = 1,

	/* Has a main/alt speaker switch */
	.has_speaker_switching = 1,

	/* Has a 'Retain 48V' switch */
	.has_retain48v = 1,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.gain_halos_count = 4,

	.port_names = s18i8_gen3_port_names,

	.sw_port_mapping = s18i8_gen3_sw_port_mapping,

	.mux_size = { 60, 60, 60, 56, 50 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 8, 8, 8, 8, 8 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d",
			.dst_remapping = s18i8_analogue_out_remapping
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_ADAT] = {
			.id = SCARLETT2_PORT_ID_ADAT,
			.num = { 8, 0, 0, 0, 0 },
			.src_descr = "ADAT In %d",
			.src_num_offset = 1,
			.dst_descr = "ADAT Out %d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 10, 20, 20, 20, 20 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 8, 20, 20, 16, 10 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_port_name s18i20_gen3_port_names[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, "Main Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 1, "Main Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 2, "Alt Monitor L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 3, "Alt Monitor R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 6, "Headphones 1 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 7, "Headphones 1 R" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 8, "Headphones 2 L" },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 9, "Headphones 2 R" },

	{ SCARLETT2_PORT_OUT,  SCARLETT2_PORT_TYPE_PCM, 8, "Loopback L" },
	{ SCARLETT2_PORT_OUT,  SCARLETT2_PORT_TYPE_PCM, 9, "Loopback R" },

	{ -1, -1, -1, NULL }
};

static const struct scarlett2_sw_port_mapping s18i20_gen3_sw_port_mapping[] = {
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, 0, 10 },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ADAT2,    0, 4  },
	{ SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_PCM,      8, 2  }, /* Loopback */

	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ANALOGUE, 0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_SPDIF,    0, 2  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT,     0, 8  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_ADAT2,    0, 4  },
	{ SCARLETT2_PORT_IN,  SCARLETT2_PORT_TYPE_PCM,      0, 20 },

	{ -1, -1, -1, -1}
};

static const struct scarlett2_device_info s18i20_gen3_info = {
	.usb_id = USB_ID(0x1235, 0x8215),

	/* The analogue line outputs on the 18i20 can be switched
	 * between software and hardware volume control
	 */
	.line_out_hw_vol = 1,

	/* Mute and dim buttons */
	.button_count = 2,

	/* The first two analogue inputs can be switched between line
	 * and instrument levels.
	 */
	.level_input_count = 2,

	/* The first eight analogue inputs have an optional pad. */
	.pad_input_count = 8,

	/* The first eight analogue inputs have an optional "air" feature. */
	.air_input_count = 8,
	
	/* Two 48V phantom power switches */
	.power_48v_count = 2,

	/* Gen 3 devices have an MSD mode */
	.has_msd_mode = 1,

	/* Has a main/alt speaker switch */
	.has_speaker_switching = 1,

	/* Has a talkback speaker switch */
	.has_talkback = 1,

	/* Has a 'Retain 48V' switch */
	.has_retain48v = 1,

	.has_mux = 1,

	.has_mixer = 1,

	.has_sw_config = 1,

	.has_meters = 1,

	.has_hw_volume = 1,

	.port_names = s18i20_gen3_port_names,

	.sw_port_mapping = s18i20_gen3_sw_port_mapping,

	.mux_size = { 77, 77, 77, 73, 46 },

	.ports = {
		[SCARLETT2_PORT_TYPE_ANALOGUE] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE,
			.num = { 8, 10, 10, 10, 10 },
			.src_descr = "Analogue In %02d",
			.src_num_offset = 1,
			.dst_descr = "Analogue Out %02d"
		},
		[SCARLETT2_PORT_TYPE_SPDIF] = {
			.id = SCARLETT2_PORT_ID_SPDIF,
			.num = { 2, 2, 2, 2, 2 },
			.src_descr = "S/PDIF In %d",
			.src_num_offset = 1,
			.dst_descr = "S/PDIF Out %d"
		},
		[SCARLETT2_PORT_TYPE_ADAT] = {
			.id = SCARLETT2_PORT_ID_ADAT,
			.num = { 8, 8, 8, 8, 0 },
			.src_descr = "ADAT In %d",
			.src_num_offset = 1,
			.dst_descr = "ADAT Out %d"
		},
		[SCARLETT2_PORT_TYPE_MIX] = {
			.id = SCARLETT2_PORT_ID_MIX,
			.num = { 12, 24, 24, 24, 24 },
			.src_descr = "Mix %c Out",
			.src_num_offset = 'A',
			.dst_descr = "Mix In %02d"
		},
		[SCARLETT2_PORT_TYPE_PCM] = {
			.id = SCARLETT2_PORT_ID_PCM,
			.num = { 20, 20, 20, 18, 10 },
			.src_descr = "PCM In %02d",
			.src_num_offset = 1,
			.dst_descr = "PCM Out %02d"
		},
		[SCARLETT2_PORT_TYPE_INT_MIC] = {
			.id = SCARLETT2_PORT_ID_ANALOGUE + 8,
			.num = { 1, 0, 0, 0, 0 },
			.src_descr = "Internal Mic"
		},
		[SCARLETT2_PORT_TYPE_TALKBACK] = {
			.id = SCARLETT2_PORT_ID_MIX + 24,
			.num = { 0, 1, 1, 1, 1 },
			.dst_descr = "Talkback"
		},
	},

	.config = scarlett2_pro_config_items
};

static const struct scarlett2_device_info *scarlett2_supported_devices[] = {
	/* Supported Gen2 devices */
	&s6i6_gen2_info,
	&s18i8_gen2_info,
	&s18i20_gen2_info,

	/* Supported Gen3 devices */
	&s2i2_gen3_info,
	&s4i4_gen3_info,
	&s8i6_gen3_info,
	&s18i8_gen3_info,
	&s18i20_gen3_info,

	/* End of list */
	NULL
};

/*** USB Interactions ***/

/* Interrupt flags for volume, mute/dim button, and sync changes */
#define SCARLETT2_USB_INTERRUPT_ACK              0x00000001
#define SCARLETT2_USB_INTERRUPT_SYNC_CHANGE      0x00000008
#define SCARLETT2_USB_INTERRUPT_BUTTON_CHANGE    0x00200000
#define SCARLETT2_USB_INTERRUPT_VOL_CHANGE       0x00400000
#define SCARLETT2_USB_INTERRUPT_LINE_CTL_CHANGE  0x00800000
#define SCARLETT2_USB_INTERRUPT_SPEAKER_CHANGE   0x01000000

/* Commands for sending/receiving requests/responses */
#define SCARLETT2_USB_CMD_INIT 0
#define SCARLETT2_USB_CMD_REQ 2
#define SCARLETT2_USB_CMD_RESP 3

#define SCARLETT2_USB_INIT_1                     0x00000000
#define SCARLETT2_USB_INIT_2                     0x00000002
#define SCARLETT2_USB_CONFIG_SAVE                0x00000006
#define SCARLETT2_USB_GET_METER_LEVELS           0x00001001
#define SCARLETT2_USB_SET_MIX                    0x00002002
#define SCARLETT2_USB_GET_MUX                    0x00003001
#define SCARLETT2_USB_SET_MUX                    0x00003002
#define SCARLETT2_USB_GET_DATA                   0x00800000
#define SCARLETT2_USB_SET_DATA                   0x00800001
#define SCARLETT2_USB_DATA_CMD                   0x00800002

/*#define SCARLETT2_USB_VOLUME_STATUS_OFFSET 0x31*/
#define SCARLETT2_VOLUMES_BASE                   0x34


#define SCARLETT2_USB_METER_LEVELS_GET_MAGIC 1

/* volume status is read together (matches scarlett2_config_items[]) */
struct scarlett2_usb_volume_status {
	u8 pad0[0x31];

	/* mute & dim buttons */
	u8 buttons[SCARLETT2_BUTTON_MAX]; /* 0x31 */

	u8 pad1; /* 0x33 */

	/* software volume setting */
	s16 sw_vol[SCARLETT2_ANALOGUE_OUT_MAX]; /* 0x34 */

	/* actual volume of output inc. dim (-18dB) */
	s16 hw_vol[SCARLETT2_ANALOGUE_OUT_MAX]; /* 0x48 */

	/* hardware mute buttons */
	u8 mute[SCARLETT2_ANALOGUE_OUT_MAX]; /* 0x5C */

	/* sw (0) or hw (1) controlled */
	u8 sw_hw_switch[SCARLETT2_ANALOGUE_OUT_MAX]; /* 0x66 */

	u8 pad3[6]; /* 0x70 */

	/* front panel volume knob */
	s16 master_vol; /* 0x76 */
	
	u8 pad4[0x88]; /* 0x78 */
} __packed;

/* Decode floating-point value into valid scarlett gain
 * The input floating-point value may be any (including Infs and NaNs)
 * The output integer value is in range of -160 to 12 (dB with 0.5 step)
 */
static int scarlett2_float_to_mixer_level(u32 v)
{
	u32 exp, frac;
	int sign, res;

	exp  = (v >> 23) & 0xff;
	if (exp < 0x7e) /* abs(v) < 0.5f */
		return 0;

	sign = (v >> 31);
	if (exp > 0x85) /* abs(v) > 80.0f ? */
		return (sign) ? -160 : 12;

	/* Compute the fraction part */
	frac = (v & 0x007fffffu) | 0x00800000u; /* 24 bits normalized */
	frac = frac >> (0x95 - exp); /* 0x7f - exp + 22 */
	res = (sign) ? -frac : frac;

	/* Limit the value and return */
	if (res < -160)
		return -160;
	return (res < 12) ? res : 12;
}

/* Convert a port number index (per info->ports) to a hardware ID */
static u32 scarlett2_id_to_mux(const struct scarlett2_ports *ports, int direction, int num)
{
	int port_type;

	if ((direction < 0) || (direction >= SCARLETT2_PORT_DIRECTIONS) || (num < 0))
		return 0;

	for (port_type = 0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type) {
		if (num < ports[port_type].num[direction])
			return ports[port_type].id + num;
		num -= ports[port_type].num[direction];
	}

	return 0; /* Could not encode port */
}

static int scarlett2_count_ports(const struct scarlett2_ports *ports, int direction) {
	int port_type, count = 0;

	for (port_type=0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type)
		count += ports[port_type].num[direction];

	return count;
}

/* Convert a hardware ID to port number index (per info->ports) */
static int scarlett2_mux_to_id(const struct scarlett2_ports *ports, int direction, u32 mux_id)
{
	int port_type, port_id, port_num, port_base;

	if ((direction < 0) || (direction >= SCARLETT2_PORT_DIRECTIONS))
		return -1; /* Could not decode port */

	port_id  = mux_id & SCARLETT2_PORT_ID_MASK;
	if (port_id == SCARLETT2_PORT_ID_NONE)
		return -1;
	
	port_num = mux_id & SCARLETT2_PORT_NUM_MASK;
	port_base= 0;

	for (port_type=0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type) {
		/* Port identifier matches? */
		if (port_id == (ports[port_type].id & SCARLETT2_PORT_ID_MASK)) {
			if (port_num < ports[port_type].num[direction])
				return port_base + port_num;
			port_num -= ports[port_type].num[direction];
		}

		port_base += ports[port_type].num[direction];
	}

	return -1; /* Could not decode port */
}

static int scarlett2_output_index(struct usb_mixer_interface *mixer, int port_type, int port_num)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	int i, type, index, count;

	static const int assignment_order[] = {
		SCARLETT2_PORT_TYPE_ANALOGUE,
		SCARLETT2_PORT_TYPE_SPDIF,
		SCARLETT2_PORT_TYPE_ADAT,
	};

	/* Compute proper base index and return mute index in the mute array if possible */
	for (i=0, index=0; i<sizeof(assignment_order)/sizeof(int); ++i, index += count) {
		type  = assignment_order[i];
		count = info->ports[type].num[SCARLETT2_PORT_OUT];
		if (port_type == type)
			return (port_num < count) ? (index + port_num) : -1;
	}

	return -1;
}

/* Format port number to the proper port name */
static char *scarlett2_fmt_port_name(char *out, int len, const char *fmt, const struct scarlett2_device_info *info, int direction, int num)
{
	int port_type;
	const char *xfmt;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	char full[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	const struct scarlett2_ports *ports = info->ports;
	const struct scarlett2_port_name *port_name;

	strncpy(out, "Off", len);
	if ((direction < 0) || (direction >= SCARLETT2_PORT_DIRECTIONS) || (num < 0))
		return out;

	for (port_type = 0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type) {
		/* The number does match the range ? */
		if (num < ports[port_type].num[direction]) {
			/* Is there 'special' name found ? */
			name[0] = '\0';
			for (port_name = info->port_names; (port_name != NULL) && (port_name->name != NULL); ++port_name) {
				if ((port_name->direction == direction) &&
				    (port_name->type == port_type) &&
				    (port_name->index == num)) {
					snprintf(name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, " (%s)", port_name->name);
					break;
				}
			}

			/* Format the physical port name */
			if ((direction == SCARLETT2_PORT_OUT) && (ports[port_type].dst_remapping)) /* Apply port remapping */
				num = ports[port_type].dst_remapping[num];
			xfmt = (direction == SCARLETT2_PORT_IN) ? ports[port_type].src_descr : ports[port_type].dst_descr;
			num += (direction == SCARLETT2_PORT_IN) ? ports[port_type].src_num_offset : 1;
			snprintf(full, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, xfmt, num);
			strncat(full, name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN - strlen(full) - 1);

			/* Output the final format */
			snprintf(out, len, fmt, full);
			return out;
		}

		num -= ports[port_type].num[direction];
	}

	return out;
}

/* get the starting port index number for a given port type/direction */
static int scarlett2_get_port_num(const struct scarlett2_ports *ports, int direction, int type, int num)
{
	int i;
	for (i = 0; i < type; i++)
		num += ports[i].num[direction];

	return num;
}

static int scarlett2_decode_port(int *out_type, int *out_num, const struct scarlett2_ports *ports, int direction, int id)
{
	int port_type;

	for (port_type = 0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type) {
		/* The number does match the range ? */
		if (id < ports[port_type].num[direction]) {
			if (out_type) *out_type = port_type;
			if (out_num) *out_num = id;
			return 0;
		}

		id -= ports[port_type].num[direction];
	}

	if (out_type) *out_type = -1;
	if (out_num) *out_num = -1;

	return -EINVAL;
}

static int scarlett2_get_sw_port_num(const struct scarlett2_sw_port_mapping *mapping, int direction, int type, int num)
{
	int base;
	if (!mapping)
		return -1;

	for (base = 0 ; mapping->direction >= 0; ++mapping) {
		if (direction != mapping->direction)
			continue;
		if (type == mapping->type) {
			num -= mapping->index;
			return (num >= 0) && (num < mapping->count) ? base + num : -1;
		}
		base += mapping->count;
	}

	return -1;
}

static int scarlett2_sw2drv_port_num(const struct scarlett2_ports *ports, const struct scarlett2_sw_port_mapping *mapping, int direction, int num)
{
	int base;
	if (!mapping)
		return -1;

	if ((num--) < 0)
		return -1;
	if (!num)
		return 0;

	for (base = 0; mapping->direction >= 0; ++mapping) {
		if (direction != mapping->direction)
			continue;
		if (num < mapping->count)
			return base + num;

		num -= mapping->count;
		base += ports[mapping->type].num[direction];
	}

	return -1;
}

static int scarlett2_drv2sw_port_num(const struct scarlett2_ports *ports, const struct scarlett2_sw_port_mapping *mapping, int direction, int num)
{
	int port_type;
	if ((num < 0) || (!mapping))
		return -1;

	for (port_type = 0; port_type < SCARLETT2_PORT_TYPE_COUNT; ++port_type) {
		/* The number does match the range ? */
		if (num < ports[port_type].num[direction])
			return scarlett2_get_sw_port_num(mapping, direction, port_type, num);

		num -= ports[port_type].num[direction];
	}

	return -1;
}

static void scarlett2_fill_request_header(struct scarlett2_mixer_data *private,
					  struct scarlett2_usb_packet *req,
					  u32 cmd, u16 req_size)
{
	/* sequence must go up by 1 for each request */
	u16 seq = private->scarlett2_seq++;

	req->cmd = cpu_to_le32(cmd);
	req->size = cpu_to_le16(req_size);
	req->seq = cpu_to_le16(seq);
	req->error = 0;
	req->pad = 0;
}

static int scarlett2_usb_tx(struct usb_device *dev, int interface,
			    void *buf, u16 size)
{
	return snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev, 0),
			SCARLETT2_USB_CMD_REQ,
			USB_RECIP_INTERFACE | USB_TYPE_CLASS | USB_DIR_OUT,
			0, interface, buf, size);
}

static int scarlett2_usb_rx(struct usb_device *dev, int interface,
			    u32 usb_req, void *buf, u16 size)
{
	return snd_usb_ctl_msg(dev, usb_sndctrlpipe(dev, 0),
			usb_req,
			USB_RECIP_INTERFACE | USB_TYPE_CLASS | USB_DIR_IN,
			0, interface, buf, size);
}

static int scarlett2_usb(
	struct usb_mixer_interface *mixer, u32 cmd,
	void *req_data, u16 req_size, void *resp_data, u16 resp_size);

static int scarlett2_commit_software_config(
	struct usb_mixer_interface *mixer,
	void *ptr, /* the pointer of the first changed byte in the configuration */
	int bytes /* the actual number of bytes changed in the configuration */
);

/* Cargo cult proprietary initialisation sequence */
static int scarlett2_usb_init(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	struct usb_device *dev = chip->dev;
	struct scarlett2_mixer_data *private = mixer->private_data;
	u16 buf_size = sizeof(struct scarlett2_usb_packet) + 8;
	struct scarlett2_usb_packet *buf;
	int err;

	if (snd_usb_pipe_sanity_check(dev, usb_sndctrlpipe(dev, 0))) {
		return -EINVAL;
	}

	buf = kmalloc(buf_size, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		goto error;
	}

	// step 0
	err = scarlett2_usb_rx(dev, private->interface, SCARLETT2_USB_CMD_INIT,
			       buf, buf_size);
	if (err < 0)
		goto error;

	// step 1
	private->scarlett2_seq = 1;
	err = scarlett2_usb(mixer, SCARLETT2_USB_INIT_1, NULL, 0, NULL, 0);
	if (err < 0)
		goto error;

	// step 2
	private->scarlett2_seq = 1;
	err = scarlett2_usb(mixer, SCARLETT2_USB_INIT_2, NULL, 0, NULL, 84);
	if (err < 0)
		goto error;

	err = 0;

error:
	kfree(buf);
	return err;
}

/* Send a proprietary format request to the Scarlett interface */
static int scarlett2_usb(
	struct usb_mixer_interface *mixer, u32 cmd,
	void *req_data, u16 req_size, void *resp_data, u16 resp_size)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	struct usb_device *dev = mixer->chip->dev;
	u16 req_buf_size = sizeof(struct scarlett2_usb_packet) + req_size;
	u16 resp_buf_size = sizeof(struct scarlett2_usb_packet) + resp_size;
	struct scarlett2_usb_packet *req, *resp = NULL;
	int err;

	req = kmalloc(req_buf_size, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto error;
	}

	resp = kmalloc(resp_buf_size, GFP_KERNEL);
	if (!resp) {
		err = -ENOMEM;
		goto error;
	}

	mutex_lock(&private->usb_mutex);

	/* build request message and send it */
	scarlett2_fill_request_header(private, req, cmd, req_size);

	if (req_size)
		memcpy(req->data, req_data, req_size);

	err = scarlett2_usb_tx(dev, private->interface, req, req_buf_size);

	if (err != req_buf_size) {
		usb_audio_err(
			mixer->chip,
			"Scarlett Gen 2 USB request result cmd %x was %d\n",
			cmd, err);
		err = -EINVAL;
		goto unlock;
	}

	/* send a second message to get the response */
	err = scarlett2_usb_rx(dev, private->interface, SCARLETT2_USB_CMD_RESP,
			       resp, resp_buf_size);

	/* validate the response */
	if (err != resp_buf_size) {
		usb_audio_err(
			mixer->chip,
			"Scarlett Gen 2 USB response result cmd %x was %d expected %d\n",
			cmd, err, resp_buf_size);
		err = -EINVAL;
		goto unlock;
	}

	/* cmd/seq/size should match except when initialising
	 * seq sent = 1, response = 0
	 */
	if (resp->cmd != req->cmd ||
	    (resp->seq != req->seq && (req->seq != 1 || resp->seq != 0)) ||
	    resp_size != le16_to_cpu(resp->size) ||
	    resp->error ||
	    resp->pad) {
		usb_audio_err(
			mixer->chip,
			"Scarlett Gen 2 USB invalid response; "
			   "cmd tx/rx %d/%d seq %d/%d size %d/%d "
			   "error %d pad %d\n",
			le32_to_cpu(req->cmd), le32_to_cpu(resp->cmd),
			le16_to_cpu(req->seq), le16_to_cpu(resp->seq),
			resp_size, le16_to_cpu(resp->size),
			le32_to_cpu(resp->error),
			le32_to_cpu(resp->pad));
		err = -EINVAL;
		goto unlock;
	}

	if (resp_data && resp_size > 0)
		memcpy(resp_data, resp->data, resp_size);

unlock:
	mutex_unlock(&private->usb_mutex);
error:
	kfree(req);
	kfree(resp);
	return err;
}

/* Send SCARLETT2_USB_DATA_CMD SCARLETT2_USB_CONFIG_SAVE */
static void scarlett2_config_save(struct usb_mixer_interface *mixer)
{
	__le32 req = cpu_to_le32(SCARLETT2_USB_CONFIG_SAVE);

	scarlett2_usb(mixer, SCARLETT2_USB_DATA_CMD,
		      &req, sizeof(u32),
		      NULL, 0);
}

/* Delayed work to save config */
static void scarlett2_config_save_work(struct work_struct *work)
{
	struct scarlett2_mixer_data *private =
		container_of(work, struct scarlett2_mixer_data, work.work);

	scarlett2_config_save(private->mixer);
}

/* Send a USB message to set a configuration parameter (volume level,
 * sw/hw volume switch, line/inst level switch, pad, or air switch)
 */
static int scarlett2_usb_set_config(
	struct usb_mixer_interface *mixer,
	int config_item_num, int index, int value)
{
	struct {
		__le32 offset;
		__le32 bytes;
		__le32 value;
	} __packed req;
	__le32 req2;
	int err;
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_config *config_item = &info->config[config_item_num];

	if (config_item->size <= 0) {
		usb_audio_warn(mixer->chip, "There is no existing config item %d\n", config_item_num);
		return -EINVAL;
	}

	/* Cancel any pending NVRAM save */
	cancel_delayed_work_sync(&private->work);

	/* Send the configuration parameter data */
	req.offset = cpu_to_le32(config_item->offset + index * config_item->size);
	req.bytes  = cpu_to_le32(config_item->size);
	req.value  = cpu_to_le32(value);

	err = scarlett2_usb(mixer, SCARLETT2_USB_SET_DATA,
			    &req, sizeof(u32) * 2 + config_item->size,
			    NULL, 0);
	if (err < 0)
		return err;

	/* Activate the change */
	if (config_item->activate > 0) {
		req2 = cpu_to_le32(config_item->activate);
		err = scarlett2_usb(mixer, SCARLETT2_USB_DATA_CMD,
			    &req2, sizeof(req2), NULL, 0);
		if (err < 0)
			return err;
	}

	/* Schedule the change to be written to NVRAM */
	schedule_delayed_work(&private->work, msecs_to_jiffies(2000));

	return 0;
}

/* Send a set of USB messages to get configuration data; result placed in *data */
static int scarlett2_usb_get(
	struct usb_mixer_interface *mixer,
	int offset, void *data, int bytes)
{
	struct {
		__le32 offset;
		__le32 size;
	} __packed req;

	int i, chunk, err;
	u8 *buf = (u8 *)data;

	/* Request the config space with fixed-size data chunks */
	for (i=0; i<bytes; i += chunk) {
		/* Compute the chunk size */
		chunk = (bytes - i);
		if (chunk > SCARLETT2_SW_CONFIG_PACKET_SIZE)
			chunk = SCARLETT2_SW_CONFIG_PACKET_SIZE;

		/* Request yet another chunk */
		req.offset = cpu_to_le32(offset + i);
		req.size   = cpu_to_le32(chunk);

		err = scarlett2_usb(mixer, SCARLETT2_USB_GET_DATA, &req, sizeof(req), &buf[i], chunk);
		if (err < 0)
			return err;
	}

	return 0;
}

/* Send a set of USB messages to set configuration data */
static int scarlett2_usb_set(
	struct usb_mixer_interface *mixer,
	int offset, const void *data, int bytes)
{
	struct {
		__le32 offset;
		__le32 size;
		u8 data[SCARLETT2_SW_CONFIG_PACKET_SIZE];
	} __packed req;
	int i, chunk, err = 0;
	const u8 *buf = (const u8 *)data;

	/* Transfer the configuration with fixed-size data chunks */
	for (i=0; i<bytes; i += chunk) {
		/* Compute the chunk size */
		chunk = (bytes - i);
		if (chunk > SCARLETT2_SW_CONFIG_PACKET_SIZE)
			chunk = SCARLETT2_SW_CONFIG_PACKET_SIZE;

		/* Send yet another chunk of data */
		req.offset = cpu_to_le32(offset + i);
		req.size   = cpu_to_le32(chunk);
		memcpy(req.data, &buf[i], chunk);

		err = scarlett2_usb(mixer, SCARLETT2_USB_SET_DATA, &req, chunk + sizeof(__le32)*2, NULL, 0);
		if (err < 0)
			return err;
	}

	return err;
}

/* Send a USB message to get configuration parameters; result placed in *buf */
static int scarlett2_usb_get_config(
	struct usb_mixer_interface *mixer,
	int config_item_num, int count, void *buf)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_config *config_item = &info->config[config_item_num];

	/* No such configuration entry? */
	if (config_item->size <= 0) {
		usb_audio_warn(mixer->chip, "Configuration item #%d was not found\n", config_item_num);
		return -EINVAL;
	}

	return scarlett2_usb_get(mixer, config_item->offset, buf, config_item->size * count);
}

/* Send a USB message to get volume status; result placed in *buf */
static int scarlett2_usb_get_volume_status(
	struct usb_mixer_interface *mixer,
	struct scarlett2_usb_volume_status *buf)
{
	return scarlett2_usb_get(mixer, 0, buf, sizeof(*buf));
}

/* Send a USB message to set the volumes for all inputs of one mix
 * (values obtained from private->mix[])
 */
static int scarlett2_usb_set_mix(struct usb_mixer_interface *mixer,
				     int mix_num)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;

	struct {
		__le16 mix_num;
		__le16 data[SCARLETT2_INPUT_MIX_MAX];
	} __packed req;

	int i, j;
	int num_mixer_in = info->ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_OUT];
	int volume;

	req.mix_num = cpu_to_le16(mix_num);

	for (i = 0, j = mix_num * num_mixer_in; i < num_mixer_in; i++, j++) {
		volume = (private->mix_mutes[j]) ? 0 : private->mix[j]; /* Apply mute control */
		req.data[i] = cpu_to_le16(scarlett2_mixer_values[volume]);
	}

	return scarlett2_usb(mixer, SCARLETT2_USB_SET_MIX,
			     &req, (num_mixer_in + 1) * sizeof(u16),
			     NULL, 0);
}

/* Send USB messages to get mux inputs */
static int scarlett2_usb_get_mux(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_ports *ports = info->ports;
	int mux_size, i, src_port, dst_port, err;
	u32 mux_id;
	struct {
		__le16 num;
		__le16 count;
	} __packed req;

	__le32 data[SCARLETT2_MUX_MAX];

	mux_size  = info->mux_size[SCARLETT2_PORT_OUT];

	/* Send the request */
	req.num   = 0;
	req.count = cpu_to_le16(mux_size);
	err = scarlett2_usb(mixer, SCARLETT2_USB_GET_MUX, &req, 2 * sizeof(__le16), data, sizeof(__le32) * mux_size);
	if (err < 0)
		return err;

	/* Clear mux state */
	for (i=0 ; i < SCARLETT2_MUX_MAX; ++i)
		private->mux[i] = 0;

	for (i=0; i<mux_size; ++i) {
		/* Decode input and output port indexes, remember mux state for the port */
		mux_id = le32_to_cpu(data[i]);
		src_port = scarlett2_mux_to_id(ports, SCARLETT2_PORT_IN, mux_id >> 12);
		dst_port = scarlett2_mux_to_id(ports, SCARLETT2_PORT_OUT, mux_id);
		if ((dst_port >= 0) && (dst_port < SCARLETT2_MUX_MAX))
			private->mux[dst_port] = src_port;
	}

	return err;
}

/* Send USB messages to set mux inputs */
static int scarlett2_usb_set_mux(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_ports *ports = info->ports;
	int direction, conn_id, order, port, port_idx, err = 0;
	int mute_idx;
	u32 src_mux, dst_mux;

	static const int assignment_order[] = {
		SCARLETT2_PORT_TYPE_PCM,
		SCARLETT2_PORT_TYPE_ANALOGUE,
		SCARLETT2_PORT_TYPE_SPDIF,
		SCARLETT2_PORT_TYPE_ADAT,
		SCARLETT2_PORT_TYPE_MIX,
		SCARLETT2_PORT_TYPE_TALKBACK
	};

	struct {
		__le16 pad;
		__le16 num;
		__le32 data[SCARLETT2_MUX_MAX];
	} __packed req;

	/* mux settings for each rate */
	for (direction = SCARLETT2_PORT_OUT_44; direction <= SCARLETT2_PORT_OUT_176; ++direction) {
		/* init request */
		req.pad = 0;
		req.num = cpu_to_le16(direction - SCARLETT2_PORT_OUT_44);

		/* form the request data */
		conn_id = 0;
		for (order = 0; order < sizeof(assignment_order)/sizeof(int); ++order) {
			int port_type = assignment_order[order];

			for (port = 0; port < ports[port_type].num[direction]; ++port) {
				port_idx = scarlett2_get_port_num(ports, SCARLETT2_PORT_OUT, port_type, port);

				/* Lower 12 bits for destination, next 12 bits for source */
				mute_idx= scarlett2_output_index(mixer, port_type, port);
				src_mux = ((mute_idx >= 0) && (private->mutes[mute_idx])) ? 0 :
				          scarlett2_id_to_mux(ports, SCARLETT2_PORT_IN, private->mux[port_idx]);
				dst_mux = scarlett2_id_to_mux(ports, SCARLETT2_PORT_OUT, port_idx);

				req.data[conn_id++] = cpu_to_le32((src_mux << 12) | dst_mux);
			}
		}

		/* Fill rest mux data with zeros */
		for ( ; conn_id < info->mux_size[direction]; ++conn_id)
			req.data[conn_id] = 0;

		/* Send the SET_MUX notification */
		err = scarlett2_usb(mixer, SCARLETT2_USB_SET_MUX,
				    &req, 2 * sizeof(__le16) + conn_id * sizeof(__le32),
				    NULL, 0);
		if (err < 0)
			return err;
	}

	return err;
}

/* Send USB message to get meter levels */
static int scarlett2_usb_get_meter_levels(struct usb_mixer_interface *mixer,
					  u16 *levels)
{
	struct {
		__le16 pad;
		__le16 num_meters;
		__le32 magic;
	} __packed req;
	u32 resp[SCARLETT2_NUM_METERS];
	int i, err;

	req.pad = 0;
	req.num_meters = cpu_to_le16(SCARLETT2_NUM_METERS);
	req.magic = cpu_to_le32(SCARLETT2_USB_METER_LEVELS_GET_MAGIC);
	err = scarlett2_usb(mixer, SCARLETT2_USB_GET_METER_LEVELS,
			    &req, sizeof(req), resp, sizeof(resp));
	if (err < 0)
		return err;

	/* copy, convert to u16 */
	for (i = 0; i < SCARLETT2_NUM_METERS; i++)
		levels[i] = resp[i];

	return 0;
}

/*** Control Functions ***/
static int scarlett2_bool_enum_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *uinfo)
{
	static const char *const values[2] = {
		"Off", "On"
	};

	return snd_ctl_enum_info(uinfo, 1, 2, values);
}

/* helper function to create a new control */
static int scarlett2_add_new_ctl(struct usb_mixer_interface *mixer,
				 const struct snd_kcontrol_new *ncontrol,
				 int index, int channels, const char *name,
				 struct snd_kcontrol **kctl_return)
{
	struct snd_kcontrol *kctl;
	struct usb_mixer_elem_info *elem;
	int err;

	elem = kzalloc(sizeof(*elem), GFP_KERNEL);
	if (!elem)
		return -ENOMEM;

	elem->head.mixer = mixer;
	elem->control = index;
	elem->head.id = index;
	elem->channels = channels;

	kctl = snd_ctl_new1(ncontrol, elem);
	if (!kctl) {
		kfree(elem);
		return -ENOMEM;
	}
	kctl->private_free = snd_usb_mixer_elem_free;

	strlcpy(kctl->id.name, name, sizeof(kctl->id.name));

	err = snd_usb_mixer_add_control(&elem->head, kctl);
	if (err < 0)
		return err;

	if (kctl_return)
		*kctl_return = kctl;

	return 0;
}

/*** Analogue Line Out Volume Controls ***/

/* Update hardware volume controls after receiving notification that
 * they have changed
 */
static int scarlett2_update_volumes(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	struct scarlett2_usb_volume_status volume_status;

	int num_line_out  = info->ports[SCARLETT2_PORT_TYPE_ANALOGUE].num[SCARLETT2_PORT_OUT];
	int err, i;
	s16 volume;

	private->vol_updated = 0;

	/* Obtain actual volume status */
	err = scarlett2_usb_get_volume_status(mixer, &volume_status);
	if (err < 0)
		return err;

	private->master_vol = clamp(
		volume_status.master_vol + SCARLETT2_VOLUME_BIAS,
		0, SCARLETT2_VOLUME_BIAS);

	/** Update volume settings for each analogue output */
	for (i = 0; i < num_line_out; i++) {
		/* Update software/hardware switch status */
		private->vol_sw_hw_switch[i] = info->line_out_hw_vol && volume_status.sw_hw_switch[i];

		/* If volume is software-controlled, try to read it's value from software configuration */
		if (private->vol_sw_hw_switch[i]) {
			/* Reset to master volume */
			private->vol[i] = private->master_vol;
		}
		else if (private->sw_cfg) {
			/* Read volume from software config */
			volume = le16_to_cpu(private->sw_cfg->volume[i].volume);
			private->vol[i] = clamp(volume + SCARLETT2_VOLUME_BIAS, 0, SCARLETT2_VOLUME_BIAS);
		}
		else {
			/* Read volume from device volume status */
			volume = le16_to_cpu(volume_status.sw_vol[i]);
			private->vol[i] = clamp(volume + SCARLETT2_VOLUME_BIAS, 0, SCARLETT2_VOLUME_BIAS);
		}
	}

	/* Update Mute/Dim hardware buttons */
	for (i = 0; i < private->info->button_count; i++)
		private->buttons[i] = !!volume_status.buttons[i];

	return 0;
}

static int scarlett2_volume_ctl_info(struct snd_kcontrol *kctl,
				     struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = elem->channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = SCARLETT2_VOLUME_BIAS;
	uinfo->value.integer.step = 1;
	return 0;
}

static int scarlett2_master_volume_ctl_get(struct snd_kcontrol *kctl,
					   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->vol_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_volumes(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.integer.value[0] = private->master_vol;
	return 0;
}

static int scarlett2_volume_ctl_get(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	int index = elem->control;

	if (private->vol_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_volumes(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.integer.value[0] = private->vol[index];
	return 0;
}

static int scarlett2_volume_ctl_put(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	struct scarlett2_sw_cfg_volume *sw_vol;
	int index = elem->control;
	int oval, val, err = 0;
	u16 volume;

	mutex_lock(&private->data_mutex);

	oval = private->vol[index];
	val = ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->vol[index] = val;

	/* Update volume for the output */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_LINE_OUT_VOLUME,
				       index, val - SCARLETT2_VOLUME_BIAS);
	if (err != 0)
		goto unlock;

	/* Update software configuration if possible */
	if ((private->sw_cfg) &&
	    (!private->vol_sw_hw_switch[index])) {
		volume = val - SCARLETT2_VOLUME_BIAS;
		sw_vol = &private->sw_cfg->volume[index];
		sw_vol->volume  = cpu_to_le16(volume);
		sw_vol->changed = 1;

		err = scarlett2_commit_software_config(mixer, sw_vol, sizeof(struct scarlett2_sw_cfg_volume));
		if (err < 0)
			goto unlock;
	}
	if (err == 0)
		err = 1;

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const DECLARE_TLV_DB_MINMAX(
	db_scale_scarlett2_gain, -SCARLETT2_VOLUME_BIAS * 100, 0
);

static const struct snd_kcontrol_new scarlett2_master_volume_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READ |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.name = "",
	.info = scarlett2_volume_ctl_info,
	.get  = scarlett2_master_volume_ctl_get,
	.private_value = 0, /* max value */
	.tlv = { .p = db_scale_scarlett2_gain }
};

static const struct snd_kcontrol_new scarlett2_line_out_volume_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.name = "",
	.info = scarlett2_volume_ctl_info,
	.get  = scarlett2_volume_ctl_get,
	.put  = scarlett2_volume_ctl_put,
	.private_value = 0, /* max value */
	.tlv = { .p = db_scale_scarlett2_gain }
};

/*** HW/SW Volume Switch Controls ***/

static int scarlett2_sw_hw_enum_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *uinfo)
{
	static const char *const values[2] = {
		"SW", "HW"
	};

	return snd_ctl_enum_info(uinfo, 1, 2, values);
}

static int scarlett2_sw_hw_enum_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] =
		private->vol_sw_hw_switch[elem->control];
	return 0;
}

static int scarlett2_sw_hw_enum_ctl_put(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;
	s16 volume;

	mutex_lock(&private->data_mutex);

	oval = private->vol_sw_hw_switch[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->vol_sw_hw_switch[index] = val;
	private->vol_updated = 1; /* Trigger volumes to be updated */

	/* Change access mode to RO (hardware controlled volume)
	 * or RW (software controlled volume)
	 */
	if (val) {
		private->vol_ctls[index]->vd[0].access &=
			~SNDRV_CTL_ELEM_ACCESS_WRITE;

		/* Set volume to current HW volume */
		err = scarlett2_usb_set_config(
			mixer, SCARLETT2_CONFIG_LINE_OUT_VOLUME,
			index, private->master_vol - SCARLETT2_VOLUME_BIAS);
		if (err < 0)
			goto unlock;
	}
	else {
		private->vol_ctls[index]->vd[0].access |=
			SNDRV_CTL_ELEM_ACCESS_WRITE;

		if (private->sw_cfg) {
			/* Reset volume to software config */
			volume = le16_to_cpu(private->sw_cfg->volume[index].volume);
			private->vol[index] = clamp(volume + SCARLETT2_VOLUME_BIAS, 0, SCARLETT2_VOLUME_BIAS);
		}

		/* Set volume to current SW volume */
		err = scarlett2_usb_set_config(
			mixer, SCARLETT2_CONFIG_LINE_OUT_VOLUME,
			index, private->vol[index] - SCARLETT2_VOLUME_BIAS);
		if (err < 0)
			goto unlock;
	}

	/* Notify of RO/RW change */
	snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_INFO | SNDRV_CTL_EVENT_MASK_VALUE,
		       &private->vol_ctls[index]->id);

	/* Send SW/HW switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_SW_HW_SWITCH,
				       index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_sw_hw_enum_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_sw_hw_enum_ctl_info,
	.get  = scarlett2_sw_hw_enum_ctl_get,
	.put  = scarlett2_sw_hw_enum_ctl_put,
};

/*** Gain Halos Switch Controls ***/
static int scarlett2_ghalo_color_enum_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *uinfo)
{
	static const char *const values[8] = {
		"Off",
		"Red",
		"Green",
		"Amber",
		"Blue",
		"Pink",
		"Light Blue",
		"Light Pink"
	};

	return snd_ctl_enum_info(uinfo, 1, 8, values);
}

static int scarlett2_ghalo_custom_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->ghalo_custom;

	return 0;
}

static int scarlett2_ghalo_level_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->ghalo_levels[elem->control];
	return 0;
}

static int scarlett2_ghalo_led_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->ghalo_leds[elem->control];
	return 0;
}

static int scarlett2_ghalo_custom_ctl_put(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int oval, val, err = 0;
	int command;

	mutex_lock(&private->data_mutex);

	oval = private->ghalo_custom;
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->ghalo_custom = val;
	command = (val) ? 0x02 : 0;

	/* Set gain halo control */
	err = scarlett2_usb_set_config(
		mixer, SCARLETT2_CONFIG_GAIN_HALO_ENABLE,
		0, command);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static int scarlett2_ghalo_level_ctl_put(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->ghalo_levels[index];
	val = ucontrol->value.integer.value[0];
	val = clamp(val, 0, 7);

	if (oval == val)
		goto unlock;

	private->ghalo_levels[index] = val;

	/* Set gain halo control */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_GAIN_HALO_LEVELS, index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static int scarlett2_ghalo_led_ctl_put(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->ghalo_leds[index];
	val = ucontrol->value.integer.value[0];
	val = clamp(val, 0, 7);

	if (oval == val)
		goto unlock;

	private->ghalo_leds[index] = val;

	/* Set gain halo control */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_GAIN_HALO_LEDS, index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_ghalo_custom_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_ghalo_custom_ctl_get,
	.put  = scarlett2_ghalo_custom_ctl_put,
};

static const struct snd_kcontrol_new scarlett2_ghalo_level_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_ghalo_color_enum_ctl_info,
	.get  = scarlett2_ghalo_level_ctl_get,
	.put  = scarlett2_ghalo_level_ctl_put,
};

static const struct snd_kcontrol_new scarlett2_ghalo_led_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_ghalo_color_enum_ctl_info,
	.get  = scarlett2_ghalo_led_ctl_get,
	.put  = scarlett2_ghalo_led_ctl_put,
};

/*** Line Level/Instrument Level Switch Controls ***/
static int scarlett2_update_line_ctl_switches(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	u8 pad_switches[SCARLETT2_PAD_SWITCH_MAX];
	u8 level_switches[SCARLETT2_LEVEL_SWITCH_MAX];
	u8 pow_switch, retain48v;

	int i, err = 0;

	private->line_ctl_updated = 0;

	/* Update PAD settings */
	if (info->pad_input_count) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_PAD_SWITCH,
			info->pad_input_count,
			pad_switches);
		if (err < 0)
			return err;

		for (i = 0; i < info->pad_input_count; i++)
			private->pad_switch[i] = !!pad_switches[i];
	}

	/* Update LINE/INST settings */
	if (info->level_input_count) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_LEVEL_SWITCH,
			info->level_input_count,
			level_switches);
		if (err < 0)
			return err;

		for (i = 0; i < info->level_input_count; i++)
			private->level_switch[i] = !!level_switches[i];
	}

	/* Update phantom power settings */
	if (info->power_48v_count) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_48V_SWITCH,
			1, &pow_switch);
		if (err < 0)
			return err;

		for (i = 0; i < info->power_48v_count; i++)
			private->pow_switch[i] = !! (pow_switch & (1 << i));
	}

	/* 'Retain 48V switch */
	if (info->has_retain48v) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_RETAIN_48V,
			1, &retain48v);
		if (err < 0)
			return err;

		private->retain48v_switch = !! retain48v;
	}

	return 0;
}

static int scarlett2_level_enum_ctl_info(struct snd_kcontrol *kctl,
					 struct snd_ctl_elem_info *uinfo)
{
	static const char *const values[2] = {
		"Line", "Inst"
	};

	return snd_ctl_enum_info(uinfo, 1, 2, values);
}

static int scarlett2_level_enum_ctl_get(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->line_ctl_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_line_ctl_switches(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] =
		private->level_switch[elem->control];
	return 0;
}

static int scarlett2_level_enum_ctl_put(struct snd_kcontrol *kctl,
					struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->level_switch[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->level_switch[index] = val;

	/* Send inst change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_LEVEL_SWITCH,
				       index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_level_enum_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_level_enum_ctl_info,
	.get  = scarlett2_level_enum_ctl_get,
	.put  = scarlett2_level_enum_ctl_put,
};

/*** Pad Switch Controls ***/
static int scarlett2_pad_ctl_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->line_ctl_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_line_ctl_switches(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] =
		private->pad_switch[elem->control];
	return 0;
}

static int scarlett2_pad_ctl_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->pad_switch[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->pad_switch[index] = val;

	/* Send pad change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_PAD_SWITCH,
				       index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_pad_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_pad_ctl_get,
	.put  = scarlett2_pad_ctl_put,
};

/*** Air Switch Controls ***/

static int scarlett2_air_ctl_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] =
		private->air_switch[elem->control];
	return 0;
}

static int scarlett2_air_ctl_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->air_switch[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->air_switch[index] = val;

	/* Send switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_AIR_SWITCH,
				       index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_air_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_air_ctl_get,
	.put  = scarlett2_air_ctl_put,
};

/*** 48V Switch Controls ***/
static int scarlett2_48v_ctl_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->line_ctl_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_line_ctl_switches(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] =
		private->pow_switch[elem->control];
	return 0;
}

static int scarlett2_48v_ctl_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;

	int index = elem->control;
	int i, oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->pow_switch[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->pow_switch[index] = val;
	val = 0;
	for (i = 0; i < info->power_48v_count; ++i)
		val |= (private->pow_switch[i] << i);

	/* Send switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_48V_SWITCH, 0, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_48v_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_48v_ctl_get,
	.put  = scarlett2_48v_ctl_put
};

/* Retain 48V Switch Control */
static int scarlett2_retain48v_ctl_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->retain48v_switch;

	return 0;
}


static int scarlett2_retain48v_ctl_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);
	oval = private->retain48v_switch;
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->retain48v_switch = val;

	/* Send switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_RETAIN_48V, 0, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_retain48v_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_retain48v_ctl_get,
	.put  = scarlett2_retain48v_ctl_put
};

/*** Mute/Dim Controls ***/

static int scarlett2_button_ctl_get(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->vol_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_volumes(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] = private->buttons[elem->control];
	return 0;
}

static int scarlett2_button_ctl_put(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->buttons[index];
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->buttons[index] = val;

	/* Send switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_BUTTONS,
				       index, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_button_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_button_ctl_get,
	.put  = scarlett2_button_ctl_put
};

/*** Mute for each output ***/
static int scarlett2_mute_ctl_get(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->vol_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_volumes(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] = ! private->mutes[elem->control];
	return 0;
}

static int scarlett2_mute_ctl_put(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;

	int analog_outs = info->ports[SCARLETT2_PORT_TYPE_ANALOGUE].num[SCARLETT2_PORT_OUT];
	int index = elem->control;
	int oval, val, err = 0;
	u32 mutes;

	mutex_lock(&private->data_mutex);

	oval = private->mutes[index];
	val = !ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->mutes[index] = val;

	/* Update mute controls */
	if ((info->has_hw_volume) && (index < analog_outs)) {
		/* Update state of hardware configuration */
		err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_MUTES,
				       index, val);
	}
	else if ((info->has_mux) && (private->sw_cfg != NULL)) {
		/* Set mute bit for the corresponding output */
		mutes = le32_to_cpu(private->sw_cfg->mute_sw);
		index = 1 << index;
		mutes = (val) ? (mutes | index) : (mutes & (~index));
		private->sw_cfg->mute_sw = cpu_to_le32(mutes);

		/* Update software configuration */
		err = scarlett2_commit_software_config(mixer, &private->sw_cfg->mute_sw, sizeof(__le32));
	}
	else
		err = -EINVAL;

	if (err < 0)
		goto unlock;

	/* Update MUX settings as it does the original software */
	err = scarlett2_usb_set_mux(mixer);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_mute_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = snd_ctl_boolean_mono_info,
	.get  = scarlett2_mute_ctl_get,
	.put  = scarlett2_mute_ctl_put
};

static int scarlett2_add_mute_ctls(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	u8 hw_mutes[SCARLETT2_ANALOGUE_OUT_MAX];
	u32 sw_mutes;

	int num_line_out  = info->ports[SCARLETT2_PORT_TYPE_ANALOGUE].num[SCARLETT2_PORT_OUT];
	int num_spdif_out = info->ports[SCARLETT2_PORT_TYPE_SPDIF].num[SCARLETT2_PORT_OUT];
	int num_adat_out  = info->ports[SCARLETT2_PORT_TYPE_ADAT].num[SCARLETT2_PORT_OUT];
	int err, i, port, index = 0;

	/* Add mutes for line outputs */
	if (info->has_hw_volume) {
		/* Read hardware state of mutes */
		err = scarlett2_usb_get_config(mixer, SCARLETT2_CONFIG_MUTES, num_line_out, hw_mutes);
		if (err < 0)
			return err;

		for (i=0; i<num_line_out; ++i, ++index) {
			/* Read hardware mute settings */
			private->mutes[index] = !! hw_mutes[i];

			/* Format the mute switch name */
			port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, i);
			scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Mute", info, SCARLETT2_PORT_OUT, port);

			/* Add port to list */
			err = scarlett2_add_new_ctl(mixer,
					    &scarlett2_mute_ctl,
					    index, 1, s, &private->mute_ctls[index]);
			if (err < 0)
				return err;
		}
	}

	/* Software mutes */
	if (info->has_mux && private->sw_cfg) {
		/* Read state of mutes from software config */
		sw_mutes = le32_to_cpu(private->sw_cfg->mute_sw);

		/* Add mutes for S/PDIF outputs */
		for (i=0; i<num_spdif_out; ++i, ++index) {
			/* Read software mute settings */
			private->mutes[index] = !! (sw_mutes & (1 << index));

			/* Format the mute switch name */
			port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_SPDIF, i);
			scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Mute", info, SCARLETT2_PORT_OUT, port);

			/* Add port to list */
			err = scarlett2_add_new_ctl(mixer,
					    &scarlett2_mute_ctl,
					    index, 1, s, &private->mute_ctls[index]);
			if (err < 0)
				return err;
		}

		/* Add mutes for ADAT outputs */
		for (i=0; i<num_adat_out; ++i, ++index) {
			/* Read software mute settings */
			private->mutes[index] = !! (sw_mutes & (1 << index));

			/* Format the mute switch name */
			port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ADAT, i);
			scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Mute", info, SCARLETT2_PORT_OUT, port);

			/* Add port to list */
			err = scarlett2_add_new_ctl(mixer,
						    &scarlett2_mute_ctl,
						    index, 1, s, &private->mute_ctls[index]);
			if (err < 0)
				return err;
		}
	}

	return 0;
}

/*** Create the analogue output controls ***/
static int scarlett2_add_line_out_ctls(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_ports *ports = info->ports;
	int num_line_out = ports[SCARLETT2_PORT_TYPE_ANALOGUE].num[SCARLETT2_PORT_OUT];
	int err, i, port;
	s16 level;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

	/* Add R/O HW volume control */
	if (info->line_out_hw_vol) {
		snprintf(s, sizeof(s), "Master Playback Volume");
		err = scarlett2_add_new_ctl(mixer,
					    &scarlett2_master_volume_ctl,
					    0, 1, s, &private->master_vol_ctl);
		if (err < 0)
			return err;
	}

	/* Add volume controls */
	for (i = 0; i < num_line_out; i++) {
		/* Volume Fader */
		port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, i);
		scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Volume", info, SCARLETT2_PORT_OUT, port);
		err = scarlett2_add_new_ctl(mixer,
					    &scarlett2_line_out_volume_ctl,
					    i, 1, s, &private->vol_ctls[i]);
		if (err < 0)
			return err;

		/* Initialize the value with software configuration
		 * Make the fader read-only if the SW/HW switch is set to HW
		 */
		if ((private->sw_cfg) && (!private->vol_sw_hw_switch[i])) {
			level  = le16_to_cpu(private->sw_cfg->volume[i].volume);
			private->vol[i] = clamp(level + SCARLETT2_VOLUME_BIAS, 0, SCARLETT2_VOLUME_BIAS);
			private->vol_ctls[i]->vd[0].access |= SNDRV_CTL_ELEM_ACCESS_WRITE;
		}
		else {
			private->vol[i] = private->master_vol;
			private->vol_ctls[i]->vd[0].access &= ~SNDRV_CTL_ELEM_ACCESS_WRITE;
		}

		/* SW/HW Switch */
		if (info->line_out_hw_vol) {
			scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Control", info, SCARLETT2_PORT_OUT, port);
			err = scarlett2_add_new_ctl(mixer,
						    &scarlett2_sw_hw_enum_ctl,
						    i, 1, s, NULL);
			if (err < 0)
				return err;
		}
	}

	/* Add HW button controls */
	for (i = 0; i < private->info->button_count; i++) {
		err = scarlett2_add_new_ctl(mixer, &scarlett2_button_ctl,
					    i, 1, scarlett2_button_names[i],
					    &private->button_ctls[i]);
		if (err < 0)
			return err;
	}

	/* Commit actual volumes */
	if (private->sw_cfg) {
		for (i = 0; i < num_line_out; i++) {
			err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_LINE_OUT_VOLUME,
			                               i, private->vol[i] - SCARLETT2_VOLUME_BIAS);
			if (err < 0)
				return err;
		}
	}

	return err;
}

/*** Create the analogue input controls ***/
static int scarlett2_add_line_in_ctls(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	int err, i, port;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

	/* Add input level (line/inst) controls */
	for (i = 0; i < info->level_input_count; i++) {
		port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, i);
		scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Mode Switch", info, SCARLETT2_PORT_IN, port);
		err = scarlett2_add_new_ctl(mixer, &scarlett2_level_enum_ctl,
					    i, 1, s, &private->level_ctls[i]);
		if (err < 0)
			return err;
	}

	/* Add input pad controls */
	for (i = 0; i < info->pad_input_count; i++) {
		port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, i);
		scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Pad Switch", info, SCARLETT2_PORT_IN, port);
		err = scarlett2_add_new_ctl(mixer, &scarlett2_pad_ctl,
					    i, 1, s, &private->pad_ctls[i]);
		if (err < 0)
			return err;
	}

	/* Add input air controls */
	for (i = 0; i < info->air_input_count; i++) {
		port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_ANALOGUE, i);
		scarlett2_fmt_port_name(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Air Switch", info, SCARLETT2_PORT_IN, port);
		err = scarlett2_add_new_ctl(mixer, &scarlett2_air_ctl,
					    i, 1, s, NULL);
		if (err < 0)
			return err;
	}

	/* Add input 48v controls */
	for (i = 0; i < info->power_48v_count; i++) {
		snprintf(s, sizeof(s), "Analogue In 48V Switch %d", i + 1);
		err = scarlett2_add_new_ctl(mixer, &scarlett2_48v_ctl,
					    i, 1, s, &private->pow_ctls[i]);
		if (err < 0)
			return err;
	}

	/* Add 'Retain 48v' control */
	if (info->has_retain48v) {
		err = scarlett2_add_new_ctl(mixer, &scarlett2_retain48v_ctl,
					    0, 1, "Analogue In 48V Retain", NULL);
		if (err < 0)
			return err;
	}

	return 0;
}

/*** Create the Gain Halo controls ***/
static int scarlett2_add_ghalo_ctls(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	int err, i, val;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	u8 ghalo_flag, ghalo_leds[SCARLETT2_GAIN_HALO_LEDS_MAX], ghalo_levels[SCARLETT2_GAIN_HALO_LEVELS];
	static const char * const level_names[SCARLETT2_GAIN_HALO_LEVELS] = {
		"LED Clip Color",
		"LED Pre-Clip Color",
		"LED Good Color"
	};

	/* Check that device supports gain halos */
	if (info->gain_halos_count <= 0)
		return 0;

	/* Read custom color settings and add control */
	err = scarlett2_usb_get_config(mixer, SCARLETT2_CONFIG_GAIN_HALO_ENABLE, 1, &ghalo_flag);
	if (err < 0)
		return err;

	private->ghalo_custom = (ghalo_flag == 0x02);

	err = scarlett2_add_new_ctl(mixer, &scarlett2_ghalo_custom_ctl, 0, 1, "LED Custom Colors", NULL);
	if (err < 0)
		return err;

	/* Read state of level colors and add controls */
	err = scarlett2_usb_get_config(mixer, SCARLETT2_CONFIG_GAIN_HALO_LEVELS, SCARLETT2_GAIN_HALO_LEVELS, ghalo_levels);
	if (err < 0)
		return err;

	for (i = 0; i < SCARLETT2_GAIN_HALO_LEVELS; i++) {
		val = ghalo_levels[i];
		private->ghalo_levels[i] = clamp(val, 0, 7);

		err = scarlett2_add_new_ctl(mixer, &scarlett2_ghalo_level_ctl, i, 1, level_names[i], NULL);
		if (err < 0)
			return err;
	}

	/* Read state of custom colors and add controls */
	err = scarlett2_usb_get_config(mixer, SCARLETT2_CONFIG_GAIN_HALO_LEDS, info->gain_halos_count, ghalo_leds);
	if (err < 0)
		return err;

	for (i = 0; i<info->gain_halos_count; ++i) {
		val = ghalo_leds[i];
		private->ghalo_leds[i] = clamp(val, 0, 7);

		snprintf(s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "LED %d Custom Color", i);
		err = scarlett2_add_new_ctl(mixer, &scarlett2_ghalo_led_ctl, i, 1, s, NULL);
		if (err < 0)
			return err;
	}

	return 0;
}

/*** Mixer Volume Controls ***/
static int scarlett2_mixer_ctl_info(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = elem->channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = SCARLETT2_MIXER_MAX_VALUE;
	uinfo->value.integer.step = 1;
	return 0;
}

static int scarlett2_mixer_ctl_get(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.integer.value[0] = private->mix[elem->control];
	return 0;
}

static int scarlett2_mixer_ctl_put(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	int oval, val, mix_num, input_num, err = 0;
	int index = elem->control;
	u32 level;
	__le32 *gain;

	mutex_lock(&private->data_mutex);

	oval      = private->mix[index];
	val       = ucontrol->value.integer.value[0];
	mix_num   = index / SCARLETT2_INPUT_MIX_MAX;
	input_num = index % SCARLETT2_INPUT_MIX_MAX;

	if (oval == val)
		goto unlock;

	private->mix[index] = val;
	err = scarlett2_usb_set_mix(mixer, mix_num);
	if (err < 0)
		goto unlock;

	/* Update software configuration data */
	level = scarlett2_sw_config_mixer_values[val];
	gain  = &private->sw_cfg->mixer[mix_num][input_num];
	*gain = cpu_to_le32(level << 16); /* Convert to F32LE */

	scarlett2_commit_software_config(mixer, gain, sizeof(__le32));

	if (err == 0)
		err = 1;

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const DECLARE_TLV_DB_MINMAX(
	db_scale_scarlett2_mixer,
	SCARLETT2_MIXER_MIN_DB * 100,
	SCARLETT2_MIXER_MAX_DB * 100
);

static const struct snd_kcontrol_new scarlett2_mixer_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
		  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
	.name = "",
	.info = scarlett2_mixer_ctl_info,
	.get  = scarlett2_mixer_ctl_get,
	.put  = scarlett2_mixer_ctl_put,
	.private_value = SCARLETT2_MIXER_MAX_DB, /* max value */
	.tlv = { .p = db_scale_scarlett2_mixer }
};

/*** Mixer Mute Controls ***/
static int scarlett2_mixer_mute_ctl_get(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	ucontrol->value.enumerated.item[0] = !private->mix_mutes[elem->control];
	return 0;
}

static int scarlett2_mixer_mute_ctl_put(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	struct scarlett2_sw_cfg *sw_cfg = private->sw_cfg;

	int num_inputs = info->ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_OUT];
	int index = elem->control;
	int i, oval, val, err = 0;
	int mix_num;
	u8 *mutes;
	u32 mask;

	mutex_lock(&private->data_mutex);

	oval = private->mix_mutes[index];
	val = !ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->mix_mutes[index] = val;

	/* Compute the mixer to update */
	mix_num   = index / SCARLETT2_INPUT_MIX_MAX;

	if (sw_cfg != NULL) {
		/* Build the mute mask */
		mutes = &private->mix_mutes[mix_num * SCARLETT2_INPUT_MIX_MAX];
		mask = 0;
		for (i = 0; i < num_inputs; ++i)
			mask |= mutes[i] << i;

		/* Update software config for corresponding mixer */
		sw_cfg->mixer_mute[mix_num] = cpu_to_le32(mask);
		err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_mute[mix_num], sizeof(__le32));
		if (err < 0)
			goto unlock;
	}

	/* Update MIX settings as it does the original software */
	err = scarlett2_usb_set_mix(mixer, mix_num);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_mixer_mute_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = snd_ctl_boolean_mono_info,
	.get  = scarlett2_mixer_mute_ctl_get,
	.put  = scarlett2_mixer_mute_ctl_put
};

static int scarlett2_add_mixer_ctls(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_ports *ports = private->info->ports;
	struct scarlett2_sw_cfg *sw_cfg = private->sw_cfg;

	int err, i, j;
	int mix_idx, num_inputs, num_outputs;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	u32 level, mask;

	/* Check that device has mixer */
	if (!private->info->has_mixer)
		return 0;

	num_inputs  = ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_OUT];
	num_outputs = ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_IN];

	/* For each mixer */
	for (i=0; i<num_outputs; ++i) {
		mix_idx   = i * SCARLETT2_INPUT_MIX_MAX;

		/* Decode software config for mixer channel */
		mask = (sw_cfg) ? le32_to_cpu(sw_cfg->mixer_mute[i]) : 0;

		/* Add Mix control */
		for (j = 0; j < num_inputs; ++j, ++mix_idx) {
			level = le32_to_cpu(private->sw_cfg->mixer[i][j]);
			private->mix[mix_idx] = scarlett2_float_to_mixer_level(level) - (SCARLETT2_MIXER_MIN_DB * 2);
			private->mix_mutes[mix_idx] = !!(mask & (1 << j));

			/* Add Mixer volume control */
			snprintf(s, sizeof(s), "Mix %c In %02d Volume", 'A' + i, j + 1);
			err = scarlett2_add_new_ctl(mixer, &scarlett2_mixer_ctl, mix_idx, 1, s, NULL);
			if (err < 0)
				return err;

			/* Add Mixer mute control */
			snprintf(s, sizeof(s), "Mix %c In %02d Switch", 'A' + i, j + 1);
			err = scarlett2_add_new_ctl(mixer, &scarlett2_mixer_mute_ctl, mix_idx, 1, s, NULL);
			if (err < 0)
				return err;
		}

		/* Commit the actual mix state at startup */
		err = scarlett2_usb_set_mix(mixer, i);
		if (err < 0)
			return err;
	}

	return 0;
}

/*** Mux Source Selection Controls ***/
static int scarlett2_mux_src_enum_ctl_info(struct snd_kcontrol *kctl,
					   struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	int item = uinfo->value.enumerated.item;
	int items = private->num_inputs + 1;
	int port = clamp(item, 0, private->num_inputs) - 1;

	uinfo->type  = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = elem->channels;
	uinfo->value.enumerated.items = items;
	if (!items)
		return 0;
	if (item >= items)
		item = uinfo->value.enumerated.item = items - 1;

	scarlett2_fmt_port_name(uinfo->value.enumerated.name,
	                        sizeof(uinfo->value.enumerated.name),
	                        "%s",
	                        private->info,
	                        SCARLETT2_PORT_IN, port);

	return 0;
}

static int scarlett2_mux_src_enum_ctl_get(struct snd_kcontrol *kctl,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->mux[elem->control] + 1; /* Element 0 is always 'Off' */
	return 0;
}

static int scarlett2_commit_sw_routing(struct usb_mixer_interface *mixer, int src_port, int dst_port) {
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	struct scarlett2_sw_cfg *sw_cfg = private->sw_cfg;
	int err, i;
	int num_mixer_ins;
	int dst_port_type, dst_port_num;
	int src_port_type, src_port_num;
	int in_idx, out_idx, op_idx;
	char src[SNDRV_CTL_ELEM_ID_NAME_MAXLEN], dst[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	u32 mask;

	/* Nothing to do if there is no software config */
	if ((!sw_cfg) || (!info->sw_port_mapping))
		return 0;

	/* Decoded port OK? */
	err = scarlett2_decode_port(&dst_port_type, &dst_port_num, info->ports, SCARLETT2_PORT_OUT, dst_port);
	if (err < 0)
		return 0;

	/* DEBUG: output routing information */
	scarlett2_fmt_port_name(src, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_IN, src_port);
	scarlett2_fmt_port_name(dst, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_OUT, dst_port);

	if (dst_port_type == SCARLETT2_PORT_TYPE_MIX) {
		/* Decode input port index */
		in_idx  = scarlett2_drv2sw_port_num(info->ports, info->sw_port_mapping, SCARLETT2_PORT_IN, src_port);
		if (in_idx < 0)
			return 0;

		/* If output is working in stereo mode - switch it into mono mode */
		if (sw_cfg->mixer_in_map[dst_port_num] & 0x80) {
			/* Eliminate the 'FAT'-style allocation table for mixer lines */
			num_mixer_ins = info->ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_OUT];
			for (i = 0; i < num_mixer_ins; ++i) {
				/* The stereo pair should have bit 7 set */
				op_idx = sw_cfg->mixer_in_map[i];
				if (!(op_idx & 0x80))
					continue;

				/* The beginning of stereo pair should refer next channel (non-zero value) */
				op_idx &= 0x7f;
				if ((!op_idx) || (op_idx >= num_mixer_ins))
					continue;

				/* The current channel or referenced channel should match mixer index */
				if ((i == dst_port_num) || (op_idx == dst_port_num)) {
					sw_cfg->mixer_in_map[i]      = 0;
					sw_cfg->mixer_in_map[op_idx] = 0;
					err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_in_map, sizeof(u8) * num_mixer_ins);
					if (err < 0)
						return err;
					break;
				}
			}
		}

		/* Now we can update software configuration to set up proper routing */
		sw_cfg->mixer_in_mux[dst_port_num] = in_idx + 1;
		err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_in_mux[dst_port_num], sizeof(u8));
		if (err < 0)
			return err;
	}
	else {
		/* Decode source port type */
		err = scarlett2_decode_port(&src_port_type, &src_port_num, info->ports, SCARLETT2_PORT_IN, src_port);
		if (err < 0)
			return 0;

		/* Check that output is valid */
		out_idx = scarlett2_get_sw_port_num(info->sw_port_mapping, SCARLETT2_PORT_OUT, dst_port_type, dst_port_num);
		if (out_idx < 0)
			return 0;

		op_idx  = out_idx & (~1);

		/* If output is working in stereo mode - switch it into mono mode */
		mask    = le32_to_cpu(sw_cfg->stereo_sw);
		if (mask & (3 << op_idx)) {
			mask &= ~(3 << op_idx);

			/* Reset stereo mask for output channel */
			sw_cfg->stereo_sw = cpu_to_le32(mask);
			err = scarlett2_commit_software_config(mixer, &sw_cfg->stereo_sw, sizeof(__le32));
			if (err < 0)
				return err;

			/* Update routing for odd output channel if it is required */
			if (sw_cfg->out_mux[op_idx+1] != (sw_cfg->out_mux[op_idx]+1)) {
				sw_cfg->out_mux[op_idx+1] = sw_cfg->out_mux[op_idx] + 1;
				err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_in_map[op_idx], sizeof(u8) * 2);
				if (err < 0)
					return err;
			}

			/* Update mixer usage mask if mixer is enabled */
			mask = le32_to_cpu(sw_cfg->mixer_bind);
			if ((mask >> op_idx) & 3) { /* For mixer enabled for channels both bits should be 0 */
				mask &= ~(3 << op_idx);

				sw_cfg->mixer_bind = cpu_to_le32(mask);
				err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_bind, sizeof(__le32));
				if (err < 0)
					return err;
			}
		}

		/* Perform routing depending on the source port type */
		mask = le32_to_cpu(sw_cfg->mixer_bind);
		if (src_port_type == SCARLETT2_PORT_TYPE_MIX) {
			in_idx = src_port_num;
			/* Reset the mixer usage mask */
			mask  &= ~(1 << out_idx);
		} else {
			in_idx = scarlett2_get_sw_port_num(info->sw_port_mapping, SCARLETT2_PORT_IN, src_port_type, src_port_num);
			mask  |= (1 << out_idx);
		}

		/* Update mixer settings */
		sw_cfg->mixer_bind = cpu_to_le32(mask);
		err = scarlett2_commit_software_config(mixer, &sw_cfg->mixer_bind, sizeof(__le32));
		if (err < 0)
			return err;

		/* Now we can update software configuration to set up proper routing */
		sw_cfg->out_mux[out_idx] = in_idx + 1;
		err = scarlett2_commit_software_config(mixer, &sw_cfg->out_mux[out_idx], sizeof(u8));
		if (err < 0)
			return err;
	}

	return 0;
}

static int scarlett2_mux_src_enum_ctl_put(struct snd_kcontrol *kctl,
					  struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	int index = elem->control;
	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->mux[index];
	val  = ucontrol->value.integer.value[0];
	val  = clamp(val, 0, private->num_inputs) - 1;

	if (oval == val)
		goto unlock;

	/* Update routing for software configuration */
	err = scarlett2_commit_sw_routing(mixer, val, index);
	if (err < 0)
		goto unlock;

	/* Commit routing settings */
	private->mux[index] = val;
	err = scarlett2_usb_set_mux(mixer);
	if (err == 0)
		err = 1;

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_mux_src_enum_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_mux_src_enum_ctl_info,
	.get  = scarlett2_mux_src_enum_ctl_get,
	.put  = scarlett2_mux_src_enum_ctl_put,
};

static int scarlett2_parse_sw_mux(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	struct scarlett2_sw_cfg *sw_cfg = private->sw_cfg;
	char src[SNDRV_CTL_ELEM_ID_NAME_MAXLEN], dst[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

	int port_type, port_count, num_mix_in;
	int dst_port, src_port, mix_bit;
	int sw_idx, sp_idx;
	u32 st_map, mix_map;
	int i, j;

	static const int sw_list[] = {
		SCARLETT2_PORT_TYPE_ANALOGUE,
		SCARLETT2_PORT_TYPE_SPDIF,
		SCARLETT2_PORT_TYPE_ADAT,
		SCARLETT2_PORT_TYPE_PCM
	};

	/* If we have software configuration and port mapping - apply them */
	if ((sw_cfg == NULL) || (info->sw_port_mapping == NULL))
		return 0;

	/* Apply physical output routing */
	st_map = le32_to_cpu(sw_cfg->stereo_sw);
	mix_map = le32_to_cpu(sw_cfg->mixer_bind);

	for (i=0; i < sizeof(sw_list)/sizeof(int); ++i) {
		port_type = sw_list[i];
		port_count = info->ports[port_type].num[SCARLETT2_PORT_OUT];

		for (j=0; j<port_count; ++j) {
			sw_idx = scarlett2_get_sw_port_num(info->sw_port_mapping, SCARLETT2_PORT_OUT, port_type, j);
			if (sw_idx < 0) /* Skip port if it is not mapped in software configuration */
				continue;

			/* Check that destination port is valid */
			dst_port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, port_type, j);
			if ((dst_port < 0) || (dst_port >= SCARLETT2_MUX_MAX))
				continue;

			/* Check whether output is in stereo or mono mode and fetch the source port number */
			sp_idx = sw_idx & (~1); /* Index of byte for the corresponding stereo (even) channel */

			if (st_map & ((1 << sw_idx) | (1 << sp_idx))) {
				src_port = (sw_idx & 1) ? sw_cfg->out_mux[sp_idx]+1 : sw_cfg->out_mux[sp_idx];
				mix_bit  = 1 << sp_idx;
			}
			else {
				src_port = sw_cfg->out_mux[sw_idx];
				mix_bit  = 1 << sw_idx;
			}

			/* Check that channel is routed to mixer */
			if (mix_map & mix_bit) /* Bit is set - not routed to mixer, it is a physical port number */
				src_port = scarlett2_sw2drv_port_num(info->ports, info->sw_port_mapping, SCARLETT2_PORT_IN, src_port);
			else if (src_port > 0) /* Bit not set and source number is greater than zero - routed via mixer */
				src_port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_IN, SCARLETT2_PORT_TYPE_MIX, src_port - 1);

			/* DEBUG: output routing information */
			scarlett2_fmt_port_name(src, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_IN,  src_port);
			scarlett2_fmt_port_name(dst, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_OUT, dst_port);

			/* Write the actual mux configuration */
			private->mux[dst_port] = src_port;
		}
	}

	/* Apply mixer routing */
	num_mix_in = (info->has_mixer) ? info->ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_OUT] : 0;
	for (i=0; i < num_mix_in; ++i) {
		sw_idx = i;
		src_port = sw_cfg->mixer_in_mux[sw_idx];

		/* Decode the routing port */
		src_port = scarlett2_sw2drv_port_num(info->ports, info->sw_port_mapping, SCARLETT2_PORT_IN, src_port);

		/* Check that destination port is valid */
		dst_port = scarlett2_get_port_num(info->ports, SCARLETT2_PORT_OUT, SCARLETT2_PORT_TYPE_MIX, sw_idx);
		if ((dst_port < 0) || (dst_port >= SCARLETT2_MUX_MAX))
			continue;

		/* DEBUG: output routing information */
		scarlett2_fmt_port_name(src, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_IN, src_port);
		scarlett2_fmt_port_name(dst, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s", info, SCARLETT2_PORT_OUT, dst_port);

		/* Write the actual mux configuration */
		private->mux[dst_port] = src_port;
	}

	return 0;
}

static int scarlett2_init_mux(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;

	int port, err;
	char name[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];

	/* Check that device has MUX */
	if (!info->has_mux)
		return 0;

	/* Read MUX settings */
	err = scarlett2_usb_get_mux(mixer);
	if (err < 0)
		return err;

	/* Parse software MUX configuration if present */
	err = scarlett2_parse_sw_mux(mixer);
	if (err < 0)
		return err;

	/* Apply MUX settings */
	err = scarlett2_usb_set_mux(mixer);
	if (err < 0)
		return err;

	/* Create mux control ports based on output port list */
	for (port = 0; port < private->num_outputs; ++port) {
		/* Create port descriptor */
		scarlett2_fmt_port_name(name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN, "%s Source", info, SCARLETT2_PORT_OUT, port);
		err = scarlett2_add_new_ctl(mixer,
					    &scarlett2_mux_src_enum_ctl,
					    port, 1, name, NULL);
		if (err < 0)
			return err;
	}

	return err;
}

/*** Meter Controls ***/

static int scarlett2_meter_ctl_info(struct snd_kcontrol *kctl,
				    struct snd_ctl_elem_info *uinfo)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;

	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = elem->channels;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 4095;
	uinfo->value.integer.step = 1;
	return 0;
}

static int scarlett2_meter_ctl_get(struct snd_kcontrol *kctl,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	u16 meter_levels[SCARLETT2_NUM_METERS];
	int i, err;

	err = scarlett2_usb_get_meter_levels(elem->head.mixer, meter_levels);
	if (err < 0)
		return err;

	for (i = 0; i < elem->channels; i++)
		ucontrol->value.integer.value[i] = meter_levels[i];

	return 0;
}

static const struct snd_kcontrol_new scarlett2_meter_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_PCM,
	.access = SNDRV_CTL_ELEM_ACCESS_READ | SNDRV_CTL_ELEM_ACCESS_VOLATILE,
	.name = "",
	.info = scarlett2_meter_ctl_info,
	.get  = scarlett2_meter_ctl_get
};

static int scarlett2_add_meter_ctl(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;

	/** Ensure the device has level meters */
	if (!private->info->has_meters)
		return 0;

	return scarlett2_add_new_ctl(mixer, &scarlett2_meter_ctl,
				     0, SCARLETT2_NUM_METERS,
				     "Level Meter", NULL);
}

/*** MSD Controls ***/

static int scarlett2_msd_ctl_get(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct scarlett2_mixer_data *private = elem->head.mixer->private_data;

	ucontrol->value.enumerated.item[0] = private->msd_switch;
	return 0;
}

static int scarlett2_msd_ctl_put(struct snd_kcontrol *kctl,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	int oval, val, err = 0;

	mutex_lock(&private->data_mutex);

	oval = private->msd_switch;
	val = !!ucontrol->value.integer.value[0];

	if (oval == val)
		goto unlock;

	private->msd_switch = val;

	/* Send switch change to the device */
	err = scarlett2_usb_set_config(mixer, SCARLETT2_CONFIG_MSD_SWITCH,
				       0, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_msd_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_bool_enum_ctl_info,
	.get  = scarlett2_msd_ctl_get,
	.put  = scarlett2_msd_ctl_put,
};

static int scarlett2_add_msd_ctl(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;

	/* Check that device has MSD mode */
	if (!info->has_msd_mode)
		return 0;

	/* If MSD mode is off, hide the switch by default */
	if (!private->msd_switch && !(mixer->chip->setup & SCARLETT2_MSD_ENABLE))
		return 0;

	/* Add MSD control */
	return scarlett2_add_new_ctl(mixer, &scarlett2_msd_ctl,
				     0, 1, "MSD Mode", NULL);
}

/*** Speaker Switching Control ***/
static int scarlett2_update_speaker_switch_enum_ctl(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	u8 speaker_switching, speaker_switch;
	int err = 0;
	
	private->speaker_updated = 0;
	if (!info->has_speaker_switching)
		return 0;

	/* check if speaker switching is enabled */
	err = scarlett2_usb_get_config(
		mixer,
		SCARLETT2_CONFIG_SPEAKER_SWITCHING_SWITCH,
		1, &speaker_switching);
	if (err < 0)
		return err;
	
	/* get actual speaker & talkback configuration */
	err = scarlett2_usb_get_config(
		mixer,
		SCARLETT2_CONFIG_MAIN_ALT_SPEAKER_SWITCH,
		1, &speaker_switch);
	if (err < 0)
		return err;

	/* decode speaker & talkback values */
	private->speaker_switch  = (speaker_switching) ? (speaker_switch & 1) + 1 : 0;
	if (info->has_talkback)
		private->talkback_switch = !!(speaker_switch & 2);
	
	return 0;
}


static int scarlett2_speaker_switch_enum_ctl_info(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_info *uinfo)
{
	static const char *const values[3] = {
		"Off", "Main", "Alt"
	};

	return snd_ctl_enum_info(uinfo, 1, 3, values);
}

static int scarlett2_speaker_switch_enum_ctl_get(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->speaker_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_speaker_switch_enum_ctl(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] =
		private->speaker_switch;
	return 0;
}

static int scarlett2_talkback_switch_ctl_get(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (private->speaker_updated) {
		mutex_lock(&private->data_mutex);
		scarlett2_update_speaker_switch_enum_ctl(mixer);
		mutex_unlock(&private->data_mutex);
	}

	ucontrol->value.enumerated.item[0] =
		private->talkback_switch;
	return 0;
}

static int scarlett2_mix_talkback_switch_ctl_get(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	ucontrol->value.enumerated.item[0] =
		private->mix_talkback[elem->control];
	return 0;
}

static int scarlett2_speaker_switch_update_state(struct usb_mixer_interface *mixer, int alt, int talkback) {
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	int old_alt, old_talk, err = 0;

	mutex_lock(&private->data_mutex);

	old_alt = private->speaker_switch;
	old_talk = private->talkback_switch;
	
	if ((old_alt == alt) && (old_talk == talkback))
		goto unlock;

	private->speaker_switch = alt;
	private->talkback_switch = talkback;
	
	/* enable/disable speaker switching */
	if (old_alt == 0 || alt == 0) {
		err = scarlett2_usb_set_config(
			mixer, SCARLETT2_CONFIG_SPEAKER_SWITCHING_SWITCH,
			0, !!alt);
	}

	/* update talkback speaker and talkback */
	if (!err) {
		int val = (alt == 2);
		if (info->has_talkback)
			val |= talkback << 1;

		err = scarlett2_usb_set_config(
			mixer, SCARLETT2_CONFIG_MAIN_ALT_SPEAKER_SWITCH,
			0, val);
	}

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static int scarlett2_speaker_switch_enum_ctl_put(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	return scarlett2_speaker_switch_update_state(mixer, 
						     ucontrol->value.integer.value[0],
						     private->talkback_switch);
}

static int scarlett2_talkback_switch_ctl_put(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;

	return scarlett2_speaker_switch_update_state(mixer, 
						     private->speaker_switch,
						     ucontrol->value.integer.value[0]);
}

static int scarlett2_mix_talkback_switch_ctl_put(
	struct snd_kcontrol *kctl, struct snd_ctl_elem_value *ucontrol)
{
	struct usb_mixer_elem_info *elem = kctl->private_data;
	struct usb_mixer_interface *mixer = elem->head.mixer;
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_ports *ports = private->info->ports;
	int i, val, old_val, num_mixes, err = 0;

	mutex_lock(&private->data_mutex);

	old_val  = private->mix_talkback[elem->control];
	val      = !! ucontrol->value.integer.value[0];
	if (old_val == val)
		goto unlock;

	private->mix_talkback[elem->control] = val;

	/* Build bit mask for each mixer channel and commit to device */
	val      = 0;
	num_mixes = ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_IN];
	for (i=0; i<num_mixes; ++i)
		val |= (private->mix_talkback[i]) << i;

	err = scarlett2_usb_set_config(
		mixer, SCARLETT2_CONFIG_MIX_TALKBACK,
		0, val);

unlock:
	mutex_unlock(&private->data_mutex);
	return err;
}

static const struct snd_kcontrol_new scarlett2_speaker_switch_enum_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = scarlett2_speaker_switch_enum_ctl_info,
	.get  = scarlett2_speaker_switch_enum_ctl_get,
	.put  = scarlett2_speaker_switch_enum_ctl_put,
};

static const struct snd_kcontrol_new scarlett2_talkback_switch_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = snd_ctl_boolean_mono_info,
	.get  = scarlett2_talkback_switch_ctl_get,
	.put  = scarlett2_talkback_switch_ctl_put,
};

static const struct snd_kcontrol_new scarlett2_mix_talkback_switch_ctl = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "",
	.info = snd_ctl_boolean_mono_info,
	.get  = scarlett2_mix_talkback_switch_ctl_get,
	.put  = scarlett2_mix_talkback_switch_ctl_put,
};

static int scarlett2_add_speaker_switch_ctl(
	struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_ports *ports = private->info->ports;
	char s[SNDRV_CTL_ELEM_ID_NAME_MAXLEN];
	int err = 0, num_mixes, i;

	if (info->has_speaker_switching) {
		/* Add speaker switching control */
		err = scarlett2_add_new_ctl(
			mixer, &scarlett2_speaker_switch_enum_ctl,
			0, 1, "Speaker Switching", &private->speaker_ctl);
		
		if (err < 0)
			return err;
	}
	
	if (info->has_talkback) {
		/* Add talkback switching control */
		err = scarlett2_add_new_ctl(
			mixer, &scarlett2_talkback_switch_ctl,
			0, 1, "Talkback Switching", &private->talkback_ctl);
		
		if (err < 0)
			return err;
		
		/* Add 'talkback enable' for mix */
		num_mixes = ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_IN];
		for (i=0; i<num_mixes; ++i) {
			snprintf(s, sizeof(s), "Mix %c Talkback", 'A' + i);
			
			err = scarlett2_add_new_ctl(
				mixer, &scarlett2_mix_talkback_switch_ctl,
				i, 1, s, &private->mix_talkback_ctls[i]);
		
			if (err < 0)
				return err;
		}
	}
	
	return 0;
}

/*** Cleanup/Suspend Callbacks ***/

static void scarlett2_private_free(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;

	cancel_delayed_work_sync(&private->work);
	if (private->sw_cfg != NULL)
		kfree(private->sw_cfg);
	kfree(private);
	mixer->private_data = NULL;
}

static void scarlett2_private_suspend(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;

	if (cancel_delayed_work_sync(&private->work))
		scarlett2_config_save(private->mixer);
}

/* Look through the interface descriptors for the Focusrite Control
 * interface (bInterfaceClass = 255 Vendor Specific Class) and set the
 * interface number, endpoint address, packet size, and interval in
 * private
 */
static int scarlett2_find_fc_interface(struct usb_device *dev,
				       struct scarlett2_mixer_data *private) {
	struct usb_host_config *config = dev->actconfig;
	int i;

	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		struct usb_interface *intf = config->interface[i];
		struct usb_interface_descriptor *desc =
			&intf->altsetting[0].desc;
		if (desc->bInterfaceClass == 255) {
			struct usb_endpoint_descriptor *epd =
				get_endpoint(intf->altsetting, 0);
			private->interface = desc->bInterfaceNumber;
			private->endpoint = epd->bEndpointAddress &
				USB_ENDPOINT_NUMBER_MASK;
			private->maxpacketsize = epd->wMaxPacketSize;
			private->interval = epd->bInterval;
			return 0;
		}
	}

	return -1;
}

/* Initialise private data, routing, sequence number */
static int scarlett2_init_private(struct usb_mixer_interface *mixer,
				  const struct scarlett2_device_info *info)
{
	struct scarlett2_mixer_data *private =
		kzalloc(sizeof(struct scarlett2_mixer_data), GFP_KERNEL);
	int err;

	if (!private)
		return -ENOMEM;

	mutex_init(&private->usb_mutex);
	mutex_init(&private->data_mutex);
	INIT_DELAYED_WORK(&private->work, scarlett2_config_save_work);
	mixer->private_data = private;
	mixer->private_free = scarlett2_private_free;
	mixer->private_suspend = scarlett2_private_suspend;
	private->info = info;
	private->num_inputs = scarlett2_count_ports(info->ports, SCARLETT2_PORT_IN);
	private->num_outputs = scarlett2_count_ports(info->ports, SCARLETT2_PORT_OUT);
	private->scarlett2_seq = 0;
	private->mixer = mixer;
	private->vol_updated = 0;
	private->line_ctl_updated = 0;
	private->speaker_updated = 0;
	private->speaker_switch = 0;
	private->talkback_switch = 0;
	private->sw_cfg = NULL;

	err = scarlett2_find_fc_interface(mixer->chip->dev, private);

	if (err < 0)
		return -EINVAL;


	return 0;
}

/* Read line-in config and line-out volume settings on start */
static int scarlett2_read_configs(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	const struct scarlett2_ports *ports = info->ports;
	u8 air_switches[SCARLETT2_AIR_SWITCH_MAX];
	u8 msd_switch;
	u8 speaker_switching, speaker_switch;
	__le16 mix_talkbacks;
	int err, i, num_mixes, val;

	/* LINE/INST, PAD, 48V power, 48V retain */
	err = scarlett2_update_line_ctl_switches(mixer);
	if (err < 0)
		return err;

	/* AIR input settings */
	if (info->air_input_count) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_AIR_SWITCH,
			info->air_input_count,
			air_switches);
		if (err < 0)
			return err;
		for (i = 0; i < info->air_input_count; i++)
			private->air_switch[i] = !! air_switches[i];
	}

	/* Mass Storage Device (MSD) mode */
	if (info->has_msd_mode) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_MSD_SWITCH,
			1,
			&msd_switch);
		if (err < 0)
			return err;
		private->msd_switch = msd_switch;
	}

	/* Speaker switching (ALT button) and optional TALKBACK button */
	if (info->has_speaker_switching) {
		/* check if speaker switching is enabled */
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_SPEAKER_SWITCHING_SWITCH,
			1, &speaker_switching);
		if (err < 0)
			return err;
	
		/* get actual speaker & talkback configuration */
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_MAIN_ALT_SPEAKER_SWITCH,
			1, &speaker_switch);
		if (err < 0)
			return err;

		/* decode speaker & talkback values */
		private->speaker_switch  = (speaker_switching) ? (speaker_switch & 1) + 1 : 0;
		if (info->has_talkback)
			private->talkback_switch = !!(speaker_switch & 2);
	}

	/* Talkback routing to each output of internal mixer */
	if (info->has_talkback) {
		err = scarlett2_usb_get_config(
			mixer,
			SCARLETT2_CONFIG_MIX_TALKBACK,
			1, &mix_talkbacks);
		if (err < 0)
			return err;

		/* Each talkback switch is just a bit assigned to the corresponding mixer output */
		val = le16_to_cpu(mix_talkbacks);
		num_mixes = ports[SCARLETT2_PORT_TYPE_MIX].num[SCARLETT2_PORT_IN];
		for (i=0; i<num_mixes; ++i)
			private->mix_talkback[i] = !!(val & (1 << i));
	}

	/* Hardware-controlled volume and mute settings for outputs */
	err = scarlett2_update_volumes(mixer);
	if (err < 0)
		return err;

	return 0;
}

static void scarlett2_calc_software_cksum(struct scarlett2_sw_cfg *sw) {
	int i;
	s32 checksum;
	const __le32 *ckptr;

	/* The checksum is a sign-invered sum of 32-bit integers
	  assuming checksum field value being 0 */
	checksum = 0;
	sw->checksum = 0;
	ckptr = (__le32 *)sw;
	for (i=0; i<sizeof(struct scarlett2_sw_cfg); i += sizeof(__le32))
		checksum -= le32_to_cpu(*(ckptr++));
	sw->checksum = cpu_to_le32(checksum);
}

static int scarlett2_read_software_configs(struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	struct scarlett2_sw_cfg *sw;
	int err;
	__le16 le_sw_cfg_size;
	int sw_cfg_size;

	/* Check that device has software configuration */
	if (!private->info->has_sw_config) {
		usb_audio_info(mixer->chip, "Device has no software configuration");
		return 0;
	}

	/* Obtain actual size of sofware config */
	err = scarlett2_usb_get(mixer,
	                SCARLETT2_SW_CONFIG_BASE + offsetof(struct scarlett2_sw_cfg, szof),
	                &le_sw_cfg_size, sizeof(le_sw_cfg_size)
	      );

	if (err < 0)
		return err;
		
	/* We need to create software configuration area if it does not exist
	   or it is present and has valid size */
	sw_cfg_size = le16_to_cpu(le_sw_cfg_size);

	/* Allocate space for software config */
	sw = kzalloc(sizeof(struct scarlett2_sw_cfg), GFP_KERNEL);
	if (sw == NULL)
		return -ENOMEM;

	if (sw_cfg_size == 0) {
		usb_audio_info(mixer->chip, "Creating software configuration area for device");
		sw->all_size = cpu_to_le16(sizeof(struct scarlett2_sw_cfg) + 0x0c);
		sw->magic1   = cpu_to_le16(0x3006);
		sw->version  = cpu_to_le32(0x5);
		sw->szof     = cpu_to_le16(sizeof(struct scarlett2_sw_cfg));
		scarlett2_calc_software_cksum(sw);

		err = scarlett2_usb_set(mixer, SCARLETT2_SW_CONFIG_BASE, sw, sizeof(struct scarlett2_sw_cfg));
	}
	else if (sw_cfg_size != sizeof(struct scarlett2_sw_cfg)) {
		/* Free allocated area, output warning and return */
		usb_audio_warn(mixer->chip, "Unsupported size of software configuration "
		    "area (0x%x), expected to be 0x%x, will proceed with significantly "
		    "lower functionality",
		    sw_cfg_size, (int)(sizeof(struct scarlett2_sw_cfg))
		);
		goto leave;
	} else
		err = scarlett2_usb_get(mixer, SCARLETT2_SW_CONFIG_BASE, sw, sizeof(struct scarlett2_sw_cfg));

	if (err < 0)
		goto leave;
	
	/* Validate the software configuration area header */
	if ((le16_to_cpu(sw->all_size) != (sizeof(struct scarlett2_sw_cfg) + 0x0c)) ||
	    (le16_to_cpu(sw->magic1) != 0x3006) ||
	    (le32_to_cpu(sw->version) != 0x5) ||
	    (le16_to_cpu(sw->szof) != sizeof(struct scarlett2_sw_cfg))) {

		usb_audio_warn(mixer->chip, "The format of software configuration header "
		    "does not match expected, will proceed with significantly "
		    "lower functionality"
		);
		goto leave;
	}

	/* Commit the pointer */
	private->sw_cfg = sw;
	sw              = NULL;

	usb_audio_info(mixer->chip, "Successfully initialized software configuration area");

leave:
	if (sw != NULL)
		kfree(sw);
	return err;
}


static int scarlett2_commit_software_config(
	struct usb_mixer_interface *mixer,
	void *ptr, /* the pointer of the first changed byte in the configuration */
	int bytes /* the actual number of bytes changed in the configuration */
)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	int offset, err = 0;

	/* Check bounds, we should not exceed them */
	offset = ((u8 *)ptr) - ((u8 *)private->sw_cfg);

	if ((private->sw_cfg == NULL) ||
	    (offset < 0) || 
	    ((offset + bytes) > sizeof(struct scarlett2_sw_cfg))) {
		usb_audio_warn(mixer->chip, "tried to commit data with invalid offset %d", offset);
		return -EINVAL;
	}

	/* Re-compute the checksum of the software configuration area */
	scarlett2_calc_software_cksum(private->sw_cfg);

	/* Cancel any pending NVRAM save */
	cancel_delayed_work_sync(&private->work);

	/* Transfer the configuration with fixed-size data chunks */
	err = scarlett2_usb_set(mixer, SCARLETT2_SW_CONFIG_BASE + offset, ptr, bytes);
	if (err < 0)
		goto leave;

	/* Transfer the actual checksum */
	err = scarlett2_usb_set(mixer, SCARLETT2_SW_CONFIG_BASE + offsetof(struct scarlett2_sw_cfg, checksum),
	                        &private->sw_cfg->checksum, sizeof(private->sw_cfg->checksum)
	         );
	if (err < 0)
		goto leave;

leave:
	/* Schedule the change to be written to NVRAM */
	schedule_delayed_work(&private->work, msecs_to_jiffies(2000));
	return err;
}

/* Notify on volume change */
static void scarlett2_mixer_interrupt_vol_change(
	struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_ports *ports = private->info->ports;
	int num_line_out =
		ports[SCARLETT2_PORT_TYPE_ANALOGUE].num[SCARLETT2_PORT_OUT];
	int i;

	private->vol_updated = 1;

	snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
		       &private->master_vol_ctl->id);

	for (i = 0; i < num_line_out; i++) {
		if (!private->vol_sw_hw_switch[i])
			continue;
		snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &private->vol_ctls[i]->id);
	}
}

/* Notify on PAD/INST/48V button state change */
static void scarlett2_mixer_interrupt_line_in_ctl_change(
	struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	const struct scarlett2_device_info *info = private->info;
	int i;

	/* Trigger all PAD inputs for changes */
	if (info->pad_input_count) {
		private->line_ctl_updated = 1;

		for (i = 0; i < info->pad_input_count; i++) {
			snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &private->pad_ctls[i]->id);
		}
	}
	
	/* Trigger all INST inputs for changes */
	if (info->level_input_count) {
		private->line_ctl_updated = 1;

		for (i = 0; i < info->level_input_count; i++) {
			snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &private->level_ctls[i]->id);
		}
	}
	
	/* Trigger all 48V inputs for changes */
	if (info->power_48v_count) {
		private->line_ctl_updated = 1;

		for (i = 0; i < info->power_48v_count; i++) {
			snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
				       &private->pow_ctls[i]->id);
		}
	}
}

/* Notify on button change */
static void scarlett2_mixer_interrupt_button_change(
	struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;
	int i;

	private->vol_updated = 1;

	for (i = 0; i < private->info->button_count; i++)
		snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &private->button_ctls[i]->id);
}

/* Notify on speaker change */
static void scarlett2_mixer_interrupt_speaker_change(
	struct usb_mixer_interface *mixer)
{
	struct scarlett2_mixer_data *private = mixer->private_data;

	private->speaker_updated = 1;

	snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
		       &private->speaker_ctl->id);

	if (private->info->has_talkback)
		snd_ctl_notify(mixer->chip->card, SNDRV_CTL_EVENT_MASK_VALUE,
			       &private->talkback_ctl->id);
}

/* Interrupt callback */
static void scarlett2_mixer_interrupt(struct urb *urb)
{
	struct usb_mixer_interface *mixer = urb->context;
	int len = urb->actual_length;
	int ustatus = urb->status;

	if (ustatus != 0)
		goto requeue;

	if (len == 8) {
		u32 data = le32_to_cpu(*(u32 *)urb->transfer_buffer);

		if (data & SCARLETT2_USB_INTERRUPT_VOL_CHANGE)
			scarlett2_mixer_interrupt_vol_change(mixer);
		if (data & SCARLETT2_USB_INTERRUPT_LINE_CTL_CHANGE)
			scarlett2_mixer_interrupt_line_in_ctl_change(mixer);
		if (data & SCARLETT2_USB_INTERRUPT_BUTTON_CHANGE)
			scarlett2_mixer_interrupt_button_change(mixer);
		if (data & SCARLETT2_USB_INTERRUPT_SPEAKER_CHANGE)
			scarlett2_mixer_interrupt_speaker_change(mixer);
	} else {
		usb_audio_err(mixer->chip,
			      "scarlett mixer interrupt length %d\n", len);
	}

requeue:
	if (ustatus != -ENOENT &&
	    ustatus != -ECONNRESET &&
	    ustatus != -ESHUTDOWN) {
		urb->dev = mixer->chip->dev;
		urb->actual_length = 0;
		*(u32 *)urb->transfer_buffer = 0;
		usb_submit_urb(urb, GFP_ATOMIC);
	}
}

static int scarlett2_mixer_status_create(struct usb_mixer_interface *mixer)
{
	struct usb_device *dev = mixer->chip->dev;
	struct scarlett2_mixer_data *private = mixer->private_data;
	unsigned int pipe = usb_rcvintpipe(dev, private->endpoint);
	void *transfer_buffer;

	if (mixer->urb) {
		usb_audio_err(mixer->chip,
			      "%s: mixer urb already in use!\n", __func__);
		return 0;
	}

	if (snd_usb_pipe_sanity_check(dev, pipe))
		return -EINVAL;

	mixer->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!mixer->urb)
		return -ENOMEM;

	transfer_buffer = kmalloc(private->maxpacketsize, GFP_KERNEL);
	if (!transfer_buffer)
		return -ENOMEM;

	usb_fill_int_urb(mixer->urb, dev, pipe,
			 transfer_buffer, private->maxpacketsize,
			 scarlett2_mixer_interrupt, mixer,
			 private->interval);

	return usb_submit_urb(mixer->urb, GFP_KERNEL);
}

/* Entry point */
int snd_scarlett_gen2_controls_create(struct usb_mixer_interface *mixer)
{
	struct snd_usb_audio *chip = mixer->chip;
	const struct scarlett2_device_info *info;
	int i, err;

	/* only use UAC_VERSION_2 */
	if (!mixer->protocol)
		return 0;

	/* Find actual device descriptor */
	for (i=0; ; ++i) {
		info = scarlett2_supported_devices[i];
		if (info == NULL) /* End of list, device is not (yet) supported */
			return -EINVAL;
		if (info->usb_id == chip->usb_id)
			break;
	}

	if (!(chip->setup & SCARLETT2_ENABLE)) {
		usb_audio_err(chip,
			"Focusrite Scarlett Gen 2/3 Mixer Driver disabled; "
			"use options snd_usb_audio vid=0x%04x pid=0x%04x "
			"device_setup=1 to enable and report any issues "
			"to g@b4.vu",
			USB_ID_VENDOR(chip->usb_id),
			USB_ID_PRODUCT(chip->usb_id));
		return 0;
	}

	usb_audio_err(chip, "Focusrite Scarlett Gen 2/3 Mixer Driver enabled pid=0x%04x", USB_ID_PRODUCT(chip->usb_id));

	/* Initialise private data, routing, sequence number */
	err = scarlett2_init_private(mixer, info);
	if (err < 0)
		return err;

	/* Send proprietary USB initialisation sequence */
	err = scarlett2_usb_init(mixer);
	if (err < 0)
		return err;

	/* Read volume levels and controls from the interface */
	err = scarlett2_read_configs(mixer);
	if (err < 0)
		return err;

	/* Read software configuration containing mixer gains */
	err = scarlett2_read_software_configs(mixer);
	if (err < 0)
		return err;

	/* Create the analogue output controls */
	err = scarlett2_add_line_out_ctls(mixer);
	if (err < 0)
		return err;

	/* Create the mute controls */
	err = scarlett2_add_mute_ctls(mixer);
	if (err < 0)
		return err;

	/* Create the analogue input controls */
	err = scarlett2_add_line_in_ctls(mixer);
	if (err < 0)
		return err;

	/* Create the input, output, and mixer mux input selections */
	err = scarlett2_init_mux(mixer);
	if (err < 0)
		return err;

	/* Create the matrix mixer controls */
	err = scarlett2_add_mixer_ctls(mixer);
	if (err < 0)
		return err;

	/* Create the level meter controls */
	err = scarlett2_add_meter_ctl(mixer);
	if (err < 0)
		return err;

	/* Create gain halos controls */
	err = scarlett2_add_ghalo_ctls(mixer);
	if (err < 0)
		return err;

	/* Create the MSD control */
	err = scarlett2_add_msd_ctl(mixer);
	if (err < 0)
		return err;

	/* Create the speaker switch control */
	err = scarlett2_add_speaker_switch_ctl(mixer);
	if (err < 0)
		return err;

	/* Set up the interrupt polling */
	err = scarlett2_mixer_status_create(mixer);
	if (err < 0)
		return err;

	return 0;
}
