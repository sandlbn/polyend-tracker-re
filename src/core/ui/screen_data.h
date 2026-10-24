// OT fork: Data screen (DATASCREEN-lite) in the Games slot of images built
// without Games (OT_GAMES=0): File F7 "Data". See screen_data.cpp.
#pragma once

#include "ui/screen_games.h"   // OT_GAMES
#include "ui/ui.h"

#if !OT_GAMES
#include <cstdint>

namespace ot {

void openData(App& app);

class DataScreen final : public Screen {
public:
    using Screen::Screen;
    const char* name() const override { return "Data"; }
    void start(int opt) override;
    void update(uint32_t now) override;
    void draw(Gfx& g) override;
    void onButton(uint8_t id, uint8_t state) override;
    void padLeds(uint8_t* levels) override;
    int mode = 0;   // 0 barcode, 1 matrix, 2 numbers

private:
    static constexpr int kTracks = 16, kHistory = 64;
    void refreshBar();
    uint16_t hist_[kHistory] = {};
    uint8_t width_[kHistory][kTracks] = {};
    int head_ = 0;
    int lastStep_ = -1;
    float level_[kTracks] = {};
    uint32_t lastDraw_ = 0;
    uint32_t frame_ = 0;
};

}  // namespace ot
#endif
