// Minimal Vulkan setup: instance, device, one queue, buffers, compute pipelines and timestamps.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bp {

#define VK_CHECK(expr)                                                                    \
    do {                                                                                  \
        VkResult vk_check_result_ = (expr);                                               \
        if (vk_check_result_ != VK_SUCCESS)                                               \
            throw std::runtime_error(std::string(#expr " failed: ") +                     \
                                     std::to_string(int(vk_check_result_)));              \
    } while (0)

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void *mapped = nullptr; // host-visible buffers only
};

class VkContext {
public:
    // instanceExtensions: extra extensions (e.g. from SDL for a window surface).
    VkContext(const std::vector<const char *> &instanceExtensions, bool validation);
    ~VkContext();
    VkContext(const VkContext &) = delete;
    VkContext &operator=(const VkContext &) = delete;

    // Must be called before createDevice when presenting to a surface.
    void setSurface(VkSurfaceKHR surface) { surface_ = surface; }
    void createDevice();

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkCommandPool commandPool() const { return commandPool_; }
    const VkPhysicalDeviceProperties &properties() const { return props_; }

    Buffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    void destroyBuffer(Buffer &b);

    // Records and waits for a one-off command buffer.
    template <class F> void immediate(F &&record);

    // Copies host data into a device-local buffer through a staging buffer.
    void upload(const Buffer &dst, const void *data, VkDeviceSize size, VkDeviceSize offset = 0);
    void download(const Buffer &src, void *data, VkDeviceSize size, VkDeviceSize offset = 0);

    VkShaderModule loadShader(const std::string &path);

private:
    VkCommandBuffer beginOneOff();
    void endOneOff(VkCommandBuffer cmd);
    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags flags) const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    bool validation_ = false;
};

template <class F> void VkContext::immediate(F &&record) {
    VkCommandBuffer cmd = beginOneOff();
    record(cmd);
    endOneOff(cmd);
}

// A global "everything before must finish before everything after" barrier for compute work.
void computeBarrier(VkCommandBuffer cmd);

} // namespace bp
