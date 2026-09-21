#include <cstdint>
#include <cstdio>
#include <cstring>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "common.h"

static const char *MAC_ADDRESS = "REC_MAC_ADDRESS";
static const char *RECEIVE_CALLBACK = "REC_CB";

// 関数定義
void init_NVS();
void init_wifi();
void print_mac();

// receive cb
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) { // recv_info -> 送信元のMacAddress、RSSIなどが格納された構造体のポインタ
    if (recv_info->rx_ctrl != nullptr) {
        int rssi = recv_info->rx_ctrl->rssi;
        ESP_LOGI(RECEIVE_CALLBACK, "Received!: %d bytes(RSSI: %d dBm) from MAC: %02X:%02X:%02X:%02X:%02X:%02X",
            len,
            rssi,
            recv_info->src_addr[0],
            recv_info->src_addr[1],
            recv_info->src_addr[2],
            recv_info->src_addr[3],
            recv_info->src_addr[4],
            recv_info->src_addr[5] // 送信元のMacAddressを格納
        );
    }

    // 受信サイズが構造体のサイズと一致しているかチェック
    if (len == sizeof(struct_t) && result == ESP_OK) {
        const struct_t *recv_data = (const struct_t *)data; // 生データを構造体の型にキャスト

        // アロー演算子でメンバ変数にアクセスして出力
        ESP_LOGI(RECEIVE_CALLBACK, "Sensor ID: %d", recv_data->sensor_id);
        ESP_LOGI(RECEIVE_CALLBACK, "message: %s", recv_data->message);
    } else {
        ESP_LOGE(RECEIVE_CALLBACK, "receive data are missmatch!");
    }
}

// print MacAddress
void print_mac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(MAC_ADDRESS, "MAC Address: " MACSTR, MAC2STR(mac));
}

// initialize NVS
void init_NVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

// initialize wifi
void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

extern "C" void app_main() {
    init_NVS();
    init_wifi();
    print_mac();
    ESP_ERROR_CHECK(esp_now_init()); // initialize esp-now

    // CB登録
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // loop
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}