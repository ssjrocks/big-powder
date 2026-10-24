// TptBrush's insides: TPT's brush objects. Only for the glue compiled with TPT's settings (TptTools.cpp, ClassicSim.cpp).
#pragma once
#include "classic/TptTools.h"

#include "gui/game/Brush.h"

#include <memory>
#include <vector>

struct bp::TptBrush::Impl {
    bool perfectCircle = true;
    std::vector<std::unique_ptr<Brush>> brushes; // GameModel::brushList
    std::vector<uint8_t> bitmap, outline;        // the current brush's, read back after each change
    void refresh(int index);
};
