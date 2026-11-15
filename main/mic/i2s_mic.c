// i2s_mic.c 

// ICS-43432 (I2S, 24-bit in 32-bit slot) -> float -> DC block
//   -> waveform min/max bins   (mic_waveform_publish)
//   -> 512-pt FFT -> 64 triangular mel bands, dB SPL   (mic_spec_publish)
//   -> overall dB SPL (time domain), dB(A), dB(C)      (mic_get_levels)
//
// Task layout:
//   I2S_READ : blocks in i2s_channel_read, converts to float, pushes to a
//              FreeRTOS stream buffer (never blocks on the push)
//   I2S_PROC : blocks on the stream buffer until a full 50 ms chunk exists,
//              runs the DSP, publishes
//   I2S_LOG  : optional 1 Hz logging
//
// All three tasks do float math, so they are pinned to one core (MIC_CORE).

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_dsp.h"

#include "i2s_mic.h"
#include "mic_state.h"

static const char *TAG = "i2s_mic";

// i2s gpio //
#define GPIO_BCLK   13
#define GPIO_DIN    12   /* DOUT on the breakout → DIN on the S3 */
#define GPIO_WS     11   /* LRCL on the breakout */


// Config //
#define SAMPLE_RATE   MIC_SAMPLE_RATE
#define PROCESS_PERIOD_MS    50    // publish frequency
#define HOP_SAMPLES   (SAMPLE_RATE * PROCESS_PERIOD_MS / 1000)   // 800 @ 16kHz

#define NUM_WAVE_BINS        50 // must match bins[] in mic_waveform_sample_t
#define WAVE_PER_BIN  (HOP_SAMPLES / NUM_WAVE_BINS)  // 16 @ 16 kHz
 
#define FFT_SIZE            512     // 31.25 Hz bins @ 16 kHz
#define N_BINS              (FFT_SIZE / 2 + 1)
#define NUM_MEL_BINS        64      // must match mel_bins[] in mic_spec_sample_t
#define MEL_FMIN_HZ         20.0f

#define I2S_READ_FRAMES     256  // 16 ms @ 16kHz
#define I2S_DMA_DESC_NUM    6    // ~96ms of DMA slack
#define RAW_STREAM_BYTES    (HOP_SAMPLES * sizeof(float) *4) // 4 chunks of slack

#define DC_BLOCK_R          0.995f  // 1-pole DC blocker, fc ~ 13 Hz @ 16 kHz

// ICS-43432: -26 dBFS (sine) @ 94 dB SPL. A full-scale sine has RMS 0.707, so
//   dB SPL = 10*log10(mean_square) + 94 + 26 + 3.01
#define MIC_SPL_OFFSET_DB   (94.0f + 26.0f + 3.01f)

// task specs
#define MIC_CORE            1
#define READ_TASK_PRIO      6
#define PROC_TASK_PRIO      4
#define LOG_TASK_PRIO       1

_Static_assert(HOP_SAMPLES % NUM_WAVE_BINS == 0, "HOP_SAMPLES must divide evenly into NUM_WAVE_BINS");
_Static_assert(HOP_SAMPLES >= FFT_SIZE, "HOP_SAMPLES must be >= FFT_SIZE");
_Static_assert(sizeof(((mic_waveform_sample_t *)0)->bins) / sizeof(wave_bin_t) == NUM_WAVE_BINS,
               "NUM_WAVE_BINS does not match mic_waveform_sample_t");
_Static_assert(sizeof(((mic_spec_sample_t *)0)->mel_bins) / sizeof(float) == NUM_MEL_BINS,
               "NUM_MEL_BINS does not match mic_spec_sample_t");


// State //
static i2s_chan_handle_t      s_rx_chan;
static StreamBufferHandle_t   s_raw;
static volatile bool          s_verbose = true;
static volatile uint32_t      s_dropped_samples = 0;

// dsp buffers for processing
static float s_chunk[HOP_SAMPLES];
static float s_filt[HOP_SAMPLES];
static float s_fft[FFT_SIZE * 2] __attribute__((aligned(16)));
static float s_hann[FFT_SIZE];
static float s_pscale[N_BINS]; // one-sided, window-normalised |X|^2 -> mean-square
static float s_pwr[N_BINS];    // per-bin mean-square (full-scale^2 units)
static float s_wA[N_BINS];     // A-weighting power gain per bin
static float s_wC[N_BINS];     // C-weighting power gain per bin

typedef struct {
    uint16_t first; // first FFT bin
    uint16_t count; // number of bins
    uint16_t woff;  // offset into s_mel_w
} mel_band_t;

#define MEL_W_MAX (2 * N_BINS + NUM_MEL_BINS)
static mel_band_t s_mel[NUM_MEL_BINS];
static float      s_mel_w[MEL_W_MAX];

// Latest summary levels (for LoRa / UI)
static portMUX_TYPE s_lvl_mux = portMUX_INITIALIZER_UNLOCKED;
static mic_levels_t s_levels;
static bool         s_levels_valid = false;
 

// Helpers //
static inline float hz_to_mel(float hz) {
    return 1127.0f * logf(1.0f + hz / 700.0f);
}

static inline float mel_to_hz(float mel) {
    return 700.0f * (expf(mel / 1127.0f) - 1.0f);
}

// mean square (dBFS to dB SPL)
static inline float pwr_to_spl(float p) {
    return 10.0f * log10f(fmaxf(p, 1e-12f)) + MIC_SPL_OFFSET_DB;
}


static inline int16_t to_i16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (int16_t)(v * 32767.0f);
}

void i2s_set_verbose(bool on) {
    s_verbose = on;
    ESP_LOGI(TAG, "verbose logging %s", on ? "on" : "off");
}

bool mic_get_levels(mic_levels_t *out) {
    if (!out) return false;
    bool valid;
    portENTER_CRITICAL(&s_lvl_mux);
    *out = s_levels;
    valid = s_levels_valid;
    portEXIT_CRITICAL(&s_lvl_mux);
    return valid;
}

// dsp table setup
static esp_err_t dsp_tables_init(void) {
    ESP_RETURN_ON_ERROR(dsps_fft2r_init_fc32(NULL, FFT_SIZE), TAG, "fft init");
    dsps_wind_hann_f32(s_hann, FFT_SIZE);

    // window power norm: sum(s_pwr) == mean-square of signal
    double sumw2 = 0.0;
    for (int i=0; i < FFT_SIZE; i++) {
        sumw2 += (double)s_hann[i] * s_hann[i];
    }
    const float base = (float)(1.0 / ((double)FFT_SIZE * sumw2));
    for (int k=0; k<N_BINS; k++) {
        s_pscale[k] = (k==0 || k==FFT_SIZE / 2) ? base : 2.0f * base;
    }

    // // A / C weighting power gains per bin (IEC 61672 analog responses)
    // https://www.virtins.com/doc/Frequency-Weightings-for-Sound-and-Vibration-Perceived-by-Humans-using-Multi-Instrument.pdf

    const double p1 = 20.6 * 20.6, p2 = 107.7 * 107.7;
    const double p3 = 737.9 * 737.9, p4 = 12194.0 * 12194.0;
    for (int k = 0; k < N_BINS; k++) {
        double f  = (double)k * SAMPLE_RATE / FFT_SIZE;
        double f2 = f * f;
        double ra = p4 * f2 * f2 / ((f2 + p1) * sqrt((f2 + p2) * (f2 + p3)) * (f2 + p4));
        double rc = p4 * f2 / ((f2 + p1) * (f2 + p4));
        double ga = ra * 1.258925;    // +2.00 dB normalisation
        double gc = rc * 1.006932;    // +0.06 dB normalisation
        s_wA[k] = (float)(ga * ga);
        s_wC[k] = (float)(gc * gc);
    }
    return ESP_OK;
}

// Triangular mel filterbank, stored sparsely (only bins with weight > 0).
static esp_err_t mel_filterbank_init(void)
{
    const float m_lo = hz_to_mel(MEL_FMIN_HZ);
    const float m_hi = hz_to_mel(SAMPLE_RATE / 2.0f);
    float edge_hz[NUM_MEL_BINS + 2];
    for (int i = 0; i < NUM_MEL_BINS + 2; i++) {
        edge_hz[i] = mel_to_hz(m_lo + i * (m_hi - m_lo) / (NUM_MEL_BINS + 1));
    }
 
    const float bin_hz = (float)SAMPLE_RATE / FFT_SIZE;
    int w_used = 0;
 
    for (int m = 0; m < NUM_MEL_BINS; m++) {
        const float l = edge_hz[m], c = edge_hz[m + 1], r = edge_hz[m + 2];
        int first = -1, count = 0;
        int woff = w_used;
 
        for (int k = 0; k < N_BINS; k++) {
            float f = k * bin_hz;
            float w = 0.0f;
            if (f > l && f <= c)      w = (f - l) / (c - l);
            else if (f > c && f < r)  w = (r - f) / (r - c);
            if (w > 0.0f) {
                ESP_RETURN_ON_FALSE(w_used < MEL_W_MAX, ESP_ERR_NO_MEM, TAG, "mel weights overflow");
                if (first < 0) first = k;
                s_mel_w[w_used++] = w;
                count++;
            }
        }
 
        // Band narrower than one FFT bin: fall back to the nearest bin.
        if (count == 0) {
            int k = (int)roundf(c / bin_hz);
            if (k < 0) k = 0;
            if (k > N_BINS - 1) k = N_BINS - 1;
            ESP_RETURN_ON_FALSE(w_used < MEL_W_MAX, ESP_ERR_NO_MEM, TAG, "mel weights overflow");
            first = k;
            s_mel_w[w_used++] = 1.0f;
            count = 1;
        }
 
        s_mel[m].first = (uint16_t)first;
        s_mel[m].count = (uint16_t)count;
        s_mel[m].woff  = (uint16_t)woff;
    }
    return ESP_OK;
}


// i2s // 

static esp_err_t i2s_init(void) {

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_READ_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan), TAG, "new channel");
 
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = GPIO_BCLK,
            .ws   = GPIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din  = GPIO_DIN,
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;   /* SEL tied to GND */

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "init std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable channel");
    return ESP_OK;
}


// tasks
static void i2s_read_task(void *arg)
{
    (void)arg;
    static int32_t  i2s_buf[I2S_READ_FRAMES];
    static float    f32_buf[I2S_READ_FRAMES];
 
    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, i2s_buf, sizeof(i2s_buf),
                                         &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S read error: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));   // don't spin if the channel is broken
            continue;
        }
 
        int n_frames = (int)(bytes_read / sizeof(int32_t));
 
        /* ICS-43432: 24-bit audio in top 3 bytes of each 32-bit word.
         * Shift right by 8, then normalise to [-1, 1] using the
         * 24-bit full-scale value (8 388 608 = 2^23).               */
        for (int i = 0; i < n_frames; i++) {
            f32_buf[i] = (float)(i2s_buf[i] >> 8) / 8388608.0f;
        }

        size_t want = n_frames * sizeof(float);
        size_t sent = xStreamBufferSend(s_raw, f32_buf, want, 0);
        if (sent < want) {
            s_dropped_samples += (uint32_t)((want - sent) / sizeof(float));
            ESP_LOGW(TAG, "raw stream full, dropped samples");
        }
    }
}


static void i2s_process_task(void *arg) {

    (void)arg;
    float dc_x1 = 0.0f, dc_y1 = 0.0f;
    bool  primed = false;

    while (1) {

        // Collect exactly one chunk
        // Receive can return fewer bytes than requested, so accumulate.
        uint8_t *dst = (uint8_t *)s_chunk;
        size_t filled = 0;
        while (filled < sizeof(s_chunk)) {
            filled += xStreamBufferReceive(s_raw, dst + filled,
                                           sizeof(s_chunk) - filled, portMAX_DELAY);
        }

        // DC block + time-domain mean-square
        if (!primed) { dc_x1 = s_chunk[0]; primed = true; }   // avoid start-up step
        float sumsq = 0.0f;
        for (int i = 0; i < HOP_SAMPLES; i++) {
            float x = s_chunk[i];
            float y = x - dc_x1 + DC_BLOCK_R * dc_y1;
            dc_x1 = x;
            dc_y1 = y;
            s_filt[i] = y;
            sumsq += y * y;
        }
        const float db_spl = pwr_to_spl(sumsq / HOP_SAMPLES);
        const uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000);

        // Waveform min/max bins
        mic_waveform_sample_t w;
        w.t_ms = t_ms;
        for (int b = 0; b < NUM_WAVE_BINS; b++) {
            const float *p = &s_filt[b * WAVE_PER_BIN];
            float lo = p[0], hi = p[0];
            for (int j = 1; j < WAVE_PER_BIN; j++) {
                if (p[j] < lo) lo = p[j];
                if (p[j] > hi) hi = p[j];
            }
            w.bins[b].min = to_i16(lo);
            w.bins[b].max = to_i16(hi);
        }
        mic_waveform_publish(&w);

        // FFT of the most recent FFT_SIZE samples
        const float *seg = &s_filt[HOP_SAMPLES - FFT_SIZE];
        for (int i = 0; i < FFT_SIZE; i++) {
            s_fft[2 * i]     = seg[i] * s_hann[i];
            s_fft[2 * i + 1] = 0.0f;
        }
        dsps_fft2r_fc32(s_fft, FFT_SIZE);
        dsps_bit_rev_fc32(s_fft, FFT_SIZE);
 
        float pa = 0.0f, pc = 0.0f;
        for (int k = 0; k < N_BINS; k++) {
            float re = s_fft[2 * k], im = s_fft[2 * k + 1];
            float p  = (re * re + im * im) * s_pscale[k];
            s_pwr[k] = p;
            pa += p * s_wA[k];
            pc += p * s_wC[k];
        }

        // Mel bands (band power -> dB SPL)
        mic_spec_sample_t sp;
        sp.t_ms = t_ms;
        for (int m = 0; m < NUM_MEL_BINS; m++) {
            const mel_band_t *mb = &s_mel[m];
            const float *wt = &s_mel_w[mb->woff];
            float e = 0.0f;
            for (int j = 0; j < mb->count; j++) {
                e += s_pwr[mb->first + j] * wt[j];
            }
            sp.mel_bins[m] = pwr_to_spl(e);
        }
        mic_spec_publish(&sp);

        //  Summary levels ------------------------------------------
        portENTER_CRITICAL(&s_lvl_mux);
        s_levels.t_ms   = t_ms;
        s_levels.db_spl = db_spl;
        s_levels.db_a   = pwr_to_spl(pa);
        s_levels.db_c   = pwr_to_spl(pc);
        s_levels_valid  = true;
        portEXIT_CRITICAL(&s_lvl_mux);
    }
}

static void i2s_log_task(void *arg)
{
    (void)arg;
    while (1) {
        // This task has no blocking primitive to wait on, so vTaskDelay IS
        // the right pacing mechanism here.
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_verbose) continue;
 
        mic_levels_t l;
        if (mic_get_levels(&l)) {
            ESP_LOGI(TAG, "SPL %.1f dB | dB(A) %.1f | dB(C) %.1f | dropped samples %lu",
                     l.db_spl, l.db_a, l.db_c, (unsigned long)s_dropped_samples);
        } else {
            ESP_LOGW(TAG, "no audio processed yet");
        }
    }
}
 

// public entry point
esp_err_t i2s_start(void) {
     ESP_RETURN_ON_FALSE(mic_state_init() == 0, ESP_ERR_NO_MEM, TAG, "mic state init");
 
    s_raw = xStreamBufferCreate(RAW_STREAM_BYTES, I2S_READ_FRAMES * sizeof(float));
    ESP_RETURN_ON_FALSE(s_raw, ESP_ERR_NO_MEM, TAG, "raw stream buffer");
 
    ESP_RETURN_ON_ERROR(dsp_tables_init(), TAG, "dsp tables");
    ESP_RETURN_ON_ERROR(mel_filterbank_init(), TAG, "mel filterbank");
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "i2s init");
 
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(i2s_process_task, "I2S_PROC", 6144, NULL,
                                                PROC_TASK_PRIO, NULL, MIC_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "process task");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(i2s_read_task, "I2S_READ", 4096, NULL,
                                                READ_TASK_PRIO, NULL, MIC_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "read task");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(i2s_log_task, "I2S_LOG", 3072, NULL,
                                                LOG_TASK_PRIO, NULL, MIC_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "log task");
    return ESP_OK;
}