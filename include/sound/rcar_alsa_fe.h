/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __DSP_PCM_PLATFORM_H
#define __DSP_PCM_PLATFORM_H

#define DSP_CTRL_RPMSG_VERSION (0x0U)
#define CR_CTRL_RPMSG_VERSION  (0x0U)

#define PERIOD_MS        (5U)        /* dsp process time for 1 period frame*/
#define BUFFER_LEN       (16384U)    /* 16kB */
#define FRAME_RATE       (48000U)    /* 48kHz */
#define PERIOD_FRAMES    (FRAME_RATE * PERIOD_MS / 1000)
#define PERIOD_BYTES     (PERIOD_FRAMES * 4)

#define RPMSG_INIT          (0x3001) /* Init message from CA to remote core */

/* CR Control Message Types */
#define OPEN_REQ            (0x1001) /* CA -> CR */
#define OPEN_RESP           (0x9001) /* CR -> CA */
#define TRIGGER_START       (0x1002) /* CA -> CR*/
#define TRIGGER_PAUSE       (0x1003) /* CA -> CR*/
#define TRIGGER_RESUME      (0x1004) /* CA -> CR*/
#define TRIGGER_STOP        (0x1005) /* CA -> CR*/
#define POS_QUERY_CR        (0x1008) /* CA -> CR*/
#define POS_REPLY_CR        (0x9008) /* CR -> CA*/
#define EVENT_STARTED       (0x9010) /* CR -> CA*/
#define EVENT_PAUSED        (0x9011) /* CR -> CA*/
#define EVENT_RESUMED       (0x9012) /* CR -> CA*/
#define EVENT_STOPPED       (0x9013) /* CR -> CA*/
#define EVENT_XRUN          (0xA014) /* CR -> CA*/
#define EVENT_HP_TAKEOVER   (0xA015) /* CR -> CA*/
#define EVENT_HP_RELEASED   (0xA016) /* CR -> CA*/
#define EVENT_ERROR         (0xA017) /* CR -> CA*/

/* DSP Control Message type */
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

struct open_req_msg {
	uint32_t dir;               /* playback=0, capture=1 */
	uint32_t rate;              /* app rate */
	uint32_t channels;
	uint32_t format;            /* ALSA format */
	uint32_t period_frames;     /* ALSA period size */
	uint32_t periods;           /* period count */
	uint64_t pcm_rb_phys;       /* phys addr of PCM RB */
	uint32_t pcm_rb_size;
} __attribute__((packed));

struct open_resp_msg {
	uint32_t status;            /* 0 for success, -eerno for failure */
	uint32_t stream_id;
	uint32_t hw_rate;           /* hw rate */
	uint32_t hw_channels;
	uint32_t hw_format;         /* hw format */
	uint64_t hw_rb_phys;        /* phys addr of LP/MIC RB */
	uint32_t hw_rb_size;
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
	struct open_req_msg open_req;
	struct open_resp_msg open_resp;
	struct dsp_config_req_msg dsp_config;
	struct dsp_status_msg dsp_status;
};

struct rpmsg_packet {
	struct rpmsg_hdr header;
	union rpmsg_payload payload;
} __attribute__((packed));

#endif /* __DSP_PCM_PLATFORM_H */
