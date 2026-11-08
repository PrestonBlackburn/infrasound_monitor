// generic_ring_buffer.c
#include "generic_ring_buffer.h"
#include <string.h>

static inline uint32_t count_locked(const generic_ring_buffer_t *r) {
    return r->head - r->tail;
}

int generic_ring_init(generic_ring_buffer_t *r, void *storage, size_t element_size, uint32_t cap_pow2) {
    if (!r || !storage || element_size == 0 || cap_pow2 == 0 || (cap_pow2 & (cap_pow2 - 1)) != 0) {
        return -1;
    }

    r->buf = (uint8_t *)storage;
    r->element_size = element_size;
    r->mask = cap_pow2 - 1;
    r->head = 0;
    r->tail = 0;
    r->dropped = 0;
    r->lock = xSemaphoreCreateMutex();
    
    return r->lock ? 0 : -1;
}

void generic_ring_push(generic_ring_buffer_t *r, const void *s) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    
    // Full / Drop oldest case
    if (count_locked(r) > r->mask) {
        r->tail++;
        r->dropped++;
    }
    
    // Copy sample into the correct ring buffer slot
    size_t index = r->head & r->mask;
    memcpy(r->buf + (index * r->element_size), s, r->element_size);
    r->head++;
    
    xSemaphoreGive(r->lock);
}

uint32_t generic_ring_drain(generic_ring_buffer_t *r, void *out, uint32_t max) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    
    uint32_t n = count_locked(r);
    if (n > max) {
        n = max;
    }
    
    uint8_t *out_bytes = (uint8_t *)out;
    for (uint32_t i = 0; i < n; i++) {
        size_t index = (r->tail + i) & r->mask;
        memcpy(out_bytes + (i * r->element_size), 
               r->buf + (index * r->element_size), 
               r->element_size);
    }
    
    r->tail += n;
    xSemaphoreGive(r->lock);
    return n;
}

uint32_t generic_ring_count(generic_ring_buffer_t *r) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t n = count_locked(r);
    xSemaphoreGive(r->lock);
    return n;
}

uint32_t generic_ring_dropped(generic_ring_buffer_t *r) {
    xSemaphoreTake(r->lock, portMAX_DELAY);
    uint32_t d = r->dropped;
    xSemaphoreGive(r->lock);
    return d;
}