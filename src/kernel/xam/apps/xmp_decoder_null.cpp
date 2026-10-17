/**
 * @file        kernel/xam/apps/xmp_decoder_null.cpp
 * @brief       No song decoder on this platform (user playlists list but stay silent).
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "xmp_decoder.h"

namespace rex::kernel::xam::apps {

std::unique_ptr<XmpSongDecoder> OpenXmpSongFile(const std::filesystem::path&, std::string* error) {
  if (error) {
    *error = "no song decoder on this platform";
  }
  return nullptr;
}

}  // namespace rex::kernel::xam::apps
