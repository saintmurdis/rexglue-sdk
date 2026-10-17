/**
 * @file        kernel/xam/apps/xmp_library.cpp
 * @brief       User playlists from a folder for the XMP app.
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "xmp_library.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

#include "xmp_decoder.h"

namespace rex::kernel::xam::apps {
namespace fs = std::filesystem;
namespace {

constexpr size_t kMaxTitle = 39;  // 40 UTF-16 units with the NUL

bool Hidden(const fs::path& path) {
  const std::string name = path.filename().string();
  return name.empty() || name[0] == '.';
}

std::u16string Lower(std::u16string text) {
  for (auto& c : text) {
    if (c < 0x80) {
      c = char16_t(std::towlower(wint_t(c)));
    }
  }
  return text;
}

// Song files below `dir` (or directly in it when `recursive` is false), by path.
std::vector<XmpLibrarySong> Songs(const fs::path& dir, bool recursive, size_t* skipped) {
  std::vector<std::pair<std::u16string, fs::path>> found;
  std::error_code ec;
  auto consider = [&](const fs::directory_entry& entry) {
    std::error_code e;
    if (!entry.is_regular_file(e) || Hidden(entry.path())) {
      return;
    }
    if (IsXmpSongFile(entry.path())) {
      found.emplace_back(Lower(fs::relative(entry.path(), dir, e).u16string()), entry.path());
    } else {
      ++*skipped;
    }
  };
  if (recursive) {
    fs::recursive_directory_iterator it(dir, fs::directory_options::follow_directory_symlink |
                                                 fs::directory_options::skip_permission_denied,
                                        ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (Hidden(it->path())) {
        std::error_code e;
        if (it->is_directory(e)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      consider(*it);
    }
  } else {
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      consider(entry);
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<XmpLibrarySong> songs;
  for (auto& [key, path] : found) {
    XmpLibrarySong song;
    song.title = path.stem().u16string();
    song.path = std::move(path);
    songs.push_back(std::move(song));
  }
  return songs;
}

}  // namespace

std::u16string UniqueXmpTitle(std::u16string title, const std::vector<std::u16string>& taken) {
  if (title.size() > kMaxTitle) {
    title.resize(kMaxTitle);
  }
  if (title.empty()) {
    title = u"Playlist";
  }
  std::u16string candidate = title;
  for (int n = 2; std::find(taken.begin(), taken.end(), candidate) != taken.end(); ++n) {
    const std::u16string suffix = u" (" + fs::path(std::to_string(n)).u16string() + u")";
    candidate = title.substr(0, kMaxTitle - suffix.size()) + suffix;
  }
  return candidate;
}

XmpLibrary ScanXmpLibrary(const fs::path& folder) {
  XmpLibrary library;
  std::error_code ec;
  if (folder.empty() || !fs::is_directory(folder, ec)) {
    return library;
  }
  std::vector<std::pair<std::u16string, fs::path>> dirs;
  for (const auto& entry : fs::directory_iterator(folder, ec)) {
    std::error_code e;
    if (entry.is_directory(e) && !Hidden(entry.path())) {
      dirs.emplace_back(Lower(entry.path().filename().u16string()), entry.path());
    }
  }
  std::sort(dirs.begin(), dirs.end());
  std::vector<std::u16string> taken;
  auto add = [&](std::u16string title, std::vector<XmpLibrarySong> songs) {
    if (songs.empty()) {
      return;
    }
    XmpLibraryPlaylist playlist;
    playlist.title = UniqueXmpTitle(std::move(title), taken);
    taken.push_back(playlist.title);
    library.songs += songs.size();
    playlist.songs = std::move(songs);
    library.playlists.push_back(std::move(playlist));
  };
  for (const auto& [key, dir] : dirs) {
    add(dir.filename().u16string(), Songs(dir, true, &library.skipped));
  }
  add(folder.filename().u16string() + u" Mix", Songs(folder, false, &library.skipped));
  return library;
}

}  // namespace rex::kernel::xam::apps
