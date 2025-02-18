#ifndef WAV_PLAYER_H
#define WAV_PLAYER_H

#include <stdint.h>
#include <esp_err.h>
#include "driver/i2s_std.h"

// WAV header structure
typedef struct {
    uint8_t riff[4];          // "RIFF"
    uint32_t chunk_size;
    uint8_t wave[4];          // "WAVE"
    uint8_t fmt[4];           // "fmt "
    uint32_t subchunk1_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

// Initialize the WAV player
esp_err_t wav_player_init(i2s_chan_handle_t i2s_handle);

// Play a WAV file
esp_err_t wav_player_play(const char* filename, float volume);

#endif // WAV_PLAYER_H 