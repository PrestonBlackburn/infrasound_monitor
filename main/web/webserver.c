/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "state.h"
#include "web_assets.h"
#include "web_frame.h"
#include "webserver.h"

static const char *TAG = "webserver";

#define MAX_SSE_CLIENTS  3
#define SSE_TASK_STACK   8192
#define SSE_TASK_PRIO    3
#define SSE_PERIOD_MS    100
#define SSE_KEEPALIVE_TICKS 10 // send at least 1 frame / sec
#define SSE_BACKFILL_SAMPLES 0 // samples sent to a new client on connect (0 = start live)
#define SSE_SEND_BUF_SIZE    (6 + WEB_FRAME_BUF_SIZE + 2) // "data: " + json + "\n\n"

static httpd_handle_t s_server;
static atomic_int     s_clients;      // active sse task (thread safe int)
static atomic_bool    s_stopping;     // tells SSE tasks to wind down

static inline uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// http get for loaded static files
static esp_err_t static_get_handler(httpd_req_t *req) {
    const web_asset_t *static_asset = web_assets_find(req -> uri);
    if (!static_asset) {
        httpd_resp_send_404(req);
        return ESP_OK;
    }
    httpd_resp_set_type(req, static_asset->mime);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)static_asset->data, static_asset->len);
}

// endpoint:  /api/state  (snapshot)
static esp_err_t api_state_handler(httpd_req_t *req) {
    state_t st = {0};
    uint32_t seq = 0;
    state_get(&st, &seq, pdMS_TO_TICKS(50));

    char buf[512];
    int len = web_frame_json(buf, sizeof(buf), &st, seq, now_ms(), NULL, 0, 0, false);
    if (len < 0) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, buf, len);
}

// endpoint:  /events (sse stream)
static esp_err_t send_frame(
        httpd_req_t *req, 
        char *buf, 
        const pub_sample_t *s,
        uint32_t n, 
        uint32_t dropped, 
        bool reset
) {
    state_t st = {0};
    uint32_t seq = 0;
    state_get(&st, &seq, pdMS_TO_TICKS(20));

    memcpy(buf, "data: ", 6);
    int len = web_frame_json(
        buf + 6, 
        WEB_FRAME_BUF_SIZE, 
        &st, 
        seq, 
        now_ms(),
        s, 
        n, 
        dropped, 
        reset
    );

    if (len < 0) {
        ESP_LOGW(TAG, "frame did not fit, skipped");
        return ESP_OK;
    }
    memcpy(buf+6+len, "\n\n", 2);
    return httpd_resp_send_chunk(req, buf, 6+len+2);
}


// async request task
static void sse_task(void *arg) {
    httpd_req_t *req = arg;
    char *buf = malloc(SSE_SEND_BUF_SIZE);
    pub_sample_t smp[WEB_FRAME_MAX_SAMPLES];

    if (buf) {
        httpd_resp_set_type(req, "text/event-stream");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(req, "Connection", "keep-alive");
        // Tell EventSource to retry after 1 s if the stream drops
        bool alive = httpd_resp_send_chunk(req, "retry: 1000\n\n", HTTPD_RESP_USE_STRLEN) == ESP_OK;

        uint64_t cursor = state_cursor_latest(SSE_BACKFILL_SAMPLES);
        bool reset = true;                    // first frame tells the browser to start fresh
        uint32_t idle = 0;
        TickType_t last_wake = xTaskGetTickCount();

        ESP_LOGI(TAG, "SSE client connected (%d active)", atomic_load(&s_clients));

        while (alive && !atomic_load(&s_stopping)) {
            idle++;
            for (;;) {                        // drain in frame-sized chunks if behind
                uint32_t dropped = 0;
                uint32_t n = state_read_since(&cursor, smp, WEB_FRAME_MAX_SAMPLES, &dropped);
                if (n == 0 && !reset && idle < SSE_KEEPALIVE_TICKS) {
                    break;                    // nothing to say this tick
                }
                if (send_frame(req, buf, smp, n, dropped, reset) != ESP_OK) {
                    alive = false;            // client went away
                    break;
                }
                reset = false;
                idle = 0;
                if (n < WEB_FRAME_MAX_SAMPLES) {
                    break;                    // caught up
                }
            }
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SSE_PERIOD_MS));
        }
        free(buf);
    }

    httpd_resp_send_chunk(req, NULL, 0);       // best effort; ignored if the socket is gone
    httpd_req_async_handler_complete(req);
    atomic_fetch_sub(&s_clients, 1);
    ESP_LOGI(TAG, "SSE client left");
    vTaskDelete(NULL);
}


// event handler
static esp_err_t events_handler(httpd_req_t *req)
{
    if (atomic_fetch_add(&s_clients, 1) >= MAX_SSE_CLIENTS || atomic_load(&s_stopping)) {
        atomic_fetch_sub(&s_clients, 1);
        httpd_resp_send_custom_err(req, "503 Service Unavailable", "too many streams");
        return ESP_OK;
    }

    httpd_req_t *areq = NULL;
    if (httpd_req_async_handler_begin(req, &areq) != ESP_OK) {
        atomic_fetch_sub(&s_clients, 1);
        return ESP_FAIL;
    }
    if (xTaskCreate(sse_task, "sse", SSE_TASK_STACK, areq, SSE_TASK_PRIO, NULL) != pdPASS) {
        httpd_req_async_handler_complete(areq);
        atomic_fetch_sub(&s_clients, 1);
        return ESP_FAIL;
    }
    return ESP_OK;
}
// Server Lifecycle //

static const httpd_uri_t events_uri    = { 
    .uri = "/events",    
    .method = HTTP_GET, 
    .handler = events_handler 
};
static const httpd_uri_t api_state_uri = { 
    .uri = "/api/state", 
    .method = HTTP_GET, 
    .handler = api_state_handler 
};
static const httpd_uri_t static_uri = { 
    .uri = "/*",         
    .method = HTTP_GET, 
    .handler = static_get_handler 
};



static esp_err_t server_start(void)
{
    if (s_server) {
        return ESP_OK;
    }
    atomic_store(&s_stopping, false);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.stack_size = 6144;              // /api/state formats floats on this task
    config.send_wait_timeout = 2;          // seconds; a stalled client frees its task sooner
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t h = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&h, &config), TAG, "httpd_start");

    // Order matters: the wildcard goes last
    httpd_register_uri_handler(h, &events_uri);
    httpd_register_uri_handler(h, &api_state_uri);
    httpd_register_uri_handler(h, &static_uri);
    s_server = h;
    ESP_LOGI(TAG, "listening on port %d", config.server_port);
    return ESP_OK;
}


static void server_stop(void)
{
    if (!s_server) {
        return;
    }
    // httpd_stop() with live async requests would leave the SSE tasks holding
    // dead handles, so let them finish first.
    atomic_store(&s_stopping, true);
    for (int i = 0; i < 40 && atomic_load(&s_clients) > 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (atomic_load(&s_clients) > 0) {
        ESP_LOGW(TAG, "stopping with %d SSE clients still active", atomic_load(&s_clients));
    }
    httpd_stop(s_server);
    s_server = NULL;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    server_start();
}

static void on_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    server_stop();
}



esp_err_t webserver_start(void)
{
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "register got-ip");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_disconnected, NULL),
                        TAG, "register disconnect");
    return server_start();
}


// do we need a 404 handler still??
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    } else if (strcmp("/echo", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}