/*
 * Copyright 2025 LiveKit, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <memory>

#include "api/scoped_refptr.h"
#include "api/video_codecs/video_decoder_factory.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "modules/audio_processing/aec3/echo_canceller3.h"
#include "modules/audio_processing/audio_buffer.h"

namespace livekit_ffi {

struct AudioProcessingConfig {
  bool echo_canceller_enabled;
  bool gain_controller_enabled;
  bool high_pass_filter_enabled;
  bool noise_suppression_enabled;
  // How hard the suppressor works: 0 low, 1 moderate, 2 high, 3 very high.
  //
  // Upstream only exposes `enabled` and leaves the level at WebRTC's default
  // (moderate), which is deliberately gentle so it never damages speech. On
  // ordinary room noise that is close to inaudible, and callers reasonably
  // conclude the suppressor is not working at all. Out of range falls back to
  // moderate.
  //
  // The in-class default matters: `create_apm` default-constructs this struct,
  // and without an initializer the field would hold whatever zero-init leaves
  // there — which maps to Low and would quietly change behaviour for anyone who
  // does not set it.
  uint8_t noise_suppression_level = 1;

  webrtc::AudioProcessing::Config ToWebrtcConfig() const {
    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = echo_canceller_enabled;
    config.gain_controller2.enabled = gain_controller_enabled;
    config.gain_controller2.adaptive_digital.enabled = gain_controller_enabled;
    config.high_pass_filter.enabled = high_pass_filter_enabled;
    config.noise_suppression.enabled = noise_suppression_enabled;
    using NsLevel = webrtc::AudioProcessing::Config::NoiseSuppression::Level;
    switch (noise_suppression_level) {
      case 0:
        config.noise_suppression.level = NsLevel::kLow;
        break;
      case 2:
        config.noise_suppression.level = NsLevel::kHigh;
        break;
      case 3:
        config.noise_suppression.level = NsLevel::kVeryHigh;
        break;
      default:
        config.noise_suppression.level = NsLevel::kModerate;
        break;
    }
    return config;
  }
};

class AudioProcessingModule {
 public:
  AudioProcessingModule(const AudioProcessingConfig& config);

  int process_stream(const int16_t* src,
                     size_t src_len,
                     int16_t* dst,
                     size_t dst_len,
                     int sample_rate,
                     int num_channels);

  int process_reverse_stream(const int16_t* src,
                             size_t src_len,
                             int16_t* dst,
                             size_t dst_len,
                             int sample_rate,
                             int num_channels);

  int set_stream_delay_ms(int delay_ms);

 private:
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
};

std::unique_ptr<AudioProcessingModule> create_apm(
    bool echo_canceller_enabled,
    bool gain_controller_enabled,
    bool high_pass_filter_enabled,
    bool noise_suppression_enabled,
    uint8_t noise_suppression_level);

}  // namespace livekit_ffi
