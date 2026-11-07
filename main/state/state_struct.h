// structs to be shared between python parser + main C code
// state_struct.h

typedef struct {
    uint32_t uptime_s;
    size_t   heap_free;
    float    raw_pressure_mpa;
    float    raw_db;
    float    temperature_c;
    bool     wifi_up;
} state_t;