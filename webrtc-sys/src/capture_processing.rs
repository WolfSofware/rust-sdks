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

//! Обработка захвата после APM: свой шумодав на звуке Platform ADM.

use std::sync::Arc;

#[cxx::bridge(namespace = "livekit_ffi")]
pub mod ffi {
    extern "Rust" {
        type CaptureProcessorWrapper;

        fn initialize(self: &CaptureProcessorWrapper, sample_rate_hz: i32, num_channels: i32);
        fn process(self: &CaptureProcessorWrapper, samples: &mut [f32], sample_rate_hz: i32);
    }

    unsafe extern "C++" {
        include!("livekit/capture_processing.h");

        fn set_capture_processor(processor: Box<CaptureProcessorWrapper>);
        fn clear_capture_processor();
    }
}

/// Обработчик захвата. Зовётся из звукового потока WebRTC на каждые 10 мс:
/// `samples` — первый канал, float в диапазоне int16, менять на месте.
pub trait CaptureProcessor: Send + Sync {
    fn initialize(&self, sample_rate_hz: i32, num_channels: i32);
    fn process(&self, samples: &mut [f32], sample_rate_hz: i32);
}

pub struct CaptureProcessorWrapper {
    inner: Arc<dyn CaptureProcessor>,
}

impl CaptureProcessorWrapper {
    pub fn new(inner: Arc<dyn CaptureProcessor>) -> Self {
        Self { inner }
    }

    fn initialize(&self, sample_rate_hz: i32, num_channels: i32) {
        self.inner.initialize(sample_rate_hz, num_channels);
    }

    fn process(&self, samples: &mut [f32], sample_rate_hz: i32) {
        self.inner.process(samples, sample_rate_hz);
    }
}
