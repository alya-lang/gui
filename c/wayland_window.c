// Wayland backend for the Alya GUI toolkit (Faz 1: windowing foundation).
//
// Raw Wayland wire protocol over a Unix domain socket: no libwayland-client,
// no toolkit. Handles registry, xdg-shell surfaces, shm buffers, seat input
// (pointer + keyboard), and non-blocking event pumping.

#include "wayland_window.h"

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

struct alya_gui_window {
    int fd;
    uint32_t next_id;
    uint32_t display_id;
    uint32_t registry_id;
    uint32_t compositor_id;
    uint32_t shm_id;
    uint32_t xdg_base_id;
    uint32_t surface_id;
    uint32_t xdg_surface_id;
    uint32_t toplevel_id;
    uint32_t seat_id;
    uint32_t pointer_id;
    uint32_t keyboard_id;
    uint32_t shm_pool_fd;
    uint8_t *shm_data;
    size_t shm_size;
    uint32_t buffer_id;
    int32_t open;
    int32_t shown;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t head;
    int32_t tail;
    int32_t sync_done;
    uint32_t sync_id;
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

static int alya_gui_send(alya_gui_window_t *win, const uint8_t *buf,
                         size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(win->fd, buf + sent, len - sent, MSG_NOSIGNAL);
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
static int alya_gui_req(alya_gui_window_t *win, uint32_t obj, uint32_t opcode,
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
    return alya_gui_send(win, buf, size);
}

static uint32_t alya_gui_new_id(alya_gui_window_t *win) {
    return win->next_id++;
}

// Request carrying one NUL-terminated string argument.
static int alya_gui_req_str(alya_gui_window_t *win, uint32_t obj,
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
    return alya_gui_send(win, buf, size);
}

static int alya_gui_connect(alya_gui_window_t *win) {
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
    win->fd = fd;
    return 0;
}

// --- Event dispatch ---

static void alya_gui_handle(alya_gui_window_t *win, uint32_t obj,
                            uint32_t opcode, const uint8_t *body,
                            size_t body_len) {
    (void)body_len;
    if (obj == win->display_id && opcode == 0) {
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
    if (obj == win->registry_id && opcode == 0) {
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
        if (strcmp(iface, "wl_compositor") == 0 && win->compositor_id == 0) {
            win->compositor_id = name;
        } else if (strcmp(iface, "wl_shm") == 0 && win->shm_id == 0) {
            win->shm_id = name;
        } else if (strcmp(iface, "xdg_wm_base") == 0 && win->xdg_base_id == 0) {
            win->xdg_base_id = name;
        } else if (strcmp(iface, "wl_seat") == 0 && win->seat_id == 0) {
            win->seat_id = name;
        }
    } else if (obj == win->sync_id && opcode == 0) {
        win->sync_done = 1;
    } else if (obj == win->xdg_base_id && opcode == 0) {
        // xdg_wm_base ping: pong(serial).
        uint32_t serial = body_len >= 4 ? alya_rd32(body) : 0;
        alya_gui_req(win, win->xdg_base_id, 1, &serial, 1);
    } else if (obj == win->xdg_surface_id && opcode == 0) {
        // xdg_surface configure: ack_configure(serial).
        uint32_t serial = body_len >= 4 ? alya_rd32(body) : 0;
        alya_gui_req(win, win->xdg_surface_id, 4, &serial, 1);
    } else if (obj == win->toplevel_id && opcode == 0) {
        // xdg_toplevel configure: width/height/states (may be 0 = unset).
        if (body_len >= 8) {
            int32_t w = (int32_t)alya_rd32(body);
            int32_t h = (int32_t)alya_rd32(body + 4);
            if (w > 0 && h > 0 && (w != win->width || h != win->height)) {
                win->width = w;
                win->height = h;
                alya_gui_push_event(win, ALYA_GUI_EVENT_RESIZE);
            }
        }
    } else if (obj == win->toplevel_id && opcode == 1) {
        // xdg_toplevel close.
        win->open = 0;
        alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE);
    } else if (obj == win->seat_id && opcode == 0) {
        // seat capabilities: bit 0 = pointer, bit 1 = keyboard.
        if (body_len >= 4) {
            uint32_t caps = alya_rd32(body);
            if ((caps & 1) && win->pointer_id == 0) {
                uint32_t id = alya_gui_new_id(win);
                if (alya_gui_req(win, win->seat_id, 0, &id, 1) == 0) {
                    win->pointer_id = id;
                }
            }
            // Keyboard binding is deferred to Phase 3 input work.
        }
    } else if (obj == win->pointer_id && opcode == 2) {
        // pointer motion: time u32, surface_x fixed, surface_y fixed.
        if (body_len >= 12) {
            int32_t fx = (int32_t)alya_rd32(body + 4);
            int32_t fy = (int32_t)alya_rd32(body + 8);
            win->mouse_x = fx >> 8;
            win->mouse_y = fy >> 8;
        }
    } else if (obj == win->pointer_id && opcode == 3) {
        // pointer button: serial, time, button, state (pressed = 1).
        if (body_len >= 16) {
            uint32_t state = alya_rd32(body + 12);
            (void)state;
        }
    }
}

// Read and dispatch all pending messages (non-blocking when drained).
static void alya_gui_dispatch(alya_gui_window_t *win, int block) {
    uint8_t buf[8192];
    size_t have = 0;
    for (;;) {
        struct pollfd pfd;
        ssize_t n;
        size_t off = 0;
        pfd.fd = win->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, block ? 50 : 0) <= 0) {
            return;
        }
        n = recv(win->fd, buf, sizeof(buf), 0);
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
            alya_gui_handle(win, obj, opcode, buf + off + 8, size - 8);
            off += size;
        }
        if (!block) {
            // Drain burst without blocking; new data triggers next poll.
            struct pollfd p2;
            p2.fd = win->fd;
            p2.events = POLLIN;
            p2.revents = 0;
            if (poll(&p2, 1, 0) <= 0) {
                return;
            }
        }
    }
}

static int alya_gui_roundtrip(alya_gui_window_t *win) {
    uint32_t args[1];
    win->sync_done = 0;
    win->sync_id = alya_gui_new_id(win);
    args[0] = win->sync_id;
    // wl_display.sync(new_id).
    if (alya_gui_req(win, win->display_id, 0, args, 1) < 0) {
        return -1;
    }
    while (!win->sync_done) {
        alya_gui_dispatch(win, 1);
        if (!win->open && win->fd < 0) {
            return -1;
        }
    }
    return 0;
}

// --- Setup ---

static int alya_gui_bind_globals(alya_gui_window_t *win) {
    // wl_registry.bind(name, interface, version, new_id): string + u32s.
    if (win->compositor_id != 0) {
        uint32_t id = alya_gui_new_id(win);
        uint32_t extra[2] = {4, id};
        if (alya_gui_req_str(win, win->registry_id, 0, "wl_compositor", extra, 2) < 0) {
            return -1;
        }
        win->compositor_id = id;
    }
    if (win->shm_id != 0) {
        uint32_t id = alya_gui_new_id(win);
        uint32_t extra[2] = {1, id};
        if (alya_gui_req_str(win, win->registry_id, 0, "wl_shm", extra, 2) < 0) {
            return -1;
        }
        win->shm_id = id;
    }
    if (win->xdg_base_id != 0) {
        uint32_t id = alya_gui_new_id(win);
        uint32_t extra[2] = {1, id};
        if (alya_gui_req_str(win, win->registry_id, 0, "xdg_wm_base", extra, 2) < 0) {
            return -1;
        }
        win->xdg_base_id = id;
    }
    if (win->seat_id != 0) {
        uint32_t id = alya_gui_new_id(win);
        uint32_t extra[2] = {5, id};
        if (alya_gui_req_str(win, win->registry_id, 0, "wl_seat", extra, 2) < 0) {
            return -1;
        }
        win->seat_id = id;
    }
    return 0;
}

// Attaches a solid-color shm buffer so the mapped surface has content
// (XRGB8888 via POSIX shm_open; no _GNU_SOURCE needed).
static int alya_gui_attach_solid(alya_gui_window_t *win, uint32_t color) {
    char shm_name[64];
    int fd;
    size_t stride;
    size_t size;
    uint8_t *data;
    size_t i, count;

    if (win->shm_id == 0 || win->width <= 0 || win->height <= 0) {
        return -1;
    }
    stride = (size_t)win->width * 4;
    size = stride * (size_t)win->height;
    snprintf(shm_name, sizeof(shm_name), "/alya-gui-%d-%u", (int)getpid(),
             win->surface_id);
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
    win->buffer_id = alya_gui_new_id(win);
    {
        uint8_t msg[16];
        struct msghdr mh;
        struct iovec iov;
        char cmsg_buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr *cmsg;
        alya_wr32(msg, win->shm_id);
        alya_wr32(msg + 4, (16 << 16) | 0);
        alya_wr32(msg + 8, win->buffer_id);
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
        if (sendmsg(win->fd, &mh, MSG_NOSIGNAL) < 0) {
            munmap(data, size);
            close(fd);
            return -1;
        }
    }
    close(fd);
    win->shm_data = data;
    win->shm_size = size;
    // wl_shm_pool.create_buffer(new_id, offset, w, h, stride, XRGB8888=1).
    {
        uint32_t buf_id = alya_gui_new_id(win);
        uint8_t bmsg[8 + 6 * 4];
        alya_wr32(bmsg, win->buffer_id);
        alya_wr32(bmsg + 4, ((8 + 6 * 4) << 16) | 0);
        alya_wr32(bmsg + 8, buf_id);
        alya_wr32(bmsg + 12, 0);
        alya_wr32(bmsg + 16, (uint32_t)win->width);
        alya_wr32(bmsg + 20, (uint32_t)win->height);
        alya_wr32(bmsg + 24, (uint32_t)stride);
        alya_wr32(bmsg + 28, 1);
        if (alya_gui_send(win, bmsg, sizeof(bmsg)) < 0) {
            return -1;
        }
        win->buffer_id = buf_id;
    }
    // wl_surface.attach(buffer, 0, 0) + damage(full) — commit follows.
    {
        uint32_t aargs[3];
        uint32_t dargs[4];
        aargs[0] = win->buffer_id;
        aargs[1] = 0;
        aargs[2] = 0;
        if (alya_gui_req(win, win->surface_id, 1, aargs, 3) < 0) {
            return -1;
        }
        dargs[0] = 0;
        dargs[1] = 0;
        dargs[2] = (uint32_t)win->width;
        dargs[3] = (uint32_t)win->height;
        alya_gui_req(win, win->surface_id, 2, dargs, 4);
    }
    return 0;
}

static int alya_gui_make_surface(alya_gui_window_t *win, const char *title) {
    uint32_t args[3];
    // wl_compositor.create_surface -> surface.
    win->surface_id = alya_gui_new_id(win);
    args[0] = win->surface_id;
    if (alya_gui_req(win, win->compositor_id, 0, args, 1) < 0) {
        return -1;
    }
    // xdg_wm_base.get_xdg_surface(surface) -> xdg_surface.
    win->xdg_surface_id = alya_gui_new_id(win);
    args[0] = win->surface_id;
    args[1] = win->xdg_surface_id;
    if (alya_gui_req(win, win->xdg_base_id, 2, args, 2) < 0) {
        return -1;
    }
    // xdg_surface.get_toplevel -> toplevel.
    win->toplevel_id = alya_gui_new_id(win);
    args[0] = win->toplevel_id;
    if (alya_gui_req(win, win->xdg_surface_id, 1, args, 1) < 0) {
        return -1;
    }
    // xdg_toplevel.set_title + set_app_id.
    if (alya_gui_req_str(win, win->toplevel_id, 2, title, NULL, 0) < 0) {
        return -1;
    }
    if (alya_gui_req_str(win, win->toplevel_id, 3, "alya-gui", NULL, 0) < 0) {
        return -1;
    }
    // Attach a dark solid buffer so the surface has visible content, then
    // commit so the compositor maps it.
    alya_gui_attach_solid(win, 0xFF181825);
    if (alya_gui_req(win, win->surface_id, 6, NULL, 0) < 0) {
        return -1;
    }
    return 0;
}

alya_gui_window_t *alya_gui_window_create(const char *title, int32_t width,
                                          int32_t height) {
    alya_gui_window_t *win;
    if (width <= 0 || height <= 0) {
        return NULL;
    }
    win = (alya_gui_window_t *)calloc(1, sizeof(*win));
    if (win == NULL) {
        return NULL;
    }
    win->fd = -1;
    win->display_id = 1;
    win->next_id = 2;
    win->open = 1;
    win->width = width;
    win->height = height;
    win->shm_pool_fd = 0;
    if (alya_gui_connect(win) < 0) {
        free(win);
        return NULL;
    }
    // wl_display.get_registry -> registry id 2.
    win->registry_id = alya_gui_new_id(win);
    {
        uint32_t args[1];
        args[0] = win->registry_id;
        if (alya_gui_req(win, win->display_id, 1, args, 1) < 0) {
            close(win->fd);
            free(win);
            return NULL;
        }
    }
    if (alya_gui_roundtrip(win) < 0) {
        close(win->fd);
        free(win);
        return NULL;
    }
    if (win->compositor_id == 0 || win->xdg_base_id == 0) {
        close(win->fd);
        free(win);
        return NULL;
    }
    if (alya_gui_bind_globals(win) < 0) {
        close(win->fd);
        free(win);
        return NULL;
    }
    if (alya_gui_make_surface(win, title) < 0) {
        close(win->fd);
        free(win);
        return NULL;
    }
    if (alya_gui_roundtrip(win) < 0) {
        close(win->fd);
        free(win);
        return NULL;
    }
    return win;
}

void alya_gui_window_destroy(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    if (win->fd >= 0) {
        close(win->fd);
    }
    if (win->shm_data != NULL) {
        munmap(win->shm_data, win->shm_size);
    }
    if (win->shm_pool_fd != 0) {
        close((int)win->shm_pool_fd);
    }
    free(win);
}

void alya_gui_window_show(alya_gui_window_t *win) {
    uint32_t args[1];
    if (win == NULL) {
        return;
    }
    // (Re)commit so a hidden-then-shown surface remaps.
    args[0] = 0;
    (void)args;
    alya_gui_req(win, win->surface_id, 6, NULL, 0);
}

void alya_gui_window_hide(alya_gui_window_t *win) {
    if (win == NULL) {
        return;
    }
    // Detach buffer + commit hides the surface content.
    {
        uint32_t args[3] = {0, 0, 0};
        alya_gui_req(win, win->surface_id, 1, args, 3);
        alya_gui_req(win, win->surface_id, 6, NULL, 0);
    }
}

int32_t alya_gui_window_is_open(alya_gui_window_t *win) {
    if (win == NULL) {
        return 0;
    }
    return win->open;
}

int32_t alya_gui_window_poll(alya_gui_window_t *win, alya_gui_event_t *out) {
    if (win == NULL || out == NULL) {
        return 0;
    }
    alya_gui_dispatch(win, 0);
    if (win->head == win->tail) {
        return 0;
    }
    *out = win->queue[win->head];
    win->head = (win->head + 1) % ALYA_GUI_MAX_EVENTS;
    return 1;
}

void alya_gui_window_set_title(alya_gui_window_t *win, const char *title) {
    if (win == NULL) {
        return;
    }
    alya_gui_req_str(win, win->toplevel_id, 2, title, NULL, 0);
}

void alya_gui_window_set_size(alya_gui_window_t *win, int32_t width,
                              int32_t height) {
    uint32_t args[4];
    if (win == NULL || width <= 0 || height <= 0) {
        return;
    }
    // Hint only: xdg-shell leaves sizing to the compositor, which answers
    // with a configure event carrying the real size.
    // xdg_surface.set_window_geometry(x, y, w, h) is opcode 3.
    args[0] = 0;
    args[1] = 0;
    args[2] = (uint32_t)width;
    args[3] = (uint32_t)height;
    alya_gui_req(win, win->xdg_surface_id, 3, args, 4);
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
    if (win == NULL) {
        return;
    }
    // No protocol-level close request exists; mark locally so is_open
    // flips and a CLOSE event is observable by the pump.
    win->open = 0;
    alya_gui_push_event(win, ALYA_GUI_EVENT_CLOSE);
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
