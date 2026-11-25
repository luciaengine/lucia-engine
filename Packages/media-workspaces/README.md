# Media Workspaces

Editroom and Compositor open as dockable Lucia Editor panels on Apple Silicon.
Enable the Media Workspaces package for your project, then choose **Editroom**
or **Compositor** from the Window menu. Compositor requires macOS 26 or later.

Build the Editor with the optional vcpkg feature to include the native workspaces:

```sh
cmake --preset macos-arm64-ninja -DVCPKG_MANIFEST_FEATURES=editor-workspaces
cmake --build --preset macos-arm64-ninja --target Editor
```

When switching the feature in an existing build, pass `-U PKG_CONFIG_EXECUTABLE`
to the configure command so CMake resolves the current pkg-config executable.

The vcpkg ports pin [Editroom](https://github.com/robbietilton/Editroom) and
[Compositor](https://github.com/robbietilton/Compositor). The native bundles
carry their licenses. Editroom uses the manifest's FFmpeg executable for
media that AVFoundation cannot read.
