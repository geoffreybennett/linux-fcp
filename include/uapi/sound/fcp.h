/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Focusrite Control Protocol Driver for ALSA
 *
 * Copyright (c) 2024 by Geoffrey D. Bennett <g at b4.vu>
 */
#ifndef __UAPI_SOUND_FCP_H
#define __UAPI_SOUND_FCP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#include <linux/ioctl.h>

#define FCP_HWDEP_MAJOR 2
#define FCP_HWDEP_MINOR 0
#define FCP_HWDEP_SUBMINOR 0

#define FCP_HWDEP_VERSION \
	((FCP_HWDEP_MAJOR << 16) | \
	 (FCP_HWDEP_MINOR << 8) | \
	  FCP_HWDEP_SUBMINOR)

#define FCP_HWDEP_VERSION_MAJOR(v) (((v) >> 16) & 0xFF)
#define FCP_HWDEP_VERSION_MINOR(v) (((v) >> 8) & 0xFF)
#define FCP_HWDEP_VERSION_SUBMINOR(v) ((v) & 0xFF)

/* Get protocol version */
#define FCP_IOCTL_PVERSION _IOR('S', 0x60, int)

/* Do FCP step 0 */
struct fcp_step0 {
	void     *data;
	uint16_t  size;
};
#define FCP_IOCTL_INIT _IOWR('S', 0x64, struct fcp_step0)

/* Perform a command */
struct fcp_cmd {
	uint32_t    opcode;
	uint16_t    req_size;
	uint16_t    resp_size;
	const void *req;
	void       *resp;
};
#define FCP_IOCTL_CMD _IOWR('S', 0x65, struct fcp_cmd)

/* Set the meter map */
struct fcp_meter_map {
	const int16_t *map;
	uint16_t       map_size;
	uint16_t       meter_slots;
};
#define FCP_IOCTL_SET_METER_MAP _IOW('S', 0x66, struct fcp_meter_map)

#endif /* __UAPI_SOUND_FCP_H */
