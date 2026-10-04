/* palette_cache.c -- share world-map palette textures (config: cache_palettes)
 *
 * WHY
 * World-map objects (the Black Omen, Ozzie's fort, the Epoch, ...) are drawn
 * with a palette shader. WorldObject::setColor() (0x63f044 in libchrono 2.1.5),
 * called for every such object every frame, builds a 256x1 RGBA palette from
 * the object's 15 colours, creates a brand-new Texture2D for it (glTexImage2D
 * upload), binds it as the shader's u_texture1 and lets the autorelease pool
 * destroy it at the end of the frame. Near a big object that's ~65 texture
 * creations + destructions per frame: the profiler measured ~14 ms of
 * Texture2D::initWithData plus ~2.4 ms of ~Texture2D per frame at stock clocks.
 *
 * WHAT THIS DOES
 * setColor's GOT slot points at palette_set_color(), a line-for-line
 * re-implementation that looks the palette up in a cache (keyed by the exact
 * 60 bytes of colour data the original copies) and creates a texture only for
 * palettes it hasn't seen. Everything else is identical: same texture contents,
 * format, size and filtering, same uniform, same setGLProgramState and
 * setScale(2, 2) calls in the same order, through the same virtual slots.
 * Textures evicted from the cache are released at the start of the next frame,
 * i.e. after the frame that may still draw with them -- the same lifetime an
 * autoreleased texture had.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "palette_cache.h"

#define BUCKETS 1024 // direct-mapped; a miss just replaces the bucket's entry
#define KEY_LEN 60   // the original copies colours 1..15 (bytes 4..63)

typedef struct { float w, h; } Size;
typedef struct { uint8_t key[KEY_LEN]; uint8_t used; void *tex; } Entry;

static Entry s_cache[BUCKETS];
static void **s_pending;   // evicted textures, released next frame
static int s_npending, s_cappending;
static int s_active;

static void *(*cxx_new)(size_t);
static void  (*Texture2D_ctor)(void *);
static int   (*Texture2D_initWithData)(void *, const void *, long, int, int, int, const Size *);
static void  (*Texture2D_setAlias)(void *);
static void  (*Ref_retain)(void *);
static void  (*Ref_release)(void *);
static void  (*GLProgramState_setUniformTexture)(void *, const void *, void *);

static uint32_t hash_key(const uint8_t *k) {
  uint32_t h = 2166136261u; // FNV-1a
  for (int i = 0; i < KEY_LEN; i++) { h ^= k[i]; h *= 16777619u; }
  return h;
}

static void defer_release(void *tex) {
  if (s_npending == s_cappending) {
    const int n = s_cappending ? s_cappending * 2 : 64;
    void **p = realloc(s_pending, n * sizeof(void *));
    if (!p) { Ref_release(tex); return; } // out of memory: best effort
    s_pending = p; s_cappending = n;
  }
  s_pending[s_npending++] = tex;
}

static void *make_palette_texture(const uint8_t *pal) {
  // identical to the original: 256 RGBA entries, zeroed; 8 blocks of 16
  // colours, each block = transparent black followed by pal[1..15]
  uint8_t data[1024];
  memset(data, 0, sizeof(data));
  for (int b = 0; b < 8; b++)
    memcpy(data + b * 64 + 4, pal + 4, KEY_LEN);
  void *tex = cxx_new(0x88); // sizeof(cocos2d::Texture2D) in this build
  Texture2D_ctor(tex);
  const Size sz = { 256.f, 1.f };
  if (Texture2D_initWithData(tex, data, 1024, 2 /* RGBA8888 */, 256, 1, &sz) & 0xff)
    Texture2D_setAlias(tex);
  return tex; // refcount 1, owned by the cache
}

// WorldObject: +0x0 cocos2d::Sprite*, +0x8 cocos2d::GLProgramState*
static void palette_set_color(void *self, const uint8_t *pal) {
  const uint32_t b = hash_key(pal + 4) & (BUCKETS - 1);
  Entry *e = &s_cache[b];
  if (!e->used || memcmp(e->key, pal + 4, KEY_LEN) != 0) {
    if (e->used) defer_release(e->tex);
    memcpy(e->key, pal + 4, KEY_LEN);
    e->tex = make_palette_texture(pal);
    e->used = 1;
  }

  // libc++ short std::string "u_texture1" (size<<1 in byte 0), as the original
  uint8_t name[24] = { 10 << 1, 'u', '_', 't', 'e', 'x', 't', 'u', 'r', 'e', '1', 0 };
  void *sprite = ((void **)self)[0];
  void *state  = ((void **)self)[1];
  GLProgramState_setUniformTexture(state, name, e->tex);

  void **vt = *(void ***)sprite;
  ((void (*)(void *, void *))vt[0x310 / 8])(sprite, state);          // setGLProgramState
  ((void (*)(void *, float, float))vt[0x90 / 8])(sprite, 2.f, 2.f); // setScale(2, 2)
}

void palette_cache_frame(void) {
  if (!s_active) return;
  for (int i = 0; i < s_npending; i++) Ref_release(s_pending[i]);
  s_npending = 0;
}

static uintptr_t *got_slot(so_module *mod, const char *name) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh_name = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (strcmp(sh_name, ".rela.plt") != 0) continue;
    Elf64_Rela *rels = (Elf64_Rela *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    const size_t cnt = mod->sec_hdr[i].sh_size / sizeof(Elf64_Rela);
    for (size_t j = 0; j < cnt; j++) {
      if (ELF64_R_TYPE(rels[j].r_info) != R_AARCH64_JUMP_SLOT) continue;
      const Elf64_Sym *sym = &mod->syms[ELF64_R_SYM(rels[j].r_info)];
      if (strcmp(mod->dynstrtab + sym->st_name, name) == 0)
        return (uintptr_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    }
  }
  return NULL;
}

#define RESOLVE(var, sym) \
  do { *(uintptr_t *)&(var) = so_try_find_addr_rx(mod, sym); \
       if (!(var)) { debugPrintf("palette_cache: missing %s, not applied\n", sym); return; } } while (0)

void apply_palette_cache(so_module *mod) {
  if (!config.cache_palettes) return;

  static const char *SYM_SETCOLOR = "_ZN11WorldObject8setColorEPN7cocos2d7Color4BE";
  const uintptr_t fn = so_try_find_addr_rx(mod, SYM_SETCOLOR);
  if (!fn) return;
  // only for the libchrono 2.1.5 setColor this re-implements
  const uintptr_t off = fn - (uintptr_t)mod->load_virtbase;
  const uint32_t *code = (const uint32_t *)((uintptr_t)mod->load_base + off);
  if (off != 0x63f044 || code[0] != 0xa9bb7bfdu ||
      code[(0x63f2d4 - 0x63f044) / 4] != 0x9413ff63u ||   // bl Texture2D::initWithData
      code[(0x63f31c - 0x63f044) / 4] != 0x9413ff59u) {   // bl setUniformTexture
    debugPrintf("palette_cache: unexpected WorldObject::setColor, not applied\n");
    return;
  }

  RESOLVE(Texture2D_ctor, "_ZN7cocos2d9Texture2DC1Ev");
  RESOLVE(Texture2D_initWithData, "_ZN7cocos2d9Texture2D12initWithDataEPKvlNS0_11PixelFormatEiiRKNS_4SizeE");
  RESOLVE(Texture2D_setAlias, "_ZN7cocos2d9Texture2D21setAliasTexParametersEv");
  RESOLVE(Ref_retain, "_ZN7cocos2d3Ref6retainEv");
  RESOLVE(Ref_release, "_ZN7cocos2d3Ref7releaseEv");
  RESOLVE(GLProgramState_setUniformTexture,
          "_ZN7cocos2d14GLProgramState17setUniformTextureERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEEPNS_9Texture2DE");
  uintptr_t *new_slot = got_slot(mod, "_Znwm"); // operator new, imported from libc++_shared
  if (!new_slot || !*new_slot) { debugPrintf("palette_cache: operator new not resolved, not applied\n"); return; }
  cxx_new = (void *(*)(size_t))*new_slot;

  uintptr_t *slot = got_slot(mod, SYM_SETCOLOR);
  if (!slot) { debugPrintf("palette_cache: setColor GOT slot not found, not applied\n"); return; }
  *slot = (uintptr_t)&palette_set_color;
  s_active = 1;
  debugPrintf("palette_cache: applied\n");
}
