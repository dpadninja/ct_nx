/* config.h -- Chrono Trigger Switch wrapper configuration
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 * Copyright (C) 2026 ppkantorski <https://github.com/ppkantorski>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Memory reserved for the two .so load images. The rest of the heap backs both
// engine malloc and mesa GPU textures (field maps need the bulk) -- see
// __libnx_initheap in main.c.
#define SO_HEAP_RESERVE_MB 96

// The engine (libchrono.so) and its C++ runtime (libc++_shared.so). The Java-side
// libRMS.so/libencrypt.so are not needed -- the wrapper drives the cocos2d-x JNI
// entry points directly.
#define SO_NAME    "libchrono.so"
#define SOCPP_NAME "libc++_shared.so"

// Loose game assets (the APK's assets/ folder), served via the fake AAssetManager.
#define ASSETS_DIR "assets"

#define CONFIG_NAME "config.ini"
// Pre-INI format (flat "key value" lines, no section). read_config() migrates
// from this once if config.ini doesn't exist yet, then renames it to
// "config.txt.bak" so it's never read again -- config.ini is authoritative
// from that point on.
#define CONFIG_LEGACY_NAME "config.txt"
#define LOG_NAME    "debug.log"

#define DEBUG_LOG 0

// Opt-in diagnostics (no effect on a normal build). Set to 1 to write:
//   textdbg.log   -- every text-bitmap request (text, size, align, box w/h),
//                    so we can see exactly how the name/input fields are laid out
//   frametime.log -- a frame-time summary every ~5s (how many frames overran the
//                    16.67ms vsync budget), to tell GPU/streaming hitches apart
//                    from delta-time jitter.
// Both files land next to config.ini in /switch/ct/.
#define DEBUG_INSTR 0

// Opt-in diagnostic: count cocos2d object creation per call site and write
// the busiest sites to census.log every ~5s (see sprite_census.c).
#define SPRITE_CENSUS 0

// Opt-in diagnostic: time the game code (main thread only) and write the
// most expensive functions to prof.log every ~5s (see func_prof.c).
#define FUNC_PROFILER 0

extern int screen_width;
extern int screen_height;

// Default UI language (see lang_index in main.c for the supported codes).
// "default" means auto-detect: use the Switch console's own system language
// (Settings > System > Language) if it's one resources.bin ships, otherwise
// fall back to English. This is also config.language's fallback whenever the
// value in config.ini is missing/blank/unrecognized.
#define LANG_DEFAULT "default"

typedef struct {
  // Per-mode render resolution. -1/-1 (or anything out of range) auto-picks
  // 1280x720 handheld / 1920x1080 docked. An explicit value is honoured up to
  // 4K, so you can also render ABOVE native for supersampling (e.g. 2560x1440
  // docked) -- the compositor scales it to the real output either way. Keep a
  // 16:9 ratio to avoid stretch. The two pairs are independent: e.g. stay near
  // native docked (where GPU clock is higher) while scaling down further in
  // handheld. Applied live -- changing dock state mid-session resizes the
  // render target and tells the engine, no restart needed.
  int screen_width_handheld;
  int screen_height_handheld;
  int screen_width_docked;
  int screen_height_docked;
  char language[8];
  // Performance knobs (see read_config for defaults):
  //   gl_threaded -- 1 = run mesa's GL command submission on a second CPU core
  //                  (glthread). The engine is single-threaded and spends much
  //                  of each frame inside the nouveau driver; this offloads that
  //                  onto a spare core, shortening the critical path that
  //                  otherwise needs a CPU/EMC overclock to reach 60fps. Set 0
  //                  to disable if it causes trouble. No-op if mesa lacks it.
  //   gl_no_error -- 1 = skip mesa's internal validation of GL calls (bad enum/
  //                  state checks) before they reach the driver. cocos2d-x's
  //                  calls are already well-formed, so this just removes CPU
  //                  bookkeeping on a path that fires constantly. Set 0 if a
  //                  future mesa build ever mishandles it.
  //   game_font   -- 1 = render the engine's *system-font* labels with a TTF from
  //                  the game/SD instead of the Switch shared font, so they match
  //                  the in-game look. (The game's own TTF labels are unaffected;
  //                  they're already drawn by the engine.) Glyphs the game font
  //                  lacks fall back to the shared font. Set 0 for the old look.
  //   game_font_path -- path to the TTF to use for game_font. Relative to the
  //                  /switch/ct dir (e.g. "assets/font/xxxxx.ttf"). If empty, a
  //                  few common asset locations are probed; if none load, falls
  //                  back to the shared font.
  //   game_font_scale -- fine-tune multiplier on the game font's auto-fitted
  //                  size. 1.0 = auto (cap height matched to the shared font);
  //                  lower (e.g. 0.85) if it still looks too big, higher if small.
  //   game_font_xscale -- horizontal-only squeeze for the game font. 1.0 = none.
  //                  Use < 1 (e.g. 0.7) to condense a wide/monospaced font so the
  //                  game's pre-broken lines stay inside their window. A
  //                  proportional CT font usually needs no squeeze.
  //   text_shadow -- 1 = draw a crisp drop shadow behind the system-font labels,
  //                  in the classic CT/SNES style (offset down-right, hard-edged
  //                  so it suits a pixel font; no blur). 0 = no shadow.
  //   text_shadow_alpha -- shadow opacity, 0..255 (the shadow is black). It also
  //                  scales with each label's own alpha, so fades stay consistent.
  //                  Default 230 -- a strong, solid-looking shadow.
  //   text_shadow_scale -- multiplier on the auto offset (which itself scales with
  //                  the glyph size, ~1px per 12px of glyph). 1.0 = default; >1 for
  //                  a heavier shadow, <1 for a tighter one.
  int gl_threaded;
  int gl_no_error;
  int game_font;
  char game_font_path[256];
  float game_font_scale;
  float game_font_xscale;
  int text_shadow;
  int text_shadow_alpha;
  float text_shadow_scale;
  // Runtime binary patches applied to libchrono.so at boot (before so_finalize).
  // Each flag is independent; disable any that cause trouble with a future build.
  //   cursor_fix         -- keep selected-item text WHITE and use a dark
  //                         semi-transparent highlight instead of the cream
  //                         colour-inversion look.
  //   remove_bilinear_filter -- force GL_NEAREST (pixel-perfect point sampling)
  //                         everywhere instead of bilinear texture filtering.
  //   remove_mobile_ui   -- hide the on-screen touch-overlay buttons (field
  //                         menu button, world-map menu/map/warp buttons, and
  //                         all five right-side title-screen icon buttons).
  int cursor_fix;
  int remove_bilinear_filter;
  int remove_mobile_ui;
  // Input / controller:
  //   native_controller -- 1 = drive the engine purely through its native
  //                        controller path (cocos2d Controller -> GameController),
  //                        so on-screen prompts and <BTN_*> dialogue tags show
  //                        Switch buttons. 0 = also emit keyboard key events
  //                        (legacy compat; the engine then reverts to
  //                        keyboard-style prompts whenever a key event is seen).
  //   controller_glyphs -- 1 = force in-message <BTN_*> tags to the controller
  //                        glyph set (honours the A/B-swap setting) regardless of
  //                        the engine's own last-input-device auto-selection.
  //                        Belt-and-braces with native_controller; harmless on.
  //   right_stick_mirror -- 1 = emulate the left stick using the physical
  //                        right stick: right-stick tilt is sent to the game
  //                        on the same channel/ids as the real left stick
  //                        (CC_JOY_LX/LY), so either stick drives movement.
  //                        Right-stick tilt takes priority over the real
  //                        left stick when both are active; with the right
  //                        stick centred, the real left stick passes through
  //                        untouched. 0 = only the real left stick drives
  //                        that channel; the right stick's own position is
  //                        still sent separately as CC_JOY_RX/RY either way.
  //   key_zl / key_zr / key_plus / key_minus -- remap the four buttons this
  //                        game otherwise leaves unused to any other face /
  //                        shoulder button. Set each to one of:
  //                          key_a  key_b  key_x  key_y  key_l  key_r
  //                        and pressing the physical button then behaves
  //                        exactly as if the named button were pressed --
  //                        same engine action AND same on-screen glyph. Set
  //                        to its own name (key_zl = key_zl, the default) or
  //                        leave it at anything unrecognised to keep the
  //                        button behaving as it does normally (ZL/ZR do
  //                        nothing in-game; +/- open the pause/system menu
  //                        and select). Only these four sources are
  //                        remappable, and only to those six targets; the
  //                        edge/press tracking stays keyed to the real
  //                        button, so only what gets emitted changes. Takes
  //                        effect at boot (resolved once); relaunch to change.
  int native_controller;
  int controller_glyphs;
  int right_stick_mirror;
  char key_zl[16];
  char key_zr[16];
  char key_plus[16];
  char key_minus[16];
  // fix_diagonal_movement -- 1 = patch FieldImpl::UserScroll's diagonal-input
  //                          cases to accumulate position from the raw
  //                          per-frame delta directly (matching how
  //                          cardinal/non-diagonal movement already works),
  //                          instead of the original float divide-by-1.39 +
  //                          per-frame truncation, which under-advances the
  //                          target every frame and reads as a stutter.
  //                          Pure visual/feel fix; no save-data impact.
  int fix_diagonal_movement;
  // reuse_draw_objects -- EXPERIMENTAL. 1 = at the call sites listed in
  //             pool_sites.h, reuse the sprites / render textures the engine
  //             otherwise creates and destroys every frame (per-scanline and
  //             per-tile effects, the world-map clouds and Black Omen, battle
  //             effect backgrounds, ...). Each reused object is reset to the
  //             state a newly created one would have, so the output is the
  //             same. Big CPU saving in effect-heavy scenes. 0 = stock
  //             (default while experimental).
  int reuse_draw_objects;
  // cache_palettes -- EXPERIMENTAL. 1 = world-map objects drawn with the
  //             palette shader (Black Omen, Ozzie's fort, the Epoch, ...)
  //             share one palette texture per distinct palette instead of
  //             uploading a new one per object every frame (palette_cache.c).
  //             Same texture contents. 0 = stock (default while experimental).
  int cache_palettes;
  // fixed_timestep -- 1 = force cocos2d's per-frame delta (Director::_deltaTime)
  //                   to a constant 1/60s instead of the measured wall-clock gap.
  //                   Vsync locks presentation to 60Hz, but the measured delta
  //                   still jitters frame-to-frame, so anything timed off dt
  //                   (Actions, scheduled callbacks, the intro movie's audio
  //                   sync) can drift slightly out of step with real time.
  //                   A constant step -- what this frame-based game actually
  //                   wants -- removes that drift. NOTE: confirmed via testing
  //                   that this does NOT fix the motion shimmer/scoot artifact
  //                   -- that turned out to be a scale-alignment bug, fixed
  //                   separately by ui_scale_fix below. Keep this on
  //                   for its own real benefit (e.g. intro video/song no
  //                   longer end out of sync). Patches Director::drawScene and
  //                   ::calculateDeltaTime. Trade-off: on a sustained sub-60fps
  //                   dip motion runs slightly slow rather than skipping (rare).
  //                   See g_fixed_timestep_patches in patches.h for rationale.
  int fixed_timestep;
  // ui_scale_fix -- 1 = correct the cocos2d-x design resolution from
  //                   leftover iPhone-5 boilerplate (568x320, aspect 1.775) to
  //                   640x360 (exact 16:9). ROOT CAUSE FIX for the motion
  //                   shimmer/scoot/thinning-fine-detail artifact, confirmed via
  //                   scale_diag.log: 568x320 never divides evenly into any
  //                   Switch panel (1280x720, 1920x1080, 2560x1440 all give a
  //                   fractional mScaleX/mScaleY -- 2.253521...), so every
  //                   layer -- field, sprites, menu text -- gets stretched by a
  //                   non-integer factor and GL_NEAREST-samples a scrolling,
  //                   per-frame-shifting position, which reads as scoot/shimmer
  //                   in motion and clean at rest. 640x360 divides every Switch
  //                   resolution exactly (2x/3x/4x), so the scale is always a
  //                   whole number with no further patch needed downstream.
  //                   Supersedes the old integer_scale_fix idea (rounding the
  //                   scale after the fact only shifted the zoom level without
  //                   fixing the input). Trade-off: shows ~12% more of the
  //                   world per screen edge (a small, permanent zoom-out);
  //                   sprite/UI pixel density is unchanged. See
  //                   apply_ui_scale_fix in patches.h.
  int ui_scale_fix;
  // force_nearest -- 1 = rewrite every GL texture filter to NEAREST at the
  //                   wrapper (glTexParameteri/f interception + stamping
  //                   NEAREST at texture creation). Total coverage, unlike the
  //                   remove_bilinear_filter binary patch, which cannot reach
  //                   textures that rely on GL's default filters (default MAG
  //                   filter is GL_LINEAR) such as the field composite
  //                   buffers. A raw 1080p framebuffer screenshot proved the
  //                   output was bilinear-upscaled -- the direct cause of the
  //                   "squares aren't square / uneven widths / grey-blended
  //                   edges" artifact. 0 = leave the game's filtering alone.
  int force_nearest;
  // game_area_width_fix -- 1 = make the global ctr::gameArea rect's WIDTH
  // adaptive (visible width) instead of the hardcoded iPhone-5 568.0 it
  // ships with. Its height was already adaptive; the asymmetry is the root
  // cause of the measured 640/568 (~1.127) horizontal-only art-pixel
  // stretch that no screen_width/height combination could cancel. Exact
  // no-op when the design resolution is stock 568-wide, so safe to leave on.
  // See patches.h section 8 for the full derivation. NOTE: boot-time
  // binary patch -- changing this in config.ini requires a game relaunch;
  // the live config reload deliberately does not (and cannot) re-patch.
  int game_area_width_fix;
  // menu_alignment_fix -- 1 = fix the Equipment menu's vertical text
  // misalignment: every label the equip page itself places (the seven
  // stat rows Strength..Magic Defense and their values, the HP/MP/EXP top
  // block, the category-button captions "Bronze Blade" etc., and the
  // equipped-item info column: the stat numbers and "Speed +1" text) sits
  // exactly 1.5 design units (3 screen px at 720p) HIGHER than the
  // item-select list labels ("Wooden Sword"), which are the only
  // correctly placed text on the page. Root cause: the item list centers
  // its labels (ANCHOR_MIDDLE_LEFT at y = buttonHeight*0.5) while every
  // other label on the page is TOP-anchored with offsets that assume a
  // 16-unit label box; the real ChronoType 12pt label box is 19 units
  // tall, so top-anchored text lands labelHeight/2 - 8 = 1.5 units high.
  // Shifts all four placement paths down 1.5 in lockstep (see patches.h
  // section 10 for the per-site mechanism). Screenshot-verified: the
  // offset is exactly 3px at 720p on every affected row. Equip menu only;
  // no other menu shares these code paths. Boot-time binary patch:
  // relaunch to change.
  int menu_alignment_fix;
  // snes_video_mode -- how the field (and world map) is framed and sampled.
  //   1 (default) -- SNES-like picture: wide (~9:8, TV-style) art pixels and
  //            the full 224-row SNES picture height on the field (stock
  //            Android shows only 216 of the 224 rows; patches.h:
  //            apply_field_snes_geometry). The world map keeps the stock
  //            9:8 framing. The fractional scale (~3.6x3.2 handheld,
  //            ~5.4x4.8 docked) is drawn with a sharp-bilinear sampler
  //            (sharp_filter.c) instead of plain NEAREST, so every art-pixel
  //            column is the same width and scrolling doesn't shimmer; only
  //            the one screen pixel on each art-pixel boundary is blended.
  //            Integer-scaled draws (UI, render-to-texture) come out
  //            identical to NEAREST.
  //   0 -- pixel-perfect square pixels at a fixed 4x4 handheld /
  //            6x6 docked (view 320x180 art px; patches.h: field/map zoom
  //            patches at SQUARE_ZOOM).
  // Boot-time: relaunch to change.
  int snes_video_mode;
  // Mod pack directory. Place .ctp files (ChronoMod-compatible Chrono Trigger
  // Patch archives) here and they will be applied to resources.bin at startup
  // without touching the original file. Paths are relative to the install
  // folder (e.g. "mods" -> /switch/ct_nx/mods/). Set empty to disable.
  char mods_dir[256];
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
