// Win32 backend for the Alya GUI toolkit (Faz 1: windowing foundation).
//
// Plain C89-ish Win32: no C++ runtime, no external dependencies beyond the
// OS DLLs (user32, gdi32). Message pump is non-blocking (PeekMessage) so it
// integrates with Alya's cooperative event loop.

#include "win32_window.h"

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#define ALYA_GUI_MAX_EVENTS 64

struct alya_gui_window {
    HWND hwnd;
    int32_t open;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t head;
    int32_t tail;
    alya_gui_event_t queue[ALYA_GUI_MAX_EVENTS];
};

static const wchar_t *alya_gui_class_name(void) {
    return L"AlyaGuiWindow";
}

static void alya_gui_push_event(alya_gui_window_t *win, int32_t kind) {
    int32_t next = (win->tail + 1) % ALYA_GUI_MAX_EVENTS;
    if (next == win->head) {
        return; // queue full: drop oldest policy would go here; drop newest
    }
    win->queue[win->tail].kind = kind;
    win->queue[win->tail].width = win->width;
    win->queue[win->tail].height = win->height;
    win->queue[win->tail].mouse_x = win->mouse_x;
    win->queue[win->tail].mouse_y = win->mouse_y;
    win->tail = next;
}

static LRESULT CALLBACK alya_gui_wndproc(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam) {
    alya_gui_window_t *win = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        win = (alya_gui_window_t *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)win);
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    win = (alya_gui_window_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (win == NULL) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    switch (msg) {
    case WM_CLOSE:
        win->open = 0;
        alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE);
        return 0;
    case WM_SIZE: {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            win->width = (int32_t)(rc.right - rc.left);
            win->height = (int32_t)(rc.bottom - rc.top);
            alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        alya_gui_push_event(win, ALYA_GUI_EVENT_PAINT);
        return 0;
    }
    case WM_MOUSEMOVE:
        win->mouse_x = (int32_t)(int16_t)LOWORD(lparam);
        win->mouse_y = (int32_t)(int16_t)HIWORD(lparam);
        return 0;
    case WM_DESTROY:
        win->open = 0;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static int alya_gui_class_registered = 0;

static int alya_gui_ensure_class(void) {
    if (alya_gui_class_registered) {
        return 1;
    }
    WNDCLASSW cls;
    memset(&cls, 0, sizeof(cls));
    cls.lpfnWndProc = alya_gui_wndproc;
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = alya_gui_class_name();
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) {
        return 0;
    }
    alya_gui_class_registered = 1;
    return 1;
}

static wchar_t *alya_gui_utf8_to_wide(const char *utf8) {
    int needed;
    wchar_t *out;
    if (utf8 == NULL) {
        utf8 = "";
    }
    needed = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    out = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (out == NULL) {
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, needed);
    return out;
}

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height) {
    alya_gui_window_t *win;
    wchar_t *wtitle;
    HWND hwnd;
    RECT rc;

    if (width <= 0 || height <= 0) {
        return NULL;
    }
    if (!alya_gui_ensure_class()) {
        return NULL;
    }
    win = (alya_gui_window_t *)calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }
    wtitle = alya_gui_utf8_to_wide(title);

    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd = CreateWindowExW(
        0, alya_gui_class_name(), wtitle ? wtitle : L"",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top, NULL, NULL,
        GetModuleHandleW(NULL), win);
    free(wtitle);
    if (hwnd == NULL) {
        free(win);
        return NULL;
    }
    win->hwnd = hwnd;
    win->open = 1;
    if (GetClientRect(hwnd, &rc)) {
        win->width = (int32_t)(rc.right - rc.left);
        win->height = (int32_t)(rc.bottom - rc.top);
    } else {
        win->width = width;
        win->height = height;
    }
    return win;
}

void alya_gui_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->hwnd != NULL) {
        DestroyWindow(win->hwnd);
    }
    free(win);
}

void alya_gui_window_show(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    ShowWindow(win->hwnd, SW_SHOW);
    UpdateWindow(win->hwnd);
}

void alya_gui_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    ShowWindow(win->hwnd, SW_HIDE);
}

int32_t alya_gui_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    return win->open;
}

int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    MSG msg;
    if (win == NULL || out == NULL) {
        return 0;
    }
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (win->head == win->tail) {
        return 0;
    }
    *out = win->queue[win->head];
    win->head = (win->head + 1) % ALYA_GUI_MAX_EVENTS;
    return 1;
}

void alya_gui_window_set_title(alya_gui_window_t *win, const char *title) {
    wchar_t *wtitle;
    if (win == NULL) {
        return;
    }
    wtitle = alya_gui_utf8_to_wide(title);
    if (wtitle != NULL) {
        SetWindowTextW(win->hwnd, wtitle);
        free(wtitle);
    }
}

void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    RECT rc;
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    rc.left = 0;
    rc.top = 0;
    rc.right = width;
    rc.bottom = height;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(win->hwnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void alya_gui_window_close(alya_gui_window_t *win) {
    if (win == NULL || win->hwnd == NULL) {
        return;
    }
    PostMessageW(win->hwnd, WM_CLOSE, 0, 0);
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
