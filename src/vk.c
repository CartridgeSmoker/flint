#include "vk.h"
#include <stdbool.h>
#include <stdio.h>
#include <vulkan/vulkan_core.h>

bool vk_init(struct Vk* vk, struct Wl* wl) {
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Flint";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "Flint";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info = {0};
    create_info.sType =
        VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    VkResult result =
        vkCreateInstance(&create_info, NULL, &vk->inst);
    if (result != VK_SUCCESS) {
        fprintf(
            stderr,
            "vkCreateInstance failed with error code %d\n",
            result);
    }

    return true;
}

void vk_finish(struct Vk* vk) {
    if (vk->inst != VK_NULL_HANDLE) {
        vkDestroyInstance(vk->inst, NULL);
    }
};
