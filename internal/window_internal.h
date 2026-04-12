#ifndef WINDOW_INTERNAL_H
#define WINDOW_INTERNAL_H

#include "window.h"

typedef struct {
    int   (*init)               (window_t*, const window_create_desc*);
    void  (*shutdown)           (window_t*);
    void  (*poll_events)        (window_t*);
    int   (*should_close)       (window_t*);
    void  (*request_close)      (window_t*);
    int   (*key_down)           (window_t*, window_key);
    int   (*mouse_button_down)  (window_t*, window_mouse_button);
    void  (*mouse_pos)          (window_t*, double* x, double* y);
    void* (*get_native_handle)  (window_t*);
    void* (*get_native_display) (window_t*);
    double (*get_time)          (window_t*);
} window_backend_vtable;

typedef struct {
    uint8_t keys_down    [KEY_COUNT];
    uint8_t keys_prev    [KEY_COUNT];
    uint8_t buttons_down [MOUSE_BUTTON_COUNT];
    double  mouse_x,  mouse_y;
    double  mouse_dx, mouse_dy;
    double  scroll_dx, scroll_dy;
} window_input_state;

struct window_t {
    const window_backend_vtable* vtable;
    void*                        backend_data;
    window_input_state           input;
    window_input_state           input_prev;
    window_event_fn              event_callback;
    void*                        event_userdata;
};

void window_dispatch_event(window_t* window, const window_event* event);
const window_backend_vtable* window_backend_glfw_vtable(void);

#endif