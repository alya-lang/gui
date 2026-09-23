#ifndef ALYA_GUI_A11Y_CONTRACT_H
#define ALYA_GUI_A11Y_CONTRACT_H

// Accessibility contract for the Alya GUI toolkit (Phase 6).
//
// Screen-reader backends (UI Automation / MSAA on Windows, NSAccessibility
// on macOS, AT-SPI2 on Linux) consume the accessible tree exported by
// `a11y_export` (Alya side). Each backend implements THESE functions with C
// linkage, translating tree records into native accessibility objects:
//
//   record: { id: string, role: int, label: string,
//             x, y, w, h: int (screen coordinates) }
//
// Role codes match `AccessibleRole` (0=Unknown, 1=Window, 2=Button,
// 3=Label, 4=TextField, 5=Checkbox, 6=RadioButton, 7=Slider, 8=ProgressBar,
// 9=Dropdown, 10=List, 11=Container, 12=Canvas).

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Replaces the exposed tree wholesale. `records` is an array of record
// structs; backends copy what they need and return 1 on success.
typedef struct alya_a11y_record {
    const char *id;
    int32_t role;
    const char *label;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} alya_a11y_record_t;

int32_t alya_a11y_publish(const alya_a11y_record_t *records, int32_t count);

// Moves screen-reader focus to the record with `id`. Returns 1 when the
// backend accepted the request.
int32_t alya_a11y_focus(const char *id);

// Withdraws the whole tree (window teardown). Returns 1.
int32_t alya_a11y_clear(void);

#ifdef __cplusplus
}
#endif

#endif
