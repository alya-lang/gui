#ifndef ALYA_GUI_GPU_CONTRACT_H
#define ALYA_GUI_GPU_CONTRACT_H

// GPU surface contract for the Alya GUI toolkit (Phase 2).
//
// Each GPU backend (Direct2D on Windows, CoreGraphics-composited on macOS,
// wl_shm/XPutImage on Linux) implements THESE SIX functions with C linkage.
// The Alya side stages pixels one by one (`stage`) and uploads + swaps on
// `present`; the portable shape lives in `src/canvas/surface.alya`.
// Pixel layout contract: 32-bit XRGB, row-major, top-down.
//
// NOTE: staging is scalar (one FFI call per pixel) because Alya arrays do
// not marshal to C pointers today. A bulk-upload entry point wants compiler
// FFI support first; until then this path suits small surfaces and demos,
// and the software rasterizer stays the throughput path.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque GPU surface bound to one native window (see c/*_window.h).
typedef struct alya_gpu_surface alya_gpu_surface_t;

// Creates a GPU surface for `native_win` (an `alya_gui_window_t *`).
// Returns NULL when GPU init fails (caller falls back to software).
alya_gpu_surface_t *alya_gpu_surface_create(void *native_win, int32_t width,
                                            int32_t height);

// Destroys the surface. Safe on NULL.
void alya_gpu_surface_destroy(alya_gpu_surface_t *surf);

// Stages one XRGB pixel (`color & 0xFFFFFF`) at flat `index`
// (`y * width + x`). Out-of-range writes are ignored. Returns 1 when
// stored, 0 on NULL surface.
int32_t alya_gpu_surface_stage(alya_gpu_surface_t *surf, int32_t index,
                               int32_t color);

// Uploads the staged buffer and presents it on the bound window.
// Returns 1 on success, 0 when no content reached the screen.
int32_t alya_gpu_surface_present(alya_gpu_surface_t *surf);

// Recreates staging/swap resources after a resize (content is reset).
// Returns 1 on success.
int32_t alya_gpu_surface_resize(alya_gpu_surface_t *surf, int32_t width,
                                int32_t height);

#ifdef __cplusplus
}
#endif

#endif
