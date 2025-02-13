#include "esp_log.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

// Add this near the top of the file, after the includes and before any function definitions
static void ble_app_advertise(void);

#define DEVICE_NAME "Sherif-Midi"
#define TAG "BLE_MIDI"

// Add these definitions at the top
#define MIN_CONN_INTERVAL               15  // 15ms as per spec
#define MAX_CONN_INTERVAL               15  // Keep same as min for initial negotiation
#define SLAVE_LATENCY                   0
#define SUPERVISION_TIMEOUT             400 // Standard value

// Update the MIDI service and characteristic UUIDs to match the standard specification
static const ble_uuid128_t midi_service_uuid =
    BLE_UUID128_INIT(0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7, 
                     0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);

static const ble_uuid128_t midi_characteristic_uuid =
    BLE_UUID128_INIT(0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1, 
                     0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

static uint16_t midi_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t midi_attr_handle = 0;
static uint16_t midi_subscription_handle;

static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

// Update the GATT service definitions with proper descriptors and flags
static struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &midi_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &midi_characteristic_uuid.u,
                .access_cb = midi_chr_access,
                .flags = BLE_GATT_CHR_F_READ | 
                         BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP |  // Required by spec
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &midi_attr_handle,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16),
                        .access_cb = midi_chr_access,
                        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_WRITE,
                    },
                    {
                        0,
                    }
                },
            },
            {
                0,
            }
        },
    },
    {
        0,
    },
};

static int send_midi_notification(const uint8_t *midi_data, size_t length) {
    struct os_mbuf *om;
    if (midi_connection_handle == BLE_HS_CONN_HANDLE_NONE) {
        return BLE_HS_ENOTCONN;
    }
    
    // Ensure we have the BLE-MIDI header
    if (length < 2 || (midi_data[0] & 0x80) == 0 || (midi_data[1] & 0x80) == 0) {
        // If the data doesn't include a proper header, add one
        uint8_t *new_data = malloc(length + 2);
        if (new_data == NULL) {
            return BLE_HS_ENOMEM;
        }
        new_data[0] = 0x80;  // Timestamp high byte
        new_data[1] = 0x80;  // Timestamp low byte
        memcpy(new_data + 2, midi_data, length);
        
        om = ble_hs_mbuf_from_flat(new_data, length + 2);
        free(new_data);
    } else {
        om = ble_hs_mbuf_from_flat(midi_data, length);
    }
    
    if (om == NULL) {
        return BLE_HS_ENOMEM;
    }

    return ble_gattc_notify_custom(midi_connection_handle, midi_attr_handle, om);
}

static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            // Respond with no payload as required by spec
            return 0;

        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            if (ctxt->om->om_len > 0) {
                uint8_t *midi_data = OS_MBUF_DATA(ctxt->om, uint8_t *);
                
                // Verify header byte (bit 7 must be set)
                if ((midi_data[0] & 0x80) == 0) {
                    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
                }

                // Process timestamp and MIDI data
                uint8_t timestamp_high = midi_data[0] & 0x3F;  // bits 5-0
                uint8_t timestamp_low = midi_data[1] & 0x7F;   // bits 6-0
                uint16_t timestamp = (timestamp_high << 7) | timestamp_low;

                ESP_LOGI(TAG, "Received MIDI data, timestamp: %d", timestamp);
                
                // Process MIDI messages starting from byte 2
                for (int i = 2; i < ctxt->om->om_len; i++) {
                    if (midi_data[i] & 0x80) {  // Status byte
                        ESP_LOGI(TAG, "MIDI Status: 0x%02x", midi_data[i]);
                    }
                }
            }
            return 0;
        }

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    struct ble_gap_upd_params params;  // Changed from conn_params to upd_params
    
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                midi_connection_handle = event->connect.conn_handle;
                ESP_LOGI(TAG, "Connection established");

                // Request connection parameters update
                params.itvl_min = BLE_GAP_CONN_ITVL_MS(MIN_CONN_INTERVAL);
                params.itvl_max = BLE_GAP_CONN_ITVL_MS(MAX_CONN_INTERVAL);
                params.latency = SLAVE_LATENCY;
                params.supervision_timeout = SUPERVISION_TIMEOUT;

                ble_gap_update_params(event->connect.conn_handle, &params);
                
                // Request MTU update to maximum supported
                ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
            } else {
                midi_connection_handle = BLE_HS_CONN_HANDLE_NONE;
                ble_app_advertise();
            }
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            midi_connection_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGI(TAG, "Disconnected");
            ble_app_advertise();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(TAG, "Subscribe event; cur_notify=%d, cur_indicate=%d",
                    event->subscribe.cur_notify, event->subscribe.cur_indicate);
            if (event->subscribe.attr_handle == midi_attr_handle) {
                ESP_LOGI(TAG, "MIDI notifications %s",
                    event->subscribe.cur_notify ? "enabled" : "disabled");
            }
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU Update Event - New MTU: %d", event->mtu.value);
            break;
    }
    return 0;
}

static void ble_app_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    
    memset(&fields, 0, sizeof(fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    memset(&adv_params, 0, sizeof(adv_params));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    
    // Advertise the 128-bit MIDI service UUID
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
                      &adv_params, ble_gap_event, NULL);

    ESP_LOGI(TAG, "Started advertising");
}

static void ble_app_on_sync(void) {
    int rc;
    
    // Get the handle for our characteristic
    rc = ble_gatts_find_chr(&midi_service_uuid.u,
                           &midi_characteristic_uuid.u,
                           NULL, &midi_attr_handle);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to find characteristic handle");
        return;
    }
    
    ble_app_advertise();
}

static void host_task(void *param) {
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nimble_port_init());

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    assert(rc == 0);

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_hs_cfg.sync_cb = ble_app_on_sync;

    nimble_port_freertos_init(host_task);
}