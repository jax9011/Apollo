/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
    callback_state = std::make_shared<callback_state_t>();
    callback_state->owner = this;
  }

  wgc_capture_t::~wgc_capture_t() {
    {
      std::lock_guard lock(callback_state->mutex);
      callback_state->owner = nullptr;
    }
    if (item && closed_subscribed) {
      try {
        item.Closed(closed_token);
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Failed to remove window-close handler: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      }
    }
    if (frame_pool && frame_arrived_subscribed) {
      try {
        frame_pool.FrameArrived(frame_arrived_token);
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Failed to remove WGC frame handler: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      }
    }
    {
      std::unique_lock lock(callback_state->mutex);
      callback_state->cv.wait(lock, [this]() {
        return callback_state->active_callbacks == 0;
      });
    }
    {
      std::lock_guard callback_lock(frame_pool_mutex);
      if (produced_frame) {
        try {
          produced_frame.Close();
        } catch (winrt::hresult_error &e) {
          BOOST_LOG(warning) << "Failed to release a queued WGC frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        }
      }
      if (consumed_frame) {
        try {
          consumed_frame.Close();
        } catch (winrt::hresult_error &e) {
          BOOST_LOG(warning) << "Failed to release a consumed WGC frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        }
      }
    }
    if (capture_session) {
      try {
        capture_session.Close();
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Failed to close WGC capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      }
    }
    if (frame_pool) {
      try {
        frame_pool.Close();
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Failed to close WGC frame pool: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      }
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    window_capture = config.capture_window_handle != 0;
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    winrt::com_ptr<IGraphicsCaptureItemInterop> capture_item_factory;
    try {
      capture_item_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Failed to get the Windows Graphics Capture item factory: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    if (!capture_item_factory) {
      BOOST_LOG(error) << "Failed to get the Windows Graphics Capture item factory";
      return -1;
    }

    if (config.capture_window_handle) {
      auto hwnd = reinterpret_cast<HWND>(config.capture_window_handle);
      if (!IsWindow(hwnd)) {
        BOOST_LOG(error) << "The configured capture window is no longer available";
        return -1;
      }

      status = capture_item_factory->CreateForWindow(hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item));
    } else {
      status = capture_item_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item));
    }
    if (FAILED(status)) {
      BOOST_LOG(error) << "Windows Graphics Capture failed to acquire the target: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    if (config.capture_window_handle) {
      try {
        const auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) {
          BOOST_LOG(error) << "The configured capture window has an invalid size";
          return -1;
        }
        display->width = size.Width;
        display->height = size.Height;
        display->width_before_rotation = size.Width;
        display->height_before_rotation = size.Height;
        display->env_width = size.Width;
        display->env_height = size.Height;
        target_width = size.Width;
        target_height = size.Height;
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(error) << "Failed to get the configured window size: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        return -1;
      }
    }

    if (window_capture) {
      try {
        auto state = callback_state;
        closed_token = item.Closed([state](winrt::GraphicsCaptureItem const &sender, winrt::IInspectable const &args) {
          wgc_capture_t *owner {};
          {
            std::lock_guard lock(state->mutex);
            owner = state->owner;
            if (!owner) {
              return;
            }
            ++state->active_callbacks;
          }
          auto callback_guard = util::fail_guard([state]() {
            std::lock_guard lock(state->mutex);
            if (--state->active_callbacks == 0) {
              state->cv.notify_all();
            }
          });
          owner->on_item_closed(sender, args);
        });
        closed_subscribed = true;
      } catch (winrt::hresult_error &e) {
        BOOST_LOG(error) << "Failed to monitor capture window lifetime: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        return -1;
      }
    }

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    frame_pool_format = display->capture_format;

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      auto state = callback_state;
      frame_arrived_token = frame_pool.FrameArrived([state](winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &args) {
        wgc_capture_t *owner {};
        {
          std::lock_guard lock(state->mutex);
          owner = state->owner;
          if (!owner) {
            return;
          }
          ++state->active_callbacks;
        }
        auto callback_guard = util::fail_guard([state]() {
          std::lock_guard lock(state->mutex);
          if (--state->active_callbacks == 0) {
            state->cv.notify_all();
          }
        });
        owner->on_frame_arrived(sender, args);
      });
      frame_arrived_subscribed = true;
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(winrt::TimeSpan{ 10000000 / (config.framerate * 2) });
      }
      else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    std::lock_guard callback_lock(frame_pool_mutex);
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      if (window_capture) {
        AcquireSRWLockExclusive(&frame_lock);
        capture_failed = true;
        ReleaseSRWLockExclusive(&frame_lock);
        WakeConditionVariable(&frame_present_cv);
      }
      return;
    }
    if (frame != nullptr) {
      if (window_capture) {
        try {
          const auto size = frame.ContentSize();
          if (size.Width <= 0 || size.Height <= 0) {
            BOOST_LOG(error) << "The captured window reported an invalid size";
            AcquireSRWLockExclusive(&frame_lock);
            capture_failed = true;
            ReleaseSRWLockExclusive(&frame_lock);
            WakeConditionVariable(&frame_present_cv);
            return;
          }
          if (size.Width != target_width || size.Height != target_height) {
            frame.Close();
            sender.Recreate(
              uwp_device,
              static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(frame_pool_format),
              2,
              size
            );
            target_width = size.Width;
            target_height = size.Height;
            AcquireSRWLockExclusive(&frame_lock);
            item_resized = true;
            ReleaseSRWLockExclusive(&frame_lock);
            WakeConditionVariable(&frame_present_cv);
            return;
          }
        } catch (winrt::hresult_error &e) {
          BOOST_LOG(error) << "Failed to resize the window capture frame pool: [0x"sv << util::hex(e.code()).to_string_view() << ']';
          AcquireSRWLockExclusive(&frame_lock);
          capture_failed = true;
          ReleaseSRWLockExclusive(&frame_lock);
          WakeConditionVariable(&frame_present_cv);
          return;
        }
      }

      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        try {
          produced_frame.Close();
        } catch (winrt::hresult_error &e) {
          BOOST_LOG(warning) << "Failed to release a queued WGC frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        }
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  void wgc_capture_t::on_item_closed(winrt::GraphicsCaptureItem const &, winrt::IInspectable const &) {
    std::lock_guard callback_lock(frame_pool_mutex);
    AcquireSRWLockExclusive(&frame_lock);
    item_closed = true;
    ReleaseSRWLockExclusive(&frame_lock);
    WakeConditionVariable(&frame_present_cv);
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    *out = nullptr;
    if (release_frame() != capture_e::ok) {
      return capture_e::error;
    }

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && !item_closed && !item_resized && !capture_failed &&
        SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      const auto error = GetLastError();
      ReleaseSRWLockExclusive(&frame_lock);
      return error == ERROR_TIMEOUT ? capture_e::timeout : capture_e::error;
    }
    if (capture_failed) {
      ReleaseSRWLockExclusive(&frame_lock);
      BOOST_LOG(error) << "Windows Graphics Capture failed while capturing the target window";
      return capture_e::error;
    }
    if (item_resized) {
      item_resized = false;
      ReleaseSRWLockExclusive(&frame_lock);
      return capture_e::reinit;
    }
    if (item_closed) {
      ReleaseSRWLockExclusive(&frame_lock);
      BOOST_LOG(error) << "The captured window was closed";
      return capture_e::error;
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    try {
      auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
      if (capture_access == nullptr) {
        return capture_e::error;
      }
      auto status = capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to access the captured window texture: [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
      out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Failed to access the captured window frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return capture_e::error;
    }
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      try {
        consumed_frame.Close();
      } catch (winrt::hresult_error &e) {
        consumed_frame = nullptr;
        BOOST_LOG(error) << "Failed to release the captured window frame: [0x"sv << util::hex(e.code()).to_string_view() << ']';
        return capture_e::error;
      }
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi
