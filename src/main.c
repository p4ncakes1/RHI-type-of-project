#include "window.h"
#include "renderer.h"
#include "fatal_error.h"
#include <stdio.h>
#include <stdlib.h>

static void* read_binary_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "read_binary_file: cannot open '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    size_t padded = ((size_t)sz + 3u) & ~3u;
    void* buf = calloc(1, padded + 4);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (out_size) *out_size = padded;
    return buf;
}

static const char* GLSL_VERT =
    "#version 330 core\n"
    "layout(location = 0) in vec3 in_pos;\n"
    "layout(location = 1) in vec4 in_color;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_pos, 1.0);\n"
    "    v_color = in_color;\n"
    "}\n";

static const char* GLSL_FRAG =
    "#version 330 core\n"
    "in  vec4 v_color;\n"
    "out vec4 out_color;\n"
    "void main() {\n"
    "    out_color = v_color;\n"
    "}\n";

static const char* HLSL_VERT =
    "struct VSIn  { float3 pos : ATTR0; float4 col : ATTR1; };\n"
    "struct PSIn  { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "PSIn main(VSIn i) {\n"
    "    PSIn o;\n"
    "    o.pos = float4(i.pos, 1.0f);\n"
    "    o.col = i.col;\n"
    "    return o;\n"
    "}\n";

static const char* HLSL_FRAG =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float4 main(PSIn i) : SV_Target { return i.col; }\n";

static const float k_vertices[] = {
     0.0f,  0.5f, 0.0f,   1.0f, 0.0f, 0.0f, 1.0f,
    -0.5f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f, 1.0f,
     0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f, 1.0f,
};

typedef struct {
    renderer_t*          renderer;
    renderer_pipeline_t* pipeline;
    renderer_buffer_t*   vbo;
    window_t*            win;
    renderer_backend     backend;
    int                  width;
    int                  height;
} app_t;

static const renderer_backend k_backend_cycle[] = {
    RENDERER_BACKEND_VULKAN,
    RENDERER_BACKEND_OPENGL,
    RENDERER_BACKEND_D3D11,
};
static const char* k_backend_names[] = { "Vulkan", "OpenGL", "D3D11" };
#define BACKEND_COUNT ((int)(sizeof(k_backend_cycle) / sizeof(k_backend_cycle[0])))

static int backend_to_cycle_index(renderer_backend b) {
    for (int i = 0; i < BACKEND_COUNT; ++i)
        if (k_backend_cycle[i] == b) return i;
    return 0;
}

/* Tears down all renderer-owned objects and rebuilds them for new_backend.
 * The window is not touched.  Returns 0 on success, -1 on failure. */
static int app_switch_backend(app_t* app, renderer_backend new_backend) {
    if (app->renderer) {
        renderer_pipeline_destroy(app->renderer, app->pipeline);
        renderer_buffer_destroy(app->renderer, app->vbo);
        renderer_destroy(app->renderer);
        app->renderer = NULL;
        app->pipeline = NULL;
        app->vbo      = NULL;
    }

    app->renderer = renderer_create(&(renderer_create_desc){
        .native_window_handle  = window_get_native_handle(app->win),
        .native_display_handle = window_get_native_display(app->win),
        .width        = app->width,
        .height       = app->height,
        .vsync        = 0,
        .sample_count = 4,
        .backend      = new_backend,
    });
    if (!app->renderer) {
        fprintf(stderr, "renderer_create failed for %s\n",
                k_backend_names[backend_to_cycle_index(new_backend)]);
        return -1;
    }
    app->backend = new_backend;

    int is_vulkan = (new_backend == RENDERER_BACKEND_VULKAN);
    int is_d3d11  = (new_backend == RENDERER_BACKEND_D3D11);

    const char* vert_src  = NULL;
    const char* frag_src  = NULL;
    void*       vert_spv  = NULL;
    void*       frag_spv  = NULL;
    size_t      vert_size = 0;
    size_t      frag_size = 0;

    if (is_d3d11) {
        vert_src = HLSL_VERT;
        frag_src = HLSL_FRAG;
    } else if (is_vulkan) {
        vert_spv = read_binary_file("shaders/triangle.vert.spv", &vert_size);
        frag_spv = read_binary_file("shaders/triangle.frag.spv", &frag_size);
        if (!vert_spv || !frag_spv) {
            fprintf(stderr, "Vulkan: failed to load SPIR-V — see top of main.c\n");
            free(vert_spv); free(frag_spv);
            renderer_destroy(app->renderer);
            app->renderer = NULL;
            return -1;
        }
        vert_src = (const char*)vert_spv;
        frag_src = (const char*)frag_spv;
    } else {
        vert_src = GLSL_VERT;
        frag_src = GLSL_FRAG;
    }

    app->vbo = renderer_buffer_create(app->renderer, &(renderer_buffer_desc){
        .type  = RENDERER_BUFFER_VERTEX,
        .usage = RENDERER_BUFFER_USAGE_STATIC,
        .data  = k_vertices,
        .size  = sizeof(k_vertices),
    });

    renderer_render_pass_t* swapchain_rp = renderer_get_swapchain_render_pass(app->renderer);

    app->pipeline = renderer_pipeline_create(app->renderer, &(renderer_pipeline_desc){
        .vert_src        = vert_src,
        .frag_src        = frag_src,
        .vert_spirv_size = (uint32_t)vert_size,
        .frag_spirv_size = (uint32_t)frag_size,

        .attribs = {
            { .location = 0, .offset =  0, .format = RENDERER_ATTRIB_FLOAT3 },
            { .location = 1, .offset = 12, .format = RENDERER_ATTRIB_FLOAT4 },
        },
        .attrib_count  = 2,
        .vertex_stride = 28,
        .primitive     = RENDERER_PRIMITIVE_TRIANGLES,

        .cull_mode  = RENDERER_CULL_NONE,
        .front_face = RENDERER_FRONT_FACE_CCW,
        .fill_mode  = RENDERER_FILL_SOLID,

        .sample_count = 4,

        .depth_test_enable  = 0,
        .depth_write_enable = 0,
        .depth_compare      = RENDERER_COMPARE_ALWAYS,

        .blend_enable     = 0,
        .blend_src_color  = RENDERER_BLEND_FACTOR_ONE,
        .blend_dst_color  = RENDERER_BLEND_FACTOR_ZERO,
        .blend_op_color   = RENDERER_BLEND_OP_ADD,
        .blend_src_alpha  = RENDERER_BLEND_FACTOR_ONE,
        .blend_dst_alpha  = RENDERER_BLEND_FACTOR_ZERO,
        .blend_op_alpha   = RENDERER_BLEND_OP_ADD,
        .color_write_mask = RENDERER_COLOR_WRITE_ALL,

        .render_pass = swapchain_rp,
    });

    free(vert_spv);
    free(frag_spv);

    if (!app->pipeline) {
        fprintf(stderr, "pipeline creation failed for %s\n",
                k_backend_names[backend_to_cycle_index(new_backend)]);
        renderer_buffer_destroy(app->renderer, app->vbo);
        renderer_destroy(app->renderer);
        app->renderer = NULL;
        app->vbo      = NULL;
        return -1;
    }

    printf("Backend: %s\n", k_backend_names[backend_to_cycle_index(new_backend)]);
    return 0;
}

static void on_event(window_t* win, const window_event* e, void* userdata) {
    app_t* app = (app_t*)userdata;
    if (e->type == WINDOW_EVENT_CLOSE)
        window_request_close(win);
    if (e->type == WINDOW_EVENT_RESIZE) {
        app->width  = e->resize.width;
        app->height = e->resize.height;
        if (app->renderer)
            renderer_resize(app->renderer, e->resize.width, e->resize.height);
    }
}

int main(void) {
    window_t* win = window_create(&(window_create_desc){
        .width   = 1280,
        .height  = 720,
        .title   = "renderer test",
        .backend = WINDOW_BACKEND_GLFW,
    });
    if (!win) fatal_error("window_create failed");

    app_t app = {
        .win     = win,
        .backend = RENDERER_BACKEND_VULKAN,
        .width   = 1280,
        .height  = 720,
    };

    if (app_switch_backend(&app, app.backend) != 0)
        fatal_error("initial renderer creation failed");

    window_set_event_callback(win, on_event, &app);

    double last_time  = window_get_time(win);
    int    frame_count = 0;

    while (!window_should_close(win)) {
        double current_time = window_get_time(win);
        frame_count++;
        if (current_time - last_time >= 1.0) {
            printf("FPS: %d\n", frame_count);
            frame_count = 0;
            last_time = current_time;
        }

        window_poll_events(win);

        if (window_key_pressed(win, KEY_ESCAPE))
            window_request_close(win);

        if (window_key_pressed(win, KEY_R)) {
            int next = (backend_to_cycle_index(app.backend) + 1) % BACKEND_COUNT;
            /* Non-fatal: if the switch fails, stay on the previous backend */
            if (app_switch_backend(&app, k_backend_cycle[next]) != 0) {
                fprintf(stderr, "backend switch failed, retrying previous\n");
                app_switch_backend(&app, app.backend);
            }
        }

        if (!app.renderer || !app.pipeline)
            continue;

        int w, h;
        renderer_get_size(app.renderer, &w, &h);

        renderer_render_pass_t* swapchain_rp = renderer_get_swapchain_render_pass(app.renderer);

        renderer_begin_frame(app.renderer);
        renderer_cmd_t* cmd = renderer_cmd_begin(app.renderer);

        const renderer_clear_value clear = {
            .r = 0.1f, .g = 0.1f, .b = 0.1f, .a = 1.0f,
            .depth = 1.0f, .stencil = 0,
        };
        renderer_cmd_begin_render_pass(cmd, swapchain_rp, NULL, NULL, &clear);
        renderer_cmd_set_viewport(cmd, 0.f, 0.f, (float)w, (float)h, 0.f, 1.f);
        renderer_cmd_set_scissor(cmd, 0, 0, w, h);
        renderer_cmd_bind_pipeline(cmd, app.pipeline);
        renderer_cmd_bind_vertex_buffer(cmd, app.vbo, 0, 0);
        renderer_cmd_draw(cmd, 3, 1, 0, 0);
        renderer_cmd_end_render_pass(cmd);
        renderer_cmd_submit(app.renderer, cmd);

        renderer_end_frame(app.renderer);
        renderer_present(app.renderer);
    }

    renderer_pipeline_destroy(app.renderer, app.pipeline);
    renderer_buffer_destroy(app.renderer, app.vbo);
    renderer_destroy(app.renderer);
    window_terminate(win);
    return 0;
}