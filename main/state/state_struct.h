// structs to be shared between python parser + main C code

typedef struct {
    uint32_t uptime_s;
    size_t   heap_free;
    float    raw_pressure_mpa;
    float    raw_db;
    float    temperature_c;
    float    samples[8];
    bool     wifi_up;
} state_t;

typedef struct {
    uint32_t    sample_time;
    float       db_1_hz;
    float       db_5_hz;
    float       db_10_hz;
    float       db_20_hz;
} db_measure_t;