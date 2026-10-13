/* sharp_filter.h -- sharp-bilinear sampling for render-texture composites
 * (config: snes_video_mode = 1). See sharp_filter.c.
 */

#ifndef __SHARP_FILTER_H__
#define __SHARP_FILTER_H__

#include <GLES2/gl2.h>

// GL wrappers, wired into the libchrono import table (imports.c). Each one
// forwards to the real entry point unchanged; the tracking/rewriting they add
// is skipped entirely unless snes_video_mode is 1.
void sf_glActiveTexture(GLenum texture);
void sf_glBindTexture(GLenum target, GLuint texture);
void sf_glDeleteTextures(GLsizei n, const GLuint *textures);
void sf_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget,
                               GLuint texture, GLint level);
void sf_glShaderSource(GLuint shader, GLsizei count, const GLchar *const *string,
                       const GLint *length);
void sf_glCompileShader(GLuint shader);
void sf_glDeleteShader(GLuint shader);
void sf_glAttachShader(GLuint program, GLuint shader);
void sf_glLinkProgram(GLuint program);
void sf_glDeleteProgram(GLuint program);
void sf_glUseProgram(GLuint program);
void sf_glDrawArrays(GLenum mode, GLint first, GLsizei count);
void sf_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices);

// Called by the glTexImage2D wrapper(s) after a successful upload so the
// size of the texture bound to the active unit is known.
void sf_note_teximage(GLenum target, GLint level, GLsizei width, GLsizei height);

#endif
