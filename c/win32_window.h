#ifndef ALYA_GUI_WIN32_H
#define ALYA_GUI_WIN32_H

#include <stdint.h>

// Opaque native window handle (HWND on Windows).
typedef struct alya_gui_window alya_gui_window_t;

// Window event kinds pumped by alya_gui_window_poll.
#define ALYA_GUI_EVENT_NONE 0
#define ALYA_GUI_EVENT_CLOSE 1
#define ALYA_GUI_EVENT_RESIZE 2
#define ALYA_GUI_EVENT_PAINT 3

// Single pumped event: kind + client size + mouse position.
typedef struct alya_gui_event {
    int32_t kind;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
} alya_gui_event_t;

// Creates a top-level window (title is UTF-8). Returns NULL on failure.
// The window starts hidden; call alya_gui_window_show to display it.
alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height);

// Destroys the window. Safe on NULL.
void alya_gui_window_destroy(alya_gui_window_t *win);

// Shows / hides the window.
void alya_gui_window_show(alya_gui_window_t *win);
void alya_gui_window_hide(alya_gui_window_t *win);

// Returns 1 while the window is open, 0 after close.
int32_t alya_gui_window_is_open(alya_gui_window_t *win);

// Pumps pending OS messages without blocking. Returns 1 when an event
// was written into `out`, 0 when the queue is empty.
int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out);

// Updates the window title (UTF-8). No-op on NULL.
void alya_gui_window_set_title(alya_gui_window_t *win, const char *title);
void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height);

// Current client size. Writes 0s on NULL.
void alya_gui_window_size(alya_gui_window_t *win, int32_t *w, int32_t *h);

// Requests a graceful close (posts WM_CLOSE like the title-bar button).
// No-op on NULL.
void alya_gui_window_close(alya_gui_window_t *win);

// Polls once and stashes the event for the scalar readers below.
// Returns the event kind (0 when the queue is empty).
int32_t alya_gui_window_poll_event(alya_gui_window_t *win);

// Scalar readers for the stashed poll event (0 when none stashed yet).
int32_t alya_gui_event_width(void);
int32_t alya_gui_event_height(void);
int32_t alya_gui_event_mouse_x(void);
int32_t alya_gui_event_mouse_y(void);

#endif
