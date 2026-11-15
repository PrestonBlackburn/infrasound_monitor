// Processing logic for sending over lorawan
// We need a smaller efficient message to send over lorawan than raw data
// maybe something like
// max mPa and dB at many bands?
// 0.5 - 5 Hz
// 5 - 10 Hz
// 10-20 Hz
// 20-30 Hz
// 30-40 Hz

// We can use the same message format as the mic for now


// placeholder...
#include "mic_state.h"

// Example: Extracting key frequency buckets for your lora_message_t payload
void populate_lora_mic_buckets(lora_db_values_t *mic_bucket_out) {
    mic_spec_sample_t latest_spec;
    uint64_t cursor = mic_spec_cursor_latest(1); // Look at the very latest frame
    uint32_t dropped = 0;
    
    // Read the latest spectrogram sample
    if (mic_spec_read_since(&cursor, &latest_spec, 1, &dropped) > 0) {
        // Map your target Mel bins to your LoRa payload slots
        // Example: Bin 5 (~200Hz), Bin 15 (~1kHz), Bin 30 (~4kHz), etc.
        mic_bucket_out[0].db_spl = (int16_t)(latest_spec.mel_bins[5] * 10.0f);  
        mic_bucket_out[1].db_spl = (int16_t)(latest_spec.mel_bins[15] * 10.0f); 
        mic_bucket_out[2].db_spl = (int16_t)(latest_spec.mel_bins[30] * 10.0f); 
    }
}