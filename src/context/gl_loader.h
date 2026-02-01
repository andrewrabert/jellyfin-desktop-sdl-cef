#pragma once
#ifdef _WIN32

#include <windows.h>
#include <GL/gl.h>
#include "GL/glext.h"

// Minimal GL function loader for Windows - loads only what OpenGLRenderer needs
namespace gl {

// Framebuffer
extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;

// Renderbuffer
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;

// Shaders
extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLUNIFORM1IPROC glUniform1i;

// VAO
extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray;

// Texture
extern PFNGLACTIVETEXTUREPROC glActiveTexture;

// Initialize GL function pointers (call after context is made current)
bool initGLLoader();

}  // namespace gl

// Bring into global namespace for compatibility
using gl::glGenFramebuffers;
using gl::glDeleteFramebuffers;
using gl::glBindFramebuffer;
using gl::glCheckFramebufferStatus;
using gl::glFramebufferTexture2D;
using gl::glFramebufferRenderbuffer;
using gl::glGenRenderbuffers;
using gl::glDeleteRenderbuffers;
using gl::glBindRenderbuffer;
using gl::glRenderbufferStorage;
using gl::glCreateShader;
using gl::glDeleteShader;
using gl::glShaderSource;
using gl::glCompileShader;
using gl::glCreateProgram;
using gl::glDeleteProgram;
using gl::glAttachShader;
using gl::glLinkProgram;
using gl::glUseProgram;
using gl::glGetUniformLocation;
using gl::glUniform1i;
using gl::glGenVertexArrays;
using gl::glDeleteVertexArrays;
using gl::glBindVertexArray;
using gl::glActiveTexture;

#endif  // _WIN32
