// Copyright 2026 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Своя обработка захвата Platform ADM после APM (эхо, шум, усиление) —
//! например, нейросетевой шумодав. Обработчик один на процесс.

use std::sync::Arc;

pub use webrtc_sys::capture_processing::CaptureProcessor;
use webrtc_sys::capture_processing::{ffi, CaptureProcessorWrapper};

/// Назначить обработчик захвата; прежний снимается.
pub fn set_capture_processor(processor: Arc<dyn CaptureProcessor>) {
    ffi::set_capture_processor(Box::new(CaptureProcessorWrapper::new(processor)));
}

/// Снять обработчик: захват идёт после APM как есть.
pub fn clear_capture_processor() {
    ffi::clear_capture_processor();
}
