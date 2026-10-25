# Chapter 3: Validation and Debugging

> **Objectives:**
> - Explain why Vulkan ships with validation off.
> - Enable `VK_LAYER_KHRONOS_validation` at instance creation.
> - Set up a `VkDebugUtilsMessengerEXT` callback in correct C.
> - Filter by severity and type without hiding errors.
> - Read the first validation messages and fix them.

The previous chapters got you a working instance, GPU, and device with a graphics queue. That handshake succeeds silently even when you misuse the API, because Vulkan checks almost nothing on its own. This chapter adds the safety net: validation layers plus a debug messenger that tells you what you did wrong.

## 1. Why Vulkan trusts you by default

Older graphics APIs validated every call as it arrived. That caught mistakes early, but every application paid for the checks forever, including tested releases where they found nothing new.

Vulkan moves that cost off the critical path. The driver assumes you validated during development and performs only minimum checking at runtime. Pass a bad handle, a wrong `sType`, or a forbidden `NULL`, and the likely result is a crash, corruption, or silence rather than a helpful error code.

Validation layers restore the checks as an opt-in layer between your code and the driver: parameter checks, object lifetime tracking, threading flags, and slow-pattern warnings. Debug builds enable them; release builds do not. Developing without them is working blind, so wire them up now and leave them on until Chapter 4 and beyond.

## 2. One layer, one extension

Older SDKs shipped half a dozen separate layers with names like `VK_LAYER_LUNARG_core_validation`, `VK_LAYER_LUNARG_parameter_validation`, `VK_LAYER_LUNARG_object_tracker`, and `VK_LAYER_GOOGLE_threading`. Current SDKs roll all of that into one meta-layer:

```c
const char *layers[] = { "VK_LAYER_KHRONOS_validation" };
```

The rename from `LUNARG` to `KHRONOS` marks that consolidation: if you see the old names in older samples, treat them as parts of the single modern layer. Request it in `VkInstanceCreateInfo`, and only there. Layers are instance-level; per-device layers are deprecated. Enumerate first and enable second, as in earlier chapters: an unadvertised name fails with `VK_ERROR_LAYER_NOT_PRESENT`.

Validation also needs an instance extension that carries the messages. The modern one is `VK_EXT_debug_utils`; the older `VK_EXT_debug_report` is legacy. Enable it alongside your surface extensions:

```c
const char *exts[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
};

VkInstanceCreateInfo createInfo = {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pNext = NULL,
    .pApplicationInfo = &appInfo,
    .enabledLayerCount = 1,
    .ppEnabledLayerNames = layers,
    .enabledExtensionCount = 3,
    .ppEnabledExtensionNames = exts,
};
```

Without the extension, the messenger functions do not exist and loading them returns `NULL`. Without the layer, the messenger exists but has almost nothing to say. You need both halves.

> **Misconception:** "Listing validation, or creating the messenger, is what turns checks on."
>
> Neither step alone suffices. Enumeration only reports what exists; the messenger is only a mailbox. Checks run only when the layer name is in `ppEnabledLayerNames` at `vkCreateInstance` time *and* a messenger receives the output. Skip either half and failures stay silent.

> **Flint note:** Flint debug builds always enable `VK_LAYER_KHRONOS_validation` plus `VK_EXT_debug_utils` with a messenger logging to stderr. Release builds enable neither. If you add an instance extension for Flint, add it next to that debug list, not in place of it.

## 3. Setting up the messenger correctly

Debug utilities are extension functions, so the loader does not link them statically. Load them through `vkGetInstanceProcAddr` after the instance exists:

```c
static PFN_vkCreateDebugUtilsMessengerEXT appCreateMessenger = NULL;
static PFN_vkDestroyDebugUtilsMessengerEXT appDestroyMessenger = NULL;

appCreateMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)
    vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
appDestroyMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)
    vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
if (!appCreateMessenger || !appDestroyMessenger) {
    fprintf(stderr, "debug utils extension not present\n");
    return 1;
}
```

Next write the callback. The signature must match exactly; a near-miss compiles under a cast and then misbehaves at runtime:

```c
static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *userData)
{
    (void) severity;
    (void) types;
    (void) userData;

    fprintf(stderr, "[validation] %s\n", data->pMessage);
    return VK_FALSE;
}
```

Returning `VK_FALSE` lets the triggering call continue so one run surfaces every complaint. Returning `VK_TRUE` asks the layer to abort that call with `VK_ERROR_VALIDATION_FAILED_EXT`, which buries later errors behind the first. During development, always return `VK_FALSE`. Then describe which messages you want and create the messenger:

```c
VkDebugUtilsMessengerCreateInfoEXT msgInfo = {
    .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
    .pNext = NULL,
    .flags = 0,
    .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
    .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
        | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
    .pfnUserCallback = debugCallback,
    .pUserData = NULL,
};

VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
VkResult res = appCreateMessenger(instance, &msgInfo, NULL, &messenger);
if (res != VK_SUCCESS) {
    fprintf(stderr, "messenger creation failed: %d\n", res);
    return 1;
}
```

Two lifetime rules matter. Destroy the messenger before the instance, or shutdown itself reports leaks. To catch messages from `vkCreateInstance` itself, chain the same `msgInfo` into `pNext` of `VkInstanceCreateInfo`; without that chain, the earliest setup mistakes predate the messenger and go unreported.

## 4. Severities, types, and filtering without self-deception

Every message carries a severity and a type. Severity answers how bad: `VERBOSE` traces, `INFO` narrates, `WARNING` flags risky but defined usage, `ERROR` marks specification violations. Type answers which concern: `GENERAL` for events, `VALIDATION` for rules, `PERFORMANCE` for legal but slow patterns.

Start with warnings plus errors across all three types, as above. Add `INFO` and `VERBOSE` only when hunting a specific mystery, then turn them back off; a permanently verbose log trains you to skim. Likewise, keep `PERFORMANCE` on during daily work: it is tempting to silence slow-pattern warnings once the image looks right, but that hides the exact advice Chapter 4 on memory will build on.

## 5. The first errors you will meet, and how to read them

A validation message names the offending call, quotes the violated rule, and lists the objects involved. Read the first line for the verdict and the object list for which handle to inspect:

```
[validation ERROR] VUID-VkApplicationInfo-sType-sType: missing sType ...
```

That means a structure left `sType` unset. The fix is one field, not a redesign.

Expect this parade in your first week:

- `VK_ERROR_LAYER_NOT_PRESENT` from `vkCreateInstance`. The layer name is misspelled or SDK layers are not installed. Print the enumerated names and compare.
- `VK_ERROR_EXTENSION_NOT_PRESENT`. You asked for `VK_EXT_debug_utils` or a surface extension the loader cannot see. Same remedy: enumerate and compare.
- `NULL` messenger pointers. You skipped the extension, so `vkGetInstanceProcAddr` found nothing. Fix the extension list, not the loading code.
- Missing `sType` or `pNext` misuse. Zero-initialize structures so untouched fields are `NULL` and zero.
- Object leaks at shutdown. `OBJECT 0x... not destroyed` means teardown ran in the wrong order: destroy everything else first, the messenger second to last, the instance last.
- Stale or foreign handles. The object list identifies the handle; check which device created it and whether teardown already ran.

Fix errors in reported order. The first violation often causes the next three.

## 6. Keep validation on until the code earns its silence

Run validation from the first triangle, not from the first crash. Gate new code on a clean log: no errors, no unexplained warnings. The release build removes the checks, so the final pass must happen with them on: exercise every path, read the log, then cut the release. Chapter 4 puts this discipline to work, because explicit memory allocation punishes every unchecked mistake with corruption rather than a diagnostic.

---

### Recap

- Vulkan validates nothing by default for speed; validation layers restore the checks as an opt-in cost paid only during development.
- Enable the single modern layer `VK_LAYER_KHRONOS_validation` on the instance, with `VK_EXT_debug_utils` alongside the surface extensions; enumerate before enabling.
- Load the messenger functions via `vkGetInstanceProcAddr`, return `VK_FALSE` from the callback, chain the create info into `pNext`, and destroy the messenger before the instance.
- Filter by severity (`VERBOSE`, `INFO`, `WARNING`, `ERROR`) and type (`GENERAL`, `VALIDATION`, `PERFORMANCE`); default to warnings plus errors on all types.
- Read messages verdict-first, fix in reported order, and keep validation running from the first frame into Chapter 4.

### Exercises

1. **Wire the messenger and break it on purpose.** Register the callback, chain `msgInfo` into `pNext`, then zero out one `sType` before a create call. Confirm the `ERROR` names the call and the rule. Then destroy the messenger after the instance, note the leak report, and restore the order.
2. **Tune the filter.** Run with warnings-plus-errors, then with `INFO` added, then with `PERFORMANCE` removed. Count messages each time and pick one that changed. Which setting do you keep for daily work, and what does each alternative hide?
