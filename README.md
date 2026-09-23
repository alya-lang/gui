# gui

[![CI](https://github.com/alya-lang/gui/actions/workflows/ci.yml/badge.svg)](https://github.com/alya-lang/gui/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/alya-lang/gui?color=blue&label=License)](LICENSE)
[![Alya](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fgui%2Fmain%2Falya.toml&query=%24.package.alya-version&label=Alya&color=orange&prefix=%3E%3D)](https://github.com/alya-lang/alya)
[![Package Version](https://img.shields.io/badge/dynamic/toml?url=https%3A%2F%2Fraw.githubusercontent.com%2Falya-lang%2Fgui%2Fmain%2Falya.toml&query=%24.package.version&label=Version&color=brightgreen)](alya.toml)

Native cross-platform GUI toolkit: windowing, widgets, layout, and 2D canvas for Alya

---

## 🌟 Features

- ⚡ **Headless-Testable Foundation**: Geometry, backend detection, and event queue run without a display — full CI coverage on every runner
- 🧩 **Modular Architecture**: Clean public facade (`src/lib.alya`), domain models (`src/types.alya`), and focused core modules (`src/core/geometry.alya`, `src/core/backend.alya`, `src/core/events.alya`)
- 🖥️ **Native Backend Selection**: Host-aware HAL routing — Win32, Cocoa, Wayland/X11, or `unknown` on headless machines
- 🔒 **Public/Private Visibility (`pub`)**: Fine-grained export control with `pub` for public functions, structs, and enums, keeping internal helper functions private and encapsulated
- 📐 **Integer Geometry Primitives**: Rect overlap/union/inset math for layout, hit-testing, and damage regions
- 📬 **Portable Event Queue**: FIFO `GuiEvent` pump (mouse, keyboard, resize, close) ready for native OS loop integration
- 🧪 **Enterprise Test & Benchmark Suite**: Headless test coverage with standard assertions (`std/test`) and micro-benchmarking (`std/test` bench runner)

---

## 📁 Project Architecture

```
gui/
├── .alyalint               # Linter configuration (rules, exclusions, severity overrides)
├── .editorconfig           # Uniform formatting rules across IDEs and editors
├── .gitignore              # Ecosystem standard ignore filters
├── .vscode/                # VS Code workspace settings, DAP launch configurations & tasks
├── alya.toml               # Package manifest with dependencies and optional [build]
├── c/                      # (Optional) Native C sources for zero-dependency FFI packages
├── src/
│   ├── lib.alya            # Public API facade (pub exports, re-exports & pipeline runners)
│   ├── types.alya          # Data models, pub enums, pub structs, and struct methods
│   ├── ffi.alya            # (Optional) Native extern "C" declarations
│   └── core/               # Subdirectory module hierarchy
│       ├── geometry.alya   # Headless rect math (overlap, union, inset)
│       ├── backend.alya    # Host backend detection (windows/macos/wayland/x11)
│       └── events.alya     # Portable FIFO GUI event queue
├── examples/
│   └── demo.alya           # Comprehensive runnable walkthrough of all package capabilities
├── tests/
│   └── test_basic.alya     # Automated test suite with 100% feature coverage
└── benches/
    └── bench_basic.alya    # Micro-benchmarks measuring performance and throughput
```

> [!NOTE]
> **Visibility & Modularity:** Symbols annotated with `pub` (`pub function`, `pub struct`, `pub enum`, `pub interface`) are exported to external consumers and re-exporting modules. Symbols without `pub` remain strictly internal to their declaring module, preventing symbol collisions and implementation leakage.

---

## 📦 Installation

Add `gui` to the `[dependencies]` section in your `alya.toml`:

```toml
[dependencies]
gui = { git = "https://github.com/alya-lang/gui", branch = "main" }
```

Or install it directly using the Alya package CLI:

```bash
alya add gui --git https://github.com/alya-lang/gui --branch main
alya install
```

---

## 🚀 Quick Start

```alya
import "gui" as pkg

function main()
    # 1. Backend detection (headless-safe)
    let info = pkg::backend_details()
    say f"Backend: {info.backend} ({info.os})"

    # 2. Geometry: overlap of two windows
    let a = pkg::rect(10, 10, 100, 50)
    let b = pkg::rect(50, 30, 100, 100)
    say f"Overlap area: {pkg::overlap_area(a, b)}"

    # 3. Event queue: feed mouse + close, drain in order
    let q = pkg::event_queue()
    pkg::event_queue_push(q, pkg::event_new(pkg::GuiEventKind.MouseDown, 5, 6))
    let ev: GuiEvent = pkg::event_queue_poll(q)
    say f"Polled kind: {ev.kind}"

    # 4. Layout + retained widgets (headless)
    let rows: Rect[] = pkg::vbox(pkg::rect(0, 0, 200, 200), 10, 5, [pkg::size(0, 30)])
    let r0: Rect = rows[0]
    say f"Row0: ({r0.x}, {r0.y}, {r0.w}, {r0.h})"
    let root = pkg::widget("box", "root")
    pkg::widget_set_rect(root, pkg::rect(0, 0, 200, 200))
    let btn = pkg::button("ok", "OK")
    pkg::widget_set_rect(btn, pkg::rect(10, 10, 80, 30))
    pkg::widget_add_child(root, btn)
    say f"Clicked: {pkg::click(root, 20, 20)}"
end

main()
```

---

## 📖 API Reference

| Symbol | Visibility | Description |
|---|---|---|
| `window(title, width, height)` | `pub function` | Validated `WindowConfig` factory (falls back to `640x480` / `"Alya"`). |
| `backend()` | `pub function` | Backend name for the host (`"windows"`, `"macos"`, `"wayland"`, `"x11"`, `"unknown"`). |
| `backend_details()` | `pub function` | `BackendInfo` record (backend, os, display endpoint). |
| `event_queue()` | `pub function` | Empty FIFO `GuiEventQueue`. |
| `event_new(kind, x, y, key, text)` | `pub function` | `GuiEvent` record constructor. |
| `event_queue_push(q, ev)` | `pub function` | Appends an event; returns pending count. |
| `event_queue_poll(q)` | `pub function` | Removes and returns the next event (null when empty). |
| `event_queue_peek(q)` | `pub function` | Returns the next event without consuming it. |
| `event_queue_len(q)` | `pub function` | Pending event count. |
| `event_queue_clear(q)` | `pub function` | Drops all pending events. |
| `rect(x, y, w, h)` | `pub function` | `Rect` constructor. |
| `rect_intersect(a, b)` | `pub function` | Overlap region (empty rect when disjoint). |
| `rect_union(a, b)` | `pub function` | Smallest enclosing rect. |
| `rect_inset(r, dx, dy)` | `pub function` | Shrinks the rect on every side. |
| `overlap_area(a, b)` | `pub function` | Overlap area of two rects (`0` when disjoint). |
| `rgb(r, g, b)` / `rgba(r, g, b, a)` | `pub function` | Opaque / transparent `Color` constructors. |
| `vbox(container, padding, spacing, sizes)` | `pub function` | Stacks `Size[]` top-down; returns positioned `Rect[]`. |
| `hbox(container, padding, spacing, sizes)` | `pub function` | Stacks `Size[]` left-to-right; returns positioned `Rect[]`. |
| `widget(kind, id)` | `pub function` | Generic retained widget node. |
| `button(id, text)` / `label(id, text)` | `pub function` | Button and static label constructors. |
| `textinput(id, text)` / `checkbox(id, checked)` | `pub function` | Text input and checkbox constructors. |
| `click(root, x, y)` | `pub function` | Point-click dispatch over a widget tree (`1` consumed, `0` miss). |
| `widget_add_child(parent, child)` | `pub function` | Appends a child; returns child count. |
| `widget_find(root, id)` | `pub function` | Depth-first lookup by id (null when missing). |
| `widget_hit(root, x, y)` | `pub function` | Deepest visible node containing the point. |
| `c_add(a, b)` | `pub function` | Bundled C engine smoke test via FFI. |
| `Widget` | `pub struct` | Retained node (`id`, `kind`, `rect`, `visible`, `enabled`, `text`, `value`, `children`, callbacks). |
| `GuiBackend` | `pub enum` | Backend codes (`Unknown = 0`, `Windows = 1`, `MacOs = 2`, `Wayland = 3`, `X11 = 4`). |
| `GuiEventKind` | `pub enum` | Event kinds (`Close = 1`, `MouseDown = 4`, `KeyDown = 6`, `TextInput = 8`, ...). |
| `Point` / `Size` / `Rect` / `Color` | `pub struct` | Geometry primitives with methods (`area()`, `is_empty()`, `contains()`, `to_string()`). |
| `WindowConfig` | `pub struct` | Window creation parameters (`title`, `width`, `height`). |
| `BackendInfo` | `pub struct` | Detection result (`backend`, `os`, `display`). |
| `GuiEvent` | `pub struct` | Event record (`kind`, `x`, `y`, `key`, `text`). |
| `GuiEventQueue` | `pub struct` | FIFO queue with polling cursor (`events`, `head`). |

> [!TIP]
> **Internal Helpers & Documentation:** Public symbols are documented with `##` Markdown docstrings, enabling automatic API documentation generation via `alya doc`.

---

## 🧪 Running Tests & Benchmarks

Run the automated test suite using `alya test`:

```bash
alya test
```

Generate static API documentation:

```bash
alya doc . -o docs --markdown
```

Run the benchmark suite:

```bash
alya run benches/bench_basic.alya
```

Run the example demo:

```bash
alya run examples/demo.alya
```

Check code formatting:

```bash
alya fmt . --check
```

Run static code linter:

```bash
alya lint . --check
```

---

### 💻 Developer Tooling & VS Code Integration

This package comes preconfigured with recommended workspace settings and tasks for **Visual Studio Code**:
- **LSP & Formatting**: Auto-formatting on save and real-time Language Server diagnostics via `alya-lang.vscode-alya`.
- **DAP Debugging**: Launch configurations in `.vscode/launch.json` ready for interactive step-debugging via `F5`.
- **Predefined Tasks**: Press `Ctrl+Shift+B` or run tasks (`Test`, `Lint`, `Format`, `Build Docs`) directly from the Command Palette.

---

## 🤝 Contributing

Contributions are welcome! Please follow these steps:

1. Fork the repository and clone it locally
2. Install dependencies:
   ```bash
   alya install
   ```
3. Create your feature branch (`git checkout -b feature/my-feature`)
4. Verify tests and formatting before opening a PR:
   ```bash
   alya test
   ```
5. Commit your changes (`git commit -m "feat: add feature"`) and open a Pull Request

---

## 📄 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.