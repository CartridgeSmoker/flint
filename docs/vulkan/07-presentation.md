# Chapter 7: Presentation

> **Objectives:**
> - Explain why presentation lives in WSI extensions rather than Vulkan core, and which extensions Flint needs.
> - Create a Wayland surface with `VK_KHR_wayland_surface` and check queue-family present support.
> - Describe swapchain images, views, and the application-owned depth image.
> - Create a swapchain with the right image count, extent, format, and present mode.
> - Gate `TRANSFER_DST` usage on `supportedUsageFlags` and keep format and usage in agreement.
> - Run the acquire, record, submit, present loop with correct semaphores, layouts, and `VkResult` branches.

Chapters 4 through 6 built images nobody can see. This chapter puts them on screen: each window system owns its windows, so Vulkan talks to it through Window System Integration (WSI) extensions. Learn the common core once, bind one platform extension, and the rest is the same acquire-draw-present loop everywhere.

## 1. Presentation is an extension, not core

Not every Vulkan device has a screen, so presentation was kept out of core. The shared ground lives in `VK_KHR_surface`, which introduces the `VkSurfaceKHR` handle: Vulkan's view of a native window. Each platform adds one small extension binding a native window type to a surface: `VK_KHR_wayland_surface` on Wayland, `VK_KHR_xcb_surface` and `VK_KHR_xlib_surface` on X, `VK_KHR_win32_surface` on Windows. This guide covers the Wayland path that Flint uses; the rest of the chapter is platform neutral.

Enable both at instance creation: `VK_KHR_surface` plus one platform extension per surface:

```c
const char *instanceExts[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
};
```

Device side, the swapchain is also an extension: `VK_KHR_swapchain` must appear in `VkDeviceCreateInfo`. Forgetting either half is the classic first-presentation failure.

## 2. Surfaces: Vulkan's view of a window

On Wayland, the client already owns a `wl_surface`; Vulkan wraps it so driver and compositor agree on buffers:

```c
VkWaylandSurfaceCreateInfoKHR surfaceInfo = { 0 };
surfaceInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
surfaceInfo.display = wlDisplay;
surfaceInfo.surface = wlSurface;

VkSurfaceKHR surface = VK_NULL_HANDLE;
vkCreateWaylandSurfaceKHR(instance, &surfaceInfo, NULL, &surface);
```

The `wl_display` and `wl_surface` must stay alive as long as the `VkSurfaceKHR` exists.

Not every queue presents. Before `vkQueuePresentKHR`, ask whether the family reaches this surface:

```c
VkBool32 supported = VK_FALSE;
vkGetPhysicalDeviceSurfaceSupportKHR(physDev, queueFamilyIndex,
    surface, &supported);
```

Pick a family supporting both `VK_QUEUE_GRAPHICS_BIT` and presentation when you can; one combined family avoids ownership transfers. Otherwise use `VK_SHARING_MODE_CONCURRENT` with both family indices. One combined family is simpler and is what Flint uses.

> **Misconception:** "Once I have a surface, any queue can present to it."
>
> No. Present support is per queue family per surface. A compute-only family, or a graphics family on a second GPU with no display path, returns `VK_FALSE`. Always query `vkGetPhysicalDeviceSurfaceSupportKHR` for the actual surface and choose or create the device around a family that answers true.

## 3. Views are the only way the framebuffer sees an image

Shader stages and framebuffers reach images only through a `VkImageView`: one image, one format, one view type, one subresource range. Copy, blit, resolve, and clear still take the `VkImage` itself. Every swapchain image needs one color view before it can serve as a framebuffer attachment:

```c
VkImageViewCreateInfo viewInfo = { 0 };
viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
viewInfo.image = swapImages[i];
viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
viewInfo.format = swapFormat;
viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
viewInfo.subresourceRange.baseMipLevel = 0;
viewInfo.subresourceRange.levelCount = 1;
viewInfo.subresourceRange.baseArrayLayer = 0;
viewInfo.subresourceRange.layerCount = 1;

VkImageView view = VK_NULL_HANDLE;
vkCreateImageView(device, &viewInfo, NULL, &view);
```

Keep `format` compatible with the image format unless the image was created with `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`. Format and usage must agree end to end: a layout such as `COLOR_ATTACHMENT_OPTIMAL` is only legal when the image carries `COLOR_ATTACHMENT_BIT`. Destroy views with `vkDestroyImageView` before the swapchain they belong to.

The depth image is the mirror case: your program creates, backs, transitions, and views it. Pick a depth format with `vkGetPhysicalDeviceFormatProperties`: try `D32_SFLOAT`, fall back to `D24_UNORM_S8_UINT` or `D16_UNORM`:

```c
VkImageCreateInfo depthInfo = { 0 };
depthInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
depthInfo.imageType = VK_IMAGE_TYPE_2D;
depthInfo.format = depthFormat;
depthInfo.extent.width = extent.width;
depthInfo.extent.height = extent.height;
depthInfo.extent.depth = 1;
depthInfo.mipLevels = 1;
depthInfo.arrayLayers = 1;
depthInfo.samples = VK_SAMPLE_COUNT_1_BIT;
depthInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
depthInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
depthInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
depthInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

VkImage depthImage = VK_NULL_HANDLE;
vkCreateImage(device, &depthInfo, NULL, &depthImage);
/* Allocate device-local memory, bind, then barrier. */
```

Record that barrier on a setup buffer before first use. Rebuild image and view at every recreation, since their size tracks the extent.

## 4. Creating the swapchain: count, size, format, mode

The surface is a destination; the **swapchain** is the queue of images the window system lends you. Query capabilities, formats, and present modes first; never hard-code them:

```c
VkSurfaceCapabilitiesKHR caps = { 0 };
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physDev, surface, &caps);

uint32_t formatCount = 0;
vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &formatCount, NULL);
VkSurfaceFormatKHR *formats = /* allocated with formatCount entries */;
vkGetPhysicalDeviceSurfaceFormatsKHR(physDev, surface, &formatCount, formats);
/* VK_INCOMPLETE means the list grew; resize and re-query. */
```

Four choices matter most.

**Image count.** Request `caps.minImageCount + 1`, clamped to `caps.maxImageCount` (zero means no limit). The extra image keeps the pipeline fed; three is the usual triple-buffered sweet spot.

**Extent.** If `caps.currentExtent.width` is not `UINT32_MAX`, use exactly that extent. Otherwise use the window size clamped into `minImageExtent` and `maxImageExtent`.

**Format.** Walk the list for `VK_FORMAT_B8G8R8A8_SRGB` with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`, falling back to the first entry. A lone `VK_FORMAT_UNDEFINED` entry means no preference; fall back the same way.

**Present mode.** Query `vkGetPhysicalDeviceSurfacePresentModesKHR` and pick deliberately:

- `VK_PRESENT_MODE_FIFO_KHR` is always available: each image waits at vertical blank, no tearing, rate capped at the display. This is vsync on.
- `VK_PRESENT_MODE_FIFO_RELAXED_KHR` waits when on schedule but shows a late frame at once (tearing possible) instead of stalling a cycle; optional support.
- `VK_PRESENT_MODE_MAILBOX_KHR` keeps one pending slot: a new image replaces the waiting one. No tearing, display-rate pacing, late frames skipped.
- `VK_PRESENT_MODE_IMMEDIATE_KHR` shows at once, mid-scanout if needed: uncapped rate, tearing, lowest latency.

Then create, gating transfer usage on what the surface actually supports:

```c
VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
    imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

VkSwapchainCreateInfoKHR swapInfo = { 0 };
swapInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
swapInfo.surface = surface;
swapInfo.minImageCount = imageCount;
swapInfo.imageFormat = chosenFormat.format;
swapInfo.imageColorSpace = chosenFormat.colorSpace;
swapInfo.imageExtent = extent;
swapInfo.imageArrayLayers = 1;
swapInfo.imageUsage = imageUsage;
swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
swapInfo.preTransform = caps.currentTransform;
swapInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
swapInfo.presentMode = presentMode;
swapInfo.clipped = VK_TRUE;
swapInfo.oldSwapchain = VK_NULL_HANDLE;

VkSwapchainKHR swapchain = VK_NULL_HANDLE;
vkCreateSwapchainKHR(device, &swapInfo, NULL, &swapchain);

uint32_t imageCountOut = 0;
vkGetSwapchainImagesKHR(device, swapchain, &imageCountOut, NULL);
VkImage *swapImages = /* allocated with imageCountOut entries */;
vkGetSwapchainImagesKHR(device, swapchain, &imageCountOut, swapImages);
```

Notes on the rest: keep `currentTransform` (prefer `IDENTITY` when supported); `OPAQUE` skips compositor blending; `clipped = VK_TRUE` skips obscured pixels. Swapchain images arrive pre-bound: never allocate, bind, free, or transition them at creation.

> **Flint note:** Flint requests `B8G8R8A8_SRGB` with `SRGB_NONLINEAR`, and prefers `MAILBOX` with `FIFO` fallback at `minImageCount + 1`. The Wayland compositor composites on its own cycle, so in the common case `MAILBOX` versus `FIFO` is a latency-versus-smoothness choice, not a tearing choice.

## 5. The frame loop: acquire, record, submit, present

Each frame moves an image in four steps: acquire, record, submit, present. Two semaphores order the handoffs without stalling the CPU:

```c
uint32_t imageIndex = 0;
VkResult acquireRes = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
    imageAvailable, VK_NULL_HANDLE, &imageIndex);
if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return; /* skip this frame */
}
if (acquireRes == VK_SUBOPTIMAL_KHR) {
    recreatePending = true; /* showed, but schedule a rebuild */
}

vkWaitForFences(device, 1, &frameFence, VK_TRUE, UINT64_MAX);
vkResetFences(device, 1, &frameFence);
/* Record rendering into images[imageIndex], ending in PRESENT_SRC_KHR. */

VkPipelineStageFlags waitStage =
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

VkSubmitInfo submit = { 0 };
submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
submit.waitSemaphoreCount = 1;
submit.pWaitSemaphores = &imageAvailable;
submit.pWaitDstStageMask = &waitStage;
submit.commandBufferCount = 1;
submit.pCommandBuffers = &cmd;
submit.signalSemaphoreCount = 1;
submit.pSignalSemaphores = &renderFinished;

vkQueueSubmit(graphicsQueue, 1, &submit, frameFence);

VkPresentInfoKHR present = { 0 };
present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
present.waitSemaphoreCount = 1;
present.pWaitSemaphores = &renderFinished;
present.swapchainCount = 1;
present.pSwapchains = &swapchain;
present.pImageIndices = &imageIndex;

VkResult presentRes = vkQueuePresentKHR(graphicsQueue, &present);
if (presentRes == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
} else if (presentRes == VK_SUBOPTIMAL_KHR) {
    recreatePending = true;
}
```

The contract at each edge: acquire signals `imageAvailable`; submit waits on it so rendering never starts on an image still scanned out. Rendering signals `renderFinished`; present waits on it so the compositor never shows a half-drawn frame. The fence recycles per-frame state. Images must end in `PRESENT_SRC_KHR`: barrier to `COLOR_ATTACHMENT_OPTIMAL` at the top, to `PRESENT_SRC_KHR` at the bottom. `OUT_OF_DATE` skips the frame and recreates; `SUBOPTIMAL` still showed, so recreation can wait.

## 6. Resize, out-of-date, and cleanup

Swapchains go stale on resize or compositor reclaim. Both acquire and present report it: `VK_ERROR_OUT_OF_DATE_KHR` forces recreation before continuing; `VK_SUBOPTIMAL_KHR` advises it soon.

Recreation reuses the creation sequence, passing the old handle so the driver can recycle:

```c
vkDeviceWaitIdle(device); /* frames in flight have finished */

/* Re-query caps for newExtent: fixed currentExtent,
   or window size clamped into min/maxImageExtent. */
swapInfo.imageExtent = newExtent;
swapInfo.oldSwapchain = swapchain;
VkSwapchainKHR newSwapchain = VK_NULL_HANDLE;
vkCreateSwapchainKHR(device, &swapInfo, NULL, &newSwapchain);
vkDestroySwapchainKHR(device, swapchain, NULL);
swapchain = newSwapchain;
/* Re-fetch images and rebuild views, framebuffers, depth. */
```

Destroy image views and framebuffers before the old swapchain. `vkDeviceWaitIdle` is heavy-handed but correct for an infrequent event; per-frame code should never call it. Shutdown uses the same ordering: wait idle, destroy views and framebuffers, destroy the swapchain, then `vkDestroySurfaceKHR`.

Vsync falls out of the present mode: `FIFO` is guaranteed vsync-on; `FIFO_RELAXED` tears one late frame instead of stuttering; `MAILBOX` drops a stale frame for a fresher one; `IMMEDIATE` ignores the blank. On Wayland the compositor sets its own cadence, so even `IMMEDIATE` rarely tears visibly.

## Recap

- Presentation is WSI extensions: `VK_KHR_surface` plus one platform extension, with `VK_KHR_swapchain` on the device.
- A `VkSurfaceKHR` wraps the native window; present support is per queue family per surface, so query before committing.
- Image views are the attachment-visible form of an image; each swapchain image gets one color view with matching format. The depth image is application-owned: check tiling, bind device-local memory, transition, then view.
- Swapchain creation picks count (`min + 1`), extent (fixed or clamped), format (preferred sRGB pair, fallback to first), and mode (`FIFO` always, `FIFO_RELAXED` tear-on-late, `MAILBOX` low-latency, `IMMEDIATE` tearing).
- `imageUsage` must sit inside `supportedUsageFlags`: add `TRANSFER_DST` only when present, and keep layouts consistent with usage.
- Each frame acquires with a signal semaphore, submits waiting on it and signaling render-done, and presents waiting on render-done, ending in `PRESENT_SRC_KHR`. `OUT_OF_DATE` forces recreation, `SUBOPTIMAL` schedules it.
- Recreation passes `oldSwapchain` and rebuilds views, framebuffers, and depth. Vsync is the present mode; Flint prefers `MAILBOX` with `FIFO` fallback.

## Exercises

1. **Mode survey.** Enumerate formats and present modes, then build the swapchain forcing `FIFO`, forcing `FIFO_RELAXED` where offered, and preferring `MAILBOX` with `FIFO` fallback. Record what your driver offers and explain why `FIFO` must be the fallback.
2. **Forced recreation with depth.** Render ten frames with a depth attachment, resize, and capture `OUT_OF_DATE` or `SUBOPTIMAL`. Recreate via `oldSwapchain`, rebuild image views and the depth image at the new extent, and render ten more. What broke when you skipped the rebuild?
