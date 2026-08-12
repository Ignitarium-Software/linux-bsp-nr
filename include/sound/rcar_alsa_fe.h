// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Renesas Electronics Corporation
 */

#ifndef __DSP_PCM_PLATFORM_H
#define __DSP_PCM_PLATFORM_H

#define DSP_CTRL_RPMSG_VERSION (0x1U)
#define CR_CTRL_RPMSG_VERSION  (0x1U)

#define PERIODS_MIN         (4)
#define BUFFER_LEN_MAX      (16384U)    /* 16kB */

/* Init message from CA to remote core */
#define RPMSG_INIT          (0x3001)

/* CR Control Message Types */
#define OPEN_REQ            (0x1001) /* CA -> CR */
#define TRIGGER_START       (0x1002) /* CA -> CR */
#define TRIGGER_PAUSE       (0x1003) /* CA -> CR */
#define TRIGGER_RESUME      (0x1004) /* CA -> CR */
#define TRIGGER_STOP        (0x1005) /* CA -> CR */
#define TRIGGER_CLOSE       (0x1006) /* CA -> CR */
#define POS_QUERY_CR        (0x1007) /* CA -> CR */
#define OPEN_RESP           (0x9001) /* CR -> CA */
#define EVENT_STARTED       (0x9002) /* CR -> CA */
#define EVENT_PAUSED        (0x9003) /* CR -> CA */
#define EVENT_RESUMED       (0x9004) /* CR -> CA */
#define EVENT_STOPPED       (0x9005) /* CR -> CA */
#define EVENT_CLOSED        (0x9006) /* CR -> CA */
#define POS_REPLY_CR        (0x9007) /* CR -> CA */
#define EVENT_XRUN          (0x9011) /* CR -> CA */
#define EVENT_HP_TAKEOVER   (0x9012) /* CR -> CA */
#define EVENT_HP_RELEASED   (0x9013) /* CR -> CA */
#define EVENT_ERROR         (0x9014) /* CR -> CA */

/* DSP Control Message type */
#define CONFIG_REQ          (0x2001) /* CA -> DSP */
#define PCM_START           (0x2002) /* CA -> DSP */
#define PCM_PAUSE           (0x2003) /* CA -> DSP */
#define PCM_RESUME          (0x2004) /* CA -> DSP */
#define PCM_STOP            (0x2005) /* CA -> DSP */
#define PCM_CLOSE           (0x2006) /* CA -> DSP */
#define POS_QUERY           (0x2007) /* CA -> DSP */
#define CONFIG_REPLY        (0xA001) /* DSP -> CA */
#define PCM_STARTED         (0xA002) /* DSP -> CA */
#define PCM_PAUSED          (0xA003) /* DSP -> CA */
#define PCM_RESUMED         (0xA004) /* DSP -> CA */
#define PCM_STOPED          (0xA005) /* DSP -> CA */
#define PCM_CLOSED          (0xA006) /* DSP -> CA */
#define POS_REPLY           (0xA007) /* DSP -> CA */
#define PCM_STATUS          (0x8000) /* DSP -> CA */

/* status */
#define STATUS_SUCCESS      (0)

/* HW Audio formats */
#define FORMAT_S8           (1U << 0) /* SNDRV_PCM_FORMAT_S8 */
#define FORMAT_U8           (1U << 1) /* SNDRV_PCM_FORMAT_U8 */
#define FORMAT_S16_LE       (1U << 2) /* SNDRV_PCM_FORMAT_S16_LE */
#define FORMAT_S16_BE       (1U << 3) /* SNDRV_PCM_FORMAT_S16_BE */
#define FORMAT_U16_LE       (1U << 4) /* SNDRV_PCM_FORMAT_U16_LE */
#define FORMAT_U16_BE       (1U << 5) /* SNDRV_PCM_FORMAT_U16_BE */
#define FORMAT_S24_LE       (1U << 6) /* SNDRV_PCM_FORMAT_S24_LE */
#define FORMAT_S24_BE       (1U << 7) /* SNDRV_PCM_FORMAT_S24_BE */
#define FORMAT_U24_LE       (1U << 6) /* SNDRV_PCM_FORMAT_U24_LE */
#define FORMAT_U24_BE       (1U << 7) /* SNDRV_PCM_FORMAT_U24_BE */

/* HW Audio rates */
#define RATE_48000          (48000)
#define RATE_44100          (44100)

/* Audio stream direction */
#define DIR_PLAYBACK        (0)
#define DIR_CAPTURE         (1)

/* bit mask for reply msg_type */
#define REPLY_MSG_MASK      (0x8000)

/* remote processor type */
#define REMOTE_CR           (0)
#define REMOTE_DSP          (1)

struct rpmsg_hdr {
	uint16_t version;
	uint16_t msg_type;
	uint32_t msg_id;
	uint32_t stream_id;
	uint32_t payload_len;
} __attribute__((packed));

struct trigger_req {
	uint32_t cmd;               /* START/STOP/PAUSE/RESUME/CLOSE */
} __attribute__((packed));

struct remote_resp {
	uint32_t status;            /* 0 for success, -eerno for failure */
} __attribute__((packed));

struct open_req_msg {
	uint32_t dir;               /* playback=0, capture=1 */
} __attribute__((packed));

struct open_resp_msg {
	uint32_t status;            /* 0 for success, -eerno for failure */
	uint32_t stream_id;
	uint32_t hw_rate;           /* hw rate */
	uint32_t hw_channels;
	uint32_t hw_format;         /* hw format */
	uint64_t hw_rb_phys;        /* phys addr of LP/MIC RB */
	uint32_t hw_rb_size;
	uint32_t hw_period_frames;  /* number of frames in 1 period */
	uint32_t hw_period_bytes;   /* number of bytes in 1 period */
	uint32_t hw_periods;
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
	uint32_t period_frames;
	uint32_t period_bytes;
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
	struct remote_resp resp;
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
