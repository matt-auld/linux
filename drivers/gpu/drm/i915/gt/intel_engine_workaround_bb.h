/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2014 Intel Corporation
 */

#ifndef __INTEL_ENGINE_WORKAROUND_BB_H__
#define __INTEL_ENGINE_WORKAROUND_BB_H__

struct intel_engine_cs;

int intel_init_workaround_bb(struct intel_engine_cs *engine);
void intel_fini_workaround_bb(struct intel_engine_cs *engine);

#endif /* __INTEL_ENGINE_WORKAROUND_BB_H__ */
