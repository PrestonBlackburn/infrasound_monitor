
#include "raw_ring.h"

static inline uint32_t count_locked(const measurement_ring_buffer_t *r) {
    return r->head - r->tail; // free counters
}

int measurement_ring_init(measurement_ring_buffer_t *r, raw_sample_t *storage, uint32_t cap_pow2) {
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

void measurement_ring_push(measurement_ring_buffer_t *r, const raw_sample_t *s) {
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

uint32_t measurement_ring_drain(measurement_ring_buffer_t *r, raw_sample_t *out, uint32_t max) {
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


uint32_t measurement_ring_count(measurement_ring_buffer_t *r) {
    // lock before getting the count
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t n = count_locked(r);
    xSemaphoreGive(r->lock);
    return n;
}

uint32_t measurement_ring_dropped(measurement_ring_buffer_t *r) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t d = r->dropped;
    xSemaphoreGive(r->lock);
    return d;
}



