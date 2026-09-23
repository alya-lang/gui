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
#include <imm.h>
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
    int32_t ime_result;
    int32_t head;
    int32_t tail;
    alya_gui_event_t queue[ALYA_GUI_MAX_EVENTS];
};

static const wchar_t *alya_gui_class_name(void) {
    return L"AlyaGuiWindow";
}

static void alya_gui_push_event(alya_gui_window_t *win, int32_t kind,
                                int32_t key) {
    int32_t next = (win->tail + 1) % ALYA_GUI_MAX_EVENTS;
    if (next == win->head) {
        return; // queue full: drop oldest policy would go here; drop newest
    }
    // Coalesce mouse-motion floods: a trailing unconsumed MOUSE_MOVE is
    // refreshed in place so drags cannot starve clicks/keys.
    if (kind == ALYA_GUI_EVENT_MOUSE_MOVE && win->head != win->tail) {
        int32_t last = (win->tail + ALYA_GUI_MAX_EVENTS - 1) % ALYA_GUI_MAX_EVENTS;
        if (win->queue[last].kind == ALYA_GUI_EVENT_MOUSE_MOVE) {
            win->queue[last].width = win->width;
            win->queue[last].height = win->height;
            win->queue[last].mouse_x = win->mouse_x;
            win->queue[last].mouse_y = win->mouse_y;
            return;
        }
    }
    win->queue[win->tail].kind = kind;
    win->queue[win->tail].width = win->width;
    win->queue[win->tail].height = win->height;
    win->queue[win->tail].mouse_x = win->mouse_x;
    win->queue[win->tail].mouse_y = win->mouse_y;
    win->queue[win->tail].key = key;
    win->tail = next;
}

// UTF-8 stash for the latest TEXT_INPUT payload (truncated, NUL-terminated).
#define ALYA_GUI_TEXT_STASH 128
static char alya_gui_text_stash[ALYA_GUI_TEXT_STASH];

static void alya_gui_set_text(const char *utf8, size_t len) {
    size_t n;
    if (utf8 == NULL || len == 0) {
        alya_gui_text_stash[0] = '\0';
        return;
    }
    n = len < ALYA_GUI_TEXT_STASH - 1 ? len : ALYA_GUI_TEXT_STASH - 1;
    memcpy(alya_gui_text_stash, utf8, n);
    alya_gui_text_stash[n] = '\0';
}

// Copies a UTF-16 buffer into the TEXT_INPUT stash (truncated).
static int alya_gui_utf8_encode(char *out, unsigned cp);

static void alya_gui_set_wtext(const wchar_t *w, int wlen) {
    char tmp[ALYA_GUI_TEXT_STASH];
    int n = 0;
    int i = 0;
    unsigned high = 0;
    while (i < wlen && n < ALYA_GUI_TEXT_STASH - 1) {
        unsigned unit = (unsigned)w[i++];
        unsigned cp;
        char utf8[4];
        int k;
        int m;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            high = unit;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && high != 0) {
            cp = 0x10000 + ((high - 0xD800) << 10) + (unit - 0xDC00);
            high = 0;
        } else {
            high = 0;
            cp = unit;
        }
        if (cp < 0x20 || cp == 0x7F) {
            continue;
        }
        k = alya_gui_utf8_encode(utf8, cp);
        if (n + k > ALYA_GUI_TEXT_STASH - 1) {
            break;
        }
        for (m = 0; m < k; m++) {
            tmp[n++] = utf8[m];
        }
    }
    tmp[n] = '\0';
    alya_gui_set_text(tmp, (size_t)n);
}

// Reads an IMM composition/result string into the stash; returns its
// character count (0 when empty/unavailable).
static int alya_gui_imm_string(HWND hwnd, unsigned long kind) {
    HIMC himc;
    LONG n;
    wchar_t buf[64];
    LONG got;
    himc = ImmGetContext(hwnd);
    if (himc == NULL) {
        return 0;
    }
    n = ImmGetCompositionStringW(himc, kind, NULL, 0);
    if (n <= 0) {
        ImmReleaseContext(hwnd, himc);
        return 0;
    }
    if (n > (LONG)(sizeof(buf) - sizeof(wchar_t))) {
        n = (LONG)(sizeof(buf) - sizeof(wchar_t));
    }
    got = ImmGetCompositionStringW(himc, kind, buf, (unsigned long)n);
    ImmReleaseContext(hwnd, himc);
    if (got <= 0) {
        return 0;
    }
    alya_gui_set_wtext(buf, (int)(got / (LONG)sizeof(wchar_t)));
    return (int)(got / (LONG)sizeof(wchar_t));
}

// Encodes one code point as UTF-8; returns the byte count (1-4).
static int alya_gui_utf8_encode(char *out, unsigned cp) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
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
        alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE, 0);
        return 0;
    case WM_SIZE: {
        RECT rc;
        if (GetClientRect(hwnd, &rc)) {
            win->width = (int32_t)(rc.right - rc.left);
            win->height = (int32_t)(rc.bottom - rc.top);
            alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE, 0);
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        alya_gui_push_event(win, ALYA_GUI_EVENT_REDRAW, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
        win->mouse_x = (int32_t)(int16_t)LOWORD(lparam);
        win->mouse_y = (int32_t)(int16_t)HIWORD(lparam);
        alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_MOVE, 0);
        return 0;
    case WM_LBUTTONDOWN:
        win->mouse_x = (int32_t)(int16_t)LOWORD(lparam);
        win->mouse_y = (int32_t)(int16_t)HIWORD(lparam);
        SetCapture(hwnd);
        alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_DOWN, 0);
        return 0;
    case WM_LBUTTONUP:
        win->mouse_x = (int32_t)(int16_t)LOWORD(lparam);
        win->mouse_y = (int32_t)(int16_t)HIWORD(lparam);
        ReleaseCapture();
        alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_UP, 0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Repeat counts arrive as separate messages; each is an event.
        // `key` is the virtual-key code (VK_*); printable text follows
        // via WM_CHAR so layouts/IME stay correct.
        alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_DOWN,
                            (int32_t)wparam);
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_UP, (int32_t)wparam);
        return 0;
    case WM_CHAR: {
        // UTF-16 code unit (surrogate pairs arrive as two messages).
        static unsigned alya_gui_high = 0;
        unsigned unit = (unsigned)wparam;
        unsigned cp;
        char utf8[4];
        int n;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            alya_gui_high = unit;
            return 0;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF && alya_gui_high != 0) {
            cp = 0x10000 + ((alya_gui_high - 0xD800) << 10) +
                 (unit - 0xDC00);
            alya_gui_high = 0;
        } else {
            alya_gui_high = 0;
            cp = unit;
        }
        if (cp < 0x20 || cp == 0x7F) {
            return 0; // control characters are not text input
        }
        n = alya_gui_utf8_encode(utf8, cp);
        alya_gui_set_text(utf8, (size_t)n);
        alya_gui_push_event(win, ALYA_GUI_EVENT_TEXT_INPUT, (int32_t)cp);
        return 0;
    }
    case WM_SETFOCUS:
        alya_gui_push_event(win, ALYA_GUI_EVENT_FOCUS, 1);
        return 0;
    case WM_KILLFOCUS:
        alya_gui_push_event(win, ALYA_GUI_EVENT_FOCUS, 0);
        return 0;
    case WM_IME_STARTCOMPOSITION:
        alya_gui_push_event(win, ALYA_GUI_EVENT_IME_START, 0);
        win->ime_result = 0;
        break;
    case WM_IME_COMPOSITION: {
        unsigned long flags = (unsigned long)lparam;
        if (flags & 0x0800) {
            // GCS_RESULTSTR: final string commits the composition.
            if (alya_gui_imm_string(hwnd, 0x0800) > 0) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_IME_END, 0);
            }
            win->ime_result = 1;
            return 0;
        }
        if (flags & 0x0008) {
            // GCS_COMPSTR: live preview replaces the previous one.
            if (alya_gui_imm_string(hwnd, 0x0008) > 0) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_IME_UPDATE, 0);
            }
            return 0;
        }
        break;
    }
    case WM_IME_ENDCOMPOSITION:
        // A result already closed the composition via IME_END; only an
        // empty end remains to report.
        if (win->ime_result == 0) {
            alya_gui_set_text("", 0);
            alya_gui_push_event(win, ALYA_GUI_EVENT_IME_END, 0);
        }
        win->ime_result = 0;
        break;
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

void *alya_gui_window_native_handle(alya_gui_window_t *win) {
    if (win == NULL) {
        return NULL;
    }
    return (void *)win->hwnd;
}

int32_t alya_gui_a11y_notify(alya_gui_window_t *win, int32_t code) {
    unsigned event_id;
    if (win == NULL || win->hwnd == NULL) {
        return 0;
    }
    // WinEvents (MSAA/UIA bridge): 1 = focus, 2 = value, 3 = selection,
    // 4 = state; anything else announces a name change.
    switch (code) {
    case 1:
        event_id = 0x8005; // EVENT_OBJECT_FOCUS
        break;
    case 2:
        event_id = 0x800E; // EVENT_OBJECT_VALUECHANGE
        break;
    case 3:
        event_id = 0x8006; // EVENT_OBJECT_SELECTION
        break;
    case 4:
        event_id = 0x800A; // EVENT_OBJECT_STATECHANGE
        break;
    default:
        event_id = 0x800C; // EVENT_OBJECT_NAMECHANGE
        break;
    }
    NotifyWinEvent(event_id, win->hwnd, (LONG)(0xFFFFFFFC), 0);
    return 1;
}

static alya_gui_event_t alya_gui_stashed = {0, 0, 0, 0, 0, 0};

int32_t alya_gui_window_poll_event(alya_gui_window_t *win) {
    alya_gui_stashed.kind = 0;
    alya_gui_stashed.width = 0;
    alya_gui_stashed.height = 0;
    alya_gui_stashed.mouse_x = 0;
    alya_gui_stashed.mouse_y = 0;
    alya_gui_stashed.key = 0;
    alya_gui_text_stash[0] = '\0';
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

int32_t alya_gui_event_key(void) {
    return alya_gui_stashed.key;
}

const char *alya_gui_event_text(void) {
    return alya_gui_text_stash;
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
