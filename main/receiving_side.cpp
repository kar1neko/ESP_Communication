#include <cstdarg>
#include <cstddef>
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
#include "freertos/queue.h"
#include "common.h"
#include "soc/gpio_num.h"
#include "cstdarg"
#include "esp_timer.h"

static const char *MAC_ADDRESS = "REC_MAC_ADDRESS";
static const char *RECEIVE_CALLBACK = "REC_CB";
static const char *RECV = "RECEVE_SIDE";
static const char *STATUS = "RECV_STATE";

#define CONTROL_PIN GPIO_NUM_4

inline uint32_t get_millis() {
    return esp_timer_get_time() / 1000;
}

static uint32_t hb_recv_prev_time = 0;
static portMUX_TYPE prev_time_lock = portMUX_INITIALIZER_UNLOCKED; // timelockの定義
static uint8_t sender_mac[ESP_NOW_ETH_ALEN] = {};
static bool is_mac_saved = false;
static portMUX_TYPE sender_mac_lock = portMUX_INITIALIZER_UNLOCKED; // maclockの定義
static QueueHandle_t log_queue = nullptr; // ログ用のキューを初期化

// 送信先とログの型指定
struct pending_log_t {
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];
    char message[128];
};

static bool get_sender_mac(uint8_t *mac) {
    bool saved;
    portENTER_CRITICAL(&sender_mac_lock);
    saved = is_mac_saved;
    if (saved) {
        memcpy(mac, sender_mac, ESP_NOW_ETH_ALEN); // mac[6]に送信先のmacをコピーして保存
    }
    portEXIT_CRITICAL(&sender_mac_lock);
    return saved;
}

static bool is_saved_sender(const uint8_t *mac) {
    bool matches;
    portENTER_CRITICAL(&sender_mac_lock);
    matches = is_mac_saved &&
        memcmp(sender_mac, mac, ESP_NOW_ETH_ALEN) == 0; // sender_macとmacをESP_NOW_ETH_ALEN(6 byte)だけ比較する
    portEXIT_CRITICAL(&sender_mac_lock);
    return matches;
}

// prev_timeのアップデート
static void update_prev_time() {
    portENTER_CRITICAL(&prev_time_lock);
    hb_recv_prev_time = get_millis();
    portEXIT_CRITICAL(&prev_time_lock);
}

// prev_timeの取得
static uint64_t get_prev_time() {
    uint64_t value;
    portENTER_CRITICAL(&prev_time_lock);
    value = hb_recv_prev_time;
    portEXIT_CRITICAL(&prev_time_lock);
    return value;
}

// 送信側へログを送信
// ... は可変長の引数
void send_log_to_sender(const uint8_t *dest_mac, const char* format, ...) { // dest_macに送信側のmac_addressを格納
    if (!esp_now_is_peer_exist(dest_mac)) { // 送信機がピア登録されていなければ登録する(基本初回のみ)
        esp_now_peer_info_t peer_info = {};
        memcpy(peer_info.peer_addr, dest_mac, ESP_NOW_ETH_ALEN);
        peer_info.channel = 1;
        peer_info.encrypt = false;
        peer_info.ifidx = WIFI_IF_STA;
        esp_err_t result = esp_now_add_peer(&peer_info);
        if (result != ESP_OK && result != ESP_ERR_ESPNOW_EXIST) {
            ESP_LOGE(RECEIVE_CALLBACK, "failed to add sender peer: %s",
                esp_err_to_name(result));
            return;
        }
    }

    char buffer[128];
    va_list args; // 引数が可変長なので、まとめて引数をリストで定義する
    va_start(args, format); // formatから後に続く引数をargsに取り込む
    vsnprintf(buffer, sizeof(buffer), format, args); // bufferに文字列を書き込む
    va_end(args); // 引数リストの処理を終了する
    esp_err_t result = esp_now_send(dest_mac, (const uint8_t*)buffer, strlen(buffer)); // 送信
    if (result != ESP_OK) {
        ESP_LOGE(RECEIVE_CALLBACK, "failed to send log to sender: %s",
            esp_err_to_name(result));
    }
}

/*
 * -------------------------------------------------------
 * esp_now_recv_info_t
 * ・src_addr: 送信元のMAC-Address(uint8_t *)
 * ・des_addr: 宛先のMAC-Address(uint8_t *)
 * ・rx_ctrl: 受信時のWi-Fi制御情報(wifi_pkt_rx_ctrl_t *)
 *  -> rssiや使用channelが格納
 * -------------------------------------------------------
 */

// receive cb
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) { // recv_info -> 送信元のMacAddress、RSSIなどが格納された構造体のポインタ
    if (recv_info == nullptr || recv_info->src_addr == nullptr || len < 0 ||
        (len > 0 && data == nullptr)) {
        ESP_LOGE(RECEIVE_CALLBACK, "invalid ESP-NOW receive arguments");
        return;
    }

    // 受信サイズが構造体のサイズと一致しているかチェック
    if (len == sizeof(struct_t)) {
        portENTER_CRITICAL(&sender_mac_lock);
        if (!is_mac_saved) {
            memcpy(sender_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
            is_mac_saved = true;
        }
        bool accepted_sender =
            memcmp(sender_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN) == 0;
        portEXIT_CRITICAL(&sender_mac_lock);

        // src_addrとESP_NOW_ETH_ALENが一致しないので正常にメッセージが送れない可能性がある
        if (!accepted_sender) {
            ESP_LOGW(RECEIVE_CALLBACK, "ignoring packet from unexpected sender");
            return;
        }
        struct_t recv_data;
        // memcpy(&recv_data, data, sizeof(recv_data));

        size_t message_len = strnlen(recv_data.message, sizeof(recv_data.message));

        // HB受信判定
        if (message_len >= 4 && memcmp(recv_data.message, "[hb]", 4) == 0) {
            update_prev_time();
        }

    } else {
        ESP_LOGE(RECEIVE_CALLBACK, "receive data are missmatch");

        // is_saved_senderとlog_queueがnullptrでなければtrue
        if (is_saved_sender(recv_info->src_addr) && log_queue != nullptr) {
            pending_log_t pending = {};
            memcpy(pending.dest_mac, recv_info->src_addr, ESP_NOW_ETH_ALEN);
            snprintf(pending.message, sizeof(pending.message),
                "[Eror] data size missmatch: len: %d bytes", len);
            if (xQueueSend(log_queue, &pending, 0) != pdPASS) {
                ESP_LOGE(RECEIVE_CALLBACK, "failed to queue receive error log");
            }
        }
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)); // チャンネルを明示的に固定
    ESP_LOGI(RECV, "Starting wifi...");
}

// main
extern "C" void app_main() {
    init_NVS();
    init_wifi();
    print_mac();
    ESP_ERROR_CHECK(esp_now_init()); // initialize esp-now
    log_queue = xQueueCreate(4, sizeof(pending_log_t));
    ESP_ERROR_CHECK(log_queue == nullptr ? ESP_ERR_NO_MEM : ESP_OK);

    // CB登録
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // GPIOピンの初期化
    gpio_reset_pin(CONTROL_PIN);
    gpio_set_direction(CONTROL_PIN, GPIO_MODE_OUTPUT);

    // const uint8_t *data; // 生データの定義
    // const struct_t *recv_data = reinterpret_cast<const struct_t*>(data); // キャスト
    // esp_now_recv_info *recv_data;
    int level = 1; // HIGH/LOWの定義

    typedef enum {
        STATE_SYNC,
        STATE_NORMAL,
        STATE_ERR
    } state_t;

    state_t cur_state = STATE_SYNC;

    // loop
    while (1) {
        switch (cur_state) {
            case STATE_SYNC: {
                ESP_LOGI(STATUS, "STATE_CYNC");
                uint8_t current_sender_mac[ESP_NOW_ETH_ALEN];
                if(get_sender_mac(current_sender_mac)) {
                    ESP_LOGI(STATUS, "sync successfully");
                    update_prev_time();
                    cur_state = STATE_NORMAL; // NORMALへ遷移
                } else {
                    ESP_LOGE(STATUS, "sync unsuccessful");
                    ESP_LOGE(STATUS, "sender MAC has not been received yet");
                }
                break;
            }

            case STATE_NORMAL: {
                ESP_LOGI(STATUS, "STATE_NORMAL");
                gpio_set_level(CONTROL_PIN, 0);
                if (get_millis() - get_prev_time() >= 1500) {
                    ESP_LOGE(RECV, "fail to receive HB.");
                    cur_state = STATE_ERR; // ERRへ遷移
                } else {
                    uint8_t current_sender_mac[ESP_NOW_ETH_ALEN];
                    if (!get_sender_mac(current_sender_mac)) {
                        ESP_LOGE(RECV, "sender MAC is unavailable");
                        break;
                    }
                    pending_log_t pending;
                    while (xQueueReceive(log_queue, &pending, 0) == pdPASS) {
                        send_log_to_sender(pending.dest_mac, "%s", pending.message);
                    }
                    send_log_to_sender(current_sender_mac, "[hb][cb]");
                    if  (level == 1) {
                        ESP_LOGI(RECV, "GPIO_PIN_4 is HIGH");
                        send_log_to_sender(current_sender_mac, "[info] GPIO_PIN_4 is HIGH");
                    } else {
                        ESP_LOGI(RECV, "GPIO_PIN_4 is LOW");
                        send_log_to_sender(current_sender_mac, "[info] GPIO_PIN_4 is LOW");
                    }
                    level = !level; // debug

                }
                break;
            }

            case STATE_ERR: {
                gpio_set_level(CONTROL_PIN, 0);
                ESP_LOGE(STATUS, "error: connection lost");
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}