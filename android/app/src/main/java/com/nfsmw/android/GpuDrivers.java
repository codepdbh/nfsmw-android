package com.nfsmw.android;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import org.json.JSONObject;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;

/** User-provided adrenotools ZIPs: Turnip, PanVK or other Android ARM64 Vulkan drivers. */
final class GpuDrivers {
    static final class Driver {
        final String id, name, library;
        final File directory;
        Driver(String id, String name, File directory, String library) {
            this.id = id; this.name = name; this.directory = directory; this.library = library;
        }
    }
    private static File root(Context context) {
        File root = new File(context.getFilesDir(), "gpu_drivers"); root.mkdirs(); return root;
    }
    static List<Driver> list(Context context) {
        List<Driver> result = new ArrayList<>();
        File[] dirs = root(context).listFiles(File::isDirectory);
        if (dirs != null) for (File dir : dirs) {
            try {
                if (dir.getName().startsWith(".")) continue;
                JSONObject meta = new JSONObject(new String(Files.readAllBytes(new File(dir, "driver.json").toPath()), StandardCharsets.UTF_8));
                String library = meta.getString("libraryName");
                if (!library.matches("[A-Za-z0-9_.+-]+\\.so")) continue;
                File loadDir = new File(dir, meta.getString("folder"));
                if (!loadDir.getCanonicalPath().startsWith(dir.getCanonicalPath() + "/")) continue;
                if (new File(loadDir, library).isFile())
                    result.add(new Driver(dir.getName(), meta.getString("name"), loadDir, library));
            } catch (Exception ignored) { }
        }
        result.sort(Comparator.comparing(d -> d.name.toLowerCase(Locale.ROOT)));
        return result;
    }
    static String selectedId(Context context) {
        return GameOptions.prefs(context).getString("gpu_driver", "");
    }
    static Driver selected(Context context) {
        String id = selectedId(context);
        for (Driver driver : list(context)) if (driver.id.equals(id)) return driver;
        return null;
    }
    static void select(Context context, String id) {
        GameOptions.prefs(context).edit().putString("gpu_driver", id).apply();
        Diagnostics.invalidateGpu();
    }
    static String key(Context context) {
        return selectedId(context) + ":" + Build.FINGERPRINT + ":" + ReleaseUpdates.installed(context);
    }
    static Driver importZip(Context context, Uri uri) throws Exception {
        if (Build.VERSION.SDK_INT < 28) throw new IOException(context.getString(R.string.drv_need_android9));
        File stage = new File(root(context), ".import-" + UUID.randomUUID());
        if (!stage.mkdirs()) throw new IOException(context.getString(R.string.drv_create_dir));
        try {
            InputStream input = context.getContentResolver().openInputStream(uri);
            if (input == null) throw new IOException(context.getString(R.string.drv_open_zip));
            DriverArchive.extract(input, stage);
            File packageRoot = findMeta(context, stage);
            if (packageRoot == null) throw new IOException(context.getString(R.string.drv_missing_meta));
            JSONObject meta = new JSONObject(new String(Files.readAllBytes(new File(packageRoot, "meta.json").toPath()), StandardCharsets.UTF_8));
            String library = meta.getString("libraryName");
            if (!library.matches("[A-Za-z0-9_.+-]+\\.so")) throw new IOException(context.getString(R.string.drv_bad_lib));
            if (meta.optInt("minApi", 26) > Build.VERSION.SDK_INT)
                throw new IOException(context.getString(R.string.drv_need_newer));
            DriverArchive.validateLibrary(new File(packageRoot, library));
            String name = meta.optString("name", library);
            if (name.trim().isEmpty() || name.length() > 120) throw new IOException(context.getString(R.string.drv_bad_name));
            // Keep dependencies alongside the main library, including ZIPs with a wrapper directory.
            String relative = "payload";
            File installed = new File(root(context), UUID.randomUUID().toString());
            if (!installed.mkdirs()) throw new IOException(context.getString(R.string.drv_install));
            try {
                if (!packageRoot.renameTo(new File(installed, relative))) throw new IOException(context.getString(R.string.drv_install_pkg));
                JSONObject record = new JSONObject().put("name", name).put("libraryName", library).put("folder", relative);
                Files.write(new File(installed, "driver.json").toPath(), record.toString().getBytes(StandardCharsets.UTF_8));
                readOnly(new File(installed, relative));
            } catch (Exception error) { DriverArchive.delete(installed); throw error; }
            return new Driver(installed.getName(), name, new File(installed, relative), library);
        } finally {
            if (stage.exists()) DriverArchive.delete(stage);
        }
    }
    private static File findMeta(Context context, File root) throws IOException {
        if (new File(root, "meta.json").isFile()) return root;
        File found = null;
        File[] children = root.listFiles(File::isDirectory);
        if (children != null) for (File child : children) {
            File candidate = findMeta(context, child);
            if (candidate != null) {
                if (found != null) throw new IOException(context.getString(R.string.drv_ambiguous));
                found = candidate;
            }
        }
        return found;
    }
    private static void readOnly(File file) throws IOException {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) readOnly(child);
        else if (!file.setReadOnly()) throw new IOException("Could not protect the driver library.");
    }
    static void remove(Context context, Driver driver) throws IOException {
        File target = new File(root(context), driver.id);
        if (!target.getCanonicalFile().getParentFile().equals(root(context).getCanonicalFile()))
            throw new IOException(context.getString(R.string.drv_bad_path));
        DriverArchive.delete(target);
        if (selectedId(context).equals(driver.id)) select(context, "");
    }
    static void arguments(Context context, List<String> args) {
        Driver driver = selected(context);
        args.add("--android_native_lib_dir=" + context.getApplicationInfo().nativeLibraryDir);
        args.add("--android_tmp_dir=" + context.getCacheDir().getAbsolutePath());
        args.add("--android_gpu_driver_dir=" + (driver == null ? "" : driver.directory.getAbsolutePath() + "/"));
        args.add("--android_gpu_driver_name=" + (driver == null ? "" : driver.library));
    }
}
