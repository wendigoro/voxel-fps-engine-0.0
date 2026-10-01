#pragma once
// World render target + post chain (view only).
//
// Frame structure:
//   1. world pass  -> offscreen colour+depth at renderScale x window size
//   2. post pass   -> full-screen triangle samples the world image through
//                     the effect chain in shaders/post.frag onto the swapchain
//   3. overlay / UI passes draw on the swapchain image afterwards (main.cpp)
//
// Rendering the world offscreen is what makes the render scale real (before
// this, RENDER_SCALE was only reported) and gives every full-screen effect a
// place to run. The world pass's final layout is SHADER_READ_ONLY_OPTIMAL and
// its outgoing dependency makes the colour writes visible to the post pass's
// fragment shader; its incoming dependency waits for the previous frame's
// post read before overwriting the shared target.

#include <vulkan/vulkan.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace postfx {

// Matches the push-constant block in shaders/post.frag.
struct PostParams {
    float posterize = 0.0f;
    float dither = 0.0f;
    float crush = 1.0f;
    float time = 0.0f;
};

static constexpr VkFormat kWorldColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
static constexpr VkFormat kWorldDepthFormat = VK_FORMAT_D32_SFLOAT;

struct PostFx {
    VkDevice dev = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;

    VkRenderPass worldPass = VK_NULL_HANDLE;
    VkRenderPass postPass = VK_NULL_HANDLE;

    // Offscreen world target.
    VkExtent2D extent{0, 0};
    float scale = 1.0f;
    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE, depthMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE, depthView = VK_NULL_HANDLE;
    VkFramebuffer worldFb = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> postFbs; // one per swapchain image

    VkSampler samplerNearest = VK_NULL_HANDLE, samplerLinear = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet setNearest = VK_NULL_HANDLE, setLinear = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    static void check(VkResult r, const char* what) {
        if (r != VK_SUCCESS) throw std::runtime_error(std::string("postfx: ") + what);
    }

    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
        throw std::runtime_error("postfx: no memory type");
    }

    void init(VkDevice d, VkPhysicalDevice p, VkFormat swapFormat) {
        dev = d;
        phys = p;
        createWorldPass();
        createPostPass(swapFormat);
        createSamplersAndDescriptors();
    }

    void createWorldPass() {
        VkAttachmentDescription a[2]{};
        a[0].format = kWorldColorFormat;
        a[0].samples = VK_SAMPLE_COUNT_1_BIT;
        a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a[1] = a[0];
        a[1].format = kWorldDepthFormat;
        a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        sub.pDepthStencilAttachment = &dref;

        VkSubpassDependency deps[2]{};
        // In: the previous frame's post pass may still be sampling this image
        // (write-after-read), and its world pass used the same depth image.
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Out: the post pass samples the colour.
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 2;
        ci.pAttachments = a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 2;
        ci.pDependencies = deps;
        check(vkCreateRenderPass(dev, &ci, nullptr, &worldPass), "world render pass");
    }

    void createPostPass(VkFormat swapFormat) {
        VkAttachmentDescription a{};
        a.format = swapFormat;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel is written
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // The overlay pass that follows expects PRESENT_SRC_KHR (RULES.md rule 12).
        a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &cref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        ci.attachmentCount = 1;
        ci.pAttachments = &a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sub;
        ci.dependencyCount = 1;
        ci.pDependencies = &dep;
        check(vkCreateRenderPass(dev, &ci, nullptr, &postPass), "post render pass");
    }

    void createSamplersAndDescriptors() {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        check(vkCreateSampler(dev, &si, nullptr, &samplerNearest), "nearest sampler");
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        check(vkCreateSampler(dev, &si, nullptr, &samplerLinear), "linear sampler");

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1;
        lci.pBindings = &b;
        check(vkCreateDescriptorSetLayout(dev, &lci, nullptr, &dsl), "post set layout");

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 2;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        check(vkCreateDescriptorPool(dev, &pci, nullptr, &pool), "post pool");
        VkDescriptorSetLayout layouts[2] = {dsl, dsl};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VkDescriptorSet sets[2];
        check(vkAllocateDescriptorSets(dev, &ai, sets), "post sets");
        setNearest = sets[0];
        setLinear = sets[1];
    }

    VkShaderModule module(const std::vector<char>& code) const {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = code.size();
        ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule m = VK_NULL_HANDLE;
        check(vkCreateShaderModule(dev, &ci, nullptr, &m), "shader module");
        return m;
    }

    void createPipeline(const std::vector<char>& vertSpv, const std::vector<char>& fragSpv) {
        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostParams)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        check(vkCreatePipelineLayout(dev, &plci, nullptr, &layout), "post pipeline layout");

        VkShaderModule vs = module(vertSpv), fs = module(fragSpv);
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        st[0].module = vs;
        st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        st[1].module = fs;
        st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &cba;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.stageCount = 2;
        pci.pStages = st;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &ds;
        pci.layout = layout;
        pci.renderPass = postPass;
        const VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
        vkDestroyShaderModule(dev, vs, nullptr);
        vkDestroyShaderModule(dev, fs, nullptr);
        check(r, "post pipeline");
    }

    void createImage(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage& img,
                     VkDeviceMemory& mem, VkImageView& view) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.extent = {extent.width, extent.height, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.format = fmt;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ii.usage = usage;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(dev, &ii, nullptr, &img), "target image");
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev, img, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(dev, &ai, nullptr, &mem), "target memory");
        vkBindImageMemory(dev, img, mem, 0);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        check(vkCreateImageView(dev, &vi, nullptr, &view), "target view");
    }

    // (Re)build everything sized by the window or the render scale. The caller
    // has idled the device.
    void createTargets(VkExtent2D window, float renderScale, const std::vector<VkImageView>& swapViews) {
        destroyTargets();
        scale = std::clamp(renderScale, 0.05f, 1.0f);
        extent.width = std::max(1u, static_cast<uint32_t>(window.width * scale + 0.5f));
        extent.height = std::max(1u, static_cast<uint32_t>(window.height * scale + 0.5f));
        createImage(kWorldColorFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, color, colorMem, colorView);
        createImage(kWorldDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                    depth, depthMem, depthView);

        VkImageView atts[2] = {colorView, depthView};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = worldPass;
        fci.attachmentCount = 2;
        fci.pAttachments = atts;
        fci.width = extent.width;
        fci.height = extent.height;
        fci.layers = 1;
        check(vkCreateFramebuffer(dev, &fci, nullptr, &worldFb), "world framebuffer");

        postFbs.resize(swapViews.size());
        for (size_t i = 0; i < swapViews.size(); ++i) {
            VkFramebufferCreateInfo pf{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            pf.renderPass = postPass;
            pf.attachmentCount = 1;
            pf.pAttachments = &swapViews[i];
            pf.width = window.width;
            pf.height = window.height;
            pf.layers = 1;
            check(vkCreateFramebuffer(dev, &pf, nullptr, &postFbs[i]), "post framebuffer");
        }

        // Point both descriptor sets at the new world image.
        VkDescriptorImageInfo info[2]{};
        info[0] = {samplerNearest, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        info[1] = {samplerLinear, colorView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2]{};
        for (int k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = k == 0 ? setNearest : setLinear;
            w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &info[k];
        }
        vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }

    void destroyTargets() {
        for (VkFramebuffer fb : postFbs) vkDestroyFramebuffer(dev, fb, nullptr);
        postFbs.clear();
        if (worldFb) vkDestroyFramebuffer(dev, worldFb, nullptr);
        if (colorView) vkDestroyImageView(dev, colorView, nullptr);
        if (depthView) vkDestroyImageView(dev, depthView, nullptr);
        if (color) vkDestroyImage(dev, color, nullptr);
        if (depth) vkDestroyImage(dev, depth, nullptr);
        if (colorMem) vkFreeMemory(dev, colorMem, nullptr);
        if (depthMem) vkFreeMemory(dev, depthMem, nullptr);
        worldFb = VK_NULL_HANDLE;
        colorView = depthView = VK_NULL_HANDLE;
        color = depth = VK_NULL_HANDLE;
        colorMem = depthMem = VK_NULL_HANDLE;
    }

    // Draw the world image to swapchain image `imageIndex` through the chain.
    void record(VkCommandBuffer cmd, uint32_t imageIndex, VkExtent2D window, bool nearest,
                const PostParams& params) const {
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = postPass;
        rp.framebuffer = postFbs[imageIndex];
        rp.renderArea.extent = window;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkViewport v{0.0f, 0.0f, float(window.width), float(window.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, window};
        vkCmdSetViewport(cmd, 0, 1, &v);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        const VkDescriptorSet set = nearest ? setNearest : setLinear;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostParams), &params);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }

    void destroy() {
        if (!dev) return;
        destroyTargets();
        if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
        if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
        if (dsl) vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
        if (samplerNearest) vkDestroySampler(dev, samplerNearest, nullptr);
        if (samplerLinear) vkDestroySampler(dev, samplerLinear, nullptr);
        if (postPass) vkDestroyRenderPass(dev, postPass, nullptr);
        if (worldPass) vkDestroyRenderPass(dev, worldPass, nullptr);
        *this = PostFx{};
    }
};

} // namespace postfx
