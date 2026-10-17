/**
 * @file        kernel/xam/apps/xmp_decoder_apple.cpp
 * @brief       Song decoding with AudioToolbox's ExtAudioFile (macOS).
 *
 * @copyright   Copyright (c) 2026 Ashton (github.com/saintmurdis)
 * @license     BSD 3-Clause License
 */

#include "xmp_decoder.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace rex::kernel::xam::apps {
namespace {

std::u16string ToU16(CFStringRef text) {
  if (!text || CFGetTypeID(text) != CFStringGetTypeID()) {
    return {};
  }
  const CFIndex length = CFStringGetLength(text);
  std::u16string out(size_t(length), u'\0');
  CFStringGetCharacters(text, CFRangeMake(0, length), reinterpret_cast<UniChar*>(out.data()));
  return out;
}

std::string FourCC(UInt32 id) {
  switch (id) {
    case kAudioFormatMPEGLayer3:
      return "mp3";
    case kAudioFormatMPEG4AAC:
    case kAudioFormatMPEG4AAC_HE:
    case kAudioFormatMPEG4AAC_HE_V2:
      return "aac";
    case kAudioFormatAppleLossless:
      return "alac";
    case kAudioFormatFLAC:
      return "flac";
    case kAudioFormatLinearPCM:
      return "pcm";
    default: {
      char c[5] = {char(id >> 24), char(id >> 16), char(id >> 8), char(id), 0};
      return c;
    }
  }
}

class AppleDecoder final : public XmpSongDecoder {
 public:
  ~AppleDecoder() override {
    if (file_) {
      ExtAudioFileDispose(file_);
    }
  }

  static std::unique_ptr<XmpSongDecoder> Open(const std::filesystem::path& path,
                                              std::string* error) {
    auto fail = [&](const char* what, OSStatus status) -> std::unique_ptr<XmpSongDecoder> {
      if (error) {
        *error = std::string(what) + " (" + FourCC(UInt32(status)) + ", " +
                 std::to_string(int(status)) + ")";
      }
      return nullptr;
    };
    const std::string utf8 = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()), CFIndex(utf8.size()),
        false);
    if (!url) {
      return fail("bad path", 0);
    }
    auto decoder = std::unique_ptr<AppleDecoder>(new AppleDecoder());
    OSStatus status = ExtAudioFileOpenURL(url, &decoder->file_);
    CFRelease(url);
    if (status != noErr) {
      decoder->file_ = nullptr;
      return fail("can't open (unsupported, protected or damaged)", status);
    }
    AudioStreamBasicDescription file_format{};
    UInt32 size = sizeof(file_format);
    status = ExtAudioFileGetProperty(decoder->file_, kExtAudioFileProperty_FileDataFormat, &size,
                                     &file_format);
    if (status != noErr || file_format.mChannelsPerFrame == 0 || file_format.mSampleRate <= 0) {
      return fail("no audio format", status);
    }
    // Decode to float at 48 kHz in the file's own channels: mono is doubled and
    // more than two channels are folded to stereo in Read (the converter would
    // just drop channels 3 and up of PCM and lossless files).
    decoder->channels_ = std::min<UInt32>(file_format.mChannelsPerFrame, kMaxChannels);
    if (decoder->channels_ > 2) {
      decoder->SetupFold();
    }
    AudioStreamBasicDescription client{};
    client.mSampleRate = kSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = decoder->channels_;
    client.mBitsPerChannel = 32;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = 4 * decoder->channels_;
    client.mBytesPerPacket = client.mBytesPerFrame;
    status = ExtAudioFileSetProperty(decoder->file_, kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(client), &client);
    if (status != noErr) {
      return fail("can't convert to 48 kHz float", status);
    }
    SInt64 file_frames = 0;
    size = sizeof(file_frames);
    if (ExtAudioFileGetProperty(decoder->file_, kExtAudioFileProperty_FileLengthFrames, &size,
                                &file_frames) == noErr &&
        file_frames > 0) {
      decoder->info_.duration_ms = uint32_t(double(file_frames) * 1000.0 / file_format.mSampleRate);
    }
    decoder->info_.format = FourCC(file_format.mFormatID);
    decoder->info_.source_rate = file_format.mSampleRate;
    decoder->ReadTags();
    return decoder;
  }

  uint32_t Read(float* interleaved, uint32_t frames) override {
    float* target = interleaved;
    if (channels_ != 2) {
      scratch_.resize(size_t(frames) * channels_);
      target = scratch_.data();
    }
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = channels_;
    list.mBuffers[0].mDataByteSize = frames * 4 * channels_;
    list.mBuffers[0].mData = target;
    UInt32 read = frames;
    if (ExtAudioFileRead(file_, &read, &list) != noErr) {
      return 0;
    }
    if (channels_ == 1) {
      for (UInt32 i = 0; i < read; ++i) {
        interleaved[i * 2] = scratch_[i];
        interleaved[i * 2 + 1] = scratch_[i];
      }
    } else if (channels_ > 2) {
      for (UInt32 i = 0; i < read; ++i) {
        float l = 0.0f;
        float r = 0.0f;
        for (UInt32 c = 0; c < channels_; ++c) {
          const float v = scratch_[size_t(i) * channels_ + c];
          l += v * fold_left_[c];
          r += v * fold_right_[c];
        }
        interleaved[i * 2] = l;
        interleaved[i * 2 + 1] = r;
      }
    }
    return read;
  }

 private:
  AppleDecoder() = default;

  void ReadTags() {
    AudioFileID audio_file = nullptr;
    UInt32 size = sizeof(audio_file);
    if (ExtAudioFileGetProperty(file_, kExtAudioFileProperty_AudioFile, &size, &audio_file) !=
            noErr ||
        !audio_file) {
      return;
    }
    CFDictionaryRef tags = nullptr;
    size = sizeof(tags);
    if (AudioFileGetProperty(audio_file, kAudioFilePropertyInfoDictionary, &size, &tags) !=
            noErr ||
        !tags) {
      return;
    }
    auto get = [&](const char* key) {
      CFStringRef cf_key = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
      auto value = static_cast<CFStringRef>(CFDictionaryGetValue(tags, cf_key));
      CFRelease(cf_key);
      return ToU16(value);
    };
    info_.title = get(kAFInfoDictionary_Title);
    info_.artist = get(kAFInfoDictionary_Artist);
    info_.album = get(kAFInfoDictionary_Album);
    info_.genre = get(kAFInfoDictionary_Genre);
    const std::u16string track = get(kAFInfoDictionary_TrackNumber);  // "3" or "3/12"
    uint32_t number = 0;
    for (char16_t c : track) {
      if (c < u'0' || c > u'9') {
        break;
      }
      number = number * 10 + uint32_t(c - u'0');
    }
    info_.track_number = number;
    CFRelease(tags);
  }

  // Stereo fold weights per file channel, by its layout label: fronts in full,
  // center and surrounds at -3 dB, LFE dropped, all scaled by 0.586 (the SDK's
  // own 5.1 fold) so a full-scale mix doesn't clip.
  void SetupFold() {
    std::vector<AudioChannelLabel> labels(channels_, kAudioChannelLabel_Unknown);
    UInt32 size = 0;
    Boolean writable = false;
    if (ExtAudioFileGetPropertyInfo(file_, kExtAudioFileProperty_FileChannelLayout, &size,
                                    &writable) == noErr &&
        size >= sizeof(AudioChannelLayout)) {
      std::vector<uint8_t> raw(size);
      auto* layout = reinterpret_cast<AudioChannelLayout*>(raw.data());
      if (ExtAudioFileGetProperty(file_, kExtAudioFileProperty_FileChannelLayout, &size, layout) ==
          noErr) {
        std::vector<uint8_t> expanded;
        const AudioChannelLayout* use = layout;
        if (layout->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions) {
          const bool bitmap = layout->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelBitmap;
          const AudioFormatPropertyID property = bitmap ? kAudioFormatProperty_ChannelLayoutForBitmap
                                                        : kAudioFormatProperty_ChannelLayoutForTag;
          const void* spec = bitmap ? static_cast<const void*>(&layout->mChannelBitmap)
                                    : static_cast<const void*>(&layout->mChannelLayoutTag);
          UInt32 expanded_size = 0;
          if (AudioFormatGetPropertyInfo(property, sizeof(UInt32), spec, &expanded_size) == noErr &&
              expanded_size >= sizeof(AudioChannelLayout)) {
            expanded.resize(expanded_size);
            if (AudioFormatGetProperty(property, sizeof(UInt32), spec, &expanded_size,
                                       expanded.data()) == noErr) {
              use = reinterpret_cast<const AudioChannelLayout*>(expanded.data());
            }
          }
        }
        if (use->mChannelLayoutTag == kAudioChannelLayoutTag_UseChannelDescriptions) {
          for (UInt32 i = 0; i < use->mNumberChannelDescriptions && i < channels_; ++i) {
            labels[i] = use->mChannelDescriptions[i].mChannelLabel;
          }
        }
      }
    }
    bool known = false;
    for (AudioChannelLabel label : labels) {
      known |= label != kAudioChannelLabel_Unknown && label != kAudioChannelLabel_Unused;
    }
    if (!known) {
      // No layout: the WAV order, L R C LFE Ls Rs.
      static constexpr AudioChannelLabel kWave[] = {
          kAudioChannelLabel_Left,      kAudioChannelLabel_Right,        kAudioChannelLabel_Center,
          kAudioChannelLabel_LFEScreen, kAudioChannelLabel_LeftSurround, kAudioChannelLabel_RightSurround};
      for (UInt32 i = 0; i < channels_ && i < 6; ++i) {
        labels[i] = kWave[i];
      }
    }
    constexpr float kScale = 0.586f;
    constexpr float kSide = 0.7071f;
    fold_left_.assign(channels_, 0.0f);
    fold_right_.assign(channels_, 0.0f);
    for (UInt32 c = 0; c < channels_; ++c) {
      float l = 0.0f;
      float r = 0.0f;
      switch (labels[c]) {
        case kAudioChannelLabel_Left:
        case kAudioChannelLabel_LeftWide:
        case kAudioChannelLabel_LeftCenter:
          l = 1.0f;
          break;
        case kAudioChannelLabel_Right:
        case kAudioChannelLabel_RightWide:
        case kAudioChannelLabel_RightCenter:
          r = 1.0f;
          break;
        case kAudioChannelLabel_Center:
        case kAudioChannelLabel_CenterSurround:
          l = r = kSide;
          break;
        case kAudioChannelLabel_LeftSurround:
        case kAudioChannelLabel_LeftSurroundDirect:
        case kAudioChannelLabel_RearSurroundLeft:
          l = kSide;
          break;
        case kAudioChannelLabel_RightSurround:
        case kAudioChannelLabel_RightSurroundDirect:
        case kAudioChannelLabel_RearSurroundRight:
          r = kSide;
          break;
        case kAudioChannelLabel_LFEScreen:
        case kAudioChannelLabel_LFE2:
          break;
        default:
          l = r = 0.5f;  // anything else: centered, a little lower
          break;
      }
      fold_left_[c] = l * kScale;
      fold_right_[c] = r * kScale;
    }
  }

  static constexpr UInt32 kMaxChannels = 8;
  ExtAudioFileRef file_ = nullptr;
  UInt32 channels_ = 2;
  std::vector<float> scratch_;
  std::vector<float> fold_left_;
  std::vector<float> fold_right_;
};

}  // namespace

std::u16string NormalizeXmpName(const std::u16string& name) {
  CFMutableStringRef text = CFStringCreateMutable(kCFAllocatorDefault, 0);
  if (!text) {
    return name;
  }
  CFStringAppendCharacters(text, reinterpret_cast<const UniChar*>(name.data()), CFIndex(name.size()));
  CFStringNormalize(text, kCFStringNormalizationFormC);
  std::u16string out = ToU16(text);
  CFRelease(text);
  return out;
}

std::unique_ptr<XmpSongDecoder> OpenXmpSongFile(const std::filesystem::path& path,
                                                std::string* error) {
  return AppleDecoder::Open(path, error);
}

}  // namespace rex::kernel::xam::apps
