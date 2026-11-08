#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "lorawan.h"
#include <RadioLib.h>
#include "hal/ESP32S3Hal/Esp32S3Hal.hpp"

#include "config_infrasound_a.h"
// #include "config_b.h"
// #include "config_c.h"
// #include "config_a.h"
// #include "example_config.h"


#define USE_LORAWAN 1 // 0=plain lora (for testing); 1=LoraWAN, expects gateway to be up

// for persisting nonces per device when in production
#define LORAWAN_PERSIST_NVS  0 //persist nonces to nvs 
#define LORAWAN_ERASE_NVS_ON_BOOT 0 // for dev, should also flush on server
#define LORAWAN_SESSION_SAVE_EVERY 1

#define LORAWAN_NVS_NAMESPACE "lorawan"
#define LORAWAN_KEY_DEVEUI "deveui"
#define LORAWAN_KEY_NONCES "nonces"
#define LORAWAN_KEY_SESSION "session"


/* --- SX1262 custom wiring from castillated pins (bottom pinout, per board) --- */
#define RADIO_IRQ   (38) // (DIO1)
#define RADIO_RST   (39)
#define RADIO_GPIO  (40)   // (Busy)
#define RADIO_NSS   (41)
#define RADIO_RF_SWITCH_PIN (42) // (RF_SW) RF switch

// SPI
#define RADIO_MISO  (10)
// (shared with display)
#define RADIO_MOSI  (2)
#define RADIO_SCK   (1)

static const char *TAG = "lora";

// Testing params
#define TEST_FREQ_MHZ     915.0f
#define TEST_BW_KHZ       125.0f
#define TEST_SF           9
#define TEST_CR           7 // 4/7
#define TEST_SYNC_WORD    0x12
#define TEST_POWER_DBM    10
#define TEST_TCXO_VOLTAGE 1.6f // or 0

Esp32S3Hal hal(RADIO_SCK, RADIO_MISO, RADIO_MOSI);
static Module mod(&hal, RADIO_NSS, RADIO_IRQ, RADIO_RST, RADIO_GPIO);
static SX1262 radio(&mod);
#if USE_LORAWAN
static LoRaWANNode node(&radio, &Region, subBand);
#endif

static bool s_radio_ready = false;


#if USE_LORAWAN && LORAWAN_PERSIST_NVS

    static uint8_t s_nonces_buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
    static uint8_t s_session_buf[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];

    static bool persist_load(const char *key, uint8_t *data, size_t len) {
        nvs_handle_t h;
        if(nvs_open(LORAWAN_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
            return false; // if namespace doesn't exist
        }
        size_t got = len;
        esp_err_t err = nvs_get_blob(h, key, data, &got);
        nvs_close(h);
        return (err == ESP_OK && got == len);
    }

    static esp_err_t persist_save(const char *key, const uint8_t *data, size_t len) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(LORAWAN_NVS_NAMESPACE, NVS_READWRITE, &h);
        if (err != ESP_OK) return err;
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        return err;
    }

    static void persist_erase_all(void) {
        nvs_handle_t h;
        if(nvs_open(LORAWAN_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    // save nonce after every join attempt (failed join still uses)
    static void lorawan_save_nonces(uint64_t devEUI) {
        esp_err_t err = persist_save(LORAWAN_KEY_DEVEUI, (const uint8_t *)&devEUI, sizeof(devEUI));
        if (err == ESP_OK) {
            err = persist_save(LORAWAN_KEY_NONCES, node.getBufferNonces(),
                            RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "saving nonces to NVS failed: %s", esp_err_to_name(err));
        }
    }


    static void lorawan_save_session(void) {
        esp_err_t err = persist_save(LORAWAN_KEY_SESSION, node.getBufferSession(),
                                    RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Saving session to NVS failed: %s", esp_err_to_name(err));
        }
    }

    // restore nonces + sessions from nvs
    static bool lorawan_restore_state(uint64_t devEUI) {
        #if LORAWAN_ERASE_NVS_ON_BOOT
            ESP_LOGW(TAG, "LORAWAN_ERASE_NVS_ON_BOOT is set: wiping saved nonces/session");
            persist_erase_all();
            return false;
        #else
            uint64_t storedEUI = 0;
            if (!persist_load(LORAWAN_KEY_DEVEUI, (uint8_t *)&storedEUI, sizeof(storedEUI))) {
                ESP_LOGI(TAG, "No saved LoraWAN state in NVS, will join");
                return false;
            }
            if (storedEUI != devEUI) {
                ESP_LOGW(TAG, "Saved state is for a different DevEUI, wiping it");
                persist_erase_all();
                return false;
            }
            if (persist_load(LORAWAN_KEY_NONCES, s_nonces_buf, sizeof(s_nonces_buf))) {
                int16_t st = node.setBufferNonces(s_nonces_buf);
                if (st == RADIOLIB_ERR_NONE) {
                    ESP_LOGI(TAG, "restored join nonces");
                } else {
                    ESP_LOGW(TAG, "Restoring nonces failed: %d (starting fresh)", st);
                }
            }

            if (!persist_load(LORAWAN_KEY_SESSION, s_session_buf, sizeof(s_session_buf))) {
                ESP_LOGI(TAG, "No saved session, will join");
                return false;
            }

            int16_t st = node.setBufferSession(s_session_buf);
            if (st != RADIOLIB_ERR_NONE) {
                ESP_LOGW(TAG, "Restoring session failed: %d (will join)", st);
                return false;
            }

            st = node.activateOTAA();
            if (st == RADIOLIB_LORAWAN_SESSION_RESTORED) {
                ESP_LOGI(TAG, "Restored LoraWAN session, no join needed");
                return true;
            }
            ESP_LOGW(TAG, "Session activation returned %d (will join)", st);
            return false;
        #endif
    }

#endif // USE_LORAWAN && LORAWAN_PERSIST_NVS


static void probe_radio(void) {
    // troubleshoot failure to init
    hal.init();
    hal.pinMode(RADIO_NSS, hal.GpioModeOutput);
    hal.digitalWrite(RADIO_NSS, 1);
    hal.pinMode(RADIO_RST, hal.GpioModeOutput);
    hal.digitalWrite(RADIO_RST, 0);
    hal.delay(5);
    hal.digitalWrite(RADIO_RST, 1);
    hal.delay(20);
    hal.pinMode(RADIO_GPIO, hal.GpioModeInput);
    ESP_LOGI(TAG, "BUSY after reset = %d (expect 0)",
             (int)hal.digitalRead(RADIO_GPIO));

    uint8_t tx[2] = { 0xC0, 0x00 };   // GetStatus
    uint8_t rx[2] = { 0, 0 };
    hal.digitalWrite(RADIO_NSS, 0);
    hal.spiTransfer(tx, 2, rx);
    hal.digitalWrite(RADIO_NSS, 1);
    ESP_LOGI(TAG, "GetStatus -> %02x %02x", rx[0], rx[1]);
}

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

        bool sessionRestored = false;
        #if LORAWAN_PERSIST_NVS
            sessionRestored = lorawan_restore_state(devEUI);
        #endif

        if (!sessionRestored) {
            ESP_LOGI(TAG, "Joining LoRaWAN network...");
            int joinAttempts = 0;

            do {
                state = node.activateOTAA();
                #if LORAWAN_PERSIST_NVS
                    // track every join nonce
                    lorawan_save_nonces(devEUI);
                #endif
                
                if (state != RADIOLIB_LORAWAN_NEW_SESSION) {
                ESP_LOGE(TAG, "Join attempt %d failed: %d", ++joinAttempts, state);
                // back off after a few tries
                vTaskDelay(pdMS_TO_TICKS(joinAttempts < 6 ? 10000 : 60000));
                }
            } while (state != RADIOLIB_LORAWAN_NEW_SESSION);

            ESP_LOGI(TAG, "Joined!");
        }

        // fixed data uplink rate
        node.setDatarate(LORAWAN_UPLINK_DATA_RATE);

        #if LORAWAN_PERSIST_NVS
            uint32_t uplinksSinceSave = 0;
        #endif
    #endif

    // sends summarized data
    uint32_t counter = 0;
    while (true) {
        // placeholder payload (will eventually pull from sensor + mems mic)
        uint8_t payload[16];
        size_t len = snprintf((char *)payload, sizeof(payload), "ping %" PRIu32, counter++);
        
        #if USE_LORAWAN
            ESP_LOGI(TAG, "Sending placeholder uplink (%d bytes)", (int)len);
            int16_t txState = node.sendReceive(payload, len, LORAWAN_UPLINK_USER_PORT);
            if (txState < RADIOLIB_ERR_NONE) {
                ESP_LOGE(TAG, "sendReceive failed: %d", txState);
            } else if (txState > 0) {
                ESP_LOGI(TAG, "Downlink received in Rx%d", txState);
            } else {
                ESP_LOGI(TAG, "No downlink received");
            }

            #if LORAWAN_PERSIST_NVS
                if(txState >= RADIOLIB_ERR_NONE && ++uplinksSinceSave >= LORAWAN_SESSION_SAVE_EVERY) {
                    lorawan_save_session();
                    uplinksSinceSave = 0;
                }
            #endif
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
    hal.setCsPin(RADIO_NSS);

    if (s_radio_ready) return ESP_OK;

    // return 0 if correctly wired
    int16_t state = radio.begin(TEST_FREQ_MHZ, TEST_BW_KHZ, TEST_SF, TEST_CR,
                                TEST_SYNC_WORD, TEST_POWER_DBM,
                                8, TEST_TCXO_VOLTAGE);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "Radio init failed: %d", state);
        probe_radio();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Radio initialized OK");
    
    radio.setRfSwitchPins(RADIO_RF_SWITCH_PIN, RADIOLIB_NC);

    state = radio.setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "setDio2AsRfSwitch failed: %d", state);
        // return ESP_FAIL;
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
                            8192, NULL, 6, NULL, 0);

    return (ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
