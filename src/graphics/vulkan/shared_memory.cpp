/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/graphics/vulkan/deferred_command_buffer.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(vulkan_sparse_shared_memory, true, "GPU/Vulkan",
                    "Use sparse shared memory on Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_shared_memory_direct_upload, false, "GPU/Vulkan",
                    "On unified memory GPUs (such as Apple silicon), place shared memory in "
                    "host-visible device-local memory and write guest data to it from the CPU "
                    "when no GPU work submitted or recorded may be using the pages, instead of "
                    "copying it on the GPU, which ends the render pass")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vulkan_shared_memory_direct_upload_skip_unchanged, false, "GPU/Vulkan",
                    "With vulkan_shared_memory_direct_upload, compare pages that GPU work may "
                    "still be reading with guest memory, and skip uploading them if unchanged "
                    "(such as after clear_memory_page_state) instead of copying them on the GPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::vulkan {

VulkanSharedMemory::VulkanSharedMemory(VulkanCommandProcessor& command_processor,
                                       memory::Memory& memory,
                                       VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() {
  Shutdown(true);
}

bool VulkanSharedMemory::Initialize() {
  InitializeCommon();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Memory types that both the GPU and the CPU can access directly, for direct
  // uploads. The buffer is mapped as a whole then, so it's not sparse.
  uint32_t direct_upload_memory_types = 0;
  if (REXCVAR_GET(vulkan_shared_memory_direct_upload)) {
    direct_upload_memory_types = vulkan_device->memory_types().device_local &
                                 vulkan_device->memory_types().host_visible &
                                 vulkan_device->memory_types().host_coherent;
  }

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  if (REXCVAR_GET(vulkan_sparse_shared_memory) && !direct_upload_memory_types &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) == VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
      if (rex::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                    vulkan_device->memory_types().device_local,
                                &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        rex::bit_scan_forward(std::max(uint64_t(buffer_memory_requirements.alignment), uint64_t(1)),
                              &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(std::max(
              allocation_size_log2,
              std::max(kHostGpuMemoryOptimalSparseAllocationLog2, kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        REXGPU_ERROR(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
                   kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    REXGPU_INFO(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB Vulkan buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_, &buffer_memory_requirements);
    // For direct uploads, pick a host-visible type explicitly - the first
    // device-local one may be GPU-only (on MoltenVK, it's Private, followed by
    // Shared).
    bool direct_upload_memory_type_used = rex::bit_scan_forward(
        buffer_memory_requirements.memoryTypeBits & direct_upload_memory_types,
        &buffer_memory_type_);
    if (!direct_upload_memory_type_used &&
        !rex::bit_scan_forward(
            buffer_memory_requirements.memoryTypeBits & vulkan_device->memory_types().device_local,
            &buffer_memory_type_)) {
      REXGPU_ERROR(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last = &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize = buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext = &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(&buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr, &buffer_memory) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
    if (direct_upload_memory_type_used) {
      void* direct_upload_mapping;
      if (dfn.vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &direct_upload_mapping) ==
          VK_SUCCESS) {
        direct_upload_mapping_ = reinterpret_cast<uint8_t*>(direct_upload_mapping);
        size_t page_count = size_t(kBufferSize >> page_size_log2());
        page_gpu_use_submissions_.assign(page_count, 0);
        page_gpu_write_submissions_.assign(page_count, 0);
        page_copy_submissions_.assign(page_count, 0);
        page_copy_sources_.assign(page_count, nullptr);
      } else {
        REXGPU_ERROR("Shared memory: Failed to map the Vulkan buffer for direct uploads");
      }
    }
  }
  if (REXCVAR_GET(vulkan_shared_memory_direct_upload)) {
    if (direct_upload_mapping_) {
      REXGPU_INFO("Shared memory: Direct uploads to Vulkan memory type {}, {} KB pages",
                  buffer_memory_type_, (uint32_t(1) << page_size_log2()) >> 10);
    } else {
      REXGPU_INFO("Shared memory: Direct uploads are not available, uploading with GPU copies");
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      rex::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize, size_t(1)
                                                                           << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Freeing the memory also unmaps it.
  direct_upload_mapping_ = nullptr;
  page_gpu_use_submissions_.clear();
  page_gpu_write_submissions_.clear();
  page_copy_submissions_.clear();
  page_copy_sources_.clear();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() {
  upload_buffer_pool_->FlushWrites();

  if (direct_upload_mapping_) {
    uint64_t time_ms = rex::chrono::Clock::QueryHostUptimeMillis();
    if (time_ms - direct_upload_stats_time_ms_ >= 5000) {
      if (direct_upload_stats_time_ms_) {
        REXGPU_INFO(
            "Shared memory direct upload: {} uploads, {} still copied on the GPU; pages: {} "
            "written directly, {} unchanged, {} copied",
            direct_upload_stats_calls_, direct_upload_stats_copy_calls_,
            direct_upload_stats_pages_written_, direct_upload_stats_pages_unchanged_,
            direct_upload_stats_pages_copied_);
      }
      direct_upload_stats_time_ms_ = time_ms;
      direct_upload_stats_calls_ = 0;
      direct_upload_stats_copy_calls_ = 0;
      direct_upload_stats_pages_written_ = 0;
      direct_upload_stats_pages_unchanged_ = 0;
      direct_upload_stats_pages_copied_ = 0;
    }
  }
}

void VulkanSharedMemory::Use(Usage usage, std::pair<uint32_t, uint32_t> written_range) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second = std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask, dst_access_mask,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  }
  // Copy uploads mark their exact pages in UploadRangesWithCopy instead, as
  // their barrier range also covers the gaps between the copied ranges, and
  // their data is known.
  if (direct_upload_mapping_ && written_range.second && usage != Usage::kTransferDestination) {
    MarkPagesForCurrentSubmission(page_gpu_use_submissions_, written_range.first,
                                  written_range.second);
    MarkPagesForCurrentSubmission(page_gpu_write_submissions_, written_range.first,
                                  written_range.second);
  }
  last_written_range_ = written_range;
}

void VulkanSharedMemory::MarkRangeUsedByCurrentSubmission(uint32_t start, uint32_t length) {
  if (direct_upload_mapping_) {
    MarkPagesForCurrentSubmission(page_gpu_use_submissions_, start, length);
  }
}

void VulkanSharedMemory::RangesRequested(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  if (!direct_upload_mapping_) {
    return;
  }
  for (const std::pair<uint32_t, uint32_t>& range : ranges) {
    MarkPagesForCurrentSubmission(page_gpu_use_submissions_, range.first, range.second);
  }
}

void VulkanSharedMemory::MarkPagesForCurrentSubmission(std::vector<uint64_t>& page_submissions,
                                                       uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = (start + length - 1) >> page_size_log2();
  std::fill(page_submissions.begin() + page_first, page_submissions.begin() + (page_last + 1),
            command_processor_.GetCurrentSubmission());
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                          uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize = length_allocations
                                        << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) != VK_SUCCESS) {
    REXGPU_ERROR("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
      VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |= VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

bool VulkanSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (!direct_upload_mapping_) {
    return UploadRangesWithCopy(upload_page_ranges);
  }
  ++direct_upload_stats_calls_;
  UploadRangesDirectly(upload_page_ranges);
  if (direct_upload_copy_page_ranges_.empty()) {
    return true;
  }
  ++direct_upload_stats_copy_calls_;
  return UploadRangesWithCopy(direct_upload_copy_page_ranges_);
}

void VulkanSharedMemory::UploadRangesDirectly(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  direct_upload_copy_page_ranges_.clear();
  // A CPU write to a page is safe only if no GPU work may still access it -
  // neither a submission in flight, nor commands already recorded in the
  // current submission, which will execute after the write. Such a write needs
  // no barrier: vkQueueSubmit makes earlier host writes to host-coherent memory
  // visible to the submitted commands. Pages that GPU work may still access are
  // uploaded with a copy instead, ordered with that work on the GPU.
  // The completed submission is only updated when a submission begins, so this
  // is conservative within a submission.
  const uint64_t submission_completed = command_processor_.GetCompletedSubmission();
  const bool skip_unchanged = REXCVAR_GET(vulkan_shared_memory_direct_upload_skip_unchanged);
  auto append_copy_pages = [this](uint32_t page_first, uint32_t page_count) {
    direct_upload_stats_pages_copied_ += page_count;
    if (!direct_upload_copy_page_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_range = direct_upload_copy_page_ranges_.back();
      if (last_range.first + last_range.second == page_first) {
        last_range.second += page_count;
        return;
      }
    }
    direct_upload_copy_page_ranges_.emplace_back(page_first, page_count);
  };
  for (const std::pair<uint32_t, uint32_t>& upload_range : upload_page_ranges) {
    uint32_t page = upload_range.first;
    uint32_t page_end = upload_range.first + upload_range.second;
    while (page < page_end) {
      // Take the run of pages that are all unused, or all possibly used, by the
      // GPU work that hasn't completed yet.
      bool page_in_use = page_gpu_use_submissions_[page] > submission_completed;
      uint32_t run_end = page + 1;
      while (run_end < page_end &&
             (page_gpu_use_submissions_[run_end] > submission_completed) == page_in_use) {
        ++run_end;
      }
      uint32_t run_start_bytes = page << page_size_log2();
      uint32_t run_length_bytes = (run_end - page) << page_size_log2();
      if (!page_in_use) {
        MakeRangeValid(run_start_bytes, run_length_bytes, false);
        std::memcpy(direct_upload_mapping_ + run_start_bytes,
                    memory().TranslatePhysical(run_start_bytes), run_length_bytes);
        direct_upload_stats_pages_written_ += run_end - page;
      } else if (!skip_unchanged) {
        append_copy_pages(page, run_end - page);
      } else {
        // Data uploaded again without having been modified (such as after
        // clear_memory_page_state) doesn't need uploading. Compare it with
        // what the page will contain after the GPU work that may still write
        // to it: the source of the last copy upload if that copy may still be
        // pending, otherwise the buffer, which is safe to read while the GPU
        // may only be reading it too. Pending writes by other GPU work can't
        // be predicted.
        MakeRangeValid(run_start_bytes, run_length_bytes, false);
        for (uint32_t run_page = page; run_page < run_end; ++run_page) {
          uint32_t run_page_bytes = run_page << page_size_log2();
          const uint8_t* run_page_gpu_data = nullptr;
          if (page_gpu_write_submissions_[run_page] <= submission_completed) {
            run_page_gpu_data = page_copy_submissions_[run_page] > submission_completed
                                    ? page_copy_sources_[run_page]
                                    : direct_upload_mapping_ + run_page_bytes;
          }
          if (run_page_gpu_data &&
              !std::memcmp(run_page_gpu_data, memory().TranslatePhysical(run_page_bytes),
                           size_t(1) << page_size_log2())) {
            ++direct_upload_stats_pages_unchanged_;
          } else {
            append_copy_pages(run_page, 1);
          }
        }
      }
      page = run_end;
    }
  }
}

bool VulkanSharedMemory::UploadRangesWithCopy(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  // upload_page_ranges are sorted, use them to determine the range for the
  // ordering barrier.
  Use(Usage::kTransferDestination,
      std::make_pair(upload_page_ranges.front().first << page_size_log2(),
                     (upload_page_ranges.back().first + upload_page_ranges.back().second -
                      upload_page_ranges.front().first)
                         << page_size_log2()));
  command_processor_.SubmitBarriers(true, VulkanCommandProcessor::RenderPassReason::kUpload);
  if (VulkanCommandProcessor::FrameCounters* frame_counters = command_processor_.frame_counters()) {
    ++frame_counters->uploads;
    frame_counters->upload_ranges += upload_page_ranges.size();
    for (auto upload_range : upload_page_ranges) {
      frame_counters->upload_bytes += uint64_t(upload_range.second) << page_size_log2();
    }
  }
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset, upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      if (upload_buffer_previous != upload_buffer && !upload_regions_.empty()) {
        assert_true(upload_buffer_previous != VK_NULL_HANDLE);
        command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                       uint32_t(upload_regions_.size()), upload_regions_.data());
        upload_regions_.clear();
      }
      upload_buffer_previous = upload_buffer;
      VkBufferCopy& upload_region = upload_regions_.emplace_back();
      upload_region.srcOffset = upload_buffer_offset;
      upload_region.dstOffset = VkDeviceSize(upload_range_start << page_size_log2());
      upload_region.size = upload_buffer_size;
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      if (direct_upload_mapping_) {
        // The copy writes these pages in the current submission, from data
        // that stays in the upload buffer until the submission is completed.
        for (uint32_t i = 0; i < upload_buffer_pages; ++i) {
          uint32_t upload_page = upload_range_start + i;
          page_gpu_use_submissions_[upload_page] = submission_current;
          page_copy_submissions_[upload_page] = submission_current;
          page_copy_sources_[upload_page] = upload_buffer_mapping + (size_t(i) << page_size_log2());
        }
      }
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()), upload_regions_.data());
    upload_regions_.clear();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_READ_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

}  // namespace rex::graphics::vulkan
