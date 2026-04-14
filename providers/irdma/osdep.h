/* SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB */
/* Copyright (c) 2015 - 2025 Intel Corporation */
#ifndef IRDMA_OSDEP_H
#define IRDMA_OSDEP_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <util/udma_barrier.h>
#include <ccan/minmax.h>
#include <util/util.h>
#include <util/compiler.h>
#include <linux/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <endian.h>
#include <errno.h>
#include <util/mmio.h>
#include <infiniband/verbs.h>
extern unsigned int irdma_dbg;
#define libirdma_debug(fmt, args...)					\
do {									\
	if (irdma_dbg)							\
		fprintf(stderr, "libirdma-%s: " fmt, __func__, ##args);	\
} while (0)
#ifndef BIT
#define BIT(nr) (1UL << (nr))
#endif
#ifndef BITS_PER_LONG
#define BITS_PER_LONG (8 * sizeof(long))
#endif
#ifndef GENMASK
#define GENMASK(h, l) \
	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#endif
#ifndef BIT_ULL
#define BIT_ULL(nr) (1ULL << (nr))
#endif
#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG (8 * sizeof(long long))
#endif
#ifndef GENMASK_ULL
#define GENMASK_ULL(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (BITS_PER_LONG_LONG - 1 - (h))))
#endif
#ifndef FIELD_PREP

/* Compat for rdma-core-27.0 and OFED 4.8/RHEL 7.2. Not for UPSTREAM */
#define __bf_shf(x) (__builtin_ffsll(x) - 1)
#define FIELD_PREP(_mask, _val)                                                \
	({                                                                     \
		((typeof(_mask))(_val) << __bf_shf(_mask)) & (_mask);          \
	})

#define FIELD_GET(_mask, _reg)                                                 \
	({                                                                     \
		(typeof(_mask))(((_reg) & (_mask)) >> __bf_shf(_mask));        \
	})
#endif /* FIELD_PREP */
#endif /* IRDMA_OSDEP_H */
