// OpenGL declarations, inlined so the project has no dependency on GLEW, GLAD or
// the Khronos headers -- the same rule the DX11 bridge applies to the ReShade
// SDK and the Vulkan port applies to the loader.
//
// Windows ships an opengl32.dll that exports GL 1.1 and the WGL entry points and
// nothing else; everything from GL 1.2 on has to come from wglGetProcAddress,
// resolved against the context that is current on the calling thread. Both
// halves land in one table (GL), so the rest of the bridge never has to care
// which side a function came from.

#pragma once

#include <stdint.h>

typedef unsigned int   GLenum;
typedef unsigned char  GLboolean;
typedef unsigned int   GLbitfield;
typedef int            GLint;
typedef int            GLsizei;
typedef unsigned char  GLubyte;
typedef unsigned int   GLuint;
typedef float          GLfloat;
typedef char           GLchar;
typedef uint64_t       GLuint64;

#define GL_FALSE                              0
#define GL_TRUE                               1
#define GL_NO_ERROR                           0
#define GL_NONE                               0

#define GL_TRIANGLES                          0x0004
#define GL_DEPTH_BUFFER_BIT                   0x00000100
#define GL_COLOR_BUFFER_BIT                   0x00004000
#define GL_BACK                               0x0405
#define GL_CULL_FACE                          0x0B44
#define GL_DEPTH_TEST                         0x0B71
#define GL_DEPTH_WRITEMASK                    0x0B72
#define GL_STENCIL_TEST                       0x0B90
#define GL_VIEWPORT                           0x0BA2
#define GL_MODELVIEW_MATRIX                   0x0BA6
#define GL_PROJECTION_MATRIX                  0x0BA7
#define GL_BLEND                              0x0BE2
#define GL_SCISSOR_TEST                       0x0C11
#define GL_COLOR_WRITEMASK                    0x0C23
#define GL_DRAW_BUFFER                        0x0C01
#define GL_READ_BUFFER                        0x0C02
#define GL_UNPACK_ALIGNMENT                   0x0CF5
#define GL_PACK_ALIGNMENT                     0x0D05
#define GL_MAX_DRAW_BUFFERS                   0x8824
#define GL_DRAW_BUFFER0                       0x8825
#define GL_TEXTURE_2D                         0x0DE1
#define GL_TEXTURE_WIDTH                      0x1000
#define GL_TEXTURE_HEIGHT                     0x1001
#define GL_TEXTURE_INTERNAL_FORMAT            0x1003
#define GL_UNSIGNED_BYTE                      0x1401
#define GL_FLOAT                              0x1406
#define GL_DEPTH                              0x1801
#define GL_STENCIL                            0x1802
#define GL_DEPTH_COMPONENT                    0x1902
#define GL_RED                                0x1903
#define GL_RGBA                               0x1908
#define GL_TEXTURE                            0x1702
#define GL_VENDOR                             0x1F00
#define GL_RENDERER                           0x1F01
#define GL_VERSION                            0x1F02
#define GL_EXTENSIONS                         0x1F03
#define GL_NEAREST                            0x2600
#define GL_LINEAR                             0x2601
#define GL_TEXTURE_MAG_FILTER                 0x2800
#define GL_TEXTURE_MIN_FILTER                 0x2801
#define GL_TEXTURE_WRAP_S                     0x2802
#define GL_TEXTURE_WRAP_T                     0x2803
#define GL_RGBA8                              0x8058
#define GL_RGB10_A2                           0x8059
#define GL_RGBA16                             0x805B
#define GL_TEXTURE_BINDING_2D                 0x8069
#define GL_SAMPLE_BUFFERS                     0x80A8
#define GL_SAMPLES                            0x80A9
#define GL_CLAMP_TO_EDGE                      0x812F
#define GL_DEPTH_COMPONENT16                  0x81A5
#define GL_DEPTH_COMPONENT24                  0x81A6
#define GL_DEPTH_COMPONENT32                  0x81A7
// The attachment component sizes run RED, GREEN, BLUE, ALPHA, DEPTH, STENCIL
// from 0x8212. Getting DEPTH wrong by three lands on GREEN_SIZE, which on a
// depth attachment answers zero -- so the default framebuffer reports no depth
// at all and DLSS runs blind. Spelt out in order here so the next one is not
// counted off by hand.
#define GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE     0x8212
#define GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE   0x8213
#define GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE    0x8214
#define GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE   0x8215
#define GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE   0x8216
#define GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE 0x8217
#define GL_DEPTH_STENCIL_ATTACHMENT           0x821A
#define GL_MAJOR_VERSION                      0x821B
#define GL_MINOR_VERSION                      0x821C
#define GL_NUM_EXTENSIONS                     0x821D
#define GL_R32F                               0x822E
#define GL_RG16F                              0x822F
#define GL_RG32F                              0x8230
#define GL_RG                                 0x8227
#define GL_RGBA32F                            0x8814
#define GL_RGBA16F                            0x881A
#define GL_RGB16F                             0x881B
#define GL_ARRAY_BUFFER_BINDING               0x8894
#define GL_TEXTURE_COMPARE_MODE               0x884C
#define GL_DEPTH24_STENCIL8                   0x88F0
#define GL_FRAGMENT_SHADER                    0x8B30
#define GL_VERTEX_SHADER                      0x8B31
#define GL_COMPILE_STATUS                     0x8B81
#define GL_LINK_STATUS                        0x8B82
#define GL_INFO_LOG_LENGTH                    0x8B84
#define GL_CURRENT_PROGRAM                    0x8B8D
#define GL_TEXTURE0                           0x84C0
#define GL_ACTIVE_TEXTURE                     0x84E0
#define GL_DEPTH_STENCIL                      0x84F9
#define GL_UNSIGNED_INT_24_8                  0x84FA
#define GL_VERTEX_ARRAY_BINDING               0x85B5
#define GL_R11F_G11F_B10F                     0x8C3A
#define GL_SRGB8_ALPHA8                       0x8C43
#define GL_DRAW_FRAMEBUFFER_BINDING           0x8CA6
#define GL_READ_FRAMEBUFFER                   0x8CA8
#define GL_DRAW_FRAMEBUFFER                   0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING           0x8CAA
#define GL_RENDERBUFFER_SAMPLES               0x8CAB
#define GL_DEPTH_COMPONENT32F                 0x8CAC
#define GL_DEPTH32F_STENCIL8                  0x8CAD
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE 0x8CD0
#define GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME 0x8CD1
#define GL_FRAMEBUFFER_COMPLETE               0x8CD5
#define GL_MAX_COLOR_ATTACHMENTS              0x8CDF
#define GL_COLOR_ATTACHMENT0                  0x8CE0
#define GL_COLOR_ATTACHMENT1                  0x8CE1
#define GL_DEPTH_ATTACHMENT                   0x8D00
#define GL_FRAMEBUFFER                        0x8D40
#define GL_RENDERBUFFER                       0x8D41
#define GL_RENDERBUFFER_WIDTH                 0x8D42
#define GL_RENDERBUFFER_HEIGHT                0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT       0x8D44
#define GL_FRAMEBUFFER_SRGB                   0x8DB9
#define GL_TEXTURE_2D_MULTISAMPLE             0x9100
#define GL_TEXTURE_BINDING_2D_MULTISAMPLE     0x9104
#define GL_TEXTURE_SAMPLES                    0x9106
#define GL_CONTEXT_PROFILE_MASK               0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT           0x00000001

// ---- EXT_memory_object / EXT_memory_object_win32 --------------------------
#define GL_TEXTURE_TILING_EXT                 0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT        0x9581
#define GL_OPTIMAL_TILING_EXT                 0x9584
#define GL_LINEAR_TILING_EXT                  0x9585
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT       0x9587
#define GL_HANDLE_TYPE_D3D12_RESOURCE_EXT     0x958A
#define GL_DEVICE_UUID_EXT                    0x9597
#define GL_DRIVER_UUID_EXT                    0x9598
#define GL_DEVICE_LUID_EXT                    0x9599
#define GL_DEVICE_NODE_MASK_EXT               0x959A
#define GL_UUID_SIZE_EXT                      16
#define GL_LUID_SIZE_EXT                      8
#define GL_LAYOUT_GENERAL_EXT                 0x958D
#define GL_LAYOUT_COLOR_ATTACHMENT_EXT        0x958E
#define GL_LAYOUT_SHADER_READ_ONLY_EXT        0x9591
#define GL_LAYOUT_TRANSFER_SRC_EXT            0x9592
#define GL_LAYOUT_TRANSFER_DST_EXT            0x9593

// ---- EXT_semaphore / EXT_semaphore_win32 ----------------------------------
#define GL_HANDLE_TYPE_D3D12_FENCE_EXT        0x9594
#define GL_D3D12_FENCE_VALUE_EXT              0x9595

// ---------------------------------------------------------------------------
// The entry-point table.
//
// Every member is a plain function pointer resolved once per GL context. A null
// entry is a fact about the driver or the context, never a reason to crash: the
// bring-up check in GlResolve names exactly what was missing, and after that
// nothing optional is called without testing it first.
// ---------------------------------------------------------------------------
struct GlApi
{
    // -- GL 1.1, straight out of the opengl32.dll export table --------------
    const GLubyte *(__stdcall *GetString)(GLenum);
    GLenum    (__stdcall *GetError)(void);
    void      (__stdcall *GetIntegerv)(GLenum, GLint *);
    void      (__stdcall *GetFloatv)(GLenum, GLfloat *);
    void      (__stdcall *GetBooleanv)(GLenum, GLboolean *);
    void      (__stdcall *Enable)(GLenum);
    void      (__stdcall *Disable)(GLenum);
    GLboolean (__stdcall *IsEnabled)(GLenum);
    void      (__stdcall *Viewport)(GLint, GLint, GLsizei, GLsizei);
    void      (__stdcall *BindTexture)(GLenum, GLuint);
    void      (__stdcall *GenTextures)(GLsizei, GLuint *);
    void      (__stdcall *DeleteTextures)(GLsizei, const GLuint *);
    void      (__stdcall *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void      (__stdcall *TexParameteri)(GLenum, GLenum, GLint);
    void      (__stdcall *GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
    void      (__stdcall *DrawArrays)(GLenum, GLint, GLsizei);
    void      (__stdcall *Flush)(void);
    void      (__stdcall *Finish)(void);
    void      (__stdcall *PixelStorei)(GLenum, GLint);
    void      (__stdcall *ColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
    void      (__stdcall *DepthMask)(GLboolean);
    void      (__stdcall *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void      (__stdcall *ReadBuffer)(GLenum);
    void      (__stdcall *DrawBuffer)(GLenum);

    // -- GL 2.0+ / 3.0+, from wglGetProcAddress -----------------------------
    const GLubyte *(__stdcall *GetStringi)(GLenum, GLuint);
    void   (__stdcall *ActiveTexture)(GLenum);
    GLuint (__stdcall *CreateShader)(GLenum);
    void   (__stdcall *ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
    void   (__stdcall *CompileShader)(GLuint);
    void   (__stdcall *GetShaderiv)(GLuint, GLenum, GLint *);
    void   (__stdcall *GetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void   (__stdcall *DeleteShader)(GLuint);
    GLuint (__stdcall *CreateProgram)(void);
    void   (__stdcall *AttachShader)(GLuint, GLuint);
    void   (__stdcall *LinkProgram)(GLuint);
    void   (__stdcall *GetProgramiv)(GLuint, GLenum, GLint *);
    void   (__stdcall *GetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
    void   (__stdcall *DeleteProgram)(GLuint);
    void   (__stdcall *UseProgram)(GLuint);
    GLint  (__stdcall *GetUniformLocation)(GLuint, const GLchar *);
    void   (__stdcall *Uniform1i)(GLint, GLint);
    void   (__stdcall *Uniform1f)(GLint, GLfloat);
    void   (__stdcall *Uniform2f)(GLint, GLfloat, GLfloat);
    void   (__stdcall *UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);

    void   (__stdcall *GenFramebuffers)(GLsizei, GLuint *);
    void   (__stdcall *DeleteFramebuffers)(GLsizei, const GLuint *);
    void   (__stdcall *BindFramebuffer)(GLenum, GLuint);
    void   (__stdcall *FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (__stdcall *CheckFramebufferStatus)(GLenum);
    void   (__stdcall *BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void   (__stdcall *GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
    void   (__stdcall *DrawBuffers)(GLsizei, const GLenum *);
    void   (__stdcall *BindRenderbuffer)(GLenum, GLuint);
    void   (__stdcall *GetRenderbufferParameteriv)(GLenum, GLenum, GLint *);

    void   (__stdcall *GenVertexArrays)(GLsizei, GLuint *);
    void   (__stdcall *DeleteVertexArrays)(GLsizei, const GLuint *);
    void   (__stdcall *BindVertexArray)(GLuint);

    // -- EXT_memory_object(_win32): the D3D12 textures, aliased into GL -----
    void   (__stdcall *GetUnsignedBytevEXT)(GLenum, GLubyte *);
    void   (__stdcall *CreateMemoryObjectsEXT)(GLsizei, GLuint *);
    void   (__stdcall *DeleteMemoryObjectsEXT)(GLsizei, const GLuint *);
    void   (__stdcall *MemoryObjectParameterivEXT)(GLuint, GLenum, const GLint *);
    void   (__stdcall *ImportMemoryWin32HandleEXT)(GLuint, GLuint64, GLenum, void *);
    void   (__stdcall *TexStorageMem2DEXT)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64);

    // -- EXT_semaphore(_win32): the D3D12 fences, aliased into GL -----------
    void   (__stdcall *GenSemaphoresEXT)(GLsizei, GLuint *);
    void   (__stdcall *DeleteSemaphoresEXT)(GLsizei, const GLuint *);
    void   (__stdcall *ImportSemaphoreWin32HandleEXT)(GLuint, GLenum, void *);
    void   (__stdcall *SemaphoreParameterui64vEXT)(GLuint, GLenum, const GLuint64 *);
    void   (__stdcall *WaitSemaphoreEXT)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
    void   (__stdcall *SignalSemaphoreEXT)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
};

static GlApi GL;
