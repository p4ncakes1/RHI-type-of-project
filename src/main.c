#include "window.h"
#include "renderer.h"
#include "fatal_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define SELECTED_BACKEND RENDERER_BACKEND_OPENGL

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
    "layout(std140, binding = 1) uniform UBO {\n"
    "    vec4 colorMod;\n"
    "};\n"
    "void main() {\n"
    "    out_color = v_color * texture(tex0, v_uv) * colorMod;\n"
    "}\n";

static const char* HLSL_VERT =
    "struct VSIn  { float3 pos : ATTR0; float4 col : ATTR1; float2 uv : ATTR2; };\n"
    "struct PSIn  { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD0; };\n"
    "PSIn main(VSIn i) {\n"
    "    PSIn o;\n"
    "    o.pos = float4(i.pos, 1.0f);\n"
    "    o.col = i.col;\n"
    "    o.uv = i.uv;\n"
    "    return o;\n"
    "}\n";

static const char* HLSL_FRAG =
    "struct PSIn { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD0; };\n"
    "Texture2D    tex0 : register(t0);\n"
    "SamplerState smp0 : register(s0);\n"
    "cbuffer UBO : register(b1) { float4 colorMod; };\n"
    "float4 main(PSIn i) : SV_Target { return i.col * tex0.Sample(smp0, i.uv) * colorMod; }\n";

/* xyz + rgba + uv, 9 floats per vertex, 36-byte stride */
static const float k_vertices[] = {
     0.0f,  0.5f, 0.0f,   1.0f, 0.0f, 0.0f, 1.0f,   0.5f, 0.0f,
    -0.5f, -0.5f, 0.0f,   0.0f, 1.0f, 0.0f, 1.0f,   0.0f, 1.0f,
     0.5f, -0.5f, 0.0f,   0.0f, 0.0f, 1.0f, 1.0f,   1.0f, 1.0f,
};

typedef struct {
    window_t* win;
    renderer_t* renderer;
    renderer_pipeline_t* pipeline;
    renderer_buffer_t* vbo;
    renderer_buffer_t* ubo;
    renderer_texture_t* texture;
    int                  width;
    int                  height;
} app_t;

static void* read_binary_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "read_binary_file: cannot open '%s'\n", path);
        return NULL;
    }
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

static int create_renderer(app_t* app, renderer_backend backend) {
    app->renderer = renderer_create(&(renderer_create_desc){
        .native_window_handle  = window_get_native_handle(app->win),
        .native_display_handle = window_get_native_display(app->win),
        .width        = app->width,
        .height       = app->height,
        .vsync        = 0,
        .sample_count = 4,
        .backend      = backend,
    });
    if (!app->renderer) {
        fprintf(stderr, "renderer_create failed\n");
        return -1;
    }

    uint32_t checker_pixels[64 * 64];
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            int checker = ((x / 8) + (y / 8)) % 2;
            checker_pixels[y * 64 + x] = checker ? 0xFFFFFFFF : 0xFF000000;
        }
    }
    app->texture = renderer_texture_create(app->renderer, &(renderer_texture_desc){
        .width = 64, .height = 64,
        .format = RENDERER_TEXTURE_FORMAT_RGBA8,
        .pixels = checker_pixels,
        .min_filter = RENDERER_FILTER_NEAREST,
        .mag_filter = RENDERER_FILTER_NEAREST,
        .wrap_u = RENDERER_WRAP_REPEAT,
        .wrap_v = RENDERER_WRAP_REPEAT,
    });

    const char* vert_src  = NULL;
    const char* frag_src  = NULL;
    void* vert_spv  = NULL;
    void* frag_spv  = NULL;
    size_t vert_size = 0;
    size_t frag_size = 0;

    if (backend == RENDERER_BACKEND_D3D11) {
        vert_src = HLSL_VERT;
        frag_src = HLSL_FRAG;
    } else if (backend == RENDERER_BACKEND_VULKAN) {
        vert_spv = read_binary_file("shaders/triangle_textured.vert.spv", &vert_size);
        frag_spv = read_binary_file("shaders/triangle_textured.frag.spv", &frag_size);
        if (!vert_spv || !frag_spv) {
            fprintf(stderr, "Vulkan: failed to load SPIR-V shaders! Compile the updated GLSL to SPIR-V.\n");
            free(vert_spv); free(frag_spv);
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

    float initial_ubo_data[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    app->ubo = renderer_buffer_create(app->renderer, &(renderer_buffer_desc){
        .type  = RENDERER_BUFFER_UNIFORM,
        .usage = RENDERER_BUFFER_USAGE_DYNAMIC,
        .data  = initial_ubo_data,
        .size  = sizeof(initial_ubo_data),
    });
    if (!app->ubo) {
        fprintf(stderr, "Failed to create uniform buffer\n");
        return -1;
    }

    renderer_render_pass_t* swapchain_rp = renderer_get_swapchain_render_pass(app->renderer);

    app->pipeline = renderer_pipeline_create(app->renderer, &(renderer_pipeline_desc){
        .vert_src        = vert_src,
        .frag_src        = frag_src,
        .vert_spirv_size = (uint32_t)vert_size,
        .frag_spirv_size = (uint32_t)frag_size,

        .attribs = {
            { .location = 0, .offset =  0, .format = RENDERER_ATTRIB_FLOAT3 }, /* pos */
            { .location = 1, .offset = 12, .format = RENDERER_ATTRIB_FLOAT4 }, /* col */
            { .location = 2, .offset = 28, .format = RENDERER_ATTRIB_FLOAT2 }, /* uv  */
        },
        .attrib_count  = 3,
        .vertex_stride = 36,
        .primitive     = RENDERER_PRIMITIVE_TRIANGLES,
        .cull_mode     = RENDERER_CULL_NONE,
        .front_face    = RENDERER_FRONT_FACE_CCW,
        .fill_mode     = RENDERER_FILL_SOLID,
        .sample_count  = 4,
        .color_write_mask = RENDERER_COLOR_WRITE_ALL,
        .render_pass   = swapchain_rp,
    });

    free(vert_spv);
    free(frag_spv);

    if (!app->pipeline || !app->vbo) {
        fprintf(stderr, "pipeline or VBO creation failed\n");
        return -1;
    }

    return 0;
}

static void on_event(window_t* win, const window_event* e, void* userdata) {
    app_t* app = (app_t*)userdata;
    switch (e->type) {
        case WINDOW_EVENT_CLOSE:
            window_request_close(win);
            break;
        case WINDOW_EVENT_RESIZE:
            app->width  = e->resize.width;
            app->height = e->resize.height;
            if (app->renderer)
                renderer_resize(app->renderer, e->resize.width, e->resize.height);
            break;
        default:
            break;
    }
}

int main(void) {
    window_t* win = window_create(&(window_create_desc){
        .width   = 1280,
        .height  = 720,
        .title   = "renderer test (uniform buffer example)",
        .backend = WINDOW_BACKEND_GLFW,
    });
    if (!win) fatal_error("window_create failed");

    app_t app = {
        .win     = win,
        .width   = 1280,
        .height  = 720,
    };

    window_set_event_callback(win, on_event, &app);

    if (create_renderer(&app, SELECTED_BACKEND) != 0)
        fatal_error("renderer creation failed");

    printf("Backend: %s\n", renderer_backend_to_string(renderer_get_backend(app.renderer)));

    double last_fps_time = window_get_time(win);
    int    frame_count   = 0;

    while (!window_should_close(win)) {
        window_poll_events(win);

        if (window_key_pressed(win, KEY_ESCAPE))
            window_request_close(win);

        if (!app.renderer || !app.pipeline) continue;

        double now = window_get_time(win);
        ++frame_count;
        if (now - last_fps_time >= 1.0) {
            printf("FPS: %d\n", frame_count);
            frame_count   = 0;
            last_fps_time = now;
        }

        int w, h;
        renderer_get_size(app.renderer, &w, &h);

        float ubo_data[4] = {
            (float)(sin(now) * 0.5 + 0.5),
            1.0f, 1.0f, 1.0f
        };
        renderer_buffer_update(app.renderer, app.ubo, ubo_data, sizeof(ubo_data));

        renderer_begin_frame(app.renderer);
        renderer_cmd_t* cmd = renderer_cmd_begin(app.renderer);

        const renderer_clear_value clear = {
            .r = 0.1f, .g = 0.1f, .b = 0.1f, .a = 1.0f,
            .depth = 1.0f, .stencil = 0,
        };

        renderer_render_pass_t* swapchain_rp = renderer_get_swapchain_render_pass(app.renderer);

        renderer_cmd_begin_render_pass(cmd, swapchain_rp, NULL, NULL, &clear);
        renderer_cmd_set_viewport(cmd, 0.f, 0.f, (float)w, (float)h, 0.f, 1.f);
        renderer_cmd_set_scissor(cmd, 0, 0, w, h);

        renderer_cmd_bind_pipeline(cmd, app.pipeline);
        renderer_cmd_bind_vertex_buffer(cmd, app.vbo, 0, 0);

        renderer_cmd_bind_uniform_buffer(cmd, app.ubo, 0, 0, 0);

        renderer_cmd_bind_texture(cmd, app.texture, 0);

        renderer_cmd_draw(cmd, 3, 1, 0, 0);

        renderer_cmd_end_render_pass(cmd);
        renderer_cmd_submit(app.renderer, cmd);

        renderer_end_frame(app.renderer);
        renderer_present(app.renderer);
    }

    if (app.texture) renderer_texture_destroy(app.renderer, app.texture);
    if (app.ubo)     renderer_buffer_destroy(app.renderer, app.ubo);
    renderer_pipeline_destroy(app.renderer, app.pipeline);
    renderer_buffer_destroy(app.renderer, app.vbo);
    renderer_destroy(app.renderer);
    window_terminate(win);
    return 0;
}