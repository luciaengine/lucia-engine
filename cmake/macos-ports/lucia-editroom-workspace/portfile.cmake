# Xcode supplies the bundle's loader paths and signs its nested code.
set(VCPKG_FIXUP_MACHO_RPATH OFF)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO robbietilton/Editroom
    REF 5386a687d5b9a1ad08f0440cc3e0ec705fff7df9
    SHA512 033c2e3dfdff525106cb63524cc0c1dd4f8a5e3ac609943da4eaa451ab5edff81779074dd6e6d122d2ba2df7e03e6409feef46b2c679089b6791be1d6e03abdc
    PATCHES lucia-workspace.patch lucia-editor-interaction.patch lucia-live-texture.patch lucia-hosted-controls.patch
)

file(COPY_FILE "${CURRENT_INSTALLED_DIR}/share/lucia-media-controls/LuciaMediaControls.swift"
    "${SOURCE_PATH}/Editroom/LuciaMediaControls.swift")

find_program(XCODEBUILD xcodebuild REQUIRED)
set(build_path "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-workspace")
vcpkg_execute_required_process(
    COMMAND "${XCODEBUILD}" -project Editroom.xcodeproj -scheme Editroom -configuration Release
        -derivedDataPath "${build_path}" -clonedSourcePackagesDirPath "${build_path}/packages"
        -onlyUsePackageVersionsFromResolvedFile -jobs 4 ARCHS=arm64 ONLY_ACTIVE_ARCH=NO
        CODE_SIGNING_ALLOWED=NO MACH_O_TYPE=mh_bundle SWIFT_ACTIVE_COMPILATION_CONDITIONS=LUCIA_HOSTED
        "LD_RUNPATH_SEARCH_PATHS=@loader_path/../Frameworks" build
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME xcodebuild-${TARGET_TRIPLET}
)

set(workspace "${CURRENT_PACKAGES_DIR}/tools/${PORT}/Editroom.app")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
vcpkg_execute_required_process(
    COMMAND /usr/bin/ditto "${build_path}/Build/Products/Release/Editroom.app" "${workspace}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME stage-${TARGET_TRIPLET}
)
# Replace the upstream auxiliary executable with the manifest's FFmpeg build.
file(COPY_FILE "${CURRENT_INSTALLED_DIR}/tools/ffmpeg/ffmpeg" "${workspace}/Contents/MacOS/ffmpeg")
file(CHMOD "${workspace}/Contents/MacOS/ffmpeg"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(COPY_FILE "${CURRENT_INSTALLED_DIR}/share/ffmpeg/copyright"
    "${workspace}/Contents/Resources/FFmpeg-LICENSE.txt")
file(INSTALL "${CURRENT_INSTALLED_DIR}/share/ffmpeg/copyright"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME ffmpeg-copyright)
file(COPY_FILE "${SOURCE_PATH}/LICENSE" "${workspace}/Contents/Resources/Upstream-LICENSE.txt")
vcpkg_execute_required_process(
    COMMAND /usr/bin/codesign --force --deep --sign - "${workspace}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME codesign-${TARGET_TRIPLET}
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
