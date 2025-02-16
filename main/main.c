#include "esp_log.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include <string.h>
#include "driver/i2s.h"
#include "math.h"

#define DEVICE_NAME "ESP32-retrial"
#define TAG "BLE_MIDI"

// Add these global variable declarations
static char latest_midi_msg[128] = "";
static bool new_message = false;
static httpd_handle_t server = NULL;

// Define MIDI service and characteristic UUIDs
static const ble_uuid128_t midi_service_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
);

static const ble_uuid128_t midi_characteristic_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
);

// Add these definitions
#define I2S_NUM (0)
#define I2S_BCK_PIN (15)
#define I2S_LRCK_PIN (16)
#define I2S_DATA_PIN (17)

#define SAMPLE_RATE     44100
#define SAMPLE_BITS     16
#define CHANNELS        2

// MIDI note numbers
#define MIDI_NOTE_KICK  36
#define MIDI_NOTE_SNARE 38
#define MIDI_NOTE_HIHAT 42

// Buffer for audio samples
#define SAMPLES_PER_BUFFER 256
static int16_t audio_buffer[SAMPLES_PER_BUFFER * 2];  // *2 for stereo

// Add this MIDI state structure near the top with other global variables
struct midi_state_t {
    uint8_t last_status;
    bool running_status_active;
    uint8_t sysex_buffer[256];
    size_t sysex_len;
    bool in_sysex;
    uint32_t last_timestamp;
};

static struct midi_state_t midi_state;

// Add these utility functions before midi_chr_access
static void init_midi_state(struct midi_state_t *state) {
    memset(state, 0, sizeof(struct midi_state_t));
}

static uint32_t decode_timestamp(uint8_t high, uint8_t low) {
    return ((high & 0x3F) << 7) | (low & 0x7F);
}

static bool is_realtime_message(uint8_t status) {
    return (status >= 0xF8 && status <= 0xFF);
}

// Add this function before midi_chr_access
static void handle_running_status(uint8_t status) {
    if ((status & 0xF0) != 0xF0) {  // Not system message
        midi_state.last_status = status;
        midi_state.running_status_active = true;
    } else {
        midi_state.running_status_active = false;  // System messages clear running status
    }
}

// Add these constants for sound generation
#define PI 3.14159265358979323846
#define SAMPLE_RATE 44100
#define VOLUME 0.5

// Add these sound generation functions
static void generate_kick(int16_t* buffer, size_t samples) {
    float frequency = 150.0;  // Starting frequency
    float decay = 0.002;      // Frequency decay rate
    float amplitude = 32000 * VOLUME;  // Starting amplitude
    float amp_decay = 0.998;  // Amplitude decay factor

    for (size_t i = 0; i < samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float current_freq = frequency * exp(-decay * i);
        float sample = amplitude * sin(2.0 * PI * current_freq * t);
        
        amplitude *= amp_decay;
        
        // Convert to 16-bit and apply to both channels
        int16_t sample_int = (int16_t)sample;
        buffer[i * 2] = sample_int;     // Left channel
        buffer[i * 2 + 1] = sample_int; // Right channel
    }
}

static void generate_snare(int16_t* buffer, size_t samples) {
    float frequency = 400.0;   // Main frequency component
    float noise_mix = 0.5;     // Mix between tone and noise
    float amplitude = 32000 * VOLUME;
    float amp_decay = 0.997;   // Faster decay than kick

    for (size_t i = 0; i < samples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float tone = sin(2.0 * PI * frequency * t);
        float noise = ((float)rand() / RAND_MAX) * 2.0 - 1.0;
        float sample = amplitude * (tone * (1.0 - noise_mix) + noise * noise_mix);
        
        amplitude *= amp_decay;
        
        int16_t sample_int = (int16_t)sample;
        buffer[i * 2] = sample_int;
        buffer[i * 2 + 1] = sample_int;
    }
}

static void generate_hihat(int16_t* buffer, size_t samples) {
    float amplitude = 24000 * VOLUME;  // Slightly lower volume
    float amp_decay = 0.995;          // Fast decay

    for (size_t i = 0; i < samples; i++) {
        // Generate filtered noise
        float noise = 0;
        for (int j = 0; j < 4; j++) {  // Simple filtering
            noise += ((float)rand() / RAND_MAX) * 2.0 - 1.0;
        }
        noise /= 4.0;
        
        float sample = amplitude * noise;
        amplitude *= amp_decay;
        
        int16_t sample_int = (int16_t)sample;
        buffer[i * 2] = sample_int;
        buffer[i * 2 + 1] = sample_int;
    }
}

static void play_sound(int16_t* buffer, size_t samples) {
    size_t bytes_written;
    size_t bytes_to_write = samples * 4;  // 4 bytes per sample (2 channels * 2 bytes per sample)
    
    ESP_LOGI(TAG, "Attempting to play sound: %d bytes", bytes_to_write);
    
    // Log first few samples for debugging
    for (int i = 0; i < 4 && i < samples; i++) {
        ESP_LOGI(TAG, "Sample %d: L=%d, R=%d", i, buffer[i*2], buffer[i*2+1]);
    }
    
    esp_err_t ret = i2s_write(I2S_NUM, buffer, bytes_to_write, &bytes_written, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write to I2S: %d", ret);
    } else {
        ESP_LOGI(TAG, "Successfully wrote %d bytes to I2S", bytes_written);
    }
}

// Update the process_midi_message function to include sound generation
static void process_midi_message(uint8_t status, uint8_t* data, size_t len, uint32_t timestamp) {
    // Log received message
    ESP_LOGI(TAG, "MIDI message received - Status: 0x%02X, Len: %d, Timestamp: %lu", 
             status, len, timestamp);
    
    uint8_t message_type = status & 0xF0;
    if (message_type == 0x90 && len >= 2) {  // Note On
        uint8_t note = data[0];
        uint8_t velocity = data[1];
        
        ESP_LOGI(TAG, "Note On - Note: %d, Velocity: %d", note, velocity);
        
        if (velocity > 0) {
            switch(note) {
                case MIDI_NOTE_KICK:
                    generate_kick(audio_buffer, SAMPLES_PER_BUFFER);
                    play_sound(audio_buffer, SAMPLES_PER_BUFFER);
                    snprintf(latest_midi_msg, sizeof(latest_midi_msg), 
                        "Kick Drum - Note: %d, Velocity: %d", note, velocity);
                    break;
                    
                case MIDI_NOTE_SNARE:
                    generate_snare(audio_buffer, SAMPLES_PER_BUFFER);
                    play_sound(audio_buffer, SAMPLES_PER_BUFFER);
                    snprintf(latest_midi_msg, sizeof(latest_midi_msg), 
                        "Snare Drum - Note: %d, Velocity: %d", note, velocity);
                    break;
                    
                case MIDI_NOTE_HIHAT:
                    generate_hihat(audio_buffer, SAMPLES_PER_BUFFER);
                    play_sound(audio_buffer, SAMPLES_PER_BUFFER);
                    snprintf(latest_midi_msg, sizeof(latest_midi_msg), 
                        "Hi-Hat - Note: %d, Velocity: %d", note, velocity);
                    break;
                    
                default:
                    snprintf(latest_midi_msg, sizeof(latest_midi_msg), 
                        "Note On - Note: %d, Velocity: %d", note, velocity);
                    break;
            }
            new_message = true;
            ESP_LOGI(TAG, "Latest MIDI message: %s", latest_midi_msg);
        }
    }
}

// Replace existing midi_chr_access with this version
static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            return 0;

        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            uint8_t *data = OS_MBUF_DATA(ctxt->om, uint8_t *);
            size_t len = OS_MBUF_PKTLEN(ctxt->om);
            
            ESP_LOGI(TAG, "Received BLE write - Length: %d bytes", len);
            ESP_LOGI(TAG, "Raw data: ");
            for (size_t i = 0; i < len; i++) {
                ESP_LOGI(TAG, "  [%d]: 0x%02x", i, data[i]);
            }

            // BLE-MIDI packets must be at least 3 bytes: header, timestamp, status
            if (len < 3) {
                ESP_LOGW(TAG, "Packet too short");
                return 0;
            }

            // Process packet byte by byte
            size_t i = 0;
            while (i < len) {
                // Check for header byte (must have bit 7 set and bit 6 clear)
                if ((data[i] & 0x80) && !(data[i] & 0x40)) {
                    if (i + 1 >= len) {
                        ESP_LOGW(TAG, "Incomplete packet at byte %d", i);
                        break;  // Not enough bytes for timestamp
                    }

                    // Extract timestamp
                    uint32_t timestamp = decode_timestamp(data[i], data[i+1]);
                    i += 2;  // Move past header and timestamp bytes
                    ESP_LOGI(TAG, "Timestamp: %lu", timestamp);

                    // Process MIDI data
                    if (i < len) {
                        uint8_t status = data[i];
                        ESP_LOGI(TAG, "Status byte: 0x%02x", status);

                        if (is_realtime_message(status)) {
                            // Handle real-time message
                            process_midi_message(status, NULL, 0, timestamp);
                            i++;
                            continue;
                        }

                        if (status == 0xF0) {  // Start of SysEx
                            midi_state.in_sysex = true;
                            midi_state.sysex_len = 0;
                            i++;
                            continue;
                        }

                        if (midi_state.in_sysex) {
                            if (status == 0xF7) {  // End of SysEx
                                midi_state.in_sysex = false;
                                process_midi_message(0xF0, midi_state.sysex_buffer, 
                                                  midi_state.sysex_len, timestamp);
                                i++;
                                continue;
                            }
                            // Add to SysEx buffer
                            if (midi_state.sysex_len < sizeof(midi_state.sysex_buffer)) {
                                midi_state.sysex_buffer[midi_state.sysex_len++] = status;
                            }
                            i++;
                            continue;
                        }

                        if ((status & 0x80) == 0x80) {  // New status byte
                            handle_running_status(status);
                            if (i + 2 < len) {  // Make sure we have enough data bytes
                                process_midi_message(status, &data[i+1], 2, timestamp);
                                i += 3;  // Skip status and two data bytes
                            } else {
                                ESP_LOGW(TAG, "Incomplete message at byte %d", i);
                                break;
                            }
                        } else if (midi_state.running_status_active) {
                            // Use running status (current byte is first data byte)
                            if (i + 1 < len) {  // Make sure we have both data bytes
                                process_midi_message(midi_state.last_status, &data[i], 2, timestamp);
                                i += 2;  // Skip two data bytes
                            } else {
                                ESP_LOGW(TAG, "Incomplete running status message at byte %d", i);
                                break;
                            }
                        } else {
                            ESP_LOGW(TAG, "Invalid status byte: 0x%02x", status);
                            i++;  // Skip invalid byte
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Invalid header byte at %d: 0x%02x", i, data[i]);
                    i++;  // Skip invalid byte
                }
            }
            return 0;
        }

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

// GATT service definitions
static struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &midi_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &midi_characteristic_uuid.u,
                .access_cb = midi_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                0, // No more characteristics
            }
        },
    },
    {
        0, // No more services
    },
};

// Add this struct definition near the top with other global variables
static struct ble_gap_event_listener gap_event_listener;

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    
    memset(&fields, 0, sizeof(fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    memset(&adv_params, 0, sizeof(adv_params));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &midi_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rsp_fields.name = (uint8_t *)DEVICE_NAME;
    rsp_fields.name_len = strlen(DEVICE_NAME);
    rsp_fields.name_is_complete = 1;

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_FAST_INTERVAL1_MIN;
    adv_params.itvl_max = BLE_GAP_ADV_FAST_INTERVAL1_MAX;

    ble_gap_adv_set_fields(&fields);
    ble_gap_adv_rsp_set_fields(&rsp_fields);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, NULL, NULL);

    ESP_LOGI(TAG, "Started advertising");
}



// Add this before on_connect
static int on_mtu(uint16_t conn_handle, const struct ble_gatt_error *error,
                 uint16_t mtu, void *arg) {
    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU exchange completed: %d", mtu);
    }
    return 0;
}

// Update on_connect to use the correct struct member
static bool params_locked = false;  // Add this at the top with other globals

static void on_connect(uint16_t conn_handle, const struct ble_gap_conn_desc *desc) {
    params_locked = false;
    
    // Start with moderate parameters
    struct ble_gap_upd_params params = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(30),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(50),
        .latency = 0,
        .supervision_timeout = 0x0100,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    
    int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "Initial connection parameter update failed with error: %d", rc);
    }
    
    // Initiate MTU exchange
    ble_gattc_exchange_mtu(conn_handle, on_mtu, NULL);
}

#define MAX_CONN_UPDATE_RETRIES 5
#define CONN_UPDATE_RETRY_DELAY_MS 200

static uint8_t conn_update_retry_count = 0;
static uint16_t current_conn_handle = 0;
static bool params_stable = false;

// Forward declaration for retry function
static void try_conn_update(void *arg);

// Timer handle for retries
static esp_timer_handle_t conn_update_timer;

// Initialize timer in your app_main or initialization function
static void init_conn_update_timer(void) {
    esp_timer_create_args_t timer_args = {
        .callback = try_conn_update,
        .name = "conn_update_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &conn_update_timer));
}

static void try_conn_update(void *arg) {
    if (conn_update_retry_count >= MAX_CONN_UPDATE_RETRIES) {
        ESP_LOGI(TAG, "Max connection parameter update retries reached");
        return;
    }

    struct ble_gap_upd_params params = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(30),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(50),
        .latency = 0,
        .supervision_timeout = 0x0100,
        .min_ce_len = BLE_GAP_INITIAL_CONN_MIN_CE_LEN,
        .max_ce_len = BLE_GAP_INITIAL_CONN_MAX_CE_LEN,
    };

    int rc = ble_gap_update_params(current_conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "Connection parameter update retry %d failed: %d", 
                 conn_update_retry_count + 1, rc);
        
        // Schedule next retry
        conn_update_retry_count++;
        esp_timer_start_once(conn_update_timer, CONN_UPDATE_RETRY_DELAY_MS * 1000);
    } else {
        ESP_LOGI(TAG, "Connection parameter update retry %d initiated", 
                 conn_update_retry_count + 1);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                current_conn_handle = event->connect.conn_handle;
                conn_update_retry_count = 0;
                params_stable = false;

                // Start connection parameter update process
                try_conn_update(NULL);
                ESP_LOGI(TAG, "Connection established");
            }
            break;
            
        case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
            if (params_stable) {
                ESP_LOGI(TAG, "Rejecting L2CAP update - parameters already stable");
                return BLE_HS_EREJECT;
            }
            
            const struct ble_gap_upd_params *peer_params = event->conn_update_req.peer_params;
            float min_ms = peer_params->itvl_min * 1.25;
            float max_ms = peer_params->itvl_max * 1.25;
            ESP_LOGI(TAG, "Received L2CAP update request: interval_min=%.2f ms, interval_max=%.2f ms",
                     min_ms, max_ms);
            
            // Check if the parameters are within acceptable range
            if (min_ms <= 50.0 && max_ms <= 60.0) {
                params_stable = true;
                ESP_LOGI(TAG, "Accepting parameters within range");
                esp_timer_stop(conn_update_timer);  // Stop retry timer
                return 0;
            }
            return BLE_HS_EREJECT;
        }
            
        case BLE_GAP_EVENT_CONN_UPDATE:
            if (event->conn_update.status == 0) {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
                    float interval_ms = desc.conn_itvl * 1.25;
                    ESP_LOGI(TAG, "Connection parameters updated: interval=%.2f ms, latency=%d, supervision=%d ms",
                            interval_ms, desc.conn_latency, desc.supervision_timeout * 10);
                    
                    if (interval_ms <= 50.0) {
                        params_stable = true;
                        esp_timer_stop(conn_update_timer);  // Stop retry timer
                        ESP_LOGI(TAG, "Parameters stabilized at %.2f ms", interval_ms);
                    } else if (!params_stable && conn_update_retry_count < MAX_CONN_UPDATE_RETRIES) {
                        // Schedule next retry
                        esp_timer_start_once(conn_update_timer, CONN_UPDATE_RETRY_DELAY_MS * 1000);
                    }
                }
            } else {
                ESP_LOGW(TAG, "Connection update failed with status: %d", 
                         event->conn_update.status);
                if (!params_stable && conn_update_retry_count < MAX_CONN_UPDATE_RETRIES) {
                    // Schedule next retry
                    esp_timer_start_once(conn_update_timer, CONN_UPDATE_RETRY_DELAY_MS * 1000);
                }
            }
            break;
            
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected");
            params_stable = false;
            conn_update_retry_count = 0;
            esp_timer_stop(conn_update_timer);  // Stop retry timer
            current_conn_handle = 0;
            ble_app_advertise();
            break;
            
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
            break;
    }
    return 0;
}

// Add these utility functions
static uint32_t get_timestamp_ms(void) {
    return esp_timer_get_time() / 1000;
}

static void encode_timestamp(uint8_t *packet, uint32_t timestamp) {
    uint8_t timestamp_high = (timestamp >> 7) & 0x3F;
    uint8_t timestamp_low = timestamp & 0x7F;
    packet[0] = 0x80 | timestamp_high;  // Header byte
    packet[1] = 0x80 | timestamp_low;   // Timestamp byte
}

static size_t encode_midi_message(uint8_t *buffer, uint8_t status, uint8_t data1, uint8_t data2) {
    uint32_t timestamp = get_timestamp_ms() & 0x1FFF; // 13-bit timestamp
    encode_timestamp(buffer, timestamp);
    buffer[2] = status;
    buffer[3] = data1;
    buffer[4] = data2;
    return 5;
}

// Function to initialize I2S
static void init_i2s(void) {
    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = SAMPLE_BITS,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_LRCK_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM, &pin_config));
}




static void ble_app_on_sync(void) {
    ble_hs_cfg.sync_cb = NULL;
    ble_svc_gap_device_name_set(DEVICE_NAME);
    
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    
    memset(&fields, 0, sizeof(fields));
    memset(&adv_params, 0, sizeof(adv_params));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &midi_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                      &adv_params, gap_event, NULL);
    
    ESP_LOGI(TAG, "Started advertising");
}

// Add these definitions at the top with other #defines
#define BT_HOST_TASK_STACK_SIZE 4096
#define BT_HOST_TASK_PRIO       5
#define BT_HOST_TASK_CORE       1  // Run BT host task on core 0

// Add these definitions at the top
#define WIFI_MAXIMUM_RETRY 5
#define WIFI_RETRY_BASE_DELAY_MS 1000

static int wifi_retry_count = 0;
static bool wifi_connected = false;

static void host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started on core %d", xPortGetCoreID());
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// WiFi credentials
#define WIFI_SSID "Sherif-Home-2.4_EXT"
#define WIFI_PASS "20268575716115134561"

// HTML page with WebSocket client
static const char *html_page = "\
<!DOCTYPE html>\
<html>\
<head>\
    <title>ESP32 MIDI Logger</title>\
    <style>\
        body { font-family: Arial, sans-serif; margin: 20px; }\
        #log { background: #f0f0f0; padding: 10px; height: 400px; overflow-y: scroll; }\
    </style>\
</head>\
<body>\
    <h1>ESP32 MIDI Logger</h1>\
    <div id='log'></div>\
    <script>\
        var log = document.getElementById('log');\
        function fetchLogs() {\
            fetch('/logs')\
                .then(response => response.text())\
                .then(data => {\
                    if (data) {\
                        log.innerHTML += data + '<br>';\
                        log.scrollTop = log.scrollHeight;\
                    }\
                });\
            setTimeout(fetchLogs, 1000);\
        }\
        fetchLogs();\
    </script>\
</body>\
</html>";

// HTTP GET handler for main page
static esp_err_t get_handler(httpd_req_t *req) {
    httpd_resp_send(req, html_page, strlen(html_page));
    return ESP_OK;
}

// HTTP GET handler for logs
static esp_err_t logs_handler(httpd_req_t *req) {
    if (new_message) {
        httpd_resp_send(req, latest_midi_msg, strlen(latest_midi_msg));
        new_message = false;
    } else {
        httpd_resp_send(req, "", 0);
    }
    return ESP_OK;
}

// Start web server
static void start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // URI handler for root page
        httpd_uri_t uri_get = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get);

        // URI handler for logs
        httpd_uri_t uri_logs = {
            .uri = "/logs",
            .method = HTTP_GET,
            .handler = logs_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_logs);
    }
}

// Update the WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (wifi_retry_count < WIFI_MAXIMUM_RETRY && !wifi_connected) {
                    int delay_ms = WIFI_RETRY_BASE_DELAY_MS * (1 << wifi_retry_count);
                    ESP_LOGI(TAG, "WiFi connection failed, retrying in %d ms... (%d/%d)",
                            delay_ms, wifi_retry_count + 1, WIFI_MAXIMUM_RETRY);
                    vTaskDelay(pdMS_TO_TICKS(delay_ms));
                    esp_wifi_connect();
                    wifi_retry_count++;
                } else if (!wifi_connected) {
                    ESP_LOGE(TAG, "WiFi connection failed after %d attempts", WIFI_MAXIMUM_RETRY);
                } else {
                    ESP_LOGI(TAG, "WiFi disconnected, attempting reconnection...");
                    esp_wifi_connect();
                }
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry_count = 0;
        wifi_connected = true;
        start_webserver();
    }
}

// Update WiFi initialization
static void init_wifi(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    ESP_LOGI(TAG, "Connecting to WiFi network: %s", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_retry_count = 0;
    wifi_connected = false;
}

void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize I2S
    init_i2s();
    ESP_LOGI(TAG, "I2S initialized");

    // Initialize WiFi
    init_wifi();

    init_conn_update_timer();

    // Initialize BLE
    ESP_ERROR_CHECK(nimble_port_init());

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_hs_cfg.sync_cb = ble_app_on_sync;
    ble_hs_cfg.reset_cb = NULL;
    ble_hs_cfg.store_status_cb = NULL;
    ble_hs_cfg.gatts_register_cb = NULL;

    // Register the gap event handler
    rc = ble_gap_event_listener_register(&gap_event_listener, gap_event, NULL);
    assert(rc == 0);

    // Create the host task with specific core affinity
    xTaskCreatePinnedToCore(host_task, "host_task", BT_HOST_TASK_STACK_SIZE,
                           NULL, BT_HOST_TASK_PRIO, NULL, BT_HOST_TASK_CORE);

    // Initialize MIDI state
    init_midi_state(&midi_state);
}