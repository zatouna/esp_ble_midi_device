#include "wav_player.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "WAV_PLAYER"
#define BUFFER_SIZE 1024

static i2s_chan_handle_t i2s_handle;

esp_err_t wav_player_init(i2s_chan_handle_t handle) {
    i2s_handle = handle;
    
    // Initialize SPIFFS
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS (%s)", esp_err_to_name(ret));
        return ret;
    }
    
    return ESP_OK;
}

esp_err_t wav_player_play(const char* filename, float volume) {
    FILE* file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open file: %s", filename);
        return ESP_FAIL;
    }
    
    wav_header_t header;
    if (fread(&header, 1, sizeof(header), file) != sizeof(header)) {
        ESP_LOGE(TAG, "Failed to read WAV header");
        fclose(file);
        return ESP_FAIL;
    }
    
    // Verify WAV format
    if (memcmp(header.riff, "RIFF", 4) != 0 || 
        memcmp(header.wave, "WAVE", 4) != 0 ||
        header.audio_format != 1) { // PCM = 1
        ESP_LOGE(TAG, "Invalid WAV format");
        fclose(file);
        return ESP_FAIL;
    }
    
    // Read and play audio data
    int16_t buffer[BUFFER_SIZE];
    size_t bytes_read;
    size_t bytes_written;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        // Apply volume
        for (int i = 0; i < bytes_read / 2; i++) {
            buffer[i] = (int16_t)(buffer[i] * volume);
        }
        
        // Write to I2S
        esp_err_t ret = i2s_channel_write(i2s_handle, buffer, bytes_read, 
                                        &bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to write to I2S: %d", ret);
            fclose(file);
            return ret;
        }
    }
    
    fclose(file);
    return ESP_OK;
} 