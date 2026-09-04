/*
 * Morse-code tone playback through the onboard ES8311 codec + speaker
 * amplifier on the Waveshare ESP32-S3-ePaper-1.54 (V1).
 *
 * esp_codec_dev + raw i2s_std, DAC/output direction: an I2S TX channel,
 * es8311_codec_cfg_t.codec_mode = ...WORK_MODE_DAC, and the speaker PA
 * pin wired in so esp_codec_dev toggles it around open()/close().
 *
 * All the board-specific bits are cross-checked against Waveshare's own
 * ESP-IDF audio example for this board
 * (github.com/waveshareteam/ESP32-S3-ePaper-1.54, 02_Example/ESP-IDF/V1/
 * 08_Audio_Test), which is the only Waveshare demo that actually drives
 * this board's speaker:
 *   - I2S/I2C pins, PA pin (GPIO46), use_mclk, pa_gain: board_cfg.txt
 *     board "S3_ePaper_1_54".
 *   - Audio power rail (GPIO42) is ACTIVE-LOW -- board_power_bsp.cpp's
 *     POWEER_Audio_ON() drives it to 0. Getting this backwards leaves the
 *     codec's analog side unpowered: the I2S digital path still clocks
 *     out cleanly (no errors, writes block for the right duration) but
 *     nothing comes out of the speaker.
 *   - Playback is opened as stereo (channel = 2); a mono tone is written
 *     to both L and R. audio_bsp.c always opens the codec that way, and
 *     esp_codec_dev's format check rejects odd channel counts.
 */
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "morse_player.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "morse_player";

/* --- Board wiring (see file header for provenance) --- */
#define AUDIO_I2C_SDA_PIN   47
#define AUDIO_I2C_SCL_PIN   48
#define AUDIO_I2S_MCLK_PIN  14
#define AUDIO_I2S_BCLK_PIN  15
#define AUDIO_I2S_WS_PIN    38
#define AUDIO_I2S_DOUT_PIN  45 /* codec DAC data out of the MCU */
/* ES8311 codec power rail enable. ACTIVE-LOW: Waveshare's board_power_bsp
 * POWEER_Audio_ON() drives this pin to 0 to enable, 1 to disable -- same
 * active-low sense as the e-paper's PWR pin. (../listening_device drove it
 * high and only ever recorded; the mic's digital path came up anyway, but
 * the DAC/speaker path needs this rail actually powered.) */
#define AUDIO_PWR_PIN       42
#define AUDIO_PA_PIN        46 /* speaker amplifier enable, active-high */

/* --- Playback format --- */
/* 16 kHz is plenty for a 600 Hz sine; 16-bit. Stereo: the working
 * Waveshare playback path always opens the ES8311 with channel = 2 and
 * esp_codec_dev's format check rejects odd channel counts -- one mono
 * tone is written to both L and R. Audio is synthesized and written one
 * Morse element at a time (see morse_player_play), so playback RAM is a
 * single element buffer (~35 KB at 15 wpm) regardless of message length. */
#define SAMPLE_RATE_HZ      16000
#define BITS_PER_SAMPLE     16
#define CHANNELS            2

#define OUT_VOLUME_DEFAULT  100  /* esp_codec_dev master volume, 0..100; runtime-set via morse_player_set_volume() */
/* Sine peak as a fraction of full scale. 0.9 is the clean ceiling -- 1.0
 * lets render_tone()'s raised-cosine edges round to full scale and buzz
 * on loud element starts/ends. Drop toward 0.8 if the attached speaker
 * still spits on the edges. */
#define TONE_AMPLITUDE      0.9f

/* External amplifier gain in dB, from Waveshare's board_cfg.txt
 * ("pa_gain: 6" for board S3_ePaper_1_54). Feeds esp_codec_dev's volume
 * curve via es8311_codec_cfg_t.hw_gain so OUT_VOLUME_PCT lands where the
 * curve expects. */
#define PA_GAIN_DB          6.0f

/* Raised-cosine ramp on each keyed element's leading/trailing edge, to
 * kill the click ("key clatter") a hard on/off of a sine would make. 4ms
 * is inaudible as a slope but well above the ~1.7ms period of a 600Hz
 * tone. */
#define EDGE_RAMP_MS        4
/* Silence padded before the first / after the last element so the PA has
 * settled before the tone starts and isn't cut off mid-decay on close(). */
#define LEAD_SILENCE_MS     40
#define TAIL_SILENCE_MS     60

/* Sanity cap on the message length morse_player_play() accepts
 * (message_store clamps to MESSAGE_MAX_LEN, well below this, upstream). */
#define MAX_TEXT_LEN        512

/* International Morse, A-Z then 0-9. '.' = dit (1 unit), '-' = dah (3). */
static const char *const MORSE_ALPHA[26] = {
    ".-",   "-...", "-.-.", "-..",  ".",    "..-.", "--.",  "....", "..",   ".---",
    "-.-",  ".-..", "--",   "-.",   "---",  ".--.", "--.-", ".-.",  "...",  "-",
    "..-",  "...-", ".--",  "-..-", "-.--", "--..",
};
static const char *const MORSE_DIGIT[10] = {
    "-----", ".----", "..---", "...--", "....-",
    ".....", "-....", "--...", "---..", "----.",
};

static esp_codec_dev_handle_t s_dev;   /* NULL until morse_player_init() succeeds */
static SemaphoreHandle_t s_busy;       /* 1 permit; held (count 0) while a message is playing */
static TaskHandle_t s_task;            /* runs the synthesis off the caller's thread */
static volatile bool s_abort;          /* set by morse_player_stop(); polled between elements */
static int s_out_volume = OUT_VOLUME_DEFAULT; /* applied at the next esp_codec_dev_open() */

/* One in-flight request, filled by morse_player_play() before it notifies
 * s_task. No lock needed: s_busy gates a second play() until the task has
 * finished with the previous s_req. */
static struct {
    char text[MAX_TEXT_LEN + 1];
    int wpm;
    int tone_hz;
    bool loop;
} s_req;

static void morse_player_task(void *arg);

static const char *morse_for(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return MORSE_ALPHA[c - 'A'];
    if (c >= '0' && c <= '9') return MORSE_DIGIT[c - '0'];
    return NULL;
}

static void audio_power_on(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << AUDIO_PWR_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    io.pull_up_en = GPIO_PULLUP_ENABLE; /* matches Waveshare board_power_bsp */
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&io));
    gpio_set_level(AUDIO_PWR_PIN, 0); /* active-low: 0 = rail ON */
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the codec power rail settle */
}

esp_err_t morse_player_init(void) {
    audio_power_on();

    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = AUDIO_I2C_SCL_PIN,
        .sda_io_num = AUDIO_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    esp_err_t err = i2c_new_master_bus(&i2c_cfg, &i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* feed zeros (silence) when we're not writing, like Waveshare's codec_board */
    i2s_chan_handle_t tx_handle;
    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL); /* playback-only: no rx handle */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws = AUDIO_I2S_WS_PIN,
            .dout = AUDIO_I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_codec_dev's I2S data interface owns enable/disable of the
     * channel around open()/close() -- don't call i2s_channel_enable()
     * here (same as ../listening_device). */
    audio_codec_i2s_cfg_t data_cfg = { .tx_handle = tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
        .clock_speed_hz = 400000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = AUDIO_PA_PIN,
        .pa_reverted = false, /* board_cfg has no reversal flag -> PA enables on GPIO high */
        .use_mclk = true,
        .master_mode = false, /* ESP32 drives BCLK/WS/MCLK; codec is the I2S slave */
        .hw_gain.pa_gain = PA_GAIN_DB,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!data_if || !ctrl_if || !gpio_if || !codec_if) {
        ESP_LOGE(TAG, "Failed to build one or more ES8311 interfaces");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    s_busy = xSemaphoreCreateBinary();
    if (!s_busy) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_busy); /* start unheld = idle */

    if (xTaskCreate(morse_player_task, "morse_player", 6144, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(morse_player) failed");
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Speaker ready: ES8311 @ %dHz %d-bit %dch (PA on GPIO%d)",
             SAMPLE_RATE_HZ, BITS_PER_SAMPLE, CHANNELS, AUDIO_PA_PIN);
    return ESP_OK;
}

/* Writes `frames` stereo frames of a `tone_hz` sine at TONE_AMPLITUDE into
 * `dst` (interleaved L,R -- same value in both), with an EDGE_RAMP_MS
 * raised-cosine fade in/out on the element edges. */
static void render_tone(int16_t *dst, int frames, int tone_hz) {
    const float w = 2.0f * (float)M_PI * (float)tone_hz / (float)SAMPLE_RATE_HZ;
    int ramp = EDGE_RAMP_MS * SAMPLE_RATE_HZ / 1000;
    if (ramp > frames / 2) {
        ramp = frames / 2;
    }
    const float peak = TONE_AMPLITUDE * 32767.0f;

    for (int n = 0; n < frames; n++) {
        float env = 1.0f;
        if (ramp > 0 && n < ramp) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)n / (float)ramp));
        } else if (ramp > 0 && n >= frames - ramp) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)(frames - 1 - n) / (float)ramp));
        }
        int16_t v = (int16_t)lrintf(sinf(w * (float)n) * env * peak);
        dst[2 * n] = v;
        dst[2 * n + 1] = v;
    }
}

/* Fills `buf` with one element -- a keyed tone or silence -- of `frames`
 * frames and writes it to the codec (blocking). `buf` must hold at least
 * frames*CHANNELS int16. Returns ESP_OK / ESP_FAIL. */
static esp_err_t emit(int16_t *buf, int frames, bool tone, int tone_hz) {
    if (tone) {
        render_tone(buf, frames, tone_hz);
    } else {
        memset(buf, 0, (size_t)frames * CHANNELS * sizeof(int16_t));
    }
    int rc = esp_codec_dev_write(s_dev, buf, frames * CHANNELS * (int)sizeof(int16_t));
    return rc == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

/* Synthesizes the message and streams it to the codec, one element at a
 * time (blocking each esp_codec_dev_write). Runs on s_task, never on the
 * caller. With `loop` set it repeats the message (separated by a word gap)
 * until morse_player_stop() sets s_abort; otherwise it plays once. Bails
 * out early -- still emitting the tail silence and closing the codec
 * cleanly -- on abort or a write error. Always releases s_busy on exit
 * (the count-0 state morse_player_is_playing() checks). */
static void render_and_play(const char *text, int wpm, int tone_hz, bool loop) {
    /* PARIS timing: 1 unit = 1200ms / wpm. dit = 1 unit, dah = 3;
     * intra-character gap = 1, inter-character gap = 3, word gap = 7. */
    const int unit_ms = 1200 / wpm;
    const int spu = SAMPLE_RATE_HZ * unit_ms / 1000; /* frames per unit */

    /* One reusable buffer, sized to the largest single write (a 7-unit
     * word gap). Every symbol/gap/pad is one emit() into this buffer and
     * one blocking esp_codec_dev_write() -- so playback RAM is fixed
     * regardless of how long the message is. */
    const int max_chunk_frames = 7 * spu;
    int16_t *buf = malloc((size_t)max_chunk_frames * CHANNELS * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "out of memory for the %d-frame element buffer", max_chunk_frames);
        xSemaphoreGive(s_busy);
        return;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE_HZ,
        .channel = CHANNELS,
        .bits_per_sample = BITS_PER_SAMPLE,
    };
    esp_err_t ret = ESP_OK;
    /* Open per play, close after: esp_codec_dev_close() drops the PA, so
     * the speaker is silent (no idle hiss) between presses. The
     * "i2s_channel_disable ... not been enabled yet" line the library logs
     * on the first open of each cycle is harmless. */
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        free(buf);
        xSemaphoreGive(s_busy);
        return;
    }
    esp_codec_dev_set_out_vol(s_dev, s_out_volume);

    int64_t total_units = 0;
    int reps = 0;
    ret = emit(buf, LEAD_SILENCE_MS * SAMPLE_RATE_HZ / 1000, false, tone_hz);

    while (ret == ESP_OK && !s_abort) {
        bool first_letter = true;
        bool pending_word_gap = false;
        for (const char *p = text; *p && ret == ESP_OK && !s_abort; p++) {
            if (*p == ' ') {
                pending_word_gap = true;
                continue;
            }
            const char *code = morse_for(*p);
            if (!code) {
                ESP_LOGW(TAG, "skipping unsupported character 0x%02x", (unsigned char)*p);
                continue;
            }

            if (!first_letter) {
                int gap = pending_word_gap ? 7 : 3; /* inter-character or word gap */
                ret = emit(buf, gap * spu, false, tone_hz);
                total_units += gap;
            }
            first_letter = false;
            pending_word_gap = false;

            for (int i = 0; code[i] && ret == ESP_OK && !s_abort; i++) {
                if (i > 0) {
                    ret = emit(buf, spu, false, tone_hz); /* 1-unit intra-character gap */
                    total_units += 1;
                    if (ret != ESP_OK) {
                        break;
                    }
                }
                int on = (code[i] == '.') ? 1 : 3;
                ret = emit(buf, on * spu, true, tone_hz);
                total_units += on;
            }
        }

        reps++;
        if (!loop || s_abort || ret != ESP_OK) {
            break;
        }
        ret = emit(buf, 7 * spu, false, tone_hz); /* word gap between repeats */
        total_units += 7;
    }

    if (ret == ESP_OK) {
        /* Runs even on abort (ret stays ESP_OK): lets the PA settle so
         * close() doesn't clip the last element's decay into a click. */
        ret = emit(buf, TAIL_SILENCE_MS * SAMPLE_RATE_HZ / 1000, false, tone_hz);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_write failed mid-message");
    } else if (s_abort) {
        ESP_LOGI(TAG, "Stopped \"%s\" (BOOT press) after %d rep(s)", text, reps);
    } else if (loop) {
        ESP_LOGI(TAG, "Looped \"%s\" @ %dwpm %dHz for %d rep(s)", text, wpm, tone_hz, reps);
    } else {
        ESP_LOGI(TAG, "Played \"%s\" @ %dwpm %dHz (%lld units, ~%.1fs)",
                 text, wpm, tone_hz, (long long)total_units, (double)total_units * unit_ms / 1000.0);
    }

    esp_codec_dev_close(s_dev);
    free(buf);
    xSemaphoreGive(s_busy);
}

/* Waits for a queued request and plays it. One request in flight at a time
 * (gated by s_busy in morse_player_play). */
static void morse_player_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        render_and_play(s_req.text, s_req.wpm, s_req.tone_hz, s_req.loop);
    }
}

esp_err_t morse_player_play(const char *text, int wpm, int tone_hz, bool loop) {
    if (!s_dev) {
        ESP_LOGW(TAG, "play() before a successful morse_player_init() -- ignored");
        return ESP_ERR_INVALID_STATE;
    }
    /* wpm floor of 8 bounds the element buffer: one write is at most a
     * 7-unit word gap = 7 * (1200/wpm) ms. */
    if (wpm < 8 || wpm > 60 || tone_hz < 100 || tone_hz > 4000) {
        ESP_LOGW(TAG, "play() with out-of-range wpm=%d tone_hz=%d -- ignored", wpm, tone_hz);
        return ESP_ERR_INVALID_ARG;
    }
    if (text == NULL || strlen(text) > MAX_TEXT_LEN) {
        ESP_LOGW(TAG, "play() with missing/over-long text -- ignored");
        return ESP_ERR_INVALID_ARG;
    }

    bool any_playable = false;
    for (const char *p = text; *p; p++) {
        if (morse_for(*p)) {
            any_playable = true;
            break;
        }
    }
    if (!any_playable) {
        ESP_LOGW(TAG, "nothing playable in \"%s\"", text);
        return ESP_ERR_INVALID_ARG;
    }

    /* s_busy is released by the task when it finishes -- a non-zero take
     * here means a message is still playing. */
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) {
        ESP_LOGW(TAG, "play() while already playing -- ignored");
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_req.text, text, sizeof(s_req.text));
    s_req.wpm = wpm;
    s_req.tone_hz = tone_hz;
    s_req.loop = loop;
    s_abort = false;
    xTaskNotifyGive(s_task); /* hand off to morse_player_task */
    return ESP_OK;
}

bool morse_player_is_playing(void) {
    return s_busy != NULL && uxSemaphoreGetCount(s_busy) == 0;
}

void morse_player_stop(void) {
    if (morse_player_is_playing()) {
        s_abort = true;
        ESP_LOGI(TAG, "stop requested -- playback ends at the next element boundary");
    }
}

void morse_player_set_volume(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_out_volume = pct;
    /* Not pushed to the codec here -- it is closed between plays; picked
     * up at the next esp_codec_dev_open() in render_and_play(). */
    ESP_LOGI(TAG, "Output volume set to %d%% (applies on next play)", pct);
}
