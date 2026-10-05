/* crt.c -- optional CRT post-processing filter (config: crt_filter)
 *
 * Two styles:
 *   crt_filter = 1  "soft": a light retro look modelled on the CRT mode of
 *                   Square Enix's Pixel Remaster ports, measured from a pair
 *                   of screenshots with and without it: fine scanlines at a
 *                   fixed 120 lines per screen height (6 px at 720p) that
 *                   darken every other half-period by ~15%, slight horizontal
 *                   blur and red/blue fringing. The Pixel Remaster blurs its
 *                   pixel art more (sigma ~2.25 px, fringe ~3 px at 720p) but
 *                   keeps its menus out of that; here menus and text go
 *                   through the same filter as the game, so the defaults are
 *                   close to its menu look (0.5 px, 0.75 px) to keep text
 *                   sharp.
 *   crt_filter = 2  "CRTS": a port of Timothy Lottes' CRT-styled scaler
 *                   (public domain / Unlicense,
 *                   https://www.shadertoy.com/view/MtSfRK), the algorithm of
 *                   RetroArch's crt-lottes-fast, with its parameters and
 *                   defaults: a full tube simulation at console resolution.
 *
 * HOW IT WORKS
 * Before the engine renders a frame, crt_begin() binds an off-screen
 * framebuffer (colour texture + depth/stencil) the size of the render target.
 * The engine never notices: libchrono's glBindFramebuffer import goes through
 * crt_glBindFramebuffer(), which redirects "bind the window (0)" to this
 * framebuffer, and RenderTexture's own save/restore of the current binding
 * keeps working because it gets our framebuffer back.
 *
 * After the frame, crt_end() runs two passes and restores every bit of GL
 * state it touched, so cocos2d-x's cached state stays valid.
 * Soft style:
 *   1. Horizontal gaussian blur of the frame into a second full-res texture.
 *   2. Red/blue offset and scanlines into the window.
 * CRTS style:
 *   1. Downsample: the frame is point-sampled at the centre of every game
 *      ("art") pixel into a low-res texture -- 320x180 at the default
 *      field_zoom of 2. CRTS, like any CRT shader, needs the console-res
 *      image as input, not an already-upscaled one.
 *   2. CRTS: each output pixel is rebuilt from the 4 nearest texels of the 2
 *      nearest scanlines: gaussian horizontal blur, cosine-shaped scanlines,
 *      phosphor mask, optional curvature with rounded corners, all in linear
 *      light, with CRTS's tone curve compensating the darkening.
 */

#include <stdint.h>
#include <math.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "config.h"
#include "util.h"
#include "crt.h"

// full-resolution target the engine renders into
static GLuint s_fbo, s_tex, s_rb_ds, s_rb_d, s_rb_s;
static int    s_w, s_h;
// console-resolution copy fed to CRTS
static GLuint s_lo_fbo, s_lo_tex;
static int    s_lw, s_lh;
// horizontally blurred copy (soft style)
static GLuint s_mid_fbo, s_mid_tex;
static int    s_mode; // style the targets were built for

static int    s_failed;   // target couldn't be created at this size; don't retry every frame
static int    s_active;   // engine frame is being redirected into s_fbo

static GLuint s_prog_down, s_prog_crt, s_prog_hblur, s_prog_soft;
static GLint  s_d_tex;
static GLint  s_h_tex, s_h_step;
static GLint  s_s_tex, s_s_out, s_s_chroma, s_s_lines, s_s_dark, s_s_bright;
static GLint  s_c_tex, s_c_in, s_c_out, s_c_warp, s_c_thin, s_c_blur, s_c_mask, s_c_masktype,
              s_c_tone, s_c_corner, s_c_gamma, s_c_bright;
static PFNGLBINDVERTEXARRAYOESPROC s_bind_vao;

#define VERTEX_ARRAY_BINDING_OES 0x85B5
#define A_POS 0 // attribute location for both programs

static const char *VSH =
  "attribute vec2 a_pos;\n"
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "  v_uv = a_pos * 0.5 + 0.5;\n"
  "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
  "}\n";

// pass 1: point-sample the frame at art-pixel centres (texture is NEAREST)
static const char *FSH_DOWN =
  "precision mediump float;\n"
  "uniform sampler2D u_tex;\n"
  "varying vec2 v_uv;\n"
  "void main() { gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";

// soft style, pass 1: 5-tap horizontal gaussian (taps one sigma apart)
static const char *FSH_HBLUR =
  "precision mediump float;\n"
  "uniform sampler2D u_tex;\n"
  "uniform float u_step;\n"   // sigma in texture coords
  "varying vec2 v_uv;\n"
  "void main() {\n"
  "  vec3 c = texture2D(u_tex, v_uv).rgb * 0.4026;\n"
  "  c += (texture2D(u_tex, v_uv - vec2(u_step, 0.0)).rgb + texture2D(u_tex, v_uv + vec2(u_step, 0.0)).rgb) * 0.2442;\n"
  "  c += (texture2D(u_tex, v_uv - vec2(2.0 * u_step, 0.0)).rgb + texture2D(u_tex, v_uv + vec2(2.0 * u_step, 0.0)).rgb) * 0.0545;\n"
  "  gl_FragColor = vec4(c, 1.0);\n"
  "}\n";

// soft style, pass 2: red shifted right and blue shifted left, then
// anti-aliased square-wave scanlines
static const char *FSH_SOFT =
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform sampler2D u_tex;\n"
  "uniform vec2  u_out;\n"
  "uniform float u_chroma;\n" // red/blue offset in texture coords
  "uniform float u_lines;\n"  // scanline periods over the screen height
  "uniform float u_dark;\n"   // darkening of the dark half, 0..1
  "uniform float u_bright;\n"
  "varying vec2 v_uv;\n"
  "// integral of the bright half of the square wave over [0, t]\n"
  "float brightInt(float t) { return floor(t) * 0.5 + min(fract(t), 0.5); }\n"
  "void main() {\n"
  "  vec3 c;\n"
  "  c.r = texture2D(u_tex, v_uv - vec2(u_chroma, 0.0)).r;\n"
  "  c.g = texture2D(u_tex, v_uv).g;\n"
  "  c.b = texture2D(u_tex, v_uv + vec2(u_chroma, 0.0)).b;\n"
  "  float period = u_out.y / u_lines;\n"
  "  float y = u_out.y - gl_FragCoord.y - 0.5;\n"     // pixel top edge, from the top
  "  float t0 = y / period, t1 = (y + 1.0) / period;\n"
  "  float lit = (brightInt(t1) - brightInt(t0)) / (t1 - t0);\n"
  "  c *= mix(1.0 - u_dark, 1.0, lit) * u_bright;\n"
  "  gl_FragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
  "}\n";

// CRTS style, pass 2: CRTS, 8-tap version (from crt-lottes-fast, CRTS_WARP + CRTS_TONE)
static const char *FSH_CRT =
  "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
  "precision highp float;\n"
  "#else\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform sampler2D u_tex;\n"
  "uniform vec2  u_in;\n"       // console-res input size
  "uniform vec2  u_out;\n"      // output size
  "uniform vec2  u_warp;\n"     // curvature x/y
  "uniform float u_thin;\n"     // 0.5 fused .. 1.0 thin scanlines
  "uniform float u_blur;\n"     // horizontal blur, -3 pixely .. -1 blurry
  "uniform float u_mask;\n"     // masked channel exposure, 1 = no mask
  "uniform float u_masktype;\n" // 0 none, 1 grille lite, 2 grille, 3 shadow
  "uniform vec4  u_tone;\n"     // CrtsTone() constants
  "uniform float u_corner;\n"
  "uniform float u_gamma;\n"
  "uniform float u_bright;\n"
  "\n"
  "float FromSrgb1(float c) {\n"
  "  return (c <= 0.04045) ? c * (1.0 / 12.92) : pow(c * (1.0 / 1.055) + (0.055 / 1.055), u_gamma);\n"
  "}\n"
  "vec3 FromSrgb(vec3 c) { return vec3(FromSrgb1(c.r), FromSrgb1(c.g), FromSrgb1(c.b)); }\n"
  "float ToSrgb1(float c) { return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 0.41666) - 0.055; }\n"
  "vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }\n"
  "vec3 Fetch(vec2 uv) { return FromSrgb(texture2D(u_tex, uv).rgb); }\n"
  "\n"
  "vec3 Mask(vec2 pos, float dark) {\n"
  "  float x;\n"
  "  vec3 m;\n"
  "  if (u_masktype > 2.5) {\n"           // shadow mask
  "    pos.x += pos.y * 2.9999;\n"
  "    m = vec3(dark);\n"
  "    x = fract(pos.x * (1.0 / 6.0));\n"
  "    if (x < (1.0 / 3.0)) m.r = 1.0; else if (x < (2.0 / 3.0)) m.g = 1.0; else m.b = 1.0;\n"
  "    return m;\n"
  "  }\n"
  "  if (u_masktype > 1.5) {\n"           // aperture grille
  "    m = vec3(dark);\n"
  "    x = fract(pos.x * (1.0 / 3.0));\n"
  "    if (x < (1.0 / 3.0)) m.r = 1.0; else if (x < (2.0 / 3.0)) m.g = 1.0; else m.b = 1.0;\n"
  "    return m;\n"
  "  }\n"
  "  if (u_masktype > 0.5) {\n"           // aperture grille lite
  "    m = vec3(1.0);\n"
  "    x = fract(pos.x * (1.0 / 3.0));\n"
  "    if (x < (1.0 / 3.0)) m.r = dark; else if (x < (2.0 / 3.0)) m.g = dark; else m.b = dark;\n"
  "    return m;\n"
  "  }\n"
  "  return vec3(1.0);\n"
  "}\n"
  "\n"
  "void main() {\n"
  "  vec2 ipos = gl_FragCoord.xy;\n"
  "  vec2 rcpIn = 1.0 / u_in;\n"
  "  vec2 halfIn = 0.5 * u_in;\n"
  "  vec2 pos = ipos * (2.0 / u_out) - vec2(1.0);\n"
  "  pos *= vec2(1.0 + (pos.y * pos.y) * u_warp.x, 1.0 + (pos.x * pos.x) * u_warp.y);\n"
  "  float vin = (1.0 - ((1.0 - clamp(pos.x * pos.x, 0.0, 1.0)) * (1.0 - clamp(pos.y * pos.y, 0.0, 1.0))))\n"
  "              * (0.998 + 0.001 * u_corner);\n"
  "  vin = clamp(-vin * u_in.y + u_in.y, 0.0, 1.0);\n"
  "  pos = pos * halfIn + halfIn;\n"
  "\n"
  "  float y0 = floor(pos.y - 0.5) + 0.5;\n"
  "  float x0 = floor(pos.x - 1.5) + 0.5;\n"
  "  vec2 p = vec2(x0 * rcpIn.x, y0 * rcpIn.y);\n"
  "  vec3 colA0 = Fetch(p); p.x += rcpIn.x;\n"
  "  vec3 colA1 = Fetch(p); p.x += rcpIn.x;\n"
  "  vec3 colA2 = Fetch(p); p.x += rcpIn.x;\n"
  "  vec3 colA3 = Fetch(p); p.y += rcpIn.y;\n"
  "  vec3 colB3 = Fetch(p); p.x -= rcpIn.x;\n"
  "  vec3 colB2 = Fetch(p); p.x -= rcpIn.x;\n"
  "  vec3 colB1 = Fetch(p); p.x -= rcpIn.x;\n"
  "  vec3 colB0 = Fetch(p);\n"
  "\n"
  "  float off = pos.y - y0;\n"
  "  float pi2 = 6.28318530717958;\n"
  "  float scanA = cos(min(0.5, off * u_thin) * pi2) * 0.5 + 0.5;\n"
  "  float scanB = cos(min(0.5, (-off) * u_thin + u_thin) * pi2) * 0.5 + 0.5;\n"
  "  float off0 = pos.x - x0;\n"
  "  float off1 = off0 - 1.0;\n"
  "  float off2 = off0 - 2.0;\n"
  "  float off3 = off0 - 3.0;\n"
  "  float pix0 = exp2(u_blur * off0 * off0);\n"
  "  float pix1 = exp2(u_blur * off1 * off1);\n"
  "  float pix2 = exp2(u_blur * off2 * off2);\n"
  "  float pix3 = exp2(u_blur * off3 * off3);\n"
  "  float pixT = vin / (pix0 + pix1 + pix2 + pix3);\n"
  "  scanA *= pixT;\n"
  "  scanB *= pixT;\n"
  "  vec3 color = (colA0 * pix0 + colA1 * pix1 + colA2 * pix2 + colA3 * pix3) * scanA\n"
  "             + (colB0 * pix0 + colB1 * pix1 + colB2 * pix2 + colB3 * pix3) * scanB;\n"
  "  color *= Mask(ipos, u_mask);\n"
  "\n"
  "  float peak = max(1.0 / (256.0 * 65536.0), max(color.r, max(color.g, color.b)));\n"
  "  vec3 ratio = color / peak;\n"
  "  peak = peak / (peak * u_tone.y + u_tone.z);\n"
  "  color = ratio * peak * u_bright;\n"
  "  gl_FragColor = vec4(ToSrgb(clamp(color, 0.0, 1.0)), 1.0);\n"
  "}\n";

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, sizeof(log), NULL, log);
    debugPrintf("crt: shader error: %s\n", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

static GLuint link(const char *fsh) {
  GLuint vs = compile(GL_VERTEX_SHADER, VSH);
  GLuint fs = compile(GL_FRAGMENT_SHADER, fsh);
  if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glBindAttribLocation(p, A_POS, "a_pos");
  glLinkProgram(p);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(p, sizeof(log), NULL, log);
    debugPrintf("crt: link error: %s\n", log);
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static int init_programs(void) {
  if (s_prog_crt) return 1;
  s_prog_down  = link(FSH_DOWN);
  s_prog_crt   = link(FSH_CRT);
  s_prog_hblur = link(FSH_HBLUR);
  s_prog_soft  = link(FSH_SOFT);
  if (!s_prog_down || !s_prog_crt || !s_prog_hblur || !s_prog_soft) {
    if (s_prog_down) glDeleteProgram(s_prog_down);
    if (s_prog_crt) glDeleteProgram(s_prog_crt);
    if (s_prog_hblur) glDeleteProgram(s_prog_hblur);
    if (s_prog_soft) glDeleteProgram(s_prog_soft);
    s_prog_down = s_prog_crt = s_prog_hblur = s_prog_soft = 0;
    return 0;
  }
  s_h_tex    = glGetUniformLocation(s_prog_hblur, "u_tex");
  s_h_step   = glGetUniformLocation(s_prog_hblur, "u_step");
  s_s_tex    = glGetUniformLocation(s_prog_soft, "u_tex");
  s_s_out    = glGetUniformLocation(s_prog_soft, "u_out");
  s_s_chroma = glGetUniformLocation(s_prog_soft, "u_chroma");
  s_s_lines  = glGetUniformLocation(s_prog_soft, "u_lines");
  s_s_dark   = glGetUniformLocation(s_prog_soft, "u_dark");
  s_s_bright = glGetUniformLocation(s_prog_soft, "u_bright");
  s_d_tex      = glGetUniformLocation(s_prog_down, "u_tex");
  s_c_tex      = glGetUniformLocation(s_prog_crt, "u_tex");
  s_c_in       = glGetUniformLocation(s_prog_crt, "u_in");
  s_c_out      = glGetUniformLocation(s_prog_crt, "u_out");
  s_c_warp     = glGetUniformLocation(s_prog_crt, "u_warp");
  s_c_thin     = glGetUniformLocation(s_prog_crt, "u_thin");
  s_c_blur     = glGetUniformLocation(s_prog_crt, "u_blur");
  s_c_mask     = glGetUniformLocation(s_prog_crt, "u_mask");
  s_c_masktype = glGetUniformLocation(s_prog_crt, "u_masktype");
  s_c_tone     = glGetUniformLocation(s_prog_crt, "u_tone");
  s_c_corner   = glGetUniformLocation(s_prog_crt, "u_corner");
  s_c_gamma    = glGetUniformLocation(s_prog_crt, "u_gamma");
  s_c_bright   = glGetUniformLocation(s_prog_crt, "u_bright");
  s_bind_vao = (PFNGLBINDVERTEXARRAYOESPROC)eglGetProcAddress("glBindVertexArrayOES");
  return 1;
}

static void make_color_tex(GLuint *tex, int w, int h, GLint filter) {
  glGenTextures(1, tex);
  glBindTexture(GL_TEXTURE_2D, *tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void free_targets(void) {
  if (s_fbo)    glDeleteFramebuffers(1, &s_fbo);
  if (s_tex)    glDeleteTextures(1, &s_tex);
  if (s_rb_ds)  glDeleteRenderbuffers(1, &s_rb_ds);
  if (s_rb_d)   glDeleteRenderbuffers(1, &s_rb_d);
  if (s_rb_s)   glDeleteRenderbuffers(1, &s_rb_s);
  if (s_lo_fbo) glDeleteFramebuffers(1, &s_lo_fbo);
  if (s_lo_tex) glDeleteTextures(1, &s_lo_tex);
  if (s_mid_fbo) glDeleteFramebuffers(1, &s_mid_fbo);
  if (s_mid_tex) glDeleteTextures(1, &s_mid_tex);
  s_fbo = s_tex = s_rb_ds = s_rb_d = s_rb_s = s_lo_fbo = s_lo_tex = s_mid_fbo = s_mid_tex = 0;
  s_w = s_h = s_lw = s_lh = 0;
  s_mode = 0;
}

// number of scanlines (art-pixel rows) over the screen height
static float crt_lines(void) {
  if (config.crt_lines > 0.0f) return config.crt_lines;
  if (config.field_zoom_fix && config.field_zoom > 0.05f) return 360.0f / config.field_zoom;
  return 216.0f; // stock field camera: 360 / 1.6667
}

// (Re)create both targets for a w x h frame. Called with the window bound.
static int make_targets(int w, int h, int mode) {
  free_targets();
  GLint old_tex = 0, old_rb = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);

  // full-res target with depth + stencil, like the window surface (the engine
  // uses stencil for clipping): packed if available, separate otherwise
  // soft style samples the frame between texels (blur); CRTS point-samples it
  make_color_tex(&s_tex, w, h, mode == 1 ? GL_LINEAR : GL_NEAREST);
  glGenFramebuffers(1, &s_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_tex, 0);
  glGenRenderbuffers(1, &s_rb_ds);
  glBindRenderbuffer(GL_RENDERBUFFER, s_rb_ds);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_rb_ds);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_rb_ds);
  GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (st != GL_FRAMEBUFFER_COMPLETE) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
    glDeleteRenderbuffers(1, &s_rb_ds);
    s_rb_ds = 0;
    glGenRenderbuffers(1, &s_rb_d);
    glBindRenderbuffer(GL_RENDERBUFFER, s_rb_d);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, s_rb_d);
    glGenRenderbuffers(1, &s_rb_s);
    glBindRenderbuffer(GL_RENDERBUFFER, s_rb_s);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_rb_s);
    st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  }

  GLenum st2 = GL_FRAMEBUFFER_COMPLETE;
  if (st == GL_FRAMEBUFFER_COMPLETE && mode == 1) {
    // soft style: full-res intermediate for the separable blur
    make_color_tex(&s_mid_tex, w, h, GL_LINEAR);
    glGenFramebuffers(1, &s_mid_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_mid_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_mid_tex, 0);
    st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  } else if (st == GL_FRAMEBUFFER_COMPLETE) {
    // CRTS style: console-res target, square art pixels, one row per scanline
    float lines = crt_lines();
    if (lines < 16.0f) lines = 16.0f;
    if (lines > (float)h) lines = (float)h;
    s_lh = (int)(lines + 0.5f);
    s_lw = (int)(lines * (float)w / (float)h + 0.5f);
    make_color_tex(&s_lo_tex, s_lw, s_lh, GL_NEAREST);
    glGenFramebuffers(1, &s_lo_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_lo_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_lo_tex, 0);
    st2 = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, (GLuint)old_tex);
  glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)old_rb);

  if (st != GL_FRAMEBUFFER_COMPLETE || st2 != GL_FRAMEBUFFER_COMPLETE) {
    debugPrintf("crt: framebuffer %dx%d incomplete (0x%x / 0x%x), filter disabled\n", w, h, st, st2);
    free_targets();
    return 0;
  }
  s_w = w; s_h = h;
  s_mode = mode;
  if (mode == 1) debugPrintf("crt: soft style, %dx%d\n", w, h);
  else debugPrintf("crt: CRTS style, %dx%d frame -> %dx%d scanline image\n", w, h, s_lw, s_lh);
  return 1;
}

void crt_glBindFramebuffer(GLenum target, GLuint fb) {
  if (fb == 0 && s_active) fb = s_fbo;
  glBindFramebuffer(target, fb);
}

void crt_begin(int w, int h) {
  s_active = 0;
  const int mode = config.crt_filter == 2 ? 2 : 1;
  if (!config.crt_filter || w <= 0 || h <= 0) return;
  if (w != s_w || h != s_h || mode != s_mode) {
    if (s_failed == (w << 16 | h)) return;
    if (!init_programs() || !make_targets(w, h, mode)) { s_failed = (w << 16 | h); return; }
    s_failed = 0;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
  s_active = 1;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void crt_end(void) {
  if (!s_active) return;
  s_active = 0;

  // save everything we touch; cocos2d-x caches most of it
  GLint prog, active_tex, tex0, abuf, ebuf, vao = 0, vp[4];
  GLint en0 = 0, sz0, type0, norm0, stride0, attr_buf = 0;
  GLvoid *ptr0 = NULL;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  if (s_bind_vao) {
    glGetIntegerv(VERTEX_ARRAY_BINDING_OES, &vao);
    if (vao) s_bind_vao(0);
  }
  glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebuf); // per-VAO: read after VAO 0 is bound
  glGetIntegerv(GL_VIEWPORT, vp);
  const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST),
                  scissor = glIsEnabled(GL_SCISSOR_TEST), stencil = glIsEnabled(GL_STENCIL_TEST),
                  cull = glIsEnabled(GL_CULL_FACE);
  GLboolean cmask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &en0);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sz0);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type0);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &norm0);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride0);
  glGetVertexAttribiv(A_POS, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attr_buf);
  glGetVertexAttribPointerv(A_POS, GL_VERTEX_ATTRIB_ARRAY_POINTER, &ptr0);

  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  static const GLfloat quad[] = { -1.f, -1.f,  1.f, -1.f,  -1.f, 1.f,  1.f, 1.f };
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(A_POS);
  glVertexAttribPointer(A_POS, 2, GL_FLOAT, GL_FALSE, 0, quad);

  if (s_mode == 1) {
    // all sizes are Pixel Remaster's at 720p, scaled to the frame height
    const float k = (float)s_h / 720.0f;
    const float blur = clampf(config.crt_soft_blur, 0.0f, 8.0f) * k; // horizontal sigma, px
    // pass 1: horizontal blur
    glBindFramebuffer(GL_FRAMEBUFFER, s_mid_fbo);
    glViewport(0, 0, s_w, s_h);
    glUseProgram(s_prog_hblur);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glUniform1i(s_h_tex, 0);
    glUniform1f(s_h_step, blur / (float)s_w);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // pass 2: fringing + scanlines into the window
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(s_prog_soft);
    glBindTexture(GL_TEXTURE_2D, s_mid_tex);
    glUniform1i(s_s_tex, 0);
    glUniform2f(s_s_out, (float)s_w, (float)s_h);
    glUniform1f(s_s_chroma, clampf(config.crt_soft_fringe, 0.0f, 16.0f) * k / (float)s_w);
    glUniform1f(s_s_lines, clampf(config.crt_soft_lines, 16.0f, 2160.0f));
    glUniform1f(s_s_dark, clampf(config.crt_soft_scanlines, 0.0f, 1.0f));
    glUniform1f(s_s_bright, clampf(config.crt_brightness, 0.1f, 3.0f));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    goto restore;
  }

  // CRTS pass 1: art-pixel downsample
  glBindFramebuffer(GL_FRAMEBUFFER, s_lo_fbo);
  glViewport(0, 0, s_lw, s_lh);
  glUseProgram(s_prog_down);
  glBindTexture(GL_TEXTURE_2D, s_tex);
  glUniform1i(s_d_tex, 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // CRTS pass 2: into the window
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, s_w, s_h);
  glUseProgram(s_prog_crt);
  glBindTexture(GL_TEXTURE_2D, s_lo_tex);
  const float thin = 0.5f + 0.5f * clampf(config.crt_scanline_thinness, 0.0f, 1.0f);
  const float blur = -clampf(config.crt_scan_blur, 1.0f, 6.0f);
  float masktype = (float)(config.crt_mask_type < 0 ? 0 : config.crt_mask_type > 3 ? 3 : config.crt_mask_type);
  float mask = 1.0f - clampf(config.crt_mask_intensity, 0.0f, 1.0f);
  // CrtsTone(contrast = 1, saturation = 0, thin, mask): auto-exposure that
  // lifts the mid-level by the darkening the scanlines and mask cause
  float tmask = mask;
  if (masktype == 0.0f) tmask = 1.0f;
  if (masktype == 1.0f) tmask = 0.5f + mask * 0.5f;
  const float midOut = 0.18f / ((1.5f - thin) * (0.5f * tmask + 0.5f));
  const float pMidIn = 0.18f;
  const float tone_y = ((-pMidIn) + midOut) / ((1.0f - pMidIn) * midOut);
  const float tone_z = ((-pMidIn) * midOut + pMidIn) / (midOut * (-pMidIn) + midOut);
  const float curv = clampf(config.crt_curvature, 0.0f, 0.25f);
  glUniform1i(s_c_tex, 0);
  glUniform2f(s_c_in, (float)s_lw, (float)s_lh);
  glUniform2f(s_c_out, (float)s_w, (float)s_h);
  glUniform2f(s_c_warp, curv, curv * (float)s_h / (float)s_w); // CRTS assumes 4:3; use our aspect
  glUniform1f(s_c_thin, thin);
  glUniform1f(s_c_blur, blur);
  glUniform1f(s_c_mask, mask);
  glUniform1f(s_c_masktype, masktype);
  glUniform4f(s_c_tone, 1.0f, tone_y, tone_z, 1.0f);
  glUniform1f(s_c_corner, clampf(config.crt_corner, 0.0f, 11.0f));
  glUniform1f(s_c_gamma, clampf(config.crt_gamma, 1.0f, 3.0f));
  glUniform1f(s_c_bright, clampf(config.crt_brightness, 0.1f, 3.0f));
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

restore:
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)attr_buf);
  glVertexAttribPointer(A_POS, sz0, (GLenum)type0, (GLboolean)norm0, stride0, ptr0);
  if (!en0) glDisableVertexAttribArray(A_POS);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)ebuf);
  if (vao && s_bind_vao) s_bind_vao((GLuint)vao);
  glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
  glActiveTexture((GLenum)active_tex);
  glUseProgram((GLuint)prog);
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  if (blend)   glEnable(GL_BLEND);
  if (depth)   glEnable(GL_DEPTH_TEST);
  if (scissor) glEnable(GL_SCISSOR_TEST);
  if (stencil) glEnable(GL_STENCIL_TEST);
  if (cull)    glEnable(GL_CULL_FACE);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void crt_bind_window(void) {
  // for code that draws straight to the screen outside the engine frame
  // (movie playback): stop redirecting and target the real window
  s_active = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
