#ifndef __SPRITE_CENSUS_H__
#define __SPRITE_CENSUS_H__

#include <stdint.h>
#include "so_util.h"

// DIAGNOSTIC (SPRITE_CENSUS in config.h): see sprite_census.c
void apply_sprite_census(so_module *mod);
void census_frame(uint64_t render_ticks);

#endif
