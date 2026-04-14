#include "renderer_internal.h"
#include "fatal_error.h"
#include <stdlib.h>

static const renderer_backend_vtable* select_vtable(renderer_backend backend) {
    switch (backend) {
        case RENDERER_BACKEND_OPENGL: return renderer_backend_opengl_vtable();
        case RENDERER_BACKEND_VULKAN: return renderer_backend_vulkan_vtable();
        default:
            fatal_error("renderer_create: unknown backend %d.", (int)backend);
            return NULL;
    }
}

char* renderer_backend_to_string(renderer_backend backend) {
    switch (backend) {
        case RENDERER_BACKEND_OPENGL: return "OpenGL";
        case RENDERER_BACKEND_VULKAN: return "Vulkan";
    }
    return "NO BACKEND";
}

renderer_t* renderer_create(const renderer_create_desc* desc) {
    if (!desc || !desc->native_window_handle) return NULL;
    const renderer_backend_vtable* vtable = select_vtable(desc->backend);
    if (!vtable) return NULL;
    renderer_t* r = calloc(1, sizeof(renderer_t));
    if (!r) return NULL;
    r->vtable = vtable;
    r->tag    = desc->backend;
    r->width  = desc->width;
    r->height = desc->height;
    if (vtable->init(r, desc) != 0) { free(r); return NULL; }
    return r;
}

void renderer_destroy(renderer_t* r) {
    if (!r) return;
    r->vtable->shutdown(r);
    free(r);
}

void renderer_begin_frame(renderer_t* r) { if (r) r->vtable->begin_frame(r); }
void renderer_end_frame  (renderer_t* r) { if (r) r->vtable->end_frame(r);   }
void renderer_present    (renderer_t* r) { if (r) r->vtable->present(r);     }

void renderer_resize(renderer_t* r, int w, int h) {
    if (!r) return;
    r->width = w; r->height = h;
    r->vtable->resize(r, w, h);
}

renderer_backend renderer_get_backend(renderer_t* r) { return r->tag; }

void renderer_get_size(renderer_t* r, int* w, int* h) {
    if (!r) return;
    if (w) *w = r->width;
    if (h) *h = r->height;
}

renderer_render_pass_t* renderer_render_pass_create(renderer_t* r,
                                                    const renderer_render_pass_desc* desc) {
    if (!r || !desc) return NULL;
    return r->vtable->render_pass_create(r, desc);
}

void renderer_render_pass_destroy(renderer_t* r, renderer_render_pass_t* rp) {
    if (r && rp) r->vtable->render_pass_destroy(r, rp);
}

renderer_render_pass_t* renderer_get_swapchain_render_pass(renderer_t* r) {
    if (!r) return NULL;
    return r->vtable->get_swapchain_render_pass(r);
}

renderer_pipeline_t* renderer_pipeline_create(renderer_t* r,
                                              const renderer_pipeline_desc* desc) {
    if (!r || !desc) return NULL;
    return r->vtable->pipeline_create(r, desc);
}

void renderer_pipeline_destroy(renderer_t* r, renderer_pipeline_t* p) {
    if (r && p) r->vtable->pipeline_destroy(r, p);
}

renderer_buffer_t* renderer_buffer_create(renderer_t* r,
                                          const renderer_buffer_desc* desc) {
    if (!r || !desc) return NULL;
    return r->vtable->buffer_create(r, desc);
}

void renderer_buffer_destroy(renderer_t* r, renderer_buffer_t* b) {
    if (r && b) r->vtable->buffer_destroy(r, b);
}

void renderer_buffer_update(renderer_t* r, renderer_buffer_t* b,
                             const void* data, uint32_t size) {
    if (r && b && data) r->vtable->buffer_update(r, b, data, size);
}

renderer_texture_t* renderer_texture_create(renderer_t* r,
                                             const renderer_texture_desc* desc) {
    if (!r || !desc) return NULL;
    return r->vtable->texture_create(r, desc);
}

void renderer_texture_destroy(renderer_t* r, renderer_texture_t* t) {
    if (r && t) r->vtable->texture_destroy(r, t);
}

renderer_cmd_t* renderer_cmd_begin(renderer_t* r) {
    if (!r) return NULL;
    return r->vtable->cmd_begin(r);
}

void renderer_cmd_submit(renderer_t* r, renderer_cmd_t* cmd) {
    if (r && cmd) r->vtable->cmd_submit(r, cmd);
}

void renderer_cmd_begin_render_pass(renderer_cmd_t* cmd,
                                    renderer_render_pass_t* rp,
                                    renderer_texture_t* color,
                                    renderer_texture_t* depth,
                                    const renderer_clear_value* clear) {
    if (cmd) cmd->vtable->cmd_begin_render_pass(cmd, rp, color, depth, clear);
}

void renderer_cmd_end_render_pass(renderer_cmd_t* cmd) {
    if (cmd) cmd->vtable->cmd_end_render_pass(cmd);
}

void renderer_cmd_bind_pipeline(renderer_cmd_t* cmd, renderer_pipeline_t* p) {
    if (cmd) cmd->vtable->cmd_bind_pipeline(cmd, p);
}

void renderer_cmd_bind_vertex_buffer(renderer_cmd_t* cmd, renderer_buffer_t* b,
                                     uint32_t slot, uint32_t offset) {
    if (cmd) cmd->vtable->cmd_bind_vertex_buffer(cmd, b, slot, offset);
}

void renderer_cmd_bind_index_buffer(renderer_cmd_t* cmd, renderer_buffer_t* b,
                                    uint32_t offset) {
    if (cmd) cmd->vtable->cmd_bind_index_buffer(cmd, b, offset);
}

void renderer_cmd_bind_texture(renderer_cmd_t* cmd, renderer_texture_t* t,
                                uint32_t slot) {
    if (cmd) cmd->vtable->cmd_bind_texture(cmd, t, slot);
}

void renderer_cmd_push_constants(renderer_cmd_t* cmd, renderer_pipeline_t* p,
                                  const void* data, uint32_t size) {
    if (cmd) cmd->vtable->cmd_push_constants(cmd, p, data, size);
}

void renderer_cmd_bind_uniform_buffer(renderer_cmd_t* cmd, renderer_buffer_t* b,
                                      uint32_t slot, uint32_t byte_offset,
                                      uint32_t byte_size) {
    if (cmd && b)
        cmd->vtable->cmd_bind_uniform_buffer(cmd, b, slot, byte_offset, byte_size);
}

void renderer_cmd_set_viewport(renderer_cmd_t* cmd, float x, float y,
                                float w, float h, float min_depth, float max_depth) {
    if (cmd) cmd->vtable->cmd_set_viewport(cmd, x, y, w, h, min_depth, max_depth);
}

void renderer_cmd_set_scissor(renderer_cmd_t* cmd, int x, int y, int w, int h) {
    if (cmd) cmd->vtable->cmd_set_scissor(cmd, x, y, w, h);
}

void renderer_cmd_draw(renderer_cmd_t* cmd, uint32_t vertex_count,
                       uint32_t instance_count, uint32_t first_vertex,
                       uint32_t first_instance) {
    if (cmd) cmd->vtable->cmd_draw(cmd, vertex_count, instance_count,
                                   first_vertex, first_instance);
}

void renderer_cmd_draw_indexed(renderer_cmd_t* cmd, uint32_t index_count,
                                uint32_t instance_count, uint32_t first_index,
                                int32_t  vertex_offset, uint32_t first_instance) {
    if (cmd) cmd->vtable->cmd_draw_indexed(cmd, index_count, instance_count,
                                           first_index, vertex_offset, first_instance);
}