#ifndef SAMPLE_CONFIG_H
#define SAMPLE_CONFIG_H

#include <stdint.h>

// Structure to hold sample mapping configuration
typedef struct {
    uint8_t midi_note;
    const char* wav_file;
    float volume;
} sample_mapping_t;

// Define your sample mappings here
static const sample_mapping_t SAMPLE_MAPPINGS[] = {
    {36, "/storage/kick.wav", 1.0f},    // MIDI note 36 (C1) - Kick
    {38, "/storage/snare.wav", 1.0f},   // MIDI note 38 (D1) - Snare
    {42, "/storage/hihat.wav", 0.8f},   // MIDI note 42 (F#1) - Closed Hi-hat
    // Add more mappings as needed
};

#define NUM_SAMPLES (sizeof(SAMPLE_MAPPINGS) / sizeof(sample_mapping_t))

#endif // SAMPLE_CONFIG_H 