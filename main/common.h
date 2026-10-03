#pragma once
#include <cstdint>

// 構造体の登録
typedef struct {
    int sensor_id;
    char pin_level[8];
    char message[32]; // default: 32    
} struct_t;

typedef struct {
    int status_code; // 0: OK, -1: サイズ不一致, 999: その他
    char log_msg[64];
}response_t;