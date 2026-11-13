#pragma once

#include "UI/UIElement.h"

namespace GameEngine {

// A container whose flex-grow is a programmatic weight, so panes can share
// space by ratio. The weight is a style override, so it reaches Yoga through
// the normal style path and wins over any CSS flex-grow.
class WeightedPane : public UIElement {
public:
    explicit WeightedPane(float weight = 1.0f);

    void SetFlexWeight(float weight);
    float GetFlexWeight() const { return m_Weight; }

private:
    float m_Weight = 1.0f; // relative weight (used as Yoga flex-grow)
};

} // namespace GameEngine
