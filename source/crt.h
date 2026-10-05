#ifndef __CRT_H__
#define __CRT_H__

#include <GLES2/gl2.h>

// Optional CRT post-processing filter (config.crt_filter); see crt.c.
void crt_begin(int w, int h);   // before the engine renders a frame
void crt_end(void);             // after it, before the buffer swap
void crt_bind_window(void);     // for direct-to-screen drawing (movie playback)
void crt_glBindFramebuffer(GLenum target, GLuint fb); // libchrono's glBindFramebuffer

#endif
