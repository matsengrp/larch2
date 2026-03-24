#include <larch/vulkan_compute.hpp>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace larch {
namespace {

void vk_check(VkResult r, const char* msg) {
  if (r != VK_SUCCESS)
    throw std::runtime_error{std::string{"Vulkan error: "} + msg + " (code " +
                             std::to_string(r) + ")"};
}

uint32_t find_memory_type(VkPhysicalDeviceMemoryProperties const& mem_props,
                          uint32_t type_filter, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((type_filter & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  }
  throw std::runtime_error{"Vulkan: no suitable memory type found"};
}

}  // namespace

// ============================================================================
// vk_context
// ============================================================================

struct vk_context::impl {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkPhysicalDeviceMemoryProperties mem_props{};
  std::string device_name;

  impl() {
    // Instance — no extensions needed for compute-only.
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "larch2";
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app_info;

#ifndef NDEBUG
    // Enable validation layers in debug builds.
    static const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    // Check if available before requesting.
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available.data());
    bool have_validation = std::ranges::any_of(available, [](auto& lp) {
      return std::strcmp(lp.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
    if (have_validation) {
      ci.enabledLayerCount = 1;
      ci.ppEnabledLayerNames = layers;
    }
#endif

    vk_check(vkCreateInstance(&ci, nullptr, &instance), "vkCreateInstance");

    // Physical device — prefer discrete GPU.
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, nullptr);
    if (dev_count == 0)
      throw std::runtime_error{"Vulkan: no physical devices found"};
    std::vector<VkPhysicalDevice> devs(dev_count);
    vkEnumeratePhysicalDevices(instance, &dev_count, devs.data());

    // Score devices: discrete=3, integrated=2, cpu/other=1.
    auto score = [](VkPhysicalDevice d) -> int {
      VkPhysicalDeviceProperties p;
      vkGetPhysicalDeviceProperties(d, &p);
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return 3;
      if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) return 2;
      return 1;
    };
    std::ranges::sort(devs,
                      [&](auto a, auto b) { return score(a) > score(b); });
    physical_device = devs[0];

    VkPhysicalDeviceProperties dev_props;
    vkGetPhysicalDeviceProperties(physical_device, &dev_props);
    device_name = dev_props.deviceName;

    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    // Find compute queue family.
    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qf_count,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> qf_props(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qf_count,
                                             qf_props.data());

    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qf_count; ++i) {
      if (qf_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
        queue_family = i;
        break;
      }
    }
    if (queue_family == UINT32_MAX)
      throw std::runtime_error{"Vulkan: no compute queue family"};

    // Logical device + queue.
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queue_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    vk_check(vkCreateDevice(physical_device, &dci, nullptr, &device),
             "vkCreateDevice");
    vkGetDeviceQueue(device, queue_family, 0, &queue);

    // Command pool.
    VkCommandPoolCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = queue_family;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vk_check(vkCreateCommandPool(device, &cpci, nullptr, &command_pool),
             "vkCreateCommandPool");
  }

  ~impl() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
  }

  impl(impl const&) = delete;
  impl& operator=(impl const&) = delete;
};

vk_context::vk_context() : impl_{std::make_unique<impl>()} {}
vk_context::~vk_context() = default;
vk_context::vk_context(vk_context&&) noexcept = default;
vk_context& vk_context::operator=(vk_context&&) noexcept = default;

std::string vk_context::device_name() const { return impl_->device_name; }
vk_context::impl& vk_context::get_impl() { return *impl_; }
vk_context::impl const& vk_context::get_impl() const { return *impl_; }

// ============================================================================
// vk_buffer
// ============================================================================

struct vk_buffer::impl {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  std::size_t size = 0;

  ~impl() {
    if (device) {
      if (mapped) vkUnmapMemory(device, memory);
      if (buffer) vkDestroyBuffer(device, buffer, nullptr);
      if (memory) vkFreeMemory(device, memory, nullptr);
    }
  }
  impl() = default;
  impl(impl const&) = delete;
  impl& operator=(impl const&) = delete;
};

vk_buffer::vk_buffer(std::unique_ptr<impl> p) : impl_{std::move(p)} {}
vk_buffer::~vk_buffer() = default;
vk_buffer::vk_buffer(vk_buffer&&) noexcept = default;
vk_buffer& vk_buffer::operator=(vk_buffer&&) noexcept = default;

vk_buffer::impl& vk_buffer::get_impl() { return *impl_; }

std::size_t vk_buffer::size_bytes() const { return impl_->size; }

vk_buffer vk_buffer::create(vk_context& ctx, std::size_t size_bytes,
                            usage /*u*/) {
  auto& c = ctx.get_impl();
  auto p = std::make_unique<impl>();
  p->device = c.device;
  p->size = size_bytes;

  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = size_bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  vk_check(vkCreateBuffer(c.device, &bci, nullptr, &p->buffer),
           "vkCreateBuffer");

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(c.device, p->buffer, &req);

  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      find_memory_type(c.mem_props, req.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

  vk_check(vkAllocateMemory(c.device, &mai, nullptr, &p->memory),
           "vkAllocateMemory");
  vk_check(vkBindBufferMemory(c.device, p->buffer, p->memory, 0),
           "vkBindBufferMemory");
  vk_check(vkMapMemory(c.device, p->memory, 0, size_bytes, 0, &p->mapped),
           "vkMapMemory");

  return vk_buffer{std::move(p)};
}

void vk_buffer::upload(std::span<const std::byte> data) {
  if (data.size() > impl_->size)
    throw std::runtime_error{"vk_buffer::upload: data exceeds buffer size"};
  std::memcpy(impl_->mapped, data.data(), data.size());
}

void vk_buffer::download(std::span<std::byte> out) const {
  if (out.size() > impl_->size)
    throw std::runtime_error{"vk_buffer::download: span exceeds buffer size"};
  std::memcpy(out.data(), impl_->mapped, out.size());
}

// ============================================================================
// vk_pipeline
// ============================================================================

struct vk_pipeline::impl {
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkShaderModule shader = VK_NULL_HANDLE;
  VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  VkDescriptorSet desc_set = VK_NULL_HANDLE;
  uint32_t num_bindings = 0;
  std::size_t push_constant_size = 0;

  ~impl() {
    if (!device) return;
    if (desc_pool) vkDestroyDescriptorPool(device, desc_pool, nullptr);
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (pipe_layout) vkDestroyPipelineLayout(device, pipe_layout, nullptr);
    if (ds_layout) vkDestroyDescriptorSetLayout(device, ds_layout, nullptr);
    if (shader) vkDestroyShaderModule(device, shader, nullptr);
  }
  impl() = default;
  impl(impl const&) = delete;
  impl& operator=(impl const&) = delete;
};

vk_pipeline::vk_pipeline(std::unique_ptr<impl> p) : impl_{std::move(p)} {}
vk_pipeline::~vk_pipeline() = default;
vk_pipeline::vk_pipeline(vk_pipeline&&) noexcept = default;
vk_pipeline& vk_pipeline::operator=(vk_pipeline&&) noexcept = default;

vk_pipeline vk_pipeline::create(vk_context& ctx,
                                std::span<const std::uint32_t> spirv,
                                std::uint32_t num_storage_buffers,
                                std::size_t push_constant_size) {
  auto& c = ctx.get_impl();
  auto p = std::make_unique<impl>();
  p->device = c.device;
  p->queue = c.queue;
  p->command_pool = c.command_pool;
  p->num_bindings = num_storage_buffers;
  p->push_constant_size = push_constant_size;

  // Shader module.
  VkShaderModuleCreateInfo smci{};
  smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smci.codeSize = spirv.size() * sizeof(uint32_t);
  smci.pCode = spirv.data();
  vk_check(vkCreateShaderModule(c.device, &smci, nullptr, &p->shader),
           "vkCreateShaderModule");

  // Descriptor set layout — N storage buffers at bindings 0..N-1.
  std::vector<VkDescriptorSetLayoutBinding> bindings(num_storage_buffers);
  for (uint32_t i = 0; i < num_storage_buffers; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }

  VkDescriptorSetLayoutCreateInfo dslci{};
  dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslci.bindingCount = num_storage_buffers;
  dslci.pBindings = bindings.data();
  vk_check(
      vkCreateDescriptorSetLayout(c.device, &dslci, nullptr, &p->ds_layout),
      "vkCreateDescriptorSetLayout");

  // Pipeline layout (with optional push constants).
  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pcr.offset = 0;
  pcr.size = static_cast<uint32_t>(push_constant_size);

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &p->ds_layout;
  if (push_constant_size > 0) {
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
  }
  vk_check(vkCreatePipelineLayout(c.device, &plci, nullptr, &p->pipe_layout),
           "vkCreatePipelineLayout");

  // Compute pipeline.
  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = p->shader;
  cpci.stage.pName = "main";
  cpci.layout = p->pipe_layout;
  vk_check(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                    &p->pipeline),
           "vkCreateComputePipelines");

  // Descriptor pool + set.
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  pool_size.descriptorCount = num_storage_buffers;

  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  vk_check(vkCreateDescriptorPool(c.device, &dpci, nullptr, &p->desc_pool),
           "vkCreateDescriptorPool");

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = p->desc_pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &p->ds_layout;
  vk_check(vkAllocateDescriptorSets(c.device, &dsai, &p->desc_set),
           "vkAllocateDescriptorSets");

  return vk_pipeline{std::move(p)};
}

void vk_pipeline::dispatch(std::vector<vk_buffer*> buffers,
                           std::span<const std::byte> push_constants,
                           std::array<std::uint32_t, 3> workgroup_count) {
  auto& pi = *impl_;

  if (buffers.size() != pi.num_bindings)
    throw std::runtime_error{"vk_pipeline::dispatch: wrong buffer count"};

  // Update descriptor set with buffer bindings.
  std::vector<VkDescriptorBufferInfo> buf_infos(buffers.size());
  std::vector<VkWriteDescriptorSet> writes(buffers.size());
  for (std::size_t i = 0; i < buffers.size(); ++i) {
    buf_infos[i].buffer = buffers[i]->get_impl().buffer;
    buf_infos[i].offset = 0;
    buf_infos[i].range = VK_WHOLE_SIZE;

    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].pNext = nullptr;
    writes[i].dstSet = pi.desc_set;
    writes[i].dstBinding = static_cast<uint32_t>(i);
    writes[i].dstArrayElement = 0;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &buf_infos[i];
    writes[i].pImageInfo = nullptr;
    writes[i].pTexelBufferView = nullptr;
  }
  vkUpdateDescriptorSets(pi.device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);

  // Record command buffer.
  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pi.command_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;

  VkCommandBuffer cmd;
  vk_check(vkAllocateCommandBuffers(pi.device, &cbai, &cmd),
           "vkAllocateCommandBuffers");

  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_check(vkBeginCommandBuffer(cmd, &cbbi), "vkBeginCommandBuffer");

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pi.pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pi.pipe_layout,
                          0, 1, &pi.desc_set, 0, nullptr);

  if (!push_constants.empty() && pi.push_constant_size > 0) {
    vkCmdPushConstants(cmd, pi.pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       static_cast<uint32_t>(push_constants.size()),
                       push_constants.data());
  }

  vkCmdDispatch(cmd, workgroup_count[0], workgroup_count[1],
                workgroup_count[2]);

  vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

  // Submit and wait.
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  vk_check(vkCreateFence(pi.device, &fci, nullptr, &fence), "vkCreateFence");

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vk_check(vkQueueSubmit(pi.queue, 1, &si, fence), "vkQueueSubmit");

  vk_check(vkWaitForFences(pi.device, 1, &fence, VK_TRUE, UINT64_MAX),
           "vkWaitForFences");

  vkDestroyFence(pi.device, fence, nullptr);
  vkFreeCommandBuffers(pi.device, pi.command_pool, 1, &cmd);
}

}  // namespace larch
