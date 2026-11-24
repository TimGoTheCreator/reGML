#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#ifndef GLAPIENTRY
#define GLAPIENTRY
#endif

#ifndef APIENTRY
#define APIENTRY GLAPIENTRY
#endif

namespace regml::glmodern {

using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLbyte = signed char;
using GLshort = short;
using GLint = int;
using GLsizei = int;
using GLubyte = unsigned char;
using GLushort = unsigned short;
using GLuint = unsigned int;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLclampd = double;
using GLvoid = void;
using GLchar = char;
using GLintptr = std::ptrdiff_t;
using GLsizeiptr = std::ptrdiff_t;
using GLint64 = std::int64_t;
using GLuint64 = std::uint64_t;
struct __GLsync;
using GLsync = struct __GLsync*;

#ifndef GL_POINTS
#define GL_POINTS                         0x0000
#define GL_LINES                          0x0001
#define GL_LINE_LOOP                      0x0002
#define GL_LINE_STRIP                     0x0003
#define GL_TRIANGLES                      0x0004
#define GL_TRIANGLE_STRIP                 0x0005
#define GL_TRIANGLE_FAN                   0x0006
#define GL_LINES_ADJACENCY                0x000A
#define GL_LINE_STRIP_ADJACENCY           0x000B
#define GL_TRIANGLES_ADJACENCY            0x000C
#define GL_TRIANGLE_STRIP_ADJACENCY       0x000D
#endif

#ifndef GL_COLOR_BUFFER_BIT
#define GL_DEPTH_BUFFER_BIT               0x00000100
#define GL_STENCIL_BUFFER_BIT             0x00000400
#define GL_COLOR_BUFFER_BIT               0x00004000
#define GL_NEVER                          0x0200
#define GL_LESS                           0x0201
#define GL_EQUAL                          0x0202
#define GL_LEQUAL                         0x0203
#define GL_GREATER                        0x0204
#define GL_NOTEQUAL                       0x0205
#define GL_GEQUAL                         0x0206
#define GL_ALWAYS                         0x0207
#endif

#ifndef GL_FALSE
#define GL_FALSE                          0
#define GL_TRUE                           1
#endif

#ifndef GL_SRC_ALPHA
#define GL_ZERO                           0
#define GL_ONE                            1
#define GL_SRC_COLOR                      0x0300
#define GL_ONE_MINUS_SRC_COLOR            0x0301
#define GL_SRC_ALPHA                      0x0302
#define GL_ONE_MINUS_SRC_ALPHA            0x0303
#define GL_DST_ALPHA                      0x0304
#define GL_ONE_MINUS_DST_ALPHA            0x0305
#define GL_DST_COLOR                      0x0306
#define GL_ONE_MINUS_DST_COLOR            0x0307
#define GL_SRC_ALPHA_SATURATE             0x0308
#define GL_CONSTANT_COLOR                 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR       0x8002
#define GL_CONSTANT_ALPHA                 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA       0x8004
#define GL_FUNC_ADD                       0x8006
#define GL_MIN                            0x8007
#define GL_MAX                            0x8008
#define GL_FUNC_SUBTRACT                  0x800A
#define GL_FUNC_REVERSE_SUBTRACT          0x800B
#define GL_BLEND_EQUATION                 0x8009
#define GL_BLEND_EQUATION_RGB             0x8009
#define GL_BLEND_EQUATION_ALPHA           0x883D
#define GL_BLEND_DST_RGB                  0x80C8
#define GL_BLEND_SRC_RGB                  0x80C9
#define GL_BLEND_DST_ALPHA                0x80CA
#define GL_BLEND_SRC_ALPHA                0x80CB
#define GL_BLEND_COLOR                    0x8005
#endif

#ifndef GL_BLEND
#define GL_BLEND                          0x0BE2
#define GL_CULL_FACE                      0x0B44
#define GL_DEPTH_TEST                     0x0B71
#define GL_STENCIL_TEST                   0x0B90
#define GL_SCISSOR_TEST                   0x0C11
#define GL_DITHER                         0x0BD0
#define GL_MULTISAMPLE                    0x809D
#define GL_PRIMITIVE_RESTART              0x8F9D
#endif

#ifndef GL_FRONT
#define GL_FRONT                          0x0404
#define GL_BACK                           0x0405
#define GL_FRONT_AND_BACK                 0x0408
#define GL_CW                             0x0900
#define GL_CCW                            0x0901
#define GL_POINT                          0x1B00
#define GL_LINE                           0x1B01
#define GL_FILL                           0x1B02
#endif

#ifndef GL_BYTE
#define GL_BYTE                           0x1400
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_SHORT                          0x1402
#define GL_UNSIGNED_SHORT                 0x1403
#define GL_INT                            0x1404
#define GL_UNSIGNED_INT                   0x1405
#define GL_FLOAT                          0x1406
#define GL_DOUBLE                         0x140A
#endif

#ifndef GL_VENDOR
#define GL_VENDOR                         0x1F00
#define GL_RENDERER                       0x1F01
#define GL_VERSION                        0x1F02
#define GL_EXTENSIONS                     0x1F03
#define GL_SHADING_LANGUAGE_VERSION       0x8B8C
#define GL_NUM_EXTENSIONS                 0x821D
#endif

#ifndef GL_TEXTURE_2D
#define GL_TEXTURE_2D                     0x0DE1
#define GL_TEXTURE_3D                     0x806F
#define GL_TEXTURE_2D_ARRAY               0x8C1A
#define GL_TEXTURE_BINDING_2D             0x8069
#define GL_TEXTURE_MAG_FILTER             0x2800
#define GL_TEXTURE_MIN_FILTER             0x2801
#define GL_TEXTURE_WRAP_S                 0x2802
#define GL_TEXTURE_WRAP_T                 0x2803
#define GL_TEXTURE_WRAP_R                 0x8072
#define GL_NEAREST                        0x2600
#define GL_LINEAR                         0x2601
#define GL_NEAREST_MIPMAP_NEAREST         0x2700
#define GL_LINEAR_MIPMAP_NEAREST          0x2701
#define GL_NEAREST_MIPMAP_LINEAR          0x2702
#define GL_LINEAR_MIPMAP_LINEAR           0x2703
#define GL_REPEAT                         0x2901
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_MIRRORED_REPEAT                0x8370
#endif

#ifndef GL_RGBA
#define GL_RED                            0x1903
#define GL_GREEN                          0x1904
#define GL_BLUE                           0x1905
#define GL_ALPHA                          0x1906
#define GL_RGB                            0x1907
#define GL_RGBA                           0x1908
#define GL_BGRA                           0x80E1
#define GL_BGR                            0x80E0
#define GL_RG                             0x8227
#define GL_RG_INTEGER                     0x8228
#define GL_RED_INTEGER                    0x8D94
#define GL_RGB_INTEGER                    0x8D98
#define GL_RGBA_INTEGER                   0x8D99
#define GL_R8                             0x8229
#define GL_RG8                            0x822B
#define GL_RGB8                           0x8051
#define GL_RGBA8                          0x8058
#define GL_SRGB8                          0x8C41
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_RGBA16F                        0x881A
#define GL_RGBA32F                        0x8814
#define GL_DEPTH_COMPONENT                0x1902
#define GL_DEPTH_COMPONENT16              0x81A5
#define GL_DEPTH_COMPONENT24              0x81A6
#define GL_DEPTH_COMPONENT32F             0x8CAC
#define GL_DEPTH24_STENCIL8               0x88F0
#define GL_DEPTH_STENCIL                  0x84F9
#endif

#ifndef GL_TEXTURE0
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_TEXTURE2                       0x84C2
#define GL_TEXTURE3                       0x84C3
#define GL_TEXTURE4                       0x84C4
#define GL_TEXTURE5                       0x84C5
#define GL_TEXTURE6                       0x84C6
#define GL_TEXTURE7                       0x84C7
#define GL_ACTIVE_TEXTURE                 0x84E0
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#define GL_MAX_TEXTURE_SIZE               0x0D33
#endif

#ifndef GL_UNPACK_ALIGNMENT
#define GL_UNPACK_ALIGNMENT               0x0CF5
#define GL_PACK_ALIGNMENT                 0x0D05
#define GL_UNPACK_ROW_LENGTH              0x0CF2
#define GL_PACK_ROW_LENGTH                0x0D02
#endif

#ifndef GL_ARRAY_BUFFER
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_UNIFORM_BUFFER                 0x8A11
#define GL_STREAM_DRAW                    0x88E0
#define GL_STREAM_READ                    0x88E1
#define GL_STREAM_COPY                    0x88E2
#define GL_STATIC_DRAW                    0x88E4
#define GL_STATIC_READ                    0x88E5
#define GL_STATIC_COPY                    0x88E6
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_DYNAMIC_READ                   0x88E9
#define GL_DYNAMIC_COPY                   0x88EA
#define GL_READ_ONLY                      0x88B8
#define GL_WRITE_ONLY                     0x88B9
#define GL_READ_WRITE                     0x88BA
#define GL_BUFFER_SIZE                    0x8764
#define GL_BUFFER_USAGE                   0x8765
#define GL_ARRAY_BUFFER_BINDING           0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING   0x8895
#define GL_MAP_READ_BIT                   0x0001
#define GL_MAP_WRITE_BIT                  0x0002
#define GL_MAP_INVALIDATE_RANGE_BIT       0x0004
#define GL_MAP_INVALIDATE_BUFFER_BIT      0x0008
#define GL_MAP_FLUSH_EXPLICIT_BIT         0x0010
#define GL_MAP_UNSYNCHRONIZED_BIT         0x0020
#endif

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING           0x85B5
#endif

#ifndef GL_FRAGMENT_SHADER
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_GEOMETRY_SHADER                0x8DD9
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_VALIDATE_STATUS                0x8B83
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_ATTACHED_SHADERS               0x8B85
#define GL_ACTIVE_UNIFORMS                0x8B86
#define GL_ACTIVE_UNIFORM_MAX_LENGTH      0x8B87
#define GL_SHADER_SOURCE_LENGTH           0x8B88
#define GL_CURRENT_PROGRAM                0x8B8D
#endif

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_COLOR_ATTACHMENT1              0x8CE1
#define GL_COLOR_ATTACHMENT2              0x8CE2
#define GL_COLOR_ATTACHMENT3              0x8CE3
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_STENCIL_ATTACHMENT             0x8D20
#define GL_DEPTH_STENCIL_ATTACHMENT       0x821A
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT 0x8CD6
#define GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT 0x8CD7
#define GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER 0x8CDB
#define GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER 0x8CDC
#define GL_FRAMEBUFFER_UNSUPPORTED        0x8CDD
#define GL_FRAMEBUFFER_BINDING            0x8CA6
#define GL_RENDERBUFFER_BINDING           0x8CA7
#endif

#ifndef GL_VIEWPORT
#define GL_VIEWPORT                       0x0BA2
#define GL_SCISSOR_BOX                    0x0C10
#endif

#ifndef GL_NO_ERROR
#define GL_NO_ERROR                       0
#define GL_INVALID_ENUM                   0x0500
#define GL_INVALID_VALUE                  0x0501
#define GL_INVALID_OPERATION              0x0502
#define GL_STACK_OVERFLOW                 0x0503
#define GL_STACK_UNDERFLOW                0x0504
#define GL_OUT_OF_MEMORY                  0x0505
#define GL_INVALID_FRAMEBUFFER_OPERATION  0x0506
#endif

using PFNGLVIEWPORTPROC = void (GLAPIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height);
using PFNGLSCISSORPROC = void (GLAPIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height);
using PFNGLCLEARCOLORPROC = void (GLAPIENTRY *)(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
using PFNGLCLEARPROC = void (GLAPIENTRY *)(GLbitfield mask);
using PFNGLCLEARDEPTHPROC = void (GLAPIENTRY *)(GLdouble depth);
using PFNGLCLEARSTENCILPROC = void (GLAPIENTRY *)(GLint s);
using PFNGLCOLORMASKPROC = void (GLAPIENTRY *)(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
using PFNGLDEPTHMASKPROC = void (GLAPIENTRY *)(GLboolean flag);
using PFNGLDEPTHFUNCPROC = void (GLAPIENTRY *)(GLenum func);
using PFNGLBLENDFUNCPROC = void (GLAPIENTRY *)(GLenum sfactor, GLenum dfactor);
using PFNGLBLENDFUNCSEPARATEPROC = void (GLAPIENTRY *)(GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha);
using PFNGLBLENDEQUATIONPROC = void (GLAPIENTRY *)(GLenum mode);
using PFNGLBLENDEQUATIONSEPARATEPROC = void (GLAPIENTRY *)(GLenum modeRGB, GLenum modeAlpha);
using PFNGLCULLFACEPROC = void (GLAPIENTRY *)(GLenum mode);
using PFNGLFRONTFACEPROC = void (GLAPIENTRY *)(GLenum mode);
using PFNGLPOLYGONMODEPROC = void (GLAPIENTRY *)(GLenum face, GLenum mode);
using PFNGLLINEWIDTHPROC = void (GLAPIENTRY *)(GLfloat width);
using PFNGLENABLEPROC = void (GLAPIENTRY *)(GLenum cap);
using PFNGLDISABLEPROC = void (GLAPIENTRY *)(GLenum cap);
using PFNGLISENABLEDPROC = GLboolean (GLAPIENTRY *)(GLenum cap);
using PFNGLPIXELSTOREIPROC = void (GLAPIENTRY *)(GLenum pname, GLint param);
using PFNGLFLUSHPROC = void (GLAPIENTRY *)(void);
using PFNGLFINISHPROC = void (GLAPIENTRY *)(void);
using PFNGLREADPIXELSPROC = void (GLAPIENTRY *)(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
using PFNGLGETERRORPROC = GLenum (GLAPIENTRY *)(void);
using PFNGLGETINTEGERVPROC = void (GLAPIENTRY *)(GLenum pname, GLint *data);
using PFNGLGETFLOATVPROC = void (GLAPIENTRY *)(GLenum pname, GLfloat *data);
using PFNGLGETBOOLEANVPROC = void (GLAPIENTRY *)(GLenum pname, GLboolean *data);
using PFNGLGETSTRINGPROC = const GLubyte * (GLAPIENTRY *)(GLenum pname);
using PFNGLGETSTRINGIPROC = const GLubyte * (GLAPIENTRY *)(GLenum pname, GLuint index);

using PFNGLDRAWARRAYSPROC = void (GLAPIENTRY *)(GLenum mode, GLint first, GLsizei count);
using PFNGLDRAWELEMENTSPROC = void (GLAPIENTRY *)(GLenum mode, GLsizei count, GLenum type, const void *indices);
using PFNGLDRAWARRAYSINSTANCEDPROC = void (GLAPIENTRY *)(GLenum mode, GLint first, GLsizei count, GLsizei instancecount);
using PFNGLDRAWELEMENTSINSTANCEDPROC = void (GLAPIENTRY *)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLsizei instancecount);
using PFNGLDRAWELEMENTSBASEVERTEXPROC = void (GLAPIENTRY *)(GLenum mode, GLsizei count, GLenum type, const void *indices, GLint basevertex);

using PFNGLGENBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, GLuint *buffers);
using PFNGLDELETEBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, const GLuint *buffers);
using PFNGLBINDBUFFERPROC = void (GLAPIENTRY *)(GLenum target, GLuint buffer);
using PFNGLBUFFERDATAPROC = void (GLAPIENTRY *)(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
using PFNGLBUFFERSUBDATAPROC = void (GLAPIENTRY *)(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
using PFNGLMAPBUFFERPROC = void * (GLAPIENTRY *)(GLenum target, GLenum access);
using PFNGLMAPBUFFERRANGEPROC = void * (GLAPIENTRY *)(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
using PFNGLUNMAPBUFFERPROC = GLboolean (GLAPIENTRY *)(GLenum target);

using PFNGLGENVERTEXARRAYSPROC = void (GLAPIENTRY *)(GLsizei n, GLuint *arrays);
using PFNGLDELETEVERTEXARRAYSPROC = void (GLAPIENTRY *)(GLsizei n, const GLuint *arrays);
using PFNGLBINDVERTEXARRAYPROC = void (GLAPIENTRY *)(GLuint array);
using PFNGLENABLEVERTEXATTRIBARRAYPROC = void (GLAPIENTRY *)(GLuint index);
using PFNGLDISABLEVERTEXATTRIBARRAYPROC = void (GLAPIENTRY *)(GLuint index);
using PFNGLVERTEXATTRIBPOINTERPROC = void (GLAPIENTRY *)(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
using PFNGLVERTEXATTRIBIPOINTERPROC = void (GLAPIENTRY *)(GLuint index, GLint size, GLenum type, GLsizei stride, const void *pointer);
using PFNGLVERTEXATTRIBDIVISORPROC = void (GLAPIENTRY *)(GLuint index, GLuint divisor);

using PFNGLGENTEXTURESPROC = void (GLAPIENTRY *)(GLsizei n, GLuint *textures);
using PFNGLDELETETEXTURESPROC = void (GLAPIENTRY *)(GLsizei n, const GLuint *textures);
using PFNGLBINDTEXTUREPROC = void (GLAPIENTRY *)(GLenum target, GLuint texture);
using PFNGLACTIVETEXTUREPROC = void (GLAPIENTRY *)(GLenum texture);
using PFNGLTEXPARAMETERIPROC = void (GLAPIENTRY *)(GLenum target, GLenum pname, GLint param);
using PFNGLTEXPARAMETERFPROC = void (GLAPIENTRY *)(GLenum target, GLenum pname, GLfloat param);
using PFNGLTEXPARAMETERIVPROC = void (GLAPIENTRY *)(GLenum target, GLenum pname, const GLint *params);
using PFNGLTEXPARAMETERFVPROC = void (GLAPIENTRY *)(GLenum target, GLenum pname, const GLfloat *params);
using PFNGLTEXIMAGE2DPROC = void (GLAPIENTRY *)(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels);
using PFNGLTEXSUBIMAGE2DPROC = void (GLAPIENTRY *)(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels);
using PFNGLGENERATEMIPMAPPROC = void (GLAPIENTRY *)(GLenum target);

using PFNGLCREATESHADERPROC = GLuint (GLAPIENTRY *)(GLenum type);
using PFNGLDELETESHADERPROC = void (GLAPIENTRY *)(GLuint shader);
using PFNGLSHADERSOURCEPROC = void (GLAPIENTRY *)(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
using PFNGLCOMPILESHADERPROC = void (GLAPIENTRY *)(GLuint shader);
using PFNGLGETSHADERIVPROC = void (GLAPIENTRY *)(GLuint shader, GLenum pname, GLint *params);
using PFNGLGETSHADERINFOLOGPROC = void (GLAPIENTRY *)(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
using PFNGLCREATEPROGRAMPROC = GLuint (GLAPIENTRY *)(void);
using PFNGLDELETEPROGRAMPROC = void (GLAPIENTRY *)(GLuint program);
using PFNGLATTACHSHADERPROC = void (GLAPIENTRY *)(GLuint program, GLuint shader);
using PFNGLDETACHSHADERPROC = void (GLAPIENTRY *)(GLuint program, GLuint shader);
using PFNGLLINKPROGRAMPROC = void (GLAPIENTRY *)(GLuint program);
using PFNGLUSEPROGRAMPROC = void (GLAPIENTRY *)(GLuint program);
using PFNGLVALIDATEPROGRAMPROC = void (GLAPIENTRY *)(GLuint program);
using PFNGLGETPROGRAMIVPROC = void (GLAPIENTRY *)(GLuint program, GLenum pname, GLint *params);
using PFNGLGETPROGRAMINFOLOGPROC = void (GLAPIENTRY *)(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
using PFNGLGETATTRIBLOCATIONPROC = GLint (GLAPIENTRY *)(GLuint program, const GLchar *name);
using PFNGLBINDATTRIBLOCATIONPROC = void (GLAPIENTRY *)(GLuint program, GLuint index, const GLchar *name);
using PFNGLGETUNIFORMLOCATIONPROC = GLint (GLAPIENTRY *)(GLuint program, const GLchar *name);
using PFNGLUNIFORM1IPROC = void (GLAPIENTRY *)(GLint location, GLint v0);
using PFNGLUNIFORM2IPROC = void (GLAPIENTRY *)(GLint location, GLint v0, GLint v1);
using PFNGLUNIFORM3IPROC = void (GLAPIENTRY *)(GLint location, GLint v0, GLint v1, GLint v2);
using PFNGLUNIFORM4IPROC = void (GLAPIENTRY *)(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
using PFNGLUNIFORM1FPROC = void (GLAPIENTRY *)(GLint location, GLfloat v0);
using PFNGLUNIFORM2FPROC = void (GLAPIENTRY *)(GLint location, GLfloat v0, GLfloat v1);
using PFNGLUNIFORM3FPROC = void (GLAPIENTRY *)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
using PFNGLUNIFORM4FPROC = void (GLAPIENTRY *)(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
using PFNGLUNIFORM1IVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLint *value);
using PFNGLUNIFORM2IVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLint *value);
using PFNGLUNIFORM3IVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLint *value);
using PFNGLUNIFORM4IVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLint *value);
using PFNGLUNIFORM1FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLfloat *value);
using PFNGLUNIFORM2FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLfloat *value);
using PFNGLUNIFORM3FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLfloat *value);
using PFNGLUNIFORM4FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, const GLfloat *value);
using PFNGLUNIFORMMATRIX2FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
using PFNGLUNIFORMMATRIX3FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);
using PFNGLUNIFORMMATRIX4FVPROC = void (GLAPIENTRY *)(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value);

using PFNGLGENFRAMEBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, GLuint *framebuffers);
using PFNGLDELETEFRAMEBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, const GLuint *framebuffers);
using PFNGLBINDFRAMEBUFFERPROC = void (GLAPIENTRY *)(GLenum target, GLuint framebuffer);
using PFNGLFRAMEBUFFERTEXTURE2DPROC = void (GLAPIENTRY *)(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
using PFNGLCHECKFRAMEBUFFERSTATUSPROC = GLenum (GLAPIENTRY *)(GLenum target);
using PFNGLGENRENDERBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, GLuint *renderbuffers);
using PFNGLDELETERENDERBUFFERSPROC = void (GLAPIENTRY *)(GLsizei n, const GLuint *renderbuffers);
using PFNGLBINDRENDERBUFFERPROC = void (GLAPIENTRY *)(GLenum target, GLuint renderbuffer);
using PFNGLRENDERBUFFERSTORAGEPROC = void (GLAPIENTRY *)(GLenum target, GLenum internalformat, GLsizei width, GLsizei height);
using PFNGLFRAMEBUFFERRENDERBUFFERPROC = void (GLAPIENTRY *)(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer);
using PFNGLBLITFRAMEBUFFERPROC = void (GLAPIENTRY *)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
using PFNGLDRAWBUFFERPROC = void (GLAPIENTRY *)(GLenum buf);

extern PFNGLVIEWPORTPROC glViewport;
extern PFNGLSCISSORPROC glScissor;
extern PFNGLCLEARCOLORPROC glClearColor;
extern PFNGLCLEARPROC glClear;
extern PFNGLCLEARDEPTHPROC glClearDepth;
extern PFNGLCLEARSTENCILPROC glClearStencil;
extern PFNGLCOLORMASKPROC glColorMask;
extern PFNGLDEPTHMASKPROC glDepthMask;
extern PFNGLDEPTHFUNCPROC glDepthFunc;
extern PFNGLBLENDFUNCPROC glBlendFunc;
extern PFNGLBLENDFUNCSEPARATEPROC glBlendFuncSeparate;
extern PFNGLBLENDEQUATIONPROC glBlendEquation;
extern PFNGLBLENDEQUATIONSEPARATEPROC glBlendEquationSeparate;
extern PFNGLCULLFACEPROC glCullFace;
extern PFNGLFRONTFACEPROC glFrontFace;
extern PFNGLPOLYGONMODEPROC glPolygonMode;
extern PFNGLLINEWIDTHPROC glLineWidth;
extern PFNGLENABLEPROC glEnable;
extern PFNGLDISABLEPROC glDisable;
extern PFNGLISENABLEDPROC glIsEnabled;
extern PFNGLPIXELSTOREIPROC glPixelStorei;
extern PFNGLFLUSHPROC glFlush;
extern PFNGLFINISHPROC glFinish;
extern PFNGLREADPIXELSPROC glReadPixels;
extern PFNGLGETERRORPROC glGetError;
extern PFNGLGETINTEGERVPROC glGetIntegerv;
extern PFNGLGETFLOATVPROC glGetFloatv;
extern PFNGLGETBOOLEANVPROC glGetBooleanv;
extern PFNGLGETSTRINGPROC glGetString;
extern PFNGLGETSTRINGIPROC glGetStringi;

extern PFNGLDRAWARRAYSPROC glDrawArrays;
extern PFNGLDRAWELEMENTSPROC glDrawElements;
extern PFNGLDRAWARRAYSINSTANCEDPROC glDrawArraysInstanced;
extern PFNGLDRAWELEMENTSINSTANCEDPROC glDrawElementsInstanced;
extern PFNGLDRAWELEMENTSBASEVERTEXPROC glDrawElementsBaseVertex;

extern PFNGLGENBUFFERSPROC glGenBuffers;
extern PFNGLDELETEBUFFERSPROC glDeleteBuffers;
extern PFNGLBINDBUFFERPROC glBindBuffer;
extern PFNGLBUFFERDATAPROC glBufferData;
extern PFNGLBUFFERSUBDATAPROC glBufferSubData;
extern PFNGLMAPBUFFERPROC glMapBuffer;
extern PFNGLMAPBUFFERRANGEPROC glMapBufferRange;
extern PFNGLUNMAPBUFFERPROC glUnmapBuffer;

extern PFNGLGENVERTEXARRAYSPROC glGenVertexArrays;
extern PFNGLDELETEVERTEXARRAYSPROC glDeleteVertexArrays;
extern PFNGLBINDVERTEXARRAYPROC glBindVertexArray;
extern PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray;
extern PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray;
extern PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer;
extern PFNGLVERTEXATTRIBIPOINTERPROC glVertexAttribIPointer;
extern PFNGLVERTEXATTRIBDIVISORPROC glVertexAttribDivisor;

extern PFNGLGENTEXTURESPROC glGenTextures;
extern PFNGLDELETETEXTURESPROC glDeleteTextures;
extern PFNGLBINDTEXTUREPROC glBindTexture;
extern PFNGLACTIVETEXTUREPROC glActiveTexture;
extern PFNGLTEXPARAMETERIPROC glTexParameteri;
extern PFNGLTEXPARAMETERFPROC glTexParameterf;
extern PFNGLTEXPARAMETERIVPROC glTexParameteriv;
extern PFNGLTEXPARAMETERFVPROC glTexParameterfv;
extern PFNGLTEXIMAGE2DPROC glTexImage2D;
extern PFNGLTEXSUBIMAGE2DPROC glTexSubImage2D;
extern PFNGLGENERATEMIPMAPPROC glGenerateMipmap;

extern PFNGLCREATESHADERPROC glCreateShader;
extern PFNGLDELETESHADERPROC glDeleteShader;
extern PFNGLSHADERSOURCEPROC glShaderSource;
extern PFNGLCOMPILESHADERPROC glCompileShader;
extern PFNGLGETSHADERIVPROC glGetShaderiv;
extern PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog;
extern PFNGLCREATEPROGRAMPROC glCreateProgram;
extern PFNGLDELETEPROGRAMPROC glDeleteProgram;
extern PFNGLATTACHSHADERPROC glAttachShader;
extern PFNGLDETACHSHADERPROC glDetachShader;
extern PFNGLLINKPROGRAMPROC glLinkProgram;
extern PFNGLUSEPROGRAMPROC glUseProgram;
extern PFNGLVALIDATEPROGRAMPROC glValidateProgram;
extern PFNGLGETPROGRAMIVPROC glGetProgramiv;
extern PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog;
extern PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation;
extern PFNGLBINDATTRIBLOCATIONPROC glBindAttribLocation;
extern PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation;
extern PFNGLUNIFORM1IPROC glUniform1i;
extern PFNGLUNIFORM2IPROC glUniform2i;
extern PFNGLUNIFORM3IPROC glUniform3i;
extern PFNGLUNIFORM4IPROC glUniform4i;
extern PFNGLUNIFORM1FPROC glUniform1f;
extern PFNGLUNIFORM2FPROC glUniform2f;
extern PFNGLUNIFORM3FPROC glUniform3f;
extern PFNGLUNIFORM4FPROC glUniform4f;
extern PFNGLUNIFORM1IVPROC glUniform1iv;
extern PFNGLUNIFORM2IVPROC glUniform2iv;
extern PFNGLUNIFORM3IVPROC glUniform3iv;
extern PFNGLUNIFORM4IVPROC glUniform4iv;
extern PFNGLUNIFORM1FVPROC glUniform1fv;
extern PFNGLUNIFORM2FVPROC glUniform2fv;
extern PFNGLUNIFORM3FVPROC glUniform3fv;
extern PFNGLUNIFORM4FVPROC glUniform4fv;
extern PFNGLUNIFORMMATRIX2FVPROC glUniformMatrix2fv;
extern PFNGLUNIFORMMATRIX3FVPROC glUniformMatrix3fv;
extern PFNGLUNIFORMMATRIX4FVPROC glUniformMatrix4fv;

extern PFNGLGENFRAMEBUFFERSPROC glGenFramebuffers;
extern PFNGLDELETEFRAMEBUFFERSPROC glDeleteFramebuffers;
extern PFNGLBINDFRAMEBUFFERPROC glBindFramebuffer;
extern PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D;
extern PFNGLCHECKFRAMEBUFFERSTATUSPROC glCheckFramebufferStatus;
extern PFNGLGENRENDERBUFFERSPROC glGenRenderbuffers;
extern PFNGLDELETERENDERBUFFERSPROC glDeleteRenderbuffers;
extern PFNGLBINDRENDERBUFFERPROC glBindRenderbuffer;
extern PFNGLRENDERBUFFERSTORAGEPROC glRenderbufferStorage;
extern PFNGLFRAMEBUFFERRENDERBUFFERPROC glFramebufferRenderbuffer;
extern PFNGLBLITFRAMEBUFFERPROC glBlitFramebuffer;
extern PFNGLDRAWBUFFERPROC glDrawBuffer;

bool load_gl_core_3_3(void* (*proc_loader)(const char* name) = nullptr);

[[nodiscard]] bool is_gl_core_loaded() noexcept;

} // namespace regml::glmodern

using regml::glmodern::GLenum;
using regml::glmodern::GLboolean;
using regml::glmodern::GLbitfield;
using regml::glmodern::GLbyte;
using regml::glmodern::GLshort;
using regml::glmodern::GLint;
using regml::glmodern::GLsizei;
using regml::glmodern::GLubyte;
using regml::glmodern::GLushort;
using regml::glmodern::GLuint;
using regml::glmodern::GLfloat;
using regml::glmodern::GLclampf;
using regml::glmodern::GLdouble;
using regml::glmodern::GLclampd;
using regml::glmodern::GLchar;
using regml::glmodern::GLintptr;
using regml::glmodern::GLsizeiptr;
using regml::glmodern::GLsync;

using regml::glmodern::glViewport;
using regml::glmodern::glScissor;
using regml::glmodern::glClearColor;
using regml::glmodern::glClear;
using regml::glmodern::glClearDepth;
using regml::glmodern::glClearStencil;
using regml::glmodern::glColorMask;
using regml::glmodern::glDepthMask;
using regml::glmodern::glDepthFunc;
using regml::glmodern::glBlendFunc;
using regml::glmodern::glBlendFuncSeparate;
using regml::glmodern::glBlendEquation;
using regml::glmodern::glBlendEquationSeparate;
using regml::glmodern::glCullFace;
using regml::glmodern::glFrontFace;
using regml::glmodern::glPolygonMode;
using regml::glmodern::glLineWidth;
using regml::glmodern::glEnable;
using regml::glmodern::glDisable;
using regml::glmodern::glIsEnabled;
using regml::glmodern::glPixelStorei;
using regml::glmodern::glFlush;
using regml::glmodern::glFinish;
using regml::glmodern::glReadPixels;
using regml::glmodern::glGetError;
using regml::glmodern::glGetIntegerv;
using regml::glmodern::glGetFloatv;
using regml::glmodern::glGetBooleanv;
using regml::glmodern::glGetString;
using regml::glmodern::glGetStringi;

using regml::glmodern::glDrawArrays;
using regml::glmodern::glDrawElements;
using regml::glmodern::glDrawArraysInstanced;
using regml::glmodern::glDrawElementsInstanced;
using regml::glmodern::glDrawElementsBaseVertex;

using regml::glmodern::glGenBuffers;
using regml::glmodern::glDeleteBuffers;
using regml::glmodern::glBindBuffer;
using regml::glmodern::glBufferData;
using regml::glmodern::glBufferSubData;
using regml::glmodern::glMapBuffer;
using regml::glmodern::glMapBufferRange;
using regml::glmodern::glUnmapBuffer;

using regml::glmodern::glGenVertexArrays;
using regml::glmodern::glDeleteVertexArrays;
using regml::glmodern::glBindVertexArray;
using regml::glmodern::glEnableVertexAttribArray;
using regml::glmodern::glDisableVertexAttribArray;
using regml::glmodern::glVertexAttribPointer;
using regml::glmodern::glVertexAttribIPointer;
using regml::glmodern::glVertexAttribDivisor;

using regml::glmodern::glGenTextures;
using regml::glmodern::glDeleteTextures;
using regml::glmodern::glBindTexture;
using regml::glmodern::glActiveTexture;
using regml::glmodern::glTexParameteri;
using regml::glmodern::glTexParameterf;
using regml::glmodern::glTexParameteriv;
using regml::glmodern::glTexParameterfv;
using regml::glmodern::glTexImage2D;
using regml::glmodern::glTexSubImage2D;
using regml::glmodern::glGenerateMipmap;

using regml::glmodern::glCreateShader;
using regml::glmodern::glDeleteShader;
using regml::glmodern::glShaderSource;
using regml::glmodern::glCompileShader;
using regml::glmodern::glGetShaderiv;
using regml::glmodern::glGetShaderInfoLog;
using regml::glmodern::glCreateProgram;
using regml::glmodern::glDeleteProgram;
using regml::glmodern::glAttachShader;
using regml::glmodern::glDetachShader;
using regml::glmodern::glLinkProgram;
using regml::glmodern::glUseProgram;
using regml::glmodern::glValidateProgram;
using regml::glmodern::glGetProgramiv;
using regml::glmodern::glGetProgramInfoLog;
using regml::glmodern::glGetAttribLocation;
using regml::glmodern::glBindAttribLocation;
using regml::glmodern::glGetUniformLocation;
using regml::glmodern::glUniform1i;
using regml::glmodern::glUniform2i;
using regml::glmodern::glUniform3i;
using regml::glmodern::glUniform4i;
using regml::glmodern::glUniform1f;
using regml::glmodern::glUniform2f;
using regml::glmodern::glUniform3f;
using regml::glmodern::glUniform4f;
using regml::glmodern::glUniform1iv;
using regml::glmodern::glUniform2iv;
using regml::glmodern::glUniform3iv;
using regml::glmodern::glUniform4iv;
using regml::glmodern::glUniform1fv;
using regml::glmodern::glUniform2fv;
using regml::glmodern::glUniform3fv;
using regml::glmodern::glUniform4fv;
using regml::glmodern::glUniformMatrix2fv;
using regml::glmodern::glUniformMatrix3fv;
using regml::glmodern::glUniformMatrix4fv;

using regml::glmodern::glGenFramebuffers;
using regml::glmodern::glDeleteFramebuffers;
using regml::glmodern::glBindFramebuffer;
using regml::glmodern::glFramebufferTexture2D;
using regml::glmodern::glCheckFramebufferStatus;
using regml::glmodern::glGenRenderbuffers;
using regml::glmodern::glDeleteRenderbuffers;
using regml::glmodern::glBindRenderbuffer;
using regml::glmodern::glRenderbufferStorage;
using regml::glmodern::glFramebufferRenderbuffer;
using regml::glmodern::glBlitFramebuffer;
using regml::glmodern::glDrawBuffer;
