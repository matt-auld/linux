/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2014 Intel Corporation
 */

#ifndef __GEN8_ENGINE_CS_H__
#define __GEN8_ENGINE_CS_H__

#include <linux/types.h>

struct i915_request;

int gen8_emit_flush_render(struct i915_request *request, u32 mode);
int gen8_emit_flush(struct i915_request *request, u32 mode);
int gen11_emit_flush_render(struct i915_request *request, u32 mode);
int gen12_emit_flush_render(struct i915_request *request, u32 mode);
int gen12_emit_flush(struct i915_request *request, u32 mode);

int gen8_emit_bb_start_noarb(struct i915_request *rq,
			     u64 offset, u32 len,
			     const unsigned int flags);
int gen8_emit_bb_start(struct i915_request *rq,
		       u64 offset, u32 len,
		       const unsigned int flags);

#endif /* __GEN8_ENGINE_CS_H__ */
