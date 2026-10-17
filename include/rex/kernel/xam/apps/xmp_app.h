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

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xam/app_manager.h>
#include <rex/thread/mutex.h>

namespace rex::runtime {
class FunctionDispatcher;
class ThreadState;
}  // namespace rex::runtime

namespace rex {
namespace kernel {
namespace xam {
namespace apps {

// Only source of docs for a lot of these functions:
// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Scenes/Media/Music/ScnMusic.cpp

class XmpApp : public system::xam::App {
 public:
  enum class State : uint32_t {
    kIdle = 0,
    kPlaying = 1,
    kPaused = 2,
  };
  enum class PlaybackClient : uint32_t {
    kSystem = 0,
    kTitle = 1,
  };
  enum class PlaybackMode : uint32_t {
    // kInOrder = ?,
    kUnknown = 0,
  };
  enum class RepeatMode : uint32_t {
    // kNoRepeat = ?,
    kUnknown = 0,
  };
  struct Song {
    enum class Format : uint32_t {
      kWma = 0,
      kMp3 = 1,
    };

    uint32_t handle;
    std::u16string file_path;
    std::u16string name;
    std::u16string artist;
    std::u16string album;
    std::u16string album_artist;
    std::u16string genre;
    uint32_t track_number;
    uint32_t duration_ms;
    Format format;
  };
  struct Playlist {
    uint32_t handle;
    std::u16string name;
    uint32_t flags;
    std::vector<std::unique_ptr<Song>> songs;
  };

  // User playlists (the console's music library) and their songs. Titles are
  // what the title matches on: SSX looks a playlist up again by title, never by
  // its 572-byte ID.
  struct UserSong {
    std::u16string title;
    std::u16string artist;
    std::u16string album;
    double tone_hz = 0.0;  // test tone (--xmp_test_tone)
    uint32_t duration_ms = 0;
  };
  struct UserPlaylist {
    std::u16string title;
    std::vector<UserSong> songs;
  };
  // XMP_USER_PLAYLIST_INFO, in guest layout.
  static constexpr size_t kUserPlaylistInfoSize = 0x334;
  using UserPlaylistRecord = std::array<uint8_t, kUserPlaylistInfoSize>;

  explicit XmpApp(system::KernelState* kernel_state);
  ~XmpApp() override;

  // The records XamCreateEnumeratorHandle(0xFA, 0x70026) hands out.
  std::vector<UserPlaylistRecord> UserPlaylistRecords() const;

  X_HRESULT XMPGetStatus(uint32_t status_ptr);

  X_HRESULT XMPCreateTitlePlaylist(uint32_t songs_ptr, uint32_t song_count,
                                   uint32_t playlist_name_ptr, const std::u16string& playlist_name,
                                   uint32_t flags, uint32_t out_song_handles,
                                   uint32_t out_playlist_handle);
  X_HRESULT XMPDeleteTitlePlaylist(uint32_t playlist_handle);
  X_HRESULT XMPPlayTitlePlaylist(uint32_t playlist_handle, uint32_t song_handle);
  X_HRESULT XMPContinue();
  X_HRESULT XMPStop(uint32_t unk);
  X_HRESULT XMPPause();
  X_HRESULT XMPNext();
  X_HRESULT XMPPrevious();

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  static const uint32_t kMsgStateChanged = 0x0A000001;
  static const uint32_t kMsgPlaybackBehaviorChanged = 0x0A000002;
  static const uint32_t kMsgPlaybackControllerChanged = 0x0A000003;

  void OnStateChanged();

  X_HRESULT XMPCreateUserPlaylistEnumerator(uint32_t buffer_ptr);
  X_HRESULT XMPPlayUserPlaylist(uint32_t buffer_ptr, uint32_t buffer_length);
  X_HRESULT XMPCaptureOutput(uint32_t buffer_ptr);
  X_HRESULT XMPGetCurrentSong(uint32_t info_ptr);
  // User-playlist side of the transport messages; false if no user playlist is active.
  bool UserTransport(uint32_t message);
  // Audio Worker, before each title audio frame: one block of capture output.
  void PumpCapture(runtime::FunctionDispatcher* dispatcher, runtime::ThreadState* thread_state);
  // Renders 256 stereo frames of the current song; moves to the next song at the
  // end of one. Returns false at the end of the playlist. capture_mutex_ held.
  bool RenderUserFrames(float* interleaved, uint32_t frames);

  std::atomic<State> state_;
  PlaybackClient playback_client_;
  PlaybackMode playback_mode_;
  RepeatMode repeat_mode_;
  uint32_t unknown_flags_;
  float volume_;
  Playlist* active_playlist_;
  int active_song_index_;

  rex::thread::global_critical_region global_critical_region_;
  std::unordered_map<uint32_t, Playlist*> playlists_;
  uint32_t next_playlist_handle_;
  uint32_t next_song_handle_;

  // User playlists: fixed after construction.
  std::vector<UserPlaylist> user_playlists_;
  // Capture output (XMPCaptureOutput) and the user playlist it plays. Guarded by
  // capture_mutex_, which the pump holds while the title's callback runs, so
  // Stop, Pause, Next and capture-off wait for a callback in progress.
  std::mutex capture_mutex_;
  int active_user_playlist_ = -1;
  size_t active_user_song_ = 0;
  uint64_t song_frame_ = 0;  // frames of the current song already delivered
  uint32_t capture_callback_ = 0;
  uint32_t capture_context_ = 0;
  bool capture_title_render_ = false;
  uint32_t capture_buffer_ = 0;  // 2 KB guest buffer for one block
  uint64_t capture_blocks_ = 0;
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
