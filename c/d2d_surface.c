// Direct2D surface backend for the Alya GUI toolkit (Phase 2).
//
// Software-drawn XRGB pixels are staged per pixel from Alya, then uploaded
// to an ID2D1Bitmap and drawn through an HWND render target: real GPU
// composition with zero SDK headers. Direct2D COM interfaces are declared
// manually (vtable order matches d2d1.h) so this file compiles on any host;
// it links (-ld2d1) and runs on Windows only. Non-Windows builds get NULL
// stubs so the shared contract always links.

#include "gpu_contract.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// --- Minimal D2D declarations (ABI matches d2d1.h / dxgiformat.h) ---

typedef long HRESULT;
typedef unsigned long ULONG;
#ifndef S_OK
#define S_OK ((HRESULT)0)
#endif

typedef struct {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
} ALYA_GUID;

static const ALYA_GUID ALYA_IID_ID2D1Factory = {
    0x06152247, 0x6f50, 0x465a, {0x92, 0x45, 0x11, 0x8b, 0xfd, 0x3b, 0x60, 0x07}};

#define ALYA_D2D1_FACTORY_TYPE_SINGLE_THREADED 0
#define ALYA_DXGI_FORMAT_B8G8R8A8_UNORM 87
#define ALYA_D2D1_ALPHA_MODE_IGNORE 3

typedef struct {
    int format;
    int alphaMode;
} AlyaD2D1PixelFormat;

typedef struct {
    unsigned int width;
    unsigned int height;
} AlyaD2D1SizeU;

typedef struct {
    int type;
    AlyaD2D1PixelFormat pixelFormat;
    float dpiX;
    float dpiY;
    int usage;
    int minLevel;
} AlyaD2D1RenderTargetProps;

typedef struct {
    HWND hwnd;
    AlyaD2D1SizeU pixelSize;
    int presentOptions;
} AlyaD2D1HwndRenderTargetProps;

typedef struct {
    AlyaD2D1PixelFormat pixelFormat;
    float dpiX;
    float dpiY;
} AlyaD2D1BitmapProps;

typedef struct {
    float left;
    float top;
    float right;
    float bottom;
} AlyaD2D1RectF;

typedef struct AlyaD2D1BitmapVtbl AlyaD2D1BitmapVtbl;
typedef struct {
    const AlyaD2D1BitmapVtbl *lpVtbl;
} AlyaD2D1Bitmap;

struct AlyaD2D1BitmapVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(AlyaD2D1Bitmap *, const void *, void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(AlyaD2D1Bitmap *);
    ULONG(STDMETHODCALLTYPE *Release)(AlyaD2D1Bitmap *);
    void(STDMETHODCALLTYPE *GetFactory)(AlyaD2D1Bitmap *, void **);
    void *GetSize;
    void *GetPixelSize;
    void *GetPixelFormat;
    void *GetDpi;
    void *CopyFromBitmap;
    void *CopyFromRenderTarget;
    HRESULT(STDMETHODCALLTYPE *CopyFromMemory)(AlyaD2D1Bitmap *,
                                              const AlyaD2D1RectF *,
                                              const void *, unsigned int);
};

typedef struct AlyaD2D1RenderTargetVtbl AlyaD2D1RenderTargetVtbl;
typedef struct {
    const AlyaD2D1RenderTargetVtbl *lpVtbl;
} AlyaD2D1RenderTarget;

struct AlyaD2D1RenderTargetVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(AlyaD2D1RenderTarget *, const void *,
                                              void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(AlyaD2D1RenderTarget *);
    ULONG(STDMETHODCALLTYPE *Release)(AlyaD2D1RenderTarget *);
    void *GetFactory;
    HRESULT(STDMETHODCALLTYPE *CreateBitmap)(AlyaD2D1RenderTarget *,
                                            AlyaD2D1SizeU, const void *,
                                            unsigned int,
                                            const AlyaD2D1BitmapProps *,
                                            AlyaD2D1Bitmap **);
    void *CreateBitmapFromWicBitmap;
    void *CreateSharedBitmap;
    void *CreateBitmapBrush;
    HRESULT(STDMETHODCALLTYPE *CreateSolidColorBrush)(AlyaD2D1RenderTarget *,
                                                     const void *, const void *,
                                                     void **);
    void *CreateGradientStopCollection;
    void *CreateLinearGradientBrush;
    void *CreateRadialGradientBrush;
    void *CreateCompatibleRenderTarget;
    void *CreateLayer;
    void *CreateMesh;
    void *DrawLine;
    void *DrawRectangle;
    void *FillRectangle;
    void *DrawRoundedRectangle;
    void *FillRoundedRectangle;
    void *DrawEllipse;
    void *FillEllipse;
    void *DrawGeometry;
    void *FillGeometry;
    void *FillMesh;
    void *FillOpacityMask;
    void(STDMETHODCALLTYPE *DrawBitmap)(AlyaD2D1RenderTarget *,
                                       AlyaD2D1Bitmap *,
                                       const AlyaD2D1RectF *, float, int,
                                       const AlyaD2D1RectF *);
    void *DrawText;
    void(STDMETHODCALLTYPE *DrawTextLayout)(AlyaD2D1RenderTarget *, const void *,
                                           const void *, const void *, int);
    void *DrawGlyphRun;
    void *SetTransform;
    void *GetTransform;
    void *SetAntialiasMode;
    void *GetAntialiasMode;
    void *SetTextAntialiasMode;
    void *GetTextAntialiasMode;
    void *SetTextRenderingParams;
    void *GetTextRenderingParams;
    void *SetTags;
    void *GetTags;
    void *PushLayer;
    void *PopLayer;
    void *Flush;
    void *SaveDrawingState;
    void *RestoreDrawingState;
    void *PushAxisAlignedClip;
    void *PopAxisAlignedClip;
    void *Clear;
    void(STDMETHODCALLTYPE *BeginDraw)(AlyaD2D1RenderTarget *);
    HRESULT(STDMETHODCALLTYPE *EndDraw)(AlyaD2D1RenderTarget *, void *, void *);
    void *GetPixelFormat;
    void *SetDpi;
    void *GetDpi;
    void *GetSize;
    void *GetPixelSize;
    void *GetMaximumBitmapSize;
    void *IsSupported;
    // ID2D1HwndRenderTarget extension.
    void *CheckWindowState;
    HRESULT(STDMETHODCALLTYPE *Resize)(AlyaD2D1RenderTarget *,
                                      const AlyaD2D1SizeU *);
    void *GetHwnd;
};

typedef struct AlyaD2D1FactoryVtbl AlyaD2D1FactoryVtbl;
typedef struct {
    const AlyaD2D1FactoryVtbl *lpVtbl;
} AlyaD2D1Factory;

struct AlyaD2D1FactoryVtbl {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(AlyaD2D1Factory *, const void *,
                                              void **);
    ULONG(STDMETHODCALLTYPE *AddRef)(AlyaD2D1Factory *);
    ULONG(STDMETHODCALLTYPE *Release)(AlyaD2D1Factory *);
    void *ReloadSystemMetrics;
    void *GetDesktopDpi;
    void *CreateRectangleGeometry;
    void *CreateRoundedRectangleGeometry;
    void *CreateEllipseGeometry;
    void *CreateGeometryGroup;
    void *CreateTransformedGeometry;
    void *CreatePathGeometry;
    void *CreateStrokeStyle;
    void *CreateDrawingStateBlock;
    void *CreateWicBitmapRenderTarget;
    HRESULT(STDMETHODCALLTYPE *CreateHwndRenderTarget)(
        AlyaD2D1Factory *, const AlyaD2D1RenderTargetProps *,
        const AlyaD2D1HwndRenderTargetProps *, AlyaD2D1RenderTarget **);
    void *CreateDxgiSurfaceRenderTarget;
    void *CreateDCRenderTarget;
};

typedef HRESULT(STDMETHODCALLTYPE *AlyaD2D1CreateFactoryFn)(
    int, const ALYA_GUID *, const void *, void **);

// --- Text via classic GDI (TextOutW): zero COM risk, ClearType by
// default, available on every Windows. Drawn over the last presented
// bitmap as an overlay (DirectWrite child objects faulted on this
// machine's dwrite.dll, so DWrite stays out until that is understood).

typedef struct {
    float x;
    float y;
} AlyaD2D1PointF;

typedef struct {
    float r;
    float g;
    float b;
    float a;
} AlyaD2D1ColorF;

struct alya_gpu_surface {
    AlyaD2D1Factory *factory;
    AlyaD2D1RenderTarget *target;
    AlyaD2D1Bitmap *bitmap;
    uint32_t *staging;
    int32_t width;
    int32_t height;
    HWND hwnd;
};

extern void *alya_gui_window_native_handle(void *win);

static void alya_d2d_release_target(alya_gpu_surface_t *surf) {
    if (surf->bitmap != NULL) {
        surf->bitmap->lpVtbl->Release(surf->bitmap);
        surf->bitmap = NULL;
    }
    if (surf->target != NULL) {
        surf->target->lpVtbl->Release(surf->target);
        surf->target = NULL;
    }
}

alya_gpu_surface_t *alya_gpu_surface_create(void *native_win, int32_t width,
                                            int32_t height) {
    alya_gpu_surface_t *surf;
    HWND hwnd;
    HMODULE d2d;
    AlyaD2D1CreateFactoryFn create_factory;
    AlyaD2D1RenderTargetProps rt_props;
    AlyaD2D1HwndRenderTargetProps hwnd_props;

    if (native_win == NULL || width <= 0 || height <= 0) {
        return NULL;
    }
    hwnd = (HWND)alya_gui_window_native_handle(native_win);
    if (hwnd == NULL) {
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
    surf->width = width;
    surf->height = height;
    surf->hwnd = hwnd;

    d2d = LoadLibraryW(L"d2d1.dll");
    if (d2d == NULL) {
        free(surf->staging);
        free(surf);
        return NULL;
    }
    create_factory = (AlyaD2D1CreateFactoryFn)(void *)GetProcAddress(d2d, "D2D1CreateFactory");
    if (create_factory == NULL) {
        free(surf->staging);
        free(surf);
        return NULL;
    }
    if (create_factory(ALYA_D2D1_FACTORY_TYPE_SINGLE_THREADED,
                       &ALYA_IID_ID2D1Factory, NULL,
                       (void **)&surf->factory) < 0 ||
        surf->factory == NULL) {
        free(surf->staging);
        free(surf);
        return NULL;
    }
    memset(&rt_props, 0, sizeof(rt_props));
    rt_props.pixelFormat.format = ALYA_DXGI_FORMAT_B8G8R8A8_UNORM;
    rt_props.pixelFormat.alphaMode = ALYA_D2D1_ALPHA_MODE_IGNORE;
    memset(&hwnd_props, 0, sizeof(hwnd_props));
    hwnd_props.hwnd = hwnd;
    hwnd_props.pixelSize.width = (unsigned int)width;
    hwnd_props.pixelSize.height = (unsigned int)height;
    if (surf->factory->lpVtbl->CreateHwndRenderTarget(
            surf->factory, &rt_props, &hwnd_props, &surf->target) < 0 ||
        surf->target == NULL) {
        surf->factory->lpVtbl->Release(surf->factory);
        free(surf->staging);
        free(surf);
        return NULL;
    }
    return surf;
}

void alya_gpu_surface_destroy(alya_gpu_surface_t *surf) {
    if (surf == NULL) {
        return;
    }
    alya_d2d_release_target(surf);
    if (surf->factory != NULL) {
        surf->factory->lpVtbl->Release(surf->factory);
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

int32_t alya_gpu_surface_present(alya_gpu_surface_t *surf) {
    AlyaD2D1SizeU size;
    AlyaD2D1BitmapProps bmp_props;
    HRESULT hr;
    if (surf == NULL || surf->target == NULL || surf->staging == NULL) {
        return 0;
    }
    if (surf->bitmap == NULL) {
        size.width = (unsigned int)surf->width;
        size.height = (unsigned int)surf->height;
        memset(&bmp_props, 0, sizeof(bmp_props));
        bmp_props.pixelFormat.format = ALYA_DXGI_FORMAT_B8G8R8A8_UNORM;
        bmp_props.pixelFormat.alphaMode = ALYA_D2D1_ALPHA_MODE_IGNORE;
        hr = surf->target->lpVtbl->CreateBitmap(
            surf->target, size, surf->staging,
            (unsigned int)surf->width * 4, &bmp_props, &surf->bitmap);
        if (hr < 0 || surf->bitmap == NULL) {
            return 0;
        }
    } else {
        hr = surf->bitmap->lpVtbl->CopyFromMemory(
            surf->bitmap, NULL, surf->staging,
            (unsigned int)surf->width * 4);
        if (hr < 0) {
            return 0;
        }
    }
    surf->target->lpVtbl->BeginDraw(surf->target);
    surf->target->lpVtbl->DrawBitmap(surf->target, surf->bitmap, NULL, 1.0f, 1,
                                     NULL);
    hr = surf->target->lpVtbl->EndDraw(surf->target, NULL, NULL);
    return hr >= 0 ? 1 : 0;
}

// Draws UTF-8 text over the window via GDI (overlaying the last presented
// bitmap). Returns 1 when the text was drawn.
int32_t alya_gpu_surface_text(alya_gpu_surface_t *surf, const char *utf8,
                              int32_t x, int32_t y, int32_t size_px,
                              int32_t color) {
    HDC hdc;
    wchar_t wtext[128];
    int wlen;
    HFONT font;
    HGDIOBJ old;
    unsigned long col;
    int ok;
    if (surf == NULL || surf->hwnd == NULL || utf8 == NULL ||
        utf8[0] == '\0' || size_px <= 0) {
        return 0;
    }
    wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wtext, 127);
    if (wlen <= 1) {
        return 0;
    }
    wlen -= 1; // exclude the NUL from the drawn length
    hdc = GetDC(surf->hwnd);
    if (hdc == NULL) {
        return 0;
    }
    // Negative height = character height; CLEARTYPE_QUALITY = 5.
    font = CreateFontW(-size_px, 0, 0, 0, 400, 0, 0, 0, 1, 0, 0, 5, 0,
                       L"Segoe UI");
    if (font == NULL) {
        ReleaseDC(surf->hwnd, hdc);
        return 0;
    }
    // COLORREF is 0x00BBGGRR; our color is 0xRRGGBB.
    col = ((unsigned long)(color & 0xFF) << 16) |
          (unsigned long)(color & 0xFF00) |
          ((unsigned long)((color >> 16) & 0xFF));
    SetBkMode(hdc, 1); // TRANSPARENT
    SetTextColor(hdc, col);
    old = SelectObject(hdc, font);
    ok = TextOutW(hdc, (int)x, (int)y, wtext, (unsigned)wlen);
    SelectObject(hdc, old);
    DeleteObject(font);
    ReleaseDC(surf->hwnd, hdc);
    return ok ? 1 : 0;
}

int32_t alya_gpu_surface_resize(alya_gpu_surface_t *surf, int32_t width,
                                int32_t height) {
    uint32_t *staging;
    AlyaD2D1SizeU size;
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
    if (surf->bitmap != NULL) {
        surf->bitmap->lpVtbl->Release(surf->bitmap);
        surf->bitmap = NULL;
    }
    if (surf->target != NULL) {
        size.width = (unsigned int)width;
        size.height = (unsigned int)height;
        if (surf->target->lpVtbl->Resize(surf->target, &size) < 0) {
            return 0;
        }
    }
    return 1;
}

#else // non-Windows: NULL stubs so the contract always links.

#include <stddef.h>

struct alya_gpu_surface {
    int unused;
};

alya_gpu_surface_t *alya_gpu_surface_create(void *native_win, int32_t width,
                                            int32_t height) {
    (void)native_win;
    (void)width;
    (void)height;
    return NULL;
}

void alya_gpu_surface_destroy(alya_gpu_surface_t *surf) {
    (void)surf;
}

int32_t alya_gpu_surface_stage(alya_gpu_surface_t *surf, int32_t index,
                               int32_t color) {
    (void)surf;
    (void)index;
    (void)color;
    return 0;
}

int32_t alya_gpu_surface_present(alya_gpu_surface_t *surf) {
    (void)surf;
    return 0;
}

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
    (void)surf;
    (void)width;
    (void)height;
    return 0;
}

#endif
