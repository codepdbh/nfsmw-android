package com.nfsmw.android;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;
import android.os.Process;
import android.util.Log;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.TimeUnit;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Own-app reports only. Generating a report never uploads or sends it. */
final class Diagnostics {
    static final String EMAIL = "daniebatuani@gmail.com";
    static final String ISSUES = "https://github.com/codepdbh/nfsmw-android/issues/new";
    private static final int LIMIT = 256 * 1024;
    private static JSONObject cachedGpu;
    private static native String nativeGpuReport(String hooks, String temp, String directory, String library);
    static synchronized void invalidateGpu() { cachedGpu = null; }
    static synchronized void acceptGpu(Context context, String report) throws Exception {
        cachedGpu = new JSONObject(report);
        context.getSharedPreferences("nfsmw_diagnostics", 0).edit()
                .putString("gpu_key", GpuDrivers.key(context)).putString("gpu_report", report).apply();
    }
    static String probeSelectedDriver(Context context) {
        System.loadLibrary("nfsmw_diagnostics");
        GpuDrivers.Driver driver = GpuDrivers.selected(context);
        return nativeGpuReport(context.getApplicationInfo().nativeLibraryDir, context.getCacheDir().getAbsolutePath(),
                driver == null ? "" : driver.directory.getAbsolutePath(), driver == null ? "" : driver.library);
    }
    static synchronized JSONObject gpu(Context context) {
        android.content.SharedPreferences prefs = context.getSharedPreferences("nfsmw_diagnostics", 0);
        if (GpuDrivers.key(context).equals(prefs.getString("gpu_key", ""))) {
            try { return new JSONObject(prefs.getString("gpu_report", "{}")); } catch (Exception ignored) { }
        }
        if (GpuDrivers.selected(context) == null) return gpu();
        JSONObject unknown = new JSONObject();
        try { unknown.put("requestedDriver", GpuDrivers.selected(context).name)
                .put("probeError", "Driver seleccionado pendiente de probar"); } catch (Exception ignored) { }
        return unknown;
    }

    static synchronized JSONObject gpu() {
        if (cachedGpu == null) {
            try {
                System.loadLibrary("nfsmw_diagnostics");
                cachedGpu = new JSONObject(nativeGpuReport("", "", "", ""));
            } catch (Exception | LinkageError error) {
                // A failed probe is not proof of incompatibility. Record it and
                // let the native runtime perform its normal capability checks.
                cachedGpu = new JSONObject();
                try { cachedGpu.put("probeError", error.toString()); } catch (Exception ignored) { }
            }
        }
        return cachedGpu;
    }

    static String incompatibility(Context context) {
        JSONObject result = gpu(context);
        if (result.optBoolean("compatible", true)) return null;
        StringBuilder message = new StringBuilder("El controlador de ")
                .append(result.optString("gpu", "esta GPU"))
                .append(" no ofrece las funciones que necesita el renderizador nativo.");
        message.append("\n\nVulkan del dispositivo: ").append(result.optString("vulkan", "no disponible"));
        message.append("\n\nBajar la resolución no resuelve esta incompatibilidad. Puedes enviar el diagnóstico para ayudarnos a estudiar soporte.");
        return message.toString();
    }

    static String summary(Context context) {
        String version = "desconocida";
        try { version = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (Exception ignored) { }
        JSONObject device = gpu(context);
        return "NFSMW Android Evolved " + version + "\nTeléfono: " + Build.MANUFACTURER + " " + Build.MODEL
                + "\nAndroid: " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")"
                + "\nGPU: " + device.optString("gpu", "no disponible")
                + "\nVulkan: " + device.optString("vulkan", "no disponible")
                + "\nDriver seleccionado: " + (GpuDrivers.selected(context) == null ? "Sistema" : GpuDrivers.selected(context).name)
                + "\nEdición del juego: " + editionLine(context) + "\n";
    }

    private static String editionLine(Context context) {
        GameEdition edition = GameEdition.of(context,
                new File(android.os.Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME));
        if (edition == null) return "sin default.xex";
        return edition.describe() + (edition.supported() ? "" : " (no compatible, SHA-256 " + edition.sha256.substring(0, 12) + ")");
    }

    static void recordLaunch(Context context) {
        context.getSharedPreferences("nfsmw_diagnostics", Context.MODE_PRIVATE).edit()
                .putLong("last_launch_ms", System.currentTimeMillis()).putInt("last_launch_pid", Process.myPid()).apply();
    }

    static File createReport(Context context) throws Exception {
        File directory = new File(context.getCacheDir(), "reports");
        if (!directory.isDirectory() && !directory.mkdirs()) throw new IOException("No se pudo crear el informe.");
        // Leave recent attachments available while a mail app or document picker
        // still reads them. Remove only reports older than seven days.
        File[] old = directory.listFiles();
        if (old != null) for (File file : old) {
            if (file.isFile() && file.lastModified() < System.currentTimeMillis() - TimeUnit.DAYS.toMillis(7)) file.delete();
        }
        String date = new SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(new Date());
        File report = new File(directory, "NFSMW-diagnostico-" + date + ".zip");
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(report))) {
            ActivityManager manager = (ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
            ActivityManager.MemoryInfo memory = new ActivityManager.MemoryInfo();
            if (manager != null) manager.getMemoryInfo(memory);
            StringBuilder info = new StringBuilder(summary(context));
            info.append("Fecha: ").append(new Date()).append("\nABI: ").append(Arrays.toString(Build.SUPPORTED_ABIS))
                    .append("\nRAM total: ").append(memory.totalMem).append("\nRAM disponible: ").append(memory.availMem)
                    .append("\nÚltimo intento de iniciar: ").append(context.getSharedPreferences("nfsmw_diagnostics", 0)
                            .getLong("last_launch_ms", 0)).append("\n\nOpciones:\n");
            for (GameOptions.Option option : GameOptions.ALL) info.append(option.key).append('=').append(GameOptions.get(context, option.key)).append('\n');
            info.append("\nArgumentos efectivos:\n");
            for (String argument : GameOptions.arguments(context)) info.append(argument).append('\n');
            info.append("\nDescribe qué ocurrió, cómo reproducirlo y la edición del juego.\n");
            add(zip, "informe.txt", info.toString().getBytes(StandardCharsets.UTF_8));
            add(zip, "gpu.json", gpu(context).toString(2).getBytes(StandardCharsets.UTF_8));
            addFile(zip, "last-java-crash.txt", new File(context.getFilesDir(), "last-java-crash.txt"));
            File logs = new File(context.getFilesDir(), "nfsmw/user/logs");
            File[] nativeLogs = logs.listFiles(file -> file.isFile() && file.getName().endsWith(".log"));
            if (nativeLogs != null) {
                Arrays.sort(nativeLogs, Comparator.comparingLong(File::lastModified).reversed());
                for (int i = 0; i < Math.min(2, nativeLogs.length); i++) addFile(zip, "native/" + nativeLogs[i].getName(), nativeLogs[i]);
            }
            addLogcat(zip);
            if (Build.VERSION.SDK_INT >= 30 && manager != null) addExitInfo(zip, manager, context);
        } catch (Exception error) {
            report.delete();
            throw error;
        }
        return report;
    }

    private static void add(ZipOutputStream zip, String name, byte[] data) throws IOException {
        zip.putNextEntry(new ZipEntry(name));
        zip.write(data);
        zip.closeEntry();
    }

    private static void addFile(ZipOutputStream zip, String name, File file) throws IOException {
        if (!file.isFile()) return;
        // Keep startup and the last errors, even after a log storm. Never load
        // a whole multi-megabyte native log or attach game data / saved games.
        try (RandomAccessFile input = new RandomAccessFile(file, "r"); ByteArrayOutputStream output = new ByteArrayOutputStream()) {
            long size = input.length();
            int head = (int) Math.min(size, LIMIT / 2);
            byte[] bytes = new byte[head];
            input.readFully(bytes);
            output.write(bytes);
            if (size > head) {
                int tail = (int) Math.min(size - head, LIMIT / 2);
                if (size > LIMIT) output.write("\n[... registro recortado: inicio y final ...]\n".getBytes(StandardCharsets.UTF_8));
                input.seek(size - tail);
                bytes = new byte[tail];
                input.readFully(bytes);
                output.write(bytes);
            }
            add(zip, name, output.toByteArray());
        }
    }

    private static byte[] readLimited(InputStream input) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        while (output.size() < LIMIT) {
            int count = input.read(buffer, 0, Math.min(buffer.length, LIMIT - output.size()));
            if (count < 0) break;
            output.write(buffer, 0, count);
        }
        return output.toByteArray();
    }

    private static void addLogcat(ZipOutputStream zip) throws IOException {
        java.lang.Process process = null;
        try {
            process = new ProcessBuilder("logcat", "-d", "-b", "main", "-b", "crash", "-v", "threadtime",
                    "-t", "1500", "--uid=" + Process.myUid()).redirectErrorStream(true).start();
            java.lang.Process active = process;
            Thread timeout = new Thread(() -> {
                try { if (!active.waitFor(3, TimeUnit.SECONDS)) active.destroyForcibly(); }
                catch (InterruptedException ignored) { active.destroyForcibly(); Thread.currentThread().interrupt(); }
            }, "ReportLogcatTimeout");
            timeout.setDaemon(true);
            timeout.start();
            try (InputStream input = process.getInputStream()) { add(zip, "logcat.txt", readLimited(input)); }
        } catch (IOException error) {
            add(zip, "logcat-error.txt", error.toString().getBytes(StandardCharsets.UTF_8));
        } finally { if (process != null) process.destroy(); }
    }

    private static void addExitInfo(ZipOutputStream zip, ActivityManager manager, Context context) throws IOException {
        try {
            List<ApplicationExitInfo> exits = manager.getHistoricalProcessExitReasons(context.getPackageName(), 0, 5);
            StringBuilder info = new StringBuilder();
            boolean traceAdded = false;
            for (ApplicationExitInfo exit : exits) {
                info.append("Timestamp: ").append(exit.getTimestamp()).append(" reason: ").append(exit.getReason())
                        .append(" status: ").append(exit.getStatus()).append(" description: ").append(exit.getDescription()).append('\n');
                if (!traceAdded && (exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE
                        || exit.getReason() == ApplicationExitInfo.REASON_ANR)) {
                    try (InputStream trace = exit.getTraceInputStream()) {
                        if (trace != null) {
                            add(zip, exit.getReason() == ApplicationExitInfo.REASON_CRASH_NATIVE ? "native-crash.pb" : "anr-trace.txt", readLimited(trace));
                            traceAdded = true;
                        }
                    } catch (IOException error) { info.append("Trace unavailable: ").append(error).append('\n'); }
                }
            }
            add(zip, "process-exits.txt", info.toString().getBytes(StandardCharsets.UTF_8));
        } catch (RuntimeException error) {
            Log.w("NFSMW", "Historial de cierres no disponible", error);
            add(zip, "process-exits.txt", error.toString().getBytes(StandardCharsets.UTF_8));
        }
    }
}
