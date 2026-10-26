/*
 * Copyright 2026 LiveKit
 *
 * Licensed under the Apache License, Version 2.0 (the “License”);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an “AS IS” BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "livekit/capture_processing.h"

#include <algorithm>
#include <optional>

#include "modules/audio_processing/audio_buffer.h"
#include "rtc_base/synchronization/mutex.h"
#include "webrtc-sys/src/capture_processing.rs.h"

namespace livekit_ffi {
namespace {

// Обработчик из Rust и то, с чем APM последний раз проинициализировал
// переходник: назначенному позже обработчику сообщаем то же самое.
struct Slot {
  webrtc::Mutex mutex;
  std::optional<rust::Box<CaptureProcessorWrapper>> processor;
  int sample_rate_hz = 0;
  int num_channels = 0;
};

Slot& slot() {
  // Без деструктора: APM может обработать кадр и при выходе из процесса.
  static Slot* instance = new Slot();
  return *instance;
}

class CapturePostProcessing : public webrtc::CustomProcessing {
 public:
  void Initialize(int sample_rate_hz, int num_channels) override {
    Slot& s = slot();
    webrtc::MutexLock lock(&s.mutex);
    s.sample_rate_hz = sample_rate_hz;
    s.num_channels = num_channels;
    if (s.processor) {
      (*s.processor)->initialize(sample_rate_hz, num_channels);
    }
  }

  // Обработчик видит первый канал (float в диапазоне int16); остальные
  // каналы получают его результат — микрофон голоса по сути моно.
  void Process(webrtc::AudioBuffer* audio) override {
    Slot& s = slot();
    webrtc::MutexLock lock(&s.mutex);
    if (!s.processor || audio->num_channels() == 0) {
      return;
    }
    float* first = audio->channels()[0];
    const size_t frames = audio->num_frames();
    (*s.processor)->process(rust::Slice<float>(first, frames), s.sample_rate_hz);
    for (size_t ch = 1; ch < audio->num_channels(); ++ch) {
      std::copy(first, first + frames, audio->channels()[ch]);
    }
  }

  std::string ToString() const override {
    return "livekit_capture_post_processing";
  }
};

}  // namespace

std::unique_ptr<webrtc::CustomProcessing> create_capture_post_processing() {
  return std::make_unique<CapturePostProcessing>();
}

void set_capture_processor(rust::Box<CaptureProcessorWrapper> processor) {
  Slot& s = slot();
  webrtc::MutexLock lock(&s.mutex);
  if (s.sample_rate_hz > 0) {
    processor->initialize(s.sample_rate_hz, s.num_channels);
  }
  s.processor = std::move(processor);
}

void clear_capture_processor() {
  Slot& s = slot();
  webrtc::MutexLock lock(&s.mutex);
  s.processor.reset();
}

}  // namespace livekit_ffi
