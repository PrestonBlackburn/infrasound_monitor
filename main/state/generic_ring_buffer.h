#pragma once
// generic_ring_buffer.h

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"


typedef struct {
    uint8_t          *buf;          // Raw byte storage pointer
    size_t           element_size;  // Size of each sample (e.g., sizeof(raw_sample_t))
    uint32_t         mask;          // capacity - 1 (capacity must be power of 2)
    uint32_t         head;
    uint32_t         tail;   
    uint32_t         dropped;       // Samples overwritten
    SemaphoreHandle_t lock;
} generic_ring_buffer_t;

/**
 * @brief Initialize the generic ring buffer.
 * @param r Pointer to ring buffer control block.
 * @param storage Pre-allocated memory block (must be capacity * element_size bytes).
 * @param element_size Size of a single sample (sizeof(sample_type)).
 * @param cap_pow2 Capacity of the buffer (must be a power of 2).
 */
int generic_ring_init(generic_ring_buffer_t *r, void *storage, size_t element_size, uint32_t cap_pow2);

void generic_ring_push(generic_ring_buffer_t *r, const void *s);
uint32_t generic_ring_drain(generic_ring_buffer_t *r, void *out, uint32_t max);
uint32_t generic_ring_count(generic_ring_buffer_t *r);
uint32_t generic_ring_dropped(generic_ring_buffer_t *r);