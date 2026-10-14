#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "web_frame.h"

typedef struct {
    char *p;
    size_t cap;
    size_t len;
    bool  ok;
} sb_t;

// is a variadic function really the simplest option here?
static void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// fixed buffer instead of cJSON
static void sb_printf(sb_t *b, const char *fmt, ...) {
    if (!b->ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->ok = false;      // truncated: caller gets -1
        return;
    }
    b->len += (size_t)n;
}

static inline float finite_or_zero(float v) {
    return isfinite(v) ? v : 0.0f;
}

int web_frame_json(char *buf, size_t cap, const state_t *st, uint32_t seq,
                   uint32_t now_ms, const pub_sample_t *s, uint32_t n,
                   uint32_t dropped, bool reset)
{
    sb_t b = { .p = buf, .cap = cap, .len = 0, .ok = true };

    sb_printf(&b,
        "{\"seq\":%" PRIu32 ",\"now_ms\":%" PRIu32
        ",\"uptime_s\":%" PRIu32 ",\"heap_free\":%lu"
        ",\"raw_pressure_mpa\":%.2f,\"raw_db\":%.1f,\"temperature_c\":%.1f"
        ",\"wifi_up\":%s",
        seq, now_ms, st->uptime_s, (unsigned long)st->heap_free,
        finite_or_zero(st->raw_pressure_mpa), finite_or_zero(st->raw_db),
        finite_or_zero(st->temperature_c), st->wifi_up ? "true" : "false");

    if (reset || n > 0) {
        uint32_t t0 = n > 0 ? s[0].t_ms : now_ms;
        sb_printf(&b,
            ",\"window\":{\"reset\":%s,\"t0\":%" PRIu32 ",\"dropped\":%" PRIu32 ",\"dt\":[",
            reset ? "true" : "false", t0, dropped);
        for (uint32_t i = 0; i < n; i++) {
            // unsigned subtraction is wrap-safe
            sb_printf(&b, "%s%" PRIu32, i ? "," : "", (uint32_t)(s[i].t_ms - t0));
        }
        sb_printf(&b, "],\"value\":[");
        for (uint32_t i = 0; i < n; i++) {
            sb_printf(&b, "%s%" PRId32, i ? "," : "", s[i].mpa);
        }
        sb_printf(&b, "]}");
    }

    sb_printf(&b, "}");
    return b.ok ? (int)b.len : -1;
}