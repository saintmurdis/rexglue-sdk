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

#include <rex/kernel/xam/apps/xmp_app.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <random>

#include <rex/audio/pre_frame_hook.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include "xmp_decoder.h"
#include "xmp_library.h"
#include "xmp_streamer.h"

REXCVAR_DEFINE_STRING(xmp_music_folder, "", "XMP",
                      "Folder of user playlists for titles' custom music: each subfolder is a "
                      "playlist of the song files below it (MP3, AAC/M4A, ALAC, WAV, AIFF, FLAC); "
                      "~ is the home folder. Empty: none");
REXCVAR_DEFINE_BOOL(xmp_test_tone, false, "XMP",
                    "Offer a user playlist \"Test Tone\" (440 Hz, then 660 Hz, two minutes each) "
                    "for testing the title's custom music through XMPCaptureOutput");

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kCaptureFrames = 256;  // one title audio frame
constexpr uint32_t kCaptureBytes = kCaptureFrames * 2 * sizeof(float);
constexpr size_t kMaxMetadataChars = 40;  // XMP_MAX_METADATA_STRING, NUL included

// Writes a big-endian UTF-16 string, cut to fit `units` code units with its NUL.
void StoreString(uint8_t* dst, const std::u16string& text, size_t units = kMaxMetadataChars) {
  const std::u16string cut = TruncateXmpText(text, units - 1);  // never half a surrogate pair
  for (size_t i = 0; i < units; ++i) {
    memory::store_and_swap<uint16_t>(dst + i * 2, i < cut.size() ? uint16_t(cut[i]) : uint16_t(0));
  }
}

// Reads a big-endian UTF-16 string of at most `units` code units.
std::u16string LoadString(const uint8_t* src, size_t units = kMaxMetadataChars) {
  std::u16string text;
  for (size_t i = 0; i < units; ++i) {
    const char16_t c = char16_t(memory::load_and_swap<uint16_t>(src + i * 2));
    if (!c) {
      break;
    }
    text.push_back(c);
  }
  return text;
}

uint64_t Fnv1a(const std::u16string& text) {
  uint64_t hash = 1469598103934665603ull;
  for (char16_t c : text) {
    hash = (hash ^ uint8_t(c)) * 1099511628211ull;
    hash = (hash ^ uint8_t(c >> 8)) * 1099511628211ull;
  }
  return hash;
}

std::filesystem::path ExpandHome(const std::string& text) {
  if (text == "~" || text.rfind("~/", 0) == 0) {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "") / text.substr(text.size() > 1 ? 2 : 1);
  }
  return text;
}

std::string MinutesSeconds(uint32_t ms) {
  const uint32_t s = ms / 1000;
  return std::to_string(s / 60) + ":" + (s % 60 < 10 ? "0" : "") + std::to_string(s % 60);
}

}  // namespace

struct XmpApp::UserMusic {
  std::filesystem::path folder;
  XmpLibrary library;
  XmpStreamer streamer;
};

XmpApp::XmpApp(KernelState* kernel_state)
    : App(kernel_state, 0xFA),
      state_(State::kIdle),
      playback_client_(PlaybackClient::kTitle),
      playback_mode_(PlaybackMode::kUnknown),
      repeat_mode_(RepeatMode::kUnknown),
      unknown_flags_(0),
      volume_(1.0f),
      active_playlist_(nullptr),
      active_song_index_(0),
      next_playlist_handle_(1),
      next_song_handle_(1),
      user_(std::make_unique<UserMusic>()) {
  // Scanned once: titles ask for the playlist count once per session (SSX caches it).
  const std::string folder_setting = REXCVAR_GET(xmp_music_folder);
  const std::filesystem::path folder = ExpandHome(folder_setting);
  user_->folder = folder;
  user_->library = ScanXmpLibrary(folder);
  if (REXCVAR_GET(xmp_test_tone)) {
    std::vector<std::u16string> taken;
    for (const auto& playlist : user_->library.playlists) {
      taken.push_back(playlist.title);
    }
    XmpLibraryPlaylist tone;
    tone.title = UniqueXmpTitle(u"Test Tone", taken);
    tone.songs.push_back({{}, u"Tone 440 Hz", 440.0, 120000});
    tone.songs.push_back({{}, u"Tone 660 Hz", 660.0, 120000});
    user_->library.songs += tone.songs.size();
    user_->library.playlists.push_back(std::move(tone));
  }
  if (!folder_setting.empty() || REXCVAR_GET(xmp_test_tone)) {
    REXKRNL_INFO("XMP: {} user playlists, {} songs{} (music folder {}{})",
                 user_->library.playlists.size(), user_->library.songs,
                 user_->library.skipped
                     ? ", " + std::to_string(user_->library.skipped) + " files not songs"
                     : std::string(),
                 rex::path_to_utf8(folder),
                 folder_setting.empty() || std::filesystem::is_directory(folder) ? ""
                                                                                 : ", missing");
    for (const auto& playlist : user_->library.playlists) {
      REXKRNL_INFO("XMP:   \"{}\": {} songs", rex::string::to_utf8(playlist.title),
                   playlist.songs.size());
    }
  }
  rex::audio::SetPreFrameHook(
      [this](runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state) {
        PumpCapture(dispatcher, thread_state);
      });
}

XmpApp::~XmpApp() {
  rex::audio::SetPreFrameHook({});  // before the streamer and its thread go
  user_.reset();
}

std::vector<XmpApp::UserPlaylistRecord> XmpApp::UserPlaylistRecords() const {
  std::vector<UserPlaylistRecord> records;
  for (const auto& playlist : user_->library.playlists) {
    UserPlaylistRecord record{};
    // 572-byte ID, opaque to the title: a tag and a hash of the title, so it
    // stays the same between runs.
    std::memcpy(record.data(), "SSXP", 4);
    memory::store_and_swap<uint64_t>(record.data() + 4, Fnv1a(playlist.title));
    StoreString(record.data() + 0x23C, playlist.title);
    records.push_back(record);
  }
  return records;
}

X_HRESULT XmpApp::XMPGetStatus(uint32_t state_ptr) {
  if (!XThread::GetCurrentThread()->main_thread()) {
    // Some stupid games will hammer this on a thread - induce a delay
    // here to keep from starving real threads.
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }

  REXKRNL_TRACE("XMPGetStatus({:08X})", state_ptr);
  memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(state_ptr),
                                   static_cast<uint32_t>(state_.load()));
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                         uint32_t playlist_name_ptr,
                                         const std::u16string& playlist_name, uint32_t flags,
                                         uint32_t out_song_handles, uint32_t out_playlist_handle) {
  REXKRNL_DEBUG(
      "XMPCreateTitlePlaylist({:08X}, {:08X}, {:08X}({}), {:08X}, {:08X}, "
      "{:08X})",
      songs_ptr, song_count, playlist_name_ptr, rex::string::to_utf8(playlist_name), flags,
      out_song_handles, out_playlist_handle);
  auto playlist = std::make_unique<Playlist>();
  playlist->handle = ++next_playlist_handle_;
  playlist->name = playlist_name;
  playlist->flags = flags;
  if (songs_ptr) {
    for (uint32_t i = 0; i < song_count; ++i) {
      auto song = std::make_unique<Song>();
      song->handle = ++next_song_handle_;
      uint8_t* song_base = memory_->TranslateVirtual(songs_ptr + (i * 36));
      song->file_path = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 0)));
      song->name = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 4)));
      song->artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 8)));
      song->album = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 12)));
      song->album_artist = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 16)));
      song->genre = memory::load_and_swap<std::u16string>(
          memory_->TranslateVirtual(memory::load_and_swap<uint32_t>(song_base + 20)));
      song->track_number = memory::load_and_swap<uint32_t>(song_base + 24);
      song->duration_ms = memory::load_and_swap<uint32_t>(song_base + 28);
      song->format = static_cast<Song::Format>(memory::load_and_swap<uint32_t>(song_base + 32));
      if (out_song_handles) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_song_handles + (i * 4)),
                                         song->handle);
      }
      playlist->songs.emplace_back(std::move(song));
    }
  }
  if (out_playlist_handle) {
    memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(out_playlist_handle),
                                     playlist->handle);
  }

  auto global_lock = global_critical_region_.Acquire();
  playlists_.insert({playlist->handle, playlist.get()});
  playlist.release();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPDeleteTitlePlaylist(uint32_t playlist_handle) {
  REXKRNL_DEBUG("XMPDeleteTitlePlaylist({:08X})", playlist_handle);
  auto global_lock = global_critical_region_.Acquire();
  auto it = playlists_.find(playlist_handle);
  if (it == playlists_.end()) {
    REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
    return X_E_NOTFOUND;
  }
  auto playlist = it->second;
  if (playlist == active_playlist_) {
    XMPStop(0);
  }
  playlists_.erase(it);
  delete playlist;
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle) {
  REXKRNL_DEBUG("XMPPlayTitlePlaylist({:08X}, {:08X})", playlist_handle, song_handle);
  Playlist* playlist = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = playlists_.find(playlist_handle);
    if (it == playlists_.end()) {
      REXKRNL_ERROR("Playlist {:08X} not found", playlist_handle);
      return X_E_NOTFOUND;
    }
    playlist = it->second;
  }

  if (playback_client_ == PlaybackClient::kSystem) {
    REXKRNL_WARN("XMPPlayTitlePlaylist: System playback is enabled!");
    return X_E_SUCCESS;
  }

  // Start playlist?
  REXKRNL_WARN("Playlist playback not supported");
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    active_user_playlist_ = -1;
  }
  active_playlist_ = playlist;
  active_song_index_ = 0;
  state_ = State::kPlaying;
  OnStateChanged();
  kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 1);
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPContinue() {
  REXKRNL_DEBUG("XMPContinue()");
  if (state_ == State::kPaused) {
    state_ = State::kPlaying;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPStop(uint32_t unk) {
  assert_zero(unk);
  REXKRNL_DEBUG("XMPStop({:08X})", unk);
  active_playlist_ = nullptr;  // ?
  active_song_index_ = 0;
  state_ = State::kIdle;
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPause() {
  REXKRNL_DEBUG("XMPPause()");
  if (state_ == State::kPlaying) {
    state_ = State::kPaused;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPNext() {
  REXKRNL_DEBUG("XMPNext()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  active_song_index_ = (active_song_index_ + 1) % active_playlist_->songs.size();
  OnStateChanged();
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPrevious() {
  REXKRNL_DEBUG("XMPPrevious()");
  if (!active_playlist_) {
    return X_E_NOTFOUND;
  }
  state_ = State::kPlaying;
  if (!active_song_index_) {
    active_song_index_ = static_cast<int>(active_playlist_->songs.size()) - 1;
  } else {
    --active_song_index_;
  }
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::OnStateChanged() {
  kernel_state_->BroadcastNotification(kMsgStateChanged, static_cast<uint32_t>(state_.load()));
}

X_HRESULT XmpApp::XMPCreateUserPlaylistEnumerator(uint32_t buffer_ptr) {
  // {client, flags, private enumerator structure}; buffer_length is 0. The
  // records were put in the enumerator by XamCreateEnumeratorHandle.
  struct {
    rex::be<uint32_t> xmp_client;
    rex::be<uint32_t> flags;
    rex::be<uint32_t> private_enum_structure_ptr;
  }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
  REXKRNL_DEBUG("XMPCreateUserPlaylistEnumerator({:08X}, {:08X}, {:08X})",
                uint32_t(args->xmp_client), uint32_t(args->flags),
                uint32_t(args->private_enum_structure_ptr));
  if (!args->private_enum_structure_ptr) {
    return X_E_INVALIDARG;
  }
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPPlayUserPlaylist(uint32_t buffer_ptr, uint32_t buffer_length) {
  // {client, XMP_USER_PLAYLIST_INFO}: 824 bytes. Matched by title, as the title
  // itself does.
  const uint8_t* record = memory_->TranslateVirtual(buffer_ptr) + 4;
  const std::u16string title = LoadString(record + 0x23C);
  const auto& playlists = user_->library.playlists;
  int index = -1;
  for (size_t i = 0; i < playlists.size(); ++i) {
    if (playlists[i].title == title) {
      index = int(i);
      break;
    }
  }
  if (index < 0) {
    REXKRNL_WARN("XMP: play user playlist \"{}\": no such playlist", rex::string::to_utf8(title));
    return X_E_NOTFOUND;
  }
  const uint32_t mode = static_cast<uint32_t>(playback_mode_);
  const uint32_t repeat = static_cast<uint32_t>(repeat_mode_);
  bool restarted = true;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (index == active_user_playlist_ && state_ != State::kIdle) {
      restarted = false;  // a repeated play doesn't restart the song
    } else {
      active_user_playlist_ = index;
      // XDK playback behaviour: mode 1 shuffles; repeat mode 1 plays the
      // playlist once, 0 repeats it.
      user_order_.resize(playlists[index].songs.size());
      std::iota(user_order_.begin(), user_order_.end(), size_t(0));
      if (mode == 1) {
        std::shuffle(user_order_.begin(), user_order_.end(), std::mt19937(std::random_device{}()));
      }
      user_repeat_ = repeat != 1;
      StartUserPlayback(0);
    }
    active_playlist_ = nullptr;
    // Playing before this returns: the title stops the session if its first
    // status check after the start reads idle.
    state_ = State::kPlaying;
  }
  REXKRNL_INFO("XMP: {} user playlist \"{}\" ({} songs, {}, {}; {} bytes)",
               restarted ? "playing" : "already playing", rex::string::to_utf8(title),
               playlists[index].songs.size(), mode == 1 ? "shuffle" : "in order",
               repeat != 1 ? "repeat" : "once", buffer_length);
  OnStateChanged();
  return X_E_SUCCESS;
}

void XmpApp::StartUserPlayback(size_t position) {
  const auto& songs = user_->library.playlists[active_user_playlist_].songs;
  user_->streamer.Start(
      [&songs](size_t song) -> std::unique_ptr<XmpSongDecoder> {
        if (song >= songs.size()) {
          return nullptr;
        }
        const XmpLibrarySong& entry = songs[song];
        if (entry.path.empty()) {
          return OpenXmpTestTone(entry.tone_hz, entry.tone_ms, entry.title);
        }
        std::string error;
        auto decoder = OpenXmpSongFile(entry.path, &error);
        if (!decoder) {
          REXKRNL_WARN("XMP: skipping {}: {}", rex::path_to_utf8(entry.path), error);
        }
        return decoder;
      },
      user_order_, position, user_repeat_);
  log_now_playing_ = true;
}

XmpApp::UserLibraryStatus XmpApp::GetUserLibraryStatus() {
  UserLibraryStatus status;
  if (!REXCVAR_GET(xmp_music_folder).empty()) {
    status.folder = rex::path_to_utf8(user_->folder);
  }
  for (const auto& playlist : user_->library.playlists) {
    status.playlists.emplace_back(rex::string::to_utf8(playlist.title), playlist.songs.size());
  }
  status.songs = user_->library.songs;
  status.skipped = user_->library.skipped;
  std::lock_guard<std::mutex> lock(capture_mutex_);
  size_t song = 0;
  XmpSongInfo info;
  if (active_user_playlist_ >= 0 && user_->streamer.Current(nullptr, &song, &info)) {
    const auto& playlist = user_->library.playlists[active_user_playlist_];
    status.playlist = rex::string::to_utf8(playlist.title);
    status.song = rex::string::to_utf8(info.title.empty() ? playlist.songs[song].title : info.title);
    if (!info.artist.empty()) {
      status.song += " - " + rex::string::to_utf8(info.artist);
    }
  }
  return status;
}

bool XmpApp::LogNowPlaying() {
  size_t position = 0;
  size_t song = 0;
  XmpSongInfo info;
  if (active_user_playlist_ < 0 || !user_->streamer.Current(&position, &song, &info)) {
    return true;
  }
  if (info.format.empty()) {
    return false;  // the decode thread hasn't opened the song yet
  }
  const auto& playlist = user_->library.playlists[active_user_playlist_];
  const std::u16string& title = info.title.empty() ? playlist.songs[song].title : info.title;
  REXKRNL_INFO("XMP: now playing \"{}\" {}/{}: \"{}\"{} ({}, {:.1f} kHz, {})",
               rex::string::to_utf8(playlist.title), position + 1, user_order_.size(),
               rex::string::to_utf8(title),
               info.artist.empty() ? std::string() : " by " + rex::string::to_utf8(info.artist),
               info.format, info.source_rate / 1000.0, MinutesSeconds(info.duration_ms));
  return true;
}

X_HRESULT XmpApp::XMPCaptureOutput(uint32_t buffer_ptr) {
  struct {
    rex::be<uint32_t> xmp_client;
    rex::be<uint32_t> callback;
    rex::be<uint32_t> context;
    rex::be<uint32_t> title_render;
  }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
  static_assert_size(decltype(*args), 16);
  // Waits for a callback in progress before changing anything.
  std::lock_guard<std::mutex> lock(capture_mutex_);
  const uint32_t callback = args->callback;
  if (callback && !capture_buffer_) {
    capture_buffer_ = memory_->SystemHeapAlloc(kCaptureBytes);
  }
  if (!callback && capture_callback_) {
    REXKRNL_INFO("XMP: capture output off after {} blocks", capture_blocks_);
  } else if (callback && callback != capture_callback_) {
    REXKRNL_INFO("XMP: capture output on: callback {:08X}, context {:08X}, title rendering {}",
                 callback, uint32_t(args->context), uint32_t(args->title_render));
  }
  capture_callback_ = callback;
  capture_context_ = args->context;
  capture_title_render_ = args->title_render != 0;
  capture_blocks_ = callback ? capture_blocks_ : 0;
  return X_E_SUCCESS;
}

X_HRESULT XmpApp::XMPGetCurrentSong(uint32_t info_ptr) {
  // XMP_SONGINFO, 0x3E0 bytes: handle, a 572-byte media ID, then title,
  // artist, album, album artist and genre at 80-byte (40-character) spacing,
  // track number, duration (ms), format.
  uint8_t* info = memory_->TranslateVirtual(info_ptr);
  XmpSongInfo song_info;
  std::u16string fallback_title;
  size_t position = 0;
  size_t song_index = 0;
  bool user_song = false;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (active_user_playlist_ >= 0 && user_->streamer.Current(&position, &song_index, &song_info)) {
      fallback_title = user_->library.playlists[active_user_playlist_].songs[song_index].title;
      user_song = true;
    }
  }
  if (user_song) {
    std::memset(info, 0, 0x3E0);
    memory::store_and_swap<uint32_t>(info + 0x000, uint32_t(song_index + 1));
    StoreString(info + 0x240, song_info.title.empty() ? fallback_title : song_info.title);
    StoreString(info + 0x290, song_info.artist);
    StoreString(info + 0x2E0, song_info.album);
    StoreString(info + 0x380, song_info.genre);
    memory::store_and_swap<uint32_t>(info + 0x3D0,
                                     song_info.track_number ? song_info.track_number
                                                            : uint32_t(position + 1));
    memory::store_and_swap<uint32_t>(info + 0x3D4, song_info.duration_ms);
    memory::store_and_swap<uint32_t>(info + 0x3D8, uint32_t(Song::Format::kMp3));
    return X_E_SUCCESS;
  }
  if (!active_playlist_ || active_playlist_->songs.empty()) {
    return X_E_FAIL;
  }
  const auto& song = active_playlist_->songs[active_song_index_];
  std::memset(info, 0, 0x3E0);
  memory::store_and_swap<uint32_t>(info + 0x000, song->handle);
  StoreString(info + 0x240, song->name);
  StoreString(info + 0x290, song->artist);
  StoreString(info + 0x2E0, song->album);
  StoreString(info + 0x330, song->album_artist);
  StoreString(info + 0x380, song->genre);
  memory::store_and_swap<uint32_t>(info + 0x3D0, song->track_number);
  memory::store_and_swap<uint32_t>(info + 0x3D4, song->duration_ms);
  memory::store_and_swap<uint32_t>(info + 0x3D8, static_cast<uint32_t>(song->format));
  return X_E_SUCCESS;
}

bool XmpApp::UserTransport(uint32_t message) {
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (active_user_playlist_ < 0) {
      return false;
    }
    const size_t count = user_order_.size();
    size_t position = 0;
    user_->streamer.Current(&position, nullptr, nullptr);
    switch (message) {
      case 0x00070003:  // Continue (the title's flow control sends Pause/Continue)
        if (state_ == State::kPaused) {
          state_ = State::kPlaying;
        }
        break;
      case 0x00070004:  // Stop
        REXKRNL_INFO("XMP: user playlist stopped");
        user_->streamer.Stop();
        active_user_playlist_ = -1;
        state_ = State::kIdle;
        break;
      case 0x00070005:  // Pause
        if (state_ == State::kPlaying) {
          state_ = State::kPaused;
        }
        break;
      case 0x00070006:  // Next
        StartUserPlayback(count ? (position + 1) % count : 0);
        state_ = State::kPlaying;
        break;
      case 0x00070007:  // Previous
        StartUserPlayback(count ? (position + count - 1) % count : 0);
        state_ = State::kPlaying;
        break;
      default:
        return false;
    }
    REXKRNL_DEBUG("XMP: user transport {:08X} -> state {}", message,
                  static_cast<uint32_t>(state_.load()));
  }
  OnStateChanged();
  return true;
}
void XmpApp::PumpCapture(runtime::FunctionDispatcher* dispatcher,
                         runtime::ThreadState* thread_state) {
  bool notify = false;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    // The title paces the feed itself: it sends Pause when its ring is full
    // enough and Continue when it has drained, so only a playing player feeds.
    if (!capture_callback_ || !capture_title_render_ || !capture_buffer_ ||
        active_user_playlist_ < 0 || state_ != State::kPlaying) {
      return;
    }
    float block[kCaptureFrames * 2];
    const XmpStreamer::ReadResult read = user_->streamer.Read(block, kCaptureFrames);
    // 48 kHz stereo float, big-endian in guest memory.
    uint8_t* dst = memory_->TranslateVirtual(capture_buffer_);
    for (uint32_t i = 0; i < kCaptureFrames * 2; ++i) {
      memory::store_and_swap<float>(dst + i * 4, block[i]);
    }
    uint64_t args[] = {capture_buffer_, capture_context_, 1};
    dispatcher->Execute(thread_state, capture_callback_, args, 3);
    ++capture_blocks_;
    if (capture_blocks_ % 11250 == 0) {  // once a minute
      REXKRNL_INFO("XMP: capture output: {} blocks delivered, {} short", capture_blocks_,
                   user_->streamer.underrun_blocks());
    }
    if (read.ended) {
      REXKRNL_INFO("XMP: user playlist finished");
      active_user_playlist_ = -1;
      state_ = State::kIdle;
      notify = true;
    } else if (read.song_changed || log_now_playing_) {
      // A new song: the title reads its info (title, length) on the
      // notification, so send it once the decode thread has opened the file;
      // until then the info has only the file name.
      log_now_playing_ = !LogNowPlaying();
      notify = read.song_changed || !log_now_playing_;
    }
  }
  if (notify) {
    OnStateChanged();
  }
}
X_HRESULT XmpApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  if (message >= 0x00070003 && message <= 0x00070007 && UserTransport(message)) {
    return X_E_SUCCESS;
  }
  switch (message) {
    case 0x00070025:
      return XMPCreateUserPlaylistEnumerator(buffer_ptr);
    case 0x0007003B:
      return XMPPlayUserPlaylist(buffer_ptr, buffer_length);
    case 0x00070002: {
      assert_true(!buffer_length || buffer_length == 12);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t storage_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t song_handle = memory::load_and_swap<uint32_t>(buffer + 8);  // 0?
      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(storage_ptr));
      assert_true(xmp_client == 0x00000002);
      return XMPPlayTitlePlaylist(playlist_handle, song_handle);
    }
    case 0x00070003: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPContinue();
    }
    case 0x00070004: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t unk = memory::load_and_swap<uint32_t>(buffer + 4);
      assert_true(xmp_client == 0x00000002);
      return XMPStop(unk);
    }
    case 0x00070005: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPause();
    }
    case 0x00070006: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPNext();
    }
    case 0x00070007: {
      assert_true(!buffer_length || buffer_length == 4);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      assert_true(xmp_client == 0x00000002);
      return XMPPrevious();
    }
    case 0x00070008: {
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode;
        rex::be<uint32_t> repeat_mode;
        rex::be<uint32_t> flags;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPSetPlaybackBehavior({:08X}, {:08X}, {:08X})", uint32_t(args->playback_mode),
                    uint32_t(args->repeat_mode), uint32_t(args->flags));
      playback_mode_ = static_cast<PlaybackMode>(uint32_t(args->playback_mode));
      repeat_mode_ = static_cast<RepeatMode>(uint32_t(args->repeat_mode));
      unknown_flags_ = args->flags;
      kernel_state_->BroadcastNotification(kMsgPlaybackBehaviorChanged, 0);
      return X_E_SUCCESS;
    }
    case 0x00070009: {
      assert_true(!buffer_length || buffer_length == 8);
      uint32_t xmp_client = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t state_ptr = memory::load_and_swap<uint32_t>(buffer + 4);  // out ptr to 4b - expect 0
      assert_true(xmp_client == 0x00000002);
      return XMPGetStatus(state_ptr);
    }
    case 0x0007000B: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> volume_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetVolume({:08X})", uint32_t(args->volume_ptr));
      memory::store_and_swap<float>(memory_->TranslateVirtual(args->volume_ptr), volume_);
      return X_E_SUCCESS;
    }
    case 0x0007000C: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<float> value;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPSetVolume({:g})", float(args->value));
      volume_ = args->value;
      return X_E_SUCCESS;
    }
    case 0x0007000D: {
      assert_true(!buffer_length || buffer_length == 36);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
        rex::be<uint32_t> storage_size;
        rex::be<uint32_t> songs_ptr;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> playlist_name_ptr;
        rex::be<uint32_t> flags;
        rex::be<uint32_t> song_handles_ptr;
        rex::be<uint32_t> playlist_handle_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 36);

      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playlist_handle_ptr),
                                       args->storage_ptr);
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      std::u16string playlist_name;
      if (!args->playlist_name_ptr) {
        playlist_name = u"";
      } else {
        playlist_name = memory::load_and_swap<std::u16string>(
            memory_->TranslateVirtual(args->playlist_name_ptr));
      }
      // dummy_alloc_ptr is the result of a XamAlloc of storage_size.
      assert_true(uint32_t(args->storage_size) == 4 + uint32_t(args->song_count) * 128);
      return XMPCreateTitlePlaylist(args->songs_ptr, args->song_count, args->playlist_name_ptr,
                                    playlist_name, args->flags, args->song_handles_ptr,
                                    args->storage_ptr);
    }
    case 0x0007000E: {
      // XMPGetCurrentSong
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> unk_ptr;
        rex::be<uint32_t> info_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);
      REXKRNL_DEBUG("XMPGetCurrentSong({:08X}, {:08X})", uint32_t(args->unk_ptr),
                    uint32_t(args->info_ptr));
      if (!args->info_ptr) {
        return X_E_INVALIDARG;
      }
      return XMPGetCurrentSong(args->info_ptr);
    }
    case 0x00070013: {
      assert_true(!buffer_length || buffer_length == 8);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> storage_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 8);

      uint32_t playlist_handle =
          memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(args->storage_ptr));
      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      return XMPDeleteTitlePlaylist(playlist_handle);
    }
    case 0x0007001A: {
      // XMPSetPlaybackController: {client, controller request (0 game, 1 user,
      // 4 restore), locked}.
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller;
        rex::be<uint32_t> locked;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      REXKRNL_DEBUG("XMPSetPlaybackController({:08X}, {:08X}, {:08X})", uint32_t(args->xmp_client),
                    uint32_t(args->controller), uint32_t(args->locked));
      const uint32_t controller = args->controller;
      playback_client_ =
          controller == 0 || controller == 4 ? PlaybackClient::kTitle : PlaybackClient::kSystem;
      kernel_state_->BroadcastNotification(kMsgPlaybackControllerChanged,
                                           playback_client_ == PlaybackClient::kTitle ? 1 : 0);
      return X_E_SUCCESS;
    }
    case 0x0007001B: {
      // XMPGetPlaybackController
      assert_true(!buffer_length || buffer_length == 12);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> controller_ptr;
        rex::be<uint32_t> locked_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002);
      REXKRNL_DEBUG("XMPGetPlaybackController({:08X}, {:08X}, {:08X})", uint32_t(args->xmp_client),
                    uint32_t(args->controller_ptr), uint32_t(args->locked_ptr));
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->controller_ptr), 0);
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->locked_ptr), 0);

      if (!XThread::GetCurrentThread()->main_thread()) {
        // Atrain spawns a thread 82437FD0 to call this in a tight loop forever.
        rex::thread::Sleep(std::chrono::milliseconds(10));
      }

      return X_E_SUCCESS;
    }
    case 0x00070029: {
      // XMPGetPlaybackBehavior
      assert_true(!buffer_length || buffer_length == 16);
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> playback_mode_ptr;
        rex::be<uint32_t> repeat_mode_ptr;
        rex::be<uint32_t> unk3_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 16);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      REXKRNL_DEBUG("XMPGetPlaybackBehavior({:08X}, {:08X}, {:08X})",
                    uint32_t(args->playback_mode_ptr), uint32_t(args->repeat_mode_ptr),
                    uint32_t(args->unk3_ptr));
      if (args->playback_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->playback_mode_ptr),
                                         static_cast<uint32_t>(playback_mode_));
      }
      if (args->repeat_mode_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->repeat_mode_ptr),
                                         static_cast<uint32_t>(repeat_mode_));
      }
      if (args->unk3_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->unk3_ptr), unknown_flags_);
      }
      return X_E_SUCCESS;
    }
    case 0x0007002E: {
      assert_true(!buffer_length || buffer_length == 12);
      // Query of size for XamAlloc - the result of the alloc is passed to
      // 0x0007000D.
      struct {
        rex::be<uint32_t> xmp_client;
        rex::be<uint32_t> song_count;
        rex::be<uint32_t> size_ptr;
      }* args = memory_->TranslateVirtual<decltype(args)>(buffer_ptr);
      static_assert_size(decltype(*args), 12);

      assert_true(args->xmp_client == 0x00000002 || args->xmp_client == 0x00000000);
      // We don't use the storage, so just fudge the number.
      memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(args->size_ptr),
                                       4 + uint32_t(args->song_count) * 128);
      return X_E_SUCCESS;
    }
    case 0x0007003D:
      return XMPCaptureOutput(buffer_ptr);
  }
  REXKRNL_ERROR(
      "Unimplemented XMP message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
