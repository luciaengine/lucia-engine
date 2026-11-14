package org.openengine.player;

import android.content.pm.PackageManager;
import android.content.pm.ApplicationInfo;
import android.os.Bundle;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import org.libsdl.app.SDLActivity;

public final class PlayerActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() { return new String[] {"main"}; }

    @Override
    protected String[] getArguments() {
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0) {
            String[] arguments = getIntent().getStringArrayExtra("arguments");
            if (arguments != null) return arguments;
        }
        return super.getArguments();
    }

    // SDL invokes main on its native application thread, after the Activity's
    // UI is responsive and before native code opens any packaged content.
    @Override
    protected void main() {
        applyDebugEnvironment();
        try {
            prepareContent();
        } catch (IOException | PackageManager.NameNotFoundException error) {
            throw new IllegalStateException("Cannot extract packaged game content", error);
        }
        super.main();
    }

    private void applyDebugEnvironment() {
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) == 0) return;
        Bundle extras = getIntent().getExtras();
        if (extras == null) return;
        String prefix = "SDL_ENV.";
        for (String key : extras.keySet()) {
            Object value = extras.get(key);
            if (key.startsWith(prefix) && value instanceof String) {
                nativeSetenv(key.substring(prefix.length()), (String)value);
            }
        }
    }

    private void prepareContent() throws IOException, PackageManager.NameNotFoundException {
        String version = Long.toString(getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime);
        File root = new File(getFilesDir(), "game");
        File marker = new File(root, ".installed-version");
        if (marker.isFile() && version.equals(new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8))) {
            return;
        }
        File staging = new File(getFilesDir(), "game.staging");
        deleteTree(staging);
        copyAsset("game", staging);
        Files.write(new File(staging, ".installed-version").toPath(), version.getBytes(StandardCharsets.UTF_8));
        deleteTree(root);
        if (!staging.renameTo(root)) throw new IOException("Cannot publish packaged content at " + root);
    }

    private void deleteTree(File path) throws IOException {
        if (!path.exists()) return;
        if (path.isDirectory() && !Files.isSymbolicLink(path.toPath())) {
            File[] children = path.listFiles();
            if (children == null) throw new IOException("Cannot list " + path);
            for (File child : children) deleteTree(child);
        }
        if (!path.delete()) throw new IOException("Cannot remove " + path);
    }

    private void copyAsset(String path, File destination) throws IOException {
        String[] children = getAssets().list(path);
        if (children != null && children.length > 0) {
            if (!destination.isDirectory() && !destination.mkdirs()) {
                throw new IOException("Cannot create " + destination);
            }
            for (String child : children) {
                copyAsset(path + "/" + child, new File(destination, child));
            }
        } else {
            try (InputStream input = getAssets().open(path);
                 FileOutputStream output = new FileOutputStream(destination)) {
                byte[] bytes = new byte[65536];
                int count;
                while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
            }
        }
    }
}
