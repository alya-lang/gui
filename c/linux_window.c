// Linux backend for the Alya GUI toolkit (Faz 1: windowing foundation).
//
// Wayland (raw wire protocol) is tried first; Xlib/X11 is the runtime
// fallback when no compositor is reachable. Both live in this single TU so
// every public `alya_gui_window_*` symbol is defined exactly once.

#include "linux_window.h"
#include "gpu_contract.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

#define ALYA_GUI_MAX_EVENTS 64

// Backend selector: 0 = none, 1 = Wayland, 2 = X11.
#define ALYA_GUI_LINUX_NONE 0
#define ALYA_GUI_LINUX_WAYLAND 1
#define ALYA_GUI_LINUX_X11 2

struct alya_gui_window {
    int active;
    // Shared state (both backends).
    int32_t open;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t head;
    int32_t tail;
    alya_gui_event_t queue[ALYA_GUI_MAX_EVENTS];
    // Wayland state.
    int wl_fd;
    uint32_t wl_next_id;
    uint32_t wl_display_id;
    uint32_t wl_registry_id;
    uint32_t wl_compositor_id;
    uint32_t wl_shm_id;
    uint32_t wl_xdg_base_id;
    uint32_t wl_surface_id;
    uint32_t wl_xdg_surface_id;
    uint32_t wl_toplevel_id;
    uint32_t wl_seat_id;
    uint32_t wl_pointer_id;
    uint32_t wl_keyboard_id;
    uint32_t wl_shm_pool_fd;
    uint8_t *wl_shm_data;
    size_t wl_shm_size;
    uint32_t wl_buffer_id;
    int32_t wl_sync_done;
    uint32_t wl_sync_id;
    // X11 state.
    Display *xdisplay;
    Window xwindow;
    Atom xwm_delete;
    XIM xim;
    XIC xic;
};

static void alya_gui_push_event(alya_gui_window_t *win, int32_t kind,
                                int32_t key) {
    int32_t next;
    if (win == NULL) {
        return;
    }
    next = (win->tail + 1) % ALYA_GUI_MAX_EVENTS;
    if (next == win->head) {
        return;
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

static alya_gui_event_t alya_gui_stashed = {0, 0, 0, 0, 0, 0};

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

// --- Wayland wire helpers ---
// --- Wire helpers (all integers little-endian on supported targets) ---

static uint32_t alya_rd32(const uint8_t *p) {
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void alya_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

#define ALYA_GUI_MAX_EVENTS 64

static int wl_send(alya_gui_window_t *win, const uint8_t *buf,
                         size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(win->wl_fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

// Request with fixed-size payload words (no strings/fds/new_ids).
static int wl_req(alya_gui_window_t *win, uint32_t obj, uint32_t opcode,
                        const uint32_t *words, size_t nwords) {
    uint8_t buf[64];
    uint32_t size = (uint32_t)(8 + nwords * 4);
    if (8 + nwords * 4 > sizeof(buf)) {
        return -1;
    }
    alya_wr32(buf, obj);
    alya_wr32(buf + 4, (size << 16) | (opcode & 0xFFFF));
    for (size_t i = 0; i < nwords; i++) {
        alya_wr32(buf + 8 + i * 4, words[i]);
    }
    return wl_send(win, buf, size);
}

static uint32_t wl_new_id(alya_gui_window_t *win) {
    return win->wl_next_id++;
}

// Request carrying one NUL-terminated string argument.
static int wl_req_str(alya_gui_window_t *win, uint32_t obj,
                            uint32_t opcode, const char *str,
                            const uint32_t *extra, size_t nextra) {
    uint8_t buf[512];
    size_t slen = str ? strlen(str) + 1 : 1;
    size_t padded = (slen + 3) & ~((size_t)3);
    uint32_t size = (uint32_t)(8 + 4 + padded + nextra * 4);
    size_t off = 0;
    if (size > sizeof(buf)) {
        return -1;
    }
    alya_wr32(buf, obj);
    alya_wr32(buf + 4, (size << 16) | (opcode & 0xFFFF));
    off = 8;
    alya_wr32(buf + off, (uint32_t)slen);
    off += 4;
    memset(buf + off, 0, padded);
    if (str) {
        memcpy(buf + off, str, slen - 1);
    }
    off += padded;
    for (size_t i = 0; i < nextra; i++) {
        alya_wr32(buf + off, extra[i]);
        off += 4;
    }
    return wl_send(win, buf, size);
}

static int wl_connect(alya_gui_window_t *win) {
    const char *sock_name = getenv("WAYLAND_DISPLAY");
    const char *run_dir;
    char path[256];
    struct sockaddr_un addr;
    int fd;
    if (sock_name == NULL || sock_name[0] == '\0') {
        return -1;
    }
    if (sock_name[0] != '/') {
        run_dir = getenv("XDG_RUNTIME_DIR");
        if (run_dir == NULL) {
            return -1;
        }
        snprintf(path, sizeof(path), "%s/%s", run_dir, sock_name);
        sock_name = path;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_name, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    win->wl_fd = fd;
    return 0;
}

// --- Event dispatch ---

static void wl_handle(alya_gui_window_t *win, uint32_t obj,
                            uint32_t opcode, const uint8_t *body,
                            size_t body_len) {
    (void)body_len;
    if (obj == win->wl_display_id && opcode == 0) {
        // wl_display error: object_id u32, code u32, message string.
        // Logged to stderr; the compositor kills us right after, which the
        // next recv observes as a closed connection.
        if (body_len >= 8) {
            uint32_t code = alya_rd32(body + 4);
            fprintf(stderr, "alya-gui: wayland protocol error %u\n",
                    (unsigned)code);
        }
        return;
    }
    if (obj == win->wl_registry_id && opcode == 0) {
        // global: name u32, interface string, version u32.
        uint32_t name;
        uint32_t slen;
        const char *iface;
        uint32_t version;
        size_t off;
        if (body_len < 12) {
            return;
        }
        name = alya_rd32(body);
        slen = alya_rd32(body + 4);
        iface = (const char *)(body + 8);
        off = 8 + ((slen + 3) & ~((size_t)3));
        if (off + 4 > body_len) {
            return;
        }
        version = alya_rd32(body + off);
        (void)version;
        if (strcmp(iface, "wl_compositor") == 0 && win->wl_compositor_id == 0) {
            win->wl_compositor_id = name;
        } else if (strcmp(iface, "wl_shm") == 0 && win->wl_shm_id == 0) {
            win->wl_shm_id = name;
        } else if (strcmp(iface, "xdg_wm_base") == 0 && win->wl_xdg_base_id == 0) {
            win->wl_xdg_base_id = name;
        } else if (strcmp(iface, "wl_seat") == 0 && win->wl_seat_id == 0) {
            win->wl_seat_id = name;
        }
    } else if (obj == win->wl_sync_id && opcode == 0) {
        win->wl_sync_done = 1;
    } else if (obj == win->wl_xdg_base_id && opcode == 0) {
        // xdg_wm_base ping: pong(serial).
        uint32_t serial = body_len >= 4 ? alya_rd32(body) : 0;
        wl_req(win, win->wl_xdg_base_id, 1, &serial, 1);
    } else if (obj == win->wl_xdg_surface_id && opcode == 0) {
        // xdg_surface configure: ack_configure(serial).
        uint32_t serial = body_len >= 4 ? alya_rd32(body) : 0;
        wl_req(win, win->wl_xdg_surface_id, 4, &serial, 1);
    } else if (obj == win->wl_toplevel_id && opcode == 0) {
        // xdg_toplevel configure: width/height/states (may be 0 = unset).
        if (body_len >= 8) {
            int32_t w = (int32_t)alya_rd32(body);
            int32_t h = (int32_t)alya_rd32(body + 4);
            if (w > 0 && h > 0 && (w != win->width || h != win->height)) {
                win->width = w;
                win->height = h;
                alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE, 0);
            }
        }
    } else if (obj == win->wl_toplevel_id && opcode == 1) {
        // xdg_toplevel close.
        win->open = 0;
        alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE, 0);
    } else if (obj == win->wl_seat_id && opcode == 0) {
        // seat capabilities: bit 0 = pointer, bit 1 = keyboard.
        if (body_len >= 4) {
            uint32_t caps = alya_rd32(body);
            if ((caps & 1) && win->wl_pointer_id == 0) {
                uint32_t id = wl_new_id(win);
                if (wl_req(win, win->wl_seat_id, 0, &id, 1) == 0) {
                    win->wl_pointer_id = id;
                }
            }
            if ((caps & 2) && win->wl_keyboard_id == 0) {
                uint32_t id = wl_new_id(win);
                if (wl_req(win, win->wl_seat_id, 1, &id, 1) == 0) {
                    win->wl_keyboard_id = id;
                }
            }
        }
    } else if (obj == win->wl_pointer_id && opcode == 2) {
        // pointer motion: time u32, surface_x fixed, surface_y fixed.
        if (body_len >= 12) {
            int32_t fx = (int32_t)alya_rd32(body + 4);
            int32_t fy = (int32_t)alya_rd32(body + 8);
            win->mouse_x = fx >> 8;
            win->mouse_y = fy >> 8;
            alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_MOVE, 0);
        }
    } else if (obj == win->wl_pointer_id && opcode == 3) {
        // pointer button: serial, time, button, state (pressed = 1).
        // Coords come from the last motion event (buttons carry none).
        if (body_len >= 16) {
            uint32_t state = alya_rd32(body + 12);
            if (state == 1) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_DOWN, 0);
            } else {
                alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_UP, 0);
            }
        }
    } else if (obj == win->wl_keyboard_id && opcode == 3) {
        // keyboard key: serial, time, key (evdev code), state (1 = pressed).
        // Text composition needs the input-method protocol (Phase 6);
        // printable text arrives on backends with OS text services.
        if (body_len >= 16) {
            uint32_t key = alya_rd32(body + 8);
            uint32_t state = alya_rd32(body + 12);
            if (state == 1) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_DOWN,
                                    (int32_t)key);
            } else {
                alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_UP,
                                    (int32_t)key);
            }
        }
    }
}

// Read and dispatch all pending messages (non-blocking when drained).
static void wl_dispatch(alya_gui_window_t *win, int block) {
    uint8_t buf[8192];
    size_t have = 0;
    for (;;) {
        struct pollfd pfd;
        ssize_t n;
        size_t off = 0;
        pfd.fd = win->wl_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, block ? 50 : 0) <= 0) {
            return;
        }
        n = recv(win->wl_fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                return;
            }
            win->open = 0;
            return;
        }
        have = (size_t)n;
        while (off + 8 <= have) {
            uint32_t obj = alya_rd32(buf + off);
            uint32_t hdr = alya_rd32(buf + off + 4);
            uint32_t size = hdr >> 16;
            uint32_t opcode = hdr & 0xFFFF;
            if (size < 8 || off + size > have) {
                break;
            }
            wl_handle(win, obj, opcode, buf + off + 8, size - 8);
            off += size;
        }
        if (!block) {
            // Drain burst without blocking; new data triggers next poll.
            struct pollfd p2;
            p2.fd = win->wl_fd;
            p2.events = POLLIN;
            p2.revents = 0;
            if (poll(&p2, 1, 0) <= 0) {
                return;
            }
        }
    }
}

static int wl_roundtrip(alya_gui_window_t *win) {
    uint32_t args[1];
    win->wl_sync_done = 0;
    win->wl_sync_id = wl_new_id(win);
    args[0] = win->wl_sync_id;
    // wl_display.sync(new_id).
    if (wl_req(win, win->wl_display_id, 0, args, 1) < 0) {
        return -1;
    }
    while (!win->wl_sync_done) {
        wl_dispatch(win, 1);
        if (!win->open && win->wl_fd < 0) {
            return -1;
        }
    }
    return 0;
}

// --- Setup ---

static int wl_bind_globals(alya_gui_window_t *win) {
    // wl_registry.bind(name, interface, version, new_id): string + u32s.
    if (win->wl_compositor_id != 0) {
        uint32_t id = wl_new_id(win);
        uint32_t extra[2] = {4, id};
        if (wl_req_str(win, win->wl_registry_id, 0, "wl_compositor", extra, 2) < 0) {
            return -1;
        }
        win->wl_compositor_id = id;
    }
    if (win->wl_shm_id != 0) {
        uint32_t id = wl_new_id(win);
        uint32_t extra[2] = {1, id};
        if (wl_req_str(win, win->wl_registry_id, 0, "wl_shm", extra, 2) < 0) {
            return -1;
        }
        win->wl_shm_id = id;
    }
    if (win->wl_xdg_base_id != 0) {
        uint32_t id = wl_new_id(win);
        uint32_t extra[2] = {1, id};
        if (wl_req_str(win, win->wl_registry_id, 0, "xdg_wm_base", extra, 2) < 0) {
            return -1;
        }
        win->wl_xdg_base_id = id;
    }
    if (win->wl_seat_id != 0) {
        uint32_t id = wl_new_id(win);
        uint32_t extra[2] = {5, id};
        if (wl_req_str(win, win->wl_registry_id, 0, "wl_seat", extra, 2) < 0) {
            return -1;
        }
        win->wl_seat_id = id;
    }
    return 0;
}

// Attaches a solid-color shm buffer so the mapped surface has content
// (XRGB8888 via POSIX shm_open; no _GNU_SOURCE needed).
static int wl_attach_solid(alya_gui_window_t *win, uint32_t color) {
    char shm_name[64];
    int fd;
    size_t stride;
    size_t size;
    uint8_t *data;
    size_t i, count;

    if (win->wl_shm_id == 0 || win->width <= 0 || win->height <= 0) {
        return -1;
    }
    stride = (size_t)win->width * 4;
    size = stride * (size_t)win->height;
    snprintf(shm_name, sizeof(shm_name), "/alya-gui-%d-%u", (int)getpid(),
             win->wl_surface_id);
    fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    shm_unlink(shm_name);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    data = (uint8_t *)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }
    count = size / 4;
    for (i = 0; i < count; i++) {
        alya_wr32(data + i * 4, color);
    }
    // wl_shm.create_pool(new_id, fd, size): the fd travels out-of-band
    // via SCM_RIGHTS and occupies zero inline bytes.
    win->wl_buffer_id = wl_new_id(win);
    {
        uint8_t msg[16];
        struct msghdr mh;
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr *cmsg;
        alya_wr32(msg, win->wl_shm_id);
        alya_wr32(msg + 4, (16 << 16) | 0);
        alya_wr32(msg + 8, win->wl_buffer_id);
        alya_wr32(msg + 12, (uint32_t)size);
        memset(&mh, 0, sizeof(mh));
        iov.iov_base = msg;
        iov.iov_len = sizeof(msg);
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cmsg_buf;
        mh.msg_controllen = sizeof(cmsg_buf);
        cmsg = CMSG_FIRSTHDR(&mh);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
        mh.msg_controllen = cmsg->cmsg_len;
        if (sendmsg(win->wl_fd, &mh, MSG_NOSIGNAL) < 0) {
            munmap(data, size);
            close(fd);
            return -1;
        }
    }
    close(fd);
    win->wl_shm_data = data;
    win->wl_shm_size = size;
    // wl_shm_pool.create_buffer(new_id, offset, w, h, stride, XRGB8888=1).
    {
        uint32_t buf_id = wl_new_id(win);
        uint8_t bmsg[8 + 6 * 4];
        alya_wr32(bmsg, win->wl_buffer_id);
        alya_wr32(bmsg + 4, ((8 + 6 * 4) << 16) | 0);
        alya_wr32(bmsg + 8, buf_id);
        alya_wr32(bmsg + 12, 0);
        alya_wr32(bmsg + 16, (uint32_t)win->width);
        alya_wr32(bmsg + 20, (uint32_t)win->height);
        alya_wr32(bmsg + 24, (uint32_t)stride);
        alya_wr32(bmsg + 28, 1);
        if (wl_send(win, bmsg, sizeof(bmsg)) < 0) {
            return -1;
        }
        win->wl_buffer_id = buf_id;
    }
    // wl_surface.attach(buffer, 0, 0) + damage(full) — commit follows.
    {
        uint32_t aargs[3];
        uint32_t dargs[4];
        aargs[0] = win->wl_buffer_id;
        aargs[1] = 0;
        aargs[2] = 0;
        if (wl_req(win, win->wl_surface_id, 1, aargs, 3) < 0) {
            return -1;
        }
        dargs[0] = 0;
        dargs[1] = 0;
        dargs[2] = (uint32_t)win->width;
        dargs[3] = (uint32_t)win->height;
        wl_req(win, win->wl_surface_id, 2, dargs, 4);
    }
    return 0;
}

static int wl_make_surface(alya_gui_window_t *win, const char *title) {
    uint32_t args[3];
    // wl_compositor.create_surface -> surface.
    win->wl_surface_id = wl_new_id(win);
    args[0] = win->wl_surface_id;
    if (wl_req(win, win->wl_compositor_id, 0, args, 1) < 0) {
        return -1;
    }
    // xdg_wm_base.get_xdg_surface(surface) -> xdg_surface.
    win->wl_xdg_surface_id = wl_new_id(win);
    args[0] = win->wl_surface_id;
    args[1] = win->wl_xdg_surface_id;
    if (wl_req(win, win->wl_xdg_base_id, 2, args, 2) < 0) {
        return -1;
    }
    // xdg_surface.get_toplevel -> toplevel.
    win->wl_toplevel_id = wl_new_id(win);
    args[0] = win->wl_toplevel_id;
    if (wl_req(win, win->wl_xdg_surface_id, 1, args, 1) < 0) {
        return -1;
    }
    // xdg_toplevel.set_title + set_app_id.
    if (wl_req_str(win, win->wl_toplevel_id, 2, title, NULL, 0) < 0) {
        return -1;
    }
    if (wl_req_str(win, win->wl_toplevel_id, 3, "alya-gui", NULL, 0) < 0) {
        return -1;
    }
    // Attach a dark solid buffer so the surface has visible content, then
    // commit so the compositor maps it.
    wl_attach_solid(win, 0xFF181825);
    if (wl_req(win, win->wl_surface_id, 6, NULL, 0) < 0) {
        return -1;
    }
    return 0;
}

// X11 fallback backend for the Alya GUI toolkit (Faz 1: windowing).
//
// Minimal Xlib client: window create/map, non-blocking event pump, close
// protocol (WM_DELETE_WINDOW), resize/motion tracking. Used when no Wayland
// compositor is reachable. Links against libX11 (present on X11 desktops).

static alya_gui_window_t *x11_window_create(const char *title, int32_t width,
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
    win->xdisplay = display;
    win->xwindow = window;
    win->xwm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &win->xwm_delete, 1);
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
                     ButtonPressMask | ButtonReleaseMask | KeyPressMask |
                     KeyReleaseMask | FocusChangeMask);
    win->active = ALYA_GUI_LINUX_X11;
    // XIM input method for composed text (CJK etc.); plain KeySym path
    // below stays the fallback when no IM answers.
    win->xim = XOpenIM(display, NULL, NULL, NULL);
    if (win->xim != NULL) {
        win->xic = XCreateIC(win->xim, XNInputStyle,
                             (long)(XIMPreeditNothing | XIMStatusNothing),
                             XNClientWindow, window, XNFocusWindow, window,
                             NULL);
        if (win->xic == NULL) {
            XCloseIM(win->xim);
            win->xim = NULL;
        }
    }
    win->active = ALYA_GUI_LINUX_X11;
    win->open = 1;
    win->width = width;
    win->height = height;
    return win;
}

static void x11_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->xic != NULL) {
        XDestroyIC(win->xic);
        win->xic = NULL;
    }
    if (win->xim != NULL) {
        XCloseIM(win->xim);
        win->xim = NULL;
    }
    if (win->xdisplay != NULL) {
        if (win->xwindow != 0) {
            XDestroyWindow(win->xdisplay, win->xwindow);
        }
        XCloseDisplay(win->xdisplay);
    }
    free(win);
}

static void x11_window_show(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    XMapWindow(win->xdisplay, win->xwindow);
    XFlush(win->xdisplay);
}

static void x11_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    XUnmapWindow(win->xdisplay, win->xwindow);
    XFlush(win->xdisplay);
}

static int32_t x11_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    return win->open;
}

static int32_t x11_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    Display *display;
    if (win == NULL || out == NULL) {
        return 0;
    }
    display = win->xdisplay;
    while (XPending(display) > 0) {
        XEvent ev;
        XNextEvent(display, &ev);
        // Let the input method filter composed-text events first.
        if (win->xic != NULL && XFilterEvent(&ev, None)) {
            continue;
        }
        switch (ev.type) {
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == win->xwm_delete) {
                win->open = 0;
                alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE, 0);
            }
            break;
        case ConfigureNotify:
            if (ev.xconfigure.width != win->width ||
                ev.xconfigure.height != win->height) {
                win->width = ev.xconfigure.width;
                win->height = ev.xconfigure.height;
                alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE, 0);
            }
            break;
        case Expose:
            if (ev.xexpose.count == 0) {
                alya_gui_push_event(win, ALYA_GUI_EVENT_REDRAW, 0);
            }
            break;
        case MotionNotify:
            win->mouse_x = ev.xmotion.x;
            win->mouse_y = ev.xmotion.y;
            alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_MOVE, 0);
            break;
        case ButtonPress:
            win->mouse_x = ev.xbutton.x;
            win->mouse_y = ev.xbutton.y;
            alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_DOWN, 0);
            break;
        case ButtonRelease:
            win->mouse_x = ev.xbutton.x;
            win->mouse_y = ev.xbutton.y;
            alya_gui_push_event(win, ALYA_GUI_EVENT_MOUSE_UP, 0);
            break;
        case KeyPress:
        case KeyRelease: {
            // KeySym identifies the key (layout-dependent); printable text
            // prefers the XIM path (composed CJK), else XLookupString.
            KeySym sym = XLookupKeysym(&ev.xkey, 0);
            if (ev.type == KeyPress) {
                char text[64];
                int n = 0;
                if (win->xic != NULL) {
                    Status status = 0;
                    n = Xutf8LookupString(win->xic, &ev.xkey, text,
                                          (int)sizeof(text) - 1, &sym,
                                          &status);
                    if (status == XBufferOverflow) {
                        n = 0;
                    }
                } else {
                    n = XLookupString(&ev.xkey, text, (int)sizeof(text) - 1,
                                      NULL, NULL);
                }
                alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_DOWN,
                                    (int32_t)sym);
                if (n > 0 && (unsigned char)text[0] >= 0x20) {
                    alya_gui_set_text(text, (size_t)n);
                    alya_gui_push_event(win, ALYA_GUI_EVENT_TEXT_INPUT, 0);
                }
            } else {
                alya_gui_push_event(win, ALYA_GUI_EVENT_KEY_UP,
                                    (int32_t)sym);
            }
            break;
        }
        case FocusIn:
            if (win->xic != NULL) {
                XSetICFocus(win->xic);
            }
            alya_gui_push_event(win, ALYA_GUI_EVENT_FOCUS, 1);
            break;
        case FocusOut:
            if (win->xic != NULL) {
                XUnsetICFocus(win->xic);
            }
            alya_gui_push_event(win, ALYA_GUI_EVENT_FOCUS, 0);
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

static void x11_window_set_title(alya_gui_window_t *win, const char *title) {
    if (win == NULL || title == NULL) {
        return;
    }
    XStoreName(win->xdisplay, win->xwindow, title);
    XFlush(win->xdisplay);
}

static void x11_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    XResizeWindow(win->xdisplay, win->xwindow, (unsigned)width,
                  (unsigned)height);
    XFlush(win->xdisplay);
}

static void x11_window_size(alya_gui_window_t *win, int32_t *w, int32_t *h) {
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

static void x11_window_close(alya_gui_window_t *win) {
    XEvent ev;
    if (win == NULL) {
        return;
    }
    // Synthesize the same ClientMessage path as the window manager close.
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win->xwindow;
    ev.xclient.message_type =
        XInternAtom(win->xdisplay, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)win->xwm_delete;
    XSendEvent(win->xdisplay, win->xwindow, False, NoEventMask, &ev);
    XFlush(win->xdisplay);
}

// --- Wayland public entry points (static; dispatch layer selects) ---

static alya_gui_window_t *wl_window_create(const char *title, int32_t width,
                                           int32_t height) {
    alya_gui_window_t *win;
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    win = (alya_gui_window_t *)calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }
    win->active = ALYA_GUI_LINUX_WAYLAND;
    win->wl_fd = -1;
    win->wl_display_id = 1;
    win->wl_next_id = 2;
    win->open = 1;
    win->width = width;
    win->height = height;
    win->wl_shm_pool_fd = 0;
    if (wl_connect(win) < 0) {
        free(win);
        return NULL;
    }
    // wl_display.get_registry -> registry id 2.
    win->wl_registry_id = wl_new_id(win);
    {
        uint32_t args[1];
        args[0] = win->wl_registry_id;
        if (wl_req(win, win->wl_display_id, 1, args, 1) < 0) {
            close(win->wl_fd);
            free(win);
            return NULL;
        }
    }
    if (wl_roundtrip(win) < 0) {
        close(win->wl_fd);
        free(win);
        return NULL;
    }
    if (win->wl_compositor_id == 0 || win->wl_xdg_base_id == 0) {
        close(win->wl_fd);
        free(win);
        return NULL;
    }
    if (wl_bind_globals(win) < 0) {
        close(win->wl_fd);
        free(win);
        return NULL;
    }
    if (wl_make_surface(win, title) < 0) {
        close(win->wl_fd);
        free(win);
        return NULL;
    }
    if (wl_roundtrip(win) < 0) {
        close(win->wl_fd);
        free(win);
        return NULL;
    }
    return win;
}

static void wl_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->wl_fd >= 0) {
        close(win->wl_fd);
    }
    if (win->wl_shm_data != NULL) {
        munmap(win->wl_shm_data, win->wl_shm_size);
    }
    if (win->wl_shm_pool_fd != 0) {
        close((int)win->wl_shm_pool_fd);
    }
    free(win);
}

static void wl_window_show(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    // (Re)commit so a hidden-then-shown surface remaps.
    wl_req(win, win->wl_surface_id, 6, NULL, 0);
}

static void wl_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    // Detach buffer + commit hides the surface content.
    {
        uint32_t args[3] = {0, 0, 0};
        wl_req(win, win->wl_surface_id, 1, args, 3);
        wl_req(win, win->wl_surface_id, 6, NULL, 0);
    }
}

static int32_t wl_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    return win->open;
}

static int32_t wl_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    if (win == NULL || out == NULL) {
        return 0;
    }
    wl_dispatch(win, 0);
    if (win->head == win->tail) {
        return 0;
    }
    *out = win->queue[win->head];
    win->head = (win->head + 1) % ALYA_GUI_MAX_EVENTS;
    return 1;
}

static void wl_window_set_title(alya_gui_window_t *win, const char *title) {
    if (win == NULL) {
        return;
    }
    wl_req_str(win, win->wl_toplevel_id, 2, title, NULL, 0);
}

static void wl_window_set_size(alya_gui_window_t *win, int32_t width,
                               int32_t height) {
    uint32_t args[4];
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    args[0] = 0;
    args[1] = 0;
    args[2] = (uint32_t)width;
    args[3] = (uint32_t)height;
    wl_req(win, win->wl_xdg_surface_id, 3, args, 4);
}

static void wl_window_close(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    win->open = 0;
    alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE, 0);
}

// --- Public API: runtime backend dispatch ---

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height) {
    // Wayland first; X11 fallback when no compositor answers.
    alya_gui_window_t *wl = wl_window_create(title, width, height);
    if (wl != NULL) {
        return wl;
    }
    return x11_window_create(title, width, height);
}

void alya_gui_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_destroy(win);
    } else {
        x11_window_destroy(win);
    }
}

void alya_gui_window_show(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_show(win);
    } else {
        x11_window_show(win);
    }
}

void alya_gui_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_hide(win);
    } else {
        x11_window_hide(win);
    }
}

int32_t alya_gui_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        return wl_window_is_open(win);
    }
    return x11_window_is_open(win);
}

int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    if (win == NULL || out == NULL) {
        return 0;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        return wl_window_poll(win, out);
    }
    return x11_window_poll(win, out);
}

void alya_gui_window_set_title(alya_gui_window_t *win, const char *title) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_set_title(win, title);
    } else {
        x11_window_set_title(win, title);
    }
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
    // Both backends track size in the shared fields.
    if (w != NULL) {
        *w = win->width;
    }
    if (h != NULL) {
        *h = win->height;
    }
}

void alya_gui_window_close(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_close(win);
    } else {
        x11_window_close(win);
    }
}

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
        return 0;
    }
    if (alya_gui_window_poll(win, &alya_gui_stashed) == 0) {
        return 0;
    }
    return alya_gui_stashed.kind;
}

void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    if (win == NULL) {
        return;
    }
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        wl_window_set_size(win, width, height);
    } else {
        x11_window_set_size(win, width, height);
    }
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

int32_t alya_gui_a11y_notify(alya_gui_window_t *win, int32_t code) {
    (void)win;
    (void)code;
    // Linux screen readers ride AT-SPI over the session bus, which needs
    // the bus registry dance plus the provider tree (struct-array FFI:
    // see a11y_contract.h). Honest stub until that follow-up lands.
    return 0;
}

// --- GPU surface: Wayland shm upload, X11 XPutImage ---
//
// Staged XRGB reaches the screen without EGL: on Wayland through a
// persistent shm pool (one buffer per present, the previous buffer is
// destroyed), on X11 through a 24-bit TrueColor XImage. Both are real
// presents to the bound window; EGL stays future work.

struct alya_gpu_surface {
    alya_gui_window_t *win;
    uint32_t *staging;
    int32_t width;
    int32_t height;
    int pool_fd;
    uint8_t *pool_data;
    size_t pool_size;
    uint32_t wl_pool_id;
    uint32_t wl_buffer;
};

alya_gpu_surface_t *alya_gpu_surface_create(void *native_win, int32_t width,
                                            int32_t height) {
    alya_gpu_surface_t *surf;
    if (native_win == NULL || width <= 0 || height <= 0) {
        return NULL;
    }
    surf = (alya_gpu_surface_t *)calloc(1, sizeof(*surf));
    if (surf == NULL) {
        return NULL;
    }
    surf->staging =
        (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (surf->staging == NULL) {
        free(surf);
        return NULL;
    }
    surf->win = (alya_gui_window_t *)native_win;
    surf->width = width;
    surf->height = height;
    surf->pool_fd = -1;
    return surf;
}

void alya_gpu_surface_destroy(alya_gpu_surface_t *surf) {
    if (surf == NULL) {
        return;
    }
    if (surf->win != NULL &&
        surf->win->active == ALYA_GUI_LINUX_WAYLAND) {
        if (surf->wl_buffer != 0) {
            wl_req(surf->win, surf->wl_buffer, 0, NULL, 0);
            wl_dispatch(surf->win, 0);
        }
    }
    if (surf->pool_data != NULL) {
        munmap(surf->pool_data, surf->pool_size);
    }
    free(surf->staging);
    free(surf);
}

int32_t alya_gpu_surface_stage(alya_gpu_surface_t *surf, int32_t index,
                               int32_t color) {
    if (surf == NULL || surf->staging == NULL || index < 0) {
        return 0;
    }
    if (index >= surf->width * surf->height) {
        return 0;
    }
    surf->staging[index] = (uint32_t)(color & 0xFFFFFF);
    return 1;
}

// Ensures the shm pool holds at least `need` bytes (recreates + unmaps
// the old one). Returns 0 on success.
static int wl_surface_ensure_pool(alya_gui_window_t *win,
                                  alya_gpu_surface_t *surf, size_t need) {
    char shm_name[64];
    int fd;
    uint8_t *data;
    uint8_t msg[16];
    struct msghdr mh;
    struct iovec iov;
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    if (surf->pool_data != NULL && surf->pool_size >= need) {
        return 0;
    }
    if (win->wl_shm_id == 0) {
        return -1;
    }
    snprintf(shm_name, sizeof(shm_name), "/alya-gpu-%d-%p", (int)getpid(),
             (void *)surf);
    fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return -1;
    }
    shm_unlink(shm_name);
    if (ftruncate(fd, (off_t)need) < 0) {
        close(fd);
        return -1;
    }
    data = (uint8_t *)mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                           0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }
    if (surf->wl_buffer != 0) {
        wl_req(win, surf->wl_buffer, 0, NULL, 0);
        surf->wl_buffer = 0;
    }
    if (surf->pool_data != NULL) {
        munmap(surf->pool_data, surf->pool_size);
    }
    surf->wl_pool_id = wl_new_id(win);
    alya_wr32(msg, win->wl_shm_id);
    alya_wr32(msg + 4, (16 << 16) | 0);
    alya_wr32(msg + 8, surf->wl_pool_id);
    alya_wr32(msg + 12, (uint32_t)need);
    memset(&mh, 0, sizeof(mh));
    iov.iov_base = msg;
    iov.iov_len = sizeof(msg);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cmsg_buf;
    mh.msg_controllen = sizeof(cmsg_buf);
    cmsg = CMSG_FIRSTHDR(&mh);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
    mh.msg_controllen = cmsg->cmsg_len;
    if (sendmsg(win->wl_fd, &mh, MSG_NOSIGNAL) < 0) {
        munmap(data, need);
        close(fd);
        return -1;
    }
    close(fd);
    surf->pool_fd = -1;
    surf->pool_data = data;
    surf->pool_size = need;
    return 0;
}

// Uploads staging to a Wayland surface: pool copy + create_buffer +
// attach + damage + commit. Returns 1 when the commit was sent.
static int wl_surface_upload(alya_gui_window_t *win, alya_gpu_surface_t *surf) {
    size_t stride;
    size_t size;
    uint32_t buf_id;
    uint8_t bmsg[8 + 6 * 4];
    uint32_t aargs[3];
    uint32_t dargs[4];
    if (win->wl_surface_id == 0) {
        return 0;
    }
    stride = (size_t)surf->width * 4;
    size = stride * (size_t)surf->height;
    if (wl_surface_ensure_pool(win, surf, size) < 0) {
        return 0;
    }
    memcpy(surf->pool_data, surf->staging, size);
    // Retire the previously presented buffer before replacing it.
    if (surf->wl_buffer != 0) {
        wl_req(win, surf->wl_buffer, 0, NULL, 0);
        surf->wl_buffer = 0;
    }
    // wl_shm_pool.create_buffer(new_id, offset, w, h, stride, XRGB8888=1).
    buf_id = wl_new_id(win);
    alya_wr32(bmsg, surf->wl_pool_id);
    alya_wr32(bmsg + 4, ((8 + 6 * 4) << 16) | 0);
    alya_wr32(bmsg + 8, buf_id);
    alya_wr32(bmsg + 12, 0);
    alya_wr32(bmsg + 16, (uint32_t)surf->width);
    alya_wr32(bmsg + 20, (uint32_t)surf->height);
    alya_wr32(bmsg + 24, (uint32_t)stride);
    alya_wr32(bmsg + 28, 1);
    if (wl_send(win, bmsg, sizeof(bmsg)) < 0) {
        return 0;
    }
    surf->wl_buffer = buf_id;
    // wl_surface.attach(buffer, 0, 0) + damage(full) + commit.
    aargs[0] = buf_id;
    aargs[1] = 0;
    aargs[2] = 0;
    if (wl_req(win, win->wl_surface_id, 1, aargs, 3) < 0) {
        return 0;
    }
    dargs[0] = 0;
    dargs[1] = 0;
    dargs[2] = (uint32_t)surf->width;
    dargs[3] = (uint32_t)surf->height;
    wl_req(win, win->wl_surface_id, 2, dargs, 4);
    wl_req(win, win->wl_surface_id, 6, NULL, 0);
    return 1;
}

// Presents staging on X11 via a 24-bit TrueColor XImage (visual masks
// queried, never assumed). Returns 1 when the image reached the window.
static int x11_surface_upload(alya_gui_window_t *win,
                              alya_gpu_surface_t *surf) {
    Display *display = win->xdisplay;
    int screen = DefaultScreen(display);
    XVisualInfo vinfo;
    XImage *img;
    if (win->xwindow == 0) {
        return 0;
    }
    if (!XMatchVisualInfo(display, screen, 24, TrueColor, &vinfo)) {
        return 0;
    }
    img = XCreateImage(display, vinfo.visual, (unsigned)vinfo.depth, ZPixmap,
                       0, (char *)surf->staging, (unsigned)surf->width,
                       (unsigned)surf->height, 32,
                       (int)((size_t)surf->width * 4));
    if (img == NULL) {
        return 0;
    }
    img->byte_order = LSBFirst;
    img->red_mask = (unsigned long)vinfo.red_mask;
    img->green_mask = (unsigned long)vinfo.green_mask;
    img->blue_mask = (unsigned long)vinfo.blue_mask;
    XPutImage(display, win->xwindow, DefaultGC(display, screen), img, 0, 0, 0,
              0, (unsigned)surf->width, (unsigned)surf->height);
    XFlush(display);
    img->data = NULL;
    XDestroyImage(img);
    return 1;
}

int32_t alya_gpu_surface_present(alya_gpu_surface_t *surf) {
    alya_gui_window_t *win;
    if (surf == NULL || surf->win == NULL || surf->staging == NULL) {
        return 0;
    }
    win = surf->win;
    if (win->active == ALYA_GUI_LINUX_WAYLAND) {
        return wl_surface_upload(win, surf);
    }
    return x11_surface_upload(win, surf);
}

int32_t alya_gpu_surface_resize(alya_gpu_surface_t *surf, int32_t width,
                                int32_t height) {
    uint32_t *staging;
    if (surf == NULL || width <= 0 || height <= 0) {
        return 0;
    }
    staging =
        (uint32_t *)calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    if (staging == NULL) {
        return 0;
    }
    free(surf->staging);
    surf->staging = staging;
    surf->width = width;
    surf->height = height;
    // The shm pool is recreated lazily on the next present.
    return 1;
}

// Text rasterization wants a font stack (freetype/fontconfig); until
// that follow-up lands, canvas text on Linux stays GPU-surface-free.
// Honest stub so the contract links everywhere.
int32_t alya_gpu_surface_text(alya_gpu_surface_t *surf, const char *utf8,
                              int32_t x, int32_t y, int32_t size_px,
                              int32_t color) {
    (void)surf;
    (void)utf8;
    (void)x;
    (void)y;
    (void)size_px;
    (void)color;
    return 0;
}
