// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2026 Google Corporation */
#ifndef RDMA_CORE_BAZEL_BUILD_HEADERS_VALGRIND_MEMCHECK_H_
#define RDMA_CORE_BAZEL_BUILD_HEADERS_VALGRIND_MEMCHECK_H_

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if __has_feature(memory_sanitizer)
#include <sanitizer/msan_interface.h>
#define VALGRIND_MAKE_MEM_DEFINED(p, s) __msan_unpoison(p, s)
#else
#define VALGRIND_MAKE_MEM_DEFINED(p, s) (0)
#endif

#endif  /* RDMA_CORE_BAZEL_BUILD_HEADERS_VALGRIND_MEMCHECK_H_ */
