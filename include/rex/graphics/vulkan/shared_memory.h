#pragma once
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
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/vulkan/upload_buffer_pool.h>

namespace rex::graphics::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, memory::Memory& memory,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {});

  VkBuffer buffer() const { return buffer_; }

  // Whether guest data is written to the buffer directly by the CPU when no GPU
  // work may be accessing the pages (vulkan_shared_memory_direct_upload).
  bool direct_upload_enabled() const { return direct_upload_mapping_ != nullptr; }
  // For direct uploads, records that the GPU may read the range in the current
  // submission without it having been requested in it (such as vertex buffers
  // whose residency is cached by the command processor).
  void MarkRangeUsedByCurrentSubmission(uint32_t start, uint32_t length);

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

  void RangesRequested(const std::vector<std::pair<uint32_t, uint32_t>>& ranges) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  // Copies through the upload buffer pool with GPU copy commands, which end the
  // render pass.
  bool UploadRangesWithCopy(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges);
  // Writes the pages that no submitted or recorded GPU work may access directly
  // to the mapped buffer, and puts the rest in direct_upload_copy_page_ranges_.
  void UploadRangesDirectly(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges);
  void MarkPagesForCurrentSubmission(std::vector<uint64_t>& page_submissions, uint32_t start,
                                     uint32_t length);

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // Direct uploads, only when the buffer is in host-visible, host-coherent,
  // device-local memory (on unified memory, such as Apple GPUs).
  uint8_t* direct_upload_mapping_ = nullptr;
  // Per page, the last submission in which the GPU may access it (read or
  // write), and the last one in which GPU work other than a copy upload (a
  // resolve or memory export) may write to it.
  std::vector<uint64_t> page_gpu_use_submissions_;
  std::vector<uint64_t> page_gpu_write_submissions_;
  // Per page, the last submission that copies an upload to it, and the source
  // of that copy in the upload buffer pool, which is kept until the submission
  // is completed.
  std::vector<uint64_t> page_copy_submissions_;
  std::vector<const uint8_t*> page_copy_sources_;
  std::vector<std::pair<uint32_t, uint32_t>> direct_upload_copy_page_ranges_;
  // Statistics logged every 5 seconds while direct uploads are enabled.
  uint64_t direct_upload_stats_time_ms_ = 0;
  uint32_t direct_upload_stats_calls_ = 0;
  uint32_t direct_upload_stats_copy_calls_ = 0;
  uint32_t direct_upload_stats_pages_written_ = 0;
  uint32_t direct_upload_stats_pages_unchanged_ = 0;
  uint32_t direct_upload_stats_pages_copied_ = 0;
};

}  // namespace rex::graphics::vulkan
