// Library standar C untuk input/output dan string
#include <stdio.h>
#include <string.h>

// FreeRTOS library untuk multitasking
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Library untuk kontrol GPIO ESP32
#include "driver/gpio.h"

// Logging untuk debugging
#include "esp_log.h"

// Library client MQTT dari ESP-IDF
#include "mqtt_client.h"

// Library untuk JSON parsing dan pembuatan objek
#include "cJSON.h"

// Library WiFi ESP32
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"

// Library UART (digunakan untuk komunikasi Modbus)
#include "driver/uart.h"

// Logging tag untuk identifikasi log dari modul ini
#define TAG "NODE"

// Pin output untuk mengontrol kipas
#define FAN_GPIO GPIO_NUM_25

// URI alamat MQTT broker lokal
#define MQTT_URI "mqtt://192.168.1.100"

// Konfigurasi UART untuk Modbus RTU
#define MODBUS_UART_PORT UART_NUM_1
#define MODBUS_TXD_PIN GPIO_NUM_17
#define MODBUS_RXD_PIN GPIO_NUM_16
#define MODBUS_RTS_PIN GPIO_NUM_21
#define MODBUS_BAUDRATE 9600

// ID slave Modbus
#define SLAVE_ID 1

// Ukuran buffer UART
#define BUF_SIZE 256

// Handle client MQTT
static esp_mqtt_client_handle_t client;
// Status koneksi MQTT
static bool mqtt_connected = false;

// Suhu dasar (baseline)
float base_temp = 27.0;
// Ambang suhu = 2% lebih tinggi dari suhu dasar
float TempTreshold = base_temp * 1.02;
// Status kipas (0 = mati, 1 = menyala)
int fan_state = 0;

// Fungsi mengontrol kipas berdasarkan suhu
void fan_control(float current_temp) {
    if (current_temp >= TempTreshold && fan_state == 0) {
        gpio_set_level(FAN_GPIO, 1);  // Nyalakan kipas
        fan_state = 1;
    } else if (current_temp < TempTreshold && fan_state == 1) {
        gpio_set_level(FAN_GPIO, 0);  // Matikan kipas
        fan_state = 0;
    }
}

// Fungsi konversi data Modbus 2 register ke float (32-bit)
float modbus_to_float(uint16_t reg_high, uint16_t reg_low) {
    union {
        uint32_t i;
        float f;
    } u;
    u.i = ((uint32_t)reg_high << 16) | reg_low;
    return u.f;
}

// Fungsi perhitungan CRC Modbus RTU
uint16_t modbus_crc(uint8_t *buf, int len) {
    uint16_t crc = 0xFFFF;
    for (int pos = 0; pos < len; pos++) {
        crc ^= buf[pos];
        for (int i = 0; i < 8; i++) {
            if (crc & 1) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// Fungsi membaca 2 register Modbus lalu mengubahnya ke float
float read_modbus_register_float(uint16_t start_reg) {
    uint8_t tx_buf[8], rx_buf[BUF_SIZE];
    uint16_t crc, reg_high, reg_low;

    // Susun perintah Modbus: [ID, FUNC, ADDR_H, ADDR_L, NUM_H, NUM_L]
    tx_buf[0] = SLAVE_ID;
    tx_buf[1] = 0x03;  // Fungsi 03 = Read Holding Registers
    tx_buf[2] = (start_reg >> 8) & 0xFF;
    tx_buf[3] = start_reg & 0xFF;
    tx_buf[4] = 0x00;
    tx_buf[5] = 0x02;  // Baca 2 register (4 byte float)
    
    // Hitung dan tambahkan CRC ke pesan
    crc = modbus_crc(tx_buf, 6);
    tx_buf[6] = crc & 0xFF;
    tx_buf[7] = (crc >> 8) & 0xFF;

    // Kirim perintah dan baca respons dari slave
    uart_flush(MODBUS_UART_PORT);
    uart_write_bytes(MODBUS_UART_PORT, (const char *)tx_buf, 8);
    int len = uart_read_bytes(MODBUS_UART_PORT, rx_buf, BUF_SIZE, pdMS_TO_TICKS(100));

    // Jika respons valid, ekstrak data dan konversi ke float
    if (len >= 7) {
        reg_high = ((uint16_t)rx_buf[3] << 8) | rx_buf[4];
        reg_low  = ((uint16_t)rx_buf[5] << 8) | rx_buf[6];
        return modbus_to_float(reg_high, reg_low);
    } else {
        ESP_LOGW(TAG, "Modbus read failed");
        return -1.0;
    }
}

// Wrapper fungsi untuk membaca data dari alamat tertentu
float read_modbus_voltage()     { return read_modbus_register_float(0x0000); }
float read_modbus_current()     { return read_modbus_register_float(0x0002); }
float read_modbus_power()       { return read_modbus_register_float(0x0004); }
float read_modbus_temperature() { return read_modbus_register_float(0x0006); }

// Fungsi mengecek status koneksi WiFi
bool is_wifi_connected() {
    wifi_ap_record_t info;
    return esp_wifi_sta_get_ap_info(&info) == ESP_OK;
}

// Task FreeRTOS untuk membaca data sensor dan kirim MQTT
void mqtt_send_task(void *pvParameter) {
    while (1) {
        // Cek koneksi WiFi
        if (!is_wifi_connected()) {
            ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
            esp_wifi_disconnect();
            esp_wifi_connect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        // Cek koneksi MQTT
        if (!mqtt_connected) {
            ESP_LOGW(TAG, "MQTT not connected");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Baca data dari sensor via Modbus
        float voltage = read_modbus_voltage();
        float current = read_modbus_current();
        float power   = read_modbus_power();
        float temp    = read_modbus_temperature();

        // Jika ada data invalid
        if (voltage < 0 || current < 0 || power < 0 || temp < 0) {
            ESP_LOGW(TAG, "Invalid sensor data");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Kendalikan kipas jika perlu
        fan_control(temp);

        // Buat JSON untuk dikirim ke MQTT
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "status", "OK");
        cJSON_AddStringToObject(root, "deviceID", "yourname");

        cJSON *data = cJSON_CreateObject();
        cJSON_AddNumberToObject(data, "v", voltage);
        cJSON_AddNumberToObject(data, "i", current);
        cJSON_AddNumberToObject(data, "pa", power);
        cJSON_AddNumberToObject(data, "temp", temp);
        cJSON_AddStringToObject(data, "fan", fan_state ? "ON" : "OFF");

        cJSON_AddItemToObject(root, "data", data);

        // Kirim ke topik MQTT
        char *message = cJSON_PrintUnformatted(root);
        if (message) {
            esp_mqtt_client_publish(client, "DATA/LOCAL/SENSOR/PANEL_1", message, 0, 1, 0);
            ESP_LOGI(TAG, "Data sent: %s", message);
            free(message);
        }

        // Hapus objek JSON dari memori
        cJSON_Delete(root);

        // Tunggu 1 detik
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Callback event handler MQTT
static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT connected");
            mqtt_connected = true;
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            mqtt_connected = false;
            break;
        default:
            break;
    }
    return ESP_OK;
}

// Fungsi inisialisasi MQTT
void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = MQTT_URI,
        .client_id = "node1",
        .event_handle = mqtt_event_handler_cb,
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

// Fungsi inisialisasi WiFi
void wifi_init_sta(void) {
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "kurnia",
            .password = "123456789",
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi initialized");
}

// Fungsi inisialisasi UART untuk komunikasi Modbus
void modbus_uart_init(void) {
    uart_config_t uart_config = {
        .baud_rate = MODBUS_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(MODBUS_UART_PORT, &uart_config);
    uart_set_pin(MODBUS_UART_PORT, MODBUS_TXD_PIN, MODBUS_RXD_PIN, MODBUS_RTS_PIN, UART_PIN_NO_CHANGE);
    uart_driver_install(MODBUS_UART_PORT, BUF_SIZE * 2, 0, 0, NULL, 0);
}

// Fungsi utama (entry point)
void app_main() {
    // Inisialisasi NVS (non-volatile storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Atur GPIO untuk kipas sebagai output
    gpio_pad_select_gpio(FAN_GPIO);
    gpio_set_direction(FAN_GPIO, GPIO_MODE_OUTPUT);

    // Inisialisasi WiFi, UART Modbus, dan MQTT
    wifi_init_sta();
    modbus_uart_init();
    mqtt_app_start();

    // Buat task untuk kirim data sensor secara periodik
    xTaskCreate(&mqtt_send_task, "mqtt_send_task", 8192, NULL, 5, NULL);
}
