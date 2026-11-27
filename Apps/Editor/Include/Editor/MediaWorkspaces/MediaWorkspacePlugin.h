#pragma once

namespace GameEngine::Editor
{
class IEditorPlugin;

enum class MediaWorkspaceKind
{
    Editroom,
    Compositor,
};

IEditorPlugin& GetMediaWorkspaceEditorPlugin(MediaWorkspaceKind kind);
}
