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

#include "mf_video_encoder_factory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d10.h>
#include <d3d11_3.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>
// CODECAPI_* — GUID'ы из codecapi.h: определяем их здесь, а не ищем в
// библиотеке. selectany не даст дублей, если кто-то сделает так же.
#include <initguid.h>
#include <codecapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "api/environment/environment.h"
#include "api/video/i420_buffer.h"
#include "api/video/video_frame.h"
#include "common_video/h264/h264_bitstream_parser.h"
#include "common_video/h264/h264_common.h"
#include "modules/video_coding/codecs/h264/include/h264.h"
#include "modules/video_coding/include/video_codec_interface.h"
#include "modules/video_coding/include/video_error_codes.h"
#include "rtc_base/logging.h"
#include "third_party/libyuv/include/libyuv/convert_from.h"

namespace webrtc {

namespace {

using Microsoft::WRL::ComPtr;

// MFT_ENUM_ADAPTER_LUID (mfapi.h, Windows 10 1703+): у какой видеокарты этот
// кодировщик. Свой экземпляр GUID'а — чтобы не зависеть от того, с какой
// версией заголовков собирают; нет атрибута — смотрим на код производителя.
constexpr GUID kMftEnumAdapterLuid = {
    0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};

constexpr UINT kVendorNvidia = 0x10DE;
constexpr UINT kVendorAmd = 0x1002;
constexpr UINT kVendorIntel = 0x8086;

// Сколько кадров держим в очереди на вход, пока кодировщик не попросит ещё.
// Больше — растёт задержка: лучше выбросить старый кадр экрана.
constexpr size_t kMaxPendingInputs = 3;

// RTC_LOG не понимает std::hex — HRESULT печатаем сами.
std::string Hex(HRESULT hr) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08lx", static_cast<unsigned long>(hr));
  return text;
}

// Вызывающих потоков несколько (очередь кодирования WebRTC, наш поток
// событий), а Media Foundation нужен COM. Отпускать COM не будем: потоки живут
// долго, а преждевременный CoUninitialize уронил бы живые объекты.
void EnsureCom() {
  thread_local bool done = false;
  if (done) {
    return;
  }
  // RPC_E_CHANGED_MODE — поток уже в STA. Media Foundation работает и там.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  done = true;
}

bool EnsureMediaFoundation() {
  static const bool started = [] {
    EnsureCom();
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MFStartup failed: " << Hex(hr);
      return false;
    }
    return true;
  }();
  return started;
}

std::string Narrow(const wchar_t* wide) {
  if (!wide) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr,
                                 nullptr);
  if (size <= 1) {
    return {};
  }
  std::string out(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), size, nullptr,
                      nullptr);
  return out;
}

const char* VendorName(UINT vendor_id) {
  switch (vendor_id) {
    case kVendorNvidia:
      return "NVIDIA";
    case kVendorAmd:
      return "AMD";
    case kVendorIntel:
      return "Intel";
    default:
      return "other";
  }
}

// Порядок производителей внутри одного класса карт.
int VendorOrder(UINT vendor_id) {
  switch (vendor_id) {
    case kVendorNvidia:
      return 0;
    case kVendorAmd:
      return 1;
    case kVendorIntel:
      return 2;
    default:
      return 3;
  }
}

// Один аппаратный кодировщик H.264 и видеокарта, к которой он относится.
struct Candidate {
  ComPtr<IMFActivate> activate;
  std::string name;
  std::optional<LUID> luid;
  UINT vendor_id = 0;
  // Встройка (общая с процессором память). Для кодирования важен не
  // графический чип, а отдельный блок кодирования, и у дискретных карт он
  // заметно лучше — поэтому дискретные всегда впереди.
  bool integrated = false;
  int rank = 0;
};

ComPtr<IDXGIAdapter1> FindAdapter(const std::optional<LUID>& luid) {
  if (!luid) {
    return nullptr;
  }
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
        desc.AdapterLuid.LowPart == luid->LowPart &&
        desc.AdapterLuid.HighPart == luid->HighPart) {
      return adapter;
    }
    adapter.Reset();
  }
  return nullptr;
}

// D3D11-устройство на нужной видеокарте: и для признака «встройка», и для
// менеджера устройств, который кодировщику отдаём. nullptr — по умолчанию.
ComPtr<ID3D11Device> CreateDevice(IDXGIAdapter1* adapter) {
  ComPtr<ID3D11Device> device;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                      D3D_FEATURE_LEVEL_11_0};
  UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  HRESULT hr = D3D11CreateDevice(
      adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
      nullptr, flags, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device,
      nullptr, nullptr);
  if (FAILED(hr)) {
    return nullptr;
  }
  // Кодировщик дёргает устройство из своих потоков.
  ComPtr<ID3D10Multithread> multithread;
  if (SUCCEEDED(device.As(&multithread))) {
    multithread->SetMultithreadProtected(TRUE);
  }
  return device;
}

bool IsIntegrated(ID3D11Device* device) {
  D3D11_FEATURE_DATA_D3D11_OPTIONS2 options{};
  if (device &&
      SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2,
                                            &options, sizeof(options)))) {
    return options.UnifiedMemoryArchitecture == TRUE;
  }
  return false;
}

UINT VendorFromAttribute(IMFActivate* activate) {
  wchar_t* value = nullptr;
  UINT32 length = 0;
  UINT vendor = 0;
  if (SUCCEEDED(activate->GetAllocatedString(
          MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &value, &length)) &&
      value) {
    // Формат «VEN_10DE».
    const wchar_t* hex = wcsstr(value, L"VEN_");
    if (hex) {
      vendor = static_cast<UINT>(wcstoul(hex + 4, nullptr, 16));
    }
    CoTaskMemFree(value);
  }
  return vendor;
}

// Все аппаратные кодировщики H.264 (NV12 → H.264), лучшие первыми.
std::vector<Candidate> EnumerateCandidates() {
  std::vector<Candidate> candidates;
  if (!EnsureMediaFoundation()) {
    return candidates;
  }
  EnsureCom();

  MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activates = nullptr;
  UINT32 count = 0;
  HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                         MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         &input, &output, &activates, &count);
  if (FAILED(hr)) {
    RTC_LOG(LS_WARNING) << "MFTEnumEx failed: " << Hex(hr);
    return candidates;
  }

  for (UINT32 i = 0; i < count; ++i) {
    Candidate c;
    c.activate.Attach(activates[i]);

    wchar_t* name = nullptr;
    UINT32 name_length = 0;
    if (SUCCEEDED(c.activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                 &name, &name_length))) {
      c.name = Narrow(name);
      CoTaskMemFree(name);
    }

    LUID luid{};
    UINT32 blob_size = 0;
    if (SUCCEEDED(c.activate->GetBlob(kMftEnumAdapterLuid,
                                      reinterpret_cast<UINT8*>(&luid),
                                      sizeof(luid), &blob_size)) &&
        blob_size == sizeof(luid)) {
      c.luid = luid;
    }

    ComPtr<IDXGIAdapter1> adapter = FindAdapter(c.luid);
    if (adapter) {
      DXGI_ADAPTER_DESC1 desc{};
      adapter->GetDesc1(&desc);
      c.vendor_id = desc.VendorId;
    } else {
      c.vendor_id = VendorFromAttribute(c.activate.Get());
    }
    ComPtr<ID3D11Device> device = CreateDevice(adapter.Get());
    c.integrated = IsIntegrated(device.Get());
    c.rank = (c.integrated ? 10 : 0) + VendorOrder(c.vendor_id);
    candidates.push_back(std::move(c));
  }
  CoTaskMemFree(activates);

  std::stable_sort(candidates.begin(), candidates.end(),
                   [](const Candidate& a, const Candidate& b) {
                     return a.rank < b.rank;
                   });
  return candidates;
}

std::string Describe(const Candidate& c) {
  return c.name + " [" + VendorName(c.vendor_id) +
         (c.integrated ? ", встроенная" : ", дискретная") + "]";
}

HRESULT SetCodecValue(ICodecAPI* api, const GUID& key, UINT32 value) {
  if (!api) {
    return E_POINTER;
  }
  VARIANT var;
  VariantInit(&var);
  var.vt = VT_UI4;
  var.ulVal = value;
  return api->SetValue(&key, &var);
}

// То, что нужно от входного кадра, чтобы собрать выходной: кодировщик
// возвращает только время, остальное ищем по нему.
struct FrameInfo {
  LONGLONG time_hns = 0;
  uint32_t rtp_timestamp = 0;
  int64_t ntp_time_ms = 0;
  int64_t capture_time_ms = 0;
  VideoRotation rotation = kVideoRotation_0;
  std::optional<ColorSpace> color_space;
};

class MediaFoundationH264Encoder : public VideoEncoder {
 public:
  MediaFoundationH264Encoder(const Environment& env,
                             const SdpVideoFormat& format,
                             std::vector<Candidate> candidates)
      : env_(env), format_(format), candidates_(std::move(candidates)) {}

  ~MediaFoundationH264Encoder() override { Release(); }

  int32_t InitEncode(const VideoCodec* codec,
                     const VideoEncoder::Settings& settings) override {
    if (!codec || codec->codecType != kVideoCodecH264 ||
        codec->maxFramerate == 0 || codec->width < 2 || codec->height < 2) {
      return WEBRTC_VIDEO_CODEC_ERR_PARAMETER;
    }
    Release();
    codec_ = *codec;
    settings_ = settings;
    // NV12 и кодировщики требуют чётных сторон; нечётный край отрежем.
    width_ = codec_.width & ~1;
    height_ = codec_.height & ~1;
    fps_ = std::max<uint32_t>(1, codec_.maxFramerate);
    target_bps_ = std::max<uint32_t>(codec_.startBitrate, 100) * 1000;
    // Заново, а не прежний список: однажды выключенный (ShutdownObject)
    // кодировщик повторно включать ненадёжно, а InitEncode приходит и на
    // каждую смену разрешения.
    candidates_ = EnumerateCandidates();
    return StartFromCandidate(0);
  }

  int32_t RegisterEncodeCompleteCallback(
      EncodedImageCallback* callback) override {
    callback_ = callback;
    if (software_) {
      software_->RegisterEncodeCompleteCallback(callback);
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Release() override {
    StopMft();
    if (software_) {
      software_->Release();
      software_.reset();
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  int32_t Encode(const VideoFrame& frame,
                 const std::vector<VideoFrameType>* frame_types) override {
    // Кодировщик сломался посреди трансляции — следующий по списку, без
    // остановки потока: кодек тот же, нужен только ключевой кадр.
    if (failed_.load()) {
      RTC_LOG(LS_WARNING) << "MF H264: кодировщик " << CurrentName()
                          << " отказал, переключаемся";
      StopMft();
      int32_t result = StartFromCandidate(candidate_index_ + 1);
      if (result != WEBRTC_VIDEO_CODEC_OK) {
        return result;
      }
    }
    if (software_) {
      return software_->Encode(frame, frame_types);
    }
    if (!mft_ || !callback_) {
      return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
    }
    if (!sending_) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }
    if (frame_types && !frame_types->empty() &&
        (*frame_types)[0] == VideoFrameType::kEmptyFrame) {
      return WEBRTC_VIDEO_CODEC_NO_OUTPUT;
    }

    bool key_frame = key_frame_request_ ||
                     (frame_types && !frame_types->empty() &&
                      (*frame_types)[0] == VideoFrameType::kVideoFrameKey);

    ComPtr<IMFSample> sample = MakeSample(frame);
    if (!sample) {
      return WEBRTC_VIDEO_CODEC_ENCODER_FAILURE;
    }
    if (key_frame) {
      SetCodecValue(codec_api_.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
      key_frame_request_ = false;
    }

    FrameInfo info;
    info.time_hns = frame.timestamp_us() * 10;
    info.rtp_timestamp = frame.rtp_timestamp();
    info.ntp_time_ms = frame.ntp_time_ms();
    info.capture_time_ms = frame.render_time_ms();
    info.rotation = frame.rotation();
    info.color_space = frame.color_space();

    if (async_) {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_infos_.push_back(info);
      pending_.push_back(sample);
      while (pending_.size() > kMaxPendingInputs) {
        pending_.pop_front();
        frame_infos_.pop_front();
      }
      FeedLocked();
      return WEBRTC_VIDEO_CODEC_OK;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      frame_infos_.push_back(info);
    }
    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "MF H264 ProcessInput: " << Hex(hr);
      failed_ = true;
      return WEBRTC_VIDEO_CODEC_OK;
    }
    while (DrainOneOutput()) {
    }
    return WEBRTC_VIDEO_CODEC_OK;
  }

  void SetRates(const RateControlParameters& parameters) override {
    if (software_) {
      software_->SetRates(parameters);
      return;
    }
    const uint32_t bps = parameters.bitrate.get_sum_bps();
    if (bps == 0) {
      sending_ = false;
      return;
    }
    if (!sending_) {
      // Поток не шёл — получателю нужен ключевой кадр.
      key_frame_request_ = true;
    }
    sending_ = true;
    if (bps != target_bps_ && mft_) {
      target_bps_ = bps;
      HRESULT hr = SetCodecValue(codec_api_.Get(),
                                 CODECAPI_AVEncCommonMeanBitRate, bps);
      if (FAILED(hr)) {
        RTC_LOG(LS_VERBOSE) << "MF H264: битрейт на лету не меняется ("
                            << Hex(hr) << ")";
      }
    }
  }

  EncoderInfo GetEncoderInfo() const override {
    if (software_) {
      return software_->GetEncoderInfo();
    }
    EncoderInfo info;
    info.supports_native_handle = false;
    info.implementation_name = "MediaFoundation H264";
    info.scaling_settings = VideoEncoder::ScalingSettings::kOff;
    info.is_hardware_accelerated = true;
    info.supports_simulcast = false;
    info.preferred_pixel_formats = {VideoFrameBuffer::Type::kI420};
    return info;
  }

 private:
  std::string CurrentName() const {
    return candidate_index_ < candidates_.size()
               ? Describe(candidates_[candidate_index_])
               : std::string("—");
  }

  // Запустить первый работающий кодировщик начиная с `first`; ни один не
  // пошёл — программный H.264.
  int32_t StartFromCandidate(size_t first) {
    failed_ = false;
    for (size_t i = first; i < candidates_.size(); ++i) {
      candidate_index_ = i;
      if (StartMft(candidates_[i])) {
        RTC_LOG(LS_INFO) << "MF H264: кодирует " << Describe(candidates_[i])
                         << ", " << width_ << "x" << height_ << " @ " << fps_
                         << "fps, " << target_bps_ << " bps"
                         << (async_ ? "" : " (синхронный)");
        sending_ = true;
        key_frame_request_ = true;
        return WEBRTC_VIDEO_CODEC_OK;
      }
      RTC_LOG(LS_WARNING) << "MF H264: " << Describe(candidates_[i])
                          << " не запустился, пробуем следующий";
      StopMft();
    }
    candidate_index_ = candidates_.size();
    RTC_LOG(LS_WARNING)
        << "MF H264: аппаратных кодировщиков не осталось — программный H264";
    software_ = CreateH264Encoder(env_, H264EncoderSettings::Parse(format_));
    if (callback_) {
      software_->RegisterEncodeCompleteCallback(callback_);
    }
    return software_->InitEncode(&codec_, settings_);
  }

  bool StartMft(const Candidate& candidate) {
    EnsureCom();
    HRESULT hr = candidate.activate->ActivateObject(IID_PPV_ARGS(&mft_));
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF H264 ActivateObject: " << Hex(hr);
      return false;
    }
    activated_ = candidate.activate;

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(mft_->GetAttributes(&attributes))) {
      UINT32 is_async = 0;
      async_ = SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) &&
               is_async;
      if (async_) {
        attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      }
      attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    // Менеджер устройств на ТОЙ ЖЕ видеокарте, что и кодировщик: на машине с
    // двумя картами чужое устройство кодировщик не примет. Не принял
    // менеджер вовсе — работаем без него, с кадрами в памяти.
    device_ = CreateDevice(FindAdapter(candidate.luid).Get());
    if (device_ &&
        SUCCEEDED(MFCreateDXGIDeviceManager(&reset_token_, &device_manager_)) &&
        SUCCEEDED(device_manager_->ResetDevice(device_.Get(), reset_token_))) {
      hr = mft_->ProcessMessage(
          MFT_MESSAGE_SET_D3D_MANAGER,
          reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
      if (FAILED(hr)) {
        RTC_LOG(LS_INFO) << "MF H264: без менеджера D3D (" << Hex(hr)
                         << ")";
        device_manager_.Reset();
      }
    }

    // Параметры кодирования — до типов: часть кодировщиков иначе их не примет.
    mft_.As(&codec_api_);
    SetCodecValue(codec_api_.Get(), CODECAPI_AVEncCommonRateControlMode,
                  eAVEncCommonRateControlMode_CBR);
    SetCodecValue(codec_api_.Get(), CODECAPI_AVEncCommonMeanBitRate,
                  target_bps_);
    SetCodecValue(codec_api_.Get(), CODECAPI_AVLowLatencyMode, TRUE);
    SetCodecValue(codec_api_.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
    // Ключевые кадры — по запросу получателей, а не по расписанию.
    SetCodecValue(codec_api_.Get(), CODECAPI_AVEncMPVGOPSize, 0);

    if (!SetOutputType() || !SetInputType()) {
      return false;
    }

    MFT_OUTPUT_STREAM_INFO stream_info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(0, &stream_info))) {
      provides_samples_ =
          (stream_info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                  MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
      output_buffer_size_ = stream_info.cbSize;
    }
    RefreshSequenceHeader();

    if (FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
        FAILED(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
      RTC_LOG(LS_WARNING) << "MF H264: не начался поток";
      return false;
    }

    if (async_) {
      if (FAILED(mft_.As(&events_))) {
        return false;
      }
      running_ = true;
      event_thread_ = std::thread([this] { EventLoop(); });
    }
    return true;
  }

  bool SetOutputType() {
    for (UINT32 profile : {static_cast<UINT32>(eAVEncH264VProfile_ConstrainedBase),
                           static_cast<UINT32>(eAVEncH264VProfile_Base)}) {
      ComPtr<IMFMediaType> type;
      if (FAILED(MFCreateMediaType(&type))) {
        return false;
      }
      type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
      type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
      type->SetUINT32(MF_MT_AVG_BITRATE, target_bps_);
      MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
      MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_, 1);
      MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
      type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
      type->SetUINT32(MF_MT_MPEG2_PROFILE, profile);
      HRESULT hr = mft_->SetOutputType(0, type.Get(), 0);
      if (SUCCEEDED(hr)) {
        return true;
      }
      RTC_LOG(LS_INFO) << "MF H264 SetOutputType(profile " << profile
                       << "): " << Hex(hr);
    }
    return false;
  }

  bool SetInputType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) {
      return false;
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, width_);
    HRESULT hr = mft_->SetInputType(0, type.Get(), 0);
    if (FAILED(hr)) {
      RTC_LOG(LS_WARNING) << "MF H264 SetInputType: " << Hex(hr);
      return false;
    }
    return true;
  }

  // SPS/PPS из типа на выходе: часть кодировщиков кладёт их только туда, а
  // ключевой кадр без них получатель не откроет.
  void RefreshSequenceHeader() {
    ComPtr<IMFMediaType> type;
    if (FAILED(mft_->GetOutputCurrentType(0, &type))) {
      return;
    }
    UINT32 size = 0;
    if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) &&
        size > 0) {
      std::vector<uint8_t> header(size);
      if (SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header.data(),
                                  size, nullptr))) {
        std::lock_guard<std::mutex> lock(mutex_);
        sequence_header_ = std::move(header);
      }
    }
  }

  void StopMft() {
    running_ = false;
    if (mft_) {
      mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    // Shutdown будит поток, ждущий GetEvent.
    if (activated_) {
      activated_->ShutdownObject();
    }
    if (event_thread_.joinable()) {
      event_thread_.join();
    }
    events_.Reset();
    codec_api_.Reset();
    mft_.Reset();
    activated_.Reset();
    device_manager_.Reset();
    device_.Reset();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.clear();
      frame_infos_.clear();
      need_input_ = 0;
      sequence_header_.clear();
    }
    async_ = false;
  }

  ComPtr<IMFSample> MakeSample(const VideoFrame& frame) {
    scoped_refptr<I420BufferInterface> i420 =
        frame.video_frame_buffer()->ToI420();
    if (!i420) {
      return nullptr;
    }
    // Размер разошёлся с настроенным (нечётный край, смена окна) —
    // приводим к нему, а не роняем кадр.
    if (i420->width() != width_ || i420->height() != height_) {
      scoped_refptr<I420Buffer> scaled = I420Buffer::Create(width_, height_);
      scaled->ScaleFrom(*i420);
      i420 = scaled;
    }

    const DWORD size = static_cast<DWORD>(width_ * height_ * 3 / 2);
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(size, &buffer))) {
      return nullptr;
    }
    BYTE* data = nullptr;
    if (FAILED(buffer->Lock(&data, nullptr, nullptr))) {
      return nullptr;
    }
    libyuv::I420ToNV12(i420->DataY(), i420->StrideY(), i420->DataU(),
                       i420->StrideU(), i420->DataV(), i420->StrideV(), data,
                       width_, data + width_ * height_, width_, width_,
                       height_);
    buffer->Unlock();
    buffer->SetCurrentLength(size);

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)) ||
        FAILED(sample->AddBuffer(buffer.Get()))) {
      return nullptr;
    }
    sample->SetSampleTime(frame.timestamp_us() * 10);
    sample->SetSampleDuration(10'000'000LL / fps_);
    return sample;
  }

  // Под mutex_: отдать кодировщику очередной кадр, если он готов принять.
  void FeedLocked() {
    while (need_input_ > 0 && !pending_.empty()) {
      ComPtr<IMFSample> sample = pending_.front();
      pending_.pop_front();
      --need_input_;
      HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "MF H264 ProcessInput: " << Hex(hr);
        failed_ = true;
        return;
      }
    }
  }

  void EventLoop() {
    EnsureCom();
    while (running_) {
      ComPtr<IMFMediaEvent> event;
      HRESULT hr = events_->GetEvent(0, &event);
      if (FAILED(hr)) {
        if (running_) {
          RTC_LOG(LS_ERROR) << "MF H264 GetEvent: " << Hex(hr);
          failed_ = true;
        }
        return;
      }
      MediaEventType type = MEUnknown;
      event->GetType(&type);
      HRESULT status = S_OK;
      event->GetStatus(&status);
      if (FAILED(status)) {
        RTC_LOG(LS_ERROR) << "MF H264 событие " << type << ": " << Hex(status);
        failed_ = true;
        return;
      }
      switch (type) {
        case METransformNeedInput: {
          std::lock_guard<std::mutex> lock(mutex_);
          ++need_input_;
          FeedLocked();
          break;
        }
        case METransformHaveOutput:
          DrainOneOutput();
          break;
        case MEError:
          failed_ = true;
          return;
        default:
          break;
      }
    }
  }

  // Забрать один готовый кадр. false — больше нечего (или ошибка).
  bool DrainOneOutput() {
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    ComPtr<IMFSample> own_sample;
    if (!provides_samples_) {
      ComPtr<IMFMediaBuffer> buffer;
      DWORD size = output_buffer_size_ ? output_buffer_size_
                                       : static_cast<DWORD>(width_ * height_ * 3 / 2);
      if (FAILED(MFCreateMemoryBuffer(size, &buffer)) ||
          FAILED(MFCreateSample(&own_sample)) ||
          FAILED(own_sample->AddBuffer(buffer.Get()))) {
        return false;
      }
      output.pSample = own_sample.Get();
    }

    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
    if (output.pEvents) {
      output.pEvents->Release();
    }
    ComPtr<IMFSample> sample;
    if (provides_samples_) {
      sample.Attach(output.pSample);
    } else {
      sample = own_sample;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
      return false;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
      ComPtr<IMFMediaType> type;
      if (SUCCEEDED(mft_->GetOutputAvailableType(0, 0, &type))) {
        mft_->SetOutputType(0, type.Get(), 0);
      }
      RefreshSequenceHeader();
      return true;
    }
    if (FAILED(hr)) {
      RTC_LOG(LS_ERROR) << "MF H264 ProcessOutput: " << Hex(hr);
      failed_ = true;
      return false;
    }
    if (sample) {
      Deliver(sample.Get());
    }
    return true;
  }

  void Deliver(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
      return;
    }
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)) || length == 0) {
      return;
    }
    std::vector<uint8_t> bytes(data, data + length);
    buffer->Unlock();

    LONGLONG time = 0;
    sample->GetSampleTime(&time);
    FrameInfo info;
    std::vector<uint8_t> header;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // Кадры выходят по порядку; всё, что раньше этого, кодировщик
      // выбросил сам — их сведения больше не нужны.
      while (!frame_infos_.empty() && frame_infos_.front().time_hns < time) {
        frame_infos_.pop_front();
      }
      if (!frame_infos_.empty() && frame_infos_.front().time_hns == time) {
        info = frame_infos_.front();
        frame_infos_.pop_front();
      } else {
        info.time_hns = time;
        info.rtp_timestamp = static_cast<uint32_t>(time / 10 * 90 / 1000);
      }
      header = sequence_header_;
    }

    bool key = false;
    bool has_sps = false;
    for (const auto& index :
         H264::FindNaluIndices(MakeArrayView(bytes.data(), bytes.size()))) {
      const H264::NaluType type =
          H264::ParseNaluType(bytes[index.payload_start_offset]);
      key |= type == H264::kIdr;
      has_sps |= type == H264::kSps;
    }
    if (key && !has_sps && !header.empty()) {
      bytes.insert(bytes.begin(), header.begin(), header.end());
    }

    EncodedImage image;
    image._encodedWidth = width_;
    image._encodedHeight = height_;
    image.SetRtpTimestamp(info.rtp_timestamp);
    image.SetSimulcastIndex(0);
    image.ntp_time_ms_ = info.ntp_time_ms;
    image.capture_time_ms_ = info.capture_time_ms;
    image.rotation_ = info.rotation;
    image.content_type_ = VideoContentType::UNSPECIFIED;
    image.timing_.flags = VideoSendTiming::kInvalid;
    image._frameType =
        key ? VideoFrameType::kVideoFrameKey : VideoFrameType::kVideoFrameDelta;
    if (info.color_space) {
      image.SetColorSpace(*info.color_space);
    }
    image.SetEncodedData(EncodedImageBuffer::Create(bytes.data(), bytes.size()));
    image.set_size(bytes.size());
    parser_.ParseBitstream(image);
    image.qp_ = parser_.GetLastSliceQp().value_or(-1);

    CodecSpecificInfo codec_info;
    codec_info.codecType = kVideoCodecH264;
    codec_info.codecSpecific.H264.packetization_mode =
        H264PacketizationMode::NonInterleaved;
    if (callback_) {
      callback_->OnEncodedImage(image, &codec_info);
    }
  }

  const Environment& env_;
  const SdpVideoFormat format_;
  std::vector<Candidate> candidates_;
  size_t candidate_index_ = 0;

  VideoCodec codec_{};
  VideoEncoder::Settings settings_{VideoEncoder::Capabilities(false), 1, 0};
  int width_ = 0;
  int height_ = 0;
  uint32_t fps_ = 30;
  uint32_t target_bps_ = 0;
  bool sending_ = false;
  bool key_frame_request_ = true;

  EncodedImageCallback* callback_ = nullptr;
  std::unique_ptr<VideoEncoder> software_;

  ComPtr<IMFActivate> activated_;
  ComPtr<IMFTransform> mft_;
  ComPtr<IMFMediaEventGenerator> events_;
  ComPtr<ICodecAPI> codec_api_;
  ComPtr<ID3D11Device> device_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT reset_token_ = 0;
  bool async_ = false;
  bool provides_samples_ = false;
  DWORD output_buffer_size_ = 0;

  std::thread event_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> failed_{false};

  // Общее для очереди кодирования и потока событий.
  std::mutex mutex_;
  std::deque<ComPtr<IMFSample>> pending_;
  std::deque<FrameInfo> frame_infos_;
  int need_input_ = 0;
  std::vector<uint8_t> sequence_header_;

  H264BitstreamParser parser_;
};

}  // namespace

MediaFoundationVideoEncoderFactory::MediaFoundationVideoEncoderFactory() {
  // Тот же профиль, что объявляет NVIDIA-кодировщик и OpenH264: Constrained
  // Baseline с packetization-mode=1 понимают все получатели.
  supported_formats_.push_back(SdpVideoFormat(
      "H264", {{"profile-level-id", "42e01f"},
               {"level-asymmetry-allowed", "1"},
               {"packetization-mode", "1"}}));
}

MediaFoundationVideoEncoderFactory::~MediaFoundationVideoEncoderFactory() =
    default;

bool MediaFoundationVideoEncoderFactory::IsSupported() {
  static const bool supported = [] {
    std::vector<Candidate> candidates = EnumerateCandidates();
    if (candidates.empty()) {
      RTC_LOG(LS_INFO) << "MF H264: аппаратных кодировщиков нет";
      return false;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
      RTC_LOG(LS_INFO) << "MF H264 кодировщик #" << i << ": "
                       << Describe(candidates[i]);
    }
    return true;
  }();
  return supported;
}

std::unique_ptr<VideoEncoder> MediaFoundationVideoEncoderFactory::Create(
    const Environment& env,
    const SdpVideoFormat& format) {
  if (format.name != "H264") {
    return nullptr;
  }
  std::vector<Candidate> candidates = EnumerateCandidates();
  if (candidates.empty()) {
    return nullptr;
  }
  return std::make_unique<MediaFoundationH264Encoder>(env, format,
                                                      std::move(candidates));
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetSupportedFormats() const {
  return supported_formats_;
}

std::vector<SdpVideoFormat>
MediaFoundationVideoEncoderFactory::GetImplementations() const {
  return supported_formats_;
}

}  // namespace webrtc
