// [I] USB audio on the device (#97): the K66 side of the USB audio function
// (third_party/teensy3 usb_desc.h, OT_USB_AUDIO=1; interfaces 4..6, iso IN 8,
// iso OUT 9, feedback 10). usb_dev.c calls this driver's C hooks from the
// USB interrupt; the stream goes through core/usb/usb_audio_in.h.
//
// Computer -> Tracker: usbAudioIn() is HalRefs::usbIn, called once per audio
// block; it also refreshes the feedback value the computer paces by.
// Tracker -> computer: the IN endpoint sends silence for now (step 2 of #97).
// OT_USB_AUDIO=0: usbAudioIn() returns nullptr (no USB audio in that image).
#pragma once

#include <cstdint>

namespace ot {
namespace hal {

// The computer's stream for the next block (128 interleaved frames) or
// nullptr. Audio-block context only.
const int16_t* usbAudioIn(void* ctx);
// True in an image with the USB audio function.
bool usbAudioAvailable();

// Diagnostics: packets received, under-runs, frames dropped, ring fill and
// the feedback value (10.14).
struct UsbAudioStats {
    uint32_t packets, underruns, dropped, fill, feedback;
    bool playing, streaming;
};
UsbAudioStats usbAudioStats();

}  // namespace hal
}  // namespace ot
