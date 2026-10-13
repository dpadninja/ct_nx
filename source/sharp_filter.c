/* sharp_filter.c -- sharp-bilinear sampling for render-texture composites
 * (config: snes_video_mode = 1)
 *
 * WHY
 * With snes_video_mode=1 the field and WorldMap keep the shipped 9:8 geometry:
 * one art pixel is 3.75x3.33 screen px handheld, 5.625x5 docked. The field
 * planes are composited into RenderTextures at 1 texel per art pixel and those
 * textures are then drawn to the screen at that fractional scale. Under
 * NEAREST sampling a fractional scale gives columns of alternating width (3 and
 * 4 px), and the pattern crawls as the camera scrolls -- the shimmer. Plain
 * bilinear fixes the crawl but blurs every pixel.
 *
 * WHAT THIS DOES
 * Sharp-bilinear: each texel is drawn as a flat block and only the single
 * screen pixel that straddles a texel boundary is blended, weighted by how
 * much of it falls on either side. Every column ends up the same width and
 * scrolling is smooth. At an exact integer scale the weights are all 0 or 1,
 * so the result is bit-identical to NEAREST.
 *
 * HOW
 *  - glShaderSource: cocos's stock sprite fragment shaders (those that sample
 *    texture2D(CC_Texture0, v_texCoord) and nothing else from a texture) get
 *    that call replaced by sharp_tex(), defined right above main(). The
 *    texel->pixel scale comes from fwidth() (GL_OES_standard_derivatives).
 *    Label/distance-field/ETC1-alpha shaders and the game's own shaders
 *    (palette, effects) are left alone. If a rewritten shader fails to
 *    compile, the original source is restored and recompiled.
 *  - Only textures attached to a framebuffer (RenderTexture targets) get the
 *    filter: before each draw with a rewritten program the size of the
 *    texture on unit 0 goes into u_sharpTexSize, or (0,0) for any other
 *    texture, which makes sharp_tex() a plain texture2D(). UI, fonts and
 *    sprite atlases therefore sample exactly as before.
 *  - Texture sampling stays NEAREST; sharp_tex() fetches texel centres and
 *    blends them itself, so remove_bilinear_filter/force_nearest don't matter.
 */

#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "config.h"
#include "util.h"
#include "sharp_filter.h"

#define SF_MAX_TEX   16384 // GL names are small sequential ints; larger ids just go untracked
#define SF_MAX_SHD   4096
#define SF_MAX_PROG  4096
#define SF_MAX_UNITS 8

typedef struct { GLsizei w, h; unsigned char rt; } TexInfo;
typedef struct { char *orig; unsigned char rewritten; } ShdInfo;
typedef struct { GLint loc; unsigned char sharp; GLsizei last_w, last_h; } ProgInfo;

static TexInfo  s_tex[SF_MAX_TEX];
static ShdInfo  s_shd[SF_MAX_SHD];
static ProgInfo s_prog[SF_MAX_PROG];
static GLuint   s_bound[SF_MAX_UNITS];
static GLuint   s_unit = 0;
static GLuint   s_cur_prog = 0;
static int      s_state = -1; // -1 = undecided, 0 = off, 1 = on
static int      s_nrewritten = 0;

static const char k_sample[] = "texture2D(CC_Texture0, v_texCoord)";

static const char k_ext[] = "#extension GL_OES_standard_derivatives : enable\n";

// px is in texel units with texel centres on integers. s = screen px per
// texel per axis. w is the coverage of the right/top texel within this
// fragment's footprint; axes scaled below 2x fall back to NEAREST so 1:1
// render-to-texture passes stay unfiltered.
static const char k_func[] =
  "uniform highp vec2 u_sharpTexSize;\n"
  "lowp vec4 sharp_tex(sampler2D tex, highp vec2 uv) {\n"
  "  if (u_sharpTexSize.x <= 0.0) return texture2D(tex, uv);\n"
  "  highp vec2 px = uv * u_sharpTexSize - 0.5;\n"
  "  highp vec2 s = 1.0 / max(fwidth(px), vec2(1.0e-5));\n"
  "  highp vec2 i = floor(px);\n"
  "  highp vec2 f = px - i;\n"
  "  highp vec2 w = clamp((f - 0.5) * s + 0.5, 0.0, 1.0);\n"
  "  w = mix(step(0.5, f), w, step(1.99, s));\n"
  "  highp vec2 inv = 1.0 / u_sharpTexSize;\n"
  "  highp vec2 c0 = (i + 0.5) * inv;\n"
  "  highp vec2 c1 = c0 + inv;\n"
  "  lowp vec4 a = texture2D(tex, c0);\n"
  "  lowp vec4 b = texture2D(tex, vec2(c1.x, c0.y));\n"
  "  lowp vec4 c = texture2D(tex, vec2(c0.x, c1.y));\n"
  "  lowp vec4 d = texture2D(tex, c1);\n"
  "  return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);\n"
  "}\n";

static int sf_enabled(void) {
  if (s_state >= 0) return s_state;
  // Decided on first use, when the engine's GL context is current.
  s_state = 0;
  if (!config.snes_video_mode) return 0;
  const char *ext = (const char *)glGetString(GL_EXTENSIONS);
  if (!ext || !strstr(ext, "GL_OES_standard_derivatives")) {
    debugPrintf("sharp_filter: GL_OES_standard_derivatives missing -- disabled, NEAREST only\n");
    return 0;
  }
  debugPrintf("sharp_filter: enabled (snes_video_mode=1)\n");
  s_state = 1;
  return 1;
}

// --- texture tracking -------------------------------------------------------

void sf_glActiveTexture(GLenum texture) {
  s_unit = texture - GL_TEXTURE0;
  glActiveTexture(texture);
}

void sf_glBindTexture(GLenum target, GLuint texture) {
  if (target == GL_TEXTURE_2D && s_unit < SF_MAX_UNITS) s_bound[s_unit] = texture;
  glBindTexture(target, texture);
}

void sf_note_teximage(GLenum target, GLint level, GLsizei width, GLsizei height) {
  if (target != GL_TEXTURE_2D || level != 0 || s_unit >= SF_MAX_UNITS) return;
  GLuint id = s_bound[s_unit];
  if (id && id < SF_MAX_TEX) { s_tex[id].w = width; s_tex[id].h = height; }
}

void sf_glDeleteTextures(GLsizei n, const GLuint *textures) {
  for (GLsizei i = 0; i < n; i++) {
    GLuint id = textures[i];
    if (id < SF_MAX_TEX) memset(&s_tex[id], 0, sizeof(s_tex[id])); // names get reused
    for (int u = 0; u < SF_MAX_UNITS; u++) if (s_bound[u] == id) s_bound[u] = 0;
  }
  glDeleteTextures(n, textures);
}

void sf_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                               GLuint texture, GLint level) {
  if (texture && texture < SF_MAX_TEX && textarget == GL_TEXTURE_2D)
    s_tex[texture].rt = 1;
  glFramebufferTexture2D(target, attachment, textarget, texture, level);
}

// --- shader rewrite ---------------------------------------------------------

static size_t count_of(const char *src, const char *needle) {
  size_t n = 0, len = strlen(needle);
  for (const char *q = src; (q = strstr(q, needle)); q += len) n++;
  return n;
}

// Only plain colour sprites: every texture fetch is CC_Texture0 at v_texCoord
// (so not ShaderDrawPalettedTexture.fsh, whose CC_Texture0 holds palette
// indices that must never be blended) and no text-effect inputs.
static int wants_rewrite(const char *src) {
  size_t n = count_of(src, k_sample);
  return n && n == count_of(src, "texture2D(") && strstr(src, "void main") &&
         !strstr(src, "u_textColor") && !strstr(src, "u_effectColor") &&
         !strstr(src, "sharp_tex");
}

static char *concat_sources(GLsizei count, const GLchar *const *string, const GLint *length) {
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
  char *out = malloc(total + 1);
  if (!out) return NULL;
  char *p = out;
  for (GLsizei i = 0; i < count; i++) {
    size_t n = (length && length[i] >= 0) ? (size_t)length[i] : strlen(string[i]);
    memcpy(p, string[i], n);
    p += n;
  }
  *p = 0;
  return out;
}

// Builds: [#version line] + #extension + source-with-sharp_tex. Returns NULL
// if the source can't be rewritten safely.
static char *rewrite_source(const char *src) {
  const char *main_at = strstr(src, "void main");
  size_t nsample = count_of(src, k_sample);
  if (!main_at || !nsample) return NULL;

  // #extension must precede any non-preprocessor token; keep a #version
  // line (if any) first.
  size_t head = 0;
  const char *ver = strstr(src, "#version");
  if (ver) {
    const char *eol = strchr(ver, '\n');
    if (!eol || eol > main_at) return NULL;
    head = (size_t)(eol + 1 - src);
  }

  const size_t grow = sizeof("sharp_tex") - sizeof("texture2D"); // per replaced call
  size_t len = strlen(src) + sizeof(k_ext) + sizeof(k_func) + nsample * grow + 1;
  char *out = malloc(len);
  if (!out) return NULL;
  char *p = out;
  memcpy(p, src, head); p += head;
  memcpy(p, k_ext, sizeof(k_ext) - 1); p += sizeof(k_ext) - 1;
  for (const char *q = src + head; *q;) {
    if (q == main_at) { memcpy(p, k_func, sizeof(k_func) - 1); p += sizeof(k_func) - 1; }
    if (!strncmp(q, k_sample, sizeof(k_sample) - 1)) {
      memcpy(p, "sharp_tex", 9); p += 9;
      q += 9; // "texture2D" -- the argument list is copied as-is
      continue;
    }
    *p++ = *q++;
  }
  *p = 0;
  return out;
}

void sf_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                       const GLint *length) {
  if (shader < SF_MAX_SHD) {
    free(s_shd[shader].orig);
    s_shd[shader].orig = NULL;
    s_shd[shader].rewritten = 0;
  }
  if (!sf_enabled() || shader >= SF_MAX_SHD || count <= 0) {
    glShaderSource(shader, count, string, length);
    return;
  }
  GLint type = 0;
  glGetShaderiv(shader, GL_SHADER_TYPE, &type);
  char *src = (type == GL_FRAGMENT_SHADER) ? concat_sources(count, string, length) : NULL;
  char *rw = (src && wants_rewrite(src)) ? rewrite_source(src) : NULL;
  if (!rw) {
    free(src);
    glShaderSource(shader, count, string, length);
    return;
  }
  const GLchar *one = rw;
  glShaderSource(shader, 1, &one, NULL);
  free(rw);
  s_shd[shader].orig = src; // kept until compile succeeds, for the fallback
  s_shd[shader].rewritten = 1;
}

void sf_glCompileShader(GLuint shader) {
  glCompileShader(shader);
  if (shader >= SF_MAX_SHD || !s_shd[shader].rewritten) return;
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok) {
    s_nrewritten++;
    debugPrintf("sharp_filter: rewrote fragment shader %u (%d total)\n", shader, s_nrewritten);
  } else {
    char log[512] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
    debugPrintf("sharp_filter: shader %u failed, using original: %s\n", shader, log);
    const GLchar *orig = s_shd[shader].orig;
    glShaderSource(shader, 1, &orig, NULL);
    glCompileShader(shader);
    s_shd[shader].rewritten = 0;
  }
  free(s_shd[shader].orig);
  s_shd[shader].orig = NULL;
}

void sf_glDeleteShader(GLuint shader) {
  if (shader < SF_MAX_SHD) {
    free(s_shd[shader].orig);
    memset(&s_shd[shader], 0, sizeof(s_shd[shader]));
  }
  glDeleteShader(shader);
}

// --- programs and draws -----------------------------------------------------

void sf_glAttachShader(GLuint program, GLuint shader) {
  if (program < SF_MAX_PROG && shader < SF_MAX_SHD && s_shd[shader].rewritten)
    s_prog[program].sharp = 1;
  glAttachShader(program, shader);
}

void sf_glLinkProgram(GLuint program) {
  glLinkProgram(program);
  if (program >= SF_MAX_PROG || !s_prog[program].sharp) return;
  s_prog[program].loc = glGetUniformLocation(program, "u_sharpTexSize");
  s_prog[program].last_w = s_prog[program].last_h = -1; // uniform starts at (0,0); force first set
  if (s_prog[program].loc < 0) s_prog[program].sharp = 0;
}

void sf_glDeleteProgram(GLuint program) {
  if (program < SF_MAX_PROG) memset(&s_prog[program], 0, sizeof(s_prog[program])); // names get reused
  if (s_cur_prog == program) s_cur_prog = 0;
  glDeleteProgram(program);
}

void sf_glUseProgram(GLuint program) {
  s_cur_prog = program;
  glUseProgram(program);
}

static void sf_prepare_draw(void) {
  if (s_cur_prog >= SF_MAX_PROG || !s_prog[s_cur_prog].sharp) return;
  ProgInfo *pi = &s_prog[s_cur_prog];
  GLuint tex = s_bound[0];
  GLsizei w = 0, h = 0;
  if (tex && tex < SF_MAX_TEX && s_tex[tex].rt) { w = s_tex[tex].w; h = s_tex[tex].h; }
  if (w == pi->last_w && h == pi->last_h) return;
  glUniform2f(pi->loc, (GLfloat)w, (GLfloat)h);
  pi->last_w = w;
  pi->last_h = h;
}

void sf_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  sf_prepare_draw();
  glDrawArrays(mode, first, count);
}

void sf_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  sf_prepare_draw();
  glDrawElements(mode, count, type, indices);
}
