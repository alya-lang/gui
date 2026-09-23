#ifndef ALYA_GUI_GPU_CONTRACT_H
#define ALYA_GUI_GPU_CONTRACT_H

// GPU surface contract for the Alya GUI toolkit (Phase 2).
//
// Each GPU backend (Direct2D on Windows, Metal on macOS, EGL on Linux)
// implements THESE FOUR functions with C linkage against its native window
// handle. The Alya side (`src/canvas/surface.alya`) keeps the portable
// shape; `surface_present` routes here on GPU builds, to the raster buffer
// otherwise. Pixel layout contract: 32-bit XRGB, row-major, top-down.

#include <stdint.h>

// Opaque GPU surface bound to one native window (see c/*_window.h).
typedef struct alya_gpu_surface alya_gpu_surface_t;

#ifdef __cplusplus
extern "C" {
#endif

// Creates a GPU surface for `native_win` (an `alya_gui_window_t *`).
// Returns NULL when GPU init fails (caller falls back to software).
alya_gpu_surface_t *alya_gpu_surface_create(void *native_win, int32_t width,
                                            int32_t height);

// Destroys the surface. Safe on NULL.
void alya_gpu_surface_destroy(alya_gpu_surface_t *surf);

// Uploads XRGB pixel rows and presents. `pixels` holds width*height
// entries; `stride` is bytes per row. Returns 1 on success.
int32_t alya_gpu_surface_present(alya_gpu_surface_t *surf,
                                 const uint32_t *pixels, int32_t width,
                                 int32_t height, int32_t stride);

// Recreates swap resources after a resize. Returns 1 on success.
int32_t alya_gpu_surface_resize(alya_gpu_surface_t *surf, int32_t width,
                                int32_t height);

#ifdef __cplusplus
}
#endif

#endif
