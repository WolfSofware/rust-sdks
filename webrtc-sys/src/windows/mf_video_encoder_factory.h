/*
 * Copyright 2026 WolfTeam
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

#ifndef LIVEKIT_MF_VIDEO_ENCODER_FACTORY_H_
#define LIVEKIT_MF_VIDEO_ENCODER_FACTORY_H_

#include <memory>
#include <vector>

#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"

namespace webrtc {

// Аппаратный H.264 на Windows через Media Foundation: один путь для NVIDIA,
// AMD и Intel — каждый драйвер регистрирует свой кодировщик (MFT), и мы
// выбираем среди них по видеокарте:
//   дискретные NVIDIA → AMD → Intel, затем встроенные AMD → Intel.
// Не запустился выбранный — следующий по списку; не запустился никакой —
// программный H.264 (OpenH264): кодек к этому моменту уже согласован.
class MediaFoundationVideoEncoderFactory : public VideoEncoderFactory {
 public:
  MediaFoundationVideoEncoderFactory();
  ~MediaFoundationVideoEncoderFactory() override;

  // Есть ли в системе хотя бы один аппаратный кодировщик H.264. Ответ
  // кэшируется на время жизни процесса.
  static bool IsSupported();

  std::unique_ptr<VideoEncoder> Create(const Environment& env,
                                       const SdpVideoFormat& format) override;

  std::vector<SdpVideoFormat> GetSupportedFormats() const override;

  std::vector<SdpVideoFormat> GetImplementations() const override;

 private:
  std::vector<SdpVideoFormat> supported_formats_;
};

}  // namespace webrtc

#endif  // LIVEKIT_MF_VIDEO_ENCODER_FACTORY_H_
