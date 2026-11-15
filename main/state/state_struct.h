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

// shared mic info
typedef struct {
    uint32_t uptime_s;
    float    waveform_dbspl;
} mic_waveform_t;

typedef struct {
    uint32_t uptime_s;
    float    mel_spectrogram[64];
} mic_spectrogram_t;

// struct for combining mic data and sensor data
// version, type, message length, array[dB SPL value, hz]
// int8 (1byte), char (1 byte), int32 (4 bytes), [int16 (2 bytes), int16 (2 bytes)], []... = min 12 bytes to < 100 bytes (max array len of 20 in a single message)
typedef struct {
    int16_t db_spl;
    int16_t hz;
} lora_db_values_t;

// will merge last two arrays of struct together for lora message
// can either be updated from Sensor loop or Mic loop
typedef struct {
    int8_t             version;       // set once
    char               message_type;  // set once
    int32_t            message_length;
    // max values? (still considering)
    int16_t            mic_db_spl;
    int16_t            mic_db_a;
    int16_t            mic_db_c;
    int16_t            sensor_db_spl;
    int16_t            sensor_db_a; // expected to be 0?
    int16_t            sensor_db_c;
    // rough freq buckets
    lora_db_values_t   sensor[5]; // always have 5 buckets for the sensor
    lora_db_values_t   mic[64];   // starting with 64 mel bins
} lora_message_t;
