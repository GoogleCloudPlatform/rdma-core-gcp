// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2026 Google Corporation */
#ifndef RDMA_CORE_BAZEL_BUILD_HEADERS_CONFIG_H_
#define RDMA_CORE_BAZEL_BUILD_HEADERS_CONFIG_H_

/* This is the stuff that a configure script would have generated for us. */

#define STREAM_CLOEXEC "e"

#define HAVE_STATEMENT_EXPR 1
#define HAVE_BUILTIN_TYPES_COMPATIBLE_P 1
#define HAVE_TYPEOF 1
#define HAVE_ISBLANK 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CLZL 1
#define HAVE_FUNC_ATTRIBUTE_ALWAYS_INLINE 1
#define HAVE_FUNC_ATTRIBUTE_IFUNC 1
#define HAVE_LITTLE_ENDIAN 1
#define SIZEOF_LONG __SIZEOF_LONG__

#define PACKAGE_VERSION "64.0"

#define RDMA_CDEV_DIR "/dev/infiniband"
#define IBVERBS_PABI_VERSION 64
#define VERBS_IOCTL_ONLY 0
#define VERBS_WRITE_ONLY 0

#define VERBS_PROVIDER_SUFFIX ""
#define VERBS_PROVIDER_DIR ""
#define IBV_CONFIG_DIR ""
#define IBDIAG_NODENAME_MAP_PATH ""

#define IBACM_PORT_FILE "/var/run/ibacm.port"
#define IBACM_SERVER_BASE "ibacm-unix.sock"
#define IBACM_SERVER_PATH "/var/run/ibacm.sock"

#if defined(__cplusplus) && !defined(typeof)
#define typeof __typeof__
#endif

#endif  /* RDMA_CORE_BAZEL_BUILD_HEADERS_CONFIG_H_ */
