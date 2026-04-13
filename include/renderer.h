#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

typedef struct renderer_t             renderer_t;
typedef struct renderer_render_pass_t renderer_render_pass_t;
typedef struct renderer_pipeline_t    renderer_pipeline_t;
typedef struct renderer_buffer_t      renderer_buffer_t;
typedef struct renderer_texture_t     renderer_texture_t;
typedef struct renderer_cmd_t         renderer_cmd_t;

typedef enum {
    RENDERER_BACKEND_OPENGL,
    RENDERER_BACKEND_VULKAN,
    RENDERER_BACKEND_D3D11,
} renderer_backend;

char* renderer_backend_to_string(renderer_backend backend);

typedef struct {
    void*            native_window_handle;
    void*            native_display_handle;
    int              width;
    int              height;
    int              vsync;
    int              sample_count;   /* MSAA; 1 = disabled, 2/4/8 = multisampled   */
    renderer_backend backend;
} renderer_create_desc;

renderer_t* renderer_create  (const renderer_create_desc* desc);
void        renderer_destroy (renderer_t* renderer);

void renderer_begin_frame (renderer_t* renderer);
void renderer_end_frame   (renderer_t* renderer);
void renderer_present     (renderer_t* renderer);
void renderer_resize      (renderer_t* renderer, int width, int height);

renderer_backend renderer_get_backend (renderer_t* renderer);
void             renderer_get_size    (renderer_t* renderer, int* width, int* height);

typedef enum {
    RENDERER_TEXTURE_FORMAT_RGBA8,
    RENDERER_TEXTURE_FORMAT_RGB8,
    RENDERER_TEXTURE_FORMAT_RG8,
    RENDERER_TEXTURE_FORMAT_R8,
    RENDERER_TEXTURE_FORMAT_RGBA16F,
    RENDERER_TEXTURE_FORMAT_RG16F,
    RENDERER_TEXTURE_FORMAT_R16F,
    RENDERER_TEXTURE_FORMAT_RGBA32F,
    RENDERER_TEXTURE_FORMAT_RG32F,
    RENDERER_TEXTURE_FORMAT_R32F,
    RENDERER_TEXTURE_FORMAT_RGBA8_SRGB,
    RENDERER_TEXTURE_FORMAT_BC1_UNORM,       /* DXT1  */
    RENDERER_TEXTURE_FORMAT_BC1_SRGB,
    RENDERER_TEXTURE_FORMAT_BC3_UNORM,       /* DXT5  */
    RENDERER_TEXTURE_FORMAT_BC3_SRGB,
    RENDERER_TEXTURE_FORMAT_BC4_UNORM,       /* ATI1  */
    RENDERER_TEXTURE_FORMAT_BC5_UNORM,       /* ATI2  */
    RENDERER_TEXTURE_FORMAT_DEPTH16,
    RENDERER_TEXTURE_FORMAT_DEPTH32F,
    RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8,
    RENDERER_TEXTURE_FORMAT_DEPTH32F_STENCIL8,
} renderer_texture_format;

typedef enum {
    RENDERER_LOAD_OP_LOAD,           /* keep existing contents            */
    RENDERER_LOAD_OP_CLEAR,          /* clear to the provided clear value */
    RENDERER_LOAD_OP_DONT_CARE,      /* undefined — fastest on tile GPUs  */
} renderer_load_op;

typedef enum {
    RENDERER_STORE_OP_STORE,         /* write result to memory            */
    RENDERER_STORE_OP_DONT_CARE,     /* discard — fastest on tile GPUs    */
} renderer_store_op;

typedef struct {
    renderer_texture_format color_format;
    renderer_texture_format depth_format;
    int                     has_depth;
    int                     has_stencil;
    int                     sample_count;    /* 1 = no MSAA                   */
    renderer_load_op        color_load_op;
    renderer_store_op       color_store_op;
    renderer_load_op        depth_load_op;
    renderer_store_op       depth_store_op;
    renderer_load_op        stencil_load_op;
    renderer_store_op       stencil_store_op;
} renderer_render_pass_desc;

renderer_render_pass_t* renderer_render_pass_create  (renderer_t* renderer,
                                                      const renderer_render_pass_desc* desc);
void                    renderer_render_pass_destroy (renderer_t* renderer,
                                                      renderer_render_pass_t* rp);

/* Returns built-in swapchain render pass; do not destroy. */
renderer_render_pass_t* renderer_get_swapchain_render_pass(renderer_t* renderer);

#define RENDERER_PUSH_CONSTANT_MAX_SIZE 128
#define RENDERER_MAX_VERTEX_ATTRIBS      16

typedef enum {
    RENDERER_ATTRIB_FLOAT1 = 1,
    RENDERER_ATTRIB_FLOAT2 = 2,
    RENDERER_ATTRIB_FLOAT3 = 3,
    RENDERER_ATTRIB_FLOAT4 = 4,
} renderer_attrib_format;

typedef struct {
    uint32_t               location;
    uint32_t               offset;
    renderer_attrib_format format;
} renderer_vertex_attrib;

typedef enum {
    RENDERER_PRIMITIVE_TRIANGLES,
    RENDERER_PRIMITIVE_TRIANGLE_STRIP,
    RENDERER_PRIMITIVE_LINES,
    RENDERER_PRIMITIVE_POINTS,
} renderer_primitive;

typedef enum {
    RENDERER_COMPARE_NEVER,
    RENDERER_COMPARE_LESS,
    RENDERER_COMPARE_EQUAL,
    RENDERER_COMPARE_LEQUAL,
    RENDERER_COMPARE_GREATER,
    RENDERER_COMPARE_NOTEQUAL,
    RENDERER_COMPARE_GEQUAL,
    RENDERER_COMPARE_ALWAYS,
} renderer_compare_op;

typedef enum {
    RENDERER_BLEND_FACTOR_ZERO,
    RENDERER_BLEND_FACTOR_ONE,
    RENDERER_BLEND_FACTOR_SRC_ALPHA,
    RENDERER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    RENDERER_BLEND_FACTOR_DST_ALPHA,
    RENDERER_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
} renderer_blend_factor;

typedef enum {
    RENDERER_STENCIL_OP_KEEP,
    RENDERER_STENCIL_OP_ZERO,
    RENDERER_STENCIL_OP_REPLACE,
    RENDERER_STENCIL_OP_INCREMENT_CLAMP,
    RENDERER_STENCIL_OP_DECREMENT_CLAMP,
    RENDERER_STENCIL_OP_INVERT,
    RENDERER_STENCIL_OP_INCREMENT_WRAP,
    RENDERER_STENCIL_OP_DECREMENT_WRAP,
} renderer_stencil_op;

typedef struct {
    renderer_stencil_op fail_op;        /* stencil test fails                */
    renderer_stencil_op depth_fail_op;  /* stencil passes, depth fails       */
    renderer_stencil_op pass_op;        /* both pass                         */
    renderer_compare_op compare_op;
    uint8_t             compare_mask;
    uint8_t             write_mask;
    uint8_t             reference;      /* static ref; 0 = use dynamic state */
} renderer_stencil_state;

typedef enum {
    RENDERER_CULL_NONE,
    RENDERER_CULL_FRONT,
    RENDERER_CULL_BACK,
} renderer_cull_mode;

typedef enum {
    RENDERER_FRONT_FACE_CCW,   /* counter-clockwise (OpenGL/Vulkan default) */
    RENDERER_FRONT_FACE_CW,    /* clockwise (D3D default)                   */
} renderer_front_face;

typedef enum {
    RENDERER_FILL_SOLID,
    RENDERER_FILL_WIREFRAME,
} renderer_fill_mode;

typedef enum {
    RENDERER_BLEND_OP_ADD,
    RENDERER_BLEND_OP_SUBTRACT,
    RENDERER_BLEND_OP_REV_SUBTRACT,
    RENDERER_BLEND_OP_MIN,
    RENDERER_BLEND_OP_MAX,
} renderer_blend_op;

typedef enum {
    RENDERER_COLOR_WRITE_R    = 1 << 0,
    RENDERER_COLOR_WRITE_G    = 1 << 1,
    RENDERER_COLOR_WRITE_B    = 1 << 2,
    RENDERER_COLOR_WRITE_A    = 1 << 3,
    RENDERER_COLOR_WRITE_ALL  = 0xF,
} renderer_color_write_mask;

typedef struct {
    const char* vert_src;
    const char* frag_src;
    uint32_t vert_spirv_size;
    uint32_t frag_spirv_size;

    renderer_vertex_attrib  attribs[RENDERER_MAX_VERTEX_ATTRIBS];
    uint32_t                attrib_count;
    uint32_t                vertex_stride;

    renderer_primitive primitive;

    renderer_cull_mode  cull_mode;
    renderer_front_face front_face;
    renderer_fill_mode  fill_mode;
    int                 scissor_test_enable;
    float               depth_bias_constant_factor;
    float               depth_bias_slope_factor;
    float               depth_bias_clamp;
    int                 depth_clamp_enable;   /* clamp frags instead of clip  */

    int                 sample_count;         /* must match render pass       */
    int                 alpha_to_coverage_enable;

    int                 depth_test_enable;
    int                 depth_write_enable;
    renderer_compare_op depth_compare;
    int                 stencil_test_enable;
    renderer_stencil_state stencil_front;
    renderer_stencil_state stencil_back;

    int                   blend_enable;
    renderer_blend_factor blend_src_color;
    renderer_blend_factor blend_dst_color;
    renderer_blend_op     blend_op_color;
    renderer_blend_factor blend_src_alpha;
    renderer_blend_factor blend_dst_alpha;
    renderer_blend_op     blend_op_alpha;
    renderer_color_write_mask color_write_mask;

    uint32_t push_constant_size;

    renderer_render_pass_t* render_pass;
} renderer_pipeline_desc;

renderer_pipeline_t* renderer_pipeline_create  (renderer_t* renderer,
                                                const renderer_pipeline_desc* desc);
void                 renderer_pipeline_destroy (renderer_t* renderer,
                                                renderer_pipeline_t* pipeline);

typedef enum {
    RENDERER_BUFFER_VERTEX,
    RENDERER_BUFFER_INDEX,
    RENDERER_BUFFER_UNIFORM,
} renderer_buffer_type;

typedef enum {
    RENDERER_BUFFER_USAGE_STATIC,
    RENDERER_BUFFER_USAGE_DYNAMIC,
} renderer_buffer_usage;

typedef struct {
    renderer_buffer_type  type;
    renderer_buffer_usage usage;
    const void*           data;
    uint32_t              size;
} renderer_buffer_desc;

renderer_buffer_t* renderer_buffer_create  (renderer_t* renderer,
                                            const renderer_buffer_desc* desc);
void               renderer_buffer_destroy (renderer_t* renderer,
                                            renderer_buffer_t* buffer);
void               renderer_buffer_update  (renderer_t* renderer,
                                            renderer_buffer_t* buffer,
                                            const void* data,
                                            uint32_t size);

typedef enum {
    RENDERER_TEXTURE_USAGE_SAMPLED        = 1 << 0,  /* shader read                 */
    RENDERER_TEXTURE_USAGE_RENDER_TARGET  = 1 << 1,  /* color attachment            */
    RENDERER_TEXTURE_USAGE_DEPTH_STENCIL  = 1 << 2,  /* depth/stencil attachment    */
    RENDERER_TEXTURE_USAGE_STORAGE        = 1 << 3,  /* UAV / image load-store      */
    RENDERER_TEXTURE_USAGE_COPY_SRC       = 1 << 4,
    RENDERER_TEXTURE_USAGE_COPY_DST       = 1 << 5,
} renderer_texture_usage_flags;

typedef enum {
    RENDERER_FILTER_LINEAR,
    RENDERER_FILTER_NEAREST,
} renderer_filter;

typedef enum {
    RENDERER_WRAP_REPEAT,
    RENDERER_WRAP_CLAMP_TO_EDGE,
    RENDERER_WRAP_MIRRORED_REPEAT,
    RENDERER_WRAP_CLAMP_TO_BORDER,
} renderer_wrap_mode;

typedef struct {
    const void*             pixels;
    int                     width;
    int                     height;
    renderer_texture_format format;
    int                     generate_mipmaps;
    int                     mip_levels;       /* 0 = auto (full chain)        */
    int                     array_layers;     /* 1 = plain 2-D texture        */
    int                     sample_count;     /* 1 = no MSAA                  */
    uint32_t                usage;            /* renderer_texture_usage_flags */
    renderer_filter         min_filter;
    renderer_filter         mag_filter;
    renderer_wrap_mode      wrap_u;
    renderer_wrap_mode      wrap_v;
    renderer_wrap_mode      wrap_w;
    float                   max_anisotropy;   /* 1.0 = disabled               */
} renderer_texture_desc;

renderer_texture_t* renderer_texture_create  (renderer_t* renderer,
                                              const renderer_texture_desc* desc);
void                renderer_texture_destroy (renderer_t* renderer,
                                              renderer_texture_t* texture);

typedef struct {
    float r, g, b, a;
    float depth;
    uint32_t stencil;
} renderer_clear_value;

renderer_cmd_t* renderer_cmd_begin  (renderer_t* renderer);
void            renderer_cmd_submit (renderer_t* renderer, renderer_cmd_t* cmd);

void renderer_cmd_begin_render_pass (renderer_cmd_t*          cmd,
                                     renderer_render_pass_t*  rp,
                                     renderer_texture_t*      target_texture,
                                     renderer_texture_t*      depth_texture,
                                     const renderer_clear_value* clear);
void renderer_cmd_end_render_pass   (renderer_cmd_t* cmd);

void renderer_cmd_bind_pipeline (renderer_cmd_t* cmd, renderer_pipeline_t* pipeline);
void renderer_cmd_bind_vertex_buffer (renderer_cmd_t*    cmd,
                                      renderer_buffer_t* buffer,
                                      uint32_t           slot,
                                      uint32_t           byte_offset);
void renderer_cmd_bind_index_buffer  (renderer_cmd_t*    cmd,
                                      renderer_buffer_t* buffer,
                                      uint32_t           byte_offset);
void renderer_cmd_bind_texture (renderer_cmd_t*     cmd,
                                renderer_texture_t* texture,
                                uint32_t            slot);
void renderer_cmd_push_constants (renderer_cmd_t*      cmd,
                                  renderer_pipeline_t* pipeline,
                                  const void*          data,
                                  uint32_t             size);

#define RENDERER_MAX_TEXTURE_SLOTS 4          /* = VK_MAX_BOUND_TEXTURES          */
#define RENDERER_MAX_UBO_SLOTS     8          /* = VK_MAX_BOUND_UBOS              */
#define RENDERER_UBO_BINDING(slot) (RENDERER_MAX_TEXTURE_SLOTS + (slot))

void renderer_cmd_bind_uniform_buffer(renderer_cmd_t*    cmd,
                                      renderer_buffer_t* buffer,
                                      uint32_t           slot,
                                      uint32_t           byte_offset,
                                      uint32_t           byte_size);

void renderer_cmd_set_viewport (renderer_cmd_t* cmd,
                                float x, float y,
                                float width, float height,
                                float min_depth, float max_depth);
void renderer_cmd_set_scissor  (renderer_cmd_t* cmd,
                                int x, int y,
                                int width, int height);
void renderer_cmd_draw         (renderer_cmd_t* cmd,
                                uint32_t vertex_count,
                                uint32_t instance_count,
                                uint32_t first_vertex,
                                uint32_t first_instance);
void renderer_cmd_draw_indexed (renderer_cmd_t* cmd,
                                uint32_t index_count,
                                uint32_t instance_count,
                                uint32_t first_index,
                                int32_t  vertex_offset,
                                uint32_t first_instance);

#endif