#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "state.h"

#define WEB_FRAME_MAX_SAMPLES 64   // SSE task max read frames
#define WEB_FRAME_BUF_SIZE  2048 // buffer for JSON messages

// writes 1 json frame into buf and returns len
int web_frame_json(char *buf, size_t cap, const state_t *st, uint32_t seq,
                    uint32_t now_ms, const pub_sample_t *s, uint32_t n,
                uint32_t dropped, bool reset);


