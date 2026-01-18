#pragma once

#include "SelectionState.hpp"

namespace devmode::frame_editors {

class AxisAdjuster {
public:
    AxisAdjuster() = default;
    explicit AxisAdjuster(SelectionState* selection) : selection_(selection) {}

    void set_selection(SelectionState* selection) { selection_ = selection; }

    void cycle_axis() {
        if (!selection_) return;
        switch (selection_->axis) {
            case AdjustmentAxis::X: selection_->axis = AdjustmentAxis::Y; break;
            case AdjustmentAxis::Y: selection_->axis = AdjustmentAxis::Z; break;
            case AdjustmentAxis::Z: selection_->axis = AdjustmentAxis::X; break;
        }
    }

    AdjustmentAxis axis() const {
        return selection_ ? selection_->axis : AdjustmentAxis::X;
    }

    void reset_axis(AdjustmentAxis axis) {
        if (selection_) {
            selection_->axis = axis;
        }
    }

private:
    SelectionState* selection_ = nullptr;
};

}  // namespace devmode::frame_editors
