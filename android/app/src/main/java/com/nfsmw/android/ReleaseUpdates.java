package com.nfsmw.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.*;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;
import javax.net.ssl.HttpsURLConnection;

/** Optional official GitHub updates. No APK is installed or downloaded in the background. */
final class ReleaseUpdates {
    private static final String REPO = "https://github.com/codepdbh/nfsmw-android/";
    private static final String API = "https://api.github.com/repos/codepdbh/nfsmw-android/releases/latest";
    static final class Result {
        final String tag, url, error;
        Result(String tag, String url, String error) { this.tag = tag; this.url = url; this.error = error; }
    }
    static SharedPreferences prefs(Context context) { return context.getSharedPreferences("nfsmw_updates", 0); }
    static String installed(Context context) {
        try { return context.getPackageManager().getPackageInfo(context.getPackageName(), 0).versionName; }
        catch (Exception ignored) { return "0.5.4"; }
    }
    static void check(Context context, boolean force, Consumer<Result> callback) {
        Context app = context.getApplicationContext();
        Handler main = new Handler(Looper.getMainLooper());
        Thread worker = new Thread(() -> {
            Result result;
            try { result = fetch(app, force); }
            catch (Exception error) { result = new Result(null, null, app.getString(R.string.diag_fetch_failed)); }
            Result value = result;
            main.post(() -> callback.accept(value));
        }, "GitHubReleaseCheck");
        worker.setDaemon(true); worker.start();
    }
    private static Result fetch(Context context, boolean force) throws Exception {
        SharedPreferences prefs = prefs(context);
        long age = System.currentTimeMillis() - prefs.getLong("last_check", 0);
        if (!force && age >= 0 && age < TimeUnit.HOURS.toMillis(6)) {
            String tag = prefs.getString("latest_tag", "");
            String url = prefs.getString("latest_url", REPO + "releases/latest");
            return new Result(tag, url.startsWith(REPO + "releases/") ? url : REPO + "releases/latest", null);
        }
        HttpsURLConnection connection = (HttpsURLConnection) new URL(API).openConnection();
        connection.setConnectTimeout(6000); connection.setReadTimeout(6000);
        connection.setRequestProperty("Accept", "application/vnd.github+json");
        connection.setRequestProperty("User-Agent", "NFSMW-Android-Evolved/" + installed(context));
        connection.setRequestProperty("X-GitHub-Api-Version", "2026-03-10");
        try {
            if (connection.getResponseCode() != 200) throw new IOException("GitHub unavailable");
            ByteArrayOutputStream body = new ByteArrayOutputStream();
            try (InputStream input = connection.getInputStream()) {
                byte[] buffer = new byte[8192]; int n;
                while ((n = input.read(buffer)) != -1) {
                    if (body.size() + n > 512 * 1024) throw new IOException("Response too large");
                    body.write(buffer, 0, n);
                }
            }
            JSONObject release = new JSONObject(new String(body.toByteArray(), StandardCharsets.UTF_8));
            String tag = release.getString("tag_name");
            if (release.optBoolean("draft") || release.optBoolean("prerelease")) tag = "";
            String url = REPO + "releases/tag/" + Uri.encode(tag);
            // Prefer this release's ARM64 APK. Source ZIPs and links outside our repository are ignored.
            JSONArray assets = release.optJSONArray("assets");
            if (assets != null) for (int i = 0; i < assets.length(); i++) {
                JSONObject asset = assets.getJSONObject(i);
                String name = asset.optString("name").toLowerCase(java.util.Locale.ROOT);
                String download = asset.optString("browser_download_url");
                if (name.endsWith(".apk") && download.startsWith(REPO + "releases/download/" + Uri.encode(tag) + "/")) {
                    url = download;
                    if (name.contains("arm64")) break;
                }
            }
            prefs.edit().putLong("last_check", System.currentTimeMillis())
                    .putString("latest_tag", tag).putString("latest_url", url).apply();
            return new Result(tag, url, null);
        } finally { connection.disconnect(); }
    }
}
