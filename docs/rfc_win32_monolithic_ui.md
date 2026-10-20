# RFC: Monolithic Win32 Application Architecture with Embedded WebView2 & imagoro-ui

**RFC Number:** 2026-RFC-01  
**Status:** Proposed  
**Created:** 2026-10-04  
**Target:** `voxel-fps-engine-0.0` (C++ Vulkan engine) & `imagoro-ui` (React front-end)  
**Authors:** Antigravity / Engineering Team  
**Governing Rules:** [`RULES.md`](../RULES.md) (sim/view authority, determinism, layer separation)

---

## 1. Executive Summary

This RFC details the architecture for wrapping the C++ Vulkan voxel engine and the React-based [**`imagoro-ui`**](file:///c:/Users/gryph/kepler/worktrees/recov-iui) workspace into a **single, cohesive, monolithic Windows application**.

Under this architecture:
1. **Single Executable & Single Window:** A single `voxel_engine.exe` manages the top-level Win32 window (`HWND`), creating two coordinated child windows:
   - A **Vulkan 3D Viewport child window** (`g_hViewportWnd`) dedicated to hardware-accelerated 3D world, model, and playfield rendering.
   - A **WebView2 child window** (`g_hWebPanelWnd`) hosting `imagoro-ui` (React + TypeScript + Vite) for dockable tabs, editor panels, inspector, palette pickers, toolbars, and node-graph canvases.
2. **Unified Command-Line Mode Switcher:**
   `voxel_engine.exe --mode {engine | editor | validate | headless}`
3. **Strict Sim/View Authority Boundary ([`RULES.md`](../RULES.md)):** The simulation remains isolated, authoritative, and deterministic. The React UI never directly touches engine or simulation memory; communication is strictly asynchronous via a JSON-RPC / message bridge (`ICoreWebView2::PostWebMessageAsJson` and `WebMessageReceived`).
4. **Java Painter Coexistence:** The standalone Java painter ([`java/painter/`](../java/painter/)) is **not** discarded; both the engine, the Java painter, and `imagoro-ui` share the canonical **`voxfmt v2`** data format. The Java painter remains functional throughout development as an external authoring tool.

---

## 2. Motivation & Architectural Goals

### 2.1 The Current Fragmentation
Currently, engine workflows are fragmented across separate tools:
- The **C++ Vulkan engine** (`src/main.cpp`) runs the 3D game and a minimal Dear ImGui menu (`src/vis_menu.hpp`).
- The **Java painter** (`java/painter/`) authors voxel maps and item bounding volumes in a Swing desktop app.
- **Python scripts** (`python/projectiles/`) manage ballistic curves and material tables.
- **`imagoro-ui`** (`recov-iui`) contains a modern, block-based React workspace and node-canvas architecture designed for tool integration, but is not yet wired to the C++ runtime.

### 2.2 Goals
- **Single Window UX:** Combine the 3D viewport and rich editor UI into one window with flexible layouts (docked sidebars, top toolbars, full-viewport game toggle).
- **Fast Iteration:** React/Vite hot-reloading for UI widgets during development, without recompiling C++.
- **Zero Simulation Compromise:** Zero impact on the 60 Hz / 120 Hz fixed-step deterministic simulation loops and golden hash baselines (smoke: `c74104775f89ae9f`, stress: `102f4bb7f500cd54`; see §10).
- **Loose File Distribution:** Keep `data/` and web assets loose in `build/` for instant editing without forced binary resource embedding.

---

## 3. Windowing Model: Win32 Multi-Child Architecture

```
+-------------------------------------------------------------------------------+
| Top-Level Window (g_hMainWindow)                                              |
| Menu Bar / Window Controls / Title                                           |
+------------------------------------+------------------------------------------+
| Left Panel / Toolbar:              | Central 3D Viewport:                     |
| WebView2 Child (g_hWebPanelWnd)   | Vulkan Child (g_hViewportWnd)            |
|                                    |                                          |
| - Scene Hierarchy                  | - Hardware Vulkan Swapchain              |
| - Voxel Palette Picker             | - Authoritative Unit Cubes               |
| - Inspector / Entity Properties    | - 16mm Humanoid Models (Phase 11)        |
| - DAG Node Canvas (M3)             | - Terrain & Roads (Phase 8)              |
| - Visuals & Post Menu              | - Debris & Particles                     |
|                                    | - Reticle / Crosshair                    |
+------------------------------------+------------------------------------------+
| Bottom Dock / Status: Console / Log / Telemetry (WebView2 or Win32 splitter) |
+-------------------------------------------------------------------------------+
```

### 3.1 Window Hierarchy
```cpp
// 1. Top-Level Shell Window
g_hMainWindow = CreateWindowExA(
    0, "VoxelAppClass", "Voxel FPS Engine 0.0",
    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
    CW_USEDEFAULT, CW_USEDEFAULT, width, height,
    nullptr, nullptr, g_hInstance, nullptr
);

// 2. Child Viewport for Vulkan Rendering
g_hViewportWnd = CreateWindowExA(
    0, "VoxelViewportClass", nullptr,
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
    viewportX, viewportY, viewportW, viewportH,
    g_hMainWindow, (HMENU)IDC_VIEWPORT, g_hInstance, nullptr
);

// 3. Child Container for Microsoft WebView2
g_hWebPanelWnd = CreateWindowExA(
    0, "VoxelWebPanelClass", nullptr,
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
    panelX, panelY, panelW, panelH,
    g_hMainWindow, (HMENU)IDC_WEBPANEL, g_hInstance, nullptr
);
```

### 3.2 Binding Vulkan to the Child HWND
In [`src/main.cpp`](../src/main.cpp#L2334), Vulkan creates its surface from an `HWND`. In this model, `ci.hwnd` points directly to the child viewport:

```cpp
VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
ci.hwnd = g_hViewportWnd; // Bound to child, not top-level
ci.hinstance = g_hInstance;
if (vkCreateWin32SurfaceKHR(g_instance, &ci, nullptr, &g_surface) != VK_SUCCESS) {
    fail("vkCreateWin32SurfaceKHR on child viewport failed");
}
```

### 3.3 Resize & Dock Splitter Management
On `WM_SIZE` or when dragging a splitter bar in `g_hMainWindow`:
1. Calculate target rectangles for `g_hViewportWnd` and `g_hWebPanelWnd`.
2. Update child positions via `SetWindowPos` or `DeferWindowPos`.
3. Update WebView2 controller bounds via `pController->put_Bounds(webRect)`.
4. Trigger Vulkan swapchain recreation flag (`g_resized = true;`) so Vulkan rebuilds swapchain buffers matching the new `g_hViewportWnd` dimensions.

---

## 4. Microsoft WebView2 Integration

### 4.1 Initialization & Hosting
Microsoft WebView2 (Evergreen Runtime) is bundled with modern Windows 10/11. The engine links `WebView2LoaderStatic.lib` or loads `WebView2Loader.dll` dynamically.

```cpp
CreateCoreWebView2EnvironmentWithOptions(
    nullptr, // System runtime
    L"build/webview_profile", // Isolated user data folder
    nullptr,
    Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hParent](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            env->CreateCoreWebView2Controller(
                hParent,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                        g_webController = controller;
                        controller->get_CoreWebView2(&g_webView);
                        setupWebViewBridge();
                        navigateUiHome();
                        return S_OK;
                    }
                ).Get()
            );
            return S_OK;
        }
    ).Get()
);
```

### 4.2 Local Asset Resolution (No HTTP Server Needed in Production)
In release mode, WebView2 maps local files directly to an origin without requiring an embedded HTTP web server:
```cpp
g_webView->SetVirtualHostNameToFolderMapping(
    L"imagoro.engine",
    L"data/ui/dist",
    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
);
g_webView->Navigate(L"https://imagoro.engine/index.html");
```
In development mode, `voxel_engine.exe` navigates to Vite's dev server:
```cpp
g_webView->Navigate(L"http://localhost:8790");
```

---

## 5. Sim/View Boundary & Bidirectional IPC Bridge

Per [`RULES.md`](../RULES.md), the simulation is strictly isolated. The UI resides in the **View / Overlay layer**.

```
+------------------------------------------------------------------------------------+
|                                  BROWSER PROCESS                                   |
|  imagoro-ui (React)  <-->  EventBus  <-->  window.chrome.webview.postMessage       |
+-----------------------------------------|------------------------------------------+
                                          | JSON-RPC Messages
+-----------------------------------------|------------------------------------------+
|  ICoreWebView2::WebMessageReceived      |                                          |
|                                         v                                          |
|                              [ UiActionQueue (Thread-Safe) ]                        |
|                                         |                                          |
|                                         v                                          |
|  ENGINE MAIN LOOP                                                                  |
|  1. Drain UiActionQueue at tick start                                              |
|  2. Convert to authoritative SimInput or EditorTransaction                         |
|  3. Step Simulation (Fixed 60/120 Hz, Deterministic)                                |
|  4. Generate View Snapshot                                                         |
|  5. Render Vulkan Frame                                                            |
|  6. Dispatch View Snapshot to UI -> ICoreWebView2::PostWebMessageAsJson           |
|                                                                                    |
|                                NATIVE ENGINE PROCESS                               |
+------------------------------------------------------------------------------------+
```

### 5.1 UI to Engine (Command Dispatch)
Actions triggered in React (palette selection, entity editing, camera focus, visual params) are sent via `postMessage`:
```ts
// React (imagoro-ui)
window.chrome.webview.postMessage({
  type: "ACTION_PAINT_VOXEL",
  payload: {
    cell: [12, 1, 40],
    material: 2,
    paletteIndex: 14
  }
});
```

On the C++ side, `WebMessageReceived` pushes the command into a thread-safe queue:
```cpp
// C++ Engine Bridge
g_webView->add_WebMessageReceived(
    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            LPWSTR jsonRaw;
            args->get_WebMessageAsJson(&jsonRaw);
            std::string msg = utf16ToUtf8(jsonRaw);
            CoTaskMemFree(jsonRaw);

            g_uiActionQueue.push(parseUiAction(msg));
            return S_OK;
        }
    ).Get(),
    &g_webMessageToken
);
```
At the start of the next simulation/frame tick:
- **Game/Sim Actions:** Appended to `SimInput` queue (authoritative, verified against game rules).
- **Editor Actions:** Executed via an `EditorTransaction` that records an undo/redo step, stamps occupancy, and dirties the corresponding chunk mesh.
- **Visual Param Actions:** Updates `VisualParams` directly on the view side (without touching sim).

### 5.2 Engine to UI (View Snapshot Stream)
The engine posts read-only telemetry, hierarchy, and selection snapshots back to React at 60 Hz or upon state change:
```cpp
void broadcastViewSnapshot(const ViewSnapshot& snap) {
    if (!g_webView) return;
    std::string json = serializeSnapshot(snap);
    std::wstring wideJson = utf8ToUtf16(json);
    g_webView->PostWebMessageAsJson(wideJson.c_str());
}
```

In `imagoro-ui`, incoming messages are piped directly to its core `EventBus`:
```ts
// packages/core / apps/command-center
window.chrome.webview.addEventListener('message', (event) => {
    bus.dispatch(event.data);
});
```

---

## 6. Execution Modes & CLI Contract

The single executable `voxel_engine.exe` supports four discrete modes:

| Mode Flag | Viewport Layout | WebView2 Shell | Dear ImGui | Use Case |
|---|---|---|---|---|
| `--mode engine` *(default)* | 100% Client Rect | Hidden / Inactive | Optional (Esc) | Fullscreen/Windowed player runtime |
| `--mode editor` | Docked alongside panels | Active (Tabs & Tools) | Disabled | Unified development environment |
| `--mode validate` | Headless (No HWND) | None | None | Runs SmokeAll, stress suites, exit codes 1–10 |
| `--mode headless` | Headless (Offscreen) | None | None | CLI map export, `--capture` generation |

---

## 7. Java Painter Coexistence & Migration Plan

The user specifically noted: *"do we absolutely have to forego java? i was hoping to incorporate imagoro-ui as the basis."*

**No, Java is not dropped.** The transition maintains complete data compatibility:
1. **Shared Schema (`voxfmt v2`):**
   Both `java/painter/` and `imagoro-ui` read and write identical `.vox.json` files:
   - JSON header: schema version, dimensions, seed, terrain class, lighting/environment layer, entity descriptors.
   - RLE payload: compact run-length encoded voxel occupancy and appearance palettes.
2. **External Tooling Support:**
   The `imagoro-ui` editor menu includes an "Open in Java Painter" action:
   ```ts
   action: "LAUNCH_EXTERNAL_TOOL",
   tool: "java_painter",
   target: activeMapPath
   ```
   The engine process monitors the active map file via Win32 `ReadDirectoryChangesW`. When the Java painter saves a file, the C++ engine hot-reloads the voxel chunks in place.
3. **Phased Block Porting:**
   As documented in [`recov-iui/packages/adapter-jvm/README.md`](file:///c:/Users/gryph/kepler/worktrees/recov-iui/packages/adapter-jvm/README.md), the Java panels (`ToolDockPanel`, `OrbitPreviewPanel`, `OrthoSlicePanel`) will be incrementally reimplemented as React blocks in `@imagoro/blocks-*`. Once feature parity is achieved, developers can use either tool interchangeably.

---

## 8. Build Pipeline & Dependency Packaging

The build pipeline unifies under [`scripts/build.ps1`](../scripts/build.ps1):

1. **Prerequisites Check:**
   - Detects MSVC / Clang, Vulkan SDK (`glslc`), and Node/pnpm.
2. **Build Steps:**
   ```powershell
   # 1. Compile Shaders
   glslc shaders/voxel.vert -o build/shaders/voxel.vert.spv
   glslc shaders/voxel.frag -o build/shaders/voxel.frag.spv
   glslc shaders/char.vert  -o build/shaders/char.vert.spv

   # 2. Build imagoro-ui Assets (if modified or not present)
   if (-not $SkipUI) {
       pnpm --filter @imagoro/command-center build
       # Distributes to build/data/ui/dist
   }

   # 3. Compile C++ Monolith
   cl.exe /O2 /std:c++20 /I src /I third_party/imgui ... src/main.cpp `
       /link /LIBPATH:third_party/webview2 WebView2LoaderStatic.lib ... `
       /OUT:build/voxel_engine.exe
   ```
3. **Loose Files Policy Preserved:**
   `build/` contains:
   - `voxel_engine.exe`
   - `data/` (maps, items, prefabs, loose and editable)
   - `data/ui/dist/` (static HTML/JS/CSS bundle)
   - `shaders/` (`.spv` bytecode)

---

## 9. Phased Implementation Roadmap

* **Phase E.1: Win32 Multi-Child Shell Harness**
  - Refactor window creation in `src/main.cpp` into top-level shell + child viewport `HWND`.
  - Validate that Vulkan swapchain and input handling continue operating with zero regressions on existing gates (`SmokeAll`).
* **Phase E.2: WebView2 Host Integration**
  - Integrate WebView2 loader; initialize `ICoreWebView2Controller` into child panel `HWND`.
  - Test static HTML loading from `data/ui/dist/`.
* **Phase E.3: Bridge & EventBus Wiring**
  - Implement C++ `UiActionQueue` and JSON serialization for view snapshots.
  - Wire `imagoro-ui` `EventBus` to `window.chrome.webview`.
* **Phase E.4: Core Editor Blocks Integration**
  - Mount Viewport Control block, Visual Params block (replacing Dear ImGui in editor mode), and Scene Hierarchy block.
  - Wire voxel raycast pick/hover from Vulkan to UI inspector.
* **Phase E.5: Map & Prefab Authoring Parity**
  - Connect voxel painting commands and prefab stamping to `EditorTransaction`.
  - Hot-reload sync with Java painter via `voxfmt v2`.

---

## 10. Verification & Quality Gates

Any implementation of this RFC must satisfy:
1. **Deterministic Sim Invariance:** `sim_fingerprint` must be bit-identical run to run, **regardless of frame rate**: `--smoke` → `c74104775f89ae9f`, `--stress` → `102f4bb7f500cd54` (`.\launch.ps1 -Action SmokeAll` / `-Action Stress`; read `build\smoke_ok.txt` / `build\stress_ok.txt`). Refreshed 2026-10-16 at the p12 tip. The previous `main` baselines were `48052f2f495b732d` / `d0d27027a069de8d`; this branch changes the sim (e.g. `warehouse_v2` is the default map), so they no longer apply. To check frame-rate independence, compare a solo `--stress` run against one with a second engine instance running alongside: the frame counts differ but the fingerprint must not. Any authored or interpolated *view* state (animation pose, eye interpolation, mesh) reaching a sim-tick code path breaks this; see the 2026-10-16 regression, where authored animation fed `syncCameraToPlayer()`.
2. **View Isolation:** `scripts/check_view_isolation.ps1` must confirm that no UI header or WebView2 symbol is included or referenced by any simulation module (`sim_*.hpp`, `ballistics.hpp`, `movement.hpp`).
3. **Exit Code Compatibility:** Headless modes must strictly preserve standard exit codes (0 = Success, 1 = Exception, 2 = Movement, 3 = Jsonx, 4 = Map, 5 = Mesh workers, 6 = Ballistics, 7 = Incomplete run, 8 = Pause/Eye, 9 = Visual params, 10 = Terrain).
