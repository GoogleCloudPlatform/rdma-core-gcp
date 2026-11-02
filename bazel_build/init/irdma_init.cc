// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2026 Google Corporation */
#include "bazel_build/init/init.h"

static void __attribute__((constructor)) InitLibIrdmaSubsystem() {
  irdma_init_userspace_driver();
}
