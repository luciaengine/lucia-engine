set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO robbietilton/Compositor
    REF cbb9fb65da647b1e9c36cd396d35e5b7fea2f199
    SHA512 92c80b67d7fe6b3c0ba74f30b0c96a67f90450f87355f4ac7959f79171cbcfa55eb29d8bb78eb2d58fe5ba2e810133a14422a1ac55b4979d3dd112bde5979715
    PATCHES lucia-workspace.patch
)

find_program(XCODEBUILD xcodebuild REQUIRED)
set(build_path "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-workspace")
vcpkg_execute_required_process(
    COMMAND "${XCODEBUILD}" -project Compositor.xcodeproj -scheme Compositor -configuration Release
        -derivedDataPath "${build_path}" -clonedSourcePackagesDirPath "${build_path}/packages"
        -onlyUsePackageVersionsFromResolvedFile -jobs 4 ARCHS=arm64 ONLY_ACTIVE_ARCH=NO
        CODE_SIGNING_ALLOWED=NO MACH_O_TYPE=mh_bundle SWIFT_ACTIVE_COMPILATION_CONDITIONS=LUCIA_HOSTED
        "LD_RUNPATH_SEARCH_PATHS=@loader_path/../Frameworks" build
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME xcodebuild-${TARGET_TRIPLET}
)

set(workspace "${CURRENT_PACKAGES_DIR}/tools/${PORT}/Compositor.app")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/tools/${PORT}")
vcpkg_execute_required_process(
    COMMAND /usr/bin/ditto "${build_path}/Build/Products/Release/Compositor.app" "${workspace}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME stage-${TARGET_TRIPLET}
)
file(COPY_FILE "${SOURCE_PATH}/LICENSE" "${workspace}/Contents/Resources/Upstream-LICENSE.txt")
vcpkg_execute_required_process(
    COMMAND /usr/bin/codesign --force --deep --sign - "${workspace}"
    WORKING_DIRECTORY "${SOURCE_PATH}"
    LOGNAME codesign-${TARGET_TRIPLET}
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
