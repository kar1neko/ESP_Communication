#include <cstdint>
#include <cstdio>
#include <cstring>
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

/*
 * FIXME recv_logとsend_logが同じログ出してる
 * E (13168) RECV_LOG: [info] GPIO_PIN_4 is HIGH
 * I (13168) SEND_LOG: [info] GPIO_PIN_4 is HIGH
 */

static const char *MAC_ADDRESS = "SEND_MAC_ADDRESS";
static const char *LOG = "SEND_LOG";
static const char *r_LOG = "recv_LOG";
static uint8_t receiver_mac[6] = {0x98, 0xA3, 0x16, 0x8F, 0xB6, 0x0C}; // 受信側のMac Address

// 関数定義
void init_NVS();
void init_wifi();
static void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status);
void print_macAddress();
void on_log_recv();

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

static void on_log_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    ESP_LOGE("RECV_LOG", "%.*s", len, (const char* )data);
    if (len >= 6 && memcmp(data, "[info]", 6) == 0) {
        ESP_LOGI(LOG, "%.*s", len, (const char *)data);
    } else if (len >= 6 && memcmp(data, "[Eror]", 6) == 0) {
        ESP_LOGE(r_LOG, "%.*s", len, (const char *)data);
    }
}

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
    int cnt = 0;
    // std::string s = "natinal institute of technology asahikawa-collage";
    
    // loop
    while (1) {
        snprintf(send_data.message, sizeof(send_data.message), "%d", cnt);
        esp_err_t result = esp_now_send(receiver_mac, (const uint8_t *)&send_data, sizeof(send_data));
        if (result == ESP_OK) {
            ESP_LOGI(LOG, "Send- ID: %d, message: %s", send_data.sensor_id, send_data.message);
        } else {
            ESP_LOGE(LOG, "semd err %s", esp_err_to_name(result));
        }
        cnt = (cnt + 1) % 10;
        
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}