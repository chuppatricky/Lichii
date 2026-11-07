// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fg_flow_median_comp_spv.h"
#include "video_core/host_shaders/fg_flow_refine_comp_spv.h"
#include "video_core/host_shaders/fg_flow_search_comp_spv.h"
#include "video_core/host_shaders/fg_luma_down_comp_spv.h"
#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"

namespace Vulkan {

namespace {

// ΓöÇΓöÇ Precise pipeline barriers ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

void BarrierComputeWriteToRead(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

// ΓöÇΓöÇ Batched dual-copy ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
// Copy one source image into TWO destinations with only 3 barrier calls
// instead of 6 (avoiding redundant source transitions).
void CopyImageToTwo(vk::CommandBuffer cmdbuf, VkImage source,
                    VkImage dest_a, VkImage dest_b, VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    // 1. Transition source ΓåÆ TRANSFER_SRC, both dests ΓåÆ TRANSFER_DST (batched)
    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(dest_a, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        MakeBarrier(dest_b, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    // 2. Copy source ΓåÆ dest_a, then source ΓåÆ dest_b (source stays TRANSFER_SRC)
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_a,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_b,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    // 3. Transition all three back to GENERAL (batched)
    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_a, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(dest_b, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}

// Single-image copy (for first-frame init)
[[maybe_unused]] void CopyImageToImage(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination,
                      VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    const std::array before{
        MakeBarrier(source, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(destination, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, {}, {}, after);
}



// Every compute pass of the flow reads what the one before wrote.
void ComputeToCompute(vk::CommandBuffer cmdbuf) {
    const VkMemoryBarrier barrier{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, barrier);
}

u32 Groups(u32 size) {
    return (size + 7) / 8;
}

// Brightness differences are in 0..1 per texel; this many per texel of vector length keeps flat
// areas at zero motion without outweighing a real match.
constexpr float LAMBDA = 0.0002f;
constexpr s32 COARSE_RADIUS = 6;
constexpr size_t MAX_INTERP_SETS = 8;

struct DownPushConstants {
    s32 dst_w;
    s32 dst_h;
    float inv_src_w;
    float inv_src_h;
    s32 factor;
    s32 from_rgb;
};

struct SearchPushConstants {
    s32 w;
    s32 h;
    s32 radius;
    float lambda;
};

struct RefinePushConstants {
    s32 w;
    s32 h;
    s32 coarse_w;
    s32 coarse_h;
    s32 ratio;
    float lambda;
};

struct MedianPushConstants {
    s32 w;
    s32 h;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float blend_factor;
    float texel_pixels;
};

VkWriteDescriptorSet StorageWrite(const VkDescriptorImageInfo& info, VkDescriptorSet set,
                                  u32 binding) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .pImageInfo = &info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
}

} // anonymous namespace

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent}, m_format{format} {
    CreateResources(device);
    CreatePasses(device);
    WriteFlowDescriptors(device);
    m_initialized = true;
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    const auto make = [&](VkExtent2D size, VkFormat format, VkImageUsageFlags extra = 0) {
        OwnedImage owned;
        owned.extent = size;
        owned.image = CreateWrappedImage(m_allocator, size, format, extra);
        owned.view = CreateWrappedImageView(device, owned.image, format);
        return owned;
    };
    m_history = make(m_extent, m_format);
    // Copied into the history every frame, so it must be a transfer source too (AMD drivers pick
    // the image's compression from its usage flags).
    m_current = make(m_extent, m_format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    for (size_t level = 0; level < LevelFactors.size(); ++level) {
        // Whole blocks only, so the downsample never reads past the frame's edge.
        const VkExtent2D size{(std::max)(1u, m_extent.width / LevelFactors[level]),
                              (std::max)(1u, m_extent.height / LevelFactors[level])};
        for (auto& pyramid : m_pyramid) {
            pyramid[level] = make(size, VK_FORMAT_R16_SFLOAT);
        }
        m_vectors[level] = make(size, VK_FORMAT_R16G16_SFLOAT);
    }
    m_final_vectors = make(m_vectors[0].extent, VK_FORMAT_R16G16_SFLOAT);

    // Sets: 2 x 3 downsample, 2 search, 2 x 2 refine, 1 median, up to MAX_INTERP_SETS.
    constexpr u32 max_sets = 6 + 2 + 4 + 1 + MAX_INTERP_SETS;
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             6 + 2 * 2 + 4 * 3 + 1 + MAX_INTERP_SETS * 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, max_sets},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = max_sets,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreatePasses(const Device& device) {
    m_shaders = {
        BuildShader(device, FG_LUMA_DOWN_COMP_SPV),
        BuildShader(device, FG_FLOW_SEARCH_COMP_SPV),
        BuildShader(device, FG_FLOW_REFINE_COMP_SPV),
        BuildShader(device, FG_FLOW_MEDIAN_COMP_SPV),
        BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV),
    };
    const auto make = [&](Pass& pass, size_t sampled, u32 push_size, VkShaderModule shader) {
        std::vector<VkDescriptorType> types(sampled, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        types.push_back(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        pass.set_layout = CreateWrappedDescriptorSetLayout(
            device, std::span<const VkDescriptorType>{types}, VK_SHADER_STAGE_COMPUTE_BIT);
        const VkPushConstantRange push_range{
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = push_size,
        };
        pass.pipeline_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = pass.set_layout.address(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_range,
        });
        pass.pipeline = CreateWrappedComputePipeline(device, pass.pipeline_layout, shader);
    };
    make(m_down, 1, sizeof(DownPushConstants), *m_shaders[0]);
    make(m_search, 2, sizeof(SearchPushConstants), *m_shaders[1]);
    make(m_refine, 3, sizeof(RefinePushConstants), *m_shaders[2]);
    make(m_median, 1, sizeof(MedianPushConstants), *m_shaders[3]);
    make(m_interp, 3, sizeof(InterpPushConstants), *m_shaders[4]);
}

void Fsr3FrameGen::WriteFlowDescriptors(const Device& device) {
    const auto allocate = [&](Pass& pass, size_t count) {
        std::vector<VkDescriptorSetLayout> layouts(count, *pass.set_layout);
        return CreateWrappedDescriptorSets(m_desc_pool, layouts);
    };
    std::vector<VkDescriptorImageInfo> sampled;
    sampled.reserve(64);
    std::vector<VkDescriptorImageInfo> storage;
    storage.reserve(32);
    std::vector<VkWriteDescriptorSet> writes;
    const auto sample = [&](VkImageView view, VkDescriptorSet set, u32 binding) {
        writes.push_back(CreateWriteDescriptorSet(sampled, *m_linear_sampler, view, set, binding));
    };
    const auto store = [&](VkImageView view, VkDescriptorSet set, u32 binding) {
        const auto& info = storage.emplace_back(VkDescriptorImageInfo{
            .sampler = VK_NULL_HANDLE,
            .imageView = view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        });
        writes.push_back(StorageWrite(info, set, binding));
    };

    for (size_t parity = 0; parity < 2; ++parity) {
        const auto& current = m_pyramid[parity];
        const auto& previous = m_pyramid[1 - parity];

        m_down_sets[parity] = allocate(m_down, 3);
        sample(*m_current.view, m_down_sets[parity][0], 0);
        store(*current[0].view, m_down_sets[parity][0], 1);
        for (size_t level = 1; level < 3; ++level) {
            sample(*current[level - 1].view, m_down_sets[parity][level], 0);
            store(*current[level].view, m_down_sets[parity][level], 1);
        }

        m_search_sets[parity] = allocate(m_search, 1);
        sample(*current[2].view, m_search_sets[parity][0], 0);
        sample(*previous[2].view, m_search_sets[parity][0], 1);
        store(*m_vectors[2].view, m_search_sets[parity][0], 2);

        // [0] refines level 1 from level 2, [1] level 0 from level 1.
        m_refine_sets[parity] = allocate(m_refine, 2);
        for (size_t i = 0; i < 2; ++i) {
            const size_t level = 1 - i;
            sample(*current[level].view, m_refine_sets[parity][i], 0);
            sample(*previous[level].view, m_refine_sets[parity][i], 1);
            sample(*m_vectors[level + 1].view, m_refine_sets[parity][i], 2);
            store(*m_vectors[level].view, m_refine_sets[parity][i], 3);
        }
    }
    m_median_set = allocate(m_median, 1);
    sample(*m_vectors[0].view, m_median_set[0], 0);
    store(*m_final_vectors.view, m_median_set[0], 1);

    device.GetLogical().UpdateDescriptorSets(writes, {});
}

VkDescriptorSet Fsr3FrameGen::InterpolationSet(const Device& device, Scheduler& scheduler,
                                               VkImageView destination) {
    for (const auto& entry : m_interp_sets) {
        if (entry.destination == destination) {
            return entry.set[0];
        }
    }
    if (m_interp_sets.size() >= MAX_INTERP_SETS) {
        // The presented frames were recreated: wait until no command uses the old sets.
        scheduler.Finish();
        m_interp_sets.clear();
    }
    std::vector<VkDescriptorSetLayout> layouts{*m_interp.set_layout};
    auto& entry = m_interp_sets.emplace_back(
        InterpSet{destination, CreateWrappedDescriptorSets(m_desc_pool, layouts)});

    std::vector<VkDescriptorImageInfo> sampled;
    sampled.reserve(3);
    std::vector<VkWriteDescriptorSet> writes{
        CreateWriteDescriptorSet(sampled, *m_linear_sampler, *m_current.view, entry.set[0], 0),
        CreateWriteDescriptorSet(sampled, *m_linear_sampler, *m_history.view, entry.set[0], 1),
        CreateWriteDescriptorSet(sampled, *m_linear_sampler, *m_final_vectors.view, entry.set[0],
                                 2),
    };
    const VkDescriptorImageInfo destination_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = destination,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    writes.push_back(StorageWrite(destination_info, entry.set[0], 3));
    device.GetLogical().UpdateDescriptorSets(writes, {});
    return entry.set[0];
}

void Fsr3FrameGen::RecordPyramid(vk::CommandBuffer cmdbuf, size_t parity) const {
    cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_down.pipeline);
    for (size_t level = 0; level < 3; ++level) {
        const VkExtent2D source = level == 0 ? m_extent : m_pyramid[parity][level - 1].extent;
        const VkExtent2D destination = m_pyramid[parity][level].extent;
        const DownPushConstants constants{
            .dst_w = static_cast<s32>(destination.width),
            .dst_h = static_cast<s32>(destination.height),
            .inv_src_w = 1.0f / static_cast<float>(source.width),
            .inv_src_h = 1.0f / static_cast<float>(source.height),
            .factor = level == 0 ? static_cast<s32>(LevelFactors[0])
                                 : static_cast<s32>(LevelFactors[level] / LevelFactors[level - 1]),
            .from_rgb = level == 0 ? 1 : 0,
        };
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_down.pipeline_layout, 0,
                                  m_down_sets[parity][level], {});
        cmdbuf.PushConstants(*m_down.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(Groups(destination.width), Groups(destination.height), 1);
        ComputeToCompute(cmdbuf);
    }
}

void Fsr3FrameGen::Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                           VkImageView current_view, u64 frame_count) {
    static_cast<void>(device);
    static_cast<void>(current_view);

    if (!m_layouts_ready) {
        scheduler.Record([this](vk::CommandBuffer cmdbuf) {
            std::vector<VkImageMemoryBarrier> barriers;
            const auto add = [&](const OwnedImage& owned) {
                barriers.push_back(MakeBarrier(*owned.image, 0,
                                               VK_ACCESS_SHADER_READ_BIT |
                                                   VK_ACCESS_SHADER_WRITE_BIT,
                                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL));
            };
            for (const auto& pyramid : m_pyramid) {
                for (const auto& level : pyramid) {
                    add(level);
                }
            }
            for (const auto& level : m_vectors) {
                add(level);
            }
            add(m_final_vectors);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barriers);
        });
        m_layouts_ready = true;
    }

    if (!m_has_history) {
        // First frame: it is both frames, and its pyramid the previous one for the next frame.
        const size_t parity = m_parity;
        scheduler.Record([this, current_frame, parity](vk::CommandBuffer cmdbuf) {
            CopyImageToTwo(cmdbuf, current_frame, *m_history.image, *m_current.image, m_extent);
            RecordPyramid(cmdbuf, parity);
        });
        m_has_history = true;
        m_last_frame_count = frame_count;
        return;
    }

    // ── Duplicate-frame detection ──
    if (frame_count == m_last_frame_count && m_flow_valid) {
        return;
    }
    m_last_frame_count = frame_count;
    m_parity = 1 - m_parity;
    const size_t parity = m_parity;

    scheduler.Record([this, current_frame, parity](vk::CommandBuffer cmdbuf) {
        // The last real frame becomes the history and this one the current copy, before the flow
        // reads them: Generate() interpolates between the two.
        CopyImageToImage(cmdbuf, *m_current.image, *m_history.image, m_extent);
        CopyImageToImage(cmdbuf, current_frame, *m_current.image, m_extent);
        // The previous frame's passes read the images this frame's passes write.
        ComputeToCompute(cmdbuf);

        RecordPyramid(cmdbuf, parity);

        const VkExtent2D coarse = m_vectors[2].extent;
        const SearchPushConstants search{
            .w = static_cast<s32>(coarse.width),
            .h = static_cast<s32>(coarse.height),
            .radius = COARSE_RADIUS,
            .lambda = LAMBDA,
        };
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_search.pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_search.pipeline_layout, 0,
                                  m_search_sets[parity][0], {});
        cmdbuf.PushConstants(*m_search.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, search);
        cmdbuf.Dispatch(Groups(coarse.width), Groups(coarse.height), 1);
        ComputeToCompute(cmdbuf);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_refine.pipeline);
        for (size_t i = 0; i < 2; ++i) {
            const size_t level = 1 - i;
            const VkExtent2D size = m_vectors[level].extent;
            const VkExtent2D from = m_vectors[level + 1].extent;
            const RefinePushConstants refine{
                .w = static_cast<s32>(size.width),
                .h = static_cast<s32>(size.height),
                .coarse_w = static_cast<s32>(from.width),
                .coarse_h = static_cast<s32>(from.height),
                .ratio = static_cast<s32>(LevelFactors[level + 1] / LevelFactors[level]),
                .lambda = LAMBDA,
            };
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_refine.pipeline_layout,
                                      0, m_refine_sets[parity][i], {});
            cmdbuf.PushConstants(*m_refine.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, refine);
            cmdbuf.Dispatch(Groups(size.width), Groups(size.height), 1);
            ComputeToCompute(cmdbuf);
        }

        const VkExtent2D fine = m_final_vectors.extent;
        const MedianPushConstants median{
            .w = static_cast<s32>(fine.width),
            .h = static_cast<s32>(fine.height),
        };
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_median.pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_median.pipeline_layout, 0,
                                  m_median_set[0], {});
        cmdbuf.PushConstants(*m_median.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, median);
        cmdbuf.Dispatch(Groups(fine.width), Groups(fine.height), 1);
        ComputeToCompute(cmdbuf);
    });

    m_flow_valid = true;
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler,
                            VkImageView destination_storage_view, float blend_factor,
                            float flow_scale) {
    static_cast<void>(flow_scale);
    if (!m_has_history || !m_flow_valid) {
        return;
    }
    const VkDescriptorSet set = InterpolationSet(device, scheduler, destination_storage_view);

    // Interpolation dispatches at full resolution
    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .blend_factor = blend_factor,
        .texel_pixels = static_cast<float>(LevelFactors[0]),
    };

    scheduler.Record([this, set, constants, out_w, out_h](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp.pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *m_interp.pipeline_layout, 0,
                                  set, {});
        cmdbuf.PushConstants(*m_interp.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch(Groups(out_w), Groups(out_h), 1);
    });
}

} // namespace Vulkan
