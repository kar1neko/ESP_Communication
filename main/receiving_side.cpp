#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <sys/types.h>
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
#include "cstdarg"

static const char *MAC_ADDRESS = "REC_MAC_ADDRESS";
static const char *RECEIVE_CALLBACK = "REC_CB";
static const char *RECV = "RECEVE_SIDE";

#define CONTROL_PIN GPIO_NUM_4

// 関数定義
void init_NVS();
void init_wifi();
void print_mac();
void send_log_to_sender();

// 送信側へログを送信
// ... は可変長の引数
void send_log_to_sender(const uint8_t *dest_mac, const char* format, ...) { // dest_macに送信側のmac_addressを格納
    if (!esp_now_is_peer_exist(dest_mac)) { // 送信機がピア登録されていなければ登録する(基本初回のみ)
        esp_now_peer_info peer_info = {};
        memcpy(peer_info.peer_addr, dest_mac, ESP_NOW_ETH_ALEN);
        peer_info.channel = 1;
        peer_info.encrypt = false;
        peer_info.ifidx = WIFI_IF_STA;
        esp_now_add_peer(&peer_info);
    }

    char buffer[128];
    va_list args; // 引数が可変長なので、まとめて引数をリストで定義する
    va_start(args, format); // formatから後に続く引数をargsに取り込む
    vsnprintf(buffer, sizeof(buffer), format, args); // bufferに文字列を書き込む
    va_end(args); // 引数リストの処理を終了する

    esp_now_send(dest_mac, (const uint8_t*)buffer, strlen(buffer)); // 送信
    
}

/*
 * esp_now_recv_info_t
 * ・src_addr: 送信元のMAC-Address(uint8_t *)
 * ・des_addr: 宛先のMAC-Address(uint8_t *)
 * ・rx_ctrl: 受信時のWi-Fi制御情報(wifi_pkt_rx_ctrl_t *)
 *  -> rssiや使用channelが格納
 */

static uint8_t sender_mac[6]; // 送信先のMac-Addressを保存
static bool Is_mac_saved = false;

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

    memcpy(sender_mac, recv_info->src_addr, 6); // sender_macにsrc_addrを保存
    Is_mac_saved = true;

    // 受信サイズが構造体のサイズと一致しているかチェック
    if (len == sizeof(struct_t)) {
        // 生データを構造体の型にキャスト
        const struct_t *recv_data = reinterpret_cast<const struct_t* >(data);

        // アロー演算子でメンバ変数にアクセスして出力
        ESP_LOGI(RECEIVE_CALLBACK, "Sensor ID: %d", recv_data->sensor_id);
        ESP_LOGI(RECEIVE_CALLBACK, "message: %s", recv_data->message);
    } else {
        ESP_LOGE(RECEIVE_CALLBACK, "receive data are missmatch!");

        // UCCのコーヒー不味すぎる
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "[RX Err] Data size missmatch. len: %d bytes", len);
        send_log_to_sender(recv_info->src_addr, "[Eror] data size missmatch: %s", err_msg); // 送信側(src_addr)にerr_msgを送信
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
    if (ret == ESP_OK){
        ESP_LOGI(RECV, "initialize NVS Successfully!");
    }
    else {
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)); // チャンネルを明示的に固定
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

    // const uint8_t *data; // 生データの定義
    // const struct_t *recv_data = reinterpret_cast<const struct_t*>(data); // キャスト
    // esp_now_recv_info *recv_data;
    int level = 1; // HIGH/LOWの定義
    
    // loop
    while (1) {
        //TODO まず接続確立のためにテストテキストの送信をさせたい
        gpio_set_level(CONTROL_PIN, level);
        level = !level;
        if (level == 1) {
            ESP_LOGI(RECV, "GPIO_PIN_4 is HIGH");
            if (Is_mac_saved) {
            send_log_to_sender(sender_mac, "[info] GPIO_PIN_4 is HIGH");
            } else {
                ESP_LOGE(RECV, "failed to get sender mac");
                ESP_LOGE(RECV, "sender_mac: MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                    sender_mac[0],
                    sender_mac[1],
                    sender_mac[2],
                    sender_mac[3],
                    sender_mac[4],
                    sender_mac[5]
                );
            }
        } else {
            ESP_LOGI(RECV, "GPIO_PIN_4 is LOW");
            if (Is_mac_saved) {
                send_log_to_sender(sender_mac, "[info] GPIO_PIN_4 is LOW");
            } else {
                ESP_LOGE(RECV, "failed to get sender mac");

            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}