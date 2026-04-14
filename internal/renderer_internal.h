#ifndef RENDERER_INTERNAL_H
#define RENDERER_INTERNAL_H

#include "renderer.h"

struct renderer_t             { const struct renderer_backend_vtable* vtable;
                                void* backend_data;
                                renderer_backend tag;
                                int width; int height; };
struct renderer_render_pass_t { void* backend_data; };
struct renderer_pipeline_t    { void* backend_data; };
struct renderer_buffer_t      { void* backend_data; };
struct renderer_texture_t     { void* backend_data; };
struct renderer_cmd_t         { void* backend_data;
                                const struct renderer_backend_vtable* vtable; };

typedef struct renderer_backend_vtable {
    int  (*init)    (renderer_t*, const renderer_create_desc*);
    void (*shutdown)(renderer_t*);
    void (*begin_frame)(renderer_t*);
    void (*end_frame)  (renderer_t*);
    void (*present)    (renderer_t*);
    void (*resize)     (renderer_t*, int w, int h);

    renderer_render_pass_t* (*get_swapchain_render_pass)(renderer_t*);
    renderer_render_pass_t* (*render_pass_create) (renderer_t*,
                                                   const renderer_render_pass_desc*);
    void                    (*render_pass_destroy)(renderer_t*,
                                                   renderer_render_pass_t*);

    renderer_pipeline_t* (*pipeline_create) (renderer_t*,
                                             const renderer_pipeline_desc*);
    void                 (*pipeline_destroy)(renderer_t*, renderer_pipeline_t*);

    renderer_buffer_t* (*buffer_create) (renderer_t*, const renderer_buffer_desc*);
    void               (*buffer_destroy)(renderer_t*, renderer_buffer_t*);
    void               (*buffer_update) (renderer_t*, renderer_buffer_t*,
                                         const void* data, uint32_t size);

    renderer_texture_t* (*texture_create) (renderer_t*, const renderer_texture_desc*);
    void                (*texture_destroy)(renderer_t*, renderer_texture_t*);

    renderer_cmd_t* (*cmd_begin) (renderer_t*);
    void            (*cmd_submit)(renderer_t*, renderer_cmd_t*);

    void (*cmd_begin_render_pass)(renderer_cmd_t*, renderer_render_pass_t*,
                                  renderer_texture_t* color,
                                  renderer_texture_t* depth,
                                  const renderer_clear_value*);
    void (*cmd_end_render_pass)  (renderer_cmd_t*);

    void (*cmd_bind_pipeline)      (renderer_cmd_t*, renderer_pipeline_t*);
    void (*cmd_bind_vertex_buffer) (renderer_cmd_t*, renderer_buffer_t*,
                                    uint32_t slot, uint32_t byte_offset);
    void (*cmd_bind_index_buffer)  (renderer_cmd_t*, renderer_buffer_t*,
                                    uint32_t byte_offset);
    void (*cmd_bind_texture)       (renderer_cmd_t*, renderer_texture_t*,
                                    uint32_t slot);
    void (*cmd_push_constants)     (renderer_cmd_t*, renderer_pipeline_t*,
                                    const void* data, uint32_t size);
    void (*cmd_bind_uniform_buffer)(renderer_cmd_t*, renderer_buffer_t*,
                                    uint32_t slot,
                                    uint32_t byte_offset,
                                    uint32_t byte_size);
    void (*cmd_set_viewport)       (renderer_cmd_t*,
                                    float x, float y, float w, float h,
                                    float min_depth, float max_depth);
    void (*cmd_set_scissor)        (renderer_cmd_t*,
                                    int x, int y, int w, int h);
    void (*cmd_draw)               (renderer_cmd_t*, uint32_t vertex_count,
                                    uint32_t instance_count,
                                    uint32_t first_vertex,
                                    uint32_t first_instance);
    void (*cmd_draw_indexed)       (renderer_cmd_t*, uint32_t index_count,
                                    uint32_t instance_count,
                                    uint32_t first_index,
                                    int32_t  vertex_offset,
                                    uint32_t first_instance);
} renderer_backend_vtable;

const renderer_backend_vtable* renderer_backend_opengl_vtable(void);
const renderer_backend_vtable* renderer_backend_vulkan_vtable(void);

#endif