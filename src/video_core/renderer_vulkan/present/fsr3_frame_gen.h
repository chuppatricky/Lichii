// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// The built-in frame generation ("FSR3" in the settings; it doesn't use AMD's FSR 3 code).
/// Estimates motion between the last two game frames from the picture alone and builds the frames
/// between them:
///  - each game frame: a brightness pyramid at 1/8, 1/16 and 1/32 of the presented size, a full
///    search at 1/32 (+-6 texels, so +-192 pixels), refinement at 1/16 and 1/8 from the median
///    coarse vectors, and a 3x3 median of the result;
///  - each generated frame: per pixel, the vector among a few nearby field vectors and zero whose
///    previous-frame and current-frame samples agree best, then those two samples blended.
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    void Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                 VkImageView current_view, u64 frame_count);

    void Generate(const Device& device, Scheduler& scheduler, VkImageView destination_storage_view,
                  float blend_factor, float flow_scale);

    [[nodiscard]] bool IsReady() const {
        return m_initialized && m_has_history;
    }

    /// The older and the newer of the two game frames being interpolated (debug dumps).
    [[nodiscard]] VkImage HistoryImage() const {
        return *m_history.image;
    }
    [[nodiscard]] VkImage CurrentImage() const {
        return *m_current.image;
    }

    /// Presented pixels per texel of each pyramid level, finest first.
    static constexpr std::array<u32, 3> LevelFactors{8, 16, 32};

private:
    struct Pass {
        vk::DescriptorSetLayout set_layout;
        vk::PipelineLayout pipeline_layout;
        vk::Pipeline pipeline;
    };
    struct OwnedImage {
        vk::Image image;
        vk::ImageView view;
        VkExtent2D extent{};
    };

    void CreateResources(const Device& device);
    void CreatePasses(const Device& device);
    void WriteFlowDescriptors(const Device& device);
    VkDescriptorSet InterpolationSet(const Device& device, Scheduler& scheduler,
                                     VkImageView destination);
    void RecordPyramid(vk::CommandBuffer cmdbuf, size_t parity) const;

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    std::array<vk::ShaderModule, 5> m_shaders;

    Pass m_down;
    Pass m_search;
    Pass m_refine;
    Pass m_median;
    Pass m_interp;

    vk::DescriptorPool m_desc_pool;
    std::array<vk::DescriptorSets, 2> m_down_sets;   // per pyramid parity, one per level
    std::array<vk::DescriptorSets, 2> m_search_sets; // per parity
    std::array<vk::DescriptorSets, 2> m_refine_sets; // per parity, finest level first
    vk::DescriptorSets m_median_set;

    // Generated frames go to the presented frames' images, which rotate: one set per image so a
    // set is never rewritten while an earlier frame's commands may still use it.
    struct InterpSet {
        VkImageView destination;
        vk::DescriptorSets set;
    };
    std::vector<InterpSet> m_interp_sets;

    // The last two game frames: history is the older one.
    OwnedImage m_history;
    OwnedImage m_current;

    // Brightness pyramids of the last two game frames (swapped each frame) and motion fields.
    std::array<std::array<OwnedImage, 3>, 2> m_pyramid;
    std::array<OwnedImage, 3> m_vectors;
    OwnedImage m_final_vectors;

    size_t m_parity{};
    bool m_initialized{};
    bool m_has_history{};
    bool m_layouts_ready{};

    // Duplicate-frame detection: skip the flow when the frame hasn't changed
    u64 m_last_frame_count{};
    bool m_flow_valid{};
};

} // namespace Vulkan
