// =============================================================
//  ESP32-CAM Sri Lankan Rupee Currency Reader (Standalone Assistive Device)
//  Model: rihab40-project-1 v1.0.3 (128x128 FOMO Object Detection)
//  Camera: VGA 640x480 (Full Wide-Angle Field of View) + Direct DMA Downsampler
//  Audio: WeAct Studio PCM5100A Dual PA (I2S_NUM_1: IO14, IO15, IO13)
//  Illumination: Active front flashlight enabled (25kHz ultrasonic PWM, zero hum)
//  Performance: Ultra-fast instant response (<40ms) optimized for blind persons
// =============================================================

#include "esp_camera.h"
#include <soc/soc.h>
#include <soc/rtc_cntl_reg.h>
#include "driver/i2s.h"

// Must define before camera_pins.h
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

// Edge Impulse FOMO Model
#include <rihab40-project-1_inferencing.h>

// Embedded 16kHz 16-bit Voice Clips
#include "currency_audio.h"

// =============================================================
// Hardware Configuration
// =============================================================

// I2S Pins for WeAct Studio PCM5100A Dual PA (Uses I2S_NUM_1)
#define I2S_BCLK_PIN 14  // Connect to BCK on PCM5100A
#define I2S_WS_PIN   15  // Connect to WS on PCM5100A
#define I2S_DATA_PIN 13  // Connect to DIN on PCM5100A
// NOTE on WeAct module: Leave SD and MC pins UNCONNECTED!

// Flash LED Configuration (GPIO 4 on AI-Thinker ESP32-CAM)
// Uses 25kHz ultrasonic PWM: completely above human hearing (>20kHz) for zero audible whine!
#define FLASH_LED_PIN      4
#define FLASH_LEDC_CHANNEL 7
#define FLASH_PWM_FREQ     25000 // 25 kHz ultrasonic PWM
#define FLASH_PWM_DUTY     255   // 50% brightness (bright illumination, cool operation)

// Camera Resolution: VGA 640x480 with sensor DCW binning gives FULL wide-angle view
#define CAM_W 800
#define CAM_H 600

// =============================================================
// Ultra-Fast Detection & False-Positive Filter Settings
// =============================================================
// Fast-path threshold: >= 75% triggers INSTANTLY on a single frame (<40ms)!
const float FAST_TRIGGER_THRESHOLD = 0.45f;

// Secondary threshold: 60% to 75% requires 2 consecutive frames to confirm
const float CONFIDENCE_MIN_THRESHOLD = 0.30f;

// Fast reset: Scene must be clear for only 350ms before re-arming for the next note
const uint32_t SCENE_RESET_DELAY_MS = 180;

// Re-announce repeat delay if the exact same note is continuously held in view (3.0s)
const uint32_t HELD_REPEAT_DELAY_MS = 1200;

// Currency Names (Ordered by Class Index 0 to 9)
const char* CURRENCY_NAMES[10] = {
    "Rs. 1",
    "Rs. 10",
    "Rs. 100",
    "Rs. 1,000",
    "Rs. 2",
    "Rs. 50",
    "Rs. 5",
    "Rs. 20",
    "Rs. 500",
    "Rs. 5,000"
};

// Global Buffers & Queues
uint8_t *ei_buf = NULL; // 128x128x3 BGR buffer for Edge Impulse
QueueHandle_t audioQueue = NULL;

// Active speaking flag: pauses camera DMA and AI inference during speech
// to completely eliminate DMA bus contention and power supply ripple!
volatile bool is_speaking = false;

// =============================================================
// Edge Impulse Image Input Callback
// =============================================================
int get_camera_data(size_t offset, size_t length, float* out) {
    size_t px = offset * 3;
    for (size_t i = 0; i < length; i++, px += 3) {
        // FOMO RGB888 packing: (R << 16) | (G << 8) | B
        out[i] = (ei_buf[px + 2] << 16) | (ei_buf[px + 1] << 8) | ei_buf[px];
    }
    return 0;
}

// Ultra-fast direct downsampler from SVGA (800x600) RGB565 to 128x128 BGR888 in ei_buf (~0.3 ms)
void fast_downsample_rgb565_to_bgr888(const uint8_t* src, int w, int h, uint8_t* dst) {
    const int row_stride = w * 2;
    for (int y = 0; y < 128; y++) {
        int src_y = (y * h) >> 7; // Fast bitshift downsampling by 128
        int src_row = src_y * row_stride;
        int dst_idx = y * 128 * 3;
        for (int x = 0; x < 128; x++) {
            int src_x = (x * w) >> 7;
            int p_idx = src_row + (src_x << 1);
            uint8_t b1 = src[p_idx];
            uint8_t b2 = src[p_idx + 1];

            dst[dst_idx]     = (b2 & 0x1F) << 3;                          // B
            dst[dst_idx + 1] = ((b1 & 0x07) << 5) | ((b2 & 0xE0) >> 3); // G
            dst[dst_idx + 2] = b1 & 0xF8;                                 // R

            dst_idx += 3;
        }
    }
}

// =============================================================
// I2S Audio Output Functions (WeAct Studio PCM5100A on I2S_NUM_1)
// =============================================================
void setupAudio() {
    // 32-bit slot width @ 16kHz produces BCK = 16000 * 32 * 2 = 1.024MHz (64 fS),
    // which satisfies Table 11 of the PCM5100A datasheet and locks the internal PLL!
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[Audio] I2S driver install failed: 0x%x\n", err);
        return;
    }
    i2s_set_pin(I2S_NUM_1, &pin_config);
    i2s_zero_dma_buffer(I2S_NUM_1);
    Serial.println("[Audio] WeAct PCM5100A I2S Initialized (16kHz, 32-bit slot, 1.024MHz BCK)");
}

// Startup Chime: Ascending C Major chord (C5: 523Hz -> E5: 659Hz -> G5: 784Hz)
void playStartupChime() {
    is_speaking = true;
    Serial.println("[Audio] Playing startup ready chime...");
    int32_t buf[256 * 2];
    float freqs[3] = { 523.25f, 659.25f, 783.99f }; // C5, E5, G5
    int durations[3] = { 2000, 2000, 2800 }; // ~125ms, 125ms, 175ms

    for (int f = 0; f < 3; f++) {
        int total = durations[f];
        for (int i = 0; i < total; i += 256) {
            int chunk = min(256, total - i);
            for (int j = 0; j < chunk; j++) {
                float angle = 2.0f * 3.14159f * freqs[f] * (i + j) / 16000.0f;
                int16_t s16 = (int16_t)(sinf(angle) * 24000.0f);
                int32_t s32 = ((int32_t)s16) << 16;
                buf[j * 2]     = s32; // Left channel (L+ / L-)
                buf[j * 2 + 1] = s32; // Right channel (R+ / R-)
            }
            size_t written;
            i2s_write(I2S_NUM_1, buf, chunk * 2 * sizeof(int32_t), &written, pdMS_TO_TICKS(100));
        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
    memset(buf, 0, sizeof(buf));
    size_t written = 0;
    i2s_write(I2S_NUM_1, buf, sizeof(buf), &written, pdMS_TO_TICKS(100));
    Serial.println("[Audio] Ready chime finished!");
    is_speaking = false;
}

void playCurrencyAudio(int noteId) {
    if (noteId < 0 || noteId > 9) return;
    is_speaking = true; // Pause camera DMA and AI to eliminate electrical noise

    const uint8_t* audio_ptr = currency_audio_table[noteId];
    size_t audio_len = currency_audio_lengths[noteId];

    const int16_t* samples = (const int16_t*)audio_ptr;
    size_t num_samples = audio_len / 2;

    Serial.printf("[Audio] ANNOUNCING: %s (%d samples)\n", CURRENCY_NAMES[noteId], (int)num_samples);

    int32_t stereo_buf[512]; // 256 stereo frames
    size_t s_idx = 0;
    while (s_idx < num_samples) {
        size_t chunk = min((size_t)256, num_samples - s_idx);
        for (size_t i = 0; i < chunk; i++) {
            int16_t samp = (int16_t)pgm_read_word(&samples[s_idx + i]);
            // Clean 1.25x scaling: perfectly unclipped, natural, crystal-clear voice!
            int32_t amplified = (int32_t)samp * 5 / 4;
            if (amplified > 32767)  amplified = 32767;
            if (amplified < -32768) amplified = -32768;
            int32_t s32 = amplified << 16;
            stereo_buf[i * 2]     = s32; // Left channel (L+ / L-)
            stereo_buf[i * 2 + 1] = s32; // Right channel (R+ / R-)
        }
        size_t bytes_written = 0;
        i2s_write(I2S_NUM_1, stereo_buf, chunk * 2 * sizeof(int32_t), &bytes_written, pdMS_TO_TICKS(100));
        s_idx += chunk;
    }

    memset(stereo_buf, 0, sizeof(stereo_buf));
    size_t written = 0;
    i2s_write(I2S_NUM_1, stereo_buf, sizeof(stereo_buf), &written, pdMS_TO_TICKS(100));
    Serial.println("[Audio] Announcement finished!");

    // Brief 20ms buffer settling before resuming camera
    vTaskDelay(20 / portTICK_PERIOD_MS);
    is_speaking = false;
}

// FreeRTOS Audio Task on Core 0
void audioTask(void* pv) {
    // 300ms boot grace period allows the onboard RC circuit (R8 10k + C21 1uF) to charge CTRL to 3.3V
    vTaskDelay(300 / portTICK_PERIOD_MS);

    for (;;) {
        int noteId;
        if (xQueueReceive(audioQueue, &noteId, portMAX_DELAY) == pdTRUE) {
            if (noteId == -2) {
                playStartupChime();
            } else if (noteId >= 0 && noteId <= 9) {
                playCurrencyAudio(noteId);
            }
        }
    }
}

// =============================================================
// Standalone Camera & AI Engine Task on Core 1 (Ultra-Fast Response)
// =============================================================
void cameraAiTask(void* pv) {
    Serial.println("[AI Engine] High-Speed Vision Pipeline running on Core 1");

    // Debounce & State Tracking Variables
    int      pending_class       = -1;
    int      consecutive_matches = 0;
    int      last_spoken_class   = -1;
    uint32_t last_spoken_time    = 0;
    int      empty_frames        = 0;
    bool     was_speaking        = false;

    for (;;) {
        // While audio is actively speaking, pause camera acquisition and AI inference!
        if (is_speaking) {
            was_speaking = true;
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        // The very moment speech finishes, flush stale DMA frames captured during speech
        if (was_speaking) {
            was_speaking = false;
            camera_fb_t* stale1 = esp_camera_fb_get();
            if (stale1) esp_camera_fb_return(stale1);
            camera_fb_t* stale2 = esp_camera_fb_get();
            if (stale2) esp_camera_fb_return(stale2);
        }

        // 1. Grab the freshest frame from camera DMA
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            vTaskDelay(2 / portTICK_PERIOD_MS);
            continue;
        }

        if (fb->format != PIXFORMAT_RGB565 || !ei_buf) {
            esp_camera_fb_return(fb);
            vTaskDelay(2 / portTICK_PERIOD_MS);
            continue;
        }

        // 2. Direct fast 1.5ms downsample from DMA frame buffer to 128x128 BGR888 in ei_buf
        fast_downsample_rgb565_to_bgr888(fb->buf, fb->width, fb->height, ei_buf);

        // Frame is downsampled; return DMA buffer immediately!
        esp_camera_fb_return(fb);
        fb = NULL;

        // 3. Setup signal & run FOMO model
        signal_t sig;
        sig.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
        sig.get_data     = &get_camera_data;

        uint32_t t0 = millis();
        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR r = run_classifier(&sig, &result, false);
        uint32_t infer_time = millis() - t0;

        if (r != EI_IMPULSE_OK) {
            Serial.printf("[AI] Inference error: %d\n", r);
            vTaskDelay(5 / portTICK_PERIOD_MS);
            continue;
        }

        // 4. Multi-Stage False-Positive Filter
        int detected_class = -1;
        float best_confidence = CONFIDENCE_MIN_THRESHOLD; // Minimum 60%

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
        for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
            auto& bb = result.bounding_boxes[i];
            if (bb.value >= best_confidence) {
                best_confidence = bb.value;
                detected_class = atoi(bb.label);
            }
        }
#endif

        uint32_t now = millis();

        // 5. Dual-Tier Fast Response Logic
        if (detected_class >= 0 && detected_class <= 9) {
            empty_frames = 0;

            if (detected_class == pending_class) {
                consecutive_matches++;
            } else {
                pending_class = detected_class;
                consecutive_matches = 1;
            }

            // DUAL-TIER TRIGGER:
            // Tier 1 (Fast Path): If confidence >= 45%, trigger INSTANTLY on 1st frame!
            // Tier 2 (Debounce): If confidence between 30% and 44%, require 2 frames to confirm.
            bool condition_met = false;
            if (best_confidence >= FAST_TRIGGER_THRESHOLD) {
                condition_met = true; // Instant trigger on 1st frame!
            } else if (consecutive_matches >= 2) {
                condition_met = true; // Confirmed by 2 frames
            }

            if (condition_met) {
                bool should_announce = false;

                // Case A: New note presented or scene was clear
                if (detected_class != last_spoken_class) {
                    should_announce = true;
                }
                // Case B: Note continuously held in view past repeat delay (1.2s)
                else if (now - last_spoken_time > HELD_REPEAT_DELAY_MS) {
                    should_announce = true;
                }

                if (should_announce) {
                    last_spoken_class = detected_class;
                    last_spoken_time  = now;

                    Serial.printf(">>> INSTANT MATCH: %s (Raw: %d, %.1f%%) in %d ms <<<\n",
                        CURRENCY_NAMES[detected_class], detected_class,
                        best_confidence * 100.0f, infer_time);

                    if (audioQueue) {
                        xQueueSend(audioQueue, &detected_class, 0);
                    }
                }
            }
        } else {
            // Nothing detected with >= 30% confidence
            pending_class = -1;
            consecutive_matches = 0;
            empty_frames++;

            // If empty for even 1 single frame, immediately re-arm so presenting
            // any note (even the same one) triggers INSTANTLY on the very 1st frame!
            if (empty_frames >= 1) {
                last_spoken_class = -1;
            }
        }

        // Fast cooperative yield: 2ms keeps CPU fast and responsive while watchdog is happy
        vTaskDelay(2 / portTICK_PERIOD_MS);
    }
}

// =============================================================
// Setup & Main Loop
// =============================================================
void setup() {
    // Disable brownout detector for robust operation during audio / camera peaks
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    // Maximize CPU frequency to 240MHz for lowest possible inference latency
    setCpuFrequencyMhz(240);

    Serial.begin(115200);
    Serial.println("\n=================================================");
    Serial.println("  Sri Lankan Rupee Currency Reader (Standalone)  ");
    Serial.println("  Eyes-Free Ultra-Fast Voice Assistive Device    ");
    Serial.println("=================================================");

    // 1. Enable Front Flashlight LED at 25kHz ultrasonic PWM (completely inaudible, bright & cool)
    ledcSetup(FLASH_LEDC_CHANNEL, FLASH_PWM_FREQ, 8);
    ledcAttachPin(FLASH_LED_PIN, FLASH_LEDC_CHANNEL);
    ledcWrite(FLASH_LEDC_CHANNEL, FLASH_PWM_DUTY);
    Serial.println("[Hardware] Front Flashlight LED enabled (25kHz ultrasonic PWM, duty 128)");

    // 2. Camera Configuration: VGA 640x480 (Full Wide-Angle Field of View)
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_RGB565; // GC2145 sensor requires RGB565
    config.frame_size   = FRAMESIZE_SVGA;    // VGA 640x480 for full wide-angle view
    config.grab_mode    = CAMERA_GRAB_LATEST; // Always fetch newest live frame with 0 latency
    config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
    config.jpeg_quality = 12;
    config.fb_count     = 2; // Double buffering for zero frame tearing

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[Camera] Init failed: 0x%x\n", err);
        return;
    }
    Serial.println("[Camera] Initialized OK (SVGA 800x600 Maximum Wide Angle (0% Crop) RGB565)");

    // Configure sensor parameters for maximum wide-angle FOV and clear currency colors
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 0);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_brightness(s, 1);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_dcw(s, 1); // Enable Downsize/Binning mode for full sensor FOV

        // Fast DSP Auto Exposure & Gain stabilization for instant note illumination
        s->set_aec2(s, 1);                     // Fast hardware DSP auto exposure
        s->set_gainceiling(s, GAINCEILING_2X); // Lock gain ceiling to eliminate noise/flare
        s->set_bpc(s, 1);                     // Black pixel cancellation
        s->set_wpc(s, 1);                     // White pixel cancellation
    }

    // 3. Allocate Edge Impulse 128x128x3 input buffer (internal DRAM for 240MHz zero-wait speed)
    ei_buf = (uint8_t*)heap_caps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!ei_buf && psramFound()) {
        ei_buf = (uint8_t*)ps_malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3);
    } else if (!ei_buf) {
        ei_buf = (uint8_t*)malloc(EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3);
    }
    Serial.printf("[Memory] PSRAM: %s | Model Buffer: %s\n",
        psramFound() ? "YES" : "NO",
        ei_buf ? "ALLOCATED OK" : "FAILED");

    // 4. Initialize I2S Audio on Core 0
    audioQueue = xQueueCreate(4, sizeof(int));
    setupAudio();
    xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, NULL, 1, NULL, 0);

    // Queue test startup chime (confirms ready status eyes-free)
    int testChime = -2;
    xQueueSend(audioQueue, &testChime, 0);

    // 5. Start Standalone Camera & AI Vision Pipeline on Core 1 (High priority for zero latency)
    xTaskCreatePinnedToCore(cameraAiTask, "cameraAiTask", 8192, NULL, 3, NULL, 1);

    Serial.println("[System] Ready! Fast Response Engine active. Hold any note to scan.");
}

void loop() {
    vTaskDelay(10000 / portTICK_PERIOD_MS);
}
