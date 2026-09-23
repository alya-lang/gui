// Cocoa backend for the Alya GUI toolkit (Faz 1: windowing foundation).
//
// Pure C over the Objective-C runtime C API: no ObjC syntax, so this file
// compiles on any host with a C compiler. Cocoa/AppKit frameworks link on
// macOS only. Retina scaling is handled by the OS (backingScaleFactor);
// coordinates below are in backing pixels as reported by the view.

#include "cocoa_window.h"

#include <stdlib.h>
#include <string.h>

// --- Objective-C runtime C API (declared manually, no headers needed) ---

typedef struct objc_class *Class;
typedef struct objc_object *id;
typedef const struct objc_selector *SEL;
typedef signed char BOOL;

extern id objc_getClass(const char *name);
extern SEL sel_registerName(const char *name);
extern Class objc_allocateClassPair(Class superclass, const char *name,
                                    size_t extraBytes);
extern void objc_registerClassPair(Class cls);
extern id objc_msgSend(id self, SEL op, ...);
extern double objc_msgSend_fpret(id self, SEL op, ...);
#if defined(__x86_64__)
extern void objc_msgSend_stret(void *st, id self, SEL op, ...);
#endif

// NSRect passed BY VALUE (no stret needed for arguments).
typedef struct NSRect {
    double x;
    double y;
    double w;
    double h;
} NSRect;

typedef struct NSPoint {
    double x;
    double y;
} NSPoint;

typedef struct NSSize {
    double w;
    double h;
} NSSize;

// NSWindowStyleMask (subset).
#define NSWindowStyleMaskTitled 1
#define NSWindowStyleMaskClosable 2
#define NSWindowStyleMaskMiniaturizable 4
#define NSWindowStyleMaskResizable 8
// NSBackingStoreType.
#define NSBackingStoreBuffered 2
// NSApplicationActivationPolicy.
#define NSApplicationActivationPolicyRegular 0
// NSEventMask (subset).
#define NSAnyEventMask 0xFFFFFFFFFFFFFFFFULL

#define ALYA_GUI_MAX_EVENTS 64

struct alya_gui_window {
    id app;
    id window;
    id view;
    id delegate;
    int32_t open;
    int32_t shown;
    int32_t hidden_by_api;
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

static SEL alya_sel(const char *name) {
    return sel_registerName(name);
}

static id alya_str(const char *utf8) {
    Class cls = objc_getClass("NSString");
    SEL sel = alya_sel("stringWithUTF8String:");
    if (utf8 == NULL) {
        utf8 = "";
    }
    return ((id(*)(id, SEL, const char *))objc_msgSend)(
        (id)cls, sel, utf8);
}

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height) {
    alya_gui_window_t *win;
    Class appCls, winCls, viewCls, delCls;
    SEL sel;
    id app, window, view, delegate;
    NSRect frame;

    if (width <= 0 || height <= 0) {
        return NULL;
    }
    win = (alya_gui_window_t *)calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }

    // NSApplication shared instance + regular app policy.
    appCls = objc_getClass("NSApplication");
    sel = alya_sel("sharedApplication");
    app = ((id(*)(id, SEL))objc_msgSend)((id)appCls, sel);
    sel = alya_sel("setActivationPolicy:");
    ((void(*)(id, SEL, long))objc_msgSend)(
        app, sel, (long)NSApplicationActivationPolicyRegular);

    // NSWindow alloc/init with content rect.
    winCls = objc_getClass("NSWindow");
    sel = alya_sel("alloc");
    window = ((id(*)(id, SEL))objc_msgSend)((id)winCls, sel);
    frame.x = 100.0;
    frame.y = 100.0;
    frame.w = (double)width;
    frame.h = (double)height;
    sel = alya_sel("initWithContentRect:styleMask:backing:defer:");
    window = ((id(*)(id, SEL, NSRect, unsigned long, unsigned long, BOOL))objc_msgSend)(
        window, sel, frame,
        (unsigned long)(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable),
        (unsigned long)NSBackingStoreBuffered, (BOOL)0);
    if (window == NULL) {
        free(win);
        return NULL;
    }

    // Keep the NSWindow object alive after close so destroy() stays safe.
    sel = alya_sel("setReleasedWhenClosed:");
    ((void(*)(id, SEL, BOOL))objc_msgSend)(window, sel, (BOOL)0);

    // Title.
    sel = alya_sel("setTitle:");
    ((void(*)(id, SEL, id))objc_msgSend)(window, sel, alya_str(title));

    // Content view with layer backing + mouse tracking.
    sel = alya_sel("contentView");
    view = ((id(*)(id, SEL))objc_msgSend)(window, sel);
    sel = alya_sel("setWantsLayer:");
    ((void(*)(id, SEL, BOOL))objc_msgSend)(view, sel, (BOOL)1);
    viewCls = objc_getClass("NSView");
    (void)viewCls;

    // Delegate: tiny NSObject subclass reserved for future input hooks.
    // Close observation intentionally uses isVisible polling (see poll())
    // so no runtime method injection is required.
    delCls = objc_allocateClassPair(objc_getClass("NSObject"),
                                    "AlyaGuiWindowDelegate", 0);
    if (delCls != NULL) {
        objc_registerClassPair(delCls);
        sel = alya_sel("alloc");
        delegate = ((id(*)(id, SEL))objc_msgSend)((id)delCls, sel);
        sel = alya_sel("init");
        delegate = ((id(*)(id, SEL))objc_msgSend)(delegate, sel);
        sel = alya_sel("setDelegate:");
        ((void(*)(id, SEL, id))objc_msgSend)(window, sel, delegate);
        win->delegate = delegate;
    } else {
        win->delegate = NULL;
    }

    win->app = app;
    win->window = window;
    win->view = view;
    win->open = 1;
    win->width = width;
    win->height = height;
    return win;
}

void alya_gui_window_destroy(alya_gui_window_t *win) {
    SEL sel;
    if (win == NULL) {
        return;
    }
    if (win->window != NULL) {
        sel = alya_sel("close");
        ((void(*)(id, SEL))objc_msgSend)(win->window, sel);
    }
    free(win);
}

void alya_gui_window_show(alya_gui_window_t *win) {
    SEL sel;
    if (win == NULL) {
        return;
    }
    sel = alya_sel("makeKeyAndOrderFront:");
    ((void(*)(id, SEL, id))objc_msgSend)(win->window, sel, NULL);
    sel = alya_sel("activateIgnoringOtherApps:");
    ((void(*)(id, SEL, BOOL))objc_msgSend)(win->app, sel, (BOOL)1);
    win->shown = 1;
    win->hidden_by_api = 0;
}

void alya_gui_window_hide(alya_gui_window_t *win) {
    SEL sel;
    if (win == NULL) {
        return;
    }
    sel = alya_sel("orderOut:");
    ((void(*)(id, SEL, id))objc_msgSend)(win->window, sel, NULL);
    win->hidden_by_api = 1;
}

int32_t alya_gui_window_is_open(alya_gui_window_t *win) {
    SEL sel;
    if (win == NULL) {
        return 0;
    }
    if (win->open == 0) {
        return 0;
    }
    if (win->shown && !win->hidden_by_api) {
        // A shown, non-hidden window that stopped being visible was closed
        // by the user (red button) or performClose:.
        sel = alya_sel("isVisible");
        if (!((BOOL(*)(id, SEL))objc_msgSend)(win->window, sel)) {
            return 0;
        }
    }
    return 1;
}

int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    SEL sel;
    id pool, event, winObj;
    unsigned long long mask;
    if (win == NULL || out == NULL) {
        return 0;
    }
    // Drain NSApp event queue without blocking.
    sel = alya_sel("autoreleasepool");
    (void)sel;
    pool = NULL;
    {
        Class poolCls = objc_getClass("NSAutoreleasePool");
        SEL allocSel = alya_sel("alloc");
        SEL initSel = alya_sel("init");
        if (poolCls != NULL) {
            pool = ((id(*)(id, SEL))objc_msgSend)((id)poolCls, allocSel);
            if (pool != NULL) {
                pool = ((id(*)(id, SEL))objc_msgSend)(pool, initSel);
            }
        }
    }
    mask = NSAnyEventMask;
    for (;;) {
        id ev;
        sel = alya_sel("nextEventMatchingMask:untilDate:inMode:dequeue:");
        ev = ((id(*)(id, SEL, unsigned long long, id, id))objc_msgSend)(
            win->app, sel, mask, NULL,
            alya_str("kCFRunLoopDefaultMode"));
        if (ev == NULL) {
            break;
        }
        sel = alya_sel("sendEvent:");
        ((void(*)(id, SEL, id))objc_msgSend)(win->app, sel, ev);
        sel = alya_sel("type");
        {
            unsigned long t =
                ((unsigned long(*)(id, SEL))objc_msgSend)(ev, sel);
            // NSEventTypeLeftMouseDown = 1, LeftMouseUp = 2, MouseMoved = 5,
            // KeyDown = 10, KeyUp = 11. Track mouse via windowConvertPoint.
            if (t == 1 || t == 2 || t == 5) {
                sel = alya_sel("window");
                winObj = ((id(*)(id, SEL))objc_msgSend)(ev, sel);
                if (winObj == win->window) {
                    NSPoint pt;
                    pt.x = 0.0;
                    pt.y = 0.0;
                    sel = alya_sel("locationInWindow");
#if defined(__x86_64__)
                    objc_msgSend_stret(&pt, ev, sel);
#else
                    // arm64 (and others): struct returns use plain msgSend.
                    *(NSPoint volatile *)&pt =
                        ((NSPoint(*)(id, SEL))objc_msgSend)(ev, sel);
#endif
                    win->mouse_x = (int32_t)pt.x;
                    win->mouse_y = (int32_t)pt.y;
                }
            }
        }
        sel = alya_sel("updateWindows");
        ((void(*)(id, SEL))objc_msgSend)(win->app, sel);
    }
    if (pool != NULL) {
        sel = alya_sel("drain");
        ((void(*)(id, SEL))objc_msgSend)(pool, sel);
    }
    // Observe close: a shown, non-hidden window that stopped being
    // visible was closed by the user or performClose:.
    {
        SEL visSel = alya_sel("isVisible");
        BOOL vis = ((BOOL(*)(id, SEL))objc_msgSend)(win->window, visSel);
        if (!vis && win->open == 1 && win->shown && !win->hidden_by_api) {
            win->open = 0;
            alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE);
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
    SEL sel;
    if (win == NULL) {
        return;
    }
    sel = alya_sel("setTitle:");
    ((void(*)(id, SEL, id))objc_msgSend)(win->window, sel, alya_str(title));
}

void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    SEL sel;
    NSSize size;
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    size.w = (double)width;
    size.h = (double)height;
    sel = alya_sel("setContentSize:");
    ((void(*)(id, SEL, NSSize))objc_msgSend)(win->window, sel, size);
    win->width = width;
    win->height = height;
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
    SEL sel;
    if (win == NULL || win->window == NULL) {
        return;
    }
    sel = alya_sel("performClose:");
    ((void(*)(id, SEL, id))objc_msgSend)(win->window, sel, NULL);
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
