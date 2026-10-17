/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      xeXamEnumerate(handle, flags, buffer, buffer_length,
                     !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

// The music player's (XMP, app 0xFA) user-playlist enumerator: open message
// 0x70026, close 0x70027, items of XMP_USER_PLAYLIST_INFO (0x334 bytes). The
// title then sends 0x70025 with the enumerator's private area and reads the
// records with XamEnumerate. Every other kind keeps failing as before.
u32 XamCreateEnumeratorHandle_entry(u32 user_index, u32 app_id, u32 open_message,
                                    u32 close_message, u32 extra_size, u32 item_count, u32 flags,
                                    mapped_u32 out_handle) {
  constexpr uint32_t kXmpAppId = 0xFA;
  constexpr uint32_t kXmpUserPlaylistEnumerate = 0x70026;
  if (app_id != kXmpAppId || open_message != kXmpUserPlaylistEnumerate || !out_handle) {
    REXKRNL_DEBUG("XamCreateEnumeratorHandle(app {:08X}, open {:08X}): unsupported", app_id,
                  open_message);
    return X_ERROR_INVALID_PARAMETER;
  }
  auto* xmp = static_cast<apps::XmpApp*>(REX_KERNEL_STATE()->app_manager()->GetApp(kXmpAppId));
  if (!xmp) {
    return X_ERROR_INVALID_PARAMETER;
  }
  auto e = make_object<XStaticUntypedEnumerator>(REX_KERNEL_STATE(), std::max<uint32_t>(item_count, 1),
                                                 apps::XmpApp::kUserPlaylistInfoSize);
  auto result =
      e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, nullptr);
  if (XFAILED(result)) {
    return result;
  }
  const auto records = xmp->UserPlaylistRecords();
  for (const auto& record : records) {
    std::memcpy(e->AppendItem(), record.data(), record.size());
  }
  REXKRNL_DEBUG("XamCreateEnumeratorHandle: XMP user playlists, {} of {} per call", records.size(),
                item_count);
  *out_handle = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)
