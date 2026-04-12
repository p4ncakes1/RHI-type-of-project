#include "window_internal.h"
#include <stdlib.h>
#include <stdio.h>
#include <GLFW/glfw3.h>

#if defined(_WIN32)
  #define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
  #define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(__linux__)
  #define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

typedef struct { GLFWwindow* handle; } glfw_data;

static window_key s_key_table[GLFW_KEY_LAST + 1];
static int        s_key_table_ready = 0;

static void build_key_table(void) {
    if (s_key_table_ready) return;
    for (int i = 0; i <= GLFW_KEY_LAST; i++) s_key_table[i] = KEY_UNKNOWN;
    for (int i = GLFW_KEY_A; i <= GLFW_KEY_Z; i++) s_key_table[i] = (window_key)(KEY_A + (i - GLFW_KEY_A));
    for (int i = GLFW_KEY_0; i <= GLFW_KEY_9; i++) s_key_table[i] = (window_key)(KEY_0 + (i - GLFW_KEY_0));
    for (int i = GLFW_KEY_F1; i <= GLFW_KEY_F12; i++) s_key_table[i] = (window_key)(KEY_F1 + (i - GLFW_KEY_F1));
    for (int i = GLFW_KEY_KP_0; i <= GLFW_KEY_KP_9; i++) s_key_table[i] = (window_key)(KEY_NP_0 + (i - GLFW_KEY_KP_0));
    s_key_table[GLFW_KEY_SPACE]         = KEY_SPACE;
    s_key_table[GLFW_KEY_APOSTROPHE]    = KEY_APOSTROPHE;
    s_key_table[GLFW_KEY_COMMA]         = KEY_COMMA;
    s_key_table[GLFW_KEY_MINUS]         = KEY_MINUS;
    s_key_table[GLFW_KEY_PERIOD]        = KEY_PERIOD;
    s_key_table[GLFW_KEY_SLASH]         = KEY_SLASH;
    s_key_table[GLFW_KEY_SEMICOLON]     = KEY_SEMICOLON;
    s_key_table[GLFW_KEY_EQUAL]         = KEY_EQUAL;
    s_key_table[GLFW_KEY_LEFT_BRACKET]  = KEY_LEFT_BRACKET;
    s_key_table[GLFW_KEY_BACKSLASH]     = KEY_BACKSLASH;
    s_key_table[GLFW_KEY_RIGHT_BRACKET] = KEY_RIGHT_BRACKET;
    s_key_table[GLFW_KEY_GRAVE_ACCENT]  = KEY_GRAVE;
    s_key_table[GLFW_KEY_ESCAPE]        = KEY_ESCAPE;
    s_key_table[GLFW_KEY_ENTER]         = KEY_ENTER;
    s_key_table[GLFW_KEY_TAB]           = KEY_TAB;
    s_key_table[GLFW_KEY_BACKSPACE]     = KEY_BACKSPACE;
    s_key_table[GLFW_KEY_INSERT]        = KEY_INSERT;
    s_key_table[GLFW_KEY_DELETE]        = KEY_DELETE;
    s_key_table[GLFW_KEY_RIGHT]         = KEY_RIGHT;
    s_key_table[GLFW_KEY_LEFT]          = KEY_LEFT;
    s_key_table[GLFW_KEY_DOWN]          = KEY_DOWN;
    s_key_table[GLFW_KEY_UP]            = KEY_UP;
    s_key_table[GLFW_KEY_PAGE_UP]       = KEY_PAGE_UP;
    s_key_table[GLFW_KEY_PAGE_DOWN]     = KEY_PAGE_DOWN;
    s_key_table[GLFW_KEY_HOME]          = KEY_HOME;
    s_key_table[GLFW_KEY_END]           = KEY_END;
    s_key_table[GLFW_KEY_CAPS_LOCK]     = KEY_CAPS_LOCK;
    s_key_table[GLFW_KEY_SCROLL_LOCK]   = KEY_SCROLL_LOCK;
    s_key_table[GLFW_KEY_NUM_LOCK]      = KEY_NUM_LOCK;
    s_key_table[GLFW_KEY_PRINT_SCREEN]  = KEY_PRINT_SCREEN;
    s_key_table[GLFW_KEY_PAUSE]         = KEY_PAUSE;
    s_key_table[GLFW_KEY_LEFT_SHIFT]    = KEY_LEFT_SHIFT;
    s_key_table[GLFW_KEY_LEFT_CONTROL]  = KEY_LEFT_CONTROL;
    s_key_table[GLFW_KEY_LEFT_ALT]      = KEY_LEFT_ALT;
    s_key_table[GLFW_KEY_LEFT_SUPER]    = KEY_LEFT_SUPER;
    s_key_table[GLFW_KEY_RIGHT_SHIFT]   = KEY_RIGHT_SHIFT;
    s_key_table[GLFW_KEY_RIGHT_CONTROL] = KEY_RIGHT_CONTROL;
    s_key_table[GLFW_KEY_RIGHT_ALT]     = KEY_RIGHT_ALT;
    s_key_table[GLFW_KEY_RIGHT_SUPER]   = KEY_RIGHT_SUPER;
    s_key_table[GLFW_KEY_KP_DECIMAL]    = KEY_NP_DECIMAL;
    s_key_table[GLFW_KEY_KP_DIVIDE]     = KEY_NP_DIVIDE;
    s_key_table[GLFW_KEY_KP_MULTIPLY]   = KEY_NP_MULTIPLY;
    s_key_table[GLFW_KEY_KP_SUBTRACT]   = KEY_NP_SUBTRACT;
    s_key_table[GLFW_KEY_KP_ADD]        = KEY_NP_ADD;
    s_key_table[GLFW_KEY_KP_ENTER]      = KEY_NP_ENTER;
    s_key_table_ready = 1;
}

static window_key glfw_translate_key(int glfw_key) {
    if (glfw_key < 0 || glfw_key > GLFW_KEY_LAST) return KEY_UNKNOWN;
    return s_key_table[glfw_key];
}

static window_mod_flags glfw_translate_mods(int mods) {
    window_mod_flags out = 0;
    if (mods & GLFW_MOD_SHIFT)   out |= MOD_SHIFT;
    if (mods & GLFW_MOD_CONTROL) out |= MOD_CONTROL;
    if (mods & GLFW_MOD_ALT)     out |= MOD_ALT;
    if (mods & GLFW_MOD_SUPER)   out |= MOD_SUPER;
    if (mods & GLFW_MOD_CAPS_LOCK) out |= MOD_CAPS;
    if (mods & GLFW_MOD_NUM_LOCK)  out |= MOD_NUM;
    return out;
}

static void cb_key(GLFWwindow* h, int key, int scancode, int action, int mods) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = {0};
    if      (action == GLFW_PRESS)   e.type = WINDOW_EVENT_KEY_PRESS;
    else if (action == GLFW_RELEASE) e.type = WINDOW_EVENT_KEY_RELEASE;
    else if (action == GLFW_REPEAT)  e.type = WINDOW_EVENT_KEY_REPEAT;
    e.key.key      = glfw_translate_key(key);
    e.key.scancode = scancode;
    e.key.mods     = glfw_translate_mods(mods);
    window_dispatch_event(window, &e);
}

static void cb_char(GLFWwindow* h, unsigned int codepoint) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = WINDOW_EVENT_CHAR, .character.codepoint = codepoint };
    window_dispatch_event(window, &e);
}

static void cb_mouse_button(GLFWwindow* h, int button, int action, int mods) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = {0};
    e.type = (action == GLFW_PRESS) ? WINDOW_EVENT_MOUSE_PRESS : WINDOW_EVENT_MOUSE_RELEASE;
    e.mouse_button.button = (window_mouse_button)button;
    e.mouse_button.mods   = glfw_translate_mods(mods);
    glfwGetCursorPos(h, &e.mouse_button.x, &e.mouse_button.y);
    window_dispatch_event(window, &e);
}

static void cb_cursor_pos(GLFWwindow* h, double x, double y) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = {
        .type = WINDOW_EVENT_MOUSE_MOVE,
        .mouse_move.x = x, .mouse_move.y = y,
        .mouse_move.dx = x - window->input.mouse_x,
        .mouse_move.dy = y - window->input.mouse_y,
    };
    window_dispatch_event(window, &e);
}

static void cb_scroll(GLFWwindow* h, double dx, double dy) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = WINDOW_EVENT_MOUSE_SCROLL, .scroll.dx = dx, .scroll.dy = dy };
    window_dispatch_event(window, &e);
}

static void cb_cursor_enter(GLFWwindow* h, int entered) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = entered ? WINDOW_EVENT_MOUSE_ENTER : WINDOW_EVENT_MOUSE_LEAVE };
    window_dispatch_event(window, &e);
}

static void cb_resize(GLFWwindow* h, int w, int height) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = WINDOW_EVENT_RESIZE, .resize.width = w, .resize.height = height };
    window_dispatch_event(window, &e);
}

static void cb_close(GLFWwindow* h) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = WINDOW_EVENT_CLOSE };
    window_dispatch_event(window, &e);
}

static void cb_focus(GLFWwindow* h, int focused) {
    window_t* window = (window_t*)glfwGetWindowUserPointer(h);
    window_event e = { .type = focused ? WINDOW_EVENT_FOCUS_GAINED : WINDOW_EVENT_FOCUS_LOST };
    window_dispatch_event(window, &e);
}

static void glfw_error_callback(int code, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", code, description);
}

static int glfw_init(window_t* window, const window_create_desc* desc) {
    build_key_table();
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) { fprintf(stderr, "window_glfw: glfwInit failed\n"); return -1; }
    glfw_data* data = calloc(1, sizeof(glfw_data));
    if (!data) return -1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    data->handle = glfwCreateWindow(desc->width, desc->height, desc->title, NULL, NULL);
    if (!data->handle) { fprintf(stderr, "window_glfw: glfwCreateWindow failed\n"); free(data); return -1; }
    glfwSetWindowUserPointer(data->handle, window);
    glfwSetKeyCallback(data->handle,        cb_key);
    glfwSetCharCallback(data->handle,       cb_char);
    glfwSetMouseButtonCallback(data->handle, cb_mouse_button);
    glfwSetCursorPosCallback(data->handle,  cb_cursor_pos);
    glfwSetScrollCallback(data->handle,     cb_scroll);
    glfwSetCursorEnterCallback(data->handle, cb_cursor_enter);
    glfwSetFramebufferSizeCallback(data->handle, cb_resize);
    glfwSetWindowCloseCallback(data->handle, cb_close);
    glfwSetWindowFocusCallback(data->handle, cb_focus);
    window->backend_data = data;
    return 0;
}

static void glfw_shutdown(window_t* window) {
    glfw_data* data = window->backend_data;
    if (data) { if (data->handle) glfwDestroyWindow(data->handle); free(data); }
    glfwTerminate();
}

static void  glfw_poll_events(window_t* w)        { (void)w; glfwPollEvents(); }
static int   glfw_should_close(window_t* w)        { return glfwWindowShouldClose(((glfw_data*)w->backend_data)->handle); }
static void  glfw_request_close(window_t* w)       { glfwSetWindowShouldClose(((glfw_data*)w->backend_data)->handle, 1); }
static int   glfw_key_down(window_t* w, window_key k) { (void)w; (void)k; return 0; }
static int   glfw_mouse_down(window_t* w, window_mouse_button b) { (void)w; (void)b; return 0; }

static void* glfw_native_handle(window_t* window) {
    glfw_data* data = window->backend_data;
#if defined(_WIN32)
    return (void*)glfwGetWin32Window(data->handle);
#elif defined(__APPLE__)
    return (void*)glfwGetCocoaWindow(data->handle);
#elif defined(__linux__)
    return (void*)(uintptr_t)glfwGetX11Window(data->handle);
#else
    return NULL;
#endif
}

static void* glfw_native_display(window_t* window) {
    (void)window;
#if defined(__linux__)
    return (void*)glfwGetX11Display();
#else
    return NULL;
#endif
}

static double glfw_get_time(window_t* window) {
    (void)window;
    return glfwGetTime();
}

static const window_backend_vtable s_glfw_vtable = {
    .init               = glfw_init,
    .shutdown           = glfw_shutdown,
    .poll_events        = glfw_poll_events,
    .should_close       = glfw_should_close,
    .request_close      = glfw_request_close,
    .key_down           = glfw_key_down,
    .mouse_button_down  = glfw_mouse_down,
    .get_native_handle  = glfw_native_handle,
    .get_native_display = glfw_native_display,
    .get_time           = glfw_get_time,
};

const window_backend_vtable* window_backend_glfw_vtable(void) { return &s_glfw_vtable; }