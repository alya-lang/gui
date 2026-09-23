#ifndef ALYA_GUI_LINUX_H
#define ALYA_GUI_LINUX_H

// Linux backend contract: identical API to c/win32_window.h so one Alya
// FFI block serves every platform. Wayland is tried first; X11 (Xlib) is
// the runtime fallback when no compositor is reachable. Both live in
// c/linux_window.c so each public symbol is defined exactly once.

#include <stdint.h>

typedef struct alya_gui_window alya_gui_window_t;

#define ALYA_GUI_EVENT_NONE 0
#define ALYA_GUI_EVENT_CLOSE 1
#define ALYA_GUI_EVENT_RESIZE 2
#define ALYA_GUI_EVENT_MOUSE_MOVE 3
#define ALYA_GUI_EVENT_MOUSE_DOWN 4
#define ALYA_GUI_EVENT_MOUSE_UP 5
#define ALYA_GUI_EVENT_KEY_DOWN 6
#define ALYA_GUI_EVENT_KEY_UP 7
#define ALYA_GUI_EVENT_TEXT_INPUT 8
#define ALYA_GUI_EVENT_FOCUS 9
#define ALYA_GUI_EVENT_REDRAW 10
#define ALYA_GUI_EVENT_IME_START 11
#define ALYA_GUI_EVENT_IME_UPDATE 12
#define ALYA_GUI_EVENT_IME_END 13

typedef struct alya_gui_event {
    int32_t kind;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t key;
} alya_gui_event_t;

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height);
void alya_gui_window_destroy(alya_gui_window_t *win);
void alya_gui_window_show(alya_gui_window_t *win);
void alya_gui_window_hide(alya_gui_window_t *win);
int32_t alya_gui_window_is_open(alya_gui_window_t *win);
int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out);
void alya_gui_window_set_title(alya_gui_window_t *win, const char *title);
void alya_gui_window_size(alya_gui_window_t *win, int32_t *w, int32_t *h);
void alya_gui_window_close(alya_gui_window_t *win);
int32_t alya_gui_window_poll_event(alya_gui_window_t *win);
int32_t alya_gui_event_width(void);
int32_t alya_gui_event_height(void);
int32_t alya_gui_event_mouse_x(void);
int32_t alya_gui_event_mouse_y(void);
int32_t alya_gui_event_key(void);
const char *alya_gui_event_text(void);

// Screen-reader notification (see win32_window.h for codes). Linux
// AT-SPI needs the session-bus registry + provider tree, so this is an
// honest stub returning 0 until that follow-up lands. Safe on NULL.
int32_t alya_gui_a11y_notify(alya_gui_window_t *win, int32_t code);

#endif
