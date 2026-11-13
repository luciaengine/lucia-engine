#include "UI/Controls/WeightedPane.h"
#include "UI/StyleProperties.h"

#include <algorithm>

namespace GameEngine {

WeightedPane::WeightedPane(float weight)
{
    SetFlexWeight(weight);
}

void WeightedPane::SetFlexWeight(float weight)
{
    m_Weight = std::max(0.0f, weight);
    Style().Set(Style::FlexGrow, m_Weight);
}

} // namespace GameEngine
