// SPDX-License-Identifier: GPL-2.0 or Linux-OpenIB
/* Copyright (C) 2026 Google Corporation */
#include "bazel_build/init/init.h"

void irdma_driver_registration_module_initializer(void);

void irdma_init_userspace_driver(void) {
  irdma_driver_registration_module_initializer();
}
