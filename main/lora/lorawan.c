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
#include "config_a.h"

/* --- SX1262 hat (bottom pinout, per board) --- */
#define RADIO_NSS   (5)
#define RADIO_IRQ   (2)
#define RADIO_RST   (3)
#define RADIO_GPIO  (4)   // Busy

#define RADIO_SCK   (7)
#define RADIO_MISO  (8)
#define RADIO_MOSI  (9)

#define RADIO_RF_SWITCH_PIN (38)

static const char *TAG = "lora";


static void lorawan_send_task(void *arg)
{
    // sends summarized data
    while (true) {
        BandReport report;
        bool have_report = (xQueuePeek(s_report_queue, &report, 0) == pdTRUE);

        /* ── Message format ────────────────────────────────────────
         * byte 0    : version    (uint8)  - bump if you change this layout
         * byte 1    : type       (uint8)  - MSG_TYPE_BAND_REPORT for now,
         *                                   leaves room for future message
         *                                   kinds on the same port
         * byte 2    : length     (uint8)  - total message length in bytes,
         *                                   including this header
         * byte 3-4  : band[0] dBFS x10, int16, little-endian
         * byte 5-6  : band[1] dBFS x10, int16, little-endian
         * byte 7-8  : band[2] dBFS x10, int16, little-endian
         * byte 9-10 : band[3] dBFS x10, int16, little-endian
         *
         * Total: 11 bytes. Values are dBFS * 10 so the decoder just
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
        #define MSG_TYPE_BAND_REPORT  1

        uint8_t uplinkPayload[3 + NUM_BANDS * 2];

        static_assert(sizeof(uplinkPayload) <= LORAWAN_UPLINK_DATA_MAX,
                      "uplinkPayload exceeds LORAWAN_UPLINK_DATA_MAX");

        uplinkPayload[0] = MSG_VERSION;
        uplinkPayload[1] = MSG_TYPE_BAND_REPORT;
        uplinkPayload[2] = (uint8_t)sizeof(uplinkPayload);

        for (int b = 0; b < (int)NUM_BANDS; b++) {
            float dbfs = have_report ? report.dbfs[b] : -100.0f;

            /* Scale by 10 and clamp to int16 range before narrowing --
             * do the clamp on a wider type so the cast itself can
             * never overflow/wrap. */
            int32_t scaled = (int32_t)lroundf(dbfs * 10.0f);
            if (scaled < INT16_MIN) scaled = INT16_MIN;
            if (scaled > INT16_MAX) scaled = INT16_MAX;
            int16_t value = (int16_t)scaled;

            int off = 3 + b * 2;
            uplinkPayload[off]     = (uint8_t)(value & 0xFF);         /* low byte  */
            uplinkPayload[off + 1] = (uint8_t)((value >> 8) & 0xFF);  /* high byte */
        }

        ESP_LOGI(TAG, "Sending band report (%d bytes, have_report=%d)",
                 (int)sizeof(uplinkPayload), have_report ? 1 : 0);

        int16_t txState = node.sendReceive(uplinkPayload, sizeof(uplinkPayload),
                                            LORAWAN_UPLINK_USER_PORT);

        if (txState < RADIOLIB_ERR_NONE) {
            ESP_LOGE(TAG, "sendReceive failed: %d", txState);
        } else if (txState > 0) {
            ESP_LOGI(TAG, "Downlink received in Rx%d", txState);
        } else {
            ESP_LOGI(TAG, "No downlink received");
        }

        /* LoRaWAN duty cycle / fair use - keep this at or above your
         * region's minimum for the payload size / data rate you're using. */
        vTaskDelay(pdMS_TO_TICKS(LORAWAN_UPLINK_PERIOD));
    }
}


// public entry point
esp_err_t lorawan_start(void) {

    Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
    static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
    static SX1262 radio(&mod);
    static LoRaWANNode node(&radio, &Region, subBand);

    uint64_t joinEUI = RADIOLIB_LORAWAN_JOIN_EUI;
    uint64_t devEUI  = RADIOLIB_LORAWAN_DEV_EUI;
    uint8_t  appKey[] = { RADIOLIB_LORAWAN_APP_KEY };
    uint8_t  nwkKey[] = { RADIOLIB_LORAWAN_NWK_KEY };

    int16_t state = radio.begin();
    if (state == RADIOLIB_ERR_NONE) {
        ESP_LOGI(TAG, "Radio initialized OK");
        radio.setRfSwitchPins(RADIO_RF_SWITCH_PIN, RADIOLIB_NC);
    } else {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    radio.setDio2AsRfSwitch(true);

    state = node.beginOTAA(joinEUI, devEUI, nwkKey, appKey);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "beginOTAA failed: %d", state);
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
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

    if (state != RADIOLIB_LORAWAN_NEW_SESSION) {
        // should never hit this now...
        ESP_LOGE(TAG, "Join failed after retries");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Joined!");

    /* Fix the uplink data rate rather than letting ADR pick one on the
     * fly -- keeps your payload-size budget predictable. */
    node.setDatarate(LORAWAN_UPLINK_DATA_RATE);


    xTaskCreatePinnedToCore(lorawan_send_task, "lorawan",
                             8192, NULL, 6, NULL, 0);
}
