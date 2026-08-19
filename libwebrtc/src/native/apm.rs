// Copyright 2025 LiveKit, Inc.
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

use cxx::UniquePtr;
use webrtc_sys::apm::ffi as sys_apm;

use crate::{RtcError, RtcErrorType};

pub struct AudioProcessingModule {
    sys_handle: UniquePtr<sys_apm::AudioProcessingModule>,
}

/// How hard the noise suppressor works.
///
/// WebRTC has had these four levels all along; what was missing here was a way
/// to ask for one. The default is `Moderate`, chosen so the suppressor never
/// damages speech — which also makes it close to inaudible on ordinary room
/// noise, and leads callers to conclude it is not running at all.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum NoiseSuppressionLevel {
    Low,
    #[default]
    Moderate,
    High,
    VeryHigh,
}

impl NoiseSuppressionLevel {
    fn as_u8(self) -> u8 {
        match self {
            Self::Low => 0,
            Self::Moderate => 1,
            Self::High => 2,
            Self::VeryHigh => 3,
        }
    }
}

impl AudioProcessingModule {
    pub fn new(
        echo_canceller_enabled: bool,
        gain_controller_enabled: bool,
        high_pass_filter_enabled: bool,
        noise_suppression_enabled: bool,
    ) -> Self {
        Self::with_noise_suppression_level(
            echo_canceller_enabled,
            gain_controller_enabled,
            high_pass_filter_enabled,
            noise_suppression_enabled,
            NoiseSuppressionLevel::default(),
        )
    }

    /// Same, but says how hard the suppressor should work.
    ///
    /// Kept separate from `new` so existing callers compile and behave exactly
    /// as before: this is an addition, not a change.
    pub fn with_noise_suppression_level(
        echo_canceller_enabled: bool,
        gain_controller_enabled: bool,
        high_pass_filter_enabled: bool,
        noise_suppression_enabled: bool,
        noise_suppression_level: NoiseSuppressionLevel,
    ) -> Self {
        Self {
            sys_handle: sys_apm::create_apm(
                echo_canceller_enabled,
                gain_controller_enabled,
                high_pass_filter_enabled,
                noise_suppression_enabled,
                noise_suppression_level.as_u8(),
            ),
        }
    }

    pub fn process_stream(
        &mut self,
        data: &mut [i16],
        sample_rate: i32,
        num_channels: i32,
    ) -> Result<(), RtcError> {
        let samples_per_10ms = (sample_rate as usize / 100) * num_channels as usize;
        assert!(
            data.len() % samples_per_10ms == 0 && data.len() >= samples_per_10ms,
            "slice must have a multiple of 10ms worth of samples"
        );

        for chunk in data.chunks_mut(samples_per_10ms) {
            if unsafe {
                self.sys_handle.pin_mut().process_stream(
                    chunk.as_mut_ptr(),
                    chunk.len(),
                    chunk.as_mut_ptr(),
                    chunk.len(),
                    sample_rate,
                    num_channels,
                )
            } != 0
            {
                return Err(RtcError {
                    error_type: RtcErrorType::Internal,
                    message: "Failed to process stream".to_string(),
                });
            }
        }

        Ok(())
    }

    pub fn process_reverse_stream(
        &mut self,
        data: &mut [i16],
        sample_rate: i32,
        num_channels: i32,
    ) -> Result<(), RtcError> {
        let samples_per_10ms = (sample_rate as usize / 100) * num_channels as usize;
        assert!(
            data.len() % samples_per_10ms == 0 && data.len() >= samples_per_10ms,
            "slice must have a multiple of 10ms worth of samples"
        );

        for chunk in data.chunks_mut(samples_per_10ms) {
            if unsafe {
                self.sys_handle.pin_mut().process_reverse_stream(
                    chunk.as_mut_ptr(),
                    chunk.len(),
                    chunk.as_mut_ptr(),
                    chunk.len(),
                    sample_rate,
                    num_channels,
                )
            } != 0
            {
                return Err(RtcError {
                    error_type: RtcErrorType::Internal,
                    message: "Failed to process reverse stream".to_string(),
                });
            }
        }

        Ok(())
    }

    pub fn set_stream_delay_ms(&mut self, delay_ms: i32) -> Result<(), RtcError> {
        if self.sys_handle.pin_mut().set_stream_delay_ms(delay_ms) == 0 {
            Ok(())
        } else {
            Err(RtcError {
                error_type: RtcErrorType::Internal,
                message: "Failed to set stream delay".to_string(),
            })
        }
    }
}
