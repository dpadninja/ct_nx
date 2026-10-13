/* settings_menu.c -- port options as extra pages of the in-game Settings menu
 * (config: settings_menu)
 *
 * WHY
 * Every option of this port lives in config.ini, which can only be edited on
 * a PC. The game's own Settings screen (nsMenu::MenuNodeConfig) is entirely
 * data-driven, so the port's options can be added to it as extra pages -- the
 * engine draws them, pages through them with L/R, moves the cursor, changes
 * values with left/right and shows each row's description, exactly like the
 * stock rows.
 *
 * HOW THE MENU IS BUILT (libchrono 2.1.5)
 * MenuNodeConfig::initialize() fills this+0x368, a
 * std::vector<std::vector<RowData>> (one inner vector per page), then calls
 * setupPager(node) once. setupPager sizes the pager from pages.size(), and
 * createPage / createPageButtons / setupPageStates / changeRowValue /
 * copySystemConfigValues / initializeCofigs all index pages[page][row]
 * generically. RowData (0xC0 bytes, libc++ NDK ABI):
 *   +0x00 int type             0 = two buttons side by side (options[0..1])
 *                              1 = one value with < > arrows (any count)
 *   +0x08 std::string label
 *   +0x20 std::string description (status bar while the row is focused)
 *   +0x38 std::vector<std::string> options
 *   +0x50 std::function<int()> getter      (__f_ at +0x70)
 *   +0x80 std::function<void(int)> setter  (__f_ at +0xA0)
 *   +0xB0 int current option index
 * Stock pages hold 3 rows (y = -66 - 60*row); the pager's page buttons sit at
 * y = -302, so 4 rows still fit above them.
 *
 * WHAT THIS DOES
 * setupPager's GOT slot points at hook_setup_pager(), which appends our pages
 * to this+0x368 and then calls the real setupPager. The RowData are built
 * here byte for byte: strings and vectors are allocated with the game's own
 * operator new (the menu's destructor frees them with its operator delete),
 * and each std::function is a small-buffer functor with a hand-made
 * libc++ __function::__base vtable. Its operator() reads / writes config and
 * rewrites config.ini on every change. Options the port only reads at boot
 * say "Applies after restart." in their description.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "settings_menu.h"

#define ROWDATA_SIZE   0xC0
#define PAGES_OFF      0x368 // MenuNodeConfig: std::vector<std::vector<RowData>>
#define ROWS_PER_PAGE  4
#define MAX_STOCK_PAGES 4    // sanity bound; stock has 2

enum { K_BOOL, K_STR };

typedef struct {
  const char *label;
  const char *desc;
  int kind;
  int restart;              // 1 = only read at boot
  void *field;              // int* / char*
  int nopts;                // unused for K_BOOL
  const char *const *names; // option captions
  const char *const *svals; // K_STR tokens
  size_t ssize;             // K_STR field size
} Row;

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *const s_lang_names[] = { "System", "English", "Japanese", "German", "Italian",
                                            "Spanish", "French", "Chinese (S)", "Chinese (T)", "Korean" };
static const char *const s_lang_vals[]  = { "default", "en", "ja", "de", "it", "es", "fr", "zh", "zh-Hant", "ko" };

#define BOOL_ROW(lbl, dsc, rst, var) \
  { lbl, dsc, K_BOOL, rst, &config.var, 2, NULL, NULL, 0 }
#define STR_ROW(lbl, dsc, rst, var, names, vals) \
  { lbl, dsc, K_STR, rst, config.var, COUNT(names), names, vals, sizeof(config.var) }

// Paged ROWS_PER_PAGE at a time, in this order.
static const Row s_rows[] = {
  STR_ROW("Language", "Game language. System = console language.", 1,
          language, s_lang_names, s_lang_vals),
  BOOL_ROW("Sharp Textures", "Pixel-perfect texture filtering.", 1, remove_bilinear_filter),
};
#define NUM_ROWS COUNT(s_rows)
#define NUM_PAGES ((NUM_ROWS + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE)

static const char *const s_bool_names[] = { "Off", "On" };

// ---------------------------------------------------------------------------
// option <-> config
// ---------------------------------------------------------------------------

static int row_get(int idx) {
  const Row *r = &s_rows[idx];
  switch (r->kind) {
    case K_BOOL:
      return *(int *)r->field ? 1 : 0;
    case K_STR:
      for (int i = 0; i < r->nopts; i++)
        if (!strcmp((const char *)r->field, r->svals[i])) return i;
      return 0;
  }
  return 0;
}

static void row_set(int idx, int opt) {
  const Row *r = &s_rows[idx];
  const int n = (r->kind == K_BOOL) ? 2 : r->nopts;
  if (opt < 0) opt = 0;
  if (opt >= n) opt = n - 1;
  switch (r->kind) {
    case K_BOOL:   *(int *)r->field = opt; break;
    case K_STR:    strlcpy((char *)r->field, r->svals[opt], r->ssize); break;
  }
  if (write_config(CONFIG_NAME) < 0)
    debugPrintf("settings_menu: could not write %s\n", CONFIG_NAME);
}

// ---------------------------------------------------------------------------
// libc++ (NDK) object layouts
// ---------------------------------------------------------------------------

static void *(*cxx_new)(size_t);
static void  (*cxx_delete)(void *);

// std::string: short = byte0 size<<1, chars from byte 1 (up to 22);
// long = { alloc|1, size, data }
static void cxx_string(uint8_t *s, const char *src) {
  const size_t n = strlen(src);
  memset(s, 0, 24);
  if (n <= 22) {
    s[0] = (uint8_t)(n << 1);
    memcpy(s + 1, src, n);
  } else {
    const size_t cap = (n + 16) & ~(size_t)15;
    char *p = cxx_new(cap);
    memcpy(p, src, n + 1);
    ((uintptr_t *)s)[0] = cap | 1;
    ((uintptr_t *)s)[1] = n;
    ((uintptr_t *)s)[2] = (uintptr_t)p;
  }
}

// std::function small-buffer functor: libc++ __function::__base vtable
//   0x00 ~__base  0x08 deleting dtor  0x10 __clone()  0x18 __clone(__base*)
//   0x20 destroy  0x28 destroy_deallocate  0x30 operator()  0x38 target
//   0x40 target_type
typedef struct { const void *const *vptr; uintptr_t row; } Functor;

static void  fn_nop(Functor *self) { (void)self; }
static void  fn_free(Functor *self) { cxx_delete(self); }
static void *fn_clone(const Functor *self) {
  Functor *f = cxx_new(sizeof(*f));
  *f = *self;
  return f;
}
static void  fn_clone_into(const Functor *self, Functor *dst) { *dst = *self; }
static void *fn_null(const Functor *self) { (void)self; return NULL; }
static int   fn_get(const Functor *self) { return row_get((int)self->row); }
static void  fn_set(const Functor *self, int *v) { row_set((int)self->row, *v); }

static const void *const s_get_vtbl[] = {
  fn_nop, fn_free, fn_clone, fn_clone_into, fn_nop, fn_free, fn_get, fn_null, fn_null,
};
static const void *const s_set_vtbl[] = {
  fn_nop, fn_free, fn_clone, fn_clone_into, fn_nop, fn_free, fn_set, fn_null, fn_null,
};

static void build_row(uint8_t *rd, int idx) {
  const Row *r = &s_rows[idx];
  memset(rd, 0, ROWDATA_SIZE);

  *(int *)rd = (r->kind == K_BOOL) ? 0 : 1;
  cxx_string(rd + 0x08, r->label);
  char desc[160];
  snprintf(desc, sizeof(desc), "%s%s", r->desc, r->restart ? " Applies after restart." : "");
  cxx_string(rd + 0x20, desc);

  const int n = (r->kind == K_BOOL) ? 2 : r->nopts;
  const char *const *names = (r->kind == K_BOOL) ? s_bool_names : r->names;
  uint8_t *opts = cxx_new((size_t)n * 24);
  for (int i = 0; i < n; i++) cxx_string(opts + i * 24, names[i]);
  uintptr_t *vec = (uintptr_t *)(rd + 0x38);
  vec[0] = (uintptr_t)opts;
  vec[1] = vec[2] = (uintptr_t)(opts + n * 24);

  *(Functor *)(rd + 0x50) = (Functor){ s_get_vtbl, (uintptr_t)idx };
  *(void **)(rd + 0x70) = rd + 0x50;
  *(Functor *)(rd + 0x80) = (Functor){ s_set_vtbl, (uintptr_t)idx };
  *(void **)(rd + 0xA0) = rd + 0x80;

  *(int *)(rd + 0xB0) = row_get(idx);
}

// ---------------------------------------------------------------------------
// hook
// ---------------------------------------------------------------------------

static void (*real_setup_pager)(void *self, void *node);

static void add_pages(uint8_t *self) {
  uintptr_t *pages = (uintptr_t *)(self + PAGES_OFF);
  const size_t n = (pages[1] - pages[0]) / 24;
  if (n < 1 || n > MAX_STOCK_PAGES) {
    debugPrintf("settings_menu: unexpected page count %zu, not added\n", n);
    return;
  }
  // setupPager can run more than once on the same menu: add our pages once
  for (size_t p = 0; p < n; p++) {
    const uintptr_t *page = (const uintptr_t *)(pages[0] + p * 24);
    if (page[0] != page[1] && ((Functor *)(page[0] + 0x50))->vptr == s_get_vtbl)
      return;
  }

  uint8_t *buf = cxx_new((n + NUM_PAGES) * 24);
  memcpy(buf, (void *)pages[0], n * 24); // libc++ vectors relocate bitwise
  for (int p = 0; p < NUM_PAGES; p++) {
    const int first = p * ROWS_PER_PAGE;
    const int count = (NUM_ROWS - first < ROWS_PER_PAGE) ? NUM_ROWS - first : ROWS_PER_PAGE;
    uint8_t *rows = cxx_new((size_t)count * ROWDATA_SIZE);
    for (int i = 0; i < count; i++) build_row(rows + i * ROWDATA_SIZE, first + i);
    uintptr_t *page = (uintptr_t *)(buf + (n + p) * 24);
    page[0] = (uintptr_t)rows;
    page[1] = page[2] = (uintptr_t)(rows + count * ROWDATA_SIZE);
  }
  if (pages[0]) cxx_delete((void *)pages[0]);
  pages[0] = (uintptr_t)buf;
  pages[1] = pages[2] = (uintptr_t)(buf + (n + NUM_PAGES) * 24);
}

static void hook_setup_pager(void *self, void *node) {
  add_pages(self);
  real_setup_pager(self, node);
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

void apply_settings_menu(so_module *mod) {
  if (!config.settings_menu) return;

  static const char *SYM_SETUP_PAGER = "_ZN6nsMenu14MenuNodeConfig10setupPagerEPN7cocos2d4NodeE";
  const uintptr_t fn = so_try_find_addr_rx(mod, SYM_SETUP_PAGER);
  // the RowData layout above is libchrono 2.1.5's
  if (!fn || fn - (uintptr_t)mod->load_virtbase != 0x6fef44) {
    debugPrintf("settings_menu: unexpected MenuNodeConfig::setupPager, not applied\n");
    return;
  }

  uintptr_t *new_slot = got_slot(mod, "_Znwm");
  uintptr_t *del_slot = got_slot(mod, "_ZdlPv");
  uintptr_t *slot = got_slot(mod, SYM_SETUP_PAGER);
  if (!new_slot || !*new_slot || !del_slot || !*del_slot || !slot) {
    debugPrintf("settings_menu: GOT slots not found, not applied\n");
    return;
  }
  cxx_new = (void *(*)(size_t))*new_slot;
  cxx_delete = (void (*)(void *))*del_slot;
  real_setup_pager = (void (*)(void *, void *))fn;
  *slot = (uintptr_t)&hook_setup_pager;
  debugPrintf("settings_menu: applied (%d rows, %d pages)\n", NUM_ROWS, NUM_PAGES);
}
