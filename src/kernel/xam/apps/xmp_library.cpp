/**
 * @file        kernel/xam/apps/xmp_library.cpp
 * @brief       User playlists from a folder for the XMP app.
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#include "xmp_library.h"

#include <algorithm>
#include <set>
#include <string>
#include <system_error>

#include "xmp_decoder.h"

namespace rex::kernel::xam::apps {
namespace fs = std::filesystem;
namespace {

constexpr size_t kMaxTitle = 39;  // 40 UTF-16 units with the NUL

// UTF-16 rather than string(): that converts to the ANSI code page on Windows
// and throws for names outside it.
bool Hidden(const fs::path& path) {
  const std::u16string name = path.filename().u16string();
  return name.empty() || name[0] == u'.';
}

std::u16string Lower(std::u16string text) {
  for (auto& c : text) {
    if (c >= u'A' && c <= u'Z') {
      c = char16_t(c - u'A' + u'a');
    }
  }
  return text;
}

// Song files in `dir` (and every folder below it when `recursive`), sorted by
// their path below `dir`. Folders reached twice (a link back up the tree) are
// read once; a folder that can't be read is skipped, not the whole playlist.
std::vector<XmpLibrarySong> Songs(const fs::path& dir, bool recursive, size_t* skipped) {
  std::vector<std::pair<std::u16string, fs::path>> found;
  std::vector<fs::path> pending{dir};
  std::set<fs::path> visited;
  while (!pending.empty()) {
    const fs::path current = std::move(pending.back());
    pending.pop_back();
    std::error_code ec;
    const fs::path real = fs::weakly_canonical(current, ec);
    if (!visited.insert(ec ? current : real).second) {
      continue;
    }
    for (fs::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec)) {
      const fs::path& path = it->path();
      if (Hidden(path)) {
        continue;
      }
      std::error_code e;
      if (it->is_directory(e)) {
        if (recursive) {
          pending.push_back(path);
        }
      } else if (it->is_regular_file(e)) {
        if (IsXmpSongFile(path)) {
          found.emplace_back(Lower(path.lexically_relative(dir).u16string()), path);
        } else {
          ++*skipped;
        }
      }
    }
  }
  std::sort(found.begin(), found.end());
  std::vector<XmpLibrarySong> songs;
  for (auto& [key, path] : found) {
    XmpLibrarySong song;
    song.title = NormalizeXmpName(path.stem().u16string());
    song.path = std::move(path);
    songs.push_back(std::move(song));
  }
  return songs;
}

}  // namespace

std::u16string UniqueXmpTitle(std::u16string title, const std::vector<std::u16string>& taken) {
  title = TruncateXmpText(std::move(title), kMaxTitle);
  if (title.empty()) {
    title = u"Playlist";
  }
  std::u16string candidate = title;
  for (int n = 2; std::find(taken.begin(), taken.end(), candidate) != taken.end(); ++n) {
    std::u16string suffix = u" (";
    for (char c : std::to_string(n)) {
      suffix.push_back(char16_t(c));
    }
    suffix.push_back(u')');
    candidate = TruncateXmpText(title, kMaxTitle - suffix.size()) + suffix;
  }
  return candidate;
}

XmpLibrary ScanXmpLibrary(const fs::path& folder_setting) {
  XmpLibrary library;
  std::error_code ec;
  // "~/Music/SSX/" names its last folder like "~/Music/SSX".
  fs::path folder = folder_setting.lexically_normal();
  if (folder.has_filename() == false && folder.has_parent_path()) {
    folder = folder.parent_path();
  }
  if (folder.empty() || !fs::is_directory(folder, ec)) {
    return library;
  }
  std::vector<std::pair<std::u16string, fs::path>> dirs;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e;
    if (it->is_directory(e) && !Hidden(it->path())) {
      dirs.emplace_back(Lower(it->path().filename().u16string()), it->path());
    }
  }
  std::sort(dirs.begin(), dirs.end());
  std::vector<std::u16string> taken;
  auto add = [&](std::u16string title, std::vector<XmpLibrarySong> songs) {
    if (songs.empty()) {
      return;
    }
    XmpLibraryPlaylist playlist;
    playlist.title = UniqueXmpTitle(NormalizeXmpName(title), taken);
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
