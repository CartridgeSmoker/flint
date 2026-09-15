#ifndef VK_H
#define VK_H

#include <stdbool.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

struct Wl;

struct Vk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    // Wayland surface.
    VkSurfaceKHR surf;
};

// vk_init creates the instance, picks a device
// with Wayland present support, then creates
// the device and surface. False on failure.
bool vk_init(struct Vk* vk, struct Wl* wl);

// vk_finish destroys surface, device, and instance.
void vk_finish(struct Vk* vk);

#endif
