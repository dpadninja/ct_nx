/* func_prof.c -- DIAGNOSTIC: per-function CPU time of the game code
 *
 * Almost every function in libchrono is exported and called through the PLT,
 * so its GOT slot can be pointed at a timing stub (prof_stubs.s). This hooks
 * the game's own classes (world map, field, battle, SNES-engine layer, ...)
 * plus a few top-level cocos2d entry points, measures them on the main thread
 * only, and every ~5 s appends to prof.log:
 *   - render ms/frame (the whole engine frame),
 *   - the functions with the most SELF time (time in the function minus time
 *     in hooked functions it called), and
 *   - the functions with the most INCLUSIVE time.
 * Names are the raw mangled C++ names; demangle offline with c++filt.
 *
 * Enabled only when FUNC_PROFILER is 1 in config.h. Adds some overhead per
 * hooked call, so absolute numbers are a bit high; compare, don't trust ms
 * to the decimal.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <elf.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "func_prof.h"

#if FUNC_PROFILER

#define PROF_MAX 3072  // must match the .rept count in prof_stubs.s
#define STACK_MAX 512

extern char prof_stub_base[];
uintptr_t prof_orig[PROF_MAX]; // read by prof_stubs.s

// mangled-name prefixes (after "_ZN" / "_ZNK") of the classes to profile
static const char *const k_prefixes[] = {
  "9WorldImpl", "8WorldMap", "18WorldObjectManager", "11WorldObject", "5world",
  "9FieldImpl", "8FieldMap", "8MapTable", "9Character", "8EffectBg",
  "12BattleRender", "10BattleMenu", "11SceneBattle", "6Battle",
  "16SpecialEventImpl", "8cSfcWork", "3Asm", "9MsgWindow", "9SoundWork",
  "12SoundManager", "3ctr",
  // cocos2d pieces that can hide per-frame work
  "7cocos2d9Texture2D", "7cocos2d13RenderTexture", "7cocos2d8DrawNode",
  "7cocos2d14GLProgramState",
};
static const char *const k_exact[] = {
  "_ZN7cocos2d8Director9drawSceneEv",
  "_ZN7cocos2d9Scheduler6updateEf",
  "_ZN7cocos2d8Renderer6renderEv",
  "_ZN7cocos2d15AutoreleasePool5clearEv",
  "_ZN7cocos2d8Director8mainLoopEv",
};

typedef struct { uint64_t calls, self, incl; } Stat;
typedef struct { uint32_t id; uint64_t start, child; } Frame;

static char    *s_names[PROF_MAX];
static Stat     s_stat[PROF_MAX];
static uint16_t s_rec[PROF_MAX];  // recursion depth per function
static Frame    s_stack[STACK_MAX];
static int      s_depth;
static int      s_count;
static Handle   s_main;
static int      s_active;
static uint64_t s_render, s_t0;
static uint32_t s_frames;

uint64_t prof_enter(uint32_t id) {
  if (!s_active || threadGetCurHandle() != s_main || s_depth >= STACK_MAX)
    return 0;
  const uint64_t t = armGetSystemTick();
  Frame *f = &s_stack[s_depth++];
  f->id = id; f->start = t; f->child = 0;
  s_rec[id]++;
  return t ? t : 1;
}

void prof_leave(uint64_t start, uint32_t id) {
  if (!start || s_depth <= 0) return;
  const uint64_t now = armGetSystemTick();
  Frame *f = &s_stack[--s_depth];
  const uint64_t dt = now - f->start;
  Stat *s = &s_stat[f->id];
  s->calls++;
  s->self += dt > f->child ? dt - f->child : 0;
  if (s_rec[f->id] == 1) s->incl += dt;
  if (s_rec[f->id]) s_rec[f->id]--;
  if (s_depth > 0) s_stack[s_depth - 1].child += dt;
  (void)id;
}

static int wanted(const char *name) {
  for (size_t i = 0; i < sizeof(k_exact) / sizeof(k_exact[0]); i++)
    if (strcmp(name, k_exact[i]) == 0) return 1;
  const char *body = NULL;
  if (strncmp(name, "_ZNK", 4) == 0) body = name + 4;
  else if (strncmp(name, "_ZN", 3) == 0) body = name + 3;
  if (!body) return 0;
  for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++)
    if (strncmp(body, k_prefixes[i], strlen(k_prefixes[i])) == 0) return 1;
  return 0;
}

void apply_func_prof(so_module *mod) {
  s_main = threadGetCurHandle();
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.plt") != 0) continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    const size_t cnt = mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < cnt && s_count < PROF_MAX; j++) {
      if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT) continue;
      const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      if (sym->st_shndx == SHN_UNDEF) continue; // only libchrono's own functions
      const char *name = mod->dynstrtab + sym->st_name;
      if (!wanted(name)) continue;
      uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
      prof_orig[s_count] = *slot;
      s_names[s_count] = strdup(name); // load_base becomes unreadable after so_finalize
      *slot = (uintptr_t)prof_stub_base + (uintptr_t)s_count * 256;
      s_count++;
    }
  }
  s_active = 1;
  debugPrintf("func_prof: hooked %d functions\n", s_count);
}

static Stat s_snap[PROF_MAX];
static int s_order[PROF_MAX];
static int cmp_self(const void *a, const void *b) {
  const uint64_t x = s_snap[*(const int *)a].self, y = s_snap[*(const int *)b].self;
  return (x < y) - (x > y);
}
static int cmp_incl(const void *a, const void *b) {
  const uint64_t x = s_snap[*(const int *)a].incl, y = s_snap[*(const int *)b].incl;
  return (x < y) - (x > y);
}

void prof_frame(uint64_t render_ticks) {
  if (!s_active) return;
  const uint64_t now = armGetSystemTick();
  if (!s_t0) s_t0 = now;
  s_render += render_ticks;
  s_frames++;
  if (now - s_t0 < 5ull * armGetSystemTickFreq()) return;

  memcpy(s_snap, s_stat, sizeof(Stat) * s_count);
  const double tpm = (double)armGetSystemTickFreq() / 1000.0, fr = (double)s_frames;
  FILE *f = fopen("prof.log", "a");
  if (f) {
    fprintf(f, "== frames=%u render=%.2fms/f  (self/incl in ms per frame)\n", s_frames, s_render / tpm / fr);
    for (int i = 0; i < s_count; i++) s_order[i] = i;
    qsort(s_order, s_count, sizeof(int), cmp_self);
    fprintf(f, " -- top self --\n");
    for (int k = 0; k < s_count && k < 40; k++) {
      const Stat *s = &s_snap[s_order[k]];
      if (!s->calls) break;
      fprintf(f, "  self %7.3f  incl %7.3f  calls %8.1f  %s\n", s->self / tpm / fr,
              s->incl / tpm / fr, s->calls / fr, s_names[s_order[k]]);
    }
    qsort(s_order, s_count, sizeof(int), cmp_incl);
    fprintf(f, " -- top inclusive --\n");
    for (int k = 0; k < s_count && k < 25; k++) {
      const Stat *s = &s_snap[s_order[k]];
      if (!s->calls) break;
      fprintf(f, "  incl %7.3f  self %7.3f  calls %8.1f  %s\n", s->incl / tpm / fr,
              s->self / tpm / fr, s->calls / fr, s_names[s_order[k]]);
    }
    fclose(f);
  }
  memset(s_stat, 0, sizeof(Stat) * s_count);
  s_render = 0; s_frames = 0; s_t0 = now;
}

#else
// prof_stubs.s is always assembled; give its references something to bind to.
uintptr_t prof_orig[3072];
uint64_t prof_enter(uint32_t id) { (void)id; return 0; }
void prof_leave(uint64_t start, uint32_t id) { (void)start; (void)id; }
#endif /* FUNC_PROFILER */
