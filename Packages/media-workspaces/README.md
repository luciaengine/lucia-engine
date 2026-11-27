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

## Portable desktop hosts

Windows and Linux builds can stage media hosts from a prepared directory. Set
`GE_MEDIA_WORKSPACE_HOST_ROOT` to the absolute path of that directory when
configuring Lucia. Each workspace has its own folder named `Compositor` or
`Editroom`. Enable **Compositor**, **Editroom**, or both in the project’s Package Manager,
then reopen the project and open the corresponding panel from the Window menu.
Both packages start disabled. Projects that enabled the former Media Workspaces
package keep both panels available and can disable either independently.

### Compositor

The portable Compositor hosts use the native Windows and Linux ports of
Compositor. They open `.comp` projects and use the ports' editing and export
tools. Choose **Live Texture > Paint Project PNG** to open a texture from the
project's Assets folder. Painting updates the scene from memory as each
composite is ready. Use Compositor's export action to write the image.
**Stop Live Preview** restores the source texture in the scene.

For Linux x64, install the CompositorLX build dependencies and `patchelf`, then
use the pinned host builder. It stages the linked imaging libraries in a private
runtime folder and records their package versions and notices on Debian. Use a
Debian build environment for release artifacts. The host uses the system Qt and
Poppler Qt runtimes and an X11 or XWayland session.

```sh
bash Packages/media-workspaces/Hosts/compositor-linux/build.sh \
  /absolute/path/to/new-build-scratch /absolute/path/to/media-hosts
```

For Windows x64, prepare the dependencies in the pinned CompositorWindows
checkout using its build guide. Extract the matching portable release to a
separate directory for its runtime and notices. Run the Lucia host builder with
`-Source`, `-Build`, `-HostRoot`, and `-RuntimeDirectory` pointing to those
directories. The pinned Git revision is recorded in the script.

### Editroom

The portable Editroom host opens version 1 `.room` and `.cutplay` project files.
It imports video and audio, adds media to the timeline, splits and trims linked
clips, renders a playback preview, and exports H.264 MP4 with audio. It
preserves effect data in opened projects and reports unsupported effects
when previewing or exporting.

Build the host with Qt 6 Widgets, Multimedia, and Multimedia Widgets. On
Linux, install the Qt Multimedia playback plugin and use an X11 or XWayland
session. On Windows, run Qt's `windeployqt --release --no-translations` on
the staged `Editroom/LuciaMediaWorkspace.dll` to install the matching runtime.
Provide `ffmpeg` and `ffprobe` beside
the host or on PATH. Use FFmpeg 7.1 or newer with the `libx264` and AAC encoders.

```sh
cmake -S Packages/media-workspaces/Hosts/editroom -B /absolute/path/to/editroom-build
cmake --build /absolute/path/to/editroom-build --config Release
cmake --install /absolute/path/to/editroom-build --config Release --prefix /absolute/path/to/media-hosts
```
