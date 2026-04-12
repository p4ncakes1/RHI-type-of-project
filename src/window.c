#include "window_internal.h"
#include "fatal_error.h"
#include <stdlib.h>
#include <string.h>

void window_dispatch_event(window_t* window, const window_event* event) {
    switch (event->type) {
        case WINDOW_EVENT_KEY_PRESS:
            if (event->key.key < KEY_COUNT) window->input.keys_down[event->key.key] = 1;
            break;
        case WINDOW_EVENT_KEY_RELEASE:
            if (event->key.key < KEY_COUNT) window->input.keys_down[event->key.key] = 0;
            break;
        case WINDOW_EVENT_MOUSE_PRESS:
            if (event->mouse_button.button < MOUSE_BUTTON_COUNT) window->input.buttons_down[event->mouse_button.button] = 1;
            break;
        case WINDOW_EVENT_MOUSE_RELEASE:
            if (event->mouse_button.button < MOUSE_BUTTON_COUNT) window->input.buttons_down[event->mouse_button.button] = 0;
            break;
        case WINDOW_EVENT_MOUSE_MOVE:
            window->input.mouse_x  = event->mouse_move.x;
            window->input.mouse_y  = event->mouse_move.y;
            window->input.mouse_dx = event->mouse_move.dx;
            window->input.mouse_dy = event->mouse_move.dy;
            break;
        case WINDOW_EVENT_MOUSE_SCROLL:
            window->input.scroll_dx += event->scroll.dx;
            window->input.scroll_dy += event->scroll.dy;
            break;
        default: break;
    }
    if (window->event_callback) window->event_callback(window, event, window->event_userdata);
}

static const window_backend_vtable* select_vtable(window_backend backend) {
    switch (backend) {
        case WINDOW_BACKEND_GLFW:  return window_backend_glfw_vtable();
        default: fatal_error("window_create: unknown backend value %d.", (int)backend); return NULL;
    }
}

window_t* window_create(const window_create_desc* desc) {
    if (!desc) return NULL;
    const window_backend_vtable* vtable = select_vtable(desc->backend);
    window_t* window = calloc(1, sizeof(window_t));
    if (!window) return NULL;
    window->vtable = vtable;
    if (vtable->init(window, desc) != 0) { free(window); return NULL; }
    return window;
}

void window_terminate(window_t* window) {
    if (!window) return;
    if (window->vtable->shutdown) window->vtable->shutdown(window);
    free(window);
}

int window_should_close(window_t* window) {
    if (!window) return 1;
    return window->vtable->should_close(window);
}

void window_request_close(window_t* window) {
    if (!window) return;
    window->vtable->request_close(window);
}

void window_poll_events(window_t* window) {
    if (!window) return;
    window->input_prev = window->input;
    window->input.mouse_dx   = 0.0;
    window->input.mouse_dy   = 0.0;
    window->input.scroll_dx  = 0.0;
    window->input.scroll_dy  = 0.0;
    window->vtable->poll_events(window);
}

void window_set_event_callback(window_t* window, window_event_fn callback, void* userdata) {
    if (!window) return;
    window->event_callback = callback;
    window->event_userdata = userdata;
}

int window_key_down(window_t* window, window_key key) {
    if (!window || key >= KEY_COUNT) return 0;
    return window->input.keys_down[key];
}

int window_key_pressed(window_t* window, window_key key) {
    if (!window || key >= KEY_COUNT) return 0;
    return window->input.keys_down[key] && !window->input_prev.keys_down[key];
}

int window_key_released(window_t* window, window_key key) {
    if (!window || key >= KEY_COUNT) return 0;
    return !window->input.keys_down[key] && window->input_prev.keys_down[key];
}

int window_mouse_button_down(window_t* window, window_mouse_button btn) {
    if (!window || btn >= MOUSE_BUTTON_COUNT) return 0;
    return window->input.buttons_down[btn];
}

void window_mouse_pos(window_t* window, double* x, double* y) {
    if (!window) return;
    if (x) *x = window->input.mouse_x;
    if (y) *y = window->input.mouse_y;
}

void window_mouse_delta(window_t* window, double* dx, double* dy) {
    if (!window) return;
    if (dx) *dx = window->input.mouse_dx;
    if (dy) *dy = window->input.mouse_dy;
}

void window_mouse_scroll(window_t* window, double* dx, double* dy) {
    if (!window) return;
    if (dx) *dx = window->input.scroll_dx;
    if (dy) *dy = window->input.scroll_dy;
}

void* window_get_native_handle(window_t* window) {
    if (!window) return NULL;
    return window->vtable->get_native_handle(window);
}

void* window_get_native_display(window_t* window) {
    if (!window || !window->vtable->get_native_display) return NULL;
    return window->vtable->get_native_display(window);
}

double window_get_time(window_t* window) {
    if (!window) return 0.0;
    return window->vtable->get_time(window);
}