#include "esp_log.h"
#include "nvs_flash.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define DEVICE_NAME "ESP32 MIDI"
#define TAG "BLE_MIDI"

// Define MIDI service and characteristic UUIDs
static const ble_uuid128_t midi_service_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03
);

static const ble_uuid128_t midi_characteristic_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77
);

static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

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

static int midi_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
        case BLE_GATT_ACCESS_OP_READ_CHR:
            return 0;

        case BLE_GATT_ACCESS_OP_WRITE_CHR:
            if (ctxt->om->om_len > 0) {
                uint8_t *midi_data = OS_MBUF_DATA(ctxt->om, uint8_t *);
                
                ESP_LOGI(TAG, "Received MIDI data of length: %d", ctxt->om->om_len);
                for(int i = 0; i < ctxt->om->om_len; i++) {
                    ESP_LOGI(TAG, "Byte %d: 0x%02x", i, midi_data[i]);
                }

                if (ctxt->om->om_len >= 5) {
                    uint8_t status = midi_data[2];
                    uint8_t data1 = midi_data[3];
                    uint8_t data2 = midi_data[4];

                    switch(status & 0xF0) {
                        case 0x90:
                            ESP_LOGI(TAG, "Note On - Note: %d, Velocity: %d", data1, data2);
                            break;
                        case 0x80:
                            ESP_LOGI(TAG, "Note Off - Note: %d, Velocity: %d", data1, data2);
                            break;
                        case 0xB0:
                            ESP_LOGI(TAG, "Control Change - Controller: %d, Value: %d", data1, data2);
                            break;
                        default:
                            ESP_LOGI(TAG, "Other MIDI message - Status: 0x%02x", status);
                            break;
                    }
                }
            }
            return 0;

        default:
            return BLE_ATT_ERR_UNLIKELY;
    }
}

static void ble_app_on_sync(void) {
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