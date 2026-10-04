/* sprite_census.c -- DIAGNOSTIC: count per-frame cocos2d object creation by call site
 *
 * The engine draws most things in "immediate mode": it creates a brand-new
 * Sprite (or Label, DrawNode...) for every element it draws, visits it once
 * and lets the autorelease pool destroy it at the end of the frame. That's
 * what made the Epoch cloud layer cost ~40 ms/frame (see tenkumo_pool.c).
 * This census finds the other hot spots: every create function libchrono
 * calls through its PLT is routed through a stub (census_stubs.s) that counts
 * calls per return address. Every ~5 s the busiest call sites are appended to
 * census.log as "libchrono offset of the bl instruction: calls/frame".
 *
 * Enabled only when SPRITE_CENSUS is set to 1 in config.h.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "sprite_census.h"

#if SPRITE_CENSUS

static const char *const k_syms[] = {
  "_ZN7cocos2d6Sprite17createWithTextureEPNS_9Texture2DERKNS_4RectEb",
  "_ZN7cocos2d6Sprite17createWithTextureEPNS_9Texture2DE",
  "_ZN7cocos2d6Sprite21createWithSpriteFrameEPNS_11SpriteFrameE",
  "_ZN7cocos2d6Sprite25createWithSpriteFrameNameERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE",
  "_ZN7cocos2d6Sprite6createEv",
  "_ZN7cocos2d6Sprite6createERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE",
  "_ZN7cocos2d11SpriteFrame17createWithTextureEPNS_9Texture2DERKNS_4RectE",
  "_ZN7cocos2d11SpriteFrame17createWithTextureEPNS_9Texture2DERKNS_4RectEbRKNS_4Vec2ERKNS_4SizeE",
  "_ZN7cocos2d13RenderTexture6createEii",
  "_ZN7cocos2d13RenderTexture6createEiiNS_9Texture2D11PixelFormatEj",
  "_ZN7cocos2d8DrawNode6createEf",
  "_ZN7cocos2d4Node6createEv",
  "_ZN7cocos2d10LayerColor6createEv",
  "_ZN7cocos2d10LayerColor6createERKNS_7Color4BE",
  "_ZN7cocos2d10LayerColor6createERKNS_7Color4BEff",
  "_ZN7cocos2d5Label6createEv",
  "_ZN7cocos2d5Label20createWithSystemFontERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_fRKNS_4SizeENS_14TextHAlignmentENS_14TextVAlignmentE",
  "_ZN7cocos2d5Label25createSpriteForSystemFontERKNS_14FontDefinitionE",
  "_ZN7cocos2d5Label31createShadowSpriteForSystemFontERKNS_14FontDefinitionE",
  "_ZN7cocos2d2ui12Scale9Sprite6createEv",
  "_ZN7cocos2d2ui12Scale9Sprite6createERKNSt6__ndk112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE",
  "_ZN7cocos2d2ui12Scale9Sprite25createWithSpriteFrameNameERKNSt6__ndk112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEE",
  "_ZN7cocos2d2ui12Scale9Sprite25createWithSpriteFrameNameERKNSt6__ndk112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEERKNS_4RectE",
  "_ZN7cocos2d15AutoreleasePool5clearEv", // end-of-frame destruction of everything above
};
static const char *const k_short[] = {
  "Sprite::createWithTexture(tex,rect)", "Sprite::createWithTexture(tex)",
  "Sprite::createWithSpriteFrame", "Sprite::createWithSpriteFrameName",
  "Sprite::create()", "Sprite::create(file)",
  "SpriteFrame::createWithTexture", "SpriteFrame::createWithTexture(5)",
  "RenderTexture::create(w,h)", "RenderTexture::create(w,h,fmt,ds)",
  "DrawNode::create", "Node::create", "LayerColor::create()",
  "LayerColor::create(c)", "LayerColor::create(c,w,h)", "Label::create",
  "Label::createWithSystemFont", "Label::createSpriteForSystemFont",
  "Label::createShadowSpriteForSystemFont", "Scale9Sprite::create()",
  "Scale9Sprite::create(file)", "Scale9Sprite::createWithSpriteFrameName",
  "Scale9Sprite::createWithSpriteFrameName(r)",
  "AutoreleasePool::clear (frame-end frees)",
};
#define NUM_HOOKS ((int)(sizeof(k_syms) / sizeof(k_syms[0])))

uintptr_t census_orig[24]; // read by census_stubs.s
#define STUB(n) extern void census_stub_##n(void);
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5) STUB(6) STUB(7) STUB(8) STUB(9)
STUB(10) STUB(11) STUB(12) STUB(13) STUB(14) STUB(15) STUB(16) STUB(17) STUB(18)
STUB(19) STUB(20) STUB(21) STUB(22) STUB(23)
static void (*const k_stubs[24])(void) = {
  census_stub_0, census_stub_1, census_stub_2, census_stub_3, census_stub_4,
  census_stub_5, census_stub_6, census_stub_7, census_stub_8, census_stub_9,
  census_stub_10, census_stub_11, census_stub_12, census_stub_13, census_stub_14,
  census_stub_15, census_stub_16, census_stub_17, census_stub_18, census_stub_19,
  census_stub_20, census_stub_21, census_stub_22, census_stub_23,
};

#define TABLE_SIZE 4096 // power of two
typedef struct { uintptr_t lr; uint32_t id; uint32_t count; uint64_t ticks; } Site;
static Site s_sites[TABLE_SIZE];
static uint32_t s_per_id[24];
static uint64_t s_ticks_id[24];
static uint64_t s_render_ticks;
static int s_depth; // only count time of the outermost hooked call
static uintptr_t s_base;
static uint32_t s_frames;
static uint64_t s_t0;
static int s_active;

uint64_t census_enter(void) {
  s_depth++;
  return armGetSystemTick();
}

void census_leave(uint64_t start, uint32_t id, uintptr_t lr) {
  const uint64_t dt = armGetSystemTick() - start;
  s_depth--;
  const int outer = (s_depth == 0);
  if (id < 24) { s_per_id[id]++; if (outer) s_ticks_id[id] += dt; }
  uint32_t h = (uint32_t)((lr >> 2) * 2654435761u) & (TABLE_SIZE - 1);
  for (int probe = 0; probe < TABLE_SIZE; probe++, h = (h + 1) & (TABLE_SIZE - 1)) {
    if (s_sites[h].lr == lr) { s_sites[h].count++; s_sites[h].ticks += dt; return; }
    if (s_sites[h].lr == 0) { s_sites[h].lr = lr; s_sites[h].id = id; s_sites[h].count = 1; s_sites[h].ticks = dt; return; }
  }
}

// Returns the previous GOT value of the first matching slot, or 0.
static uintptr_t hook_got(so_module *mod, const char *name, uintptr_t dst) {
  uintptr_t prev = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.plt") != 0) continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    const size_t cnt = mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < cnt; j++) {
      if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT) continue;
      const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      if (strcmp(mod->dynstrtab + sym->st_name, name) != 0) continue;
      uintptr_t *slot = (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
      if (!prev) prev = *slot;
      *slot = dst;
    }
  }
  return prev;
}

void apply_sprite_census(so_module *mod) {
  s_base = (uintptr_t)mod->load_virtbase;
  int n = 0;
  for (int i = 0; i < NUM_HOOKS; i++) {
    // chain onto whatever is in the slot now (e.g. tenkumo_pool's hook)
    const uintptr_t prev = hook_got(mod, k_syms[i], (uintptr_t)k_stubs[i]);
    census_orig[i] = prev;
    if (prev) n++;
  }
  s_active = 1;
  debugPrintf("sprite_census: hooked %d/%d create functions\n", n, NUM_HOOKS);
}

static int cmp_sites(const void *a, const void *b) {
  const Site *x = a, *y = b;
  return (x->ticks < y->ticks) - (x->ticks > y->ticks);
}

void census_frame(uint64_t render_ticks) {
  if (!s_active) return;
  s_render_ticks += render_ticks;
  const u64 now = armGetSystemTick();
  if (!s_t0) s_t0 = now;
  s_frames++;
  if ((now - s_t0) < 5ull * armGetSystemTickFreq()) return;

  static Site top[TABLE_SIZE];
  int used = 0;
  for (int i = 0; i < TABLE_SIZE; i++)
    if (s_sites[i].lr) top[used++] = s_sites[i];
  qsort(top, used, sizeof(Site), cmp_sites);

  FILE *f = fopen("census.log", "a");
  if (f) {
    const double tpm = (double)armGetSystemTickFreq() / 1000.0; // ticks per ms
    const double fr = (double)s_frames;
    uint32_t total = 0; uint64_t tot_ticks = 0;
    for (int i = 0; i < 23; i++) { total += s_per_id[i]; tot_ticks += s_ticks_id[i]; }
    fprintf(f, "== frames=%u  render=%.2fms/f  creates=%.1f/f in %.2fms/f  frame-end frees=%.2fms/f\n",
            s_frames, s_render_ticks / tpm / fr, total / fr, tot_ticks / tpm / fr,
            s_ticks_id[23] / tpm / fr);
    for (int i = 0; i < NUM_HOOKS; i++)
      if (s_per_id[i])
        fprintf(f, "   %-44s %7.1f/f %7.3fms/f\n", k_short[i], s_per_id[i] / fr, s_ticks_id[i] / tpm / fr);
    for (int i = 0; i < used && i < 30; i++) {
      // lr points after the bl/blr; report the call instruction's offset
      fprintf(f, "   site 0x%06lx  %-40s %7.1f/f %7.3fms/f  %5.1fus/call\n",
              (unsigned long)(top[i].lr - 4 - s_base), k_short[top[i].id],
              top[i].count / fr, top[i].ticks / tpm / fr,
              top[i].ticks / tpm * 1000.0 / top[i].count);
    }
    fclose(f);
  }
  memset(s_sites, 0, sizeof(s_sites));
  memset(s_per_id, 0, sizeof(s_per_id));
  memset(s_ticks_id, 0, sizeof(s_ticks_id));
  s_render_ticks = 0;
  s_frames = 0;
  s_t0 = now;
}

#else
// census_stubs.s is always assembled; give its references something to bind to.
uintptr_t census_orig[24];
uint64_t census_enter(void) { return 0; }
void census_leave(uint64_t start, uint32_t id, uintptr_t lr) { (void)start; (void)id; (void)lr; }
#endif /* SPRITE_CENSUS */
