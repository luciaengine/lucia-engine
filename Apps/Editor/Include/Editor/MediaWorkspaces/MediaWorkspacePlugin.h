#pragma once

namespace GameEngine::Editor
{
class IEditorPlugin;

enum class MediaWorkspaceKind
{
    Editroom,
    Compositor,
    RapidRAW,
};

IEditorPlugin& GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind kind);
}
