// 2.x USB Storage mode (Tracker Mini manual §13.10; docs/PLAN_2X.md): the app
// side of handing the SD card to a computer (see App::enterUsbStorage in
// app.h, IUsbStorage in hal_api.h).
#include "app/app.h"

#include <cstdio>

namespace ot {

App::UsbEnter App::enterUsbStorage() {
    if (usbActive_) return UsbEnter::kOk;
    if (!hal_.usbStorage) return UsbEnter::kUnavailable;
    // A project open / save / import / export owns files on the card; the
    // Games hold the sample memory (their exit reloads the Workspace).
    if ((jobBusy() && jobOp() != JobOp::Autosave) || sampleBankLent || rendering_) return UsbEnter::kBusy;
    if (!booted_) return UsbEnter::kNoCard;
    // 1. Stop playback and recording.
    if (capture.active()) capture.stop();
    seq.setRecording(false);
    seq.stop();
    editMode = false;
    allVoicesOff();
    // 2. Finish the background autosave and write everything that is dirty;
    // instruments streaming from their project file are copied to the
    // Workspace first (the computer may change the project).
    finishJob();
    autosave(StreamCopies::kAll);
    for (int i = 0; i < kSampleInstrumentCount; ++i) {
        if (!instrPresent[i] || !bit(srcMarker_, i)) continue;
        // Still read from its project folder (a copy failed): say so.
        char msg[64];
        std::snprintf(msg, sizeof(msg), "Instrument %u not copied: keep its project folder", unsigned(uint8_t(i + 1)));
        ui.notify(msg, 6000);
        break;
    }
    saveConfig();
    configDirty_ = configForce_ = false;
    // 3.-5. Unmount, stop the app's SD access, present the card.
    streamUnbindAll();   // SD streaming: the computer owns the card from here
    if (!hal_.usbStorage->begin()) return UsbEnter::kNoCard;
    usbActive_ = true;
    return UsbEnter::kOk;
}

void App::exitUsbStorage() {
    if (!usbActive_) return;
    hal_.usbStorage->end();
    usbActive_ = false;
    // The computer may have changed anything: reload like after a card change
    // (spec 05 §1.5 state 3 -> 2: reopen the Workspace). Without a card the
    // loop shows "Insert SD card" until one is back.
    booted_ = false;   // boot() sets it again once the card is usable
    modulesStale_ = true;   // [I] #21: Modules/ too (boot() lists it anew)
    lastNoCardTry_ = 0;
    boot();
    lastAutosave_ = millis();
    ui.dirty();
}

void App::loopUsbStorage(uint32_t now) {
    if (hal_.usbStorage) hal_.usbStorage->service();
    if (hal_.input) {
        InputEvent ev;
        int guard = 0;
        while (guard++ < 32 && hal_.input->poll(ev)) {
            ui.handle(ev, now);
            if (!usbActive_) return;   // Exit: the card is back, the Workspace reloaded
        }
    }
    // MIDI input is read and dropped: no notes, clock or transport while the
    // card is away (the Workspace is reloaded on exit anyway).
    if (hal_.midi) {
        uint8_t buf[32];
        for (int p = 0; p < 2; ++p) {
            int guard = 0;
            while (guard++ < 8 && hal_.midi->receive(p == 0 ? MidiPortId::kDin : MidiPortId::kUsb, buf, sizeof buf) > 0) {
            }
        }
    }
    ui.update(now);
    ui.renderIfDirty(now);
}

}  // namespace ot
