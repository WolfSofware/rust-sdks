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

#pragma once

#include <memory>

#include "api/audio/audio_processing.h"
#include "rust/cxx.h"

namespace livekit_ffi {

class CaptureProcessorWrapper;

// Обработка захвата после APM (эхо, шум, усиление): свой шумодав на звуке,
// который захватывает Platform ADM. Фабрика ставит переходник в APM один раз,
// а обработчик из Rust назначается и снимается на лету — фабрика в процессе
// одна, поэтому слот общий.
std::unique_ptr<webrtc::CustomProcessing> create_capture_post_processing();

void set_capture_processor(rust::Box<CaptureProcessorWrapper> processor);
void clear_capture_processor();

}  // namespace livekit_ffi
