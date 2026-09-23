#ifndef ALYA_GUI_WIN32_H
#define ALYA_GUI_WIN32_H

#include <stdint.h>

// Opaque native window handle (HWND on Windows).
typedef struct alya_gui_window alya_gui_window_t;

// Window/input event kinds pumped by alya_gui_window_poll.
// Values mirror the Alya `GuiEventKind` enum one-to-one so the FFI layer
// forwards kinds without translation (see `native_window_drain`).
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

// Single pumped event: kind + client size + mouse position + key code.
// `key` carries the platform key code for KEY_DOWN/KEY_UP (Win32 VK,
// Cocoa keyCode, X11 KeySym, Wayland evdev) and the focus flag (1/0)
// for FOCUS. Printable text arrives via TEXT_INPUT; the UTF-8 payload
// lives in a static stash, see `alya_gui_event_text`.
typedef struct alya_gui_event {
    int32_t kind;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t key;
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

// Returns the HWND as an opaque pointer (NULL on NULL). Lets the
// Direct2D surface backend (`c/d2d_surface.c`) bind its render target
// without exposing Win32 types in the shared contract.
void *alya_gui_window_native_handle(alya_gui_window_t *win);

// Fires one screen-reader notification for the bound window:
// 1 = focus, 2 = value, 3 = selection, 4 = state (WinEvents on
// Windows, NSAccessibility on macOS, unsupported stub on Linux).
// Returns 1 when delivered, 0 otherwise. Safe on NULL.
int32_t alya_gui_a11y_notify(alya_gui_window_t *win, int32_t code);

// Polls once and stashes the event for the scalar readers below.
// Returns the event kind (0 when the queue is empty).
int32_t alya_gui_window_poll_event(alya_gui_window_t *win);

// Scalar readers for the stashed poll event (0 when none stashed yet).
int32_t alya_gui_event_width(void);
int32_t alya_gui_event_height(void);
int32_t alya_gui_event_mouse_x(void);
int32_t alya_gui_event_mouse_y(void);
int32_t alya_gui_event_key(void);
// UTF-8 text of the stashed TEXT_INPUT event ("" when none). The pointer
// stays valid until the next poll; copy it before pumping again.
const char *alya_gui_event_text(void);

#endif
