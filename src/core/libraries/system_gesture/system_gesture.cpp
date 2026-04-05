// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <map>
#include <mutex>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/libs.h"
#include "core/libraries/system_gesture/system_gesture.h"
#include "core/libraries/system_gesture/system_gesture_error.h"

namespace Libraries::SystemGesture {

// Internal structs
struct OrbisSystemGestureHandle {
    bool has_controller_info;
    bool is_updated;
    OrbisSystemGestureRectangle rect;
    // TODO: Why is this a float?
    float has_rect;
    s32 touch_recognizer_count;
    OrbisSystemGestureTouchRecognizer* touch_recognizers[55];
    OrbisSystemGesturePrimitiveTouchEvent primitive_events[6];
    u8 primitive_events_count;
    Pad::OrbisPadData cur_pad_data;
    Pad::OrbisPadData prev_pad_data;
    OrbisSystemGesturePrimitiveTouchEvent* inactive_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* beginning_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* active_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* ending_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* cancelled_primitive_events;
};

static constexpr u64 PRIMITIVE_TOUCH_EVENT_COUNT = 6;

// Actual decompiled struct
struct OrbisSystemGestureHandleInternal {
    u32 type;
    bool has_controller_info;
    bool is_updated;
    OrbisSystemGestureRectangle rect;
    float has_rect;
    u32 touch_recognizer_count;
    OrbisSystemGestureTouchRecognizer* touch_recognizers[55];
    char unk2[104];
    OrbisSystemGesturePrimitiveTouchEvent primitive_events[6];
    Pad::OrbisPadData cur_pad_data;
    Pad::OrbisPadData prev_pad_data;
    OrbisSystemGesturePrimitiveTouchEvent* inactive_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* beginning_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* active_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* ending_primitive_events;
    OrbisSystemGesturePrimitiveTouchEvent* cancelled_primitive_events;
    u8 touch_event_count;
    char unk3[7];
    float rect_width;
    float rect_height;
};

// Internal data
bool g_is_initialized{false};
std::mutex g_mtx{};
std::map<s32, OrbisSystemGestureHandle> g_handle_map{};
s32 g_sdk_version{};
s32 g_handle_count{};

// Actual library offsets
u64 library_base = 0x80e20c000;
u64 handle_array = 0xc070;
u64 lib_init = 0x5580;
u64 lib_open = 0x5650;
u64 lib_append_touch = 0x61b0;
u64 lib_create_touch = 0x6040;
u64 lib_update_prim = 0x58f0;
u64 lib_update_all = 0x6630;
u64 lib_get_touch_count = 0x68a0;

OrbisSystemGestureHandleInternal* global_handles =
    (OrbisSystemGestureHandleInternal*)(library_base + handle_array);

s32 PS4_SYSV_ABI (*Initialize)() = reinterpret_cast<s32 PS4_SYSV_ABI (*)()>(library_base +
                                                                            lib_init);
s32 PS4_SYSV_ABI (*Open)(s32 input_type, OrbisSystemGestureOpenParameter* param) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(s32 input_type, OrbisSystemGestureOpenParameter* param)>(
        library_base + lib_open);
s32 PS4_SYSV_ABI (*CreateTouchRecognizer)(s32 handle, OrbisSystemGestureTouchRecognizer* recognizer,
                                          OrbisSystemGestureType type,
                                          OrbisSystemGestureRectangle* rectangle,
                                          OrbisSystemGestureTouchRecognizerParameter* param) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(
        s32 handle, OrbisSystemGestureTouchRecognizer* recognizer, OrbisSystemGestureType type,
        OrbisSystemGestureRectangle* rectangle, OrbisSystemGestureTouchRecognizerParameter* param)>(
        library_base + lib_create_touch);
s32 PS4_SYSV_ABI (*AppendTouchRecognizer)(s32 handle,
                                          OrbisSystemGestureTouchRecognizer* recognizer) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(s32 handle,
                                          OrbisSystemGestureTouchRecognizer* recognizer)>(
        library_base + lib_append_touch);
s32 PS4_SYSV_ABI (*UpdatePrimitiveTouchRecognizer)(
    s32 handle, const OrbisSystemGestureTouchPadData* input_data) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(s32 handle,
                                          const OrbisSystemGestureTouchPadData* input_data)>(
        library_base + lib_update_prim);
s32 PS4_SYSV_ABI (*UpdateAllTouchRecognizer)(s32 handle) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(s32 handle)>(library_base + lib_update_all);
s32 PS4_SYSV_ABI (*GetTouchEventsCount)(s32 handle,
                                        const OrbisSystemGestureTouchRecognizer* recognizer) =
    reinterpret_cast<s32 PS4_SYSV_ABI (*)(s32 handle,
                                          const OrbisSystemGestureTouchRecognizer* recognizer)>(
        library_base + lib_get_touch_count);

s32 PS4_SYSV_ABI
sceSystemGestureAppendTouchRecognizer(s32 handle, OrbisSystemGestureTouchRecognizer* recognizer) {
    LOG_DEBUG(Lib_SystemGesture, "called");

    // return AppendTouchRecognizer(handle, recognizer);

    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!recognizer) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }
    if (recognizer->magic != 0x35547435) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }

    auto& lib_handle = g_handle_map[handle];
    if (lib_handle.touch_recognizer_count >= 0x37) {
        return ORBIS_SYSTEM_GESTURE_ERROR_OUT_OF_RECOGNIZER;
    }

    // Add touch recognizer to appropriate handle
    lib_handle.touch_recognizers[lib_handle.touch_recognizer_count++] = recognizer;
    recognizer->touch_recognizer_count = lib_handle.touch_recognizer_count;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureClose(s32 handle) {
    LOG_INFO(Lib_SystemGesture, "called");
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }
    g_handle_map.erase(handle);

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureCreateTouchRecognizer(
    s32 handle, OrbisSystemGestureTouchRecognizer* recognizer, OrbisSystemGestureType type,
    OrbisSystemGestureRectangle* rectangle, OrbisSystemGestureTouchRecognizerParameter* param) {
    LOG_DEBUG(Lib_SystemGesture, "called");

    // return CreateTouchRecognizer(handle, recognizer, type, rectangle, param);

    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!recognizer) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }
    if (type < OrbisSystemGestureType::Tap || type > OrbisSystemGestureType::Flick) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_SUPPORTED_GESTURE;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }

    // Zero-initialize the recognizer
    std::memset(recognizer, 0, sizeof(OrbisSystemGestureTouchRecognizer));
    recognizer->type = type;
    if (rectangle != nullptr) {
        // Only copies the four float values, doesn't touch the reserved fields.
        std::memcpy(&recognizer->rect, rectangle, sizeof(float) * 4);
    } else {
        recognizer->no_rectangle = true;
    }

    // Set param_data appropriately
    switch (type) {
    case OrbisSystemGestureType::Tap: {
        if (!param || param->tap.max_tap_count == 0) {
            recognizer->param_data = 1;
        } else {
            recognizer->param_data = param->tap.max_tap_count;
        }
    }
    case OrbisSystemGestureType::TapAndHold: {
        recognizer->param_data = !param ? 1000 : param->tap_and_hold.time_to_invoke_event;
    }
    default: // Do nothing
    }

    // Set the magic value, this is used to determine if the touch recognizer is initialized.
    recognizer->magic = 0x35547435;

    // Initialize recognizer touch events.
    for (OrbisSystemGestureTouchEvent& touch_event : recognizer->touch_events) {
        touch_event.is_updated = 1;
        touch_event.gesture_type = type;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureDebugGetVersion() {
    return 0x6a00;
}

s32 PS4_SYSV_ABI sceSystemGestureFinalizePrimitiveTouchRecognizer() {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetPrimitiveTouchEventByIndex(
    s32 handle, const u32 index, OrbisSystemGesturePrimitiveTouchEvent* touch_event) {
    LOG_DEBUG(Lib_SystemGesture, "called, index = {}", index);
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!touch_event) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }

    auto& lib_handle = g_handle_map[handle];
    if (index < lib_handle.primitive_events_count) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INDEX_OUT_OF_ARRAY;
    }

    // The "index" goes from start of the beginning list to the end of the cancelled list.
    s32 cur_index = 0;
    OrbisSystemGesturePrimitiveTouchEvent* event = nullptr;
    for (auto list_ev = lib_handle.beginning_primitive_events;
         event == nullptr && list_ev != nullptr; list_ev = list_ev->next, cur_index++) {
        if (cur_index == index) {
            event = list_ev;
        }
    }
    auto lists = std::to_array<OrbisSystemGesturePrimitiveTouchEvent*>(
        {lib_handle.beginning_primitive_events, lib_handle.active_primitive_events,
         lib_handle.ending_primitive_events, lib_handle.cancelled_primitive_events});
    for (auto list : lists) {
        for (auto list_ev = list; list_ev != nullptr && event == nullptr; list_ev = list_ev->next) {
            if (cur_index == index) {
                event = list_ev;
            }
        }
        if (event != nullptr) {
            break;
        }
    }

    if (event) {
        touch_event->primitive_id = event->primitive_id;
        touch_event->event_state = event->event_state;
        touch_event->pressed_position = event->pressed_position;
        touch_event->current_position = event->current_position;
        if (event->no_delta) {
            touch_event->delta_vector = {0, 0};
        } else {
            touch_event->delta_vector = event->delta_vector;
        }
        touch_event->is_updated = event->is_updated;
        touch_event->delta_time = event->elapsed_time;
        touch_event->elapsed_time = event->total_time;
    }

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetPrimitiveTouchEventByPrimitiveID(
    s32 handle, const u16 primitive_id, OrbisSystemGesturePrimitiveTouchEvent* touch_event) {
    LOG_DEBUG(Lib_SystemGesture, "called, primitive_id = {}", primitive_id);
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!touch_event) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }

    auto& lib_handle = g_handle_map[handle];
    OrbisSystemGesturePrimitiveTouchEvent* event = nullptr;
    auto lists = std::to_array<OrbisSystemGesturePrimitiveTouchEvent*>(
        {lib_handle.beginning_primitive_events, lib_handle.active_primitive_events,
         lib_handle.ending_primitive_events, lib_handle.cancelled_primitive_events});
    for (auto list : lists) {
        for (auto list_ev = list; list_ev != nullptr && event == nullptr; list_ev = list_ev->next) {
            if (list_ev->primitive_id == primitive_id) {
                event = list_ev;
                break;
            }
        }
        if (event != nullptr) {
            break;
        }
    }

    if (!event) {
        return ORBIS_SYSTEM_GESTURE_ERROR_EVENT_DATA_NOT_FOUND;
    }

    touch_event->primitive_id = event->primitive_id;
    touch_event->event_state = event->event_state;
    touch_event->pressed_position = event->pressed_position;
    touch_event->current_position = event->current_position;
    if (event->no_delta) {
        touch_event->delta_vector = {0, 0};
    } else {
        touch_event->delta_vector = event->delta_vector;
    }
    touch_event->is_updated = event->is_updated;
    touch_event->delta_time = event->elapsed_time;
    touch_event->elapsed_time = event->total_time;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetPrimitiveTouchEvents(
    s32 handle, OrbisSystemGesturePrimitiveTouchEvent* event_buffer, const u32 buffer_size,
    u32* event_count) {
    LOG_DEBUG(Lib_SystemGesture, "called, buffer_size = {}", buffer_size);
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!event_buffer || buffer_size == 0 || !event_count) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }

    auto& lib_handle = g_handle_map[handle];
    auto lists = std::to_array<OrbisSystemGesturePrimitiveTouchEvent*>(
        {lib_handle.beginning_primitive_events, lib_handle.active_primitive_events,
         lib_handle.ending_primitive_events, lib_handle.cancelled_primitive_events});
    std::memset(event_buffer, 0, buffer_size * 0x50);
    u32 count = 0;
    for (auto list : lists) {
        for (auto event = list; event != nullptr && count < buffer_size;
             event = event->next, count++) {
            auto& copy_event = event_buffer[count];
            copy_event.primitive_id = event->primitive_id;
            copy_event.event_state = event->event_state;
            copy_event.pressed_position = event->pressed_position;
            copy_event.current_position = event->current_position;
            if (event->no_delta) {
                copy_event.delta_vector = {0, 0};
            } else {
                copy_event.delta_vector = event->delta_vector;
            }
            copy_event.is_updated = event->is_updated;
            copy_event.delta_time = event->elapsed_time;
            copy_event.elapsed_time = event->total_time;
        }
        if (count >= buffer_size) {
            break;
        }
    }

    *event_count = count;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetPrimitiveTouchEventsCount(s32 handle) {
    LOG_TRACE(Lib_SystemGesture, "called");
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }
    return g_handle_map[handle].primitive_events_count;
}

s32 PS4_SYSV_ABI sceSystemGestureGetTouchEventByEventID(
    s32 handle, const OrbisSystemGestureTouchRecognizer* recognizer, const u32 event_id,
    OrbisSystemGestureTouchEvent* touch_event) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetTouchEventByIndex(
    s32 handle, const OrbisSystemGestureTouchRecognizer* recognizer, const u32 index,
    OrbisSystemGestureTouchEvent* touch_event) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetTouchEvents(s32 handle,
                                                const OrbisSystemGestureTouchRecognizer* recognizer,
                                                OrbisSystemGestureTouchEvent* event_buffer,
                                                const u32 buffer_size, u32* event_count) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureGetTouchEventsCount(
    s32 handle, const OrbisSystemGestureTouchRecognizer* recognizer) {
    LOG_TRACE(Lib_SystemGesture, "called");

    // return GetTouchEventsCount(handle, recognizer);

    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!recognizer) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }
    if (recognizer->magic != 0x35547435) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }

    return recognizer->touch_events_count;
}

s32 PS4_SYSV_ABI sceSystemGestureGetTouchRecognizerInformation(
    s32 handle, const OrbisSystemGestureTouchRecognizer* recognizer,
    OrbisSystemGestureTouchRecognizerInformation* info) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureInitializePrimitiveTouchRecognizer() {
    LOG_INFO(Lib_SystemGesture, "called");

    // return Initialize();

    if (g_is_initialized) {
        return ORBIS_OK;
    }
    s32 result = Libraries::Kernel::sceKernelGetCompiledSdkVersion(&g_sdk_version);
    if (result == ORBIS_OK) {
        g_is_initialized = true;
    }
    return result;
}

s32 PS4_SYSV_ABI sceSystemGestureOpen(s32 input_type, OrbisSystemGestureOpenParameter* param) {
    LOG_DEBUG(Lib_SystemGesture, "called");

    // return Open(input_type, param);

    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (input_type != 0 || param) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lk{g_mtx};
    if (g_handle_map.size() >= 8) {
        return ORBIS_SYSTEM_GESTURE_ERROR_ALREADY_OPENED;
    }

    // Not entirely accurate, but should be a close enough value to not matter.
    s32 handle = (g_handle_count + 1 * 0x100) + 0x47000000 + g_handle_count;
    g_handle_map[handle] = OrbisSystemGestureHandle{};
    g_handle_count++;

    // Initialize primitive touch events
    auto& lib_handle = g_handle_map[handle];
    std::memset(&lib_handle.primitive_events, 0,
                sizeof(OrbisSystemGesturePrimitiveTouchEvent) * PRIMITIVE_TOUCH_EVENT_COUNT);
    for (s32 i = 0; i < PRIMITIVE_TOUCH_EVENT_COUNT - 1; i++) {
        lib_handle.primitive_events[i].next = &lib_handle.primitive_events[i + 1];
    }
    lib_handle.inactive_primitive_events = &lib_handle.primitive_events[0];

    return handle;
}

s32 PS4_SYSV_ABI
sceSystemGestureRemoveTouchRecognizer(s32 handle, OrbisSystemGestureTouchRecognizer* recognizer) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureResetPrimitiveTouchRecognizer(s32 handle) {
    LOG_DEBUG(Lib_SystemGesture, "called");
    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }

    // Re-initialize primitive touch events
    auto& lib_handle = g_handle_map[handle];
    std::memset(&lib_handle.primitive_events, 0,
                sizeof(OrbisSystemGesturePrimitiveTouchEvent) * PRIMITIVE_TOUCH_EVENT_COUNT);
    for (s32 i = 0; i < PRIMITIVE_TOUCH_EVENT_COUNT - 1; i++) {
        lib_handle.primitive_events[i].next = &lib_handle.primitive_events[i + 1];
    }
    lib_handle.inactive_primitive_events = &lib_handle.primitive_events[0];

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceSystemGestureResetTouchRecognizer(s32 handle, OrbisSystemGestureTouchRecognizer* recognizer) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureUpdateAllTouchRecognizer(s32 handle) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    // s32 result = UpdateAllTouchRecognizer(handle);
    // LOG_DEBUG(Lib_SystemGesture, "{}", global_handles[0].touch_event_count);
    // return result;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureUpdatePrimitiveTouchRecognizer(
    s32 handle, const OrbisSystemGestureTouchPadData* input_data) {
    LOG_DEBUG(Lib_SystemGesture, "called");

    // static u64 count = 0;
    // input_data->pad_data_buffer->touchData.reserve1 = count++;
    // s32 result = UpdatePrimitiveTouchRecognizer(handle, input_data);
    // LOG_DEBUG(Lib_SystemGesture, "{}", global_handles[0].touch_event_count);
    // return result;

    if (!g_is_initialized) {
        return ORBIS_SYSTEM_GESTURE_ERROR_NOT_INITIALIZED;
    }
    if (!input_data || !input_data->pad_data_buffer) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    std::scoped_lock lk{g_mtx};
    if (!g_handle_map.contains(handle)) {
        return ORBIS_SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
    }
    auto& lib_handle = g_handle_map[handle];

    if (g_sdk_version >= Common::ElfInfo::FW_35) {
        // If the timestamp is the same, don't update events.
        bool is_updated =
            input_data->pad_data_buffer->timestamp != lib_handle.cur_pad_data.timestamp;
        lib_handle.is_updated = is_updated;
        for (auto event = lib_handle.primitive_events; event != nullptr; event = event->next) {
            event->is_updated = is_updated;
        }
        if (!is_updated) {
            return ORBIS_OK;
        }
    }
    lib_handle.is_updated = true;

    if (input_data->report_number != 1) {
        // Only one report is allowed
        return input_data->report_number < 1 ? ORBIS_OK
                                             : ORBIS_SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
    }

    if (!input_data->pad_data_buffer->connected) {
        // If controller is disconnected, then we invalidate old controller info,
        // and we skip update logic.
        lib_handle.has_controller_info = false;
        return ORBIS_OK;
    }

    if (!lib_handle.has_controller_info) {
        // Retrieve controller information before updating.
        Pad::OrbisPadControllerInformation controller_info{};
        s32 result = Pad::scePadGetControllerInformation(input_data->pad_handle, &controller_info);
        if (result != ORBIS_OK) {
            return result;
        } else if (controller_info.touchPadInfo.resolution.x == 0 &&
                   controller_info.touchPadInfo.resolution.y == 0 &&
                   (controller_info.touchPadInfo.pixelDensity == 0 ||
                    controller_info.touchPadInfo.pixelDensity == NAN)) {
            // Invalid touch pad info? Ends up jumping to return OK.
            return ORBIS_OK;
        }
        lib_handle.rect.width = controller_info.touchPadInfo.resolution.x;
        lib_handle.rect.height = controller_info.touchPadInfo.resolution.y;
        lib_handle.has_rect = 1.f;

        // Library does some weird checks here, but I'm pretty sure that logic is redundant.
        lib_handle.has_controller_info = true;
    }

    // Update old primitive events appropriately.
    // First, mark all cancelled events as inactive.
    for (auto event = lib_handle.cancelled_primitive_events; event != nullptr;
         event = event->next) {
        event->total_time = 0;
        event->event_state = OrbisSystemGestureTouchState::Inactive;
    }
    for (auto event_list = lib_handle.inactive_primitive_events; event_list != nullptr;
         event_list = event_list->next) {
        if (event_list->next == nullptr) {
            event_list->next = lib_handle.cancelled_primitive_events;
            break;
        }
    }
    if (lib_handle.inactive_primitive_events == nullptr) {
        lib_handle.inactive_primitive_events = lib_handle.cancelled_primitive_events;
    }
    lib_handle.cancelled_primitive_events = nullptr;

    // Mark all ending events as inactive.
    for (auto event = lib_handle.ending_primitive_events; event != nullptr; event = event->next) {
        event->total_time = 0;
        event->event_state = OrbisSystemGestureTouchState::Inactive;
    }
    for (auto event_list = lib_handle.inactive_primitive_events; event_list != nullptr;
         event_list = event_list->next) {
        if (event_list->next == nullptr) {
            event_list->next = lib_handle.ending_primitive_events;
            break;
        }
    }
    if (lib_handle.inactive_primitive_events == nullptr) {
        lib_handle.inactive_primitive_events = lib_handle.ending_primitive_events;
    }
    lib_handle.ending_primitive_events = nullptr;

    // Mark all beginning events as active.
    for (auto event = lib_handle.beginning_primitive_events; event != nullptr;
         event = event->next) {
        event->total_time = 0;
        event->event_state = OrbisSystemGestureTouchState::Active;
    }
    for (auto event_list = lib_handle.active_primitive_events; event_list != nullptr;
         event_list = event_list->next) {
        if (event_list->next == nullptr) {
            event_list->next = lib_handle.beginning_primitive_events;
            break;
        }
    }
    if (lib_handle.active_primitive_events == nullptr) {
        lib_handle.active_primitive_events = lib_handle.beginning_primitive_events;
    }
    lib_handle.beginning_primitive_events = nullptr;

    // Update creation time on all inactive events
    for (auto event = lib_handle.inactive_primitive_events; event != nullptr; event = event->next) {
        event->total_time =
            input_data->pad_data_buffer->timestamp - lib_handle.cur_pad_data.timestamp;
    }

    // Copy the new pad data into our handle
    std::memcpy(&lib_handle.prev_pad_data, &lib_handle.cur_pad_data, sizeof(Pad::OrbisPadData));
    std::memcpy(&lib_handle.cur_pad_data, input_data->pad_data_buffer, sizeof(Pad::OrbisPadData));

    // Form a new "active" list with just the events we update.
    OrbisSystemGesturePrimitiveTouchEvent* new_active_list = nullptr;
    u8 primitive_events_count = 0;

    // There are actual touches to process
    if (lib_handle.cur_pad_data.touchData.touchNum > 0 &&
        lib_handle.cur_pad_data.touchData.touchNum < 3) {
        for (s32 touch_index = 0; touch_index < lib_handle.cur_pad_data.touchData.touchNum;
             touch_index++) {
            // Retrieve touch coordinates
            u16 touch_x = lib_handle.cur_pad_data.touchData.touch[touch_index].x;
            u16 touch_y = lib_handle.cur_pad_data.touchData.touch[touch_index].y;
            u8 primitive_id = lib_handle.cur_pad_data.touchData.touch[touch_index].id;

            // Regardless of how this update goes, the event will still be counted
            primitive_events_count++;

            // First, see if there is an active event with this id.
            auto event = lib_handle.active_primitive_events;
            while (event != nullptr && event->primitive_id != primitive_id) {
                event = event->next;
            }
            if (event == nullptr) {
                // No active event, pull one from the inactive list and start it up.
                event = lib_handle.inactive_primitive_events;
                lib_handle.inactive_primitive_events = event->next;

                std::memset(event, 0, sizeof(OrbisSystemGesturePrimitiveTouchEvent));
                event->event_state = OrbisSystemGestureTouchState::Begin;
                event->primitive_id = primitive_id;
                event->current_position.x = touch_x;
                event->current_position.y = touch_y;
                event->pressed_position = event->current_position;
                event->is_updated = true;

                // Add to the end of the beginning events
                for (auto event_list = lib_handle.beginning_primitive_events; event_list != nullptr;
                     event_list = event_list->next) {
                    if (event_list->next == nullptr) {
                        event_list->next = event;
                        break;
                    }
                }
                if (lib_handle.beginning_primitive_events == nullptr) {
                    lib_handle.beginning_primitive_events = event;
                }
            } else {
                // Update the event's delta data
                if (g_sdk_version < Common::ElfInfo::FW_35 || true /*lib_handle.prev_pad_data.touchData.reserve1 != lib_handle.cur_pad_data.touchData.reserve1*/) {
                    event->delta_time = event->elapsed_time;
                    event->delta_vector.x = touch_x - event->current_position.x;
                    event->delta_vector.y = touch_y - event->current_position.y;
                } else {
                    event->no_delta = true;
                }

                // Update the current data for the event
                event->elapsed_time =
                    lib_handle.cur_pad_data.timestamp - lib_handle.prev_pad_data.timestamp;
                event->total_time += event->elapsed_time;
                event->current_position.x = touch_x;
                event->current_position.y = touch_y;

                // If it has been too long since the last update, this event is cancelled.
                // Otherwise, put it in the new active list
                if (200000 >=
                    lib_handle.cur_pad_data.timestamp - lib_handle.prev_pad_data.timestamp) {
                    lib_handle.active_primitive_events = event->next;
                    event->next = nullptr;

                    // Add to the end of the new active events
                    for (auto event_list = new_active_list; event_list != nullptr;
                         event_list = event_list->next) {
                        if (event_list->next == nullptr) {
                            event_list->next = event;
                            break;
                        }
                    }
                    if (new_active_list == nullptr) {
                        new_active_list = event;
                    }
                } else {
                    // Mark the event as cancelled.
                    event->event_state = OrbisSystemGestureTouchState::Cancelled;
                    lib_handle.active_primitive_events = event->next;
                    event->next = nullptr;

                    // Add to the end of the cancelled events
                    for (auto event_list = lib_handle.cancelled_primitive_events;
                         event_list != nullptr; event_list = event_list->next) {
                        if (event_list->next == nullptr) {
                            event_list->next = event;
                            break;
                        }
                    }
                    if (lib_handle.cancelled_primitive_events == nullptr) {
                        lib_handle.cancelled_primitive_events = event;
                    }
                }
            }
        }
    }

    // Everything remaining in the library's active list moves to the ending list.
    for (auto event = lib_handle.active_primitive_events; event != nullptr; event = event->next) {
        event->event_state = OrbisSystemGestureTouchState::End;
        primitive_events_count++;
    }
    lib_handle.ending_primitive_events = lib_handle.active_primitive_events;

    // Swap the library active list to the new active list
    lib_handle.active_primitive_events = new_active_list;

    // Update the handle's primitive event count
    lib_handle.primitive_events_count = primitive_events_count;

    return ORBIS_OK;
}

s32 PS4_SYSV_ABI
sceSystemGestureUpdateTouchRecognizer(s32 handle, OrbisSystemGestureTouchRecognizer* recognizer) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceSystemGestureUpdateTouchRecognizerRectangle(
    s32 handle, OrbisSystemGestureTouchRecognizer* recognizer,
    const OrbisSystemGestureRectangle* rect) {
    LOG_ERROR(Lib_SystemGesture, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("1MMK0W-kMgA", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureAppendTouchRecognizer);
    LIB_FUNCTION("j4yXIA2jJ68", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureClose);
    LIB_FUNCTION("FWF8zkhr854", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureCreateTouchRecognizer);
    LIB_FUNCTION("-Caws0X3+bY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureDebugGetVersion);
    LIB_FUNCTION("3QYCmMlOlCY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureFinalizePrimitiveTouchRecognizer);
    LIB_FUNCTION("KAeP0+cQPVU", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetPrimitiveTouchEventByIndex);
    LIB_FUNCTION("yBaQ0h9m1NM", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetPrimitiveTouchEventByPrimitiveID);
    LIB_FUNCTION("L8YmemOeSNY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetPrimitiveTouchEvents);
    LIB_FUNCTION("JhwByySf9FY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetPrimitiveTouchEventsCount);
    LIB_FUNCTION("lpsXm7tzeoc", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetTouchEventByEventID);
    LIB_FUNCTION("TSKvgSz5ChU", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetTouchEventByIndex);
    LIB_FUNCTION("fLTseA7XiWY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetTouchEvents);
    LIB_FUNCTION("h8uongcBNVs", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetTouchEventsCount);
    LIB_FUNCTION("0KrW5eMnrwY", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureGetTouchRecognizerInformation);
    LIB_FUNCTION("3pcAvmwKCvM", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureInitializePrimitiveTouchRecognizer);
    LIB_FUNCTION("qpo-mEOwje0", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureOpen);
    LIB_FUNCTION("ELvBVG-LKT0", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureRemoveTouchRecognizer);
    LIB_FUNCTION("o11J529VaAE", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureResetPrimitiveTouchRecognizer);
    LIB_FUNCTION("oBuH3zFWYIg", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureResetTouchRecognizer);
    LIB_FUNCTION("wPJGwI2RM2I", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureUpdateAllTouchRecognizer);
    LIB_FUNCTION("GgFMb22sbbI", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureUpdatePrimitiveTouchRecognizer);
    LIB_FUNCTION("j4h82CQWENo", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureUpdateTouchRecognizer);
    LIB_FUNCTION("4WOA1eTx3V8", "libSceSystemGesture", 1, "libSceSystemGesture",
                 sceSystemGestureUpdateTouchRecognizerRectangle);
}

} // namespace Libraries::SystemGesture
