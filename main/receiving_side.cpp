#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
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
#include "hal/gpio_types.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include "common.h"
#include "soc/gpio_num.h"

static const char *MAC_ADDRESS = "REC_MAC_ADDRESS";
static const char *RECEIVE_CALLBACK = "REC_CB";
static const char *RECV = "RECEVE_SIDE";

#define CONTROL_PIN GPIO_NUM_4

// 関数定義
void init_NVS();
void init_wifi();
void print_mac();

// 送信側へログを送信
void send_err_to_sender(const uint8_t *dest_mac, const char *err_msg) { // dest_macに送信側のmac_addressを格納
    if (!esp_now_is_peer_exist(dest_mac)) { // 送信機がピア登録されていなければ登録する(基本初回のみ)
        esp_now_peer_info peer_info = {};
        memcpy(peer_info.peer_addr, dest_mac, ESP_NOW_ETH_ALEN);
        peer_info.channel = 0;
        peer_info.encrypt = false;
        esp_now_add_peer(&peer_info);
    }
    esp_now_send(dest_mac, (const uint8_t*)err_msg, strlen(err_msg));
}

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
    if (len == sizeof(struct_t)) {
        const struct_t *recv_data = (const struct_t *)data; // 生データを構造体の型にキャスト

        // アロー演算子でメンバ変数にアクセスして出力
        ESP_LOGI(RECEIVE_CALLBACK, "Sensor ID: %d", recv_data->sensor_id);
        ESP_LOGI(RECEIVE_CALLBACK, "message: %s", recv_data->message);
    } else {
        ESP_LOGE(RECEIVE_CALLBACK, "receive data are missmatch!");

        // UCCのコーヒー不味すぎる
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "[RX Err] Data size missmatch. len: %d bytes", len);
        send_err_to_sender(recv_info->src_addr, err_msg); // 送信側(src_addr)にerr_msgを送信
    }
}

// print MacAddress
void print_mac() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(MAC_ADDRESS, "MAC Address: " MACSTR, MAC2STR(mac));
}

// initialize NVS
//FIXME: successとfailの関係がおかしい
void init_NVS() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        ESP_LOGI(RECV, "initialize NVS Successfully!");
    } else {
        ESP_LOGE(RECV, "initialize NVS failed: %s", esp_err_to_name(ret));
        // abort(); // 強制終了
    }
}

// initialize wifi
void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(RECV, "Starting wifi...");
}

// main
extern "C" void app_main() {
    init_NVS();
    init_wifi();
    print_mac();
    ESP_ERROR_CHECK(esp_now_init()); // initialize esp-now

    // CB登録
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // GPIOピンの初期化
    gpio_reset_pin(CONTROL_PIN);
    gpio_set_direction(CONTROL_PIN, GPIO_MODE_OUTPUT);

    // HIGH/LOWのセット
    int level = 0;
    
    // loop
    while (1) {
        gpio_set_level(CONTROL_PIN, level);
        level = !level;
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}