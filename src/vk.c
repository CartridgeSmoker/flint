#include "vk.h"
#include "wl.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>

bool vk_init(struct Vk* vk, struct Wl* wl) {
    memset(vk, 0, sizeof(*vk));

    // Surface extensions the instance needs.
    // The generic surface extension plus the
    // Wayland binding. Surface creation is
    // unavailable without both enabled.
    const char* exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };

    // App metadata for the loader. sType tags
    // the struct type. Names and versions are
    // informational, apiVersion sets the API
    // contract the code is written against.
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Flint";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Flint";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    // Instance request. Links the app metadata
    // and enables the extensions above. A NULL
    // allocator selects the default allocator.
    VkInstanceCreateInfo create_info = {0};
    create_info.sType =
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = 2;
    create_info.ppEnabledExtensionNames = exts;

    VkResult result =
        vkCreateInstance(&create_info, NULL, &vk->inst);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateInstance failed with error code %d\n",
            result);
        return false;
    }

    // Enumerate over physical devices.
    // First call to get the number of devices.
    uint32_t ndev = 0;
    result =
        vkEnumeratePhysicalDevices(vk->inst, &ndev, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr,
                "vkEnumeratePhysicalDevices failed with "
                "error code %d\n",
                result);
        return false;
    }
    // Capping the number of devices at 8.
    if (ndev > 8) {
        ndev = 8;
    }

    // Second call to get the devices.
    VkPhysicalDevice devices[8] = {0};
    result = vkEnumeratePhysicalDevices(vk->inst, &ndev,
                                        devices);
    if (result != VK_SUCCESS) {
        fprintf(stderr,
                "vkEnumeratePhysicalDevices failed with "
                "error code %d\n",
                result);
        return false;
    }

    if (ndev == 0) {
        fprintf(stderr,
                "No accelerator devices were found");
        return false;
    }

    // Pick device + queue family.
    uint32_t picked = 0;
    uint32_t qfam = 0;
    bool found = false;
    // Try each GPU in turn until one offers a
    // graphics queue able to present to Wayland.
    for (size_t i = 0; i < ndev && !found; i++) {
        VkQueueFamilyProperties fams[8] = {0};

        VkPhysicalDeviceProperties props = {0};
        vkGetPhysicalDeviceProperties(devices[i], &props);
        // nfam is the number of queue families
        // this GPU exposes. Families group queues
        // by capability, such as graphics or
        // compute. Count first, fill after.
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &nfam, NULL);

        // Capping nfam at 8.
        if (nfam > 8) {
            nfam = 8;
        }
        // Second call to fetch the families.
        vkGetPhysicalDeviceQueueFamilyProperties(
            devices[i], &nfam, fams);

        // Test each family: keep graphics-capable
        // families with Wayland present support.
        // Record the winners in picked and qfam.
        for (size_t f = 0; f < nfam; f++) {
            // Making sure we find the graphics specialized
            // queue.
            if (!(fams[f].queueFlags &
                  VK_QUEUE_GRAPHICS_BIT))
                continue;

            if (!vkGetPhysicalDeviceWaylandPresentationSupportKHR(
                    devices[i], f, wl->display))
                continue;

            picked = i;
            qfam = f;
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(
            stderr,
            "No fitting accelerator devices were found");
        return false;
    }
    vk->phys = devices[picked];

    // Logical device and queue.

    return true;
}

void vk_finish(struct Vk* vk) {
    if (vk->inst != VK_NULL_HANDLE) {
        vkDestroyInstance(vk->inst, NULL);
        vk->inst = VK_NULL_HANDLE;
    }
}
