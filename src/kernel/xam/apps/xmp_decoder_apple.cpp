/**
 * @file        kernel/xam/apps/xmp_decoder_apple.cpp
 * @brief       Song decoding with AudioToolbox's ExtAudioFile (macOS).
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include "xmp_decoder.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

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
    // Decode to float at 48 kHz; mono stays mono here and is doubled in Read.
    decoder->channels_ = file_format.mChannelsPerFrame == 1 ? 1 : 2;
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
    if (channels_ == 1) {
      mono_.resize(frames);
      target = mono_.data();
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
        interleaved[i * 2] = mono_[i];
        interleaved[i * 2 + 1] = mono_[i];
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

  ExtAudioFileRef file_ = nullptr;
  UInt32 channels_ = 2;
  std::vector<float> mono_;
};

}  // namespace

std::unique_ptr<XmpSongDecoder> OpenXmpSongFile(const std::filesystem::path& path,
                                                std::string* error) {
  return AppleDecoder::Open(path, error);
}

}  // namespace rex::kernel::xam::apps
