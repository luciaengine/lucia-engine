set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME iOS)
# Dependencies must share the Player deployment floor; the SDK version is not
# the minimum supported device OS (the iPhone 8 Plus remains on iOS 16).
set(VCPKG_OSX_DEPLOYMENT_TARGET 16.3)
