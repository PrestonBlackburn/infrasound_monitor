// state.c
#include <string.h>
#include <sys/param.h>
#include "freertos/semphr.h"
#include "state.h"

_Static_assert((PUB_RING_LEN & (PUB_RING_LEN -  1)) == 0, "PUB_RING_LEN must be a power of two");

#define PUB_MASK (PUB_RING_LEN - 1)
#define READ_LOCK_TIMEOUT pdMS_TO_TICKS(50)

// init ring buffer
static SemaphoreHandle_t s_lock;
static pub_sample_t      s_ring[PUB_RING_LEN];
static uint64_t          s_head;       // total samples ever published
static state_t           s_state;      // latest snapshot
static uint32_t          s_seq;        // bumps on every snapshot change
static bool              s_have_state; // true after the first publish

int state_init(void) {
    s_head = 0;
    s_seq = 0;
    s_have_state = false;
    memset(&s_state, 0, sizeof(s_state));
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? 0 : -1;
}

void state_publish(const pub_sample_t *s, uint32_t n, const state_t *snapshot) {
    xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint32_t i=0; i < n; i++) {
        s_ring[(s_head + i) & PUB_MASK] = s[i];
    }
    s_head += n;

    if (snapshot) {
        bool wifi = s_state.wifi_up;
        s_state = *snapshot;
        s_state.wifi_up = wifi;
        s_have_state = true;
    }
    s_seq++;
    xSemaphoreGive(s_lock);
}

void state_set_wifi(bool up) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.wifi_up != up) {
        s_state.wifi_up = up;
        s_seq++;
    }
    xSemaphoreGive(s_lock);
}

bool state_get(state_t *out, uint32_t *seq, TickType_t timeout) {
    if (xSemaphoreTake(s_lock, timeout) != pdTRUE) {
        return false;
    }
    *out = s_state;
    if (seq) {
        *seq = s_seq;
    }
    bool have = s_have_state;
    xSemaphoreGive(s_lock);
    return have;
}

uint64_t state_cursor_latest(uint32_t back)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint64_t head = s_head;
    xSemaphoreGive(s_lock);

    uint64_t avail = MIN(head, (uint64_t)PUB_RING_LEN);
    if (back > avail) {
        back = (uint32_t)avail;
    }
    return head - back;
}


// core of read_since
uint32_t state_read_since(uint64_t *cursor,
    pub_sample_t *out, uint32_t max, uint32_t *dropped
) {

    uint32_t missed = 0;
    uint32_t n = 0;
    
    if (xSemaphoreTake(s_lock, READ_LOCK_TIMEOUT) != pdTRUE) {
        if (dropped) {
            *dropped = 0;
        }
        return 0;
    }

    uint64_t oldest = s_head > PUB_RING_LEN ? s_head - PUB_RING_LEN : 0;
    if (*cursor < oldest) {
        missed = (uint32_t)(oldest - *cursor);
        *cursor = oldest;
    } else if (*cursor > s_head) {
        *cursor = s_head;               // stale cursor from before a reset; resync
    }
    
    uint64_t avail = s_head - *cursor;
    n = (uint32_t)MIN(avail, (uint64_t)max);
    for (uint32_t i = 0; i < n; i++) {
        out[i] = s_ring[(*cursor + i) & PUB_MASK];
    }
    *cursor += n;

    xSemaphoreGive(s_lock);

    if (dropped) {
        *dropped = missed;
    }
    return n;
}