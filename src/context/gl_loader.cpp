#ifdef _WIN32

#include "context/gl_loader.h"
#include "logging.h"

namespace gl {

PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = nullptr;
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = nullptr;
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = nullptr;
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = nullptr;
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = nullptr;

PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = nullptr;
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = nullptr;
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = nullptr;
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = nullptr;

PFNGLCREATESHADERPROC glCreateShader = nullptr;
PFNGLDELETESHADERPROC glDeleteShader = nullptr;
PFNGLSHADERSOURCEPROC glShaderSource = nullptr;
PFNGLCOMPILESHADERPROC glCompileShader = nullptr;
PFNGLCREATEPROGRAMPROC glCreateProgram = nullptr;
PFNGLDELETEPROGRAMPROC glDeleteProgram = nullptr;
PFNGLATTACHSHADERPROC glAttachShader = nullptr;
PFNGLLINKPROGRAMPROC glLinkProgram = nullptr;
PFNGLUSEPROGRAMPROC glUseProgram = nullptr;
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = nullptr;
PFNGLUNIFORM1IPROC glUniform1i = nullptr;

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = nullptr;
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = nullptr;
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = nullptr;

PFNGLACTIVETEXTUREPROC glActiveTexture = nullptr;

static void* getProc(const char* name) {
    void* proc = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!proc) {
        static HMODULE opengl32 = LoadLibraryA("opengl32.dll");
        if (opengl32) {
            proc = reinterpret_cast<void*>(GetProcAddress(opengl32, name));
        }
    }
    return proc;
}

#define LOAD_GL(name) \
    name = reinterpret_cast<decltype(name)>(getProc(#name)); \
    if (!name) { LOG_ERROR(LOG_GL, "Failed to load " #name); return false; }

bool initGLLoader() {
    static bool initialized = false;
    if (initialized) return true;

    LOAD_GL(glGenFramebuffers);
    LOAD_GL(glDeleteFramebuffers);
    LOAD_GL(glBindFramebuffer);
    LOAD_GL(glCheckFramebufferStatus);
    LOAD_GL(glFramebufferTexture2D);
    LOAD_GL(glFramebufferRenderbuffer);

    LOAD_GL(glGenRenderbuffers);
    LOAD_GL(glDeleteRenderbuffers);
    LOAD_GL(glBindRenderbuffer);
    LOAD_GL(glRenderbufferStorage);

    LOAD_GL(glCreateShader);
    LOAD_GL(glDeleteShader);
    LOAD_GL(glShaderSource);
    LOAD_GL(glCompileShader);
    LOAD_GL(glCreateProgram);
    LOAD_GL(glDeleteProgram);
    LOAD_GL(glAttachShader);
    LOAD_GL(glLinkProgram);
    LOAD_GL(glUseProgram);
    LOAD_GL(glGetUniformLocation);
    LOAD_GL(glUniform1i);

    LOAD_GL(glGenVertexArrays);
    LOAD_GL(glDeleteVertexArrays);
    LOAD_GL(glBindVertexArray);

    LOAD_GL(glActiveTexture);

    initialized = true;
    LOG_INFO(LOG_GL, "[GL] Loaded extension functions");
    return true;
}

#undef LOAD_GL

}  // namespace gl

#endif  // _WIN32
