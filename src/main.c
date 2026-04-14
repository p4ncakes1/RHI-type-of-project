#include "window.h"
#include "renderer.h"
#include "fatal_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SELECTED_BACKEND RENDERER_BACKEND_VULKAN

static const char* GLSL_VERT =
    "#version 420 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec4 in_color;\n"
    "layout(location = 2) in vec2 in_uv;\n"
    "out vec4 v_color;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_pos, 1.0);\n"
    "    v_color = in_color;\n"
    "    v_uv = in_uv;\n"
    "}\n";

static const char* GLSL_FRAG =
    "#version 420 core\n"
    "in  vec4 v_color;\n"
    "in  vec2 v_uv;\n"
    "out vec4 out_color;\n"
    "uniform sampler2D tex0;\n"
    "layout(std140, binding = 1) uniform UBO { vec4 colorMod; };\n"
    "void main() {\n"
    "    out_color = v_color * texture(tex0, v_uv) * colorMod;\n"
    "}\n";

static const char* GLSL_BLIT_VERT =
    "#version 420 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexID & 1) * 4.0 - 1.0,\n"
    "                  (gl_VertexID & 2) * 2.0 - 1.0);\n"
    "    v_uv = p * 0.5 + 0.5;\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char* GLSL_BLIT_FRAG =
    "#version 420 core\n"
    "in  vec2 v_uv;\n"
    "out vec4 out_color;\n"
    "uniform sampler2D tex0;\n"
    "layout(std140, binding = 1) uniform UBO { vec4 colorMod; };\n"
    "void main() {\n"
    "    out_color = texture(tex0, v_uv) * colorMod;\n"
    "}\n";

static const float k_vertices[] = {
     0.0f,  0.5f, 0.0f,   1.0f, 0.0f, 0.0f, 1.0f,   0.5f, 0.0f,
    -0.5f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f, 1.0f,   0.0f, 1.0f,
     0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f, 1.0f,   1.0f, 1.0f,
};

typedef struct {
    window_t*   win;
    renderer_t* renderer;
    renderer_pipeline_t* pipeline;
    renderer_buffer_t*   vbo;
    renderer_buffer_t*   ubo;
    renderer_texture_t*  checker_tex;
    renderer_texture_t*     offscreen_color;
    renderer_texture_t*     offscreen_depth;
    renderer_render_pass_t* offscreen_rp;
    renderer_pipeline_t* blit_pipeline;
    renderer_buffer_t*   blit_ubo;

    int width;
    int height;
} app_t;

static void* read_binary_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "read_binary_file: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    size_t padded = ((size_t)sz + 3u) & ~3u;
    void* buf = calloc(1, padded);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (out_size) *out_size = padded;
    return buf;
}

static int create_offscreen_targets(app_t* app, int w, int h) {
    app->offscreen_color = renderer_texture_create(app->renderer, &(renderer_texture_desc){
        .width        = w,
        .height       = h,
        .format       = RENDERER_TEXTURE_FORMAT_RGBA8,
        .usage        = RENDERER_TEXTURE_USAGE_RENDER_TARGET
                      | RENDERER_TEXTURE_USAGE_SAMPLED,
        .sample_count = 1,
        .min_filter   = RENDERER_FILTER_LINEAR,
        .mag_filter   = RENDERER_FILTER_LINEAR,
        .wrap_u       = RENDERER_WRAP_CLAMP_TO_EDGE,
        .wrap_v       = RENDERER_WRAP_CLAMP_TO_EDGE,
    });
    if (!app->offscreen_color) {
        fprintf(stderr, "offscreen color texture creation failed\n");
        return -1;
    }

    app->offscreen_depth = renderer_texture_create(app->renderer, &(renderer_texture_desc){
        .width        = w,
        .height       = h,
        .format       = RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8,
        .usage        = RENDERER_TEXTURE_USAGE_DEPTH_STENCIL,
        .sample_count = 1,
    });
    if (!app->offscreen_depth) {
        fprintf(stderr, "offscreen depth texture creation failed\n");
        renderer_texture_destroy(app->renderer, app->offscreen_color);
        app->offscreen_color = NULL;
        return -1;
    }

    app->offscreen_rp = renderer_render_pass_create(app->renderer, &(renderer_render_pass_desc){
        .color_format     = RENDERER_TEXTURE_FORMAT_RGBA8,
        .depth_format     = RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8,
        .has_depth        = 1,
        .has_stencil      = 1,
        .sample_count     = 1,
        .color_load_op    = RENDERER_LOAD_OP_CLEAR,
        .color_store_op   = RENDERER_STORE_OP_STORE,
        .depth_load_op    = RENDERER_LOAD_OP_CLEAR,
        .depth_store_op   = RENDERER_STORE_OP_DONT_CARE,
        .stencil_load_op  = RENDERER_LOAD_OP_CLEAR,
        .stencil_store_op = RENDERER_STORE_OP_DONT_CARE,
    });
    if (!app->offscreen_rp) {
        fprintf(stderr, "offscreen render pass creation failed\n");
        renderer_texture_destroy(app->renderer, app->offscreen_color);
        renderer_texture_destroy(app->renderer, app->offscreen_depth);
        app->offscreen_color = NULL;
        app->offscreen_depth = NULL;
        return -1;
    }
    return 0;
}

static void destroy_offscreen_targets(app_t* app) {
    if (app->offscreen_rp)    { renderer_render_pass_destroy(app->renderer, app->offscreen_rp);    app->offscreen_rp    = NULL; }
    if (app->offscreen_depth) { renderer_texture_destroy    (app->renderer, app->offscreen_depth); app->offscreen_depth = NULL; }
    if (app->offscreen_color) { renderer_texture_destroy    (app->renderer, app->offscreen_color); app->offscreen_color = NULL; }
}

static int create_pipelines(app_t* app,
                             const char* vert_src, uint32_t vert_sz,
                             const char* frag_src, uint32_t frag_sz,
                             const char* blit_vert_src, uint32_t blit_vert_sz,
                             const char* blit_frag_src, uint32_t blit_frag_sz) {
    app->pipeline = renderer_pipeline_create(app->renderer, &(renderer_pipeline_desc){
        .vert_src        = vert_src,
        .frag_src        = frag_src,
        .vert_spirv_size = vert_sz,
        .frag_spirv_size = frag_sz,
        .attribs = {
            { .location = 0, .offset =  0, .format = RENDERER_ATTRIB_FLOAT3 }, /* pos   */
            { .location = 1, .offset = 12, .format = RENDERER_ATTRIB_FLOAT4 }, /* color */
            { .location = 2, .offset = 28, .format = RENDERER_ATTRIB_FLOAT2 }, /* uv    */
        },
        .attrib_count     = 3,
        .vertex_stride    = 36,
        .primitive        = RENDERER_PRIMITIVE_TRIANGLES,
        .cull_mode        = RENDERER_CULL_NONE,
        .front_face       = RENDERER_FRONT_FACE_CCW,
        .fill_mode        = RENDERER_FILL_SOLID,
        .sample_count     = 1,
        .color_write_mask = RENDERER_COLOR_WRITE_ALL,
        .render_pass      = app->offscreen_rp,
    });
    if (!app->pipeline) { fprintf(stderr, "scene pipeline creation failed\n"); return -1; }

    app->blit_pipeline = renderer_pipeline_create(app->renderer, &(renderer_pipeline_desc){
        .vert_src        = blit_vert_src,
        .frag_src        = blit_frag_src,
        .vert_spirv_size = blit_vert_sz,
        .frag_spirv_size = blit_frag_sz,
        .attrib_count     = 0,
        .vertex_stride    = 0,
        .primitive        = RENDERER_PRIMITIVE_TRIANGLES,
        .cull_mode        = RENDERER_CULL_NONE,
        .front_face       = RENDERER_FRONT_FACE_CCW,
        .fill_mode        = RENDERER_FILL_SOLID,
        .sample_count     = 4,
        .color_write_mask = RENDERER_COLOR_WRITE_ALL,
        .render_pass      = renderer_get_swapchain_render_pass(app->renderer),
    });
    if (!app->blit_pipeline) { fprintf(stderr, "blit pipeline creation failed\n"); return -1; }

    return 0;
}

static int init_renderer(app_t* app, renderer_backend backend) {
    app->renderer = renderer_create(&(renderer_create_desc){
        .native_window_handle  = window_get_native_handle(app->win),
        .native_display_handle = window_get_native_display(app->win),
        .width        = app->width,
        .height       = app->height,
        .vsync        = 1,
        .sample_count = 4,
        .backend      = backend,
        .color_format = RENDERER_TEXTURE_FORMAT_RGBA8,
        .depth_format = RENDERER_TEXTURE_FORMAT_DEPTH24_STENCIL8,
    });
    if (!app->renderer) { fprintf(stderr, "renderer_create failed\n"); return -1; }

    uint32_t checker_pixels[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            checker_pixels[y * 64 + x] = ((x / 8 + y / 8) % 2) ? 0xFFFFFFFF : 0xFF000000;

    app->checker_tex = renderer_texture_create(app->renderer, &(renderer_texture_desc){
        .width = 64, .height = 64,
        .format     = RENDERER_TEXTURE_FORMAT_RGBA8,
        .usage      = RENDERER_TEXTURE_USAGE_SAMPLED,
        .pixels     = checker_pixels,
        .min_filter = RENDERER_FILTER_NEAREST,
        .mag_filter = RENDERER_FILTER_NEAREST,
        .wrap_u     = RENDERER_WRAP_REPEAT,
        .wrap_v     = RENDERER_WRAP_REPEAT,
    });
    if (!app->checker_tex) { fprintf(stderr, "checker texture creation failed\n"); return -1; }

    const char* vert_src = NULL, *frag_src = NULL;
    const char* blit_vert_src = NULL, *blit_frag_src = NULL;
    void* vert_spv = NULL, *frag_spv = NULL;
    void* blit_vert_spv = NULL, *blit_frag_spv = NULL;
    size_t vert_sz = 0, frag_sz = 0, blit_vert_sz = 0, blit_frag_sz = 0;

    if (backend == RENDERER_BACKEND_VULKAN) {
        vert_spv      = read_binary_file("shaders/triangle_textured.vert.spv", &vert_sz);
        frag_spv      = read_binary_file("shaders/triangle_textured.frag.spv", &frag_sz);
        blit_vert_spv = read_binary_file("shaders/blit.vert.spv", &blit_vert_sz);
        blit_frag_spv = read_binary_file("shaders/blit.frag.spv", &blit_frag_sz);
        if (!vert_spv || !frag_spv || !blit_vert_spv || !blit_frag_spv) {
            fprintf(stderr, "Vulkan: could not load SPIR-V shaders. "
                            "Compile GLSL to SPIR-V and place in shaders/\n");
            free(vert_spv); free(frag_spv); free(blit_vert_spv); free(blit_frag_spv);
            return -1;
        }
        vert_src = (const char*)vert_spv; frag_src = (const char*)frag_spv;
        blit_vert_src = (const char*)blit_vert_spv; blit_frag_src = (const char*)blit_frag_spv;
    } else {
        vert_src = GLSL_VERT; frag_src = GLSL_FRAG;
        blit_vert_src = GLSL_BLIT_VERT; blit_frag_src = GLSL_BLIT_FRAG;
    }

    app->vbo = renderer_buffer_create(app->renderer, &(renderer_buffer_desc){
        .type = RENDERER_BUFFER_VERTEX, .usage = RENDERER_BUFFER_USAGE_STATIC,
        .data = k_vertices, .size = sizeof(k_vertices),
    });
    if (!app->vbo) { fprintf(stderr, "VBO creation failed\n"); return -1; }

    float ones[4] = { 1, 1, 1, 1 };
    app->ubo = renderer_buffer_create(app->renderer, &(renderer_buffer_desc){
        .type = RENDERER_BUFFER_UNIFORM, .usage = RENDERER_BUFFER_USAGE_DYNAMIC,
        .data = ones, .size = sizeof(ones),
    });
    if (!app->ubo) { fprintf(stderr, "UBO creation failed\n"); return -1; }

    app->blit_ubo = renderer_buffer_create(app->renderer, &(renderer_buffer_desc){
        .type = RENDERER_BUFFER_UNIFORM, .usage = RENDERER_BUFFER_USAGE_DYNAMIC,
        .data = ones, .size = sizeof(ones),
    });
    if (!app->blit_ubo) { fprintf(stderr, "blit UBO creation failed\n"); return -1; }

    if (create_offscreen_targets(app, app->width, app->height) != 0)
        return -1;

    int rc = create_pipelines(app,
        vert_src, (uint32_t)vert_sz, frag_src, (uint32_t)frag_sz,
        blit_vert_src, (uint32_t)blit_vert_sz,
        blit_frag_src, (uint32_t)blit_frag_sz);

    free(vert_spv); free(frag_spv); free(blit_vert_spv); free(blit_frag_spv);
    return rc;
}

static void on_event(window_t* win, const window_event* e, void* userdata) {
    app_t* app = (app_t*)userdata;
    switch (e->type) {
        case WINDOW_EVENT_CLOSE:
            window_request_close(win);
            break;

        case WINDOW_EVENT_RESIZE: {
            int nw = e->resize.width  > 1 ? e->resize.width  : 1;
            int nh = e->resize.height > 1 ? e->resize.height : 1;
            app->width  = nw;
            app->height = nh;
            if (!app->renderer) break;

            renderer_resize(app->renderer, nw, nh);

            if (app->blit_pipeline) { renderer_pipeline_destroy(app->renderer, app->blit_pipeline); app->blit_pipeline = NULL; }
            if (app->pipeline)      { renderer_pipeline_destroy(app->renderer, app->pipeline);      app->pipeline = NULL; }
            destroy_offscreen_targets(app);

            if (create_offscreen_targets(app, nw, nh) != 0) break;

            renderer_backend backend = renderer_get_backend(app->renderer);
            const char* vs = NULL, *fs = NULL, *bvs = NULL, *bfs = NULL;
            void *vspv = NULL, *fspv = NULL, *bvspv = NULL, *bfspv = NULL;
            size_t vsz = 0, fsz = 0, bvsz = 0, bfsz = 0;
            if (backend == RENDERER_BACKEND_VULKAN) {
                vspv  = read_binary_file("shaders/triangle_textured.vert.spv", &vsz);
                fspv  = read_binary_file("shaders/triangle_textured.frag.spv", &fsz);
                bvspv = read_binary_file("shaders/blit.vert.spv", &bvsz);
                bfspv = read_binary_file("shaders/blit.frag.spv", &bfsz);
                vs = (const char*)vspv; fs = (const char*)fspv;
                bvs = (const char*)bvspv; bfs = (const char*)bfspv;
            } else {
                vs = GLSL_VERT; fs = GLSL_FRAG; bvs = GLSL_BLIT_VERT; bfs = GLSL_BLIT_FRAG;
            }
            create_pipelines(app, vs, (uint32_t)vsz, fs, (uint32_t)fsz,
                             bvs, (uint32_t)bvsz, bfs, (uint32_t)bfsz);
            free(vspv); free(fspv); free(bvspv); free(bfspv);
            break;
        }

        default:
            break;
    }
}

int main(void) {
    window_t* win = window_create(&(window_create_desc){
        .width   = 1280,
        .height  = 720,
        .title   = "renderer – offscreen + blit demo",
        .backend = WINDOW_BACKEND_GLFW,
    });
    if (!win) fatal_error("window_create failed");

    app_t app;
    memset(&app, 0, sizeof(app));
    app.win    = win;
    app.width  = 1280;
    app.height = 720;

    window_set_event_callback(win, on_event, &app);

    if (init_renderer(&app, SELECTED_BACKEND) != 0)
        fatal_error("renderer / resource creation failed");

    printf("Backend : %s\n", renderer_backend_to_string(renderer_get_backend(app.renderer)));
    printf("Controls: ESC = quit\n");

    double last_fps_time = window_get_time(win);
    int    frame_count   = 0;

    while (!window_should_close(win)) {
        window_poll_events(win);

        if (window_key_pressed(win, KEY_ESCAPE))
            window_request_close(win);

        if (!app.renderer || !app.pipeline || !app.blit_pipeline
            || !app.offscreen_color || !app.offscreen_rp)
            continue;

        double now = window_get_time(win);
        if (++frame_count, now - last_fps_time >= 1.0) {
            printf("FPS: %d\n", frame_count);
            frame_count = 0; last_fps_time = now;
        }

        int w, h;
        renderer_get_size(app.renderer, &w, &h);

        float scene_ubo[4] = { (float)(sin(now) * 0.5 + 0.5), 1.f, 1.f, 1.f };
        renderer_buffer_update(app.renderer, app.ubo, scene_ubo, sizeof(scene_ubo));

        float fade = (float)(sin(now * 0.4) * 0.25 + 0.75);
        float blit_ubo[4] = { fade, fade, fade, 1.f };
        renderer_buffer_update(app.renderer, app.blit_ubo, blit_ubo, sizeof(blit_ubo));

        renderer_begin_frame(app.renderer);
        renderer_cmd_t* cmd = renderer_cmd_begin(app.renderer);

        {
            const renderer_clear_value cl = {
                .r = 0.05f, .g = 0.05f, .b = 0.15f, .a = 1.f,
                .depth = 1.f, .stencil = 0,
            };
            renderer_cmd_begin_render_pass(cmd,
                app.offscreen_rp,
                app.offscreen_color,
                app.offscreen_depth,
                &cl);

            renderer_cmd_set_viewport(cmd, 0.f, 0.f, (float)w, (float)h, 0.f, 1.f);
            renderer_cmd_set_scissor (cmd, 0, 0, w, h);
            renderer_cmd_bind_pipeline      (cmd, app.pipeline);
            renderer_cmd_bind_vertex_buffer (cmd, app.vbo, 0, 0);
            renderer_cmd_bind_uniform_buffer(cmd, app.ubo, 0, 0, 0);
            renderer_cmd_bind_texture       (cmd, app.checker_tex, 0);
            renderer_cmd_draw               (cmd, 3, 1, 0, 0);

            renderer_cmd_end_render_pass(cmd);
        }
        {
            renderer_render_pass_t* sc_rp =
                renderer_get_swapchain_render_pass(app.renderer);

            const renderer_clear_value cl = {
                .r = 0.f, .g = 0.f, .b = 0.f, .a = 1.f,
                .depth = 1.f, .stencil = 0,
            };
            renderer_cmd_begin_render_pass(cmd, sc_rp, NULL, NULL, &cl);

            renderer_cmd_set_viewport(cmd, 0.f, 0.f, (float)w, (float)h, 0.f, 1.f);
            renderer_cmd_set_scissor (cmd, 0, 0, w, h);
            renderer_cmd_bind_pipeline      (cmd, app.blit_pipeline);
            renderer_cmd_bind_texture       (cmd, app.offscreen_color, 0);
            renderer_cmd_bind_uniform_buffer(cmd, app.blit_ubo, 0, 0, 0);
            renderer_cmd_draw               (cmd, 3, 1, 0, 0);

            renderer_cmd_end_render_pass(cmd);
        }

        renderer_cmd_submit(app.renderer, cmd);
        renderer_end_frame (app.renderer);
        renderer_present   (app.renderer);
    }

    if (app.blit_pipeline) renderer_pipeline_destroy(app.renderer, app.blit_pipeline);
    if (app.pipeline)      renderer_pipeline_destroy(app.renderer, app.pipeline);
    destroy_offscreen_targets(&app);
    if (app.blit_ubo)     renderer_buffer_destroy (app.renderer, app.blit_ubo);
    if (app.ubo)          renderer_buffer_destroy (app.renderer, app.ubo);
    if (app.vbo)          renderer_buffer_destroy (app.renderer, app.vbo);
    if (app.checker_tex)  renderer_texture_destroy(app.renderer, app.checker_tex);
    renderer_destroy(app.renderer);
    window_terminate(win);
    return 0;
}