# Developing

## Install dependencies

Flint needs the Wayland client libraries, the
xdg-shell protocol definitions, and the Vulkan
headers and loader.

Fedora:

```sh
sudo dnf install wayland-devel \
  wayland-protocols-devel vulkan-headers \
  vulkan-loader-devel libxkbcommon-devel
```

Ubuntu (matches CI):

```sh
sudo apt-get install libwayland-dev \
  wayland-protocols libvulkan-dev
```

Check the install with:

```sh
pkg-config --modversion wayland-client \
  wayland-scanner vulkan xkbcommon
which wayland-scanner
```

## Toolchain

C sources build as C23 (`-std=c23` in `CFLAGS`).
C++ sources build as C++23 (`-std=c++23` in
`CXXFLAGS`), both with `-pedantic`: strict ISO
dialects only, no GNU extensions. You need a
compiler pair that accepts both, e.g. recent
GCC or Clang for `CC` and `CXX`. The link step
always uses `$(CXX)` so the `src/ui.cpp`
object resolves libstdc++ without extra
flags.

## Build and check

```sh
make        # build ./bin/flint
make check  # build and run the smoke check
make clean  # remove bin/ and dist/
```

`make check` runs `./bin/flint` directly. On a
machine with no Wayland display,
`fl_app_create` returns `FL_ERR_NODISPLAY`,
`main.c` prints `no display, skipping` and exits
0, so the check passes as a headless skip rather
than failing. Any other failure (including
`FL_ERR_NOVULKAN` when Vulkan setup fails) exits
1.

## Validation layers

Debug builds (the default, without `-DNDEBUG`)
enable `VK_LAYER_KHRONOS_validation` and an
`EXT_debug_utils` messenger that logs warnings
and errors to stderr; see `vk_init` and
`vk_finish` in `src/vk.c`. Pass `-DNDEBUG` in
`CFLAGS` for a release build without validation.
The `vulkan-validationlayers` package (or your
distro equivalent) must be installed for debug
builds to create the instance successfully.

## UI sources

C++ lives only in the retained UI layer:
public header `include/flint/ui.h`, opaque
`fl_widget_t`, C API with `extern "C"`, C++23
RAII internals in `src/ui.cpp` (`std::string`,
`std::vector`, `std::unique_ptr`, all exceptions
caught at the boundary and returned as
`FL_ERR_NOMEM`), no exceptions crossing the
boundary. `Makefile` lists UI sources via
`$(wildcard src/ui.cpp)` and the `%.o: %.cpp`
rule builds them with `$(CXX)`.

## Protocol code

The `xdg-shell` XML ships with
`wayland-protocols-devel` under
`/usr/share/wayland-protocols/stable/` as
`xdg-shell/xdg-shell.xml`.
Generate the client header and private code
with `wayland-scanner` into `bin/` at build
time. Never commit generated files.

## Style

C23 for C sources, C++23 for UI C++ sources,
both `-pedantic`, warnings as errors
(`-Wall -Wextra -Werror`). Format with `clang-format`
(`ColumnLimit: 60`). Public API lives in
`include/flint/`, internals in `src/`.
