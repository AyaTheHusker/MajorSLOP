/*
 * depth_render.c — Headless Vulkan compute renderer for DepthFlow parallax.
 *
 * Exports a simple C API callable from Python via ctypes:
 *   dr_create(w, h, spirv_path) → handle
 *   dr_load_scene(handle, color_rgba, depth_gray, inpaint_rgba, iw, ih) → 0/err
 *   dr_render_frame(handle, params, n_params, output_rgba) → 0/err
 *   dr_destroy(handle)
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_PUSH 128

typedef struct {
    VkInstance        instance;
    VkPhysicalDevice  physDev;
    VkDevice          device;
    VkQueue           queue;
    uint32_t          queueFamily;
    VkCommandPool     cmdPool;
    VkCommandBuffer   cmdBuf;
    VkFence           fence;

    /* Pipeline */
    VkShaderModule    shader;
    VkDescriptorSetLayout dsLayout;
    VkPipelineLayout  pipeLayout;
    VkPipeline        pipeline;
    VkDescriptorPool  dsPool;
    VkDescriptorSet   ds;

    /* Images */
    VkImage      colorImg, depthImg, inpaintImg, outImg;
    VkDeviceMemory colorMem, depthMem, inpaintMem, outMem;
    VkImageView  colorView, depthView, inpaintView, outView;
    VkSampler    sampler;

    /* Staging buffer (upload + readback) */
    VkBuffer       staging;
    VkDeviceMemory stagingMem;
    VkDeviceSize   stagingSize;

    int width, height;
    int sceneLoaded;
} DepthRenderer;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return 0xFFFFFFFF;
}

static VkResult createImage(DepthRenderer *r, int w, int h, VkFormat fmt,
                            VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult res = vkCreateImage(r->device, &ci, NULL, img);
    if (res != VK_SUCCESS) return res;

    VkMemoryRequirements mreq;
    vkGetImageMemoryRequirements(r->device, *img, &mreq);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mreq.size;
    ai.memoryTypeIndex = findMemoryType(r->physDev, mreq.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    res = vkAllocateMemory(r->device, &ai, NULL, mem);
    if (res != VK_SUCCESS) return res;
    return vkBindImageMemory(r->device, *img, *mem, 0);
}

static VkResult createImageView(VkDevice dev, VkImage img, VkFormat fmt, VkImageView *view) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = fmt;
    ci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(dev, &ci, NULL, view);
}

static void transitionLayout(VkCommandBuffer cb, VkImage img,
                             VkImageLayout oldL, VkImageLayout newL,
                             VkAccessFlags srcA, VkAccessFlags dstA,
                             VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, NULL, 0, NULL, 1, &b);
}

static void submitAndWait(DepthRenderer *r) {
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &r->cmdBuf;
    vkQueueSubmit(r->queue, 1, &si, r->fence);
    vkWaitForFences(r->device, 1, &r->fence, VK_TRUE, UINT64_MAX);
    vkResetFences(r->device, 1, &r->fence);
}

static void uploadImageData(DepthRenderer *r, VkImage img, int w, int h,
                            const uint8_t *data, VkDeviceSize dataSize, VkFormat fmt) {
    void *mapped;
    vkMapMemory(r->device, r->stagingMem, 0, dataSize, 0, &mapped);
    memcpy(mapped, data, dataSize);
    vkUnmapMemory(r->device, r->stagingMem);

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(r->cmdBuf, 0);
    vkBeginCommandBuffer(r->cmdBuf, &bi);

    transitionLayout(r->cmdBuf, img,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = (VkExtent3D){w, h, 1};
    vkCmdCopyBufferToImage(r->cmdBuf, r->staging, img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transitionLayout(r->cmdBuf, img,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    vkEndCommandBuffer(r->cmdBuf);
    submitAndWait(r);
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void *dr_create(int width, int height, const char *spirv_path) {
    DepthRenderer *r = calloc(1, sizeof(DepthRenderer));
    if (!r) return NULL;
    r->width = width;
    r->height = height;

    /* Instance */
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "MajorSLOP-DepthRender";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    if (vkCreateInstance(&ici, NULL, &r->instance) != VK_SUCCESS) goto fail;

    /* Physical device — prefer discrete GPU */
    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(r->instance, &devCount, NULL);
    if (devCount == 0) goto fail;
    VkPhysicalDevice *devs = malloc(sizeof(VkPhysicalDevice) * devCount);
    vkEnumeratePhysicalDevices(r->instance, &devCount, devs);
    r->physDev = devs[0];
    for (uint32_t i = 0; i < devCount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devs[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            r->physDev = devs[i];
            break;
        }
    }
    free(devs);

    /* Queue family — compute */
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(r->physDev, &qfCount, NULL);
    VkQueueFamilyProperties *qfs = malloc(sizeof(VkQueueFamilyProperties) * qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(r->physDev, &qfCount, qfs);
    r->queueFamily = 0;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { r->queueFamily = i; break; }
    }
    free(qfs);

    /* Device */
    float qp = 1.0f;
    VkDeviceQueueCreateInfo dqci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dqci.queueFamilyIndex = r->queueFamily;
    dqci.queueCount = 1;
    dqci.pQueuePriorities = &qp;

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqci;
    if (vkCreateDevice(r->physDev, &dci, NULL, &r->device) != VK_SUCCESS) goto fail;
    vkGetDeviceQueue(r->device, r->queueFamily, 0, &r->queue);

    /* Command pool + buffer */
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = r->queueFamily;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(r->device, &cpci, NULL, &r->cmdPool);

    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = r->cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(r->device, &cbai, &r->cmdBuf);

    /* Fence */
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(r->device, &fci, NULL, &r->fence);

    /* Load SPIR-V */
    FILE *f = fopen(spirv_path, "rb");
    if (!f) { fprintf(stderr, "depth_render: cannot open %s\n", spirv_path); goto fail; }
    fseek(f, 0, SEEK_END);
    long spvSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *spvCode = malloc(spvSize);
    fread(spvCode, 1, spvSize, f);
    fclose(f);

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = spvSize;
    smci.pCode = spvCode;
    vkCreateShaderModule(r->device, &smci, NULL, &r->shader);
    free(spvCode);

    /* Descriptor set layout:
     *   0 = storage image (output)
     *   1 = combined image sampler (color)
     *   2 = combined image sampler (depth)
     *   3 = combined image sampler (inpaint)
     */
    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 4;
    dslci.pBindings = bindings;
    vkCreateDescriptorSetLayout(r->device, &dslci, NULL, &r->dsLayout);

    /* Pipeline layout (push constants) */
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, MAX_PUSH};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &r->dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(r->device, &plci, NULL, &r->pipeLayout);

    /* Compute pipeline */
    VkComputePipelineCreateInfo cpipe = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpipe.stage.module = r->shader;
    cpipe.stage.pName = "main";
    cpipe.layout = r->pipeLayout;
    if (vkCreateComputePipelines(r->device, VK_NULL_HANDLE, 1, &cpipe, NULL, &r->pipeline) != VK_SUCCESS)
        goto fail;

    /* Sampler (linear, clamp to edge) */
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(r->device, &sci, NULL, &r->sampler);

    /* Descriptor pool */
    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
    };
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    vkCreateDescriptorPool(r->device, &dpci, NULL, &r->dsPool);

    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = r->dsPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &r->dsLayout;
    vkAllocateDescriptorSets(r->device, &dsai, &r->ds);

    /* Staging buffer — big enough for RGBA at max resolution */
    r->stagingSize = (VkDeviceSize)width * height * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = r->stagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(r->device, &bci, NULL, &r->staging);

    VkMemoryRequirements mreq;
    vkGetBufferMemoryRequirements(r->device, r->staging, &mreq);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mreq.size;
    mai.memoryTypeIndex = findMemoryType(r->physDev, mreq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(r->device, &mai, NULL, &r->stagingMem);
    vkBindBufferMemory(r->device, r->staging, r->stagingMem, 0);

    /* Create output image */
    createImage(r, width, height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &r->outImg, &r->outMem);
    createImageView(r->device, r->outImg, VK_FORMAT_R8G8B8A8_UNORM, &r->outView);

    return r;

fail:
    free(r);
    return NULL;
}

int dr_load_scene(void *handle,
                  const uint8_t *color_rgba, int cw, int ch,
                  const uint8_t *depth_gray, int dw, int dh,
                  const uint8_t *inpaint_rgba, int iw, int ih) {
    DepthRenderer *r = (DepthRenderer *)handle;
    if (!r) return -1;

    /* Destroy old scene images if any */
    if (r->sceneLoaded) {
        vkDeviceWaitIdle(r->device);
        vkDestroyImageView(r->device, r->colorView, NULL);
        vkDestroyImageView(r->device, r->depthView, NULL);
        vkDestroyImageView(r->device, r->inpaintView, NULL);
        vkDestroyImage(r->device, r->colorImg, NULL);
        vkDestroyImage(r->device, r->depthImg, NULL);
        vkDestroyImage(r->device, r->inpaintImg, NULL);
        vkFreeMemory(r->device, r->colorMem, NULL);
        vkFreeMemory(r->device, r->depthMem, NULL);
        vkFreeMemory(r->device, r->inpaintMem, NULL);
    }

    /* Color texture (RGBA) */
    createImage(r, cw, ch, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &r->colorImg, &r->colorMem);
    createImageView(r->device, r->colorImg, VK_FORMAT_R8G8B8A8_UNORM, &r->colorView);
    uploadImageData(r, r->colorImg, cw, ch, color_rgba, (VkDeviceSize)cw * ch * 4,
                    VK_FORMAT_R8G8B8A8_UNORM);

    /* Depth texture (R8) — expand grayscale to match staging buffer alignment */
    createImage(r, dw, dh, VK_FORMAT_R8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &r->depthImg, &r->depthMem);
    createImageView(r->device, r->depthImg, VK_FORMAT_R8_UNORM, &r->depthView);
    uploadImageData(r, r->depthImg, dw, dh, depth_gray, (VkDeviceSize)dw * dh,
                    VK_FORMAT_R8_UNORM);

    /* Inpaint texture (RGBA) — use color if no inpaint */
    const uint8_t *inp_data = inpaint_rgba ? inpaint_rgba : color_rgba;
    int inp_w = inpaint_rgba ? iw : cw;
    int inp_h = inpaint_rgba ? ih : ch;
    createImage(r, inp_w, inp_h, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &r->inpaintImg, &r->inpaintMem);
    createImageView(r->device, r->inpaintImg, VK_FORMAT_R8G8B8A8_UNORM, &r->inpaintView);
    uploadImageData(r, r->inpaintImg, inp_w, inp_h, inp_data, (VkDeviceSize)inp_w * inp_h * 4,
                    VK_FORMAT_R8G8B8A8_UNORM);

    /* Update descriptor set */
    VkDescriptorImageInfo outInfo = {VK_NULL_HANDLE, r->outView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo colorInfo = {r->sampler, r->colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depthInfo = {r->sampler, r->depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo inpaintInfo = {r->sampler, r->inpaintView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[4] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, r->ds, 0, 0, 1,
         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &outInfo, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, r->ds, 1, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, r->ds, 2, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, r->ds, 3, 0, 1,
         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &inpaintInfo, NULL, NULL},
    };
    vkUpdateDescriptorSets(r->device, 4, writes, 0, NULL);

    r->sceneLoaded = 1;
    return 0;
}

int dr_render_frame(void *handle, const float *params, int n_params,
                    uint8_t *output_rgba) {
    DepthRenderer *r = (DepthRenderer *)handle;
    if (!r || !r->sceneLoaded) return -1;

    /* Push constant data: copy params, pad to MAX_PUSH */
    float pc[MAX_PUSH / 4];
    memset(pc, 0, sizeof(pc));
    int count = n_params < (MAX_PUSH / 4) ? n_params : (MAX_PUSH / 4);
    memcpy(pc, params, count * sizeof(float));

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(r->cmdBuf, 0);
    vkBeginCommandBuffer(r->cmdBuf, &bi);

    /* Transition output to GENERAL for compute write */
    transitionLayout(r->cmdBuf, r->outImg,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
        0, VK_ACCESS_SHADER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    /* Dispatch compute */
    vkCmdBindPipeline(r->cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipeline);
    vkCmdBindDescriptorSets(r->cmdBuf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            r->pipeLayout, 0, 1, &r->ds, 0, NULL);
    vkCmdPushConstants(r->cmdBuf, r->pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, MAX_PUSH, pc);

    uint32_t gx = (r->width + 15) / 16;
    uint32_t gy = (r->height + 15) / 16;
    vkCmdDispatch(r->cmdBuf, gx, gy, 1);

    /* Barrier: compute write → transfer read */
    transitionLayout(r->cmdBuf, r->outImg,
        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    /* Copy output image → staging buffer */
    VkBufferImageCopy region = {0};
    region.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = (VkExtent3D){r->width, r->height, 1};
    vkCmdCopyImageToBuffer(r->cmdBuf, r->outImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->staging, 1, &region);

    vkEndCommandBuffer(r->cmdBuf);
    submitAndWait(r);

    /* Readback */
    void *mapped;
    vkMapMemory(r->device, r->stagingMem, 0, r->stagingSize, 0, &mapped);
    memcpy(output_rgba, mapped, (size_t)r->width * r->height * 4);
    vkUnmapMemory(r->device, r->stagingMem);

    return 0;
}

void dr_destroy(void *handle) {
    DepthRenderer *r = (DepthRenderer *)handle;
    if (!r) return;
    vkDeviceWaitIdle(r->device);

    if (r->sceneLoaded) {
        vkDestroyImageView(r->device, r->colorView, NULL);
        vkDestroyImageView(r->device, r->depthView, NULL);
        vkDestroyImageView(r->device, r->inpaintView, NULL);
        vkDestroyImage(r->device, r->colorImg, NULL);
        vkDestroyImage(r->device, r->depthImg, NULL);
        vkDestroyImage(r->device, r->inpaintImg, NULL);
        vkFreeMemory(r->device, r->colorMem, NULL);
        vkFreeMemory(r->device, r->depthMem, NULL);
        vkFreeMemory(r->device, r->inpaintMem, NULL);
    }

    vkDestroyImageView(r->device, r->outView, NULL);
    vkDestroyImage(r->device, r->outImg, NULL);
    vkFreeMemory(r->device, r->outMem, NULL);

    vkDestroyBuffer(r->device, r->staging, NULL);
    vkFreeMemory(r->device, r->stagingMem, NULL);

    vkDestroySampler(r->device, r->sampler, NULL);
    vkDestroyDescriptorPool(r->device, r->dsPool, NULL);
    vkDestroyPipeline(r->device, r->pipeline, NULL);
    vkDestroyPipelineLayout(r->device, r->pipeLayout, NULL);
    vkDestroyDescriptorSetLayout(r->device, r->dsLayout, NULL);
    vkDestroyShaderModule(r->device, r->shader, NULL);
    vkDestroyFence(r->device, r->fence, NULL);
    vkDestroyCommandPool(r->device, r->cmdPool, NULL);
    vkDestroyDevice(r->device, NULL);
    vkDestroyInstance(r->instance, NULL);
    free(r);
}
