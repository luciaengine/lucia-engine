#!/usr/bin/env python3
"""Compile and package the engine-owned native mobile Player for an editor export."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile


def run(*args, **kwargs):
    print('+', ' '.join(str(arg) for arg in args), flush=True)
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def tool_path(name):
    found = shutil.which(name)
    if not found:
        raise RuntimeError(f'Install {name} and make it available on PATH.')
    return found


def latest_directory(parent):
    def version(path):
        return tuple(int(part) for part in re.findall(r'\d+', path.name))
    candidates = [path for path in parent.glob('*') if path.is_dir()]
    if not candidates:
        raise RuntimeError(f'Install the required SDK component under {parent}.')
    return max(candidates, key=version)


def android_sdk(chain):
    configured = chain.get('androidSDK') or os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if configured: return Path(configured)
    if os.name == 'nt': return Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) / 'Android/Sdk'
    if sys.platform == 'darwin': return Path.home() / 'Library/Android/sdk'
    return Path.home() / 'Android/Sdk'


def configure(request, cache):
    chain = request['toolchain']
    engine = Path(chain['engineRoot']).resolve()
    vcpkg = engine / 'dependencies/vcpkg'
    # A source checkout may share the toolchain, while each build owns its install tree.
    configured_toolchain = os.environ.get('VCPKG_ROOT')
    if configured_toolchain:
        vcpkg = Path(configured_toolchain)
    if not (vcpkg / 'scripts/buildsystems/vcpkg.cmake').is_file():
        common = subprocess.run(['git', '-C', str(engine), 'rev-parse', '--git-common-dir'], text=True, capture_output=True)
        if common.returncode == 0:
            git_dir = Path(common.stdout.strip())
            if not git_dir.is_absolute(): git_dir = engine / git_dir
            candidate = git_dir.resolve().parent / 'dependencies/vcpkg'
            if (candidate / 'scripts/buildsystems/vcpkg.cmake').is_file(): vcpkg = candidate
    if not (vcpkg / 'scripts/buildsystems/vcpkg.cmake').is_file():
        raise RuntimeError('Initialize the engine vcpkg checkout, or set VCPKG_ROOT to the installed toolchain.')
    native_input = Path(request.get('nativeSourcesPath', ''))
    modules = json.loads(native_input.read_text()).get('modules', []) if native_input.is_file() else []
    native_cmake = cache / 'game-sources.cmake'
    def quote(value):
        text = str(value).replace('\\', '/')
        delimiter = '='
        while ']' + delimiter + ']' in text: delimiter += '='
        return '[' + delimiter + '[' + text + ']' + delimiter + ']'
    lines = []
    for module in modules:
        for source in module['sources']: lines.append('target_sources(Player PRIVATE ' + quote(source) + ')')
        lines.append('target_include_directories(Player PRIVATE ' + quote(module['includeRoot']) + ')')
        for define in module.get('defines', []): lines.append('target_compile_definitions(Player PRIVATE ' + quote(define) + ')')
    native_text = '\n'.join(lines) + '\n'
    if not native_cmake.exists() or native_cmake.read_text() != native_text:
        native_cmake.write_text(native_text)
    args = ['cmake', '-S', engine, '-B', cache, f'-DGE_MOBILE_GAME_SOURCES={native_cmake}',
            f'-DCMAKE_TOOLCHAIN_FILE={vcpkg}/scripts/buildsystems/vcpkg.cmake',
            '-DVCPKG_MANIFEST_NO_DEFAULT_FEATURES=ON', '-DVCPKG_MANIFEST_FEATURES=physics;nav;ui-extras',
            '-DGE_SKIP_VCPKG_VERIFY=ON', '-DBUILD_EDITOR=OFF', '-DBUILD_TESTING=OFF',
            '-DRENDERING_BUILD_TESTS=OFF', '-DENABLE_SCRIPTING=OFF', '-DRENDERING_BUILD_WEBGPU=OFF',
            '-DCMAKE_POSITION_INDEPENDENT_CODE=ON']
    glslc = request.get('hostGlslc')
    if glslc and Path(glslc).is_file():
        args += [f'-DGLSLC_EXECUTABLE={glslc}']
    if request['platform'] == 'Android':
        sdk = android_sdk(chain)
        ndk = Path(chain['androidNDK']) if chain.get('androidNDK') else latest_directory(sdk / 'ndk')
        revision = re.search(r'Pkg.Revision\s*=\s*(\d+)', (ndk / 'source.properties').read_text())
        if not revision or int(revision[1]) < 29:
            raise RuntimeError('Install Android NDK r29 or later for the engine C++20 runtime.')
        tools = latest_directory(sdk / 'build-tools')
        if int(tools.name.split('.')[0]) < 35:
            raise RuntimeError('Install Android SDK build tools 35 or later for the SDL Java bytecode.')
        platform = latest_directory(sdk / 'platforms')
        if int(re.findall(r'\d+', platform.name)[0]) < 35:
            raise RuntimeError('Install Android SDK platform 35 or later for the SDL activity.')
        os.environ['ANDROID_NDK_HOME'] = str(ndk)
        args += ['-G', 'Ninja', '-DVCPKG_TARGET_TRIPLET=arm64-android',
                 f'-DVCPKG_CHAINLOAD_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake',
                 '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-28', '-DANDROID_STL=c++_shared',
                 f'-DCMAKE_BUILD_TYPE={request["configuration"]}', '-DRENDERING_BUILD_METAL=OFF']
    else:
        args += ['-G', 'Xcode', '-DVCPKG_TARGET_TRIPLET=arm64-ios', f'-DVCPKG_OVERLAY_PORTS={engine}/cmake/ios-ports', '-DCMAKE_SYSTEM_NAME=iOS',
                 '-DCMAKE_OSX_SYSROOT=iphoneos', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                 '-DCMAKE_OSX_DEPLOYMENT_TARGET=16.3', '-DRENDERING_BUILD_VULKAN=OFF', '-DRENDERING_BUILD_METAL=ON',
                 f'-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED={"YES" if chain.get("signingTeam") else "NO"}',
                 f'-DGE_MOBILE_APP_IDENTIFIER={request["applicationIdentifier"]}']
        if chain.get('signingTeam'):
            args += [f'-DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM={chain["signingTeam"]}',
                     '-DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic']
    run(*args)
    build = ['cmake', '--build', cache, '--config', request['configuration'], '--target', 'Player', '-j', '12']
    if request['platform'] == 'iOS' and chain.get('signingTeam'):
        build += ['--', '-allowProvisioningUpdates', '-allowProvisioningDeviceRegistration']
        if chain.get('deviceUDID'): build += ['-destination', f'id={chain["deviceUDID"]}']
    run(*build)
    return cache / 'vcpkg_installed' / ('arm64-android' if request['platform'] == 'Android' else 'arm64-ios')


def sdl_java(installed, scratch):
    spdx = json.loads((installed / 'share/sdl3/vcpkg.spdx.json').read_text())
    package = next(item for item in spdx['packages'] if item['name'] == 'libsdl-org/SDL')
    ref = package['downloadLocation'].split('@', 1)[1]
    digest = next(item['checksumValue'] for item in package['checksums'] if item['algorithm'] == 'SHA512')
    archive_name = f'libsdl-org-SDL-{ref}.tar.gz'
    download_root = Path(os.environ.get('VCPKG_DOWNLOADS', Path.home() / '.cache/vcpkg/downloads'))
    archive = download_root / archive_name
    if not archive.is_file():
        archive = scratch / archive_name
        urllib.request.urlretrieve(f'https://github.com/libsdl-org/SDL/archive/{ref}.tar.gz', archive)
    if hashlib.sha512(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError('SDL Android Java source checksum does not match the installed native library.')
    java = scratch / 'java/org/libsdl/app'
    java.mkdir(parents=True, exist_ok=True)
    marker = '/android-project/app/src/main/java/org/libsdl/app/'
    with tarfile.open(archive) as source:
        for member in source.getmembers():
            if marker in member.name and member.isfile() and member.name.endswith('.java'):
                (java / Path(member.name).name).write_bytes(source.extractfile(member).read())
    shutil.copyfile(installed / 'share/sdl3/copyright', scratch / 'SDL-LICENSE.txt')
    return list(java.glob('*.java'))


def android_package(request, cache, installed, scratch, identifier, name):
    chain = request['toolchain']
    sdk = android_sdk(chain)
    tools = latest_directory(sdk / 'build-tools')
    platform = latest_directory(sdk / 'platforms')
    android_jar = platform / 'android.jar'
    java = sdl_java(installed, scratch)
    java.append(Path(__file__).parent / 'Android/PlayerActivity.java')
    resources = []
    icon_attribute = ''
    icon = Path(request.get('iconsRoot', '')) / 'Android/AppIcon192.png'
    if icon.is_file():
        icon_dir = scratch / 'res/mipmap-xxxhdpi'
        icon_dir.mkdir(parents=True)
        shutil.copyfile(icon, icon_dir / 'icon.png')
        resource_zip = scratch / 'resources.zip'
        run(tools / ('aapt2.exe' if os.name == 'nt' else 'aapt2'), 'compile', '--dir', scratch / 'res', '-o', resource_zip)
        resources = [resource_zip]
        icon_attribute = 'android:icon="@mipmap/icon"'
    manifest = scratch / 'AndroidManifest.xml'
    from xml.sax.saxutils import escape
    manifest.write_text(f'''<manifest xmlns:android="http://schemas.android.com/apk/res/android"
        package="{identifier}" android:versionCode="1" android:versionName="{escape(request['version'], {'"': '&quot;'})}">
        <uses-sdk android:minSdkVersion="28" android:targetSdkVersion="32"/>
        <uses-feature android:name="android.hardware.vulkan.level" android:required="true" android:version="1"/>
        <uses-permission android:name="android.permission.INTERNET"/>
        <uses-permission android:name="android.permission.VIBRATE"/>
        <application android:label="{escape(json.loads(Path(request['gameConfigPath']).read_text()).get('gameName', name), {'"': '&quot;'})}" android:hasCode="true" android:debuggable="{str(request['configuration'] != 'Release').lower()}" android:extractNativeLibs="true" {icon_attribute}>
        <activity android:name="org.openengine.player.PlayerActivity" android:exported="true"
          android:configChanges="keyboard|keyboardHidden|orientation|screenSize|screenLayout|uiMode"
          android:screenOrientation="sensorLandscape">
          <intent-filter><action android:name="android.intent.action.MAIN"/><category android:name="android.intent.category.LAUNCHER"/></intent-filter>
        </activity></application></manifest>''')
    classes, dex = scratch / 'classes', scratch / 'dex'
    classes.mkdir(); dex.mkdir()
    run(tool_path('javac'), '--release', '8', '-classpath', android_jar, '-d', classes, *java)
    run(tool_path('java'), '-cp', tools / 'lib/d8.jar', 'com.android.tools.r8.D8', '--min-api', '28', '--lib', android_jar, '--output', dex, *classes.rglob('*.class'))
    unsigned = scratch / 'unsigned.apk'
    run(tools / ('aapt2.exe' if os.name == 'nt' else 'aapt2'), 'link', '-o', unsigned, '-I', android_jar, '--manifest', manifest, *resources)
    ndk = Path(chain['androidNDK']) if chain.get('androidNDK') else latest_directory(sdk / 'ndk')
    native = scratch / 'libmain.so'
    shutil.copyfile(cache / 'mobile/libmain.so', native)
    llvm = latest_directory(ndk / 'toolchains/llvm/prebuilt') / 'bin'
    run(llvm / ('llvm-strip.exe' if os.name == 'nt' else 'llvm-strip'), '--strip-debug', native)
    with zipfile.ZipFile(unsigned, 'a', compression=zipfile.ZIP_DEFLATED) as apk:
        apk.write(dex / 'classes.dex', 'classes.dex')
        apk.write(native, 'lib/arm64-v8a/libmain.so')
        cpp = next((ndk / 'toolchains/llvm/prebuilt').glob('*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so'))
        apk.write(cpp, 'lib/arm64-v8a/libc++_shared.so')
        content = Path(request['contentRoot'])
        for source in sorted(content.rglob('*')):
            if source.is_file():
                apk.write(source, 'assets/game/' + source.relative_to(content).as_posix())
        apk.write(scratch / 'SDL-LICENSE.txt', 'assets/game/ThirdParty/SDL-LICENSE.txt')
        for source in (Path(__file__).parent / 'Demo').iterdir():
            if source.is_file(): apk.write(source, 'assets/game/PlayerDemo/' + source.name)
    aligned = scratch / 'aligned.apk'
    run(tools / ('zipalign.exe' if os.name == 'nt' else 'zipalign'), '-f', '4', unsigned, aligned)
    key = Path.home() / '.android/debug.keystore'
    key.parent.mkdir(parents=True, exist_ok=True)
    if not key.exists():
        run(tool_path('keytool'), '-genkeypair', '-keystore', key, '-storepass', 'android', '-keypass', 'android',
            '-alias', 'androiddebugkey', '-keyalg', 'RSA', '-validity', '3650', '-dname', 'CN=Android Debug')
    artifact = Path(request['outputDirectory']) / f'{name}.apk'
    run(tool_path('java'), '-jar', tools / 'lib/apksigner.jar', 'sign', '--ks', key, '--ks-pass', 'pass:android', '--out', artifact, aligned)
    run(tool_path('java'), '-jar', tools / 'lib/apksigner.jar', 'verify', '--verbose', artifact)
    return artifact


def ios_package(request, cache, scratch, identifier, name):
    candidates = [cache / 'bin' / request['configuration'] / 'Player.app']
    candidates = [candidate for candidate in candidates if candidate.is_dir()]
    if not candidates:
        raise RuntimeError('The native iOS Player build did not produce Player.app.')
    app = scratch / f'{name}.app'
    shutil.copytree(candidates[0], app, symlinks=True)
    content = Path(request['contentRoot'])
    shutil.copytree(content, app, dirs_exist_ok=True)
    shutil.copytree(Path(__file__).parent / 'Demo', app / 'PlayerDemo', dirs_exist_ok=True)
    info_path = app / 'Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    run('xcrun', 'strip', '-S', app / info['CFBundleExecutable'])
    info.update(CFBundleIdentifier=identifier, CFBundleName=name, CFBundleDisplayName=json.loads(Path(request['gameConfigPath']).read_text()).get('gameName', name),
                CFBundleShortVersionString=request['version'], CFBundleVersion='1',
                UILaunchScreen={}, UIRequiresFullScreen=True,
                UISupportedInterfaceOrientations=['UIInterfaceOrientationLandscapeLeft', 'UIInterfaceOrientationLandscapeRight'])
    icons = Path(request.get('iconsRoot', '')) / 'iOS'
    if icons.is_dir():
        for icon in icons.glob('*.png'): shutil.copyfile(icon, app / icon.name)
        info['CFBundleIcons'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon120', 'AppIcon180']}}
        info['CFBundleIcons~ipad'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon152', 'AppIcon167']}}
    info_path.write_bytes(plistlib.dumps(info))
    team = request['toolchain'].get('signingTeam')
    if team:
        details = subprocess.run(['codesign', '-d', '--verbose=4', str(candidates[0])], capture_output=True, text=True, check=True)
        identity = next(line.split('=', 1)[1] for line in details.stderr.splitlines() if line.startswith('Authority='))
        extracted = subprocess.run(['codesign', '-d', '--entitlements', '-', '--xml', str(candidates[0])], capture_output=True, check=True)
        data = extracted.stdout
        start, end = data.find(b'<?xml'), data.find(b'</plist>')
        if start < 0 or end < 0: raise RuntimeError('Could not extract development signing entitlements from the native Player.')
        entitlements = scratch / 'entitlements.plist'
        entitlements.write_bytes(data[start:end + len(b'</plist>')])
        run('codesign', '--force', '--sign', identity, '--entitlements', entitlements, '--generate-entitlement-der', app)
        run('codesign', '--verify', '--deep', '--strict', app)
    artifact = Path(request['outputDirectory']) / app.name
    if artifact.exists():
        shutil.rmtree(artifact)
    shutil.move(app, artifact)
    return artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--request', type=Path, required=True)
    args = parser.parse_args()
    request = json.loads(args.request.read_text())
    if request['schemaVersion'] != 1 or request['platform'] not in ('Android', 'iOS'):
        raise RuntimeError('Unsupported mobile build request.')
    output = Path(request['outputDirectory']).resolve()
    project = Path(request['projectRoot']).resolve()
    config = json.loads(Path(request['gameConfigPath']).read_text())
    name = request['artifactName']
    if not name or name.startswith('.') or Path(name).name != name:
        raise RuntimeError('Invalid mobile artifact name.')
    identifier = request.get('applicationIdentifier') or 'org.openengine.game.g' + hashlib.sha256(str(project).encode()).hexdigest()[:12]
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+', identifier):
        raise RuntimeError('Application identifier must use reverse-domain segments starting with a letter.')
    request['applicationIdentifier'] = identifier
    cache_inputs = {key: request['toolchain'].get(key, '') for key in ('engineRoot', 'androidSDK', 'androidNDK', 'signingTeam')}
    cache_inputs['projectRoot'] = str(project)
    if request['platform'] == 'Android' and not cache_inputs['androidNDK']:
        sdk = android_sdk(request['toolchain'])
        request['toolchain']['androidNDK'] = cache_inputs['androidNDK'] = str(latest_directory(sdk / 'ndk'))
    engine_key = hashlib.sha256(json.dumps(cache_inputs, sort_keys=True).encode()).hexdigest()[:12]
    cache = Path(request['buildCache']) / engine_key / request['platform'] / request['configuration']
    cache.mkdir(parents=True, exist_ok=True)
    installed = configure(request, cache)
    with tempfile.TemporaryDirectory(prefix='.mobile-package-', dir=output) as temporary:
        scratch = Path(temporary)
        if request['platform'] == 'Android':
            artifact = android_package(request, cache, installed, scratch, identifier, name)
        else:
            artifact = ios_package(request, cache, scratch, identifier, name)
    result = {key: request[key] for key in ('requestId', 'platform', 'configuration')}
    result['artifact'] = str(artifact.relative_to(output))
    Path(request['resultPath']).write_text(json.dumps(result, indent=2))
    print(f'Built {artifact}', flush=True)


if __name__ == '__main__':
    main()
