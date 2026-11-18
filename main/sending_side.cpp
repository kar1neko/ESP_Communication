#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdlib.h>
#include <string.h>
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
#include "portmacro.h"
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
static uint8_t receiver_mac[6] = {
    0x10, 0xbd, 0xa3, 0x9d, 0x8c, 0x54
}; // 受信側のMac Address

static volatile int8_t remote_rssi = 0; // 受信側が測定したrssi値 sender -> receiver 
static volatile int8_t local_rssi = 0; // 送信側が測定したrssi値 receiver -> sender

#define CONTROL_PIN GPIO_NUM_4
#define CSV_OUTPUT GPIO_NUM_20 // D9 GPIO20 SPI_MISO
#define LOSS_LOG_MAX 128

// // 関数定義
// void init_NVS();
// void init_wifi();
// static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status);
// void print_macAddress();
// void on_log_recv();
// uint32_t get_millis();

struct loss_log_t {
    uint32_t t_loss_ms;
    uint32_t t_loss_num;
    int8_t t_remote_rssi;
    int8_t t_local_rssi;
};
static loss_log_t loss_log[LOSS_LOG_MAX];
static uint32_t loss_total = 0;
static uint32_t send_total = 0;
static portMUX_TYPE loss_log_lock = portMUX_INITIALIZER_UNLOCKED;

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

int packet_lost_cnt = 0;

static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    portENTER_CRITICAL(&loss_log_lock); // 鍵解除
    send_total++;
    if (status != ESP_NOW_SEND_SUCCESS) {
        loss_log_t &e = loss_log[loss_total % LOSS_LOG_MAX];
        e.t_loss_ms = get_millis();
        e.t_remote_rssi = remote_rssi;
        e.t_local_rssi = local_rssi;
        loss_total++;
    }
    portEXIT_CRITICAL(&loss_log_lock);
    if (status == ESP_NOW_SEND_SUCCESS) {
        ESP_LOGI(LOG, "send success!");
    } else {
        ESP_LOGE(LOG, "send fail");
        packet_lost_cnt++;
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
    } else if (len >= 8 && memcmp(data, "[hb][cb]", 8) == 0) { // heartbeat判定
        portENTER_CRITICAL(&recv_hbcb_lock);
        recv_hbcb = true;
        portEXIT_CRITICAL(&recv_hbcb_lock);

        if (recv_info->rx_ctrl != nullptr) {
            local_rssi = recv_info->rx_ctrl->rssi; // sender側が測定したrssi値を格納
        }

        char text[32];
        /*
         *  受信データはstrlenで終端の"\0"が無い
         * この状態でatoiなどを実行すると範囲外を読むことがあるため、三項演算子を使って長さの制限を行う
         */
        int n = (size_t)len < sizeof(text) -1 ? (size_t)len : sizeof(text) -1;
        memcpy(text, data, n);
        text[n] = '\0'; // 終端"\0"を追加
        // strstr(const char *, const char *) char1: 検索対象となるメインの文字列 char2: 探したい部分文字列
        const char *p = strstr(text, "rssi:"); // pにtextの中からrssi:から始まるポインタを格納する
        if(p != nullptr) {
            remote_rssi = (int8_t)atoi(p + 7); // 文字列をint8_tに変換。開始ポイントはp + 7byte.
        }
        
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

// パケットロスト記録をCSVでシリアルに出力
static void dump_lossCSV() {
    portENTER_CRITICAL(&loss_log_lock);
    uint32_t total = loss_total;
    uint32_t loss = loss_total;
    uint32_t sent = send_total;
    portEXIT_CRITICAL(&loss_log_lock);

    uint32_t cnt = total < LOSS_LOG_MAX ? total : LOSS_LOG_MAX;
    uint32_t start = total - cnt;

    printf("CSV_BEGIN,send_total=%lu,loss_total=%lu\n", (unsigned long)sent, (unsigned long)total);
    printf("time_ms,loss_num,remote_rssi_dBm,local_rssi_dBm\n");
    for (uint32_t i=0; i < cnt; i++) {
        loss_log_t e;
        portENTER_CRITICAL(&loss_log_lock);
        e = loss_log[(start + i) % LOSS_LOG_MAX];
        portEXIT_CRITICAL(&loss_log_lock);
        printf(
            "%lu,%lu,%d,%d\n",
            (unsigned long)e.t_loss_ms,
            (unsigned long)e.t_loss_num,
            (int)e.t_remote_rssi,
            (int)e.t_local_rssi
        );
    }
    printf("CSV_END\n");
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

    gpio_reset_pin(CSV_OUTPUT);
    gpio_set_direction(CSV_OUTPUT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CSV_OUTPUT, GPIO_PULLDOWN_ONLY);
    int prev_level = gpio_get_level(CSV_OUTPUT);

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

                ESP_LOGI(
                    r_LOG, "RSSI val. remote(send->recv):%d dBm, local(recv->send):%d dBm", 
                    remote_rssi, local_rssi
                );

                if (get_millis() - success_prev_time >= 150000) {
                    ESP_LOGE("STATE_LOG", "error! disconnected recv");
                    cur_state = STATE_ERR;
                }

                break;
            }

            // FIXME この状態ではsender側はerrに遷移しない
            case STATE_ERR: {
                ESP_LOGE(LOG, "error occurred!");
                dump_lossCSV();

                break;
            }
        }

        int level_now = gpio_get_level(CSV_OUTPUT);
        // CSV_OUTPUT押下でcsv output
        if (prev_level == 0 && level_now == 1) {
            dump_lossCSV();
            ESP_LOGI(LOG, "fucking saved!!!!!");
        }
        prev_level = level_now;

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