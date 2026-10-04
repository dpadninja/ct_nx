#ifndef __SPRITE_POOL_H__
#define __SPRITE_POOL_H__

#include "so_util.h"

// Reuse throw-away sprites at a few verified hot call sites (see sprite_pool.c).
void apply_sprite_pool(so_module *mod); // before so_finalize()
void sprite_pool_frame(void);           // once per frame, before rendering

#endif
