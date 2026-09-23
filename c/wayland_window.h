#ifndef ALYA_GUI_WAYLAND_H
#define ALYA_GUI_WAYLAND_H

// Wayland backend contract: identical API to c/win32_window.h so one Alya
// FFI block serves every platform. X11 fallback lives in c/x11_window.c
// behind the same declarations; only one TU is compiled per target.

#include <stdint.h>

typedef struct alya_gui_window alya_gui_window_t;

#define ALYA_GUI_EVENT_NONE 0
#define ALYA_GUI_EVENT_CLOSE 1
#define ALYA_GUI_EVENT_RESIZE 2
#define ALYA_GUI_EVENT_PAINT 3

typedef struct alya_gui_event {
    int32_t kind;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
} alya_gui_event_t;

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height);
void alya_gui_window_destroy(alya_gui_window_t *win);
void alya_gui_window_show(alya_gui_window_t *win);
void alya_gui_window_hide(alya_gui_window_t *win);
int32_t alya_gui_window_is_open(alya_gui_window_t *win);
int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out);
void alya_gui_window_set_title(alya_gui_window_t *win, const char *title);
void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height);
void alya_gui_window_size(alya_gui_window_t *win, int32_t *w, int32_t *h);
void alya_gui_window_close(alya_gui_window_t *win);
int32_t alya_gui_window_poll_event(alya_gui_window_t *win);
int32_t alya_gui_event_width(void);
int32_t alya_gui_event_height(void);
int32_t alya_gui_event_mouse_x(void);
int32_t alya_gui_event_mouse_y(void);

#endif
