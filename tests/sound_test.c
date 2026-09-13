// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/sound_mix.c: sound headers read as samples, and voices mixed
// at another rate, with a volume, an amplitude and a rate multiplier.
//
//   make tests/sound_test && tests/sound_test

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hle/sound_mix.h"

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static hle_sound_voice voice_for(const hle_sound_buffer* buffer) {
  hle_sound_voice voice;
  memset(&voice, 0, sizeof(voice));
  voice.buffer = *buffer;
  voice.rate_multiplier = 0x10000;
  voice.left = 0x0100;
  voice.right = 0x0100;
  voice.amplitude = 255;
  voice.playing = 1;
  return voice;
}

// Whether the left sums of |mix| are |expected|, frame by frame.
static int left_sums_are(const int32_t* mix, const int32_t* expected,
                         int frames) {
  for (int i = 0; i < frames; i++) {
    if (mix[2 * i] != expected[i]) {
      return 0;
    }
  }
  return 1;
}

int main(void) {
  hle_sound_buffer buffer;
  hle_sound_voice voice;
  int32_t mix[16];

  // A standard header with its samples after it: 8-bit offset binary.
  uint8_t standard_storage[64];
  memset(standard_storage, 0, sizeof(standard_storage));
  SoundHeader* standard = (SoundHeader*)standard_storage;
  standard->length = 4;
  standard->sampleRate = 44100u << 16;
  standard->encode = stdSH;
  static const uint8_t kOffsetBinary[] = { 0x80, 0xFF, 0x00, 0x90 };
  memcpy(standard->sampleArea, kOffsetBinary, sizeof(kOffsetBinary));
  check(hle_sound_buffer_from_header(standard_storage, &buffer, NULL) ==
            kHleSoundOk,
        "a standard header is read");
  check(buffer.data == standard->sampleArea && buffer.frames == 4 &&
            buffer.channels == 1 && buffer.bytes == 1,
        "a standard header's samples follow it");
  voice = voice_for(&buffer);
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 8, 44100);
  static const int32_t kStandardMix[16] = {
    0, 0, 0x7F00, 0x7F00, -0x8000, -0x8000, 0x1000, 0x1000,
  };
  check(memcmp(mix, kStandardMix, sizeof(mix)) == 0,
        "8-bit offset binary is centred on 0x80, and mono plays on both sides");
  check(!voice.playing, "a voice stops at the end of its buffer");

  // An extended header: 16-bit stereo in an Intel Mac's byte order, at half
  // the output rate.
  static const uint8_t kLittleEndianStereo[] = {
    0x00, 0x01, 0x00, 0xFF,
    0xFF, 0x7F, 0x00, 0x80,
  };
  ExtSoundHeader extended;
  memset(&extended, 0, sizeof(extended));
  extended.samplePtr = (char*)kLittleEndianStereo;
  extended.numChannels = 2;
  extended.sampleRate = 22050u << 16;
  extended.encode = extSH;
  extended.numFrames = 2;
  extended.sampleSize = 16;
  check(hle_sound_buffer_from_header((const uint8_t*)&extended, &buffer,
                                     NULL) == kHleSoundOk,
        "an extended header is read");
  voice = voice_for(&buffer);
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 8, 44100);
  static const int32_t kExtendedMix[16] = {
    0x0100, -0x0100, 0x0100, -0x0100, 0x7FFF, -0x8000, 0x7FFF, -0x8000,
  };
  check(memcmp(mix, kExtendedMix, sizeof(mix)) == 0 && !voice.playing,
        "16-bit little-endian stereo, each frame twice at twice the rate");

  // A compressed header of little-endian samples, one side at half volume.
  static const uint8_t kLittleEndian[] = { 0x00, 0x40, 0x00, 0xC0 };
  CmpSoundHeader compressed;
  memset(&compressed, 0, sizeof(compressed));
  compressed.samplePtr = (char*)kLittleEndian;
  compressed.numChannels = 1;
  compressed.sampleRate = 44100u << 16;
  compressed.encode = cmpSH;
  compressed.numFrames = 2;
  compressed.format = 'sowt';
  compressed.sampleSize = 16;
  uint32_t format = 0;
  check(hle_sound_buffer_from_header((const uint8_t*)&compressed, &buffer,
                                     &format) == kHleSoundOk &&
            format == 'sowt',
        "a compressed header of little-endian samples is read");
  voice = voice_for(&buffer);
  voice.left = 0x0080;
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 2, 44100);
  check(mix[0] == 0x2000 && mix[1] == 0x4000 && mix[2] == -0x2000 &&
            mix[3] == -0x4000,
        "the volume scales each side");
  voice = voice_for(&buffer);
  voice.amplitude = 0;
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 2, 44100);
  check(mix[0] == 0 && mix[1] == 0, "an amplitude of 0 is silent");

  // A compressed header of big-endian samples.
  static const uint8_t kBigEndian[] = { 0x40, 0x00, 0xC0, 0x00 };
  CmpSoundHeader big_endian = compressed;
  big_endian.samplePtr = (char*)kBigEndian;
  big_endian.format = 'twos';
  check(hle_sound_buffer_from_header((const uint8_t*)&big_endian, &buffer,
                                     NULL) == kHleSoundOk,
        "a compressed header of big-endian samples is read");
  voice = voice_for(&buffer);
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 2, 44100);
  check(mix[0] == 0x4000 && mix[2] == -0x4000, "'twos' samples are big-endian");

  // A rate multiplier of 2 plays every other frame.
  static const uint8_t kRamp[] = {
    0x00, 0x10, 0x00, 0x20, 0x00, 0x30, 0x00, 0x40,
  };
  compressed.samplePtr = (char*)kRamp;
  compressed.numFrames = 4;
  hle_sound_buffer_from_header((const uint8_t*)&compressed, &buffer, NULL);
  voice = voice_for(&buffer);
  voice.rate_multiplier = 0x20000;
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 4, 44100);
  static const int32_t kDoubled[4] = { 0x1000, 0x3000, 0, 0 };
  check(left_sums_are(mix, kDoubled, 4) && !voice.playing,
        "a rate multiplier of 2 plays every other frame");
  voice = voice_for(&buffer);
  voice.rate_multiplier = 0;
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 4, 44100);
  check(mix[0] == 0 && voice.playing && voice.position == 0 &&
            hle_sound_voice_remaining_ns(&voice) == UINT64_MAX,
        "a rate multiplier of 0 holds the voice");

  // Interpolation between frames when asked for.
  compressed.sampleRate = 22050u << 16;
  hle_sound_buffer_from_header((const uint8_t*)&compressed, &buffer, NULL);
  voice = voice_for(&buffer);
  voice.interpolate = 1;
  memset(mix, 0, sizeof(mix));
  hle_sound_voice_mix(&voice, mix, 8, 44100);
  static const int32_t kInterpolated[8] = {
    0x1000, 0x1800, 0x2000, 0x2800, 0x3000, 0x3800, 0x4000, 0x4000,
  };
  check(left_sums_are(mix, kInterpolated, 8) && !voice.playing,
        "interpolation at half the output rate");

  // How long voices and headers take to play.
  hle_sound_voice timing;
  memset(&timing, 0, sizeof(timing));
  timing.buffer.frames = 22050;
  timing.buffer.rate = 22050u << 16;
  timing.rate_multiplier = 0x10000;
  timing.playing = 1;
  uint64_t ns = hle_sound_voice_remaining_ns(&timing);
  check(ns > 999000000u && ns < 1001000000u,
        "a second of samples takes a second");
  timing.position = (uint64_t)11025 << 32;
  timing.rate_multiplier = 0x20000;
  ns = hle_sound_voice_remaining_ns(&timing);
  check(ns > 249000000u && ns < 251000000u,
        "half the samples left at twice the rate take a quarter of it");

  compressed.format = 'ima4';
  compressed.compressionID = -1;
  compressed.sampleRate = 44100u << 16;
  compressed.numFrames = 689;
  check(hle_sound_buffer_from_header((const uint8_t*)&compressed, &buffer,
                                     NULL) == kHleSoundUnsupported,
        "IMA 4:1 is not played");
  ns = hle_sound_header_duration_ns((const uint8_t*)&compressed, 0x10000);
  check(ns > 999000000u && ns < 1000000000u,
        "an IMA 4:1 header's packets count 64 frames each");

  uint8_t unknown[64];
  memset(unknown, 0, sizeof(unknown));
  unknown[offsetof(SoundHeader, encode)] = 0x42;
  check(hle_sound_buffer_from_header(unknown, &buffer, NULL) ==
            kHleSoundBadHeader,
        "an unknown encode is refused");

  if (failures == 0) {
    printf("all passed\n");
  }
  return failures != 0;
}
