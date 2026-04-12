#include "renderer_internal.h"
#include "fatal_error.h"
#include <glad/glad.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GLuint fbo;
    GLenum color_gl_fmt;
    int    has_depth;
} gl_render_pass_data;

typedef struct {
    GLuint program;
    renderer_vertex_attrib attribs[RENDERER_MAX_VERTEX_ATTRIBS];
    uint32_t               attrib_count;
    uint32_t               vertex_stride;
    GLenum    topology;
    GLboolean depth_test;
    GLboolean depth_write;
    GLenum    depth_func;
    GLboolean stencil_test;
    GLenum    stencil_fail_front,  stencil_zfail_front,  stencil_zpass_front,  stencil_func_front;
    GLenum    stencil_fail_back,   stencil_zfail_back,   stencil_zpass_back,   stencil_func_back;
    GLint     stencil_ref_front,   stencil_ref_back;
    GLuint    stencil_cmask_front, stencil_cmask_back;
    GLuint    stencil_wmask_front, stencil_wmask_back;
    GLboolean blend_enable;
    GLenum    blend_src_rgb,  blend_dst_rgb;
    GLenum    blend_src_a,    blend_dst_a;
    GLenum    blend_eq_rgb,   blend_eq_a;
    GLboolean color_write_r, color_write_g, color_write_b, color_write_a;
    GLenum    cull_face;      /* 0 = disabled */
    GLenum    front_face;
    GLenum    fill_mode;
    GLboolean scissor_enable;
    GLboolean depth_clamp;
    float     depth_bias_constant;
    float     depth_bias_slope;
    GLboolean multisample;
    GLboolean alpha_to_coverage;
    uint32_t push_constant_size;
    GLuint   push_ubo;
} gl_pipeline_data;

typedef struct {
    GLuint   id;
    GLenum   target;
    GLenum   usage;
    uint32_t size;
} gl_buffer_data;

typedef struct {
    GLuint id;
    GLenum target;
} gl_texture_data;

typedef struct {
    gl_pipeline_data*    bound_pipeline;
    gl_render_pass_data* bound_rp;
    GLuint               bound_vao;
} gl_cmd_data;

typedef struct {
    void*                  platform_ctx;
    renderer_render_pass_t swapchain_rp;
    gl_render_pass_data    swapchain_rp_data;
} gl_renderer_data;

#define GL_RD(r)  ((gl_renderer_data*)  (r)->backend_data)
#define GL_RP(rp) ((gl_render_pass_data*)(rp)->backend_data)
#define GL_PL(pl) ((gl_pipeline_data*)  (pl)->backend_data)
#define GL_BUF(b) ((gl_buffer_data*)    (b)->backend_data)
#define GL_TEX(t) ((gl_texture_data*)   (t)->backend_data)
#define GL_CMD(c) ((gl_cmd_data*)       (c)->backend_data)

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
typedef BOOL  (WINAPI *PFNWGLCHOOSEPIXELFORMATARBPROC)   (HDC, const int*, const FLOAT*, UINT, int*, UINT*);
typedef BOOL  (WINAPI *PFNWGLSWAPINTERVALEXTPROC)         (int);

#define WGL_DRAW_TO_WINDOW_ARB                0x2001
#define WGL_SUPPORT_OPENGL_ARB                0x2010
#define WGL_DOUBLE_BUFFER_ARB                 0x2011
#define WGL_PIXEL_TYPE_ARB                    0x2013
#define WGL_TYPE_RGBA_ARB                     0x202B
#define WGL_COLOR_BITS_ARB                    0x2014
#define WGL_DEPTH_BITS_ARB                    0x2022
#define WGL_STENCIL_BITS_ARB                  0x2023
#define WGL_SAMPLE_BUFFERS_ARB                0x2041
#define WGL_SAMPLES_ARB                       0x2042
#define WGL_CONTEXT_MAJOR_VERSION_ARB         0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB         0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB          0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB      0x00000001
#define WGL_CONTEXT_FLAGS_ARB                 0x2094
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x0002

typedef struct {
    HDC   hdc;
    HGLRC hglrc;
} wgl_ctx;

static int wgl_load_extensions(
    PFNWGLCREATECONTEXTATTRIBSARBPROC* out_create,
    PFNWGLCHOOSEPIXELFORMATARBPROC*    out_choose,
    PFNWGLSWAPINTERVALEXTPROC*         out_swap)
{
    static const char* DUMMY_CLASS = "wgl_bootstrap_class";
    WNDCLASSA wc = { .lpfnWndProc = DefWindowProcA, .hInstance = GetModuleHandleA(NULL), .lpszClassName = DUMMY_CLASS };
    RegisterClassA(&wc);
    HWND dummy = CreateWindowA(DUMMY_CLASS, "", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, NULL, NULL, wc.hInstance, NULL);
    if (!dummy) { UnregisterClassA(DUMMY_CLASS, wc.hInstance); return -1; }
    HDC dummy_dc = GetDC(dummy);
    PIXELFORMATDESCRIPTOR pfd = {
        .nSize = sizeof(pfd), .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA, .cColorBits = 32, .cDepthBits = 24, .cStencilBits = 8,
        .iLayerType = PFD_MAIN_PLANE,
    };
    int fmt = ChoosePixelFormat(dummy_dc, &pfd);
    SetPixelFormat(dummy_dc, fmt, &pfd);
    HGLRC legacy = wglCreateContext(dummy_dc);
    wglMakeCurrent(dummy_dc, legacy);
    *out_create = (PFNWGLCREATECONTEXTATTRIBSARBPROC) wglGetProcAddress("wglCreateContextAttribsARB");
    *out_choose = (PFNWGLCHOOSEPIXELFORMATARBPROC)    wglGetProcAddress("wglChoosePixelFormatARB");
    *out_swap   = (PFNWGLSWAPINTERVALEXTPROC)         wglGetProcAddress("wglSwapIntervalEXT");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    ReleaseDC(dummy, dummy_dc);
    DestroyWindow(dummy);
    UnregisterClassA(DUMMY_CLASS, wc.hInstance);
    return (*out_create && *out_choose) ? 0 : -1;
}

static void* platform_context_create(const renderer_create_desc* desc) {
    HWND hwnd = (HWND)desc->native_window_handle;
    HDC  hdc  = GetDC(hwnd);
    if (!hdc) { fprintf(stderr, "renderer_opengl: GetDC failed\n"); return NULL; }
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = NULL;
    PFNWGLCHOOSEPIXELFORMATARBPROC    wglChoosePixelFormatARB    = NULL;
    PFNWGLSWAPINTERVALEXTPROC         wglSwapIntervalEXT         = NULL;
    if (wgl_load_extensions(&wglCreateContextAttribsARB, &wglChoosePixelFormatARB, &wglSwapIntervalEXT) != 0) {
        fprintf(stderr, "renderer_opengl: WGL_ARB_create_context not supported\n");
        return NULL;
    }
    int sample_count = (desc->sample_count > 1) ? desc->sample_count : 1;
    int pf_attribs[32];
    int pf_idx = 0;
    pf_attribs[pf_idx++] = WGL_DRAW_TO_WINDOW_ARB; pf_attribs[pf_idx++] = GL_TRUE;
    pf_attribs[pf_idx++] = WGL_SUPPORT_OPENGL_ARB; pf_attribs[pf_idx++] = GL_TRUE;
    pf_attribs[pf_idx++] = WGL_DOUBLE_BUFFER_ARB;  pf_attribs[pf_idx++] = GL_TRUE;
    pf_attribs[pf_idx++] = WGL_PIXEL_TYPE_ARB;     pf_attribs[pf_idx++] = WGL_TYPE_RGBA_ARB;
    pf_attribs[pf_idx++] = WGL_COLOR_BITS_ARB;     pf_attribs[pf_idx++] = 32;
    pf_attribs[pf_idx++] = WGL_DEPTH_BITS_ARB;     pf_attribs[pf_idx++] = 24;
    pf_attribs[pf_idx++] = WGL_STENCIL_BITS_ARB;   pf_attribs[pf_idx++] = 8;
    if (sample_count > 1) {
        pf_attribs[pf_idx++] = WGL_SAMPLE_BUFFERS_ARB; pf_attribs[pf_idx++] = 1;
        pf_attribs[pf_idx++] = WGL_SAMPLES_ARB;        pf_attribs[pf_idx++] = sample_count;
    }
    pf_attribs[pf_idx++] = 0;
    int  pixel_format; UINT num_formats;
    if (!wglChoosePixelFormatARB(hdc, pf_attribs, NULL, 1, &pixel_format, &num_formats) || num_formats == 0) {
        fprintf(stderr, "renderer_opengl: wglChoosePixelFormatARB failed\n");
        return NULL;
    }
    PIXELFORMATDESCRIPTOR pfd;
    DescribePixelFormat(hdc, pixel_format, sizeof(pfd), &pfd);
    SetPixelFormat(hdc, pixel_format, &pfd);
    const int ctx_attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, 0
    };
    HGLRC hglrc = wglCreateContextAttribsARB(hdc, NULL, ctx_attribs);
    if (!hglrc) { fprintf(stderr, "renderer_opengl: wglCreateContextAttribsARB failed (0x%lX)\n", GetLastError()); return NULL; }
    wglMakeCurrent(hdc, hglrc);
    if (!gladLoadGL()) {
        fatal_error("OpenGL renderer: gladLoadGL failed");
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(hglrc);
        return NULL;
    }
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(desc->vsync ? 1 : 0);
    wgl_ctx* ctx = calloc(1, sizeof(wgl_ctx));
    if (!ctx) { wglDeleteContext(hglrc); return NULL; }
    ctx->hdc = hdc; ctx->hglrc = hglrc;
    return ctx;
}

static void platform_context_destroy(renderer_t* renderer) {
    wgl_ctx* ctx = (wgl_ctx*)GL_RD(renderer)->platform_ctx;
    if (!ctx) return;
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(ctx->hglrc);
    free(ctx);
}

static void platform_swap_buffers(renderer_t* renderer) {
    wgl_ctx* ctx = (wgl_ctx*)GL_RD(renderer)->platform_ctx;
    SwapBuffers(ctx->hdc);
}

#elif defined(__linux__)

#include <GL/glx.h>
#include <stdint.h>

#define glx_get_proc(name, type) ((type)glXGetProcAddressARB((const GLubyte*)(name)))

typedef struct {
    Display*    display;
    GLXDrawable drawable;
    GLXContext  ctx;
} glx_ctx;

static void* platform_context_create(const renderer_create_desc* desc) {
    Display* display = (Display*)desc->native_display_handle;
    Window   xwin    = (Window)(uintptr_t)desc->native_window_handle;
    if (!display) { fprintf(stderr, "renderer_opengl: native_display_handle is NULL\n"); return NULL; }
    int screen = DefaultScreen(display);
    int sample_count = (desc->sample_count > 1) ? desc->sample_count : 1;
    int fb_attribs[32];
    int fa = 0;
    fb_attribs[fa++] = GLX_X_RENDERABLE;  fb_attribs[fa++] = True;
    fb_attribs[fa++] = GLX_DRAWABLE_TYPE; fb_attribs[fa++] = GLX_WINDOW_BIT;
    fb_attribs[fa++] = GLX_RENDER_TYPE;   fb_attribs[fa++] = GLX_RGBA_BIT;
    fb_attribs[fa++] = GLX_X_VISUAL_TYPE; fb_attribs[fa++] = GLX_TRUE_COLOR;
    fb_attribs[fa++] = GLX_RED_SIZE;      fb_attribs[fa++] = 8;
    fb_attribs[fa++] = GLX_GREEN_SIZE;    fb_attribs[fa++] = 8;
    fb_attribs[fa++] = GLX_BLUE_SIZE;     fb_attribs[fa++] = 8;
    fb_attribs[fa++] = GLX_ALPHA_SIZE;    fb_attribs[fa++] = 8;
    fb_attribs[fa++] = GLX_DEPTH_SIZE;    fb_attribs[fa++] = 24;
    fb_attribs[fa++] = GLX_STENCIL_SIZE;  fb_attribs[fa++] = 8;
    fb_attribs[fa++] = GLX_DOUBLEBUFFER;  fb_attribs[fa++] = True;
    if (sample_count > 1) {
        fb_attribs[fa++] = GLX_SAMPLE_BUFFERS; fb_attribs[fa++] = 1;
        fb_attribs[fa++] = GLX_SAMPLES;        fb_attribs[fa++] = sample_count;
    }
    fb_attribs[fa++] = None;
    int num_configs;
    GLXFBConfig* cfgs = glXChooseFBConfig(display, screen, fb_attribs, &num_configs);
    if (!cfgs || num_configs == 0) { fprintf(stderr, "renderer_opengl: glXChooseFBConfig failed\n"); return NULL; }
    GLXFBConfig fb_cfg = cfgs[0];
    XFree(cfgs);
    PFNGLXCREATECONTEXTATTRIBSARBPROC glXCreateContextAttribsARB = glx_get_proc("glXCreateContextAttribsARB", PFNGLXCREATECONTEXTATTRIBSARBPROC);
    if (!glXCreateContextAttribsARB) { fprintf(stderr, "renderer_opengl: GLX_ARB_create_context not available\n"); return NULL; }
    const int ctx_attribs[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3, GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        GLX_CONTEXT_FLAGS_ARB, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB, None
    };
    GLXContext glx = glXCreateContextAttribsARB(display, fb_cfg, NULL, True, ctx_attribs);
    if (!glx) { fprintf(stderr, "renderer_opengl: glXCreateContextAttribsARB failed\n"); return NULL; }
    if (!glXMakeCurrent(display, xwin, glx)) { fprintf(stderr, "renderer_opengl: glXMakeCurrent failed\n"); glXDestroyContext(display, glx); return NULL; }
    if (!gladLoadGL()) {
        glXMakeCurrent(display, None, NULL);
        glXDestroyContext(display, glx);
        fatal_error("OpenGL renderer: gladLoadGL failed");
    }
    const char* glx_exts = glXQueryExtensionsString(display, screen);
    if (glx_exts) {
        if (strstr(glx_exts, "GLX_EXT_swap_control")) {
            PFNGLXSWAPINTERVALEXTPROC fn = glx_get_proc("glXSwapIntervalEXT", PFNGLXSWAPINTERVALEXTPROC);
            if (fn) fn(display, xwin, desc->vsync ? 1 : 0);
        } else if (strstr(glx_exts, "GLX_MESA_swap_control")) {
            PFNGLXSWAPINTERVALMESAPROC fn = glx_get_proc("glXSwapIntervalMESA", PFNGLXSWAPINTERVALMESAPROC);
            if (fn) fn(desc->vsync ? 1 : 0);
        }
    }
    glx_ctx* ctx = calloc(1, sizeof(glx_ctx));
    if (!ctx) { glXDestroyContext(display, glx); return NULL; }
    ctx->display = display; ctx->drawable = xwin; ctx->ctx = glx;
    return ctx;
}

static void platform_context_destroy(renderer_t* renderer) {
    glx_ctx* ctx = (glx_ctx*)GL_RD(renderer)->platform_ctx;
    if (!ctx) return;
    glXMakeCurrent(ctx->display, None, NULL);
    glXDestroyContext(ctx->display, ctx->ctx);
    free(ctx);
}

static void platform_swap_buffers(renderer_t* renderer) {
    glx_ctx* ctx = (glx_ctx*)GL_RD(renderer)->platform_ctx;
    glXSwapBuffers(ctx->display, ctx->drawable);
}

#else
#  error "renderer_opengl: unsupported platform (Windows and Linux only)"
#endif

/* Extension tokens that glad/glcorearb may not define — provide fallbacks. */
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT        0x83F1
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
#  define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT  0x8C4D
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#  define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT        0x83F3
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
#  define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT  0x8C4F
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#  define GL_TEXTURE_MAX_ANISOTROPY_EXT            0x84FE
#endif

static int compile_stage(GLenum type, const char* src, GLuint* out) {
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, NULL);
    glCompileShader(id);
    GLint ok; glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(id, sizeof log, NULL, log);
        fprintf(stderr, "renderer_opengl: %s compile error:\n%s\n", type == GL_VERTEX_SHADER ? "vert" : "frag", log);
        glDeleteShader(id); return -1;
    }
    *out = id; return 0;
}

/* All renderer_attrib_format values are float variants; GL type is always GL_FLOAT.
 * The component count is encoded in the enum value itself (1–4) and passed as
 * the `size` argument to glVertexAttribPointer via the cast in the caller. */
static GLenum attrib_fmt_to_gl(renderer_attrib_format f) { (void)f; return GL_FLOAT; }

static GLenum primitive_to_gl(renderer_primitive p) {
    switch (p) {
        case RENDERER_PRIMITIVE_TRIANGLES:      return GL_TRIANGLES;
        case RENDERER_PRIMITIVE_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
        case RENDERER_PRIMITIVE_LINES:          return GL_LINES;
        case RENDERER_PRIMITIVE_POINTS:         return GL_POINTS;
        default:                                return GL_TRIANGLES;
    }
}

static GLenum compare_to_gl(renderer_compare_op op) {
    switch (op) {
        case RENDERER_COMPARE_NEVER:    return GL_NEVER;
        case RENDERER_COMPARE_LESS:     return GL_LESS;
        case RENDERER_COMPARE_EQUAL:    return GL_EQUAL;
        case RENDERER_COMPARE_LEQUAL:   return GL_LEQUAL;
        case RENDERER_COMPARE_GREATER:  return GL_GREATER;
        case RENDERER_COMPARE_NOTEQUAL: return GL_NOTEQUAL;
        case RENDERER_COMPARE_GEQUAL:   return GL_GEQUAL;
        case RENDERER_COMPARE_ALWAYS:   return GL_ALWAYS;
        default:                        return GL_LEQUAL;
    }
}

static GLenum stencil_op_to_gl(renderer_stencil_op op) {
    switch (op) {
        case RENDERER_STENCIL_OP_KEEP:             return GL_KEEP;
        case RENDERER_STENCIL_OP_ZERO:             return GL_ZERO;
        case RENDERER_STENCIL_OP_REPLACE:          return GL_REPLACE;
        case RENDERER_STENCIL_OP_INCREMENT_CLAMP:  return GL_INCR;
        case RENDERER_STENCIL_OP_DECREMENT_CLAMP:  return GL_DECR;
        case RENDERER_STENCIL_OP_INVERT:           return GL_INVERT;
        case RENDERER_STENCIL_OP_INCREMENT_WRAP:   return GL_INCR_WRAP;
        case RENDERER_STENCIL_OP_DECREMENT_WRAP:   return GL_DECR_WRAP;
        default:                                   return GL_KEEP;
    }
}

static GLenum blend_factor_to_gl(renderer_blend_factor f) {
    switch (f) {
        case RENDERER_BLEND_FACTOR_ZERO:                return GL_ZERO;
        case RENDERER_BLEND_FACTOR_ONE:                 return GL_ONE;
        case RENDERER_BLEND_FACTOR_SRC_ALPHA:           return GL_SRC_ALPHA;
        case RENDERER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case RENDERER_BLEND_FACTOR_DST_ALPHA:           return GL_DST_ALPHA;
        case RENDERER_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
        default:                                        return GL_ONE;
    }
}

static GLenum blend_op_to_gl(renderer_blend_op op) {
    switch (op) {
        case RENDERER_BLEND_OP_ADD:          return GL_FUNC_ADD;
        case RENDERER_BLEND_OP_SUBTRACT:     return GL_FUNC_SUBTRACT;
        case RENDERER_BLEND_OP_REV_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case RENDERER_BLEND_OP_MIN:          return GL_MIN;
        case RENDERER_BLEND_OP_MAX:          return GL_MAX;
        default:                             return GL_FUNC_ADD;
    }
}

static GLenum tex_wrap_to_gl(renderer_wrap_mode w) {
    switch (w) {
        case RENDERER_WRAP_REPEAT:           return GL_REPEAT;
        case RENDERER_WRAP_CLAMP_TO_EDGE:    return GL_CLAMP_TO_EDGE;
        case RENDERER_WRAP_MIRRORED_REPEAT:  return GL_MIRRORED_REPEAT;
        case RENDERER_WRAP_CLAMP_TO_BORDER:  return GL_CLAMP_TO_BORDER;
        default:                             return GL_REPEAT;
    }
}

static GLenum tex_min_filter_to_gl(renderer_filter f, int has_mipmaps) {
    if (f == RENDERER_FILTER_NEAREST)
        return has_mipmaps ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST;
    return has_mipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR;
}

static GLenum tex_mag_filter_to_gl(renderer_filter f) {
    return (f == RENDERER_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
}

static GLenum tex_fmt_to_internal(renderer_texture_format f) {
    switch (f) {
        case RENDERER_TEXTURE_FORMAT_RGBA8:               return GL_RGBA8;
        case RENDERER_TEXTURE_FORMAT_RGB8:                return GL_RGB8;
        case RENDERER_TEXTURE_FORMAT_RG8:                 return GL_RG8;
        case RENDERER_TEXTURE_FORMAT_R8:                  return GL_R8;
        case RENDERER_TEXTURE_FORMAT_RGBA16F:             return GL_RGBA16F;
        case RENDERER_TEXTURE_FORMAT_RG16F:               return GL_RG16F;
        case RENDERER_TEXTURE_FORMAT_R16F:                return GL_R16F;
        case RENDERER_TEXTURE_FORMAT_RGBA32F:             return GL_RGBA32F;
        case RENDERER_TEXTURE_FORMAT_RG32F:               return GL_RG32F;
        case RENDERER_TEXTURE_FORMAT_R32F:                return GL_R32F;
        case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:          return GL_SRGB8_ALPHA8;
        case RENDERER_TEXTURE_FORMAT_BC1_UNORM:           return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case RENDERER_TEXTURE_FORMAT_BC1_SRGB:            return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
        case RENDERER_TEXTURE_FORMAT_BC3_UNORM:           return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case RENDERER_TEXTURE_FORMAT_BC3_SRGB:            return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
        case RENDERER_TEXTURE_FORMAT_BC4_UNORM:           return GL_COMPRESSED_RED_RGTC1;
        case RENDERER_TEXTURE_FORMAT_BC5_UNORM:           return GL_COMPRESSED_RG_RGTC2;
        case RENDERER_TEXTURE_FORMAT_DEPTH16:             return GL_DEPTH_COMPONENT16;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F:            return GL_DEPTH_COMPONENT32F;
        case RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8:    return GL_DEPTH24_STENCIL8;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8:   return GL_DEPTH32F_STENCIL8;
        default:                                          return GL_RGBA8;
    }
}

static int tex_fmt_is_compressed(renderer_texture_format f) {
    return (f >= RENDERER_TEXTURE_FORMAT_BC1_UNORM && f <= RENDERER_TEXTURE_FORMAT_BC5_UNORM);
}

static GLsizei tex_compressed_size(renderer_texture_format f, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (f) {
        case RENDERER_TEXTURE_FORMAT_BC1_UNORM:
        case RENDERER_TEXTURE_FORMAT_BC1_SRGB:
        case RENDERER_TEXTURE_FORMAT_BC4_UNORM:  return bw * bh * 8;
        default:                                  return bw * bh * 16;
    }
}

static void tex_fmt_to_base_type(renderer_texture_format f, GLenum* base, GLenum* type) {
    switch (f) {
        case RENDERER_TEXTURE_FORMAT_RGBA8:
        case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:     *base = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
        case RENDERER_TEXTURE_FORMAT_RGB8:            *base = GL_RGB;  *type = GL_UNSIGNED_BYTE; return;
        case RENDERER_TEXTURE_FORMAT_RG8:             *base = GL_RG;   *type = GL_UNSIGNED_BYTE; return;
        case RENDERER_TEXTURE_FORMAT_R8:              *base = GL_RED;  *type = GL_UNSIGNED_BYTE; return;
        case RENDERER_TEXTURE_FORMAT_RGBA16F:         *base = GL_RGBA; *type = GL_HALF_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_RG16F:           *base = GL_RG;   *type = GL_HALF_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_R16F:            *base = GL_RED;  *type = GL_HALF_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_RGBA32F:         *base = GL_RGBA; *type = GL_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_RG32F:           *base = GL_RG;   *type = GL_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_R32F:            *base = GL_RED;  *type = GL_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_DEPTH16:         *base = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; return;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F:        *base = GL_DEPTH_COMPONENT; *type = GL_FLOAT; return;
        case RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8:  *base = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; return;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8: *base = GL_DEPTH_STENCIL; *type = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; return;
        default: *base = GL_RGBA; *type = GL_UNSIGNED_BYTE; return;
    }
}

static void apply_pipeline_state(const gl_pipeline_data* pl) {
    if (pl->depth_test) { glEnable(GL_DEPTH_TEST); glDepthFunc(pl->depth_func); }
    else { glDisable(GL_DEPTH_TEST); }
    glDepthMask(pl->depth_write);
    if (pl->stencil_test) {
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, pl->stencil_func_front, pl->stencil_ref_front, pl->stencil_cmask_front);
        glStencilFuncSeparate(GL_BACK,  pl->stencil_func_back,  pl->stencil_ref_back,  pl->stencil_cmask_back);
        glStencilOpSeparate(GL_FRONT, pl->stencil_fail_front, pl->stencil_zfail_front, pl->stencil_zpass_front);
        glStencilOpSeparate(GL_BACK,  pl->stencil_fail_back,  pl->stencil_zfail_back,  pl->stencil_zpass_back);
        glStencilMaskSeparate(GL_FRONT, pl->stencil_wmask_front);
        glStencilMaskSeparate(GL_BACK,  pl->stencil_wmask_back);
    } else {
        glDisable(GL_STENCIL_TEST);
    }
    if (pl->blend_enable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(pl->blend_src_rgb, pl->blend_dst_rgb, pl->blend_src_a, pl->blend_dst_a);
        glBlendEquationSeparate(pl->blend_eq_rgb, pl->blend_eq_a);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(pl->color_write_r, pl->color_write_g, pl->color_write_b, pl->color_write_a);
    if (pl->cull_face) { glEnable(GL_CULL_FACE); glCullFace(pl->cull_face); }
    else { glDisable(GL_CULL_FACE); }
    glFrontFace(pl->front_face);
    glPolygonMode(GL_FRONT_AND_BACK, pl->fill_mode);
    if (pl->depth_clamp) glEnable(GL_DEPTH_CLAMP); else glDisable(GL_DEPTH_CLAMP);
    if (pl->depth_bias_constant != 0.f || pl->depth_bias_slope != 0.f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pl->depth_bias_slope, pl->depth_bias_constant);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (pl->scissor_enable) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (pl->multisample) glEnable(GL_MULTISAMPLE); else glDisable(GL_MULTISAMPLE);
    if (pl->alpha_to_coverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    else                        glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
}

static renderer_render_pass_t* opengl_get_swapchain_render_pass(renderer_t* r) {
    return &GL_RD(r)->swapchain_rp;
}

static renderer_render_pass_t* opengl_render_pass_create(
        renderer_t* r, const renderer_render_pass_desc* desc) {
    (void)r;
    gl_render_pass_data* data = calloc(1, sizeof(gl_render_pass_data));
    if (!data) return NULL;
    glGenFramebuffers(1, &data->fbo);
    data->has_depth    = desc->has_depth;
    data->color_gl_fmt = tex_fmt_to_internal(desc->color_format);
    renderer_render_pass_t* rp = calloc(1, sizeof(renderer_render_pass_t));
    if (!rp) { glDeleteFramebuffers(1, &data->fbo); free(data); return NULL; }
    rp->backend_data = data;
    return rp;
}

static void opengl_render_pass_destroy(renderer_t* r, renderer_render_pass_t* rp) {
    (void)r;
    gl_render_pass_data* data = GL_RP(rp);
    if (data->fbo) glDeleteFramebuffers(1, &data->fbo);
    free(data);
    free(rp);
}

static renderer_pipeline_t* opengl_pipeline_create(
        renderer_t* r, const renderer_pipeline_desc* desc) {
    (void)r;
    if (!desc->vert_src || !desc->frag_src || !desc->render_pass) return NULL;
    GLuint vert, frag;
    if (compile_stage(GL_VERTEX_SHADER,   desc->vert_src, &vert) != 0) return NULL;
    if (compile_stage(GL_FRAGMENT_SHADER, desc->frag_src, &frag) != 0) { glDeleteShader(vert); return NULL; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);
    glDetachShader(prog, vert); glDeleteShader(vert);
    glDetachShader(prog, frag); glDeleteShader(frag);
    GLint ok; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(prog, sizeof log, NULL, log);
        fprintf(stderr, "renderer_opengl: pipeline link error:\n%s\n", log);
        glDeleteProgram(prog); return NULL;
    }
    gl_pipeline_data* data = calloc(1, sizeof(gl_pipeline_data));
    if (!data) { glDeleteProgram(prog); return NULL; }
    data->program       = prog;
    data->attrib_count  = desc->attrib_count < RENDERER_MAX_VERTEX_ATTRIBS ? desc->attrib_count : RENDERER_MAX_VERTEX_ATTRIBS;
    data->vertex_stride = desc->vertex_stride;
    memcpy(data->attribs, desc->attribs, data->attrib_count * sizeof(renderer_vertex_attrib));
    data->topology    = primitive_to_gl(desc->primitive);
    data->depth_test  = desc->depth_test_enable  ? GL_TRUE : GL_FALSE;
    data->depth_write = desc->depth_write_enable ? GL_TRUE : GL_FALSE;
    data->depth_func  = compare_to_gl(desc->depth_compare);
    data->stencil_test = desc->stencil_test_enable ? GL_TRUE : GL_FALSE;
    data->stencil_func_front  = compare_to_gl(desc->stencil_front.compare_op);
    data->stencil_fail_front  = stencil_op_to_gl(desc->stencil_front.fail_op);
    data->stencil_zfail_front = stencil_op_to_gl(desc->stencil_front.depth_fail_op);
    data->stencil_zpass_front = stencil_op_to_gl(desc->stencil_front.pass_op);
    data->stencil_ref_front   = (GLint)desc->stencil_front.reference;
    data->stencil_cmask_front = desc->stencil_front.compare_mask ? desc->stencil_front.compare_mask : 0xFF;
    data->stencil_wmask_front = desc->stencil_front.write_mask   ? desc->stencil_front.write_mask   : 0xFF;
    data->stencil_func_back   = compare_to_gl(desc->stencil_back.compare_op);
    data->stencil_fail_back   = stencil_op_to_gl(desc->stencil_back.fail_op);
    data->stencil_zfail_back  = stencil_op_to_gl(desc->stencil_back.depth_fail_op);
    data->stencil_zpass_back  = stencil_op_to_gl(desc->stencil_back.pass_op);
    data->stencil_ref_back    = (GLint)desc->stencil_back.reference;
    data->stencil_cmask_back  = desc->stencil_back.compare_mask ? desc->stencil_back.compare_mask : 0xFF;
    data->stencil_wmask_back  = desc->stencil_back.write_mask   ? desc->stencil_back.write_mask   : 0xFF;
    data->blend_enable  = desc->blend_enable ? GL_TRUE : GL_FALSE;
    data->blend_src_rgb = blend_factor_to_gl(desc->blend_src_color);
    data->blend_dst_rgb = blend_factor_to_gl(desc->blend_dst_color);
    data->blend_src_a   = blend_factor_to_gl(desc->blend_src_alpha);
    data->blend_dst_a   = blend_factor_to_gl(desc->blend_dst_alpha);
    data->blend_eq_rgb  = blend_op_to_gl(desc->blend_op_color);
    data->blend_eq_a    = blend_op_to_gl(desc->blend_op_alpha);
    {
        uint32_t mask = desc->color_write_mask ? (uint32_t)desc->color_write_mask : (uint32_t)RENDERER_COLOR_WRITE_ALL;
        data->color_write_r = (mask & RENDERER_COLOR_WRITE_R) ? GL_TRUE : GL_FALSE;
        data->color_write_g = (mask & RENDERER_COLOR_WRITE_G) ? GL_TRUE : GL_FALSE;
        data->color_write_b = (mask & RENDERER_COLOR_WRITE_B) ? GL_TRUE : GL_FALSE;
        data->color_write_a = (mask & RENDERER_COLOR_WRITE_A) ? GL_TRUE : GL_FALSE;
    }
    switch (desc->cull_mode) {
        case RENDERER_CULL_FRONT: data->cull_face = GL_FRONT; break;
        case RENDERER_CULL_BACK:  data->cull_face = GL_BACK;  break;
        default:                  data->cull_face = 0;        break;
    }
    data->front_face   = (desc->front_face == RENDERER_FRONT_FACE_CW) ? GL_CW : GL_CCW;
    data->fill_mode    = (desc->fill_mode == RENDERER_FILL_WIREFRAME)  ? GL_LINE : GL_FILL;
    data->scissor_enable       = desc->scissor_test_enable ? GL_TRUE : GL_FALSE;
    data->depth_clamp          = desc->depth_clamp_enable  ? GL_TRUE : GL_FALSE;
    data->depth_bias_constant  = desc->depth_bias_constant_factor;
    data->depth_bias_slope     = desc->depth_bias_slope_factor;
    data->multisample       = (desc->sample_count > 1) ? GL_TRUE : GL_FALSE;
    data->alpha_to_coverage = desc->alpha_to_coverage_enable ? GL_TRUE : GL_FALSE;
    data->push_constant_size = desc->push_constant_size;
    if (desc->push_constant_size > 0) {
        glGenBuffers(1, &data->push_ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, data->push_ubo);
        glBufferData(GL_UNIFORM_BUFFER, desc->push_constant_size, NULL, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        GLuint block = glGetUniformBlockIndex(prog, "PushConstants");
        if (block != GL_INVALID_INDEX) glUniformBlockBinding(prog, block, 0);
    }
    renderer_pipeline_t* pl = calloc(1, sizeof(renderer_pipeline_t));
    if (!pl) { glDeleteProgram(prog); free(data); return NULL; }
    pl->backend_data = data;
    return pl;
}

static void opengl_pipeline_destroy(renderer_t* r, renderer_pipeline_t* pl) {
    (void)r;
    gl_pipeline_data* data = GL_PL(pl);
    glDeleteProgram(data->program);
    if (data->push_ubo) glDeleteBuffers(1, &data->push_ubo);
    free(data);
    free(pl);
}

static renderer_buffer_t* opengl_buffer_create(
        renderer_t* r, const renderer_buffer_desc* desc) {
    (void)r;
    gl_buffer_data* data = calloc(1, sizeof(gl_buffer_data));
    if (!data) return NULL;
    switch (desc->type) {
        case RENDERER_BUFFER_VERTEX:  data->target = GL_ARRAY_BUFFER; break;
        case RENDERER_BUFFER_INDEX:   data->target = GL_ELEMENT_ARRAY_BUFFER; break;
        case RENDERER_BUFFER_UNIFORM: data->target = GL_UNIFORM_BUFFER; break;
    }
    data->usage = desc->usage == RENDERER_BUFFER_USAGE_DYNAMIC ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    data->size  = desc->size;
    glGenBuffers(1, &data->id);
    glBindBuffer(data->target, data->id);
    glBufferData(data->target, desc->size, desc->data, data->usage);
    glBindBuffer(data->target, 0);
    renderer_buffer_t* buf = calloc(1, sizeof(renderer_buffer_t));
    if (!buf) { glDeleteBuffers(1, &data->id); free(data); return NULL; }
    buf->backend_data = data;
    return buf;
}

static void opengl_buffer_destroy(renderer_t* r, renderer_buffer_t* buf) {
    (void)r;
    gl_buffer_data* data = GL_BUF(buf);
    glDeleteBuffers(1, &data->id);
    free(data);
    free(buf);
}

static void opengl_buffer_update(renderer_t* r, renderer_buffer_t* buf,
                                 const void* data, uint32_t size) {
    (void)r;
    gl_buffer_data* d = GL_BUF(buf);
    glBindBuffer(d->target, d->id);
    glBufferSubData(d->target, 0, size, data);
    glBindBuffer(d->target, 0);
}

static renderer_texture_t* opengl_texture_create(
        renderer_t* r, const renderer_texture_desc* desc) {
    (void)r;
    gl_texture_data* data = calloc(1, sizeof(gl_texture_data));
    if (!data) return NULL;

    int array_layers = (desc->array_layers > 1) ? desc->array_layers : 1;
    int sample_count = (desc->sample_count  > 1) ? desc->sample_count  : 1;
    int has_mipmaps  = desc->generate_mipmaps || (desc->mip_levels > 1);
    int is_compressed = tex_fmt_is_compressed(desc->format);

    GLenum target;
    if (sample_count > 1 && array_layers > 1)      target = GL_TEXTURE_2D_MULTISAMPLE_ARRAY;
    else if (sample_count > 1)                      target = GL_TEXTURE_2D_MULTISAMPLE;
    else if (array_layers > 1)                      target = GL_TEXTURE_2D_ARRAY;
    else                                            target = GL_TEXTURE_2D;
    data->target = target;

    glGenTextures(1, &data->id);
    glBindTexture(target, data->id);

    GLenum internal_fmt = tex_fmt_to_internal(desc->format);

    if (sample_count > 1) {
        /* Multisample textures: no sampler parameters, no mips */
        if (target == GL_TEXTURE_2D_MULTISAMPLE)
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE,
                sample_count, internal_fmt, desc->width, desc->height, GL_TRUE);
        else
            glTexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY,
                sample_count, internal_fmt, desc->width, desc->height, array_layers, GL_TRUE);
    } else {
        renderer_filter min_f = desc->min_filter;
        renderer_filter mag_f = desc->mag_filter;
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, tex_min_filter_to_gl(min_f, has_mipmaps));
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, tex_mag_filter_to_gl(mag_f));
        glTexParameteri(target, GL_TEXTURE_WRAP_S, tex_wrap_to_gl(desc->wrap_u));
        glTexParameteri(target, GL_TEXTURE_WRAP_T, tex_wrap_to_gl(desc->wrap_v));
        if (target == GL_TEXTURE_2D_ARRAY)
            glTexParameteri(target, GL_TEXTURE_WRAP_R, tex_wrap_to_gl(desc->wrap_w));
        if (desc->max_anisotropy > 1.f)
            glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, desc->max_anisotropy);

        GLenum base_fmt, type;
        tex_fmt_to_base_type(desc->format, &base_fmt, &type);

        if (desc->format == RENDERER_TEXTURE_FORMAT_R8) {
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_G, GL_RED);
            glTexParameteri(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
        }

        if (is_compressed && desc->pixels) {
            GLsizei sz = tex_compressed_size(desc->format, desc->width, desc->height);
            if (target == GL_TEXTURE_2D)
                glCompressedTexImage2D(GL_TEXTURE_2D, 0, internal_fmt,
                    desc->width, desc->height, 0, sz, desc->pixels);
            else
                glCompressedTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal_fmt,
                    desc->width, desc->height, array_layers, 0,
                    sz * array_layers, desc->pixels);
        } else {
            if (target == GL_TEXTURE_2D)
                glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal_fmt,
                    desc->width, desc->height, 0, base_fmt, type, desc->pixels);
            else
                glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, (GLint)internal_fmt,
                    desc->width, desc->height, array_layers, 0, base_fmt, type, desc->pixels);
        }

        if (has_mipmaps && !is_compressed) glGenerateMipmap(target);
    }

    glBindTexture(target, 0);
    renderer_texture_t* tex = calloc(1, sizeof(renderer_texture_t));
    if (!tex) { glDeleteTextures(1, &data->id); free(data); return NULL; }
    tex->backend_data = data;
    return tex;
}

static void opengl_texture_destroy(renderer_t* r, renderer_texture_t* tex) {
    (void)r;
    glDeleteTextures(1, &GL_TEX(tex)->id);
    free(tex->backend_data);
    free(tex);
}

static renderer_cmd_t* opengl_cmd_begin(renderer_t* r) {
    gl_cmd_data* data = calloc(1, sizeof(gl_cmd_data));
    if (!data) return NULL;
    glGenVertexArrays(1, &data->bound_vao);
    renderer_cmd_t* cmd = calloc(1, sizeof(renderer_cmd_t));
    if (!cmd) { glDeleteVertexArrays(1, &data->bound_vao); free(data); return NULL; }
    cmd->backend_data = data;
    cmd->vtable       = r->vtable;
    return cmd;
}

static void opengl_cmd_submit(renderer_t* r, renderer_cmd_t* cmd) {
    (void)r;
    gl_cmd_data* data = GL_CMD(cmd);
    glDeleteVertexArrays(1, &data->bound_vao);
    free(data);
    free(cmd);
}

static void opengl_cmd_begin_render_pass(renderer_cmd_t*         cmd,
                                         renderer_render_pass_t* rp,
                                         renderer_texture_t*     color_tex,
                                         renderer_texture_t*     depth_tex,
                                         const renderer_clear_value* clear) {
    gl_cmd_data*         cd  = GL_CMD(cmd);
    gl_render_pass_data* rpd = GL_RP(rp);
    cd->bound_rp = rpd;
    glBindFramebuffer(GL_FRAMEBUFFER, rpd->fbo);
    if (rpd->fbo != 0) {
        if (color_tex) glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GL_TEX(color_tex)->id, 0);
        if (depth_tex) {
            gl_texture_data* dtd = GL_TEX(depth_tex);
            /* Use DEPTH_STENCIL_ATTACHMENT for combined depth+stencil formats,
             * DEPTH_ATTACHMENT for pure depth.  Detect by internal format. */
            GLenum attachment = GL_DEPTH_ATTACHMENT;
            GLint ifmt = 0;
            glBindTexture(dtd->target, dtd->id);
            glGetTexLevelParameteriv(dtd->target, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
            glBindTexture(dtd->target, 0);
            if (ifmt == (GLint)GL_DEPTH24_STENCIL8 || ifmt == (GLint)GL_DEPTH32F_STENCIL8)
                attachment = GL_DEPTH_STENCIL_ATTACHMENT;
            glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, dtd->id, 0);
        }
    }
    if (clear) {
        glClearColor(clear->r, clear->g, clear->b, clear->a);
        glClearDepth(clear->depth);
        glClearStencil((GLint)clear->stencil);
        GLbitfield mask = GL_COLOR_BUFFER_BIT;
        if (rpd->has_depth) mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
        glClear(mask);
    }
}

static void opengl_cmd_end_render_pass(renderer_cmd_t* cmd) {
    (void)cmd;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void opengl_cmd_bind_pipeline(renderer_cmd_t* cmd, renderer_pipeline_t* pl) {
    gl_cmd_data*      cd   = GL_CMD(cmd);
    gl_pipeline_data* data = GL_PL(pl);
    cd->bound_pipeline = data;
    glUseProgram(data->program);
    apply_pipeline_state(data);
    glBindVertexArray(cd->bound_vao);
}

static void opengl_cmd_bind_vertex_buffer(renderer_cmd_t* cmd, renderer_buffer_t* buf,
                                          uint32_t slot, uint32_t byte_offset) {
    gl_cmd_data*      cd = GL_CMD(cmd);
    gl_pipeline_data* pl = cd->bound_pipeline;
    gl_buffer_data*   bd = GL_BUF(buf);
    glBindBuffer(GL_ARRAY_BUFFER, bd->id);
    if (pl) {
        for (uint32_t i = 0; i < pl->attrib_count; ++i) {
            const renderer_vertex_attrib* a = &pl->attribs[i];
            (void)slot;
            glEnableVertexAttribArray(a->location);
            glVertexAttribPointer(a->location, (GLint)a->format, attrib_fmt_to_gl(a->format),
                                  GL_FALSE, (GLsizei)pl->vertex_stride,
                                  (const void*)(uintptr_t)(a->offset + byte_offset));
        }
    }
}

static void opengl_cmd_bind_index_buffer(renderer_cmd_t* cmd, renderer_buffer_t* buf,
                                         uint32_t byte_offset) {
    (void)cmd; (void)byte_offset;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_BUF(buf)->id);
}

static void opengl_cmd_bind_texture(renderer_cmd_t* cmd, renderer_texture_t* tex,
                                    uint32_t slot) {
    (void)cmd;
    gl_texture_data* td = GL_TEX(tex);
    glActiveTexture((GLenum)(GL_TEXTURE0 + slot));
    glBindTexture(td->target, td->id);
}

static void opengl_cmd_push_constants(renderer_cmd_t* cmd, renderer_pipeline_t* pl,
                                      const void* data, uint32_t size) {
    (void)cmd;
    gl_pipeline_data* pd = GL_PL(pl);
    if (!pd->push_ubo || size > pd->push_constant_size) return;
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, pd->push_ubo);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, size, data);
}

static void opengl_cmd_set_viewport(renderer_cmd_t* cmd,
                                    float x, float y, float w, float h,
                                    float min_depth, float max_depth) {
    (void)cmd;
    glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    glDepthRange((GLdouble)min_depth, (GLdouble)max_depth);
}

static void opengl_cmd_set_scissor(renderer_cmd_t* cmd, int x, int y, int w, int h) {
    /* Only update the scissor rect; enabling/disabling is driven by pipeline state */
    (void)cmd;
    glScissor(x, y, w, h);
}

static void opengl_cmd_draw(renderer_cmd_t* cmd,
                            uint32_t vertex_count,   uint32_t instance_count,
                            uint32_t first_vertex,   uint32_t first_instance) {
    gl_cmd_data* cd = GL_CMD(cmd);
    if (!cd->bound_pipeline) return;
    GLenum topo = cd->bound_pipeline->topology;
    (void)first_instance;
    if (instance_count <= 1) glDrawArrays(topo, (GLint)first_vertex, (GLsizei)vertex_count);
    else glDrawArraysInstanced(topo, (GLint)first_vertex, (GLsizei)vertex_count, (GLsizei)instance_count);
}

static void opengl_cmd_draw_indexed(renderer_cmd_t* cmd,
                                    uint32_t index_count,  uint32_t instance_count,
                                    uint32_t first_index,  int32_t  vertex_offset,
                                    uint32_t first_instance) {
    gl_cmd_data* cd = GL_CMD(cmd);
    if (!cd->bound_pipeline) return;
    GLenum topo = cd->bound_pipeline->topology;
    const void* idx_ptr = (const void*)(uintptr_t)(first_index * sizeof(uint32_t));
    (void)first_instance;
    if (instance_count <= 1) glDrawElementsBaseVertex(topo, (GLsizei)index_count, GL_UNSIGNED_INT, idx_ptr, vertex_offset);
    else glDrawElementsInstancedBaseVertex(topo, (GLsizei)index_count, GL_UNSIGNED_INT, idx_ptr, (GLsizei)instance_count, vertex_offset);
}

static int opengl_draw_init(renderer_t* r, void* platform_ctx) {
    gl_renderer_data* rd = calloc(1, sizeof(gl_renderer_data));
    if (!rd) return -1;
    rd->platform_ctx = platform_ctx;
    rd->swapchain_rp_data.fbo       = 0;
    rd->swapchain_rp_data.has_depth = 1;
    rd->swapchain_rp.backend_data   = &rd->swapchain_rp_data;
    r->backend_data = rd;
    return 0;
}

static void opengl_draw_shutdown(renderer_t* r) {
    free(r->backend_data);
    r->backend_data = NULL;
}

static int opengl_init(renderer_t* renderer, const renderer_create_desc* desc) {
    void* platform_ctx = platform_context_create(desc);
    if (!platform_ctx) return -1;
    fprintf(stderr, "renderer_opengl: %s — GLSL %s\n", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION));
    return opengl_draw_init(renderer, platform_ctx);
}

static void opengl_shutdown(renderer_t* renderer) {
    platform_context_destroy(renderer);
    opengl_draw_shutdown(renderer);
}

static void opengl_begin_frame(renderer_t* renderer) { (void)renderer; }
static void opengl_end_frame  (renderer_t* renderer) { (void)renderer; }

static void opengl_present(renderer_t* renderer) { platform_swap_buffers(renderer); }

static void opengl_resize(renderer_t* renderer, int width, int height) {
    (void)renderer;
    glViewport(0, 0, width, height);
}

static const renderer_backend_vtable s_opengl_vtable = {
    .init        = opengl_init,
    .shutdown    = opengl_shutdown,
    .begin_frame = opengl_begin_frame,
    .end_frame   = opengl_end_frame,
    .present     = opengl_present,
    .resize      = opengl_resize,
    .get_swapchain_render_pass = opengl_get_swapchain_render_pass,
    .render_pass_create        = opengl_render_pass_create,
    .render_pass_destroy       = opengl_render_pass_destroy,
    .pipeline_create  = opengl_pipeline_create,
    .pipeline_destroy = opengl_pipeline_destroy,
    .buffer_create  = opengl_buffer_create,
    .buffer_destroy = opengl_buffer_destroy,
    .buffer_update  = opengl_buffer_update,
    .texture_create  = opengl_texture_create,
    .texture_destroy = opengl_texture_destroy,
    .cmd_begin  = opengl_cmd_begin,
    .cmd_submit = opengl_cmd_submit,
    .cmd_begin_render_pass   = opengl_cmd_begin_render_pass,
    .cmd_end_render_pass     = opengl_cmd_end_render_pass,
    .cmd_bind_pipeline       = opengl_cmd_bind_pipeline,
    .cmd_bind_vertex_buffer  = opengl_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer   = opengl_cmd_bind_index_buffer,
    .cmd_bind_texture        = opengl_cmd_bind_texture,
    .cmd_push_constants      = opengl_cmd_push_constants,
    .cmd_set_viewport        = opengl_cmd_set_viewport,
    .cmd_set_scissor         = opengl_cmd_set_scissor,
    .cmd_draw                = opengl_cmd_draw,
    .cmd_draw_indexed        = opengl_cmd_draw_indexed,
};

const renderer_backend_vtable* renderer_backend_opengl_vtable(void) {
    return &s_opengl_vtable;
}