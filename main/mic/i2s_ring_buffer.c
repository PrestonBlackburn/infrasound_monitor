// i2s_ring_buffer.c

#include "i2s_ring_buffer.h"

static inline uint32_t count_locked(const i2s_ring_buffer_t *r) {
    return r->head - r->tail; // free counters
}

int i2s_ring_init(i2s_ring_buffer_t *r, i2s_sample_t *storage, uint32_t cap_pow2) {
    // invalid setup case
    if (!r || !storage || cap_pow2 == 0 || (cap_pow2 & (cap_pow2 -1)) != 0) {
        return -1;
    }

    r->buf = storage;
    r->mask = cap_pow2 - 1;
    r->head = 0;
    r->tail = 0;
    r->dropped = 0;
    r->lock = xSemaphoreCreateMutex();
    return  r->lock ? 0 : -1;
}

void i2s_ring_push(i2s_ring_buffer_t *r, const i2s_sample_t *s) {
    // lock and write struct
    xSemaphoreTake(r->lock, portMAX_DELAY);
    // full/drop oldest case
    if (count_locked(r) > r->mask) {
        r->tail++;
        r->dropped++;
    }
    r->buf[r->head & r->mask] = *s;
    r->head++;
    xSemaphoreGive(r->lock);
}

uint32_t i2s_ring_drain(i2s_ring_buffer_t *r, i2s_sample_t *out, uint32_t max) {
    // lock before draining structs
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t n = count_locked(r);
    if (n > max) {
        n = max;
    }
    for (uint32_t i = 0; i<n; i++) {
        out[i] = r->buf[(r->tail + i) & r->mask];
    }
    r->tail += n;
    xSemaphoreGive(r->lock);
    return n;
}


uint32_t i2s_ring_count(i2s_ring_buffer_t *r) {
    // lock before getting the count
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t n = count_locked(r);
    xSemaphoreGive(r->lock);
    return n;
}

uint32_t i2s_ring_dropped(i2s_ring_buffer_t *r) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t d = r->dropped;
    xSemaphoreGive(r->lock);
    return d;
}



