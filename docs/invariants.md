# Invariants

1. Linux on Wayland. Flint targets Linux with a Wayland-only windowing path. Rendering goes through a single Vulkan path. Platform code stays in internal windowing and surface helpers and never leaks into the public API.

