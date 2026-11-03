#include "UI/Controls/WeightedPane.h"
#include "UI/StyleProperties.h"

#include <algorithm>

namespace GameEngine {

WeightedPane::WeightedPane(float weight)
{
    SetFlexWeight(weight);
}

void WeightedPane::SetFlexWeight(float w)
{
    m_Weight = std::max(0.0f, w);
    Overrides().Set(Style::FlexGrow, m_Weight);
}

} // namespace GameEngine
