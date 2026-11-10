// mic_state.c
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "mic_state.h"

_Static_assert((W_MIC_PUB_RING_LEN & (W_MIC_PUB_RING_LEN - 1)) == 0, "W_MIC_PUB_RING_LEN must be a power of two");
_Static_assert((SPEC_RING_LEN & (SPEC_RING_LEN - 1)) == 0, "SPEC_RING_LEN must be a power of two");

#define W_MIC_PUB_MASK (W_MIC_PUB_RING_LEN - 1)
#define SPEC_MASK      (SPEC_RING_LEN - 1)
#define READ_LOCK_TIMEOUT pdMS_TO_TICKS(50)

// waveform state + ring buffer
static SemaphoreHandle_t       s_wave_lock;
// static mic_waveform_sample_t   s_wave_ring[W_MIC_PUB_RING_LEN];
static mic_waveform_sample_t *s_wave_ring; 
static uint64_t                s_wave_head = 0;

// spectrogram state
static SemaphoreHandle_t       s_spec_lock;
// static mic_spec_sample_t       s_spec_ring[SPEC_RING_LEN];
static mic_spec_sample_t     *s_spec_ring;

static uint64_t                s_spec_head = 0;


int mic_state_init(void) {
    s_wave_head = 0;
    s_spec_head = 0;

    s_wave_ring = heap_caps_calloc(W_MIC_PUB_RING_LEN, sizeof(*s_wave_ring),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_spec_ring = heap_caps_calloc(SPEC_RING_LEN, sizeof(*s_spec_ring),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_wave_ring || !s_spec_ring) return -1;

    s_wave_lock = xSemaphoreCreateMutex();
    s_spec_lock = xSemaphoreCreateMutex();
    if (!s_wave_lock || !s_spec_lock) return -1;
    return 0;
}

// ------ Waveform ------
void mic_waveform_publish(const mic_waveform_sample_t *s) {
    if (!s || !s_wave_lock) return;
    
    xSemaphoreTake(s_wave_lock, portMAX_DELAY);
    s_wave_ring[s_wave_head & W_MIC_PUB_MASK] = *s;
    s_wave_head++;
    xSemaphoreGive(s_wave_lock);
}

uint32_t mic_waveform_read_since(uint64_t *cursor, mic_waveform_sample_t *out, uint32_t max, uint32_t *dropped) {
    uint32_t missed = 0;
    
    if (!cursor || !out || xSemaphoreTake(s_wave_lock, READ_LOCK_TIMEOUT) != pdTRUE) {
        if (dropped) *dropped = 0;
        return 0;
    }

    uint64_t oldest = s_wave_head > W_MIC_PUB_RING_LEN ? s_wave_head - W_MIC_PUB_RING_LEN : 0;
    if (*cursor < oldest) {
        missed = (uint32_t)(oldest - *cursor);
        *cursor = oldest;
    } else if (*cursor > s_wave_head) {
        *cursor = s_wave_head;               
    }
    
    uint64_t avail = s_wave_head - *cursor;
    uint32_t n = (uint32_t)MIN(avail, (uint64_t)max);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_wave_ring[(*cursor + i) & W_MIC_PUB_MASK];
    }
    *cursor += n;

    xSemaphoreGive(s_wave_lock);
    if (dropped) *dropped = missed;
    return n;
}


// ----- Spectrogram -------
void mic_spec_publish(const mic_spec_sample_t *s) {
    if (!s || !s_spec_lock) return;

    xSemaphoreTake(s_spec_lock, portMAX_DELAY);
    s_spec_ring[s_spec_head & SPEC_MASK] = *s;
    s_spec_head++;
    xSemaphoreGive(s_spec_lock);
}

uint32_t mic_spec_read_since(uint64_t *cursor, mic_spec_sample_t *out, uint32_t max, uint32_t *dropped) {
    uint32_t missed = 0;
    
    if (!cursor || !out || xSemaphoreTake(s_spec_lock, READ_LOCK_TIMEOUT) != pdTRUE) {
        if (dropped) *dropped = 0;
        return 0;
    }

    uint64_t oldest = s_spec_head > SPEC_RING_LEN ? s_spec_head - SPEC_RING_LEN : 0;
    if (*cursor < oldest) {
        missed = (uint32_t)(oldest - *cursor);
        *cursor = oldest;
    } else if (*cursor > s_spec_head) {
        *cursor = s_spec_head;               
    }
    
    uint64_t avail = s_spec_head - *cursor;
    uint32_t n = (uint32_t)MIN(avail, (uint64_t)max);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_spec_ring[(*cursor + i) & SPEC_MASK];
    }
    *cursor += n;

    xSemaphoreGive(s_spec_lock);
    if (dropped) *dropped = missed;
    return n;
}