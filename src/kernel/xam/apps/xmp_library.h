/**
 * @file        kernel/xam/apps/xmp_library.h
 * @brief       User playlists from a folder for the XMP app.
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace rex::kernel::xam::apps {

struct XmpLibrarySong {
  std::filesystem::path path;  // empty for a test tone
  std::u16string title;        // from the file name until the file's tags are read
  double tone_hz = 0.0;
  uint32_t tone_ms = 0;
};

struct XmpLibraryPlaylist {
  std::u16string title;  // unique, at most 39 characters (titles match playlists by title)
  std::vector<XmpLibrarySong> songs;
};

struct XmpLibrary {
  std::vector<XmpLibraryPlaylist> playlists;
  size_t songs = 0;
  size_t skipped = 0;  // files with extensions no decoder takes
};

// Each subfolder of `folder` is a playlist named after it, with every song file
// below it (sorted by path); song files directly in `folder` make a playlist
// "<folder name> Mix". Hidden files and folders are skipped. A missing folder
// gives an empty library.
XmpLibrary ScanXmpLibrary(const std::filesystem::path& folder);

// Makes `title` fit the title's 40-character field and differ from `taken`.
std::u16string UniqueXmpTitle(std::u16string title, const std::vector<std::u16string>& taken);

}  // namespace rex::kernel::xam::apps
