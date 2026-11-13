set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES x86_64)

# The oldest macOS the engine runs on. Every port is compiled for it, so no
# dependency raises the minimum of the binaries that link it. Keep equal to
# CMAKE_OSX_DEPLOYMENT_TARGET in the root CMakeLists.txt.
set(VCPKG_OSX_DEPLOYMENT_TARGET 15.0)
