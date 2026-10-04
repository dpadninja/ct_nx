#ifndef __FUNC_PROF_H__
#define __FUNC_PROF_H__

#include <stdint.h>
#include "so_util.h"

// DIAGNOSTIC (FUNC_PROFILER in config.h): see func_prof.c
void apply_func_prof(so_module *mod);
void prof_frame(uint64_t render_ticks);

#endif
