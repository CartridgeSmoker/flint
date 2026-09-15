# Invariants

1. Wayland only. Flint targets Linux and Wayland. No X11 fallback or compatibility path.
2. Stable C ABI from day one. The public API uses opaque handles. Implementation structs and Vulkan types are not exposed in public headers.

