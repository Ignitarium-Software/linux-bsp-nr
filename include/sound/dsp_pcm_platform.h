/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __DSP_PCM_PLATFORM_H
#define __DSP_PCM_PLATFORM_H

#define DSP_CTRL_RPMSG_VERSION (0x0U)
#define BUFFER_LEN (4096U)          /* 4kB */
#define FRAME_PERIOD (4U)

/* DSP Control Message type */
#define RPMSG_INIT       0x3001     /* Init message from CA to remote core */
#define CONFIG_REQ       0x2001     /* CA-DSP */
#define PCM_START        0x2002     /* CA-DSP */
#define PCM_PAUSE        0x2003     /* CA-DSP */
#define PCM_RESUME       0x2004     /* CA-DSP */
#define PCM_STOP         0x2005     /* CA-DSP */
#define POS_QUERY        0x2006     /* CA-DSP */
#define PCM_STATUS       0x8000     /* DSP-CA (reply/heartbeat/event) */
#define CONFIG_REPLY     0xA001     /* DSP-CA (config reply) */
#define POS_REPLY        0xA006     /* DSP-CA (position/cursor reply) */

struct rpmsg_hdr {
	uint16_t version;
	uint16_t msg_type;
	uint32_t msg_id;
	uint32_t stream_id;
	uint32_t payload_len;
} __attribute__((packed));

struct trigger_req {
	uint32_t cmd;               /* START/STOP/PAUSE/RESUME/DRAIN */
} __attribute__((packed));

struct dsp_config_req_msg {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t in_rate;           /* app rate for PB/ Hw rate for REC */
	uint32_t in_channels;
	uint32_t in_format;         /* ALSA format */
	uint32_t out_rate;          /* Hw rate for PB / app rate for REC */
	uint32_t out_channels;
	uint32_t out_format;
	uint64_t pcm_rb_phys;       /* DSP write/read target for DSP */
	uint32_t pcm_rb_size;
	uint64_t hw_rb_phys;
	uint32_t hw_rb_size;
	uint32_t period_frames;     /* matches ALSA period */
	uint32_t periods;
} __attribute__((packed));

struct dsp_status_msg {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t status;            /* RUNNING, STOPPED, PAUSED, etc. */
	uint64_t pcm_dsp_pos;       /* frames consumed since stream start */
	uint64_t hw_rb_write_pos;   /* optional: frames written to LP HW RB */
	uint32_t period_frames;     /* echo for safety */
	uint32_t rb_frames;         /* total frames in PCM RB */
} __attribute__((packed));

union rpmsg_payload {
	struct trigger_req trigger;
	struct dsp_config_req_msg dsp_config;
	struct dsp_status_msg dsp_status;
};

struct rpmsg_packet {
	struct rpmsg_hdr header;
	union rpmsg_payload payload;
} __attribute__((packed));

#endif /* __DSP_PCM_PLATFORM_H */
