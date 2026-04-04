# gnome-milkdrop v2 — Agent Implementation Guide

**Status:** Approved — Ready to Implement  
**Target:** GNOME Shell 47 / 48 / 49, Wayland only  
**Sprint:** 2 days  
**Language:** C (binary) + JavaScript (GJS extension)

---

## Implementation Progress (2026-04-03)

The next-stage implementation for runtime reactivity is now in place with
test-backed coverage:

- Control protocol parser implemented and tested (`status`, `opacity`, `pause`,
    `shuffle`, `overlay`, `preset-dir`) with explicit invalid and incomplete
    command handling.
- Unix domain control socket implemented with listener lifecycle, threaded
    client handling, command responses, and cleanup semantics.
- Extension setting changes now route to control socket commands for runtime
    updates (`opacity`, `preset-dir`, `shuffle`, `overlay`) while `monitor`
    remains a restart-triggering setting.
- PipeWire audio backend now has a real capture path (when built with
    PipeWire support) feeding the lock-free ring buffer and preserving fallback
    behavior when initialization fails.
- Preset directory scanning is implemented with deterministic ordering of
    `.milk` files, runtime reload support from control commands, and cleanup.
- Renderer applies runtime opacity and shuffle state continuously on the main/
    render path without introducing per-frame IPC.

Test status:

- Full Meson test suite passes, including new `control-protocol` and
    `presets` targets.
- Backend tests now validate socket listen/connect behavior and command
    roundtrip state updates.

---

## 1. What This Builds

A MilkDrop-style audio visualizer for GNOME/Wayland. It renders full-screen
psychedelic visuals synchronized to system audio, using the MilkDrop preset
format (`.milk` files) that has been the standard for audio visualization since
Winamp.

The system has two components:

1. **`milkdrop` — a C binary** that captures system audio via PipeWire, feeds
   PCM samples to libprojectM, and renders the output into a plain GTK4 window.
2. **A GNOME Shell extension** that manages the binary's lifecycle (spawn, crash
   recovery, settings routing) and — critically — anchors the binary's window
   into the GNOME Shell compositor scene graph at the wallpaper layer, below all
   application windows.

There is no custom shader code, no expression engine, no FFT implementation.
libprojectM provides all of that. This project is the glue between the GNOME
compositor, PipeWire audio, and libprojectM.

**Why the extension must do compositor work:** on Wayland, window type hints
(`GDK_SURFACE_TYPE_HINT_DESKTOP`) are advisory. Mutter is not obligated to
honor them for z-ordering. The only reliable way to place content at the
wallpaper layer is to manipulate the GNOME Shell Clutter scene graph from
inside the extension process — exactly as Hanabi does for video wallpapers.

---

## 2. Architecture

### Process Model

```
┌──────────────────────────────────────────────────────────────────────┐
│                       milkdrop (C binary)                            │
│                                                                      │
│  PipeWire → Ring Buffer → projectm_pcm_add_float() → render_frame() │
│                                          ↓                           │
│                                    GtkGLArea                         │
│                                    (plain window, WM_CLASS=milkdrop) │
│                                                                      │
│  ┌─────────────────────────────────────────────────────────────────┐  │
│  │                    libprojectM 4.x                              │  │
│  │  .milk parser · expression engine · FFT · warp · composite      │  │
│  └─────────────────────────────────────────────────────────────────┘  │
│                                                                      │
│  ┌──────────────────────────┐   ┌──────────────────────────────────┐  │
│  │   GtkGLArea (window)     │   │   Unix socket (config only)      │  │
│  │   render signal → FBO    │   │   preset, opacity, pause         │  │
│  └──────────────────────────┘   └──────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
         ↑ spawn/kill/restart          ↑ detect window-created
         │                             │ anchor actor in Clutter graph
┌──────────────────────────────────────────────────────────────────────┐
│              GJS Extension (GNOME Shell process)                     │
│                                                                      │
│  GSettings → socket commands → exponential retry                     │
│                                                                      │
│  display.connect('window-created') → identify WM_CLASS=milkdrop      │
│  → reparent actor into global.window_group at z=bottom               │
│  → display.connect('restacked') → re-anchor on workspace switch      │
└──────────────────────────────────────────────────────────────────────┘
         │ lives inside
┌──────────────────────────────────────────────────────────────────────┐
│              GNOME Shell Clutter Scene Graph                         │
│  global.stage                                                        │
│  └── Main.layoutManager._backgroundGroup  ← static wallpaper        │
│  └── global.window_group                                             │
│      └── [milkdrop actor anchored HERE] ← wallpaper layer           │
│      └── all other app windows                                       │
│  └── Main.layoutManager.uiGroup          ← panel, shell UI          │
└──────────────────────────────────────────────────────────────────────┘
```

### Thread Model (inside the C binary)

| Thread | Responsibility | Blocks on |
|--------|---------------|-----------|
| Main / GL | GTK main loop, GtkGLArea render signal, projectM calls | vblank via GTK |
| PipeWire | Audio capture callback, ring buffer writes | PipeWire event loop |
| Control | Unix socket accept/read/write | `accept()` |

**Rule:** GL thread is the only thread that calls any projectM or OpenGL function.
The control thread communicates with the GL thread exclusively via atomic flags.
Never touch GL state from the control thread.

### Per-Frame Data Flow

```
PipeWire thread                        GL thread (render signal)
──────────────────                     ─────────────────────────
512 samples arrive                     ring_read() → float[N]
ring_push(left, right)                 projectm_pcm_add_float(pm, buf, N/2, STEREO)
  ~50ns, no lock                       check atomic preset_pending flag
                                       if set: projectm_load_preset_file()
                                       projectm_render_frame(pm)
                                         └─ projectM: FFT, eval, warp, composite
                                       glDisable(DEPTH_TEST, STENCIL_TEST)
                                       return TRUE
```

**Zero IPC during steady-state rendering.** The socket is only used for
user-triggered commands (preset switch, opacity change, pause).

---

## 3. Decisions Log

These decisions are final. Do not re-open them.

| Question | Decision | Reason |
|----------|----------|--------|
| libprojectM version | 4.x | Clean C API, available in all target distros, no C++ include archaeology |
| Windowing | GTK4 GtkGLArea | GNOME 48 dropped `wlr-layer-shell-v1`; GTK4 is fully supported on 47/48/49 |
| Z-order / wallpaper layer | Clutter scene graph manipulation from extension | `GDK_SURFACE_TYPE_HINT_DESKTOP` is advisory on Wayland; Mutter does not guarantee z-order. Only reliable method is reparenting the Clutter actor from inside the GNOME Shell process. Confirmed by Hanabi's implementation. |
| Linking | Dynamic | System component; users have apt/pacman/dnf. Static = stale projectM bundled. |
| Audio | PipeWire direct | GStreamer is 3 abstractions too many for a monitor capture stream |
| Expression engine | libprojectM (built-in) | Eliminating ~2000 lines of planned hand-rolled bytecode VM |
| FFT | libprojectM (built-in) | Eliminating kissfft dependency |
| Preset parsing | libprojectM (built-in) | Eliminating ~500 lines of planned `.milk` parser |
| Per-pixel equations | libprojectM GLSL (built-in) | GPU parallelism, zero implementation cost |
| Preset switch | `projectm_load_preset_file()` | ~50–150ms shader recompile stutter acceptable |
| HiDPI | Multiply window size by `gtk_widget_get_scale_factor()` | Required; forgetting causes blurry rendering |
| Preset shuffle | `projectm_set_preset_shuffle()` | Free from projectM, wire to GSettings |

---

## 4. Repository Layout

```
milkdrop/
├── meson.build
├── meson_options.txt
│
├── src/
│   ├── app.h               # Shared AppData struct — include everywhere
│   ├── main.c              # GTK4 app init, GtkGLArea, render/resize signals
│   ├── audio.c             # PipeWire capture + SPSC ring buffer
│   ├── audio.h
│   ├── control.c           # Unix socket command server
│   └── control.h
│
├── extension/
│   └── milkdrop@mauriciobc.github.io/
│       ├── metadata.json
│       ├── extension.js    # spawn/kill, settings, retry, compositor anchoring
│       └── prefs.js        # GNOME preferences UI
│
├── data/
│   └── org.gnome.shell.extensions.milkdrop.gschema.xml
│
└── tools/
    ├── install.sh
    ├── uninstall.sh
    └── reload.sh
```

Do not create `src/gl/`, `src/fft.c`, `src/expr.cpp`, or any vendor directories.
libprojectM replaces all of that.

---

## 5. Component Specifications

### 5.1 `src/app.h` — Shared State

The `AppData` struct is passed through all GTK callbacks and the control thread.
Define it once here, include it everywhere.

```c
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <projectM-4/projectM.h>
#include <gtk/gtk.h>

#define RING_SIZE  (8192 * 2)   // float count; must be power of 2
#define RING_MASK  (RING_SIZE - 1)
#define MAX_PRESETS 4096
#define PRESET_NAME_MAX 512
#define SOCK_PATH_MAX 256

typedef struct {
    // Audio ring buffer (SPSC, lock-free)
    _Atomic size_t write_idx;       // counts in floats
    _Atomic size_t read_idx;
    float          buffer[RING_SIZE];
} AudioRing;

typedef struct {
    // projectM handle — only touch from GL thread
    projectm_handle pm;

    // GTK
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *gl_area;

    // Audio
    AudioRing ring;
    // (pw_stream and pw_main_loop owned by audio.c, referenced via opaque ptr)
    void *pw_state;

    // Preset list
    char   preset_dir[PRESET_NAME_MAX];
    char **presets;         // NULL-terminated array of paths
    int    preset_count;
    int    preset_current;

    // Control thread → GL thread (atomic handoff, no mutex)
    _Atomic bool   preset_pending;
    char           next_preset[PRESET_NAME_MAX];   // written by control thread

    _Atomic bool   pause_pending;
    _Atomic bool   paused;

    _Atomic float  opacity_pending_value;
    _Atomic bool   opacity_pending;

    // Config
    char sock_path[SOCK_PATH_MAX];
    int  target_fps;

    // Metrics (written by GL thread, read by control thread for status)
    _Atomic float  current_fps;
} AppData;
```

---

### 5.2 `src/audio.c` + `src/audio.h` — PipeWire Capture

**Responsibility:** Open a PipeWire monitor capture stream. On each buffer
callback, write interleaved stereo float32 samples into `AppData.ring`.
Run the PipeWire event loop on its own thread.

**Interface:**

```c
// audio.h
#pragma once
#include "app.h"

// Start PipeWire capture. Spawns internal thread.
// Returns 0 on success, -1 on failure (logs reason to stderr).
int  audio_init(AppData *d);

// Stop capture and clean up. Blocks until thread exits.
void audio_cleanup(AppData *d);
```

**Ring buffer implementation** (define in `audio.c`, used inline):

```c
static inline bool ring_push(AudioRing *r, float left, float right) {
    size_t w    = atomic_load_explicit(&r->write_idx, memory_order_relaxed);
    size_t next = (w + 2) & RING_MASK;
    if (next == atomic_load_explicit(&r->read_idx, memory_order_acquire))
        return false;   // full — drop this pair
    r->buffer[w & RING_MASK]       = left;
    r->buffer[(w + 1) & RING_MASK] = right;
    atomic_store_explicit(&r->write_idx, next, memory_order_release);
    return true;
}

static inline size_t ring_read(AudioRing *r, float *dst, size_t max_floats) {
    size_t rd        = atomic_load_explicit(&r->read_idx,  memory_order_relaxed);
    size_t wr        = atomic_load_explicit(&r->write_idx, memory_order_acquire);
    size_t available = (wr - rd) & RING_MASK;
    size_t to_read   = (available < max_floats) ? available : max_floats;
    to_read         &= ~1u;   // round down to stereo pair boundary
    for (size_t i = 0; i < to_read; i++)
        dst[i] = r->buffer[(rd + i) & RING_MASK];
    atomic_store_explicit(&r->read_idx, (rd + to_read) & RING_MASK,
                          memory_order_release);
    return to_read;
}
```

**PipeWire stream callback:**

```c
static void on_process(void *userdata) {
    AppData           *d = userdata;
    struct pw_buffer  *b = pw_stream_dequeue_buffer(/* stream */);
    if (!b) return;

    struct spa_data *data     = &b->buffer->datas[0];
    float           *samples  = data->data;
    uint32_t         n_frames = data->chunk->size / (sizeof(float) * 2);

    for (uint32_t i = 0; i < n_frames; i++)
        ring_push(&d->ring, samples[i * 2], samples[i * 2 + 1]);

    pw_stream_queue_buffer(/* stream */, b);
}
```

**Fallback:** If PipeWire initialization fails (no monitor source available),
log the error to stderr and return -1. `main.c` proceeds with a silent ring
buffer — projectM renders without audio. Do not crash.

**Format negotiation:** Request `F32` (float32), stereo, 48000 Hz.
If the server negotiates a different sample rate, that is fine — projectM
accepts whatever sample rate is provided. Pass the actual rate to
`projectm_set_fps()` for BPM detection accuracy.

---

### 5.3 `src/main.c` — GTK4 App + GL Integration

**Responsibility:** Initialize the GTK4 application and GtkGLArea window.
Wire the `render` and `resize` signals to projectM. Run the GTK main loop.

#### Window Setup

```c
static void build_window(AppData *d) {
    d->window  = gtk_application_window_new(d->app);
    d->gl_area = gtk_gl_area_new();

    // Request GLES 3.0 — matches what projectM 4.x expects
    gtk_gl_area_set_use_es(GTK_GL_AREA(d->gl_area), TRUE);
    gtk_gl_area_set_required_version(GTK_GL_AREA(d->gl_area), 3, 0);

    gtk_window_set_decorated(GTK_WINDOW(d->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(d->window), 1920, 1080);

    // DO NOT set GDK_SURFACE_TYPE_HINT_DESKTOP here.
    // On Wayland, type hints are advisory and Mutter does not guarantee
    // z-ordering from them. Z-order is handled by the GJS extension via
    // Clutter scene graph manipulation (see §5.6).
    //
    // The extension identifies this window by WM_CLASS.
    // g_set_prgname() is called in main() before gtk_application_run();
    // GDK propagates it as WM_CLASS automatically.

    // Input passthrough: the visualizer should not capture clicks or focus.
    // The extension also disables input via MetaWindow, but set these too
    // as a belt-and-suspenders measure.
    gtk_widget_set_can_target(d->window, FALSE);
    gtk_widget_set_can_focus(d->window,  FALSE);

    gtk_window_set_child(GTK_WINDOW(d->window), d->gl_area);

    g_signal_connect(d->gl_area, "realize",  G_CALLBACK(on_realize),  d);
    g_signal_connect(d->gl_area, "render",   G_CALLBACK(on_render),   d);
    g_signal_connect(d->gl_area, "resize",   G_CALLBACK(on_resize),   d);
    g_signal_connect(d->gl_area, "unrealize",G_CALLBACK(on_unrealize), d);

    gtk_widget_set_visible(d->window, TRUE);
}
```

**In `main()`, before `gtk_application_run()`:**

```c
int main(int argc, char *argv[]) {
    // Set WM_CLASS so the extension can identify this window.
    // Must be called before any GDK/GTK initialization.
    g_set_prgname("milkdrop");

    // ... parse args, init AppData ...
    // ... audio_init(), control_init() ...
    // ... gtk_application_run() ...
}
```

#### `on_realize` — projectM Init

```c
static void on_realize(GtkGLArea *area, AppData *d) {
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) return;

    d->pm = projectm_create();
    if (!d->pm) {
        g_printerr("milkdrop: projectm_create() failed\n");
        return;
    }

    int w, h;
    gtk_gl_area_get_default_framebuffer_object(area); // ensure FBO exists
    w = gtk_widget_get_width(GTK_WIDGET(area));
    h = gtk_widget_get_height(GTK_WIDGET(area));

    // Account for HiDPI — projectM needs physical pixels
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    projectm_set_window_size(d->pm, (size_t)(w * scale), (size_t)(h * scale));
    projectm_set_fps(d->pm, d->target_fps);
    projectm_set_preset_shuffle(d->pm, TRUE);   // wire to GSettings later

    // Load first preset if available
    if (d->preset_count > 0)
        projectm_load_preset_file(d->pm, d->presets[d->preset_current], FALSE);
}
```

#### `on_render` — Per-Frame (Hot Path)

```c
static gboolean on_render(GtkGLArea *area, GdkGLContext *ctx, AppData *d) {
    if (!d->pm) return FALSE;

    // 1. Drain audio ring buffer and feed to projectM
    float pcm[4096];
    size_t n = ring_read(&d->ring, pcm, 4096);
    if (n > 0)
        projectm_pcm_add_float(d->pm, pcm, (uint32_t)(n / 2), PROJECTM_STEREO);

    // 2. Process pending commands from control thread
    if (atomic_exchange(&d->preset_pending, false))
        projectm_load_preset_file(d->pm, d->next_preset, FALSE);

    if (atomic_exchange(&d->pause_pending, false)) {
        bool paused = atomic_load(&d->paused);
        // projectM has no native pause; stop feeding PCM when paused
        // (already handled by not calling pcm_add above when paused flag set)
    }

    if (atomic_exchange(&d->opacity_pending, false)) {
        float op = atomic_load(&d->opacity_pending_value);
        gtk_widget_set_opacity(GTK_WIDGET(d->window), (double)op);
    }

    // 3. Render — projectM draws into GTK's currently bound FBO
    projectm_render_frame(d->pm);

    // 4. Restore GL state that GTK4 checks after render callback
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);

    // 5. Update FPS metric for status responses
    static gint64 last_time = 0;
    gint64 now = g_get_monotonic_time();
    if (last_time > 0) {
        float frame_ms = (float)(now - last_time) / 1000.0f;
        atomic_store(&d->current_fps, 1000.0f / frame_ms);
    }
    last_time = now;

    return TRUE;   // TRUE = we drew, don't clear
}
```

#### `on_resize` — HiDPI-Correct Resize

```c
static void on_resize(GtkGLArea *area, int w, int h, AppData *d) {
    if (!d->pm) return;
    gtk_gl_area_make_current(area);
    int scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    projectm_set_window_size(d->pm, (size_t)(w * scale), (size_t)(h * scale));
}
```

#### `on_unrealize` — Cleanup

```c
static void on_unrealize(GtkGLArea *area, AppData *d) {
    gtk_gl_area_make_current(area);
    if (d->pm) {
        projectm_destroy(d->pm);
        d->pm = NULL;
    }
}
```

#### Preset Directory Scan

Call at startup, after parsing args:

```c
static void scan_presets(AppData *d) {
    d->preset_count   = 0;
    d->preset_current = 0;

    GDir *dir = g_dir_open(d->preset_dir, 0, NULL);
    if (!dir) return;

    // First pass: count
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (g_str_has_suffix(name, ".milk"))
            d->preset_count++;
    }
    g_dir_rewind(dir);

    d->presets = g_new0(char *, d->preset_count + 1);
    int i = 0;
    while ((name = g_dir_read_name(dir)) && i < d->preset_count) {
        if (g_str_has_suffix(name, ".milk"))
            d->presets[i++] = g_build_filename(d->preset_dir, name, NULL);
    }
    g_dir_close(dir);

    // Sort for deterministic order
    g_qsort_with_data(d->presets, d->preset_count,
                      sizeof(char *), (GCompareDataFunc)g_strcmp0, NULL);

    g_print("milkdrop: found %d presets in %s\n",
            d->preset_count, d->preset_dir);
}
```

#### CLI Arguments

Parse with `GOptionContext` or plain `getopt`. Required flags:

| Flag | Type | Description |
|------|------|-------------|
| `--monitor` | int | Monitor index (0 = primary) |
| `--opacity` | float | Window opacity 0.0–1.0 |
| `--preset-dir` | string | Path to directory of `.milk` files |
| `--socket` | string | Path for Unix control socket |
| `--overlay` | bool | Z-order hint (reserved, no-op in v2.0) |
| `--fps` | int | Target frame rate (default: 60) |

---

### 5.4 `src/control.c` + `src/control.h` — Command Socket

**Responsibility:** Listen on a Unix socket for control commands from the
extension. Translate commands into atomic flag writes readable by the GL thread.
Never call any GL or projectM function.

**Protocol:** Binary. Fixed-size header, variable-length payload.

```c
// control.h
#pragma once
#include "app.h"

// Starts control thread. Returns 0 on success.
int  control_init(AppData *d);
void control_cleanup(AppData *d);

// Packet format
typedef struct __attribute__((packed)) {
    uint8_t  cmd;
    uint16_t payload_len;   // bytes following this header
} CmdHeader;

typedef enum : uint8_t {
    CMD_LOAD_PRESET = 1,    // payload: null-terminated path string
    CMD_NEXT_PRESET = 2,    // no payload
    CMD_PREV_PRESET = 3,    // no payload
    CMD_PAUSE       = 4,    // no payload
    CMD_RESUME      = 5,    // no payload
    CMD_SET_OPACITY = 6,    // payload: float32 (4 bytes)
    CMD_GET_STATUS  = 7,    // no payload; server writes StatusPacket response
} CmdType;

typedef struct __attribute__((packed)) {
    float   fps;
    uint8_t paused;
    char    preset_name[256];
} StatusPacket;
```

**Control thread loop:**

```c
static void *control_thread(void *arg) {
    AppData *d = arg;

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, d->sock_path, sizeof(addr.sun_path) - 1);
    unlink(d->sock_path);   // remove stale socket
    bind(srv, (struct sockaddr *)&addr, sizeof(addr));
    listen(srv, 4);

    while (1) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) break;

        CmdHeader hdr;
        if (read(client, &hdr, sizeof(hdr)) != sizeof(hdr)) {
            close(client); continue;
        }

        uint8_t payload[PRESET_NAME_MAX] = {0};
        if (hdr.payload_len > 0 && hdr.payload_len < sizeof(payload))
            read(client, payload, hdr.payload_len);

        switch ((CmdType)hdr.cmd) {
        case CMD_LOAD_PRESET:
            strncpy(d->next_preset, (char *)payload, PRESET_NAME_MAX - 1);
            atomic_store(&d->preset_pending, true);
            break;

        case CMD_NEXT_PRESET:
            // Compute next path, write to next_preset, set flag
            // Must read preset list without holding a lock — preset list
            // is written once at startup and never modified after.
            if (d->preset_count > 0) {
                int next = (d->preset_current + 1) % d->preset_count;
                strncpy(d->next_preset, d->presets[next], PRESET_NAME_MAX - 1);
                d->preset_current = next;
                atomic_store(&d->preset_pending, true);
            }
            break;

        case CMD_PREV_PRESET:
            if (d->preset_count > 0) {
                int prev = (d->preset_current - 1 + d->preset_count)
                           % d->preset_count;
                strncpy(d->next_preset, d->presets[prev], PRESET_NAME_MAX - 1);
                d->preset_current = prev;
                atomic_store(&d->preset_pending, true);
            }
            break;

        case CMD_PAUSE:
            atomic_store(&d->paused, true);
            atomic_store(&d->pause_pending, true);
            break;

        case CMD_RESUME:
            atomic_store(&d->paused, false);
            atomic_store(&d->pause_pending, true);
            break;

        case CMD_SET_OPACITY: {
            float op;
            memcpy(&op, payload, sizeof(float));
            atomic_store(&d->opacity_pending_value, op);
            atomic_store(&d->opacity_pending, true);
            break;
        }

        case CMD_GET_STATUS: {
            StatusPacket s = {
                .fps    = atomic_load(&d->current_fps),
                .paused = (uint8_t)atomic_load(&d->paused),
            };
            // preset_name: basename of current preset
            const char *path = d->preset_count > 0
                              ? d->presets[d->preset_current] : "";
            const char *base = strrchr(path, '/');
            strncpy(s.preset_name, base ? base + 1 : path,
                    sizeof(s.preset_name) - 1);
            write(client, &s, sizeof(s));
            break;
        }
        }

        close(client);
    }

    close(srv);
    unlink(d->sock_path);
    return NULL;
}
```

---

### 5.5 `extension/milkdrop@mauriciobc.github.io/extension.js`

**Full implementation is in §5.6.** This section documents the interface contract.

**Responsibilities:**
- Spawn the C binary with correct CLI args derived from GSettings
- Watch for the binary's window via `global.display::window-created`
- Anchor the window actor at the bottom of `global.window_group` (§5.6)
- Re-anchor on `global.display::restacked`
- Route settings changes to socket commands vs. binary restart (see table below)
- Restart the binary on crash with exponential backoff

**Settings routing:**

| Setting key | Action |
|-------------|--------|
| `monitor` | Restart binary (new surface needed) |
| `overlay` | Restart binary (z-order mode change) |
| `opacity` | `CMD_SET_OPACITY` via socket |
| `preset-dir` | `CMD_NEXT_PRESET` via socket |
| `shuffle` | `CMD_NEXT_PRESET` via socket |

**Required GJS imports** (these need the extension to run inside GNOME Shell):

```javascript
import Meta   from 'gi://Meta';
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
```

---

### 5.6 `extension/milkdrop@mauriciobc.github.io/extension.js` — Compositor Anchoring

**This is the most GNOME-specific part of the entire project. Read carefully.**

The extension does two distinct jobs:

1. **Process management** — spawn, kill, restart (standard extension pattern)
2. **Compositor anchoring** — place the binary's window at the wallpaper layer in
   the GNOME Shell Clutter scene graph

Job 2 is why the extension needs access to `Meta`, `Clutter`, and `Main` from
`ui/main.js`. These are only available inside the GNOME Shell process. The C
binary cannot do this for itself.

#### Why Type Hints Are Not Enough

`GDK_SURFACE_TYPE_HINT_DESKTOP` sets `_NET_WM_WINDOW_TYPE_DESKTOP` on the
Wayland surface. On X11, window managers strictly honor this for z-ordering.
On Wayland/Mutter, this is advisory. In practice on GNOME 47/48, a window with
this hint may render at normal window z-order depending on compositor state,
workspace switches, and lock/unlock cycles.

The reliable approach — confirmed by studying Hanabi's architecture — is to
intercept the window's Clutter actor after it is created and re-position it
within `global.window_group` to the bottom of the stack, below all other app
windows but above `Main.layoutManager._backgroundGroup`.

#### GNOME Shell Layer Stack

```
global.stage
├── Main.layoutManager._backgroundGroup     z=0  ← static wallpaper image
├── global.window_group                     z=1  ← all MetaWindows
│   ├── [milkdrop actor]  ← anchored HERE (bottom of window_group)
│   ├── other app window actors
│   └── ...
└── Main.layoutManager.uiGroup              z=2  ← panel, dash, overlays
```

Placing the milkdrop actor at the bottom of `global.window_group` means it
renders above the static wallpaper but below every application window. This is
the exact wallpaper layer.

#### The `restacked` Signal

GNOME Shell emits `global.display::restacked` whenever it re-sorts the window
stack — on workspace switch, window focus change, fullscreen toggle, lock/unlock,
and monitor change. After each restack, our actor may have been moved back up.
Reconnect to `restacked` and call `set_child_below_sibling(actor, null)` again.

#### Complete Implementation

```javascript
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import Meta from 'gi://Meta';
import Clutter from 'gi://Clutter';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import { Extension } from 'resource:///org/gnome/shell/extensions/extension.js';

const BINARY_WM_CLASS = 'milkdrop';   // must match g_set_prgname() in C binary
const MAX_RETRIES     = 5;
const RETRY_DELAYS    = [0, 1000, 2000, 5000, 10000];  // ms
const RESTART_KEYS    = new Set(['monitor', 'overlay']);

export default class MilkdropExtension extends Extension {

    // ── Lifecycle ───────────────────────────────────────────────────────

    enable() {
        this._settings       = this.getSettings();
        this._proc           = null;
        this._retries        = 0;
        this._retrySource    = null;
        this._rendererActor  = null;
        this._rendererWindow = null;
        this._sockPath       = `${GLib.get_user_runtime_dir()}/milkdrop.sock`;

        this._changedId = this._settings.connect('changed', (_s, key) => {
            RESTART_KEYS.has(key) ? this._restart() : this._sendSettingUpdate(key);
        });

        // Watch for renderer window before spawning so we don't miss it
        this._windowCreatedId = global.display.connect(
            'window-created',
            (_display, metaWindow) => this._onWindowCreated(metaWindow)
        );

        this._spawn();
    }

    disable() {
        this._settings.disconnect(this._changedId);
        global.display.disconnect(this._windowCreatedId);
        this._clearAnchor();
        if (this._retrySource) {
            GLib.source_remove(this._retrySource);
            this._retrySource = null;
        }
        this._kill();
    }

    _args() {
        const s = this._settings;
        const args = [
            `${GLib.get_home_dir()}/.local/bin/milkdrop`,
            '--monitor',    String(s.get_int('monitor')),
            '--opacity',    String(s.get_double('opacity')),
            '--preset-dir', s.get_string('preset-dir'),
            '--socket',     this._sockPath,
            '--fps',        '60',
        ];
        if (s.get_boolean('overlay')) args.push('--overlay');
        return args;
    }

    _spawn() {
        if (this._proc) return;
        try {
            this._proc = Gio.Subprocess.new(
                this._args(),
                Gio.SubprocessFlags.STDOUT_SILENCE |
                Gio.SubprocessFlags.STDERR_SILENCE
            );
            this._proc.wait_async(null, (_proc, res) => {
                try { _proc.wait_finish(res); } catch (_e) {}
                this._proc = null;
                this._clearAnchor();
                console.log('[Milkdrop] process exited');
                if (_proc.get_exit_status() !== 0) this._scheduleRetry();
            });
            this._retries = 0;
            console.log('[Milkdrop] spawned');
        } catch (e) {
            console.error('[Milkdrop] spawn failed:', e.message);
            this._scheduleRetry();
        }
    }

    _kill() {
        if (!this._proc) return;
        try { this._proc.send_signal(15); } catch (_e) {}
        this._proc = null;
    }

    _restart() {
        this._clearAnchor();
        this._kill();
        this._spawn();
    }

    _scheduleRetry() {
        if (this._retries >= MAX_RETRIES) {
            console.error('[Milkdrop] max retries exceeded');
            return;
        }
        const delay = RETRY_DELAYS[Math.min(this._retries, RETRY_DELAYS.length - 1)];
        this._retries++;
        this._retrySource = GLib.timeout_add(GLib.PRIORITY_DEFAULT, delay, () => {
            this._spawn();
            this._retrySource = null;
            return GLib.SOURCE_REMOVE;
        });
    }

    // ── Compositor Anchoring ────────────────────────────────────────────

    _onWindowCreated(metaWindow) {
        // Filter by WM_CLASS set via g_set_prgname() in the C binary.
        // get_wm_class() returns the instance name; get_wm_class_instance()
        // returns the class name. Either works — both are 'milkdrop'.
        if (metaWindow.get_wm_class() !== BINARY_WM_CLASS) return;

        // The Clutter actor may not exist at window-created time on all
        // GNOME versions. Defer one frame to be safe.
        GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._anchorWindow(metaWindow);
            return GLib.SOURCE_REMOVE;
        });
    }

    _anchorWindow(metaWindow) {
        const actor = metaWindow.get_compositor_private();
        if (!actor) {
            console.warn('[Milkdrop] window actor not ready, skipping anchor');
            return;
        }

        // Prevent window from appearing in taskbar, Alt+Tab, workspace thumbnails
        metaWindow.stick();                      // show on all workspaces
        metaWindow.set_skip_taskbar(true);
        metaWindow.set_skip_pager(true);

        this._rendererWindow = metaWindow;
        this._rendererActor  = actor;

        this._doAnchor();

        // Re-anchor after every restack (workspace switch, focus change,
        // lock/unlock, fullscreen toggle)
        this._restackedId = global.display.connect(
            'restacked',
            () => this._doAnchor()
        );

        console.log('[Milkdrop] window anchored at wallpaper layer');
    }

    _doAnchor() {
        if (!this._rendererActor) return;

        const windowGroup = global.window_group;

        // Reparent if needed (defensive — should already be here)
        if (this._rendererActor.get_parent() !== windowGroup)
            this._rendererActor.reparent(windowGroup);

        // Place at the very bottom of window_group:
        // below all application windows, above the background group
        windowGroup.set_child_below_sibling(this._rendererActor, null);
    }

    _clearAnchor() {
        if (this._restackedId) {
            global.display.disconnect(this._restackedId);
            this._restackedId = null;
        }
        this._rendererActor  = null;
        this._rendererWindow = null;
    }

    // ── Settings Socket Commands ─────────────────────────────────────────

    _sendSettingUpdate(key) {
        if (!this._proc) return;
        const s = this._settings;
        let buf = null;

        if (key === 'opacity') {
            // CMD_SET_OPACITY = 6, payload = float32 LE (4 bytes)
            buf = new ArrayBuffer(3 + 4);
            const v = new DataView(buf);
            v.setUint8(0, 6);
            v.setUint16(1, 4, true);
            v.setFloat32(3, s.get_double('opacity'), true);
        } else if (key === 'preset-dir') {
            buf = this._buildCmd(2, null);   // CMD_NEXT_PRESET
        } else if (key === 'shuffle') {
            buf = this._buildCmd(2, null);   // simplest: just advance preset
        }

        if (buf) this._sendRaw(buf);
    }

    _buildCmd(type, payloadStr) {
        const payload = payloadStr
            ? new TextEncoder().encode(payloadStr + '\0')
            : new Uint8Array(0);
        const buf  = new ArrayBuffer(3 + payload.byteLength);
        const view = new DataView(buf);
        view.setUint8(0, type);
        view.setUint16(1, payload.byteLength, true);
        if (payload.byteLength) new Uint8Array(buf, 3).set(payload);
        return buf;
    }

    _sendRaw(buf) {
        try {
            const conn = new Gio.SocketClient().connect(
                new Gio.UnixSocketAddress({ path: this._sockPath }), null);
            conn.get_output_stream().write_all(new Uint8Array(buf), null);
            conn.close(null);
        } catch (_e) { /* binary not yet ready — ignore */ }
    }
}
```

#### Edge Cases and Known Issues From Hanabi

| Scenario | Behavior | Mitigation |
|----------|----------|------------|
| Lock screen then unlock | Mutter re-stacks all windows; milkdrop actor moves up | `restacked` signal handler calls `_doAnchor()` |
| Workspace switch | Same as above | Same mitigation |
| Nautilus desktop icons | Nautilus also runs at desktop level using `_NET_WM_WINDOW_TYPE_DESKTOP`. Actor z-order within `window_group` is distinct from WM type. Our actor is at `null` (absolute bottom), Nautilus's actor is managed by Mutter normally. They do not collide. | WM_CLASS filter ensures we only anchor `milkdrop`, not Nautilus |
| Window actor not ready on `window-created` | On some GNOME versions `get_compositor_private()` returns null at this signal | Deferred with `GLib.idle_add()` |
| Fullscreen app covers visualizer | Correct behavior — fullscreen windows should be on top | No mitigation needed |
| `metaWindow.reparent()` deprecated in GNOME 48 | `reparent()` was deprecated in Clutter 1.10 but still works. Check GNOME 49 changelog. | If removed: use `actor.get_parent().remove_child(actor); windowGroup.add_child(actor);` |

---

### 5.7 `data/org.gnome.shell.extensions.milkdrop.gschema.xml`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<schemalist>
  <schema id="org.gnome.shell.extensions.milkdrop"
          path="/org/gnome/shell/extensions/milkdrop/">

    <key name="enabled" type="b">
      <default>true</default>
      <summary>Enable visualizer</summary>
    </key>

    <key name="monitor" type="i">
      <default>0</default>
      <summary>Monitor index</summary>
    </key>

    <key name="opacity" type="d">
      <default>1.0</default>
      <range min="0.0" max="1.0"/>
      <summary>Window opacity</summary>
    </key>

    <key name="preset-dir" type="s">
      <default>''</default>
      <summary>Directory containing .milk preset files</summary>
    </key>

    <key name="shuffle" type="b">
      <default>true</default>
      <summary>Shuffle presets</summary>
    </key>

    <key name="overlay" type="b">
      <default>false</default>
      <summary>Render above windows instead of behind (reserved)</summary>
    </key>

  </schema>
</schemalist>
```

---

### 5.8 `meson.build`

```meson
project('milkdrop', 'c',
    version: '2.0.0',
    default_options: [
        'c_std=c11',
        'optimization=3',
        'warning_level=2',
    ])

cc = meson.get_compiler('c')

projectm = dependency('libprojectM4',   required: true,
    not_found_message: 'Install libprojectm-dev (apt) / projectm (pacman) / projectM-devel (dnf)')
pipewire = dependency('libpipewire-0.3', required: true)
gtk4     = dependency('gtk4',            required: true)
threads  = dependency('threads')
m        = cc.find_library('m', required: true)

if get_option('static_link')
    projectm = dependency('libprojectM4', static: true)
endif

sources = files(
    'src/main.c',
    'src/audio.c',
    'src/control.c',
)

executable('milkdrop', sources,
    include_directories: include_directories('src'),
    dependencies: [projectm, pipewire, gtk4, threads, m],
    install: true,
    install_dir: get_option('bindir'))

# GSettings schema
install_data('data/org.gnome.shell.extensions.milkdrop.gschema.xml',
    install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')

# GNOME Shell extension
install_subdir('extension',
    install_dir: get_option('datadir') / 'gnome-shell' / 'extensions',
    strip_directory: false)
```

### `meson_options.txt`

```meson
option('static_link',
    type: 'boolean',
    value: false,
    description: 'Statically link libprojectM (not recommended for normal installs)')
```

---

## 6. Agent Task Breakdown

Tasks are sequenced with explicit dependencies and gates.
A gate must pass before starting the next group.
Each task includes the files to create/modify and the acceptance test.

---

### Day 1 — Audio and Rendering

#### TASK-01: Repository scaffold

**Files to create:**
```
meson.build
meson_options.txt
src/app.h
src/main.c          (stub: opens GTK4 window, black GtkGLArea)
src/audio.c         (stub: audio_init returns -1, prints warning)
src/audio.h
src/control.c       (stub: control_init returns 0, does nothing)
src/control.h
extension/milkdrop@mauriciobc.github.io/metadata.json
extension/milkdrop@mauriciobc.github.io/extension.js
extension/milkdrop@mauriciobc.github.io/prefs.js
data/org.gnome.shell.extensions.milkdrop.gschema.xml
tools/install.sh
tools/uninstall.sh
tools/reload.sh
```

**`metadata.json` content:**
```json
{
  "uuid":        "milkdrop@mauriciobc.github.io",
  "name":        "Milkdrop",
  "description": "MilkDrop audio visualizer for GNOME Wayland",
  "shell-version": ["47", "48", "49"],
  "version":     2
}
```

**Acceptance test:**
```sh
meson setup build
meson compile -C build
# Compiles without errors. Binary exists at build/milkdrop.
```

---

#### TASK-01.5: Compositor anchoring (extension side)

**Files to modify:** `extension/milkdrop@mauriciobc.github.io/extension.js`

Implement the full anchoring logic from §5.6 before doing any rendering work.
It is far easier to debug anchoring against the black window from TASK-01
than against a running projectM visualizer.

Required:
- `_onWindowCreated()`: filter by `WM_CLASS === 'milkdrop'`, defer with `GLib.idle_add()`
- `_anchorWindow()`: call `metaWindow.stick()`, `set_skip_taskbar(true)`, `set_skip_pager(true)`, then `_doAnchor()`
- `_doAnchor()`: `reparent` to `global.window_group` if needed, `set_child_below_sibling(actor, null)`
- Connect `global.display::restacked` → `_doAnchor()`
- `_clearAnchor()`: disconnect `restacked`, null out references

Also add `g_set_prgname("milkdrop")` to `main.c` before `gtk_application_run()`.

**Acceptance test:**
```sh
# Run the binary from TASK-01 (black window, no projectM yet)
./build/milkdrop --monitor 0 --opacity 1.0 \
    --preset-dir /tmp --socket /run/user/1000/milkdrop.sock &

# With the extension enabled:
tools/reload.sh

# Test 1: Basic z-order
# Open a terminal or any application window.
# The milkdrop window must appear BEHIND the application window, not in front.
# Open Files (Nautilus) — milkdrop must appear behind it.

# Test 2: Workspace switch
# Switch to another workspace and switch back.
# milkdrop must still be behind all windows.

# Test 3: Lock screen (the failure mode Hanabi hit in PR #172)
loginctl lock-session
# Wait 3 seconds, then unlock.
# milkdrop must still be behind all application windows.
# If it renders on top after unlock, the restacked signal handler is not working.

# Test 4: Taskbar
# The milkdrop window must NOT appear in the dock or Alt+Tab switcher.
```

**This gate must pass before TASK-02. A visualizer that renders above
application windows is not a wallpaper — it's a screensaver.**

---

#### TASK-02: projectM renders a preset (hardcoded path)

**Files to modify:** `src/main.c`

Implement the full window setup from §5.3:
- `build_window()` with GtkGLArea, type hint, non-interactive flags
- `on_realize()` with `projectm_create()`, hardcode one preset path
  (use any `.milk` file from the system if available, e.g.
  `/usr/share/projectM/presets/Aderrasi - Turning Into.milk`)
- `on_render()` calling `projectm_render_frame()` and restoring GL state
- `on_resize()` with scale factor correction
- `on_unrealize()` destroying projectM handle

**Acceptance test:**
```sh
./build/milkdrop --monitor 0 --opacity 1.0 \
    --preset-dir /usr/share/projectM/presets \
    --socket /run/user/1000/milkdrop.sock
# A window appears showing a MilkDrop preset animating at ~60fps.
# No error messages in terminal.
# GNOME Shell log (journalctl -f) shows no warnings.
```

---

#### TASK-03: PipeWire audio capture

**Files to modify:** `src/audio.c`, `src/audio.h`

Implement full PipeWire monitor capture:
- `audio_init()`: init PipeWire, create stream requesting F32 stereo 48kHz,
  `PW_STREAM_FLAG_AUTOCONNECT`, connect to monitor source
- `on_process()` callback: drain buffer, call `ring_push()` for each frame
- Start PipeWire main loop on a dedicated thread
- `audio_cleanup()`: stop loop, destroy stream, join thread
- Use `ring_push()` / `ring_read()` exactly as defined in §5.2

Wire into `main.c`: call `audio_init(d)` before `gtk_application_run()`.

**Acceptance test:**
```sh
./build/milkdrop --monitor 0 --opacity 1.0 \
    --preset-dir /usr/share/projectM/presets \
    --socket /run/user/1000/milkdrop.sock
# Play music through system speakers.
# Visualizer pulses visibly in sync with the beat.
# strace -p <pid> -e write shows no socket writes during playback.
```

**Gate — Day 1 complete.**
Both tests above pass. Preset animates. Beat response is visible.
If this gate slips, stop and debug before proceeding to Day 2.

---

### Day 2 — Config, Presets, Extension

#### TASK-04: Preset directory scan

**Files to modify:** `src/main.c`

Implement `scan_presets()` as defined in §5.3.
- Parse `--preset-dir` from CLI args
- Build `d->presets[]` array at startup
- Load `d->presets[0]` in `on_realize()`

**Acceptance test:**
```sh
./build/milkdrop --preset-dir ~/presets ...
# Terminal prints: "milkdrop: found N presets in ~/presets"
# First preset in alphabetical order is loaded.
```

---

#### TASK-05: Control socket

**Files to modify:** `src/control.c`, `src/control.h`

Implement the full control thread from §5.4:
- `control_init()`: start thread, bind socket at `d->sock_path`
- Handle all 7 commands
- `CMD_NEXT_PRESET` and `CMD_PREV_PRESET`: cycle `d->preset_current`,
  write to `d->next_preset`, set `d->preset_pending`
- `CMD_GET_STATUS`: respond with `StatusPacket`
- `control_cleanup()`: signal thread, join

Wire into `main.c`: call `control_init(d)` before `gtk_application_run()`,
`control_cleanup(d)` in the shutdown path.

**Acceptance test:**
```sh
# While milkdrop is running:
python3 -c "
import socket, struct
s = socket.socket(socket.AF_UNIX)
s.connect('/run/user/1000/milkdrop.sock')
s.send(struct.pack('<BH', 2, 0))   # CMD_NEXT_PRESET
s.close()
"
# Preset switches within 100ms.

python3 -c "
import socket, struct
s = socket.socket(socket.AF_UNIX)
s.connect('/run/user/1000/milkdrop.sock')
s.send(struct.pack('<BH', 7, 0))   # CMD_GET_STATUS
data = s.recv(512)
fps, paused = struct.unpack_from('<fB', data)
print(f'fps={fps:.1f} paused={paused}')
s.close()
"
# Prints: fps=59.8 paused=0  (or similar)
```

---

#### TASK-06: GJS extension — spawn, kill, retry

**Files to modify:** `extension/milkdrop@mauriciobc.github.io/extension.js`

Implement the full extension from §5.5.
- `enable()`: connect settings, spawn binary
- `disable()`: disconnect settings, kill binary, cancel retry
- `_spawn()`: `Gio.Subprocess.new()` with correct args, async wait
- `_onExit()`: schedule retry if exit code != 0
- `_scheduleRetry()`: exponential backoff via `GLib.timeout_add()`

**Acceptance test:**
```sh
tools/install.sh
tools/reload.sh
# Extension appears in GNOME Extensions panel.
# Enabling it spawns the binary (visible in `ps aux | grep milkdrop`).
# Disabling it kills the binary.

# Crash recovery test:
kill -9 $(pgrep milkdrop)
# Extension restarts binary within 2 seconds.
```

---

#### TASK-07: Granular settings routing

**Files to modify:** `extension/milkdrop@mauriciobc.github.io/extension.js`

Implement `_sendSettingUpdate()` from §5.5.

| Setting key | Behavior |
|-------------|----------|
| `monitor` | Restart binary |
| `overlay` | Restart binary |
| `opacity` | Send `CMD_SET_OPACITY` (6) with float32 payload |
| `preset-dir` | Send `CMD_NEXT_PRESET` (2) — binary uses current dir from CLI args; restart required for full rescan |
| `shuffle` | Send `CMD_LOAD_PRESET` with empty path to trigger internal shuffle toggle (or restart) |

**Acceptance test:**
```sh
# While extension is running:
gsettings set org.gnome.shell.extensions.milkdrop opacity 0.5
# Window opacity changes — binary does NOT restart (verify with ps — PID unchanged)

gsettings set org.gnome.shell.extensions.milkdrop monitor 0
# Binary restarts (new PID in ps output)
```

---

#### TASK-08: prefs.js

**Files to modify:** `extension/milkdrop@mauriciobc.github.io/prefs.js`

Build a minimal GNOME preferences panel with:
- Toggle: enabled
- Spinner: monitor (0–5)
- Slider: opacity (0.0–1.0)
- File chooser button: preset-dir
- Toggle: shuffle
- Toggle: overlay (labeled "Show above windows (experimental)")

Use `Adw.PreferencesPage` and `Adw.PreferencesGroup` for GNOME 47+ compatibility.

**Acceptance test:**
```sh
# Open GNOME Extensions → milkdrop → Settings
# All controls appear and respond.
# Changing opacity slider updates the window without restarting the binary.
```

---

#### TASK-09: Tools scripts

**Files:** `tools/install.sh`, `tools/uninstall.sh`, `tools/reload.sh`

```sh
# tools/install.sh
#!/usr/bin/env bash
set -e
meson setup build --prefix="$HOME/.local" --reconfigure 2>/dev/null || true
meson compile -C build
meson install -C build
glib-compile-schemas "$HOME/.local/share/glib-2.0/schemas/"
echo "Done. Run: tools/reload.sh"

# tools/reload.sh
#!/usr/bin/env bash
gnome-extensions disable milkdrop@mauriciobc.github.io 2>/dev/null || true
gnome-extensions enable  milkdrop@mauriciobc.github.io
echo "Reloaded."

# tools/uninstall.sh
#!/usr/bin/env bash
gnome-extensions disable milkdrop@mauriciobc.github.io 2>/dev/null || true
ninja -C build uninstall
```

---

## 7. Success Metrics

All metrics measured with a real `.milk` preset running and music playing.

| Metric | Target | Pass/Fail Test |
|--------|--------|----------------|
| Frame rate | ≥ 58fps | `CMD_GET_STATUS` → fps field |
| CPU (non-projectM) | < 0.5ms/frame | `perf stat` on ring_read + cmd check |
| Audio latency | < 20ms | Beat → visual response, stopwatch |
| Memory RSS | < 35MB | `ps -o pid,rss -p $(pgrep milkdrop)` |
| Steady-state IPC | 0 writes | `strace -p <pid> -e write` during playback |
| Build time (warm) | < 15s | `time meson compile -C build` |
| Preset switch | < 150ms | Stopwatch from CMD_NEXT to visual change |
| Crash recovery | < 3s | `kill -9 $(pgrep milkdrop)` — measure restart |
| GNOME Shell impact | No warnings | `journalctl -f -o cat /usr/bin/gnome-shell` |

---

## 8. Risk Register

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| `libprojectM4` not in repos | Low | High | Ubuntu 22.04+, Arch, Fedora 38+ all ship it. Provide manual build instructions in README if missing. |
| projectM stomps GL state GTK4 needs | Medium | Medium | Restore `GL_DEPTH_TEST` + `GL_STENCIL_TEST` after `render_frame()`. Already in the code. |
| projectM creates its own GL context | Low | High | 4.x is designed for embedding. If it happens, debug `projectm_create()` flags before any other work. |
| Preset shader compile blocks 150ms+ | Certain | Low | Documented behavior. No mitigation in v2.0. Precompile cache in v2.1. |
| PipeWire monitor source unavailable | Medium | Low | Fall back to silence. Binary continues running. |
| HiDPI forgotten | Medium | Medium | `gtk_widget_get_scale_factor()` is in `on_realize` and `on_resize`. Do not remove it. |
| `window-created` fires before actor is ready | Medium | Low | Deferred with `GLib.idle_add()` in `_onWindowCreated()`. |
| `restacked` fires too frequently, causing jank | Low | Low | `set_child_below_sibling()` is a Clutter z-order call, not a repaint. Cost is negligible. |
| `actor.reparent()` deprecated in GNOME 49 | Low | Medium | Fallback: `parent.remove_child(actor); windowGroup.add_child(actor)`. Check on GNOME 49 before release. |
| Milkdrop actor fights with Nautilus desktop layer | Low | Low | We anchor inside `global.window_group` at z=bottom. Nautilus's desktop window is a separate MetaWindow also managed by Mutter. WM_CLASS filter prevents cross-anchoring. |
| Z-order not restored after screen lock | Medium | Medium | `restacked` signal fires after unlock. `_doAnchor()` is called. Verified by Hanabi's approach. Test explicitly. |

---

## 9. Out of Scope (v2.0)

These are explicitly deferred. Do not implement them.

- X11 support
- `ext-layer-shell-v1` (not yet stable across GNOME 47/48/49)
- Bundled preset library (users provide their own `.milk` files)
- GUI preset editor
- D-Bus status interface (extension does not need it)
- Per-pixel equation transpiler (libprojectM handles this)
- Custom shader authoring UI
- GNOME Extensions website submission

---

## 10. Dependency Install Reference

```sh
# Ubuntu / Debian
sudo apt install \
    libprojectm-dev \
    libpipewire-0.3-dev \
    libgtk-4-dev \
    meson ninja-build pkg-config

# Arch Linux
sudo pacman -S \
    projectm \
    pipewire \
    gtk4 \
    meson ninja pkg-config

# Fedora
sudo dnf install \
    projectM-devel \
    pipewire-devel \
    gtk4-devel \
    meson ninja-build pkg-config
```

Minimum versions: libprojectM >= 4.0.0, GLib >= 2.76, GTK >= 4.10,
PipeWire >= 0.3.48, GNOME Shell 47.

---

## 11. projectM API Quick Reference

All calls must happen on the GL thread (inside GtkGLArea signal handlers
or functions called from them).

```c
// Lifecycle
projectm_handle projectm_create(void);
void            projectm_destroy(projectm_handle pm);

// Configuration
void projectm_set_window_size(projectm_handle pm, size_t w, size_t h);
void projectm_set_fps(projectm_handle pm, int fps);
void projectm_set_preset_shuffle(projectm_handle pm, bool shuffle);

// Audio input
void projectm_pcm_add_float(projectm_handle pm,
                             const float *pcm,
                             uint32_t sample_count,  // stereo pairs
                             projectm_channels channels);  // PROJECTM_STEREO

// Preset
void projectm_load_preset_file(projectm_handle pm,
                                const char *path,
                                bool smooth_transition);

// Render — call with target FBO bound
void projectm_render_frame(projectm_handle pm);
```

Include: `#include <projectM-4/projectM.h>`  
Link flag: `-lprojectM` (handled by Meson dependency)

---

## 12. GNOME Shell Clutter Layer Reference

Quick reference for the compositor anchoring code. These are the relevant
objects from `Main` and `global` accessible inside a GNOME Shell extension.

```
global.stage                         Root Clutter.Stage
│
├── Main.layoutManager._backgroundGroup
│     BackgroundGroup — holds the static desktop wallpaper image(s).
│     One MetaBackgroundActor per monitor.
│     Z-position: bottom of the visible stack.
│
├── global.window_group
│     Clutter.Actor containing all MetaWindow compositor actors.
│     The milkdrop actor is anchored HERE at z=bottom (null sibling).
│     Z-position: above background, below shell UI.
│
├── global.top_window_group
│     Clutter.Actor for always-on-top windows and screensavers.
│     Do not place anything here.
│
└── Main.layoutManager.uiGroup
      Shell.GenericContainer with the panel, dash, overview, notifications.
      Z-position: topmost — always above application windows.
```

**Key methods used in anchoring:**

```javascript
// MetaWindow methods
metaWindow.stick()                  // show on all workspaces
metaWindow.set_skip_taskbar(true)   // hide from taskbar/dock
metaWindow.set_skip_pager(true)     // hide from workspace thumbnail
metaWindow.get_compositor_private() // returns the Clutter.Actor

// Clutter.Actor methods
actor.get_parent()                  // returns current parent container
actor.reparent(newParent)           // move to new container
windowGroup.set_child_below_sibling(actor, null)  // z=bottom of container

// global.display signals
global.display.connect('window-created', (display, metaWindow) => {})
global.display.connect('restacked', () => {})
```
