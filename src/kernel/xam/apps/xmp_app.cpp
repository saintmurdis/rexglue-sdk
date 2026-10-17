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
#include <cstring>

#include <rex/audio/pre_frame_hook.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

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
  const size_t n = std::min(text.size(), units - 1);
  for (size_t i = 0; i < units; ++i) {
    memory::store_and_swap<uint16_t>(dst + i * 2, i < n ? uint16_t(text[i]) : uint16_t(0));
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

}  // namespace

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
      next_song_handle_(1) {
  if (REXCVAR_GET(xmp_test_tone)) {
    UserPlaylist tone;
    tone.title = u"Test Tone";
    tone.songs.push_back({u"Tone 440 Hz", u"rexglue", u"Test Tone", 440.0, 120000});
    tone.songs.push_back({u"Tone 660 Hz", u"rexglue", u"Test Tone", 660.0, 120000});
    user_playlists_.push_back(std::move(tone));
  }
  size_t songs = 0;
  for (const auto& playlist : user_playlists_) {
    songs += playlist.songs.size();
  }
  REXKRNL_INFO("XMP: {} user playlists, {} songs", user_playlists_.size(), songs);
  rex::audio::SetPreFrameHook(
      [this](runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state) {
        PumpCapture(dispatcher, thread_state);
      });
}

XmpApp::~XmpApp() { rex::audio::SetPreFrameHook({}); }

std::vector<XmpApp::UserPlaylistRecord> XmpApp::UserPlaylistRecords() const {
  std::vector<UserPlaylistRecord> records;
  for (const auto& playlist : user_playlists_) {
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
  int index = -1;
  for (size_t i = 0; i < user_playlists_.size(); ++i) {
    if (user_playlists_[i].title == title) {
      index = int(i);
      break;
    }
  }
  if (index < 0) {
    REXKRNL_WARN("XMP: play user playlist \"{}\": no such playlist", rex::string::to_utf8(title));
    return X_E_NOTFOUND;
  }
  bool restarted = true;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (index == active_user_playlist_ && state_ != State::kIdle) {
      restarted = false;  // a repeated play doesn't restart the song
    } else {
      active_user_playlist_ = index;
      active_user_song_ = 0;
      song_frame_ = 0;
    }
    active_playlist_ = nullptr;
    // Playing before this returns: the title stops the session if its first
    // status check after the start reads idle.
    state_ = State::kPlaying;
  }
  const auto& playlist = user_playlists_[index];
  REXKRNL_INFO("XMP: {} user playlist \"{}\" ({} songs, {} bytes)",
               restarted ? "playing" : "already playing", rex::string::to_utf8(playlist.title),
               playlist.songs.size(), buffer_length);
  OnStateChanged();
  return X_E_SUCCESS;
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
  UserSong user_song;
  uint32_t handle = 0;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    if (active_user_playlist_ >= 0) {
      const auto& songs = user_playlists_[active_user_playlist_].songs;
      if (active_user_song_ < songs.size()) {
        user_song = songs[active_user_song_];
        handle = uint32_t(active_user_song_ + 1);
      }
    }
  }
  if (handle) {
    std::memset(info, 0, 0x3E0);
    memory::store_and_swap<uint32_t>(info + 0x000, handle);
    StoreString(info + 0x240, user_song.title);
    StoreString(info + 0x290, user_song.artist);
    StoreString(info + 0x2E0, user_song.album);
    memory::store_and_swap<uint32_t>(info + 0x3D0, handle);
    memory::store_and_swap<uint32_t>(info + 0x3D4, user_song.duration_ms);
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
    const size_t count = user_playlists_[active_user_playlist_].songs.size();
    switch (message) {
      case 0x00070003:  // Continue (the title's flow control sends Pause/Continue)
        if (state_ == State::kPaused) {
          state_ = State::kPlaying;
        }
        break;
      case 0x00070004:  // Stop
        REXKRNL_INFO("XMP: user playlist stopped");
        active_user_playlist_ = -1;
        state_ = State::kIdle;
        break;
      case 0x00070005:  // Pause
        if (state_ == State::kPlaying) {
          state_ = State::kPaused;
        }
        break;
      case 0x00070006:  // Next
        active_user_song_ = count ? (active_user_song_ + 1) % count : 0;
        song_frame_ = 0;
        state_ = State::kPlaying;
        break;
      case 0x00070007:  // Previous
        active_user_song_ = count ? (active_user_song_ + count - 1) % count : 0;
        song_frame_ = 0;
        state_ = State::kPlaying;
        break;
      default:
        return false;
    }
    REXKRNL_DEBUG("XMP: user transport {:08X} -> state {}, song {}", message,
                  static_cast<uint32_t>(state_.load()), active_user_song_);
  }
  OnStateChanged();
  return true;
}

bool XmpApp::RenderUserFrames(float* interleaved, uint32_t frames) {
  const auto& songs = user_playlists_[active_user_playlist_].songs;
  uint32_t i = 0;
  while (i < frames) {
    if (active_user_song_ >= songs.size()) {
      break;
    }
    const UserSong& song = songs[active_user_song_];
    const uint64_t total = uint64_t(song.duration_ms) * kSampleRate / 1000;
    if (song_frame_ >= total) {
      // A song ends with its last samples; the next one starts with the next
      // block (the rest of this one stays silent).
      ++active_user_song_;
      song_frame_ = 0;
      if (active_user_song_ >= songs.size()) {
        active_user_song_ = 0;  // repeat the playlist
      }
      break;
    }
    const double phase = 2.0 * 3.14159265358979323846 * song.tone_hz * double(song_frame_) /
                         double(kSampleRate);
    const float v = float(0.25 * std::sin(phase));
    interleaved[i * 2] = v;
    interleaved[i * 2 + 1] = v;
    ++song_frame_;
    ++i;
  }
  for (; i < frames; ++i) {
    interleaved[i * 2] = 0.0f;
    interleaved[i * 2 + 1] = 0.0f;
  }
  return true;
}

void XmpApp::PumpCapture(runtime::FunctionDispatcher* dispatcher,
                         runtime::ThreadState* thread_state) {
  bool song_changed = false;
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    // The title paces the feed itself: it sends Pause when its ring is full
    // enough and Continue when it has drained, so only a playing player feeds.
    if (!capture_callback_ || !capture_title_render_ || !capture_buffer_ ||
        active_user_playlist_ < 0 || state_ != State::kPlaying) {
      return;
    }
    float block[kCaptureFrames * 2];
    const size_t song_before = active_user_song_;
    RenderUserFrames(block, kCaptureFrames);
    song_changed = active_user_song_ != song_before;
    // 48 kHz stereo float, big-endian in guest memory.
    uint8_t* dst = memory_->TranslateVirtual(capture_buffer_);
    for (uint32_t i = 0; i < kCaptureFrames * 2; ++i) {
      memory::store_and_swap<float>(dst + i * 4, block[i]);
    }
    uint64_t args[] = {capture_buffer_, capture_context_, 1};
    dispatcher->Execute(thread_state, capture_callback_, args, 3);
    ++capture_blocks_;
    if (capture_blocks_ == 1 || capture_blocks_ % 11250 == 0) {  // first, then once a minute
      REXKRNL_INFO("XMP: capture output: {} blocks delivered", capture_blocks_);
    }
  }
  if (song_changed) {
    OnStateChanged();  // the title reads the new song's info on this
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
