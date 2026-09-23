// X11 fallback backend for the Alya GUI toolkit (Faz 1: windowing).
//
// Minimal Xlib client: window create/map, non-blocking event pump, close
// protocol (WM_DELETE_WINDOW), resize/motion tracking. Used when no Wayland
// compositor is reachable. Links against libX11 (present on X11 desktops).

#include "x11_window.h"

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define ALYA_GUI_MAX_EVENTS 64

struct alya_gui_window {
    Display *display;
    Window window;
    Atom wm_delete;
    int32_t open;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t head;
    int32_t tail;
    alya_gui_event_t queue[ALYA_GUI_MAX_EVENTS];
};

static void alya_gui_push_event(alya_gui_window_t *win, int32_t kind) {
    int32_t next;
    if (win == NULL) {
        return;
    }
    next = (win->tail + 1) % ALYA_GUI_MAX_EVENTS;
    if (next == win->head) {
        return;
    }
    win->queue[win->tail].kind = kind;
    win->queue[win->tail].width = win->width;
    win->queue[win->tail].height = win->height;
    win->queue[win->tail].mouse_x = win->mouse_x;
    win->queue[win->tail].mouse_y = win->mouse_y;
    win->tail = next;
}

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height) {
    alya_gui_window_t *win;
    Display *display;
    int screen;
    Window root;
    Window window;
    XSizeHints *hints;

    if (width <= 0 || height <= 0) {
        return NULL;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        return NULL;
    }
    win = (alya_gui_window_t *)calloc(1, sizeof(*win));
    if (win == NULL) {
        XCloseDisplay(display);
        return NULL;
    }
    screen = DefaultScreen(display);
    root = RootWindow(display, screen);
    window = XCreateSimpleWindow(display, root, 100, 100, (unsigned)width,
                                 (unsigned)height, 0,
                                 BlackPixel(display, screen),
                                 WhitePixel(display, screen));
    if (window == 0) {
        XCloseDisplay(display);
        free(win);
        return NULL;
    }
    win->display = display;
    win->window = window;
    win->wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &win->wm_delete, 1);
    if (title != NULL) {
        XStoreName(display, window, title);
    }
    hints = XAllocSizeHints();
    if (hints != NULL) {
        hints->flags = PMinSize;
        hints->min_width = width > 1 ? width : 1;
        hints->min_height = height > 1 ? height : 1;
        XSetWMNormalHints(display, window, hints);
        XFree(hints);
    }
    XSelectInput(display, window,
                 ExposureMask | StructureNotifyMask | PointerMotionMask |
                     ButtonPressMask | KeyPressMask | KeyReleaseMask);
    win->open = 1;
    win->width = width;
    win->height = height;
    return win;
}

void alya_gui_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->display != NULL) {
        if (win->window != 0) {
            XDestroyWindow(win->display, win->window);
        }
        XCloseDisplay(win->display);
    }
    free(win);
}

void alya_gui_window_show(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    XMapWindow(win->display, win->window);
    XFlush(win->display);
}

void alya_gui_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    XUnmapWindow(win->display, win->window);
    XFlush(win->display);
}

int32_t alya_gui_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    return win->open;
}

int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    Display *display;
    if (win == NULL || out == NULL) {
        return 0;
    }
    display = win->display;
    while (XPending(display) > 0) {
        XEvent ev;
        XNextEvent(display, &ev);
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == win->wm_delete) {
                win->open = 0;
                alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE);
            }
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != win->width ||
                ev.xconfigure.height != win->height) {
                win->width = ev.xconfigure.width;
                win->height = ev.xconfigure.height;
                alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE);
            }
            break;
        case Expose:
            if (ev.xexpose.count == 0) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_PAINT);
            }
            break;
        case MotionNotify:
            win->mouse_x = ev.xmotion.x;
            win->mouse_y = ev.xmotion.y;
            break;
        default:
            break;
        }
    }
    if (win->head == win->tail) {
        return 0;
    }
    *out = win->queue[win->head];
    win->head = (win->head + 1) % ALYA_GUI_MAX_EVENTS;
    return 1;
}

void alya_gui_window_set_title(alya_gui_window_t *win, const char *title) {
    if (win == NULL || title == NULL) {
        return;
    }
    XStoreName(win->display, win->window, title);
    XFlush(win->display);
}

void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    XResizeWindow(win->display, win->window, (unsigned)width,
                  (unsigned)height);
    XFlush(win->display);
}

void alya_gui_window_size(alya_gui_window_t *win, int32_t *w, int32_t *h) {
    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (win == NULL) {
        return;
    }
    if (w != NULL) {
        *w = win->width;
    }
    if (h != NULL) {
        *h = win->height;
    }
}

void alya_gui_window_close(alya_gui_window_t *win) {
    XEvent ev;
    if (win == NULL) {
        return;
    }
    // Synthesize the same ClientMessage path as the window manager close.
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win->window;
    ev.xclient.message_type =
        XInternAtom(win->display, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)win->wm_delete;
    XSendEvent(win->display, win->window, False, NoEventMask, &ev);
    XFlush(win->display);
}

static alya_gui_event_t alya_gui_stashed = {0, 0, 0, 0, 0};

int32_t alya_gui_window_poll_event(alya_gui_window_t *win) {
    alya_gui_stashed.kind = 0;
    alya_gui_stashed.width = 0;
    alya_gui_stashed.height = 0;
    alya_gui_stashed.mouse_x = 0;
    alya_gui_stashed.mouse_y = 0;
    if (win == NULL) {
        return 0;
    }
    if (alya_gui_window_poll(win, &alya_gui_stashed) == 0) {
        return 0;
    }
    return alya_gui_stashed.kind;
}

int32_t alya_gui_event_width(void) {
    return alya_gui_stashed.width;
}

int32_t alya_gui_event_height(void) {
    return alya_gui_stashed.height;
}

int32_t alya_gui_event_mouse_x(void) {
    return alya_gui_stashed.mouse_x;
}

int32_t alya_gui_event_mouse_y(void) {
    return alya_gui_stashed.mouse_y;
}
