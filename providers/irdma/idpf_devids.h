/* SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB */
/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2023 Intel Corporation */

#ifndef _IDPF_DEVIDS_H_
#define _IDPF_DEVIDS_H_

/* Device IDs common to emr, silicon and simics */
#define IDPF_DEV_ID_PF			0x1452
#define IAVF_DEV_ID_VF			0x145C
#ifdef SIOV_SUPPORT
#define IAVF_DEV_ID_VF_SIOV		0x0DD5
#endif /* SIOV_SUPPORT */

#define IDPF_DEV_ID_PF_SIMICS		0xF002
#define IAVF_DEV_ID_VF_SIMICS		0xF00C

#endif /* _IDPF_DEVIDS_H_ */
