#ifndef _WIN32
#  error "renderer_d3d11: Direct3D 11 is Windows-only."
#endif

#include "renderer_internal.h"
#include "fatal_error.h"

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef struct d3d11_rp_data {
    int         is_swapchain;
    DXGI_FORMAT color_fmt;
    DXGI_FORMAT depth_fmt;
    int         has_depth;
} d3d11_rp_data;

typedef struct {
    ID3D11Device*            device;
    ID3D11DeviceContext*     context;
    IDXGISwapChain*          swapchain;
    /* Non-MSAA resolve target (the actual swapchain back-buffer) */
    ID3D11Texture2D*         backbuffer_tex;
    ID3D11RenderTargetView*  backbuffer_rtv;
    /* MSAA offscreen render target (only allocated when sample_count > 1) */
    ID3D11Texture2D*         msaa_color_tex;
    ID3D11RenderTargetView*  msaa_color_rtv;
    /* Depth/stencil (always matches the active color target's sample count) */
    ID3D11Texture2D*         depth_tex;
    ID3D11DepthStencilView*  depth_dsv;
    int                      vsync;
    int                      sample_count;
    renderer_render_pass_t   swapchain_rp;
    d3d11_rp_data            swapchain_rp_data;
} d3d11_renderer_data;

typedef struct {
    ID3D11VertexShader*      vs;
    ID3D11PixelShader*       ps;
    ID3D11InputLayout*       input_layout;
    ID3D11DepthStencilState* ds_state;
    ID3D11BlendState*        blend_state;
    ID3D11RasterizerState*   rs_state;
    ID3D11Buffer*            push_cb;
    UINT                     push_cb_size;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT                     vertex_stride;
    UINT                     stencil_ref;
} d3d11_pipeline_data;

typedef struct {
    ID3D11Buffer* buffer;
    UINT          size;
} d3d11_buffer_data;

typedef struct {
    ID3D11Texture2D*          texture;
    ID3D11ShaderResourceView* srv;
    ID3D11RenderTargetView*   rtv;
    ID3D11DepthStencilView*   dsv;
    ID3D11SamplerState* sampler;
} d3d11_texture_data;

typedef struct {
    d3d11_renderer_data* rd;
    d3d11_pipeline_data* bound_pipeline;
    ID3D11RenderTargetView* current_rtv;
    ID3D11DepthStencilView* current_dsv;
} d3d11_cmd_data;

#define D3D_RD(r)  ((d3d11_renderer_data*)(r)->backend_data)
#define D3D_RP(rp) ((d3d11_rp_data*)      (rp)->backend_data)
#define D3D_PL(pl) ((d3d11_pipeline_data*)(pl)->backend_data)
#define D3D_BUF(b) ((d3d11_buffer_data*)  (b)->backend_data)
#define D3D_TEX(t) ((d3d11_texture_data*) (t)->backend_data)
#define D3D_CMD(c) ((d3d11_cmd_data*)     (c)->backend_data)

static DXGI_FORMAT tex_fmt_to_dxgi(renderer_texture_format f) {
    switch (f) {
        case RENDERER_TEXTURE_FORMAT_RGBA8:               return DXGI_FORMAT_R8G8B8A8_UNORM;
        case RENDERER_TEXTURE_FORMAT_RGB8:                return DXGI_FORMAT_R8G8B8A8_UNORM; /* no 24-bit in D3D */
        case RENDERER_TEXTURE_FORMAT_RG8:                 return DXGI_FORMAT_R8G8_UNORM;
        case RENDERER_TEXTURE_FORMAT_R8:                  return DXGI_FORMAT_R8_UNORM;
        case RENDERER_TEXTURE_FORMAT_RGBA16F:             return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case RENDERER_TEXTURE_FORMAT_RG16F:               return DXGI_FORMAT_R16G16_FLOAT;
        case RENDERER_TEXTURE_FORMAT_R16F:                return DXGI_FORMAT_R16_FLOAT;
        case RENDERER_TEXTURE_FORMAT_RGBA32F:             return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case RENDERER_TEXTURE_FORMAT_RG32F:               return DXGI_FORMAT_R32G32_FLOAT;
        case RENDERER_TEXTURE_FORMAT_R32F:                return DXGI_FORMAT_R32_FLOAT;
        case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:          return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case RENDERER_TEXTURE_FORMAT_BC1_UNORM:           return DXGI_FORMAT_BC1_UNORM;
        case RENDERER_TEXTURE_FORMAT_BC1_SRGB:            return DXGI_FORMAT_BC1_UNORM_SRGB;
        case RENDERER_TEXTURE_FORMAT_BC3_UNORM:           return DXGI_FORMAT_BC3_UNORM;
        case RENDERER_TEXTURE_FORMAT_BC3_SRGB:            return DXGI_FORMAT_BC3_UNORM_SRGB;
        case RENDERER_TEXTURE_FORMAT_BC4_UNORM:           return DXGI_FORMAT_BC4_UNORM;
        case RENDERER_TEXTURE_FORMAT_BC5_UNORM:           return DXGI_FORMAT_BC5_UNORM;
        case RENDERER_TEXTURE_FORMAT_DEPTH16:             return DXGI_FORMAT_D16_UNORM;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F:            return DXGI_FORMAT_D32_FLOAT;
        case RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
        case RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8:   return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        default:                                          return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

static UINT tex_fmt_pitch(renderer_texture_format f, int width) {
    switch (f) {
        case RENDERER_TEXTURE_FORMAT_R8:                  return (UINT)width;
        case RENDERER_TEXTURE_FORMAT_RG8:                 return (UINT)(width * 2);
        case RENDERER_TEXTURE_FORMAT_RGBA8:
        case RENDERER_TEXTURE_FORMAT_RGBA8_SRGB:          return (UINT)(width * 4);
        case RENDERER_TEXTURE_FORMAT_RGB8:                return (UINT)(width * 4); /* padded to 4 in D3D */
        case RENDERER_TEXTURE_FORMAT_R16F:                return (UINT)(width * 2);
        case RENDERER_TEXTURE_FORMAT_RG16F:               return (UINT)(width * 4);
        case RENDERER_TEXTURE_FORMAT_RGBA16F:             return (UINT)(width * 8);
        case RENDERER_TEXTURE_FORMAT_R32F:                return (UINT)(width * 4);
        case RENDERER_TEXTURE_FORMAT_RG32F:               return (UINT)(width * 8);
        case RENDERER_TEXTURE_FORMAT_RGBA32F:             return (UINT)(width * 16);
        case RENDERER_TEXTURE_FORMAT_BC1_UNORM:
        case RENDERER_TEXTURE_FORMAT_BC1_SRGB:
        case RENDERER_TEXTURE_FORMAT_BC4_UNORM:           return (UINT)(((width + 3) / 4) * 8);
        case RENDERER_TEXTURE_FORMAT_BC3_UNORM:
        case RENDERER_TEXTURE_FORMAT_BC3_SRGB:
        case RENDERER_TEXTURE_FORMAT_BC5_UNORM:           return (UINT)(((width + 3) / 4) * 16);
        default:                                          return (UINT)(width * 4);
    }
}

static DXGI_FORMAT attrib_fmt_to_dxgi(renderer_attrib_format f) {
    switch (f) {
        case RENDERER_ATTRIB_FLOAT1: return DXGI_FORMAT_R32_FLOAT;
        case RENDERER_ATTRIB_FLOAT2: return DXGI_FORMAT_R32G32_FLOAT;
        case RENDERER_ATTRIB_FLOAT3: return DXGI_FORMAT_R32G32B32_FLOAT;
        case RENDERER_ATTRIB_FLOAT4: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        default:                     return DXGI_FORMAT_R32G32B32_FLOAT;
    }
}

static D3D11_PRIMITIVE_TOPOLOGY primitive_to_d3d(renderer_primitive p) {
    switch (p) {
        case RENDERER_PRIMITIVE_TRIANGLES:      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case RENDERER_PRIMITIVE_TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case RENDERER_PRIMITIVE_LINES:          return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case RENDERER_PRIMITIVE_POINTS:         return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        default:                                return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static D3D11_COMPARISON_FUNC compare_to_d3d(renderer_compare_op op) {
    switch (op) {
        case RENDERER_COMPARE_NEVER:    return D3D11_COMPARISON_NEVER;
        case RENDERER_COMPARE_LESS:     return D3D11_COMPARISON_LESS;
        case RENDERER_COMPARE_EQUAL:    return D3D11_COMPARISON_EQUAL;
        case RENDERER_COMPARE_LEQUAL:   return D3D11_COMPARISON_LESS_EQUAL;
        case RENDERER_COMPARE_GREATER:  return D3D11_COMPARISON_GREATER;
        case RENDERER_COMPARE_NOTEQUAL: return D3D11_COMPARISON_NOT_EQUAL;
        case RENDERER_COMPARE_GEQUAL:   return D3D11_COMPARISON_GREATER_EQUAL;
        case RENDERER_COMPARE_ALWAYS:   return D3D11_COMPARISON_ALWAYS;
        default:                        return D3D11_COMPARISON_LESS_EQUAL;
    }
}

static D3D11_BLEND blend_factor_to_d3d(renderer_blend_factor f) {
    switch (f) {
        case RENDERER_BLEND_FACTOR_ZERO:                return D3D11_BLEND_ZERO;
        case RENDERER_BLEND_FACTOR_ONE:                 return D3D11_BLEND_ONE;
        case RENDERER_BLEND_FACTOR_SRC_ALPHA:           return D3D11_BLEND_SRC_ALPHA;
        case RENDERER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
        case RENDERER_BLEND_FACTOR_DST_ALPHA:           return D3D11_BLEND_DEST_ALPHA;
        case RENDERER_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
        default:                                        return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op_to_d3d(renderer_blend_op op) {
    switch (op) {
        case RENDERER_BLEND_OP_ADD:          return D3D11_BLEND_OP_ADD;
        case RENDERER_BLEND_OP_SUBTRACT:     return D3D11_BLEND_OP_SUBTRACT;
        case RENDERER_BLEND_OP_REV_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
        case RENDERER_BLEND_OP_MIN:          return D3D11_BLEND_OP_MIN;
        case RENDERER_BLEND_OP_MAX:          return D3D11_BLEND_OP_MAX;
        default:                             return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_STENCIL_OP stencil_op_to_d3d(renderer_stencil_op op) {
    switch (op) {
        case RENDERER_STENCIL_OP_KEEP:             return D3D11_STENCIL_OP_KEEP;
        case RENDERER_STENCIL_OP_ZERO:             return D3D11_STENCIL_OP_ZERO;
        case RENDERER_STENCIL_OP_REPLACE:          return D3D11_STENCIL_OP_REPLACE;
        case RENDERER_STENCIL_OP_INCREMENT_CLAMP:  return D3D11_STENCIL_OP_INCR_SAT;
        case RENDERER_STENCIL_OP_DECREMENT_CLAMP:  return D3D11_STENCIL_OP_DECR_SAT;
        case RENDERER_STENCIL_OP_INVERT:           return D3D11_STENCIL_OP_INVERT;
        case RENDERER_STENCIL_OP_INCREMENT_WRAP:   return D3D11_STENCIL_OP_INCR;
        case RENDERER_STENCIL_OP_DECREMENT_WRAP:   return D3D11_STENCIL_OP_DECR;
        default:                                   return D3D11_STENCIL_OP_KEEP;
    }
}

static void d3d11_release_backbuffer(d3d11_renderer_data* rd) {
    if (rd->msaa_color_rtv) { ID3D11RenderTargetView_Release(rd->msaa_color_rtv); rd->msaa_color_rtv = NULL; }
    if (rd->msaa_color_tex) { ID3D11Texture2D_Release(rd->msaa_color_tex); rd->msaa_color_tex = NULL; }
    if (rd->backbuffer_rtv) { ID3D11RenderTargetView_Release(rd->backbuffer_rtv); rd->backbuffer_rtv = NULL; }
    if (rd->backbuffer_tex) { ID3D11Texture2D_Release(rd->backbuffer_tex); rd->backbuffer_tex = NULL; }
    if (rd->depth_dsv)      { ID3D11DepthStencilView_Release(rd->depth_dsv); rd->depth_dsv = NULL; }
    if (rd->depth_tex)      { ID3D11Texture2D_Release(rd->depth_tex); rd->depth_tex = NULL; }
}

static int d3d11_create_backbuffer(d3d11_renderer_data* rd, int w, int h) {
    HRESULT hr;

    /* Always get the non-MSAA swapchain back-buffer for the resolve/present target */
    hr = IDXGISwapChain_GetBuffer(rd->swapchain, 0, &IID_ID3D11Texture2D, (void**)&rd->backbuffer_tex);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: GetBuffer failed\n"); return -1; }
    hr = ID3D11Device_CreateRenderTargetView(rd->device, (ID3D11Resource*)rd->backbuffer_tex, NULL, &rd->backbuffer_rtv);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreateRenderTargetView (backbuffer) failed\n"); return -1; }

    UINT sc = (rd->sample_count > 1) ? (UINT)rd->sample_count : 1u;

    /* When MSAA is requested, allocate a separate multisampled color texture */
    if (rd->sample_count > 1) {
        /* Check MSAA support for the requested sample count */
        UINT quality_levels = 0;
        hr = ID3D11Device_CheckMultisampleQualityLevels(rd->device, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                         sc, &quality_levels);
        if (FAILED(hr) || quality_levels == 0) {
            fprintf(stderr, "renderer_d3d11: MSAA x%u not supported, falling back to x1\n", sc);
            sc = 1;
            rd->sample_count = 1;
        }
    }

    if (sc > 1) {
        D3D11_TEXTURE2D_DESC cd = {
            .Width      = (UINT)w, .Height = (UINT)h, .MipLevels = 1, .ArraySize = 1,
            .Format     = DXGI_FORMAT_R8G8B8A8_UNORM,
            .SampleDesc = { .Count = sc, .Quality = 0 },
            .Usage      = D3D11_USAGE_DEFAULT,
            .BindFlags  = D3D11_BIND_RENDER_TARGET,
        };
        hr = ID3D11Device_CreateTexture2D(rd->device, &cd, NULL, &rd->msaa_color_tex);
        if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: MSAA color texture creation failed\n"); return -1; }
        hr = ID3D11Device_CreateRenderTargetView(rd->device, (ID3D11Resource*)rd->msaa_color_tex, NULL, &rd->msaa_color_rtv);
        if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: MSAA color RTV creation failed\n"); return -1; }
    }

    D3D11_TEXTURE2D_DESC dd = {
        .Width      = (UINT)w, .Height = (UINT)h, .MipLevels = 1, .ArraySize = 1,
        .Format     = DXGI_FORMAT_D24_UNORM_S8_UINT,
        .SampleDesc = { .Count = sc, .Quality = 0 },
        .Usage      = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_DEPTH_STENCIL,
    };
    hr = ID3D11Device_CreateTexture2D(rd->device, &dd, NULL, &rd->depth_tex);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: depth texture creation failed\n"); return -1; }
    hr = ID3D11Device_CreateDepthStencilView(rd->device, (ID3D11Resource*)rd->depth_tex, NULL, &rd->depth_dsv);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: depth DSV creation failed\n"); return -1; }
    return 0;
}

static int d3d11_init(renderer_t* r, const renderer_create_desc* desc) {
    HWND hwnd = (HWND)desc->native_window_handle;
    d3d11_renderer_data* rd = calloc(1, sizeof(d3d11_renderer_data));
    if (!rd) return -1;
    rd->vsync        = desc->vsync;
    rd->sample_count = (desc->sample_count > 1) ? desc->sample_count : 1;

    DXGI_SWAP_CHAIN_DESC sd = {
        .BufferCount = 2,
        .BufferDesc.Width = (UINT)desc->width, .BufferDesc.Height = (UINT)desc->height,
        .BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .BufferDesc.RefreshRate = { .Numerator = 60, .Denominator = 1 },
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .OutputWindow = hwnd, .SampleDesc = { .Count = 1, .Quality = 0 },
        .Windowed = TRUE, .SwapEffect = DXGI_SWAP_EFFECT_DISCARD,
    };
    D3D_FEATURE_LEVEL feature_level;
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    UINT device_flags = 0;
#if defined(_DEBUG)
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, device_flags,
        levels, 1, D3D11_SDK_VERSION,
        &sd, &rd->swapchain, &rd->device, &feature_level, &rd->context);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: D3D11CreateDeviceAndSwapChain failed (0x%lX)\n", hr); free(rd); return -1; }

    if (d3d11_create_backbuffer(rd, desc->width, desc->height) != 0) {
        d3d11_release_backbuffer(rd);
        IDXGISwapChain_Release(rd->swapchain);
        ID3D11DeviceContext_Release(rd->context);
        ID3D11Device_Release(rd->device);
        free(rd);
        return -1;
    }
    rd->swapchain_rp_data.is_swapchain = 1;
    rd->swapchain_rp_data.color_fmt    = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd->swapchain_rp_data.depth_fmt    = DXGI_FORMAT_D24_UNORM_S8_UINT;
    rd->swapchain_rp_data.has_depth    = 1;
    rd->swapchain_rp.backend_data      = &rd->swapchain_rp_data;
    r->backend_data = rd;
    fprintf(stderr, "renderer_d3d11: feature level %d.%d\n",
            (int)((feature_level >> 12) & 0xF), (int)((feature_level >> 8) & 0xF));
    return 0;
}

static void d3d11_shutdown(renderer_t* r) {
    d3d11_renderer_data* rd = D3D_RD(r);
    if (!rd) return;
    ID3D11DeviceContext_ClearState(rd->context);
    d3d11_release_backbuffer(rd);
    IDXGISwapChain_Release(rd->swapchain);
    ID3D11DeviceContext_Release(rd->context);
    ID3D11Device_Release(rd->device);
    free(rd);
}

static void d3d11_begin_frame(renderer_t* r) { (void)r; }
static void d3d11_end_frame  (renderer_t* r) { (void)r; }

static void d3d11_present(renderer_t* r) {
    d3d11_renderer_data* rd = D3D_RD(r);
    if (rd->msaa_color_tex && rd->backbuffer_tex) {
        ID3D11DeviceContext_ResolveSubresource(rd->context,
            (ID3D11Resource*)rd->backbuffer_tex, 0,
            (ID3D11Resource*)rd->msaa_color_tex, 0,
            DXGI_FORMAT_R8G8B8A8_UNORM);
    }
    IDXGISwapChain_Present(rd->swapchain, rd->vsync ? 1 : 0, 0);
}

static void d3d11_resize(renderer_t* r, int w, int h) {
    d3d11_renderer_data* rd = D3D_RD(r);
    ID3D11DeviceContext_OMSetRenderTargets(rd->context, 0, NULL, NULL);
    d3d11_release_backbuffer(rd);
    IDXGISwapChain_ResizeBuffers(rd->swapchain, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    d3d11_create_backbuffer(rd, w, h);
}

static renderer_render_pass_t* d3d11_get_swapchain_render_pass(renderer_t* r) {
    return &D3D_RD(r)->swapchain_rp;
}

static renderer_render_pass_t* d3d11_render_pass_create(
        renderer_t* r, const renderer_render_pass_desc* desc) {
    (void)r;
    d3d11_rp_data* data = calloc(1, sizeof(d3d11_rp_data));
    if (!data) return NULL;
    data->is_swapchain = 0;
    data->color_fmt    = tex_fmt_to_dxgi(desc->color_format);
    data->depth_fmt    = tex_fmt_to_dxgi(desc->depth_format);
    data->has_depth    = desc->has_depth;
    renderer_render_pass_t* rp = calloc(1, sizeof(renderer_render_pass_t));
    if (!rp) { free(data); return NULL; }
    rp->backend_data = data;
    return rp;
}

static void d3d11_render_pass_destroy(renderer_t* r, renderer_render_pass_t* rp) {
    (void)r;
    free(rp->backend_data);
    free(rp);
}

static int compile_hlsl(ID3D11Device* dev, const char* src,
                         const char* profile, ID3DBlob** out) {
    (void)dev;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), NULL, NULL, NULL,
                            "main", profile, flags, 0, out, &err);
    if (FAILED(hr)) {
        if (err) {
            fprintf(stderr, "renderer_d3d11: HLSL %s compile error:\n%s\n",
                    profile, (const char*)ID3D10Blob_GetBufferPointer(err));
            ID3D10Blob_Release(err);
        }
        return -1;
    }
    if (err) ID3D10Blob_Release(err);
    return 0;
}

static renderer_pipeline_t* d3d11_pipeline_create(
        renderer_t* r, const renderer_pipeline_desc* desc) {
    d3d11_renderer_data* rd = D3D_RD(r);
    if (!desc->vert_src || !desc->frag_src) return NULL;
    ID3DBlob* vs_blob = NULL;
    ID3DBlob* ps_blob = NULL;
    if (compile_hlsl(rd->device, desc->vert_src, "vs_5_0", &vs_blob) != 0) return NULL;
    if (compile_hlsl(rd->device, desc->frag_src, "ps_5_0", &ps_blob) != 0) {
        ID3D10Blob_Release(vs_blob); return NULL;
    }
    d3d11_pipeline_data* data = calloc(1, sizeof(d3d11_pipeline_data));
    if (!data) goto fail;

    HRESULT hr;
    hr = ID3D11Device_CreateVertexShader(rd->device,
            ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
            NULL, &data->vs);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreateVertexShader failed\n"); goto fail; }
    hr = ID3D11Device_CreatePixelShader(rd->device,
            ID3D10Blob_GetBufferPointer(ps_blob), ID3D10Blob_GetBufferSize(ps_blob),
            NULL, &data->ps);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreatePixelShader failed\n"); goto fail; }

    uint32_t count = desc->attrib_count < RENDERER_MAX_VERTEX_ATTRIBS
                   ? desc->attrib_count : RENDERER_MAX_VERTEX_ATTRIBS;
    if (count > 0) {
        D3D11_INPUT_ELEMENT_DESC elems[RENDERER_MAX_VERTEX_ATTRIBS];
        for (uint32_t i = 0; i < count; ++i) {
            elems[i].SemanticName         = "ATTR";
            elems[i].SemanticIndex        = desc->attribs[i].location;
            elems[i].Format               = attrib_fmt_to_dxgi(desc->attribs[i].format);
            elems[i].InputSlot            = 0;
            elems[i].AlignedByteOffset    = desc->attribs[i].offset;
            elems[i].InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
            elems[i].InstanceDataStepRate = 0;
        }
        hr = ID3D11Device_CreateInputLayout(rd->device, elems, count,
                ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob),
                &data->input_layout);
        if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreateInputLayout failed\n"); goto fail; }
    }
    data->vertex_stride = desc->vertex_stride;
    data->topology      = primitive_to_d3d(desc->primitive);

    D3D11_DEPTH_STENCIL_DESC ds_desc = {
        .DepthEnable      = (BOOL)desc->depth_test_enable,
        .DepthWriteMask   = desc->depth_write_enable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO,
        .DepthFunc        = compare_to_d3d(desc->depth_compare),
        .StencilEnable    = (BOOL)desc->stencil_test_enable,
        .StencilReadMask  = desc->stencil_front.compare_mask ? desc->stencil_front.compare_mask : D3D11_DEFAULT_STENCIL_READ_MASK,
        .StencilWriteMask = desc->stencil_front.write_mask   ? desc->stencil_front.write_mask   : D3D11_DEFAULT_STENCIL_WRITE_MASK,
        .FrontFace = {
            stencil_op_to_d3d(desc->stencil_front.fail_op),
            stencil_op_to_d3d(desc->stencil_front.depth_fail_op),
            stencil_op_to_d3d(desc->stencil_front.pass_op),
            compare_to_d3d(desc->stencil_front.compare_op)
        },
        .BackFace = {
            stencil_op_to_d3d(desc->stencil_back.fail_op),
            stencil_op_to_d3d(desc->stencil_back.depth_fail_op),
            stencil_op_to_d3d(desc->stencil_back.pass_op),
            compare_to_d3d(desc->stencil_back.compare_op)
        },
    };
    data->stencil_ref = (UINT)desc->stencil_front.reference;
    hr = ID3D11Device_CreateDepthStencilState(rd->device, &ds_desc, &data->ds_state);
    if (FAILED(hr)) goto fail;

    {
        uint32_t cwm = desc->color_write_mask ? (uint32_t)desc->color_write_mask : (uint32_t)RENDERER_COLOR_WRITE_ALL;
        UINT8 d3d_mask = 0;
        if (cwm & RENDERER_COLOR_WRITE_R) d3d_mask |= D3D11_COLOR_WRITE_ENABLE_RED;
        if (cwm & RENDERER_COLOR_WRITE_G) d3d_mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
        if (cwm & RENDERER_COLOR_WRITE_B) d3d_mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
        if (cwm & RENDERER_COLOR_WRITE_A) d3d_mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;

        D3D11_BLEND_DESC blend_desc;
        memset(&blend_desc, 0, sizeof(blend_desc));
        blend_desc.AlphaToCoverageEnable  = (BOOL)desc->alpha_to_coverage_enable;
        blend_desc.IndependentBlendEnable = FALSE;
        blend_desc.RenderTarget[0].BlendEnable           = (BOOL)desc->blend_enable;
        blend_desc.RenderTarget[0].SrcBlend              = blend_factor_to_d3d(desc->blend_src_color);
        blend_desc.RenderTarget[0].DestBlend             = blend_factor_to_d3d(desc->blend_dst_color);
        blend_desc.RenderTarget[0].BlendOp               = blend_op_to_d3d(desc->blend_op_color);
        blend_desc.RenderTarget[0].SrcBlendAlpha         = blend_factor_to_d3d(desc->blend_src_alpha);
        blend_desc.RenderTarget[0].DestBlendAlpha        = blend_factor_to_d3d(desc->blend_dst_alpha);
        blend_desc.RenderTarget[0].BlendOpAlpha          = blend_op_to_d3d(desc->blend_op_alpha);
        blend_desc.RenderTarget[0].RenderTargetWriteMask = d3d_mask;
        hr = ID3D11Device_CreateBlendState(rd->device, &blend_desc, &data->blend_state);
        if (FAILED(hr)) goto fail;
    }

    {
        D3D11_CULL_MODE cull;
        switch (desc->cull_mode) {
            case RENDERER_CULL_FRONT: cull = D3D11_CULL_FRONT; break;
            case RENDERER_CULL_BACK:  cull = D3D11_CULL_BACK;  break;
            default:                  cull = D3D11_CULL_NONE;  break;
        }
        /* D3D front-face convention: FrontCounterClockwise=TRUE means CCW is front */
        BOOL ccw = (desc->front_face == RENDERER_FRONT_FACE_CCW) ? TRUE : FALSE;
        D3D11_RASTERIZER_DESC rs_desc = {
            .FillMode              = (desc->fill_mode == RENDERER_FILL_WIREFRAME) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID,
            .CullMode              = cull,
            .FrontCounterClockwise = ccw,
            .DepthBias             = (INT)desc->depth_bias_constant_factor,
            .DepthBiasClamp        = desc->depth_bias_clamp,
            .SlopeScaledDepthBias  = desc->depth_bias_slope_factor,
            .DepthClipEnable       = desc->depth_clamp_enable ? FALSE : TRUE,
            .ScissorEnable         = (BOOL)desc->scissor_test_enable,
            .MultisampleEnable     = (desc->sample_count > 1) ? TRUE : FALSE,
            .AntialiasedLineEnable = FALSE,
        };
        hr = ID3D11Device_CreateRasterizerState(rd->device, &rs_desc, &data->rs_state);
        if (FAILED(hr)) goto fail;
    }

    if (desc->push_constant_size > 0) {
        UINT aligned = (desc->push_constant_size + 15u) & ~15u;
        D3D11_BUFFER_DESC bd = {
            .ByteWidth = aligned, .Usage = D3D11_USAGE_DYNAMIC,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE,
        };
        hr = ID3D11Device_CreateBuffer(rd->device, &bd, NULL, &data->push_cb);
        if (FAILED(hr)) goto fail;
        data->push_cb_size = aligned;
    }

    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_blob);
    renderer_pipeline_t* pl = calloc(1, sizeof(renderer_pipeline_t));
    if (!pl) goto fail_after_blobs;
    pl->backend_data = data;
    return pl;

fail:
    ID3D10Blob_Release(vs_blob);
    ID3D10Blob_Release(ps_blob);
fail_after_blobs:
    if (data) {
        if (data->vs)           ID3D11VertexShader_Release(data->vs);
        if (data->ps)           ID3D11PixelShader_Release(data->ps);
        if (data->input_layout) ID3D11InputLayout_Release(data->input_layout);
        if (data->ds_state)     ID3D11DepthStencilState_Release(data->ds_state);
        if (data->blend_state)  ID3D11BlendState_Release(data->blend_state);
        if (data->rs_state)     ID3D11RasterizerState_Release(data->rs_state);
        if (data->push_cb)      ID3D11Buffer_Release(data->push_cb);
        free(data);
    }
    return NULL;
}

static void d3d11_pipeline_destroy(renderer_t* r, renderer_pipeline_t* pl) {
    (void)r;
    d3d11_pipeline_data* data = D3D_PL(pl);
    if (data->vs)           ID3D11VertexShader_Release(data->vs);
    if (data->ps)           ID3D11PixelShader_Release(data->ps);
    if (data->input_layout) ID3D11InputLayout_Release(data->input_layout);
    if (data->ds_state)     ID3D11DepthStencilState_Release(data->ds_state);
    if (data->blend_state)  ID3D11BlendState_Release(data->blend_state);
    if (data->rs_state)     ID3D11RasterizerState_Release(data->rs_state);
    if (data->push_cb)      ID3D11Buffer_Release(data->push_cb);
    free(data);
    free(pl);
}

static renderer_buffer_t* d3d11_buffer_create(
        renderer_t* r, const renderer_buffer_desc* desc) {
    d3d11_renderer_data* rd = D3D_RD(r);
    UINT bind_flags;
    UINT byte_width = desc->size;
    switch (desc->type) {
        case RENDERER_BUFFER_VERTEX:  bind_flags = D3D11_BIND_VERTEX_BUFFER;   break;
        case RENDERER_BUFFER_INDEX:   bind_flags = D3D11_BIND_INDEX_BUFFER;    break;
        case RENDERER_BUFFER_UNIFORM:
            bind_flags = D3D11_BIND_CONSTANT_BUFFER;
            byte_width = (byte_width + 15u) & ~15u;
            break;
        default: bind_flags = D3D11_BIND_VERTEX_BUFFER; break;
    }
    int is_dynamic = (desc->usage == RENDERER_BUFFER_USAGE_DYNAMIC);
    D3D11_BUFFER_DESC bd = {
        .ByteWidth = byte_width,
        .Usage = is_dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT,
        .BindFlags = bind_flags,
        .CPUAccessFlags = is_dynamic ? D3D11_CPU_ACCESS_WRITE : 0,
    };
    D3D11_SUBRESOURCE_DATA init = { .pSysMem = desc->data };
    ID3D11Buffer* buf = NULL;
    HRESULT hr = ID3D11Device_CreateBuffer(rd->device, &bd,
                                            desc->data ? &init : NULL, &buf);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreateBuffer failed (0x%lX)\n", hr); return NULL; }
    d3d11_buffer_data* data = calloc(1, sizeof(d3d11_buffer_data));
    if (!data) { ID3D11Buffer_Release(buf); return NULL; }
    data->buffer = buf;
    data->size   = byte_width;
    renderer_buffer_t* b = calloc(1, sizeof(renderer_buffer_t));
    if (!b) { ID3D11Buffer_Release(buf); free(data); return NULL; }
    b->backend_data = data;
    return b;
}

static void d3d11_buffer_destroy(renderer_t* r, renderer_buffer_t* b) {
    (void)r;
    ID3D11Buffer_Release(D3D_BUF(b)->buffer);
    free(b->backend_data);
    free(b);
}

static void d3d11_buffer_update(renderer_t* r, renderer_buffer_t* b,
                                 const void* data, uint32_t size) {
    d3d11_renderer_data* rd  = D3D_RD(r);
    d3d11_buffer_data*   bd  = D3D_BUF(b);
    ID3D11Resource*      res = (ID3D11Resource*)bd->buffer;

    /* Retrieve the actual buffer description to determine usage */
    D3D11_BUFFER_DESC desc;
    ID3D11Buffer_GetDesc(bd->buffer, &desc);

    if (desc.Usage == D3D11_USAGE_DYNAMIC) {
        /* DYNAMIC buffers: use Map/Unmap DISCARD */
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ID3D11DeviceContext_Map(rd->context, res, 0,
                         D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, data, size);
            ID3D11DeviceContext_Unmap(rd->context, res, 0);
        }
    } else {
        /* DEFAULT (static) buffers: use UpdateSubresource */
        D3D11_BOX box = { 0, 0, 0, size, 1, 1 };
        ID3D11DeviceContext_UpdateSubresource(rd->context, res, 0, &box, data, 0, 0);
    }
}

static D3D11_TEXTURE_ADDRESS_MODE tex_wrap_to_d3d(renderer_wrap_mode w) {
    switch (w) {
        case RENDERER_WRAP_REPEAT:           return D3D11_TEXTURE_ADDRESS_WRAP;
        case RENDERER_WRAP_CLAMP_TO_EDGE:    return D3D11_TEXTURE_ADDRESS_CLAMP;
        case RENDERER_WRAP_MIRRORED_REPEAT:  return D3D11_TEXTURE_ADDRESS_MIRROR;
        case RENDERER_WRAP_CLAMP_TO_BORDER:  return D3D11_TEXTURE_ADDRESS_BORDER;
        default:                             return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static renderer_texture_t* d3d11_texture_create(
        renderer_t* r, const renderer_texture_desc* desc) {
    d3d11_renderer_data* rd = D3D_RD(r);
    DXGI_FORMAT fmt      = tex_fmt_to_dxgi(desc->format);
    int is_depth         = (desc->format == RENDERER_TEXTURE_FORMAT_DEPTH16         ||
                            desc->format == RENDERER_TEXTURE_FORMAT_DEPTH32F        ||
                            desc->format == RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8 ||
                            desc->format == RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8);
    int array_layers     = (desc->array_layers > 1) ? desc->array_layers : 1;
    int sample_count     = (desc->sample_count  > 1) ? desc->sample_count  : 1;
    int has_mipmaps      = desc->generate_mipmaps || (desc->mip_levels > 1);
    UINT mip_levels      = (desc->mip_levels > 0) ? (UINT)desc->mip_levels : (has_mipmaps ? 0u : 1u);
    uint32_t usage_flags = desc->usage;
    if (!usage_flags) {
        usage_flags = is_depth ? RENDERER_TEXTURE_USAGE_DEPTH_STENCIL
                               : (RENDERER_TEXTURE_USAGE_SAMPLED | RENDERER_TEXTURE_USAGE_RENDER_TARGET);
    }

    UINT bind_flags = 0;
    if (usage_flags & RENDERER_TEXTURE_USAGE_SAMPLED)       bind_flags |= D3D11_BIND_SHADER_RESOURCE;
    if (usage_flags & RENDERER_TEXTURE_USAGE_RENDER_TARGET) bind_flags |= D3D11_BIND_RENDER_TARGET;
    if (usage_flags & RENDERER_TEXTURE_USAGE_DEPTH_STENCIL) bind_flags |= D3D11_BIND_DEPTH_STENCIL;
    if (usage_flags & RENDERER_TEXTURE_USAGE_STORAGE)       bind_flags |= D3D11_BIND_UNORDERED_ACCESS;

    UINT misc_flags = 0;
    if (has_mipmaps && !is_depth && (bind_flags & D3D11_BIND_SHADER_RESOURCE))
        misc_flags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

    D3D11_TEXTURE2D_DESC td = {
        .Width      = (UINT)desc->width,
        .Height     = (UINT)desc->height,
        .MipLevels  = mip_levels,
        .ArraySize  = (UINT)array_layers,
        .Format     = fmt,
        .SampleDesc = { .Count = (UINT)sample_count, .Quality = 0 },
        .Usage      = D3D11_USAGE_DEFAULT,
        .BindFlags  = bind_flags,
        .MiscFlags  = misc_flags,
    };
    d3d11_texture_data* data = calloc(1, sizeof(d3d11_texture_data));
    if (!data) return NULL;
    D3D11_SUBRESOURCE_DATA init_data = {
        .pSysMem     = desc->pixels,
        .SysMemPitch = tex_fmt_pitch(desc->format, desc->width)
    };
    BOOL upload_at_create = (desc->pixels != NULL) && !has_mipmaps && (sample_count == 1);
    HRESULT hr = ID3D11Device_CreateTexture2D(rd->device, &td, upload_at_create ? &init_data : NULL, &data->texture);
    if (FAILED(hr)) { fprintf(stderr, "renderer_d3d11: CreateTexture2D failed (0x%lX)\n", hr); free(data); return NULL; }

    if (!is_depth && (bind_flags & D3D11_BIND_SHADER_RESOURCE)) {
        hr = ID3D11Device_CreateShaderResourceView(rd->device, (ID3D11Resource*)data->texture, NULL, &data->srv);
        if (FAILED(hr)) goto fail;
        D3D11_SAMPLER_DESC samp_desc;
        memset(&samp_desc, 0, sizeof(samp_desc));
        int is_nearest = (desc->min_filter == RENDERER_FILTER_NEAREST && desc->mag_filter == RENDERER_FILTER_NEAREST);
        
        if (desc->max_anisotropy > 1.0f) {
            samp_desc.Filter = D3D11_FILTER_ANISOTROPIC;
        } else {
            samp_desc.Filter = is_nearest ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        }
        
        samp_desc.AddressU = tex_wrap_to_d3d(desc->wrap_u);
        samp_desc.AddressV = tex_wrap_to_d3d(desc->wrap_v);
        samp_desc.AddressW = tex_wrap_to_d3d(desc->wrap_w);
        samp_desc.MaxAnisotropy = (UINT)((desc->max_anisotropy > 1.f) ? desc->max_anisotropy : 1);
        samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp_desc.MinLOD = 0;
        samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

        hr = ID3D11Device_CreateSamplerState(rd->device, &samp_desc, &data->sampler);
        if (FAILED(hr)) goto fail;
    }
    if (!is_depth && (bind_flags & D3D11_BIND_RENDER_TARGET)) {
        hr = ID3D11Device_CreateRenderTargetView(rd->device, (ID3D11Resource*)data->texture, NULL, &data->rtv);
        if (FAILED(hr)) goto fail;
    }
    if (is_depth && (bind_flags & D3D11_BIND_DEPTH_STENCIL)) {
        hr = ID3D11Device_CreateDepthStencilView(rd->device, (ID3D11Resource*)data->texture, NULL, &data->dsv);
        if (FAILED(hr)) goto fail;
    }
    if (has_mipmaps && desc->pixels && !is_depth && data->srv) {
        ID3D11DeviceContext_UpdateSubresource(rd->context, (ID3D11Resource*)data->texture, 0, NULL,
            desc->pixels, tex_fmt_pitch(desc->format, desc->width), 0);
        ID3D11DeviceContext_GenerateMips(rd->context, data->srv);
    }

    renderer_texture_t* tex = calloc(1, sizeof(renderer_texture_t));
    if (!tex) goto fail;
    tex->backend_data = data;
    return tex;
fail:
    if (data->srv)     ID3D11ShaderResourceView_Release(data->srv);
    if (data->rtv)     ID3D11RenderTargetView_Release(data->rtv);
    if (data->dsv)     ID3D11DepthStencilView_Release(data->dsv);
    if (data->texture) ID3D11Texture2D_Release(data->texture);
    free(data);
    return NULL;
}

static void d3d11_texture_destroy(renderer_t* r, renderer_texture_t* tex) {
    (void)r;
    d3d11_texture_data* data = D3D_TEX(tex);
    if (data->sampler) ID3D11SamplerState_Release(data->sampler);
    if (data->srv)     ID3D11ShaderResourceView_Release(data->srv);
    if (data->rtv)     ID3D11RenderTargetView_Release(data->rtv);
    if (data->dsv)     ID3D11DepthStencilView_Release(data->dsv);
    if (data->texture) ID3D11Texture2D_Release(data->texture);
    free(data);
    free(tex);
}

static renderer_cmd_t* d3d11_cmd_begin(renderer_t* r) {
    d3d11_cmd_data* data = calloc(1, sizeof(d3d11_cmd_data));
    if (!data) return NULL;
    data->rd = D3D_RD(r);
    renderer_cmd_t* cmd = calloc(1, sizeof(renderer_cmd_t));
    if (!cmd) { free(data); return NULL; }
    cmd->backend_data = data;
    cmd->vtable       = r->vtable;
    return cmd;
}

static void d3d11_cmd_submit(renderer_t* r, renderer_cmd_t* cmd) {
    (void)r;
    free(cmd->backend_data);
    free(cmd);
}

static void d3d11_cmd_begin_render_pass(renderer_cmd_t*          cmd,
                                         renderer_render_pass_t*  rp,
                                         renderer_texture_t*      color_tex,
                                         renderer_texture_t*      depth_tex,
                                         const renderer_clear_value* clear) {
    d3d11_cmd_data*      cd  = D3D_CMD(cmd);
    d3d11_rp_data*       rpd = D3D_RP(rp);
    ID3D11DeviceContext* ctx = cd->rd->context;
    if (rpd->is_swapchain) {
        /* If MSAA is active, render into the MSAA offscreen target */
        cd->current_rtv = cd->rd->msaa_color_rtv ? cd->rd->msaa_color_rtv : cd->rd->backbuffer_rtv;
        cd->current_dsv = cd->rd->depth_dsv;
    } else {
        cd->current_rtv = color_tex ? D3D_TEX(color_tex)->rtv : NULL;
        cd->current_dsv = (rpd->has_depth && depth_tex) ? D3D_TEX(depth_tex)->dsv : NULL;
    }
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &cd->current_rtv, cd->current_dsv);
    if (clear) {
        if (cd->current_rtv) {
            const float col[4] = { clear->r, clear->g, clear->b, clear->a };
            ID3D11DeviceContext_ClearRenderTargetView(ctx, cd->current_rtv, col);
        }
        if (cd->current_dsv)
            ID3D11DeviceContext_ClearDepthStencilView(ctx, cd->current_dsv,
                D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, clear->depth, (UINT8)clear->stencil);
    }
}

static void d3d11_cmd_end_render_pass(renderer_cmd_t* cmd) {
    d3d11_cmd_data* cd = D3D_CMD(cmd);
    ID3D11DeviceContext_OMSetRenderTargets(cd->rd->context, 0, NULL, NULL);
    cd->current_rtv     = NULL;
    cd->current_dsv     = NULL;
}

static void d3d11_cmd_bind_pipeline(renderer_cmd_t* cmd, renderer_pipeline_t* pl) {
    d3d11_cmd_data*      cd  = D3D_CMD(cmd);
    d3d11_pipeline_data* pd  = D3D_PL(pl);
    ID3D11DeviceContext* ctx = cd->rd->context;
    cd->bound_pipeline = pd;
    ID3D11DeviceContext_VSSetShader(ctx, pd->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, pd->ps, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, pd->input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, pd->topology);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, pd->ds_state, pd->stencil_ref);
    ID3D11DeviceContext_RSSetState(ctx, pd->rs_state);
    static const float blend_factor[4] = { 1.f, 1.f, 1.f, 1.f };
    ID3D11DeviceContext_OMSetBlendState(ctx, pd->blend_state, blend_factor, 0xFFFFFFFF);
    if (pd->push_cb) {
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &pd->push_cb);
        ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &pd->push_cb);
    }
}

static void d3d11_cmd_bind_vertex_buffer(renderer_cmd_t* cmd, renderer_buffer_t* buf,
                                          uint32_t slot, uint32_t byte_offset) {
    d3d11_cmd_data* cd     = D3D_CMD(cmd);
    ID3D11Buffer*   vb     = D3D_BUF(buf)->buffer;
    UINT            stride = cd->bound_pipeline ? cd->bound_pipeline->vertex_stride : 0;
    UINT            offset = byte_offset;
    ID3D11DeviceContext_IASetVertexBuffers(cd->rd->context, slot, 1, &vb, &stride, &offset);
}

static void d3d11_cmd_bind_index_buffer(renderer_cmd_t* cmd, renderer_buffer_t* buf,
                                         uint32_t byte_offset) {
    d3d11_cmd_data* cd = D3D_CMD(cmd);
    ID3D11DeviceContext_IASetIndexBuffer(cd->rd->context, D3D_BUF(buf)->buffer, DXGI_FORMAT_R32_UINT, byte_offset);
}

static void d3d11_cmd_bind_texture(renderer_cmd_t* cmd, renderer_texture_t* tex, uint32_t slot) {
    d3d11_cmd_data* cd  = D3D_CMD(cmd);
    ID3D11ShaderResourceView* srv = tex ? D3D_TEX(tex)->srv : NULL;
    ID3D11SamplerState* smp = tex ? D3D_TEX(tex)->sampler : NULL;

    ID3D11DeviceContext_PSSetShaderResources(cd->rd->context, slot, 1, &srv);
    ID3D11DeviceContext_PSSetSamplers(cd->rd->context, slot, 1, &smp);
}

static void d3d11_cmd_push_constants(renderer_cmd_t* cmd, renderer_pipeline_t* pl,
                                      const void* data, uint32_t size) {
    d3d11_cmd_data*      cd  = D3D_CMD(cmd);
    d3d11_pipeline_data* pd  = D3D_PL(pl);
    if (!pd->push_cb) return;
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = ID3D11DeviceContext_Map(cd->rd->context, (ID3D11Resource*)pd->push_cb,
                     0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, data, size);
        ID3D11DeviceContext_Unmap(cd->rd->context, (ID3D11Resource*)pd->push_cb, 0);
    }
}

static void d3d11_cmd_set_viewport(renderer_cmd_t* cmd,
                                    float x, float y, float w, float h,
                                    float min_depth, float max_depth) {
    d3d11_cmd_data*  cd = D3D_CMD(cmd);
    D3D11_VIEWPORT   vp = { x, y, w, h, min_depth, max_depth };
    ID3D11DeviceContext_RSSetViewports(cd->rd->context, 1, &vp);
}

static void d3d11_cmd_set_scissor(renderer_cmd_t* cmd, int x, int y, int w, int h) {
    d3d11_cmd_data* cd   = D3D_CMD(cmd);
    D3D11_RECT      rect = { x, y, x + w, y + h };
    ID3D11DeviceContext_RSSetScissorRects(cd->rd->context, 1, &rect);
}

static void d3d11_cmd_draw(renderer_cmd_t* cmd,
                            uint32_t vertex_count,   uint32_t instance_count,
                            uint32_t first_vertex,   uint32_t first_instance) {
    d3d11_cmd_data* cd = D3D_CMD(cmd);
    if (instance_count <= 1)
        ID3D11DeviceContext_Draw(cd->rd->context, vertex_count, first_vertex);
    else
        ID3D11DeviceContext_DrawInstanced(cd->rd->context,
            vertex_count, instance_count, first_vertex, first_instance);
}

static void d3d11_cmd_draw_indexed(renderer_cmd_t* cmd,
                                    uint32_t index_count,  uint32_t instance_count,
                                    uint32_t first_index,  int32_t  vertex_offset,
                                    uint32_t first_instance) {
    d3d11_cmd_data* cd = D3D_CMD(cmd);
    if (instance_count <= 1)
        ID3D11DeviceContext_DrawIndexed(cd->rd->context, index_count, first_index, vertex_offset);
    else
        ID3D11DeviceContext_DrawIndexedInstanced(cd->rd->context,
            index_count, instance_count, first_index, vertex_offset, first_instance);
}

static void d3d11_cmd_bind_uniform_buffer(renderer_cmd_t* cmd,
                                           renderer_buffer_t* buf,
                                           uint32_t slot,
                                           uint32_t byte_offset,
                                           uint32_t byte_size) {
    d3d11_cmd_data* cd  = D3D_CMD(cmd);
    ID3D11Buffer*   cb  = D3D_BUF(buf)->buffer;
    UINT            reg = slot + 1u;  /* +1: b0 is push-constant buffer */
    (void)byte_offset; (void)byte_size;
    
    ID3D11DeviceContext_VSSetConstantBuffers(cd->rd->context, reg, 1, &cb);
    ID3D11DeviceContext_PSSetConstantBuffers(cd->rd->context, reg, 1, &cb);
}

static const renderer_backend_vtable s_d3d11_vtable = {
    .init        = d3d11_init,
    .shutdown    = d3d11_shutdown,
    .begin_frame = d3d11_begin_frame,
    .end_frame   = d3d11_end_frame,
    .present     = d3d11_present,
    .resize      = d3d11_resize,
    .get_swapchain_render_pass = d3d11_get_swapchain_render_pass,
    .render_pass_create        = d3d11_render_pass_create,
    .render_pass_destroy       = d3d11_render_pass_destroy,
    .pipeline_create  = d3d11_pipeline_create,
    .pipeline_destroy = d3d11_pipeline_destroy,
    .buffer_create  = d3d11_buffer_create,
    .buffer_destroy = d3d11_buffer_destroy,
    .buffer_update  = d3d11_buffer_update,
    .texture_create  = d3d11_texture_create,
    .texture_destroy = d3d11_texture_destroy,
    .cmd_begin  = d3d11_cmd_begin,
    .cmd_submit = d3d11_cmd_submit,
    .cmd_begin_render_pass   = d3d11_cmd_begin_render_pass,
    .cmd_end_render_pass     = d3d11_cmd_end_render_pass,
    .cmd_bind_pipeline       = d3d11_cmd_bind_pipeline,
    .cmd_bind_vertex_buffer  = d3d11_cmd_bind_vertex_buffer,
    .cmd_bind_index_buffer   = d3d11_cmd_bind_index_buffer,
    .cmd_bind_texture        = d3d11_cmd_bind_texture,
    .cmd_push_constants      = d3d11_cmd_push_constants,
    .cmd_set_viewport        = d3d11_cmd_set_viewport,
    .cmd_set_scissor         = d3d11_cmd_set_scissor,
    .cmd_draw                = d3d11_cmd_draw,
    .cmd_draw_indexed        = d3d11_cmd_draw_indexed,
    .cmd_bind_uniform_buffer = d3d11_cmd_bind_uniform_buffer,
};

const renderer_backend_vtable* renderer_backend_d3d11_vtable(void) {
    return &s_d3d11_vtable;
}