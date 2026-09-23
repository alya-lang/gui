#ifndef ALYA_GUI_A11Y_CONTRACT_H
#define ALYA_GUI_A11Y_CONTRACT_H

// Accessibility contract for the Alya GUI toolkit (Phase 6).
//
// Screen-reader backends consume the accessible tree exported by
// `a11y_export` (Alya side). Role codes match `AccessibleRole`
// (0=Unknown, 1=Window, 2=Button, 3=Label, 4=TextField, 5=Checkbox,
// 6=RadioButton, 7=Slider, 8=ProgressBar, 9=Dropdown, 10=List,
// 11=Container, 12=Canvas).
//
// Binding shape (scalar FFI: Alya arrays do not marshal to C pointers,
// so the tree crosses as events, not structs):
//
//   int32_t alya_gui_a11y_notify(win, code) — 1 = focus, 2 = value,
//   3 = selection, 4 = state. Windows raises WinEvents (UIA/MSAA),
//   macOS posts NSAccessibility notifications, Linux returns 0
//   (AT-SPI needs the session-bus registry + provider tree: follow-up).
//
// A full provider tree (`publish` wholesale) wants struct-array FFI
// from the compiler first; until then notifications stay window-level.

#include <stdint.h>

typedef struct alya_gui_window alya_gui_window_t;

#ifdef __cplusplus
extern "C" {
#endif

int32_t alya_gui_a11y_notify(alya_gui_window_t *win, int32_t code);

#ifdef __cplusplus
}
#endif

#endif
