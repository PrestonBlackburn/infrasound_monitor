#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "esp_timer.h"

#include <RadioLib.h>
#include "hal/ESP32S3Hal/Esp32S3Hal.hpp"

// #include "config_a.h"
// #include "config_b.h"
// #include "config_c.h"
// #include "config_a.h"
#include "example_conifg.h"


#define USE_LORAWAN 0 // 0=plain lora (for testing); 1=LoraWAN, expects gateway to be up

/* --- SX1262 custom wiring from castillated pins (bottom pinout, per board) --- */
#define RADIO_IRQ   (38) // (DIO1)
#define RADIO_RST   (39)
#define RADIO_GPIO  (40)   // (Busy)
#define RADIO_NSS   (41)
#define RADIO_RF_SWITCH_PIN (42) // (RF_SW) RF switch

// SPI
#define RADIO_MISO  (43)
// (shared with display)
#define RADIO_MOSI  (1)
#define RADIO_SCK   (2)

static const char *TAG = "lora";

// Testing params
#define TEST_FREQ_MHZ     915.0f
#define TEST_BW_KHZ       125.0f
#define TEST_SF           9
#define TEST_CR           7 // 4/7
#define TEST_SYNC_WORD    0x12
#define TEST_POWER_DBM    10
#define TEST_TCX0_VOLTAGE 1.6f // or 0

Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);
#if USE_LORAWAN
static LoRaWANNode node(&radio, &Region, subBand);
#endif

static bool s_radio_ready = false;




static void lorawan_send_task(void *arg)
{

    /* ── *Updated* Message format ────────────────────────────────────────
        * byte 0    : version    (uint8)  - bump if you change this layout
        * byte 1    : type       (uint8)  - MSG_TYPE_BAND_REPORT for now,
        *                                   leaves room for future message
        *                                   kinds on the same port
        * byte 2    : length     (uint8)  - total message length in bytes,
        *                                   including this header
        * byte 3-4  : band[0] dB SPL or FS x10, int16, little-endian
        * byte 5-6  : band[1] dB SPL or FS x10, int16, little-endian
        * byte 7-8  : band[2] dB SPL or FS x10, int16, little-endian
        * byte 9-10 : band[3] dB SPL or FS x10, int16, little-endian
        *
        * Total: 11 bytes. Values are dB SPL or FS * 10 so the decoder just
        * divides by 10 to recover one decimal place of precision,
        * e.g. -234 -> -23.4 dBFS. int16 range (+-32767) means this
        * scaling only overflows around +-3276.7 dBFS, nowhere near
        * anything you'll see in practice.
        *
        * Serialized by hand (not a C struct) so there's no risk of
        * compiler-inserted padding changing the on-wire layout --
        * what you see below is exactly what goes over the air. Bytes
        * are little-endian; make sure your network-server payload
        * decoder (TTN/ChirpStack function) reads them the same way. */
    #define MSG_VERSION           1
    #define MSG_TYPE_BAND_REPORT  2 // bump to "2" to indicate a dB SPL value instead of dBFS


    #if USE_LORAWAN
        uint64_t joinEUI = RADIOLIB_LORAWAN_JOIN_EUI;
        uint64_t devEUI  = RADIOLIB_LORAWAN_DEV_EUI;
        uint8_t  appKey[] = { RADIOLIB_LORAWAN_APP_KEY };
        uint8_t  nwkKey[] = { RADIOLIB_LORAWAN_NWK_KEY };

        int16_t state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
        if (state != RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "beginOTAA failed: %d", state);
            vTaskDelete(NULL);
        }

        ESP_LOGI(TAG, "Joining LoRaWAN network...");
        int joinAttempts = 0;
        do {
            state = node.activateOTAA();
            if (state != RADIOLIB_LORAWAN_NEW_SESSION) {
                ESP_LOGE(TAG, "Join attempt %d failed: %d", ++joinAttempts, state);
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
        } while (state != RADIOLIB_LORAWAN_NEW_SESSION);

        ESP_LOGI(TAG, "Joined!");

        // fixed data uplink rate
        node.setDatarate(LORAWAN_UPLINK_DATA_RATE);
    #endif

    // sends summarized data
    uint32_t counter = 0;
    while (true) {
        // placeholder payload (will eventually pull from sensor + mems mic)
        uint8_t payload[16];
        size_t len = snprintf((char *)payload, sizeof(payload), "ping %" PRIu32, counter++)
        
        #if USE_LORAWAN
            ESP_LOGI(TAG, "Sending placeholder uplink (%d bytes)", (int)len);
            int16_t txState = node.sendReceive(payload, len, LORAWAN_UPLINK_USER_PORT);
            if (txState < RADIOLIB_ERR_NONE) {
                ESP_LOGE(TAG, "sendReceive failed: %d", txState);
            } else if (txState > 0) {
                ESP_LOGE(TAG, "Downlink received in Rx%d", txState);
            } else {
                ESP_LOGI(TAG, "No downlink received");
            }
        #else
            ESP_LOGI(TAG, "TX: \"%s\" (%d bytes)", (char *)payload, (int)len);
            int16_t txState = radio.transmit(payload, len);
            if (txState == RADIOLIB_ERR_NONE) {
                ESP_LOGI(TAG, "TX OK");
            } else {
                ESP_LOGE(TAG, "TX failed: %d", txState);
            }
        #endif
        
        // must be below region's limits
        vTaskDelay(pdMS_TO_TICKS(LORAWAN_UPLINK_PERIOD));
    }
}

esp_err_t lorawan_radio_init(void) {
    if (s_radio_ready) return ESP_OK;

    // return 0 if correctly wired
    int16_t state = radio.begin(TEST_FREQ_MHZ, TEST_BW_KHZ, TEST_SF, TEST_CR,
                                TEST_SYNC_WORD, TEST_POWER_DBM,
                                8, TEST_TCX0_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Radio initialized OK");
    
    radio.setRfSwitchPins(RADIO_RF_SWITCH_PIN, RADIOLIB_NC);

    vTaskDelay(pdMS_TO_TICKS(1500));
    state = radio.setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "setDio2AsRfSwitch failed: %d", state);
        return ESP_FAIL;
    }
    s_radio_ready = true;
    return ESP_OK;
}

// public entry point
esp_err_t start_lorawan_task(void) {
    if (!s_radio_ready) {
        ESP_LOGE(TAG, "start_lorawan_task called before lorawan_radio_init!");
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(lorawan_send_task, "lorawan", 
                            8129, NULL, 6, NULL, 0);

    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
