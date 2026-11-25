// ShaderHost - RetroArch slang shaders (librashader, Vulkan) for ports that draw with OpenGL ES.
//
// The game's SDL window draws into an AImageReader instead of the screen (ShaderSurface.java hands SDL the reader's
// Surface). Every finished frame arrives here as an AHardwareBuffer, is imported into Vulkan and drawn through the
// chosen librashader preset onto the real screen surface (a Vulkan swap chain); with no preset it is copied as it
// is. librashader is loaded at run time (liblibrashader_capi.so); without it there are no shaders, only the copy.
// Everything Vulkan runs on the reader's own callback thread; the Java side only hands surfaces and the preset over.
// Shared by this fork's GLES Android ports (installed per port by shared_shader/install.sh).
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#define LIBRA_RUNTIME_VULKAN
#include "librashader.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#define SH_CAT2(a, b) a##_##b
#define SH_CAT(a, b) SH_CAT2(a, b)
#define SH_JNI(name) SH_CAT(Java_com_izzy_kart_ShaderHost, name)

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "ShaderHost", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "ShaderHost", __VA_ARGS__)

namespace {

// ---------------------------------------------------------------- NDK media / window (API 26+, looked up at run time)

struct AImage;
struct AImageReader;
typedef void (*ImageCallback)(void* context, AImageReader* reader);
struct ImageListener {
    void* context;
    ImageCallback onImageAvailable;
};
constexpr int32_t kFormatRgbx8888 = 0x2;   // AIMAGE_FORMAT_RGBX_8888: what SDL sets the window to (no alpha)

struct Media {
    bool tried = false, ok = false;
    int32_t (*newWithUsage)(int32_t, int32_t, int32_t, uint64_t, int32_t, AImageReader**) = nullptr;
    int32_t (*getWindow)(AImageReader*, ANativeWindow**) = nullptr;
    int32_t (*setImageListener)(AImageReader*, ImageListener*) = nullptr;
    int32_t (*acquireLatestImageAsync)(AImageReader*, AImage**, int*) = nullptr;
    int32_t (*getHardwareBuffer)(const AImage*, AHardwareBuffer**) = nullptr;
    void (*deleteAsync)(AImage*, int) = nullptr;
    void (*describe)(const AHardwareBuffer*, AHardwareBuffer_Desc*) = nullptr;
    jobject (*toSurface)(JNIEnv*, ANativeWindow*) = nullptr;
} media;

bool loadMedia() {
    if (media.tried) {
        return media.ok;
    }
    media.tried = true;
    void* m = dlopen("libmediandk.so", RTLD_NOW);
    void* a = dlopen("libandroid.so", RTLD_NOW);
    if (m == nullptr || a == nullptr) {
        return false;
    }
#define LOAD(lib, field, name) media.field = reinterpret_cast<decltype(media.field)>(dlsym(lib, name))
    LOAD(m, newWithUsage, "AImageReader_newWithUsage");
    LOAD(m, getWindow, "AImageReader_getWindow");
    LOAD(m, setImageListener, "AImageReader_setImageListener");
    LOAD(m, acquireLatestImageAsync, "AImageReader_acquireLatestImageAsync");
    LOAD(m, getHardwareBuffer, "AImage_getHardwareBuffer");
    LOAD(m, deleteAsync, "AImage_deleteAsync");
    LOAD(a, describe, "AHardwareBuffer_describe");
    LOAD(a, toSurface, "ANativeWindow_toSurface");
#undef LOAD
    media.ok = media.newWithUsage && media.getWindow && media.setImageListener && media.acquireLatestImageAsync &&
               media.getHardwareBuffer && media.deleteAsync && media.describe && media.toSurface;
    return media.ok;
}

// ---------------------------------------------------------------- librashader (as in DinoTale's shaderpass)

struct Libra {
    bool tried = false, loaded = false;
    PFN_libra_instance_abi_version abiVersion = nullptr;
    PFN_libra_preset_create presetCreate = nullptr;
    PFN_libra_preset_free presetFree = nullptr;
    PFN_libra_vk_filter_chain_create chainCreate = nullptr;
    PFN_libra_vk_filter_chain_frame chainFrame = nullptr;
    PFN_libra_vk_filter_chain_free chainFree = nullptr;
    PFN_libra_error_write errorWrite = nullptr;
    PFN_libra_error_free_string errorFreeString = nullptr;
    PFN_libra_error_free errorFree = nullptr;
    PFN_libra_preset_get_runtime_params getParams = nullptr;
    PFN_libra_preset_free_runtime_params freeParams = nullptr;
    PFN_libra_preset_set_param presetSetParam = nullptr;
    PFN_libra_vk_filter_chain_set_param chainSetParam = nullptr;
} libra;

bool loadLibra() {
    if (libra.tried) {
        return libra.loaded;
    }
    libra.tried = true;
    void* lib = dlopen("liblibrashader_capi.so", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        LOGW("librashader not loaded: %s", dlerror());
        return false;
    }
#define LOAD(field, name) libra.field = reinterpret_cast<decltype(libra.field)>(dlsym(lib, name))
    LOAD(abiVersion, "libra_instance_abi_version");
    LOAD(presetCreate, "libra_preset_create");
    LOAD(presetFree, "libra_preset_free");
    LOAD(chainCreate, "libra_vk_filter_chain_create");
    LOAD(chainFrame, "libra_vk_filter_chain_frame");
    LOAD(chainFree, "libra_vk_filter_chain_free");
    LOAD(errorWrite, "libra_error_write");
    LOAD(errorFreeString, "libra_error_free_string");
    LOAD(errorFree, "libra_error_free");
    LOAD(getParams, "libra_preset_get_runtime_params");
    LOAD(freeParams, "libra_preset_free_runtime_params");
    LOAD(presetSetParam, "libra_preset_set_param");
    LOAD(chainSetParam, "libra_vk_filter_chain_set_param");
#undef LOAD
    libra.loaded = libra.abiVersion && libra.presetCreate && libra.presetFree && libra.chainCreate && libra.chainFrame &&
                   libra.chainFree && libra.errorWrite && libra.errorFreeString && libra.errorFree &&
                   libra.abiVersion() == LIBRASHADER_CURRENT_ABI;
    if (!libra.loaded) {
        LOGW("librashader found, but not the ABI this build expects (%d)", LIBRASHADER_CURRENT_ABI);
    }
    return libra.loaded;
}

std::string libraError(libra_error_t error) {
    std::string text = "librashader error";
    char* message = nullptr;
    if (libra.errorWrite(error, &message) == 0 && message != nullptr) {
        text = message;
        libra.errorFreeString(&message);
    }
    libra.errorFree(&error);
    return text;
}

// ---------------------------------------------------------------- state

// A shader parameter (#pragma parameter) of the preset in use.
struct Param {
    std::string name, description;
    float initial, minimum, maximum, step;
};

struct Imported {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

struct Host {
    std::mutex mutex;   // everything below

    // the reader SDL draws into
    AImageReader* reader = nullptr;
    jobject readerSurface = nullptr;   // global ref
    int readerWidth = 0, readerHeight = 0;

    // the screen
    ANativeWindow* window = nullptr;

    // Vulkan
    bool vulkanTried = false, vulkanOk = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    bool foreignQueue = false;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> rendered;   // one per swap chain image
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent = { 0, 0 };

    std::unordered_map<AHardwareBuffer*, Imported> imported;

    // the shader
    std::string preset, builtPreset;
    bool chainFailed = false;
    libra_vk_filter_chain_t chain = nullptr;
    size_t frameCount = 0;
    std::vector<Param> params;                          // of the built preset
    std::unordered_map<std::string, float> overrides;   // values the player set (for H.preset)
    bool overridesDirty = false;
} H;

// True while a preset is being built (the Java side shows its progress dialog meanwhile; read without the mutex).
std::atomic<bool> compiling{ false };

// Shader Resolution (ShaderLibrary): the picture the shader works on, in lines (0 = the frame as it is). The game's
// frame (at its own resolution) is scaled down to it before the shader, as RetroArch feeds a shader the console's
// picture. Not while the engine's menu is open: it stays readable.
std::atomic<int> shaderLines{ 0 };
std::atomic<bool> engineMenuOpen{ false };

// The frame scaled down for the shader (Shader Resolution), made again when its size changes.
struct Scaled {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
} S;

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties& e) { return strcmp(e.extensionName, name) == 0; });
}

bool initVulkan() {
    if (H.vulkanTried) {
        return H.vulkanOk;
    }
    H.vulkanTried = true;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ShaderHost";
    app.apiVersion = VK_API_VERSION_1_1;
    const char* instanceExtensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instanceExtensions;
    if (vkCreateInstance(&ici, nullptr, &H.instance) != VK_SUCCESS) {
        LOGW("no Vulkan instance");
        return false;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(H.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(H.instance, &count, devices.data());
    for (VkPhysicalDevice pd : devices) {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> families(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qc, families.data());
        for (uint32_t i = 0; i < qc; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                H.physical = pd;
                H.queueFamily = i;
                break;
            }
        }
        if (H.physical != VK_NULL_HANDLE) {
            break;
        }
    }
    if (H.physical == VK_NULL_HANDLE) {
        LOGW("no Vulkan GPU with a graphics queue");
        return false;
    }

    uint32_t ec = 0;
    vkEnumerateDeviceExtensionProperties(H.physical, nullptr, &ec, nullptr);
    std::vector<VkExtensionProperties> available(ec);
    vkEnumerateDeviceExtensionProperties(H.physical, nullptr, &ec, available.data());
    std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                                            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME };
    for (const char* e : extensions) {
        if (!hasExtension(available, e)) {
            LOGW("the GPU lacks %s", e);
            return false;
        }
    }
    H.foreignQueue = hasExtension(available, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    if (H.foreignQueue) {
        extensions.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = H.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    if (vkCreateDevice(H.physical, &dci, nullptr, &H.device) != VK_SUCCESS) {
        LOGW("no Vulkan device");
        return false;
    }
    vkGetDeviceQueue(H.device, H.queueFamily, 0, &H.queue);
    H.getAhbProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(H.device, "vkGetAndroidHardwareBufferPropertiesANDROID"));

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = H.queueFamily;
    vkCreateCommandPool(H.device, &pci, nullptr, &H.pool);
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = H.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(H.device, &cai, &H.cmd);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    vkCreateFence(H.device, &fci, nullptr, &H.fence);
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    vkCreateSemaphore(H.device, &sci, nullptr, &H.acquired);

    H.vulkanOk = H.getAhbProperties != nullptr && H.pool && H.cmd && H.fence && H.acquired;
    LOGI("Vulkan ready (foreign queue: %d)", H.foreignQueue);
    return H.vulkanOk;
}

void destroySwapchain() {
    if (H.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(H.device);
    }
    for (VkSemaphore s : H.rendered) {
        vkDestroySemaphore(H.device, s, nullptr);
    }
    H.rendered.clear();
    H.swapImages.clear();
    if (H.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(H.device, H.swapchain, nullptr);
        H.swapchain = VK_NULL_HANDLE;
    }
}

void destroyScaled() {
    if (S.image != VK_NULL_HANDLE) {
        vkDestroyImage(H.device, S.image, nullptr);
        S.image = VK_NULL_HANDLE;
    }
    if (S.memory != VK_NULL_HANDLE) {
        vkFreeMemory(H.device, S.memory, nullptr);
        S.memory = VK_NULL_HANDLE;
    }
    S.width = S.height = 0;
}

bool ensureScaled(uint32_t width, uint32_t height, VkFormat format) {
    if (S.image != VK_NULL_HANDLE && S.width == width && S.height == height && S.format == format) {
        return true;
    }
    vkDeviceWaitIdle(H.device);
    destroyScaled();
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(H.device, &ici, nullptr, &S.image) != VK_SUCCESS) {
        S.image = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(H.device, S.image, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(H.physical, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(H.device, &mai, nullptr, &S.memory) != VK_SUCCESS ||
        vkBindImageMemory(H.device, S.image, S.memory, 0) != VK_SUCCESS) {
        destroyScaled();
        return false;
    }
    S.width = width;
    S.height = height;
    S.format = format;
    LOGI("the shader works on a %ux%u picture", width, height);
    return true;
}

void destroySurface() {
    destroySwapchain();
    if (H.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(H.instance, H.surface, nullptr);
        H.surface = VK_NULL_HANDLE;
    }
}

bool createSwapchain() {
    if (H.window == nullptr) {
        return false;
    }
    if (H.surface == VK_NULL_HANDLE) {
        VkAndroidSurfaceCreateInfoKHR asci = { VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        asci.window = H.window;
        if (vkCreateAndroidSurfaceKHR(H.instance, &asci, nullptr, &H.surface) != VK_SUCCESS) {
            LOGW("no Vulkan surface for the screen");
            return false;
        }
    }
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(H.physical, H.surface, &caps);
    uint32_t fc = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(H.physical, H.surface, &fc, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fc);
    vkGetPhysicalDeviceSurfaceFormatsKHR(H.physical, H.surface, &fc, formats.data());
    if (formats.empty()) {
        return false;
    }
    VkSurfaceFormatKHR format = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            format = f;
            break;
        }
    }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent = { static_cast<uint32_t>(ANativeWindow_getWidth(H.window)),
                   static_cast<uint32_t>(ANativeWindow_getHeight(H.window)) };
    }
    if (extent.width == 0 || extent.height == 0) {
        return false;
    }
    uint32_t images = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount != 0) {
        images = std::min(images, caps.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                                            : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface = H.surface;
    sci.minImageCount = images;
    sci.imageFormat = format.format;
    sci.imageColorSpace = format.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;   // the system compositor rotates
    sci.compositeAlpha = alpha;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(H.device, &sci, nullptr, &H.swapchain) != VK_SUCCESS) {
        LOGW("no swap chain");
        return false;
    }
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(H.device, H.swapchain, &n, nullptr);
    H.swapImages.resize(n);
    vkGetSwapchainImagesKHR(H.device, H.swapchain, &n, H.swapImages.data());
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    H.rendered.resize(n);
    for (auto& s : H.rendered) {
        vkCreateSemaphore(H.device, &semi, nullptr, &s);
    }
    H.swapFormat = format.format;
    H.swapExtent = extent;
    // the chain renders into the swap chain's format: build it again for a new one
    H.builtPreset = "\x01";
    LOGI("swap chain %ux%u, %u images, format %d", extent.width, extent.height, n, format.format);
    return true;
}

void freeImported() {
    for (auto& [buffer, img] : H.imported) {
        vkDestroyImage(H.device, img.image, nullptr);
        vkFreeMemory(H.device, img.memory, nullptr);
    }
    H.imported.clear();
}

const Imported* importBuffer(AHardwareBuffer* buffer) {
    auto it = H.imported.find(buffer);
    if (it != H.imported.end()) {
        return &it->second;
    }
    AHardwareBuffer_Desc desc = {};
    media.describe(buffer, &desc);
    VkAndroidHardwareBufferFormatPropertiesANDROID fp = { VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
    VkAndroidHardwareBufferPropertiesANDROID props = { VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &fp };
    if (H.getAhbProperties(H.device, buffer, &props) != VK_SUCCESS || fp.format == VK_FORMAT_UNDEFINED) {
        LOGW("the game's frames can't be read by Vulkan (format %d)", fp.format);
        return nullptr;
    }
    Imported img;
    img.format = fp.format;
    img.width = desc.width;
    img.height = desc.height;
    VkExternalMemoryImageCreateInfo emi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fp.format;
    ici.extent = { desc.width, desc.height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(H.device, &ici, nullptr, &img.image) != VK_SUCCESS) {
        LOGW("vkCreateImage for a frame failed");
        return nullptr;
    }
    VkImportAndroidHardwareBufferInfoANDROID import = { VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID };
    import.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import };
    dedicated.image = img.image;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated };
    mai.allocationSize = props.allocationSize;
    mai.memoryTypeIndex = 0;
    while (mai.memoryTypeIndex < 32 && !(props.memoryTypeBits & (1u << mai.memoryTypeIndex))) {
        mai.memoryTypeIndex++;
    }
    if (vkAllocateMemory(H.device, &mai, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(H.device, img.image, img.memory, 0) != VK_SUCCESS) {
        LOGW("importing a frame failed");
        vkDestroyImage(H.device, img.image, nullptr);
        return nullptr;
    }
    LOGI("frame buffer imported: %ux%u, format %d", img.width, img.height, img.format);
    return &H.imported.emplace(buffer, img).first->second;
}

// The chain for H.preset (none for an empty one, or when it failed): built once per preset and swap chain.
void updateChain() {
    if (H.builtPreset == H.preset) {
        return;
    }
    vkDeviceWaitIdle(H.device);
    if (H.chain != nullptr) {
        libra.chainFree(&H.chain);
        H.chain = nullptr;
    }
    H.builtPreset = H.preset;
    H.chainFailed = false;
    H.frameCount = 0;
    if (H.preset.empty()) {
        return;
    }
    if (!loadLibra()) {
        H.chainFailed = true;
        return;
    }
    struct Busy {
        Busy() { compiling = true; }
        ~Busy() { compiling = false; }
    } busy;
    libra_shader_preset_t preset = nullptr;
    libra_error_t error = libra.presetCreate(H.preset.c_str(), &preset);
    if (error != nullptr) {
        LOGW("preset %s: %s", H.preset.c_str(), libraError(error).c_str());
        H.chainFailed = true;
        return;
    }
    H.params.clear();
    if (libra.getParams != nullptr) {
        libra_preset_param_list_t list = {};
        if (libra.getParams(&preset, &list) == nullptr) {
            for (uint64_t i = 0; i < list.length; i++) {
                const libra_preset_param_t& p = list.parameters[i];
                H.params.push_back({ p.name ? p.name : "", p.description ? p.description : "", p.initial, p.minimum,
                                     p.maximum, p.step });
            }
            libra.freeParams(list);
        }
    }
    for (const auto& [name, value] : H.overrides) {   // the player's values from the start
        if (libra.presetSetParam != nullptr) {
            libra_error_t e = libra.presetSetParam(&preset, name.c_str(), value);
            if (e != nullptr) libraError(e);
        }
    }
    H.overridesDirty = false;
    libra_device_vk_t vk = {};
    vk.physical_device = H.physical;
    vk.instance = H.instance;
    vk.device = H.device;
    vk.queue = H.queue;
    vk.entry = vkGetInstanceProcAddr;
    filter_chain_vk_opt_t options = {};
    options.version = LIBRASHADER_CURRENT_VERSION;
    options.frames_in_flight = 1;
    options.use_dynamic_rendering = false;
    options.disable_cache = true;
    error = libra.chainCreate(&preset, vk, &options, &H.chain);
    if (preset != nullptr) {   // creating the chain takes the preset over
        libra.presetFree(&preset);
    }
    if (error != nullptr) {
        LOGW("chain for %s: %s", H.preset.c_str(), libraError(error).c_str());
        H.chain = nullptr;
        H.chainFailed = true;
        return;
    }
    LOGI("shader in use: %s", H.preset.c_str());
}

void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
             VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
             uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(H.cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void waitFence(int fd) {
    if (fd >= 0) {
        pollfd p = { fd, POLLIN, 0 };
        poll(&p, 1, 1000);
        close(fd);
    }
}

// One frame of the game: from the reader's buffer through the shader onto the screen.
void present(AImageReader* reader) {
    AImage* image = nullptr;
    int fence = -1;
    static int calls = 0;
    int32_t got = media.acquireLatestImageAsync(reader, &image, &fence);
    if (calls < 5) {
        LOGI("frame %d from the game: acquire %d", calls, got);
    }
    calls++;
    if (got != 0 || image == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(H.mutex);
    waitFence(fence);
    AHardwareBuffer* buffer = nullptr;
    if (H.vulkanOk && H.window != nullptr && media.getHardwareBuffer(image, &buffer) == 0 && buffer != nullptr) {
        if (H.swapchain == VK_NULL_HANDLE) {
            createSwapchain();
        }
        const Imported* input = H.swapchain != VK_NULL_HANDLE ? importBuffer(buffer) : nullptr;
        uint32_t index = 0;
        VkResult acquired = input ? vkAcquireNextImageKHR(H.device, H.swapchain, 1000000000ull, H.acquired, VK_NULL_HANDLE, &index)
                                  : VK_ERROR_UNKNOWN;
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
            destroySwapchain();
        } else if (acquired == VK_SUCCESS || acquired == VK_SUBOPTIMAL_KHR) {
            updateChain();
            if (H.chain != nullptr && H.overridesDirty && libra.chainSetParam != nullptr) {
                for (const auto& [name, value] : H.overrides) {
                    libra_error_t e = libra.chainSetParam(&H.chain, name.c_str(), value);
                    if (e != nullptr) libraError(e);
                }
                H.overridesDirty = false;
            }
            const uint32_t foreign = H.foreignQueue ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
            VkImage output = H.swapImages[index];
            vkResetCommandBuffer(H.cmd, 0);
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(H.cmd, &bi);
            // While the engine's menu is open the frame goes to the screen as it is: the menu stays sharp (it is
            // drawn into the frame, so a CRT shader would blur it).
            if (H.chain != nullptr && !engineMenuOpen) {
                // Shader Resolution: the frame scaled down (nearest) to the lines the shader works on
                const int lines = engineMenuOpen ? 0 : shaderLines.load();
                const bool scale = lines > 0 && static_cast<uint32_t>(lines) < input->height &&
                                   ensureScaled(input->width * static_cast<uint32_t>(lines) / input->height,
                                                static_cast<uint32_t>(lines), input->format);
                if (scale) {
                    barrier(input->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, foreign, H.queueFamily);
                    barrier(S.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                    VkImageBlit down = {};
                    down.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    down.srcOffsets[1] = { static_cast<int32_t>(input->width), static_cast<int32_t>(input->height), 1 };
                    down.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    down.dstOffsets[1] = { static_cast<int32_t>(S.width), static_cast<int32_t>(S.height), 1 };
                    vkCmdBlitImage(H.cmd, input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down, VK_FILTER_LINEAR);
                    barrier(S.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
                } else {
                    barrier(input->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, foreign, H.queueFamily);
                }
                barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                libra_image_vk_t in = scale ? libra_image_vk_t{ S.image, S.format, S.width, S.height }
                                            : libra_image_vk_t{ input->image, input->format, input->width, input->height };
                libra_image_vk_t out = { output, H.swapFormat, H.swapExtent.width, H.swapExtent.height };
                libra_viewport_t viewport = { 0.0f, 0.0f, H.swapExtent.width, H.swapExtent.height };
                frame_vk_opt_t fo = {};
                fo.version = LIBRASHADER_CURRENT_VERSION;
                fo.frame_direction = 1;
                fo.total_subframes = 1;
                fo.current_subframe = 1;
                fo.frames_per_second = 60.0f;
                libra_error_t error = libra.chainFrame(&H.chain, H.cmd, H.frameCount++, in, out, &viewport, nullptr, &fo);
                if (error != nullptr) {
                    LOGW("shader failed while drawing: %s", libraError(error).c_str());
                    H.chainFailed = true;
                }
                barrier(output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                if (scale) {
                    barrier(input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, H.queueFamily, foreign);
                } else {
                    barrier(input->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                            VK_ACCESS_SHADER_READ_BIT, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, H.queueFamily, foreign);
                }
            } else {
                // no shader: the frame as it is, scaled to the screen
                barrier(input->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        foreign, H.queueFamily);
                barrier(output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkImageBlit blit = {};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.srcOffsets[1] = { static_cast<int32_t>(input->width), static_cast<int32_t>(input->height), 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.dstOffsets[1] = { static_cast<int32_t>(H.swapExtent.width), static_cast<int32_t>(H.swapExtent.height), 1 };
                vkCmdBlitImage(H.cmd, input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
                barrier(input->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                        VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, H.queueFamily, foreign);
            }
            vkEndCommandBuffer(H.cmd);
            if (H.chainFailed && H.chain != nullptr) {   // after a drawing error: no shader from the next frame
                vkDeviceWaitIdle(H.device);
                libra.chainFree(&H.chain);
                H.chain = nullptr;
            }
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            si.waitSemaphoreCount = 1;
            si.pWaitSemaphores = &H.acquired;
            si.pWaitDstStageMask = &waitStage;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &H.cmd;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &H.rendered[index];
            vkResetFences(H.device, 1, &H.fence);
            if (vkQueueSubmit(H.queue, 1, &si, H.fence) == VK_SUCCESS) {
                VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &H.rendered[index];
                pi.swapchainCount = 1;
                pi.pSwapchains = &H.swapchain;
                pi.pImageIndices = &index;
                VkResult presented = vkQueuePresentKHR(H.queue, &pi);
                vkWaitForFences(H.device, 1, &H.fence, VK_TRUE, 1000000000ull);
                if (presented == VK_ERROR_OUT_OF_DATE_KHR) {
                    destroySwapchain();
                }
            }
        }
    }
    media.deleteAsync(image, -1);
}

void onImage(void*, AImageReader* reader) {
    present(reader);
}

} // namespace

extern "C" {

// The system has what this needs (API 29+ checked on the Java side, the media functions, a Vulkan GPU).
JNIEXPORT jboolean JNICALL SH_JNI(nativeAvailable)(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(H.mutex);
    return loadMedia() && initVulkan() ? JNI_TRUE : JNI_FALSE;
}

// librashader could be loaded (otherwise only "Off" works).
JNIEXPORT jboolean JNICALL SH_JNI(nativeShadersAvailable)(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(H.mutex);
    return loadLibra() ? JNI_TRUE : JNI_FALSE;
}

// The screen surface appeared or changed (SurfaceHolder.surfaceChanged): presents onto it; returns the Surface SDL
// draws into (the reader's, of the screen's size; made once).
JNIEXPORT jobject JNICALL SH_JNI(nativeAttach)(JNIEnv* env, jclass, jobject screen, jint width, jint height) {
    std::lock_guard<std::mutex> lock(H.mutex);
    if (!loadMedia() || !initVulkan()) {
        return nullptr;
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, screen);
    if (window != H.window) {
        destroySurface();
        if (H.window != nullptr) {
            ANativeWindow_release(H.window);
        }
        H.window = window;
    } else if (window != nullptr) {
        ANativeWindow_release(window);   // the same one: keep the reference held already
        destroySwapchain();               // new size
    }
    if (H.reader == nullptr && width > 0 && height > 0) {
        const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
        if (media.newWithUsage(width, height, kFormatRgbx8888, usage, 3, &H.reader) != 0) {
            LOGW("no image reader");
            H.reader = nullptr;
            return nullptr;
        }
        static ImageListener listener = { nullptr, onImage };
        media.setImageListener(H.reader, &listener);
        ANativeWindow* readerWindow = nullptr;
        media.getWindow(H.reader, &readerWindow);
        jobject surface = media.toSurface(env, readerWindow);
        H.readerSurface = env->NewGlobalRef(surface);
        env->DeleteLocalRef(surface);
        H.readerWidth = width;
        H.readerHeight = height;
        LOGI("the game draws into a %dx%d buffer", width, height);
    }
    return H.readerSurface != nullptr ? env->NewLocalRef(H.readerSurface) : nullptr;
}

// The screen surface is gone (SurfaceHolder.surfaceDestroyed).
JNIEXPORT void JNICALL SH_JNI(nativeDetach)(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lock(H.mutex);
    destroySurface();
    if (H.window != nullptr) {
        ANativeWindow_release(H.window);
        H.window = nullptr;
    }
}

// A preset is being compiled right now.
// libultraship's console variables (the game's library): the low resolution mode the game draws in.
extern "C" void CVarSetInteger(const char* name, int32_t value);

// Shader Resolution (ShaderLibrary, while the game runs): lowResMode - libultraship's gLowResMode (0 off, 2 240p
// widescreen, 3 480p widescreen; -1 left as it is); lines - the picture the shader works on (0: the frame as it is).
JNIEXPORT void JNICALL SH_JNI(nativeSetResolution)(JNIEnv*, jclass, jint lowResMode, jint lines) {
    if (lowResMode >= 0) {   // -1: the game's own setting stays
        CVarSetInteger("gLowResMode", lowResMode);
    }
    shaderLines = lines;
}

// The engine's menu is open (TouchControls' poll): the shader then works on the full frame.
JNIEXPORT void JNICALL SH_JNI(nativeSetEngineMenuOpen)(JNIEnv*, jclass, jboolean open) {
    engineMenuOpen = open == JNI_TRUE;
}

JNIEXPORT jboolean JNICALL SH_JNI(nativeCompiling)(JNIEnv*, jclass) {
    return compiling ? JNI_TRUE : JNI_FALSE;
}

// The preset (.slangp path) to draw with; empty: none. Any thread; used from the next frame.
JNIEXPORT void JNICALL SH_JNI(nativeSetPreset)(JNIEnv* env, jclass, jstring path) {
    const char* p = path != nullptr ? env->GetStringUTFChars(path, nullptr) : nullptr;
    std::lock_guard<std::mutex> lock(H.mutex);
    std::string next = p != nullptr ? p : "";
    if (next != H.preset) {
        H.overrides.clear();
        H.params.clear();
        H.builtPreset = "\x01";   // build again, also when it comes back to the one built (Reset to defaults)
    }
    H.preset = next;
    if (p != nullptr) {
        env->ReleaseStringUTFChars(path, p);
    }
}

// The parameters of the preset in use, one per line: name, description, value, min, max, step (tab separated);
// empty while there is none (or it is still being built).
JNIEXPORT jstring JNICALL SH_JNI(nativeParams)(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(H.mutex);
    std::string out;
    char num[160];
    for (const Param& p : H.params) {
        auto it = H.overrides.find(p.name);
        float v = it != H.overrides.end() ? it->second : p.initial;
        snprintf(num, sizeof num, "\t%g\t%g\t%g\t%g\t%g\n", v, p.minimum, p.maximum, p.step, p.initial);
        out += p.name + "\t" + p.description + num;
    }
    return env->NewStringUTF(out.c_str());
}

// One parameter's value (kept for the preset in use; applied from the next frame).
JNIEXPORT void JNICALL SH_JNI(nativeSetParam)(JNIEnv* env, jclass, jstring name, jfloat value) {
    const char* n = env->GetStringUTFChars(name, nullptr);
    std::lock_guard<std::mutex> lock(H.mutex);
    H.overrides[n] = value;
    H.overridesDirty = true;
    env->ReleaseStringUTFChars(name, n);
}

} // extern "C"
