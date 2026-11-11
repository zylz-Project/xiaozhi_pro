#include "es8311_audio_codec.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <array>

#define TAG "Es8311AudioCodec"

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port,
                                   int input_sample_rate, int output_sample_rate, gpio_num_t mclk,
                                   gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
                                   gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk,
                                   bool pa_inverted, float pa_voltage, bool dac_reference,
                                   float pa_gain_db, float max_output_volume_db,
                                   float speaker_makeup_gain_db, int default_output_volume,
                                   float reference_mic_gain_db) {
    duplex_ = true;  // 是否双工
    output_volume_ = std::clamp(default_output_volume, 0, 100);
    input_reference_ = dac_reference;
    input_channels_ = dac_reference ? 2 : 1;  // ADCL microphone, DACR digital reference.
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    // Keep the high-volume segment monotonic above its 80 / -10 dB anchor.
    max_output_volume_db_ = std::clamp(max_output_volume_db, -10.0f, 0.0f);
    speaker_makeup_gain_db_ = std::clamp(speaker_makeup_gain_db, 0.0f, 21.0f);
    // Close speaker/microphone placement needs ADC headroom for full duplex;
    // saturated echo cannot be reconstructed by a linear echo canceller.
    input_gain_ = dac_reference ? std::clamp(reference_mic_gain_db, 0.0f, 42.0f) : 36.0f;

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);
    ResetCodec();

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = pa_voltage;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.hw_gain.pa_gain = pa_gain_db;
    es8311_cfg.pa_reverted = pa_inverted_;
    es8311_cfg.no_dac_ref = false;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Es8311AudioCodec");
    } else {
        ESP_LOGI(TAG, "ES8311 output: PA %.1f V, PA gain %.1f dB, maximum route %.1f dB",
                 pa_voltage, pa_gain_db, max_output_volume_db_);
    }
}

void Es8311AudioCodec::ResetCodec() {
    // Hold the ES8311 digital blocks in reset for several milliseconds, as
    // recommended by the initialization guide. Normal codec initialization
    // releases the reset and starts the state machine.
    uint8_t reset_value = 0x1F;
    ESP_ERROR_CHECK(
        static_cast<esp_err_t>(ctrl_if_->write_reg(ctrl_if_, 0x00, 1, &reset_value, 1)));
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGI(TAG, "ES8311 software reset complete");
}

Es8311AudioCodec::~Es8311AudioCodec() {
    esp_codec_dev_delete(dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::UpdateDeviceState() {
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = static_cast<uint8_t>(input_channels_),
            .channel_mask = 0,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
        if (max_output_volume_db_ < 0.0f) {
            // The vendor copies the curve. Reapply on every device recreation,
            // before restoring the user's persisted 0..100 volume selection.
            esp_codec_dev_vol_map_t points[] = {
                {0, -50.0f}, {80, -10.0f}, {100, max_output_volume_db_}};
            esp_codec_dev_vol_curve_t curve = {.vol_map = points, .count = 3};
            ESP_ERROR_CHECK(esp_codec_dev_set_vol_curve(dev_, &curve));
        }
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
        if (input_reference_) {
            // Restore the requested mux after every open/resume, rather than
            // depending on the vendor's initialization-only mux write.
            ESP_ERROR_CHECK(esp_codec_dev_write_reg(dev_, 0x44, 0x58));
            int route = 0, input_format = 0, output_format = 0, gain = 0;
            ESP_ERROR_CHECK(esp_codec_dev_read_reg(dev_, 0x44, &route));
            ESP_ERROR_CHECK(esp_codec_dev_read_reg(dev_, 0x09, &input_format));
            ESP_ERROR_CHECK(esp_codec_dev_read_reg(dev_, 0x0a, &output_format));
            ESP_ERROR_CHECK(esp_codec_dev_read_reg(dev_, 0x16, &gain));
            ESP_LOGI(TAG,
                     "MR route: reg44=0x%02x, SDP in=0x%02x out=0x%02x, gain=%d dB reg16=0x%02x",
                     route, input_format, output_format, static_cast<int>(input_gain_), gain);
        }
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_close(dev_));
        esp_codec_dev_delete(dev_);
        dev_ = nullptr;
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                                            gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            {
                .sample_rate_hz = (uint32_t)output_sample_rate_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
#endif
            },
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                     .slot_mode = I2S_SLOT_MODE_STEREO,
                     .slot_mask = I2S_STD_SLOT_BOTH,
                     .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                     .ws_pol = false,
                     .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                     .left_align = true,
                     .big_endian = false,
                     .bit_order_lsb = false
#endif
        },
        .gpio_cfg = {.mclk = mclk,
                     .bclk = bclk,
                     .ws = ws,
                     .dout = dout,
                     .din = din,
                     .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8311AudioCodec::SetOutputVolume(int volume) {
    volume = std::clamp(volume, 0, 100);
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (dev_ != nullptr) {
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, volume));
    }
    AudioCodec::SetOutputVolume(volume);
}

void Es8311AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
    if (input_enabled_) {
        const int result = esp_codec_dev_read(dev_, dest, samples * sizeof(int16_t));
        if (result == ESP_CODEC_DEV_OK) {
            return samples;
        }
        ESP_LOGW(TAG, "Codec capture failed: %d", result);
    }
    return 0;
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        if (input_reference_) {
            // The shared IN_OUT codec is opened as stereo to capture ADCL/DACR.
            // Keep logical playback mono and duplicate each sample explicitly:
            // writing unexpanded mono here halves the duration and changes pitch.
            std::array<int16_t, 480> stereo;
            int written = 0;
            while (written < samples) {
                const int frames = std::min(samples - written, 240);
                for (int i = 0; i < frames; ++i) {
                    stereo[i * 2] = stereo[i * 2 + 1] = data[written + i];
                }
                const int result =
                    esp_codec_dev_write(dev_, stereo.data(), frames * 2 * sizeof(int16_t));
                if (result != ESP_CODEC_DEV_OK) {
                    ESP_LOGW(TAG, "Codec playback failed: %d", result);
                    return written;
                }
                written += frames;
            }
            return written;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
