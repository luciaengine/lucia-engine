#pragma once

namespace GameEngine::Editor
{

// Registers the editor traits of the entity's own bookkeeping components: its
// name, hierarchy links, scene identity and enable state. The inspector shows
// none of them as a section; the header, the Hierarchy panel and scene IO own
// them.
void RegisterEntityComponentTraits();

} // namespace GameEngine::Editor
