// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Sound Manager buffers as samples, and one voice mixed into stereo output
// (sound_mix.c).

#ifndef HLE_SOUND_MIX_H_
#define HLE_SOUND_MIX_H_

#include <stddef.h>
#include <stdint.h>

// A sound header's encode byte: standard, extended or compressed.
enum {
  stdSH = 0x00,
  extSH = 0xFF,
  cmpSH = 0xFE,
};

#pragma pack(push, 2)

typedef struct {
  char* samplePtr;  // NULL when the samples follow the header
  uint32_t length;  // frames
  uint32_t sampleRate;  // 16.16
  uint32_t loopStart;
  uint32_t loopEnd;
  uint8_t encode;
  uint8_t baseFrequency;
  uint8_t sampleArea[1];
} SoundHeader;

typedef struct {
  char* samplePtr;
  uint32_t numChannels;
  uint32_t sampleRate;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint8_t encode;
  uint8_t baseFrequency;
  uint32_t numFrames;
  uint8_t AIFFSampleRate[10];
  char* markerChunk;
  char* instrumentChunks;
  char* AESRecording;
  uint16_t sampleSize;  // bits
  uint16_t futureUse1;
  uint32_t futureUse2;
  uint32_t futureUse3;
  uint32_t futureUse4;
  uint8_t sampleArea[1];
} ExtSoundHeader;

typedef struct {
  char* samplePtr;
  uint32_t numChannels;
  uint32_t sampleRate;
  uint32_t loopStart;
  uint32_t loopEnd;
  uint8_t encode;
  uint8_t baseFrequency;
  uint32_t numFrames;  // packets, for a codec that packs samples
  uint8_t AIFFSampleRate[10];
  char* markerChunk;
  uint32_t format;
  uint32_t futureUse2;
  void* stateVars;
  void* leftOverSamples;
  int16_t compressionID;
  uint16_t packetSize;
  uint16_t snthID;
  uint16_t sampleSize;
  uint8_t sampleArea[1];
} CmpSoundHeader;

#pragma pack(pop)

// The samples a sound header describes.
typedef struct {
  const uint8_t* data;
  uint32_t frames;
  uint32_t rate;  // 16.16 frames a second
  uint8_t channels;  // 1 or 2, interleaved
  uint8_t bytes;  // a sample's size: 1 or 2
  uint8_t big_endian;
  uint8_t offset_binary;  // 8-bit samples centred on 0x80
} hle_sound_buffer;

enum {
  kHleSoundOk,
  kHleSoundBadHeader,
  kHleSoundUnsupported,  // compressed, or a layout not played
};

// Reads the header a bufferCmd names. |format|, when given, receives a
// compressed header's format code.
int hle_sound_buffer_from_header(const uint8_t* header,
                                 hle_sound_buffer* buffer, uint32_t* format);

// How long any header's sound plays, compressed ones included, in
// nanoseconds.
uint64_t hle_sound_header_duration_ns(const uint8_t* header,
                                      uint32_t rate_multiplier);

// One buffer being played.
typedef struct {
  hle_sound_buffer buffer;
  uint64_t position;  // 32.32 frames into the buffer
  uint32_t rate_multiplier;  // 16.16; 0 holds the voice where it is
  uint16_t left;  // 8.8: 0x0100 is full volume
  uint16_t right;
  uint8_t amplitude;  // 0-255, on top of the volume
  uint8_t interpolate;
  uint8_t playing;
} hle_sound_voice;

// Adds |frames| frames of |voice|, resampled to |output_rate|, to |mix|,
// which holds a left and a right sum for each frame. The voice stops
// playing when it reaches the end of its buffer.
void hle_sound_voice_mix(hle_sound_voice* voice, int32_t* mix, int frames,
                         int output_rate);

// How long what is left of |voice| takes to play, in nanoseconds;
// UINT64_MAX while its rate multiplier holds it.
uint64_t hle_sound_voice_remaining_ns(const hle_sound_voice* voice);

#endif  // HLE_SOUND_MIX_H_
