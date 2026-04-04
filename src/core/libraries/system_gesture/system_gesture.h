// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/enum.h"
#include "common/types.h"
#include "core/libraries/pad/pad.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::SystemGesture {

enum class OrbisSystemGestureTouchState {
    Inactive = 0,
    Begin = 1,
    Active = 2,
    End = 3,
    Cancelled = 4
};

enum class OrbisSystemGestureType {
    Tap = 1,
    Drag = 2,
    TapAndHold = 4,
    PinchOutIn = 8,
    Rotation = 16,
    Flick = 32
};

struct OrbisSystemGestureVector2 {
    float x, y;
};

struct OrbisSystemGesturePrimitiveTouchEvent {
    OrbisSystemGestureTouchState event_state;
    u16 primitive_id;
    u8 is_updated;
    u8 reserved0;
    OrbisSystemGestureVector2 pressed_position;
    OrbisSystemGestureVector2 current_position;
    OrbisSystemGestureVector2 delta_vector;
    u64 delta_time;
    u64 elapsed_time;
    u64 creation_time;
    u8 reserved1[8];
    OrbisSystemGesturePrimitiveTouchEvent* next;
    u8 reserved2[8];
};

struct OrbisSystemGestureRectangle {
    float x, y, width, height;
    u8 reserved[8];
};

struct OrbisSystemGesturePrimitiveTouchRecognizerParameter {
    u8 reserved[64];
};

struct OrbisSystemGestureTouchRecognizerInformation {
    OrbisSystemGestureType gesture_type;
    OrbisSystemGestureRectangle rectangle;
    u64 updated_time;
    u8 reserved[256];
};

struct OrbisSystemGestureTapRecognizerParameter {
    u8 max_tap_count;
    u8 reserved[63];
};

struct OrbisSystemGestureDragRecognizerParameter {
    u8 reserved[64];
};

struct OrbisSystemGestureTapAndHoldRecognizerParameter {
    u64 time_to_invoke_event;
    u8 reserved[56];
};

struct OrbisSystemGesturePinchOutInRecognizerParameter {
    u8 reserved[64];
};

struct OrbisSystemGestureRotationRecognizerParameter {
    u8 reserved[64];
};

struct OrbisSystemGestureFlickRecognizerParameter {
    u8 reserved[64];
};

union OrbisSystemGestureTouchRecognizerParameter {
    u8 parameter_buf[64];
    OrbisSystemGestureTapRecognizerParameter tap;
    OrbisSystemGestureDragRecognizerParameter drag;
    OrbisSystemGestureTapAndHoldRecognizerParameter tap_and_hold;
    OrbisSystemGesturePinchOutInRecognizerParameter pinch_out_in;
    OrbisSystemGestureRotationRecognizerParameter rotation;
    OrbisSystemGestureFlickRecognizerParameter flick;
};

struct OrbisSystemGestureTapEventProperty {
    u16 primitive_id;
    u16 : 16;
    OrbisSystemGestureVector2 position;
    u8 tapped_count;
    u8 : 8;
    u16 : 16;
    u8 reserved[72];
};

struct OrbisSystemGestureDragEventProperty {
    u16 primitive_id;
    u16 : 16;
    OrbisSystemGestureVector2 delta_vector;
    OrbisSystemGestureVector2 current_position;
    OrbisSystemGestureVector2 pressed_position;
    u8 reserved[60];
};

struct OrbisSystemGestureTapAndHoldEventProperty {
    u16 primitive_id;
    u16 : 16;
    OrbisSystemGestureVector2 pressed_position;
    u8 reserved[76];
};

struct OrbisSystemGesturePinchOutInEventProperty {
    float scale;
    struct {
        u16 primitive_id;
        u16 : 16;
        OrbisSystemGestureVector2 current_position;
        OrbisSystemGestureVector2 delta_vector;
        OrbisSystemGestureVector2 paired_position;
    } primitive[2];
    u8 reserved[28];
};

struct OrbisSystemGestureRotationEventProperty {
    float angle;
    struct {
        u16 primitive_id;
        u16 : 16;
        OrbisSystemGestureVector2 current_position;
        OrbisSystemGestureVector2 delta_vector;
        OrbisSystemGestureVector2 paired_position;
    } primitive[2];
    u8 reserved[28];
};

struct OrbisSystemGestureFlickEventProperty {
    u16 primitive_id;
    u16 : 16;
    OrbisSystemGestureVector2 delta_vector;
    OrbisSystemGestureVector2 released_position;
    OrbisSystemGestureVector2 pressed_position;
    u8 reserved[60];
};

struct OrbisSystemGestureTouchEvent {
    u32 event_id;
    OrbisSystemGestureTouchState event_state;
    OrbisSystemGestureType gesture_type;
    u8 is_updated;
    u8 padding[3];
    u64 updated_time;
    union {
        u8 property_buf[88];
        OrbisSystemGestureTapEventProperty tap;
        OrbisSystemGestureDragEventProperty drag;
        OrbisSystemGestureTapAndHoldEventProperty tap_and_hold;
        OrbisSystemGesturePinchOutInEventProperty pinch_out_in;
        OrbisSystemGestureRotationEventProperty rotation;
        OrbisSystemGestureFlickEventProperty flick;
    } property;
    u8 reserved[56];
};

struct OrbisSystemGestureTouchRecognizer {
    u32 unk;
    OrbisSystemGestureType type;
    u64 creation_time;
    u64 updated_time;
    u32 touch_recognizer_count;
    u32 touch_events_count;
    OrbisSystemGestureTouchEvent touch_events[16];
    bool no_rectangle;
    u8 reserved[3];
    OrbisSystemGestureRectangle rect;
    u8 reserved2[4];
    u64 param_data;
    u8 reserved3[56];
    u32 unk2;
    u32 magic;
    u8 reserved4[64];
};

struct OrbisSystemGestureTouchPadData {
    s32 pad_handle;
    s32 report_number;
    Pad::OrbisPadData* pad_data_buffer;
    u8 reserved[8];
};

struct OrbisSystemGestureOpenParameter {
    u8 reserved[8];
};

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::SystemGesture
