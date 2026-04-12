#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

typedef struct window_t window_t;

typedef enum {
    WINDOW_BACKEND_GLFW,
} window_backend;

typedef enum {
    KEY_UNKNOWN = 0,

    KEY_SPACE, KEY_APOSTROPHE, KEY_COMMA, KEY_MINUS, KEY_PERIOD, KEY_SLASH,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
    KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_SEMICOLON, KEY_EQUAL,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I,
    KEY_J, KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R,
    KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_LEFT_BRACKET, KEY_BACKSLASH, KEY_RIGHT_BRACKET, KEY_GRAVE,

    KEY_ESCAPE, KEY_ENTER, KEY_TAB, KEY_BACKSPACE, KEY_INSERT, KEY_DELETE,
    KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    KEY_PAGE_UP, KEY_PAGE_DOWN, KEY_HOME, KEY_END,
    KEY_CAPS_LOCK, KEY_SCROLL_LOCK, KEY_NUM_LOCK,
    KEY_PRINT_SCREEN, KEY_PAUSE,
    KEY_F1,  KEY_F2,  KEY_F3,  KEY_F4,  KEY_F5,  KEY_F6,
    KEY_F7,  KEY_F8,  KEY_F9,  KEY_F10, KEY_F11, KEY_F12,

    KEY_LEFT_SHIFT, KEY_LEFT_CONTROL, KEY_LEFT_ALT, KEY_LEFT_SUPER,
    KEY_RIGHT_SHIFT, KEY_RIGHT_CONTROL, KEY_RIGHT_ALT, KEY_RIGHT_SUPER,

    KEY_NP_0, KEY_NP_1, KEY_NP_2, KEY_NP_3, KEY_NP_4,
    KEY_NP_5, KEY_NP_6, KEY_NP_7, KEY_NP_8, KEY_NP_9,
    KEY_NP_DECIMAL, KEY_NP_DIVIDE, KEY_NP_MULTIPLY,
    KEY_NP_SUBTRACT, KEY_NP_ADD, KEY_NP_ENTER,

    KEY_COUNT
} window_key;

typedef enum {
    MOD_SHIFT   = 1 << 0,
    MOD_CONTROL = 1 << 1,
    MOD_ALT     = 1 << 2,
    MOD_SUPER   = 1 << 3,
    MOD_CAPS    = 1 << 4,
    MOD_NUM     = 1 << 5,
} window_mod_flags;

typedef enum {
    MOUSE_BUTTON_LEFT,
    MOUSE_BUTTON_RIGHT,
    MOUSE_BUTTON_MIDDLE,
    MOUSE_BUTTON_4,
    MOUSE_BUTTON_5,
    MOUSE_BUTTON_COUNT
} window_mouse_button;

typedef enum {
    WINDOW_EVENT_NONE = 0,
    WINDOW_EVENT_CLOSE,
    WINDOW_EVENT_RESIZE,
    WINDOW_EVENT_FOCUS_GAINED,
    WINDOW_EVENT_FOCUS_LOST,
    WINDOW_EVENT_MOVED,
    WINDOW_EVENT_KEY_PRESS,
    WINDOW_EVENT_KEY_RELEASE,
    WINDOW_EVENT_KEY_REPEAT,
    WINDOW_EVENT_CHAR,
    WINDOW_EVENT_MOUSE_MOVE,
    WINDOW_EVENT_MOUSE_PRESS,
    WINDOW_EVENT_MOUSE_RELEASE,
    WINDOW_EVENT_MOUSE_SCROLL,
    WINDOW_EVENT_MOUSE_ENTER,
    WINDOW_EVENT_MOUSE_LEAVE,
} window_event_type;

typedef struct {
    window_event_type type;
    union {
        struct { int width, height; } resize;
        struct { int x, y; } moved;
        struct {
            window_key       key;
            int              scancode;
            window_mod_flags mods;
        } key;
        struct { uint32_t codepoint; } character;
        struct { double x, y; double dx, dy; } mouse_move;
        struct {
            window_mouse_button button;
            double              x, y;
            window_mod_flags    mods;
        } mouse_button;
        struct { double dx, dy; } scroll;
    };
} window_event;

typedef void (*window_event_fn)(window_t* window,
                                const window_event* event,
                                void* userdata);

typedef struct {
    int            width;
    int            height;
    const char*    title;
    window_backend backend;
} window_create_desc;

window_t* window_create            (const window_create_desc* desc);
void      window_terminate         (window_t* window);
int       window_should_close      (window_t* window);
void      window_request_close     (window_t* window);
void      window_poll_events       (window_t* window);
void      window_set_event_callback(window_t* window,
                                    window_event_fn callback,
                                    void* userdata);
int       window_key_down          (window_t* window, window_key key);
int       window_key_pressed       (window_t* window, window_key key);
int       window_key_released      (window_t* window, window_key key);
int       window_mouse_button_down (window_t* window, window_mouse_button btn);
void      window_mouse_pos         (window_t* window, double* x, double* y);
void      window_mouse_delta       (window_t* window, double* dx, double* dy);
void      window_mouse_scroll      (window_t* window, double* dx, double* dy);
void*     window_get_native_handle (window_t* window);
void*     window_get_native_display (window_t* window);
double    window_get_time          (window_t* window);

#endif