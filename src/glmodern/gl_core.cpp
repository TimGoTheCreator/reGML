#include "gl_core.hpp"

#if REGML_WITH_SDL2
#include <SDL2/SDL.h>
#endif
#if REGML_WIIU
#include <gx2gl/proc.h>
#endif

namespace regml::glmodern {

// Default GL pointers to a no-op stub so calls before/without symbol resolution don't segfault
static void GLAPIENTRY gl_stub_func() {}

template <typename T>
static T make_stub() noexcept {
    return reinterpret_cast<T>(&gl_stub_func);
}

PFNGLVIEWPORTPROC glViewport = make_stub<PFNGLVIEWPORTPROC>();
PFNGLSCISSORPROC glScissor = make_stub<PFNGLSCISSORPROC>();
PFNGLCLEARCOLORPROC glClearColor = make_stub<PFNGLCLEARCOLORPROC>();
PFNGLCLEARPROC glClear = make_stub<PFNGLCLEARPROC>();
PFNGLCLEARDEPTHPROC glClearDepth = make_stub<PFNGLCLEARDEPTHPROC>();
PFNGLCLEARSTENCILPROC glClearStencil = make_stub<PFNGLCLEARSTENCILPROC>();
PFNGLCOLORMASKPROC glColorMask = make_stub<PFNGLCOLORMASKPROC>();
PFNGLDEPTHMASKPROC glDepthMask = make_stub<PFNGLDEPTHMASKPROC>();
PFNGLDEPTHFUNCPROC glDepthFunc = make_stub<PFNGLDEPTHFUNCPROC>();
PFNGLBLENDFUNCPROC glBlendFunc = make_stub<PFNGLBLENDFUNCPROC>();
PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate = make_stub<PFNGLBLENDFUNCSEPARATEPROC>();
PFNGLBLENDEQUATIONPROC glBlendEquation = make_stub<PFNGLBLENDEQUATIONPROC>();
PFNGLBLENDEQUATIONSEPARATEPROC glBlendEquationSeparate = make_stub<PFNGLBLENDEQUATIONSEPARATEPROC>();
PFNGLCULLFACEPROC glCullFace = make_stub<PFNGLCULLFACEPROC>();
PFNGLFRONTFACEPROC glFrontFace = make_stub<PFNGLFRONTFACEPROC>();
PFNGLPOLYGONMODEPROC glPolygonMode = make_stub<PFNGLPOLYGONMODEPROC>();
PFNGLLINEWIDTHPROC glLineWidth = make_stub<PFNGLLINEWIDTHPROC>();
PFNGLENABLEPROC glEnable = make_stub<PFNGLENABLEPROC>();
PFNGLDISABLEPROC glDisable = make_stub<PFNGLDISABLEPROC>();
PFNGLISENABLEDPROC glIsEnabled = make_stub<PFNGLISENABLEDPROC>();
PFNGLPIXELSTOREIPROC glPixelStorei = make_stub<PFNGLPIXELSTOREIPROC>();
PFNGLFLUSHPROC glFlush = make_stub<PFNGLFLUSHPROC>();
PFNGLFINISHPROC glFinish = make_stub<PFNGLFINISHPROC>();
PFNGLREADPIXELSPROC glReadPixels = make_stub<PFNGLREADPIXELSPROC>();
PFNGLGETERRORPROC glGetError = make_stub<PFNGLGETERRORPROC>();
PFNGLGETINTEGERVPROC glGetIntegerv = make_stub<PFNGLGETINTEGERVPROC>();
PFNGLGETFLOATVPROC glGetFloatv = make_stub<PFNGLGETFLOATVPROC>();
PFNGLGETBOOLEANVPROC glGetBooleanv = make_stub<PFNGLGETBOOLEANVPROC>();
PFNGLGETSTRINGPROC glGetString = make_stub<PFNGLGETSTRINGPROC>();
PFNGLGETSTRINGIPROC glGetStringi = make_stub<PFNGLGETSTRINGIPROC>();

PFNGLDRAWARRAYSPROC glDrawArrays = make_stub<PFNGLDRAWARRAYSPROC>();
PFNGLDRAWELEMENTSPROC glDrawElements = make_stub<PFNGLDRAWELEMENTSPROC>();
PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced = make_stub<PFNGLDRAWARRAYSINSTANCEDPROC>();
PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced = make_stub<PFNGLDRAWELEMENTSINSTANCEDPROC>();
PFNGLDRAWELEMENTSBASEVERTEXPROC glDrawElementsBaseVertex = make_stub<PFNGLDRAWELEMENTSBASEVERTEXPROC>();

PFNGLGENBUFFERSPROC glGenBuffers = make_stub<PFNGLGENBUFFERSPROC>();
PFNGLDELETEBUFFERSPROC glDeleteBuffers = make_stub<PFNGLDELETEBUFFERSPROC>();
PFNGLBINDBUFFERPROC glBindBuffer = make_stub<PFNGLBINDBUFFERPROC>();
PFNGLBUFFERDATAPROC glBufferData = make_stub<PFNGLBUFFERDATAPROC>();
PFNGLBUFFERSUBDATAPROC glBufferSubData = make_stub<PFNGLBUFFERSUBDATAPROC>();
PFNGLMAPBUFFERPROC glMapBuffer = make_stub<PFNGLMAPBUFFERPROC>();
PFNGLMAPBUFFERRANGEPROC glMapBufferRange = make_stub<PFNGLMAPBUFFERRANGEPROC>();
PFNGLUNMAPBUFFERPROC glUnmapBuffer = make_stub<PFNGLUNMAPBUFFERPROC>();

PFNGLGENVERTEXARRAYSPROC glGenVertexArrays = make_stub<PFNGLGENVERTEXARRAYSPROC>();
PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays = make_stub<PFNGLDELETEVERTEXARRAYSPROC>();
PFNGLBINDVERTEXARRAYPROC glBindVertexArray = make_stub<PFNGLBINDVERTEXARRAYPROC>();
PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = make_stub<PFNGLENABLEVERTEXATTRIBARRAYPROC>();
PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray = make_stub<PFNGLDISABLEVERTEXATTRIBARRAYPROC>();
PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = make_stub<PFNGLVERTEXATTRIBPOINTERPROC>();
PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer = make_stub<PFNGLVERTEXATTRIBIPOINTERPROC>();
PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor = make_stub<PFNGLVERTEXATTRIBDIVISORPROC>();

PFNGLGENTEXTURESPROC glGenTextures = make_stub<PFNGLGENTEXTURESPROC>();
PFNGLDELETETEXTURESPROC glDeleteTextures = make_stub<PFNGLDELETETEXTURESPROC>();
PFNGLBINDTEXTUREPROC glBindTexture = make_stub<PFNGLBINDTEXTUREPROC>();
PFNGLACTIVETEXTUREPROC glActiveTexture = make_stub<PFNGLACTIVETEXTUREPROC>();
PFNGLTEXPARAMETERIPROC glTexParameteri = make_stub<PFNGLTEXPARAMETERIPROC>();
PFNGLTEXPARAMETERFPROC glTexParameterf = make_stub<PFNGLTEXPARAMETERFPROC>();
PFNGLTEXPARAMETERIVPROC glTexParameteriv = make_stub<PFNGLTEXPARAMETERIVPROC>();
PFNGLTEXPARAMETERFVPROC glTexParameterfv = make_stub<PFNGLTEXPARAMETERFVPROC>();
PFNGLTEXIMAGE2DPROC glTexImage2D = make_stub<PFNGLTEXIMAGE2DPROC>();
PFNGLTEXSUBIMAGE2DPROC glTexSubImage2D = make_stub<PFNGLTEXSUBIMAGE2DPROC>();
PFNGLGENERATEMIPMAPPROC glGenerateMipmap = make_stub<PFNGLGENERATEMIPMAPPROC>();

PFNGLCREATESHADERPROC glCreateShader = make_stub<PFNGLCREATESHADERPROC>();
PFNGLDELETESHADERPROC glDeleteShader = make_stub<PFNGLDELETESHADERPROC>();
PFNGLSHADERSOURCEPROC glShaderSource = make_stub<PFNGLSHADERSOURCEPROC>();
PFNGLCOMPILESHADERPROC glCompileShader = make_stub<PFNGLCOMPILESHADERPROC>();
PFNGLGETSHADERIVPROC glGetShaderiv = make_stub<PFNGLGETSHADERIVPROC>();
PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = make_stub<PFNGLGETSHADERINFOLOGPROC>();
PFNGLCREATEPROGRAMPROC glCreateProgram = make_stub<PFNGLCREATEPROGRAMPROC>();
PFNGLDELETEPROGRAMPROC glDeleteProgram = make_stub<PFNGLDELETEPROGRAMPROC>();
PFNGLATTACHSHADERPROC glAttachShader = make_stub<PFNGLATTACHSHADERPROC>();
PFNGLDETACHSHADERPROC glDetachShader = make_stub<PFNGLDETACHSHADERPROC>();
PFNGLLINKPROGRAMPROC glLinkProgram = make_stub<PFNGLLINKPROGRAMPROC>();
PFNGLUSEPROGRAMPROC glUseProgram = make_stub<PFNGLUSEPROGRAMPROC>();
PFNGLVALIDATEPROGRAMPROC glValidateProgram = make_stub<PFNGLVALIDATEPROGRAMPROC>();
PFNGLGETPROGRAMIVPROC glGetProgramiv = make_stub<PFNGLGETPROGRAMIVPROC>();
PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = make_stub<PFNGLGETPROGRAMINFOLOGPROC>();
PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = make_stub<PFNGLGETATTRIBLOCATIONPROC>();
PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation = make_stub<PFNGLBINDATTRIBLOCATIONPROC>();
PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = make_stub<PFNGLGETUNIFORMLOCATIONPROC>();
PFNGLUNIFORM1IPROC glUniform1i = make_stub<PFNGLUNIFORM1IPROC>();
PFNGLUNIFORM2IPROC glUniform2i = make_stub<PFNGLUNIFORM2IPROC>();
PFNGLUNIFORM3IPROC glUniform3i = make_stub<PFNGLUNIFORM3IPROC>();
PFNGLUNIFORM4IPROC glUniform4i = make_stub<PFNGLUNIFORM4IPROC>();
PFNGLUNIFORM1FPROC glUniform1f = make_stub<PFNGLUNIFORM1FPROC>();
PFNGLUNIFORM2FPROC glUniform2f = make_stub<PFNGLUNIFORM2FPROC>();
PFNGLUNIFORM3FPROC glUniform3f = make_stub<PFNGLUNIFORM3FPROC>();
PFNGLUNIFORM4FPROC glUniform4f = make_stub<PFNGLUNIFORM4FPROC>();
PFNGLUNIFORM1IVPROC glUniform1iv = make_stub<PFNGLUNIFORM1IVPROC>();
PFNGLUNIFORM2IVPROC glUniform2iv = make_stub<PFNGLUNIFORM2IVPROC>();
PFNGLUNIFORM3IVPROC glUniform3iv = make_stub<PFNGLUNIFORM3IVPROC>();
PFNGLUNIFORM4IVPROC glUniform4iv = make_stub<PFNGLUNIFORM4IVPROC>();
PFNGLUNIFORM1FVPROC glUniform1fv = make_stub<PFNGLUNIFORM1FVPROC>();
PFNGLUNIFORM2FVPROC glUniform2fv = make_stub<PFNGLUNIFORM2FVPROC>();
PFNGLUNIFORM3FVPROC glUniform3fv = make_stub<PFNGLUNIFORM3FVPROC>();
PFNGLUNIFORM4FVPROC glUniform4fv = make_stub<PFNGLUNIFORM4FVPROC>();
PFNGLUNIFORMMATRIX2FVPROC glUniformMatrix2fv = make_stub<PFNGLUNIFORMMATRIX2FVPROC>();
PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv = make_stub<PFNGLUNIFORMMATRIX3FVPROC>();
PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv = make_stub<PFNGLUNIFORMMATRIX4FVPROC>();

PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers = make_stub<PFNGLGENFRAMEBUFFERSPROC>();
PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers = make_stub<PFNGLDELETEFRAMEBUFFERSPROC>();
PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer = make_stub<PFNGLBINDFRAMEBUFFERPROC>();
PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = make_stub<PFNGLFRAMEBUFFERTEXTURE2DPROC>();
PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus = make_stub<PFNGLCHECKFRAMEBUFFERSTATUSPROC>();
PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers = make_stub<PFNGLGENRENDERBUFFERSPROC>();
PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers = make_stub<PFNGLDELETERENDERBUFFERSPROC>();
PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer = make_stub<PFNGLBINDRENDERBUFFERPROC>();
PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage = make_stub<PFNGLRENDERBUFFERSTORAGEPROC>();
PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer = make_stub<PFNGLFRAMEBUFFERRENDERBUFFERPROC>();
PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer = make_stub<PFNGLBLITFRAMEBUFFERPROC>();
PFNGLDRAWBUFFERPROC glDrawBuffer = make_stub<PFNGLDRAWBUFFERPROC>();

static bool s_loaded = false;

bool is_gl_core_loaded() noexcept {
    return s_loaded;
}

static void* resolve_gl_proc(const char* name, void* (*custom_loader)(const char*)) {
    if (custom_loader) {
        if (void* p = custom_loader(name)) {
            return p;
        }
    }

#if REGML_WIIU
    if (void* p = GX2GL_GetProcAddress(name)) {
        return p;
    }
#endif

#if REGML_WITH_SDL2
    if (void* p = SDL_GL_GetProcAddress(name)) {
        return p;
    }
#endif

    return nullptr;
}

template <typename T>
static bool load_func(T& target, const char* name, void* (*custom_loader)(const char*)) {
    if (target != nullptr && target != make_stub<T>()) {
        return true;
    }
    void* addr = resolve_gl_proc(name, custom_loader);
    if (addr) {
        target = reinterpret_cast<T>(addr);
        return true;
    }
    target = make_stub<T>();
    return false;
}

bool load_gl_core_3_3(void* (*proc_loader)(const char* name)) {
    if (s_loaded) {
        return true;
    }

    bool ok = true;

    ok &= load_func(glViewport, "glViewport", proc_loader);
    ok &= load_func(glScissor, "glScissor", proc_loader);
    ok &= load_func(glClearColor, "glClearColor", proc_loader);
    ok &= load_func(glClear, "glClear", proc_loader);
    load_func(glClearDepth, "glClearDepth", proc_loader);
    load_func(glClearStencil, "glClearStencil", proc_loader);
    ok &= load_func(glColorMask, "glColorMask", proc_loader);
    load_func(glDepthMask, "glDepthMask", proc_loader);
    load_func(glDepthFunc, "glDepthFunc", proc_loader);
    ok &= load_func(glBlendFunc, "glBlendFunc", proc_loader);
    ok &= load_func(glBlendFuncSeparate, "glBlendFuncSeparate", proc_loader);
    ok &= load_func(glBlendEquation, "glBlendEquation", proc_loader);
    ok &= load_func(glBlendEquationSeparate, "glBlendEquationSeparate", proc_loader);
    load_func(glCullFace, "glCullFace", proc_loader);
    load_func(glFrontFace, "glFrontFace", proc_loader);
    load_func(glPolygonMode, "glPolygonMode", proc_loader);
    load_func(glLineWidth, "glLineWidth", proc_loader);
    ok &= load_func(glEnable, "glEnable", proc_loader);
    ok &= load_func(glDisable, "glDisable", proc_loader);
    load_func(glIsEnabled, "glIsEnabled", proc_loader);
    ok &= load_func(glPixelStorei, "glPixelStorei", proc_loader);
    load_func(glFlush, "glFlush", proc_loader);
    load_func(glFinish, "glFinish", proc_loader);
    load_func(glReadPixels, "glReadPixels", proc_loader);
    ok &= load_func(glGetError, "glGetError", proc_loader);
    ok &= load_func(glGetIntegerv, "glGetIntegerv", proc_loader);
    load_func(glGetFloatv, "glGetFloatv", proc_loader);
    load_func(glGetBooleanv, "glGetBooleanv", proc_loader);
    ok &= load_func(glGetString, "glGetString", proc_loader);
    load_func(glGetStringi, "glGetStringi", proc_loader);

    ok &= load_func(glDrawArrays, "glDrawArrays", proc_loader);
    ok &= load_func(glDrawElements, "glDrawElements", proc_loader);
    load_func(glDrawArraysInstanced, "glDrawArraysInstanced", proc_loader);
    load_func(glDrawElementsInstanced, "glDrawElementsInstanced", proc_loader);
    load_func(glDrawElementsBaseVertex, "glDrawElementsBaseVertex", proc_loader);

    ok &= load_func(glGenBuffers, "glGenBuffers", proc_loader);
    ok &= load_func(glDeleteBuffers, "glDeleteBuffers", proc_loader);
    ok &= load_func(glBindBuffer, "glBindBuffer", proc_loader);
    ok &= load_func(glBufferData, "glBufferData", proc_loader);
    ok &= load_func(glBufferSubData, "glBufferSubData", proc_loader);
    load_func(glMapBuffer, "glMapBuffer", proc_loader);
    load_func(glMapBufferRange, "glMapBufferRange", proc_loader);
    load_func(glUnmapBuffer, "glUnmapBuffer", proc_loader);

    ok &= load_func(glGenVertexArrays, "glGenVertexArrays", proc_loader);
    ok &= load_func(glDeleteVertexArrays, "glDeleteVertexArrays", proc_loader);
    ok &= load_func(glBindVertexArray, "glBindVertexArray", proc_loader);
    ok &= load_func(glEnableVertexAttribArray, "glEnableVertexAttribArray", proc_loader);
    ok &= load_func(glDisableVertexAttribArray, "glDisableVertexAttribArray", proc_loader);
    ok &= load_func(glVertexAttribPointer, "glVertexAttribPointer", proc_loader);
    load_func(glVertexAttribIPointer, "glVertexAttribIPointer", proc_loader);
    load_func(glVertexAttribDivisor, "glVertexAttribDivisor", proc_loader);

    ok &= load_func(glGenTextures, "glGenTextures", proc_loader);
    ok &= load_func(glDeleteTextures, "glDeleteTextures", proc_loader);
    ok &= load_func(glBindTexture, "glBindTexture", proc_loader);
    ok &= load_func(glActiveTexture, "glActiveTexture", proc_loader);
    ok &= load_func(glTexParameteri, "glTexParameteri", proc_loader);
    load_func(glTexParameterf, "glTexParameterf", proc_loader);
    load_func(glTexParameteriv, "glTexParameteriv", proc_loader);
    load_func(glTexParameterfv, "glTexParameterfv", proc_loader);
    ok &= load_func(glTexImage2D, "glTexImage2D", proc_loader);
    ok &= load_func(glTexSubImage2D, "glTexSubImage2D", proc_loader);
    load_func(glGenerateMipmap, "glGenerateMipmap", proc_loader);

    ok &= load_func(glCreateShader, "glCreateShader", proc_loader);
    ok &= load_func(glDeleteShader, "glDeleteShader", proc_loader);
    ok &= load_func(glShaderSource, "glShaderSource", proc_loader);
    ok &= load_func(glCompileShader, "glCompileShader", proc_loader);
    ok &= load_func(glGetShaderiv, "glGetShaderiv", proc_loader);
    ok &= load_func(glGetShaderInfoLog, "glGetShaderInfoLog", proc_loader);
    ok &= load_func(glCreateProgram, "glCreateProgram", proc_loader);
    ok &= load_func(glDeleteProgram, "glDeleteProgram", proc_loader);
    ok &= load_func(glAttachShader, "glAttachShader", proc_loader);
    ok &= load_func(glDetachShader, "glDetachShader", proc_loader);
    ok &= load_func(glLinkProgram, "glLinkProgram", proc_loader);
    ok &= load_func(glUseProgram, "glUseProgram", proc_loader);
    load_func(glValidateProgram, "glValidateProgram", proc_loader);
    ok &= load_func(glGetProgramiv, "glGetProgramiv", proc_loader);
    ok &= load_func(glGetProgramInfoLog, "glGetProgramInfoLog", proc_loader);
    load_func(glGetAttribLocation, "glGetAttribLocation", proc_loader);
    ok &= load_func(glBindAttribLocation, "glBindAttribLocation", proc_loader);
    ok &= load_func(glGetUniformLocation, "glGetUniformLocation", proc_loader);
    ok &= load_func(glUniform1i, "glUniform1i", proc_loader);
    load_func(glUniform2i, "glUniform2i", proc_loader);
    load_func(glUniform3i, "glUniform3i", proc_loader);
    load_func(glUniform4i, "glUniform4i", proc_loader);
    ok &= load_func(glUniform1f, "glUniform1f", proc_loader);
    load_func(glUniform2f, "glUniform2f", proc_loader);
    load_func(glUniform3f, "glUniform3f", proc_loader);
    ok &= load_func(glUniform4f, "glUniform4f", proc_loader);
    load_func(glUniform1iv, "glUniform1iv", proc_loader);
    load_func(glUniform2iv, "glUniform2iv", proc_loader);
    load_func(glUniform3iv, "glUniform3iv", proc_loader);
    load_func(glUniform4iv, "glUniform4iv", proc_loader);
    load_func(glUniform1fv, "glUniform1fv", proc_loader);
    load_func(glUniform2fv, "glUniform2fv", proc_loader);
    load_func(glUniform3fv, "glUniform3fv", proc_loader);
    load_func(glUniform4fv, "glUniform4fv", proc_loader);
    load_func(glUniformMatrix2fv, "glUniformMatrix2fv", proc_loader);
    load_func(glUniformMatrix3fv, "glUniformMatrix3fv", proc_loader);
    ok &= load_func(glUniformMatrix4fv, "glUniformMatrix4fv", proc_loader);

    load_func(glGenFramebuffers, "glGenFramebuffers", proc_loader);
    load_func(glDeleteFramebuffers, "glDeleteFramebuffers", proc_loader);
    load_func(glBindFramebuffer, "glBindFramebuffer", proc_loader);
    load_func(glFramebufferTexture2D, "glFramebufferTexture2D", proc_loader);
    load_func(glCheckFramebufferStatus, "glCheckFramebufferStatus", proc_loader);
    load_func(glGenRenderbuffers, "glGenRenderbuffers", proc_loader);
    load_func(glDeleteRenderbuffers, "glDeleteRenderbuffers", proc_loader);
    load_func(glBindRenderbuffer, "glBindRenderbuffer", proc_loader);
    load_func(glRenderbufferStorage, "glRenderbufferStorage", proc_loader);
    load_func(glFramebufferRenderbuffer, "glFramebufferRenderbuffer", proc_loader);
    load_func(glBlitFramebuffer, "glBlitFramebuffer", proc_loader);
    load_func(glDrawBuffer, "glDrawBuffer", proc_loader);

    s_loaded = ok;
    return ok;
}

} // namespace regml::glmodern
