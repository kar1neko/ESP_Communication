#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <string>
#include "driver/gpio.h"
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
#include "common.h"
#include "soc/gpio_num.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

/*TODO debug向けに作成
 * recv側のログを送信側に転送
 * rssiの値、recv, sendした番号をsenderに送信
 *
 */

static const char *MAC_ADDRESS = "SEND_MAC_ADDRESS";
static const char *LOG = "SEND_LOG";
static const char *r_LOG = "recv_LOG";
static uint8_t receiver_mac[6] = {0x04, 0x83, 0x08, 0x0E, 0x53, 0x04}; // 受信側のMac Address


#define CONTROL_PIN GPIO_NUM_4

// // 関数定義
// void init_NVS();
// void init_wifi();
// static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status);
// void print_macAddress();
// void on_log_recv();
// uint32_t get_millis();

inline uint32_t get_millis() {
    return esp_timer_get_time() / 1000;
}

void print_macAddress() {
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
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE)); // チャンネルを明示的に固定
}

static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(LOG, "send success!");
    } else {
        ESP_LOGE(LOG, "send fail");
    }
}

static bool recv_hbcb = false;
static portMUX_TYPE recv_hbcb_lock = portMUX_INITIALIZER_UNLOCKED;

static void on_log_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (recv_info == nullptr || recv_info->src_addr == nullptr || len < 0 || (len > 0 && data == nullptr)) {
        ESP_LOGE(r_LOG, "invalid ESP-NOW receive arguments");
        return;
    }
    // receiver以外からのパケットは無視する
    if (memcmp(recv_info->src_addr, receiver_mac, ESP_NOW_ETH_ALEN) != 0) {
        ESP_LOGW(r_LOG, "ignoring packet from unexpected sender " MACSTR, MAC2STR(recv_info->src_addr));
        return;
    }
    if (len >= 6 && memcmp(data, "[info]", 6) == 0) {
        ESP_LOGI(r_LOG, "%.*s", len, (const char *)data);
        // ESP_LOGI(LOG, "through point1!");
    } else if (len >= 6 && memcmp(data, "[Eror]", 6) == 0) {
        ESP_LOGE(r_LOG, "%.*s", len, (const char *)data);
        // ESP_LOGI(LOG, "through point2!");
    } else if (len >= 8 && memcmp(data, "[hb][cb]", 8) == 0) {
        portENTER_CRITICAL(&recv_hbcb_lock);
        recv_hbcb = true;
        portEXIT_CRITICAL(&recv_hbcb_lock);
    }
}

// [hb]を送信する。成功したらtrue
static bool send_hb(struct_t *data) {
    snprintf(data->message, sizeof(data->message), "[hb]");
    esp_err_t res = esp_now_send(receiver_mac, (const uint8_t *)data, sizeof(*data));
    if (res != ESP_OK) {
        ESP_LOGE(r_LOG, "send fail: %d", res);
        return false;
    }
    return true;
}

// [hb][cb]受信フラグを取得してクリアする(受信していればtrue)
static bool take_hbcb() {
    bool value;
    portENTER_CRITICAL(&recv_hbcb_lock);
    value = recv_hbcb;
    recv_hbcb = false;
    portEXIT_CRITICAL(&recv_hbcb_lock);
    return value;
}

// TODO levelのHIGH/LOW指示 sedner -> reciver
// main
extern "C" void app_main() {
    init_NVS();
    init_wifi();
    ESP_ERROR_CHECK(esp_now_init()); // initialize esp-now
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    print_macAddress();

    // 受信側のesp32を登録
    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, receiver_mac, ESP_NOW_ETH_ALEN);
    peer_info.channel = 1; // channel
    peer_info.encrypt = false;
    peer_info.ifidx = WIFI_IF_STA;
    ESP_ERROR_CHECK(esp_now_add_peer(&peer_info));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_log_recv));

    struct_t send_data = {};
    send_data.sensor_id = 101;
    int level = 0;
    // std::string s = "natinal institute of technology asahikawa-collage";
    uint32_t success_prev_time = 0, sent_hb_time = 0;

    //TODO level指示をsender側から行う
    typedef enum {
        STATE_SYNC,
        STATE_NORMAL,
        STATE_ERR
    } state_t;

    state_t cur_state = STATE_SYNC;

    // loop
    while (1) {

        switch (cur_state) {
            //FIXME 遷移条件確認
            case STATE_SYNC: {
                ESP_LOGI(LOG, "sync timer");
                // receiverから最初の[hb][cb]が返ってくるまで、300msごとに[hb]を送り続ける
                if (get_millis() - sent_hb_time >= 300) {
                    if (send_hb(&send_data)) {
                        sent_hb_time = get_millis();
                    }
                }
                if (take_hbcb()) {
                    ESP_LOGI(LOG, "sync successfully");
                    success_prev_time = get_millis();
                    cur_state = STATE_NORMAL; // normalに遷移
                }
                break;
            }

            //TODO err遷移とsenderからrecieverのHIGH/LOwを組む
            case STATE_NORMAL: {
                ESP_LOGI(LOG, "status: normal");
                if (get_millis() - sent_hb_time >= 300) { // 現在値がprev_timeから300ms経過した時
                    if (send_hb(&send_data)) {
                        sent_hb_time = get_millis();
                    }
                }

                if (take_hbcb()) {
                    success_prev_time = get_millis();
                }

                if (get_millis() - success_prev_time >= 1500) {
                    ESP_LOGE("STATE_LOG", "error! disconnected recv");
                    cur_state = STATE_ERR;
                }

                break;
            }

            // FIXME この状態ではsender側はerrに遷移しない
            case STATE_ERR: {
                ESP_LOGE(LOG, "error occurred!");

                break;
            }
        }

        // snprintf(send_data.message, sizeof(send_data.message), "%d", cnt);
        // esp_err_t result = esp_now_send(receiver_mac, (const uint8_t *)&send_data, sizeof(send_data));
        // if (result == ESP_OK) {
        //     ESP_LOGI(LOG, "Send- ID: %d, message: %s", send_data.sensor_id, send_data.message);
        // } else {
        //     ESP_LOGE(LOG, "semd err %s", esp_err_to_name(result));
        // }
        // cnt = (cnt + 1) % 10;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}