package com.nfsmw.android;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.content.Intent;
import android.content.ClipData;
import android.content.ActivityNotFoundException;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import androidx.documentfile.provider.DocumentFile;
import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MainActivity extends Activity {
    private static final int REQUEST_GAME_FOLDER = 41;
    private static final int REQUEST_STORAGE_ACCESS = 42;
    private static final int REQUEST_LEGACY_STORAGE = 43;
    private static final int REQUEST_SAVE_REPORT = 44;
    private static final int REQUEST_DRIVER_ZIP = 45;
    private static final int REQUEST_DRIVER_PROBE = 46;
    private boolean playAfterProbe;
    private boolean launcherResumed;
    private boolean checkingUpdates;
    private ReleaseUpdates.Result pendingUpdate;
    private boolean pendingUpdateManual;

    private static final String TREE_URI = "tree_uri";
    static final String GAME_FOLDER_NAME = "nsfmw-androidevolved";

    private TextView importStatus;
    private Button selectFolder;
    private Button launchGame;
    private Button sendReport;
    private File pendingReport;
    private boolean pendingGithub;
    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(LauncherLocale.apply(base));
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            playAfterProbe = state.getBoolean("play_after_probe");
            String name = state.getString("pending_report");
            if (name != null && name.equals(new File(name).getName())) {
                pendingReport = new File(new File(getCacheDir(), "reports"), name);
                pendingGithub = state.getBoolean("pending_github");
            }
        }
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY |
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_FULLSCREEN |
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        setContentView(buildScreen());
        prepareSharedGameFolder();
        checkUpdates(false);
    }

    @Override
    protected void onResume() {
        super.onResume();
        launcherResumed = true;
        if (optionsList != null) {
            refreshOptions();
        }
        if (checksList != null && hasStorageAccess()) {
            showChecks(sharedGameRoot());
        }
        if (pendingUpdate != null) {
            ReleaseUpdates.Result result = pendingUpdate;
            pendingUpdate = null;
            showUpdate(result, pendingUpdateManual);
        }
    }

    @Override protected void onPause() {
        launcherResumed = false;
        super.onPause();
    }

    private void checkUpdates(boolean manual) {
        if (checkingUpdates) return;
        checkingUpdates = true;
        if (optionsList != null) refreshOptions();
        ReleaseUpdates.check(this, manual, result -> {
            if (isFinishing() || isDestroyed()) return;
            checkingUpdates = false;
            refreshOptions();
            if (!launcherResumed) {
                pendingUpdate = result; pendingUpdateManual = manual;
            } else showUpdate(result, manual);
        });
    }

    private void showUpdate(ReleaseUpdates.Result result, boolean manual) {
        if (result.error != null) {
            if (manual) reportError(result.error);
            return;  // Offline/rate-limited checks never affect playing.
        }
        if (!ReleaseVersion.newer(result.tag, ReleaseUpdates.installed(this))) {
            if (manual) new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(getString(R.string.updates_title)).setMessage(getString(R.string.updates_uptodate, ReleaseUpdates.installed(this)))
                    .setPositiveButton(getString(R.string.action_ok), null).show();
            return;
        }
        android.content.SharedPreferences updates = ReleaseUpdates.prefs(this);
        if (!manual && result.tag.equals(updates.getString("notified_tag", ""))) return;
        updates.edit().putString("notified_tag", result.tag).apply();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.updates_new, result.tag))
                .setMessage(getString(R.string.updates_msg))
                .setPositiveButton(getString(R.string.action_update), (dialog, which) -> {
                    try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(result.url))); }
                    catch (ActivityNotFoundException error) { reportError(getString(R.string.updates_open_failed)); }
                })
                .setNegativeButton(getString(R.string.action_later), null).show();
    }

    // ---- Screen --------------------------------------------------------------------------------------------

    private static final int ACCENT = 0xFFFF8A00;
    private LinearLayout checksList;
    private LinearLayout optionsList;
    private LinearLayout shaderPanel;
    private ProgressBar shaderProgress;
    private TextView shaderText;
    private ShaderBuilder shaderBuilder;

    private View buildScreen() {
        FrameLayout screen = new FrameLayout(this);
        screen.setBackground(new GradientDrawable(GradientDrawable.Orientation.TL_BR,
                new int[] {0xFF0B0F14, 0xFF1C1407, 0xFF0B0F14}));

        LinearLayout columns = new LinearLayout(this);
        columns.setOrientation(LinearLayout.HORIZONTAL);
        columns.setPadding(dp(28), dp(16), dp(28), dp(16));
        screen.addView(columns, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        // Left: logo, game files and the two actions.
        LinearLayout left = new LinearLayout(this);
        left.setOrientation(LinearLayout.VERTICAL);
        left.setGravity(Gravity.CENTER_HORIZONTAL);
        ScrollView leftScroll = new ScrollView(this);
        leftScroll.setFillViewport(true);
        leftScroll.addView(left);
        columns.addView(leftScroll, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, 1.1f));

        ImageView logo = new ImageView(this);
        logo.setImageResource(R.drawable.banner);
        logo.setAdjustViewBounds(true);
        logo.setScaleType(ImageView.ScaleType.FIT_CENTER);
        left.addView(logo, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(92)));

        TextView subtitle = label(getString(R.string.launcher_subtitle), 13, 0x99FFFFFF, false);
        subtitle.setGravity(Gravity.CENTER);
        left.addView(subtitle, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout card = card();
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cardParams.topMargin = dp(12);
        left.addView(card, cardParams);
        card.addView(label(getString(R.string.section_game_files), 12, ACCENT, true));
        importStatus = label("", 13, 0xDDFFFFFF, false);
        importStatus.setPadding(0, dp(4), 0, dp(6));
        card.addView(importStatus);
        checksList = new LinearLayout(this);
        checksList.setOrientation(LinearLayout.VERTICAL);
        card.addView(checksList);
        shaderPanel = new LinearLayout(this);
        shaderPanel.setOrientation(LinearLayout.VERTICAL);
        shaderPanel.setPadding(0, dp(8), 0, 0);
        shaderPanel.setVisibility(View.GONE);
        shaderText = label("", 13, 0xFFFFFFFF, false);
        shaderPanel.addView(shaderText);
        shaderProgress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        shaderProgress.setMax(1000);
        shaderProgress.setProgressTintList(android.content.res.ColorStateList.valueOf(ACCENT));
        shaderPanel.addView(shaderProgress, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(10)));
        card.addView(shaderPanel);

        launchGame = actionButton(getString(R.string.action_play), true);
        launchGame.setVisibility(View.GONE);
        launchGame.setOnClickListener(view -> play());
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(62));
        playParams.topMargin = dp(12);
        left.addView(launchGame, playParams);

        selectFolder = actionButton(getString(R.string.action_choose_folder), false);
        selectFolder.setOnClickListener(view -> selectGameFolder());
        LinearLayout.LayoutParams folderParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(46));
        folderParams.topMargin = dp(10);
        left.addView(selectFolder, folderParams);

        // Right: graphics options.
        LinearLayout right = card();
        LinearLayout.LayoutParams rightParams = new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1f);
        rightParams.leftMargin = dp(24);
        columns.addView(right, rightParams);
        right.addView(label(getString(R.string.section_graphics), 12, ACCENT, true));
        TextView note = label(getString(R.string.graphics_note), 12, 0x88FFFFFF, false);
        note.setPadding(0, dp(2), 0, dp(4));
        right.addView(note);
        ScrollView scroll = new ScrollView(this);
        optionsList = new LinearLayout(this);
        optionsList.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(optionsList);
        right.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        sendReport = actionButton(getString(R.string.action_send_report), false);
        sendReport.setOnClickListener(view -> chooseReportDestination());
        LinearLayout.LayoutParams reportParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(48));
        reportParams.topMargin = dp(8);
        right.addView(sendReport, reportParams);
        refreshOptions();
        return screen;
    }

    // ---- Shader library ------------------------------------------------------------------------------------

    private void play() {
        if (GpuDrivers.selected(this) != null) {
            probeDriver(true);
            return;
        }
        playChecked();
    }

    private void playChecked() {
        launchGame.setEnabled(false);
        Diagnostics.recordLaunch(this);
        importer.execute(() -> {
            GameEdition edition = GameEdition.of(this, sharedGameRoot());
            String problem = "nativo".equals(GameOptions.get(this, GameOptions.RENDERER))
                    ? Diagnostics.incompatibility(this) : null;
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                launchGame.setEnabled(true);
                if (edition != null && !edition.supported()) {
                    // Another edition closes at once with "No function registered at 8262E768" (or 8262E9A8).
                    new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                            .setTitle(getString(R.string.edition_unsupported_title))
                            .setMessage(getString(R.string.edition_unsupported_msg,
                                    edition.describe(), GameEdition.SUPPORTED_NAME,
                                    GameEdition.SUPPORTED_SHA256.substring(0, 12)))
                            .setPositiveButton(getString(R.string.action_back), null)
                            .setNeutralButton(getString(R.string.action_play_anyway), (dialog, which) -> continuePlay(problem))
                            .show();
                    return;
                }
                continuePlay(problem);
            });
        });
    }

    private void continuePlay(String problem) {
        if (problem == null) {
            playCompatibleGame();
            return;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.renderer_test_title))
                .setMessage(getString(R.string.renderer_test_msg, problem))
                .setPositiveButton(getString(R.string.action_try_compat), (dialog, which) -> {
                    GameOptions.set(this, GameOptions.RENDERER, "xenos");
                    refreshOptions();
                    playCompatibleGame();
                })
                .setNeutralButton(getString(R.string.action_send_diag), (dialog, which) -> chooseReportDestination())
                .setNegativeButton(getString(R.string.action_back), null).show();
    }

    private void playCompatibleGame() {
        if ("xenos".equals(GameOptions.get(this, GameOptions.RENDERER)) || ShaderBuilder.hasLibrary(sharedGameRoot())) {
            startActivity(new Intent(this, GameActivity.class));
        } else {
            buildShaders(true);
        }
    }

    // ---- Tester reports ------------------------------------------------------------------------------------

    @Override
    protected void onSaveInstanceState(Bundle state) {
        if (pendingReport != null) {
            state.putString("pending_report", pendingReport.getName());
            state.putBoolean("pending_github", pendingGithub);
        }
        state.putBoolean("play_after_probe", playAfterProbe);
        super.onSaveInstanceState(state);
    }

    private void chooseReportDestination() {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.report_title))
                .setMessage(getString(R.string.report_msg))
                .setPositiveButton(getString(R.string.action_email), (dialog, which) -> prepareReport(0))
                .setNeutralButton(getString(R.string.action_github), (dialog, which) -> prepareReport(1))
                .setNegativeButton(getString(R.string.action_save_zip), (dialog, which) -> prepareReport(2)).show();
    }

    private void reportError(String message) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.diag_title)).setMessage(message).setPositiveButton(getString(R.string.action_ok), null).show();
    }

    private void prepareReport(int destination) {
        sendReport.setEnabled(false);
        sendReport.setText(getString(R.string.report_preparing));
        importer.execute(() -> {
            try {
                File report = Diagnostics.createReport(this);
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    sendReport.setEnabled(true);
                    sendReport.setText(getString(R.string.action_send_report));
                    if (destination == 0) shareReportByEmail(report);
                    else saveReport(report, destination == 1);
                });
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    sendReport.setEnabled(true);
                    sendReport.setText(getString(R.string.action_send_report));
                    reportError(getString(R.string.report_failed, error.getMessage()));
                });
            }
        });
    }

    private void shareReportByEmail(File report) {
        Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".reports", report);
        Intent email = new Intent(Intent.ACTION_SEND);
        email.setType("application/zip");
        email.putExtra(Intent.EXTRA_EMAIL, new String[]{Diagnostics.EMAIL});
        email.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.report_email_subject, Build.MODEL));
        email.putExtra(Intent.EXTRA_TEXT, getString(R.string.report_email_body, Diagnostics.summary(this)));
        email.putExtra(Intent.EXTRA_STREAM, uri);
        email.setClipData(ClipData.newUri(getContentResolver(), getString(R.string.diag_title) + " NFSMW", uri));
        email.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try { startActivity(Intent.createChooser(email, getString(R.string.report_email_chooser, Diagnostics.EMAIL))); }
        catch (ActivityNotFoundException error) { saveReport(report, false); }
    }

    private void saveReport(File report, boolean github) {
        pendingReport = report;
        pendingGithub = github;
        if (github) {
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(getString(R.string.report_github_title))
                    .setMessage(getString(R.string.report_github_msg))
                    .setPositiveButton(getString(R.string.action_save_open_github), (dialog, which) -> chooseReportFile())
                    .setNegativeButton(getString(R.string.action_cancel), (dialog, which) -> pendingReport = null).show();
        } else chooseReportFile();
    }

    private void chooseReportFile() {
        Intent save = new Intent(Intent.ACTION_CREATE_DOCUMENT);
        save.addCategory(Intent.CATEGORY_OPENABLE);
        save.setType("application/zip");
        save.putExtra(Intent.EXTRA_TITLE, pendingReport.getName());
        try { startActivityForResult(save, REQUEST_SAVE_REPORT); }
        catch (ActivityNotFoundException error) {
            pendingReport = null;
            reportError(getString(R.string.report_nosave_app));
        }
    }

    private void finishReportSave(Uri destination) {
        File report = pendingReport;
        boolean github = pendingGithub;
        pendingReport = null;
        if (report == null || !report.isFile()) {
            reportError(getString(R.string.report_gone));
            return;
        }
        importer.execute(() -> {
            try (InputStream input = new FileInputStream(report);
                 java.io.OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                if (output == null) throw new IOException(getString(R.string.file_open_failed));
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (!isFinishing() && !isDestroyed()) reportError(getString(R.string.file_save_failed, error.getMessage()));
                });
                return;
            }
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (github) {
                    String body = getString(R.string.report_github_body, Diagnostics.summary(this), report.getName());
                    Uri issue = Uri.parse(Diagnostics.ISSUES).buildUpon()
                            .appendQueryParameter("title", getString(R.string.report_issue_title, Build.MODEL))
                            .appendQueryParameter("body", body).build();
                    try { startActivity(new Intent(Intent.ACTION_VIEW, issue)); }
                    catch (ActivityNotFoundException error) { reportError(getString(R.string.report_saved_github, Diagnostics.ISSUES)); }
                } else reportError(getString(R.string.report_saved_zip, Diagnostics.EMAIL));
            });
        });
    }

    /** Builds nfsmw_shaders.nfsp from the game files (a minute or two, only once). */
    private void buildShaders(boolean thenPlay) {
        if (shaderBuilder != null) {
            return;
        }
        launchGame.setEnabled(false);
        launchGame.setAlpha(.5f);
        selectFolder.setEnabled(false);
        shaderPanel.setVisibility(View.VISIBLE);
        shaderProgress.setProgress(0);
        shaderText.setText(getString(R.string.shaders_building));
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        shaderBuilder = new ShaderBuilder(this, sharedGameRoot(), new ShaderBuilder.Listener() {
            @Override
            public void onProgress(float fraction, String text) {
                shaderProgress.setProgress(Math.round(fraction * 1000));
                shaderText.setText(text);
            }

            @Override
            public void onDone(File library) {
                shadersFinished();
                shaderPanel.setVisibility(View.GONE);
                showChecks(sharedGameRoot());
                if (thenPlay) {
                    startActivity(new Intent(MainActivity.this, GameActivity.class));
                }
            }

            @Override
            public void onError(String message) {
                shadersFinished();
                shaderText.setText(getString(R.string.shaders_failed, message));
                shaderProgress.setProgress(0);
            }
        });
        shaderBuilder.start();
    }

    private void shadersFinished() {
        shaderBuilder = null;
        getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        launchGame.setEnabled(true);
        launchGame.setAlpha(1f);
        selectFolder.setEnabled(true);
    }

    private void refreshOptions() {
        optionsList.removeAllViews();
        String launcherValue = LauncherLocale.get(this);
        optionsList.addView(optionRow(getString(R.string.option_launcher_language),
                LauncherLocale.label(this, launcherValue), this::chooseLauncherLanguage));
        GpuDrivers.Driver driver = GpuDrivers.selected(this);
        optionsList.addView(optionRow(getString(R.string.option_vulkan_driver), driver == null ? getString(R.string.driver_system) : driver.name,
                this::chooseDriver));
        optionsList.addView(optionRow(getString(R.string.option_test_driver), getString(R.string.option_test_driver_desc), () -> probeDriver(false)));
        optionsList.addView(optionRow(getString(R.string.option_check_updates),
                checkingUpdates ? getString(R.string.checking_updates) : getString(R.string.version_prefix, ReleaseUpdates.installed(this)),
                () -> checkUpdates(true)));
        for (GameOptions.Option option : GameOptions.ALL) {
            String value = GameOptions.get(this, option.key);
            optionsList.addView(optionRow(option.title(this), option.label(this, value), () -> chooseOption(option)));
        }
        SharedPreferences controls = getSharedPreferences("nfsmw_controls", MODE_PRIVATE);
        boolean stretch = controls.getBoolean("stretch", true);
        optionsList.addView(optionRow(getString(R.string.option_image_format),
                stretch ? getString(R.string.format_stretch) : getString(R.string.format_16x9), () -> {
                    controls.edit().putBoolean("stretch", !stretch).apply();
                    refreshOptions();
                }));
    }

    private void chooseLauncherLanguage() {
        String current = LauncherLocale.get(this);
        int checked = 0;
        String[] labels = new String[LauncherLocale.VALUES.length];
        for (int i = 0; i < LauncherLocale.VALUES.length; i++) {
            if (LauncherLocale.VALUES[i].equals(current)) checked = i;
            labels[i] = LauncherLocale.label(this, LauncherLocale.VALUES[i]);
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.option_launcher_language))
                .setSingleChoiceItems(labels, checked, (dialog, which) -> {
                    LauncherLocale.set(this, LauncherLocale.VALUES[which]);
                    dialog.dismiss();
                    recreate();
                })
                .setNegativeButton(getString(R.string.action_cancel), null)
                .show();
    }

    private void chooseDriver() {
        java.util.List<GpuDrivers.Driver> drivers = GpuDrivers.list(this);
        String[] names = new String[drivers.size() + 1];
        names[0] = getString(R.string.driver_system_full);
        int selected = 0;
        for (int i = 0; i < drivers.size(); ++i) {
            names[i + 1] = drivers.get(i).name;
            if (drivers.get(i).id.equals(GpuDrivers.selectedId(this))) selected = i + 1;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.driver_title))
                .setSingleChoiceItems(names, selected, (dialog, which) -> {
                    GpuDrivers.select(this, which == 0 ? "" : drivers.get(which - 1).id);
                    refreshOptions(); dialog.dismiss();
                })
                .setPositiveButton(getString(R.string.action_import_zip), (dialog, which) -> {
                    Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                            .addCategory(Intent.CATEGORY_OPENABLE);
                    startActivityForResult(picker, REQUEST_DRIVER_ZIP);
                })
                .setNeutralButton(getString(R.string.action_remove_selected), (dialog, which) -> {
                    GpuDrivers.Driver driver = GpuDrivers.selected(this);
                    if (driver == null) return;
                    try { GpuDrivers.remove(this, driver); refreshOptions(); }
                    catch (IOException error) { reportError(error.getMessage()); }
                })
                .setNegativeButton(getString(R.string.action_back), null).show();
    }

    private void probeDriver(boolean playAfter) {
        playAfterProbe = playAfter;
        launchGame.setEnabled(false);
        startActivityForResult(new Intent(this, GpuProbeActivity.class), REQUEST_DRIVER_PROBE);
    }

    private void showDriverReport(String report) {
        try {
            Diagnostics.acceptGpu(this, report);
            org.json.JSONObject gpu = new org.json.JSONObject(report);
            boolean failed = gpu.optBoolean("driverLoadFailed") || gpu.has("probeError");
            String compat = gpu.optBoolean("compatible")
                    ? getString(R.string.driver_compat_ok)
                    : getString(R.string.driver_compat_missing, String.valueOf(gpu.optJSONArray("missing")));
            String text = getString(R.string.driver_report_body,
                    gpu.optString("gpu", getString(R.string.driver_vulkan_unavailable)),
                    gpu.optString("vulkan", getString(R.string.driver_vulkan_unavailable)),
                    gpu.optInt("maxBoundDescriptorSets"), compat);
            if (failed) {
                String detail = gpu.optString("probeError", "");
                text = "Test closed or cancelled.".equals(detail)
                        ? getString(R.string.driver_report_cancelled)
                        : getString(R.string.driver_report_unavailable, detail);
            }
            if (playAfterProbe && !failed) { playAfterProbe = false; playChecked(); return; }
            playAfterProbe = false;
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle(getString(R.string.driver_report_title)).setMessage(text)
                    .setPositiveButton(getString(R.string.action_accept), null)
                    .setNeutralButton(getString(R.string.action_use_system), (dialog, which) -> {
                        GpuDrivers.select(this, ""); refreshOptions();
                    }).show();
        } catch (Exception error) { playAfterProbe = false; reportError(getString(R.string.driver_report_invalid, error.getMessage())); }
    }

    private void chooseOption(GameOptions.Option option) {
        String current = GameOptions.get(this, option.key);
        int checked = 0;
        for (int i = 0; i < option.values.length; i++) {
            if (option.values[i].equals(current)) checked = i;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(option.title(this))
                .setSingleChoiceItems(option.labels(this), checked, (dialog, which) -> {
                    GameOptions.set(this, option.key, option.values[which]);
                    dialog.dismiss();
                    refreshOptions();
                })
                .setNegativeButton(getString(R.string.action_cancel), null)
                .show();
    }

    private View optionRow(String title, String value, Runnable onClick) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(dp(12), dp(10), dp(12), dp(10));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0x14FFFFFF);
        bg.setCornerRadius(dp(10));
        row.setBackground(bg);
        row.setClickable(true);
        row.setOnClickListener(v -> onClick.run());
        row.addView(label(title, 14, 0xFFFFFFFF, false),
                new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        TextView current = label(value + "  ›", 13, ACCENT, true);
        current.setGravity(Gravity.END);
        row.addView(current);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(6);
        row.setLayoutParams(lp);
        return row;
    }

    private void showChecks(File folder) {
        checksList.removeAllViews();
        addCheck(getString(R.string.check_xex), new File(folder, "default.xex").isFile(), true);
        addCheck(getString(R.string.check_nfs), new File(folder, "NFS").isDirectory(), true);
        addCheck(getString(R.string.check_movies), new File(folder, "Movies").isDirectory(), true);
        addCheck(getString(R.string.check_shaders), ShaderBuilder.hasLibrary(folder), false);
    }

    private void addCheck(String name, boolean ok, boolean required) {
        String mark = ok ? "✓  " : (required ? "✗  " : "!  ");
        String warning = ok || required ? "" : getString(R.string.shaders_on_play);
        TextView row = label(mark + name + warning, 13,
                ok ? 0xFF7CD992 : (required ? 0xFFFF6B6B : 0xFFFFC857), false);
        row.setPadding(0, dp(2), 0, dp(2));
        checksList.addView(row);
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(12), dp(16), dp(12));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xCC121A22);
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), 0x33FFFFFF);
        card.setBackground(bg);
        return card;
    }

    private Button actionButton(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(primary ? 0xFF1A1000 : Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, primary ? 22 : 15);
        b.setTypeface(Typeface.DEFAULT_BOLD);
        b.setLetterSpacing(primary ? .12f : 0f);
        GradientDrawable bg = primary
                ? new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, new int[] {0xFFFFB300, ACCENT})
                : new GradientDrawable();
        if (!primary) {
            bg.setColor(0x22FFFFFF);
            bg.setStroke(dp(1), 0x55FFFFFF);
        }
        bg.setCornerRadius(dp(14));
        b.setBackground(bg);
        b.setStateListAnimator(null);
        return b;
    }

    private TextView label(String text, int sp, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        if (bold) {
            t.setTypeface(Typeface.DEFAULT_BOLD);
            t.setLetterSpacing(.06f);
        }
        return t;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void selectGameFolder() {
        if (!hasStorageAccess()) {
            prepareSharedGameFolder();
            return;
        }
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        String saved = getPreferences(MODE_PRIVATE).getString(TREE_URI, null);
        if (saved != null && Build.VERSION.SDK_INT >= 26) {
            intent.putExtra("android.provider.extra.INITIAL_URI", Uri.parse(saved));
        }
        startActivityForResult(intent, REQUEST_GAME_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_DRIVER_PROBE) {
            launchGame.setEnabled(true);
            showDriverReport(resultCode == RESULT_OK && data != null ? data.getStringExtra("gpu_report")
                    : "{\"probeError\":\"Test closed or cancelled.\",\"driverLoadFailed\":true}");
            return;
        }
        if (requestCode == REQUEST_DRIVER_ZIP) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
            Uri zip = data.getData();
            setImportStatus(getString(R.string.driver_importing));
            importer.execute(() -> {
                try {
                    GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                    mainHandler.post(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        GpuDrivers.select(this, driver.id); refreshOptions();
                        setImportStatus(getString(R.string.driver_imported, driver.name));
                    });
                } catch (Exception error) {
                    mainHandler.post(() -> reportError(getString(R.string.driver_import_failed, error.getMessage())));
                }
            });
            return;
        }
        if (requestCode == REQUEST_SAVE_REPORT) {
            if (resultCode == RESULT_OK && data != null && data.getData() != null) finishReportSave(data.getData());
            else pendingReport = null;
            return;
        }
        if (requestCode == REQUEST_STORAGE_ACCESS || requestCode == REQUEST_LEGACY_STORAGE) {
            if (hasStorageAccess()) prepareSharedGameFolder();
            else {
                selectFolder.setEnabled(true);
                setImportStatus(getString(R.string.storage_enable, GAME_FOLDER_NAME));
            }
            return;
        }
        if (requestCode != REQUEST_GAME_FOLDER || resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }

        Uri tree = data.getData();
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(tree, flags & Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException error) {
            setImportStatus(getString(R.string.no_permission_saved, error.getMessage()));
            return;
        }
        getPreferences(MODE_PRIVATE).edit().putString(TREE_URI, tree.toString()).apply();
        importGameFolder(tree);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_LEGACY_STORAGE) {
            if (hasStorageAccess()) prepareSharedGameFolder();
            else {
                selectFolder.setEnabled(true);
                setImportStatus(getString(R.string.storage_enable, GAME_FOLDER_NAME));
            }
        }
    }

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) return Environment.isExternalStorageManager();
        return checkSelfPermission(android.Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void prepareSharedGameFolder() {
        if (!hasStorageAccess()) {
            selectFolder.setEnabled(false);
            launchGame.setVisibility(View.GONE);
            setImportStatus(getString(R.string.storage_permission, GAME_FOLDER_NAME));
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                Intent settings = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivityForResult(settings, REQUEST_STORAGE_ACCESS);
            } else {
                requestPermissions(new String[]{android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_LEGACY_STORAGE);
            }
            return;
        }

        selectFolder.setEnabled(false);
        launchGame.setVisibility(View.GONE);
        setImportStatus(getString(R.string.reviewing_game));
        importer.execute(() -> {
            File gameRoot = sharedGameRoot();
            File oldPrivateRoot = privateGameRoot();
            File parent = gameRoot.getParentFile();
            File staging = new File(parent, "." + GAME_FOLDER_NAME + ".importing");
            File previous = new File(parent, "." + GAME_FOLDER_NAME + ".previous");
            try {
                if (isValidGameFolder(gameRoot)) {
                    if (oldPrivateRoot.exists()) deleteRecursively(oldPrivateRoot);
                    mainHandler.post(() -> showSharedGameFolder(null));
                    return;
                }

                if (isValidGameFolder(oldPrivateRoot)) {
                    mainHandler.post(() -> setImportStatus(getString(R.string.moving_game, GAME_FOLDER_NAME)));
                    deleteRecursively(staging);
                    deleteRecursively(previous);
                    copyDirectory(oldPrivateRoot, staging, staging.getCanonicalFile(), new long[]{0});
                    if (!isValidGameFolder(staging)) throw new IOException(getString(R.string.copy_failed_verify));
                    installStagedGame(staging, gameRoot, previous);
                    deleteRecursively(oldPrivateRoot);
                    deleteRecursively(previous);
                    mainHandler.post(() -> showSharedGameFolder(getString(R.string.moved_from_private)));
                    return;
                }

                if (!gameRoot.mkdirs() && !gameRoot.isDirectory()) {
                    throw new IOException(getString(R.string.create_failed, gameRoot.getAbsolutePath()));
                }
                mainHandler.post(() -> showSharedGameFolder(null));
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) {}
                mainHandler.post(() -> {
                    setImportStatus(getString(R.string.prepare_failed, error.getMessage()));
                    selectFolder.setEnabled(true);
                });
            }
        });
    }

    private File sharedGameRoot() {
        return new File(Environment.getExternalStorageDirectory(), GAME_FOLDER_NAME);
    }

    private File privateGameRoot() {
        return new File(new File(getFilesDir(), "nfsmw"), "game_root");
    }

    private static boolean isValidGameFolder(File folder) {
        return new File(folder, "default.xex").isFile() &&
                new File(folder, "NFS").isDirectory() && new File(folder, "Movies").isDirectory();
    }

    private static void installStagedGame(File staging, File gameRoot, File previous) throws IOException {
        boolean movedPrevious = false;
        if (gameRoot.exists()) {
            Files.move(gameRoot.toPath(), previous.toPath(), StandardCopyOption.REPLACE_EXISTING);
            movedPrevious = true;
        }
        try {
            Files.move(staging.toPath(), gameRoot.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } catch (IOException error) {
            if (movedPrevious) Files.move(previous.toPath(), gameRoot.toPath(), StandardCopyOption.REPLACE_EXISTING);
            throw error;
        }
    }

    private void importGameFolder(Uri tree) {
        selectFolder.setEnabled(false);
        launchGame.setVisibility(View.GONE);
        setImportStatus(getString(R.string.copying_game, GAME_FOLDER_NAME));
        importer.execute(() -> {
            File gameRoot = sharedGameRoot();
            File parent = gameRoot.getParentFile();
            File staging = new File(parent, "." + GAME_FOLDER_NAME + ".importing");
            File previous = new File(parent, "." + GAME_FOLDER_NAME + ".previous");
            try {
                deleteRecursively(staging);
                deleteRecursively(previous);
                DocumentFile source = DocumentFile.fromTreeUri(this, tree);
                if (source == null || !source.isDirectory()) throw new IOException(getString(R.string.open_folder_failed));
                copyDirectory(source, staging, staging.getCanonicalFile(), new long[]{0});
                if (!isValidGameFolder(staging)) {
                    throw new IOException(getString(R.string.folder_must_contain));
                }
                installStagedGame(staging, gameRoot, previous);
                File oldPrivateRoot = privateGameRoot();
                if (oldPrivateRoot.exists()) deleteRecursively(oldPrivateRoot);
                deleteRecursively(previous);
                mainHandler.post(() -> {
                    showSharedGameFolder(null);
                    if ("nativo".equals(GameOptions.get(this, GameOptions.RENDERER)) &&
                            !ShaderBuilder.hasLibrary(sharedGameRoot())) buildShaders(false);
                });
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) {}
                mainHandler.post(() -> setImportStatus(getString(R.string.import_failed, error.getMessage())));
            } finally {
                mainHandler.post(() -> selectFolder.setEnabled(shaderBuilder == null));
            }
        });
    }

    private void copyDirectory(DocumentFile source, File destination, File safeRoot, long[] copiedBytes) throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException(getString(R.string.invalid_path));
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException(getString(R.string.create_failed, destination));
        for (DocumentFile child : source.listFiles()) {
            String name = child.getName();
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                    name.contains("/") || name.contains("\\")) {
                throw new IOException(getString(R.string.invalid_name));
            }
            File target = new File(destination, name);
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                File parent = target.getParentFile();
                if (parent == null || !parent.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
                    throw new IOException(getString(R.string.escape_file));
                }
                if (!parent.mkdirs() && !parent.isDirectory()) throw new IOException(getString(R.string.create_folder_failed));
                try (InputStream input = getContentResolver().openInputStream(child.getUri());
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) throw new IOException(getString(R.string.read_failed, name));
                    byte[] buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((32L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus(getString(R.string.copied_mib, total / (1024 * 1024))));
                        }
                    }
                    output.getFD().sync();
                }
            }
        }
    }

    private void copyDirectory(File source, File destination, File safeRoot, long[] copiedBytes) throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException(getString(R.string.invalid_path));
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException(getString(R.string.create_failed, destination));
        File[] children = source.listFiles();
        if (children == null) throw new IOException(getString(R.string.read_failed, source.getAbsolutePath()));
        byte[] buffer = new byte[1024 * 1024];
        for (File child : children) {
            File target = new File(destination, child.getName());
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                try (InputStream input = new FileInputStream(child);
                     FileOutputStream output = new FileOutputStream(target)) {
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((128L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus(getString(R.string.moved_mib, total / (1024 * 1024))));
                        }
                    }
                    output.getFD().sync();
                }
            }
        }
    }

    private void showSharedGameFolder(String notice) {
        File gameRoot = sharedGameRoot();
        selectFolder.setEnabled(true);
        showChecks(gameRoot);
        if (isValidGameFolder(gameRoot)) {
            setImportStatus((notice == null ? "" : notice + "\n") + getString(R.string.show_folder, GAME_FOLDER_NAME));
            launchGame.setVisibility(View.VISIBLE);
            selectFolder.setText(getString(R.string.action_change_folder));
        } else {
            setImportStatus(getString(R.string.choose_folder_hint, GAME_FOLDER_NAME));
            launchGame.setVisibility(View.GONE);
            selectFolder.setText(getString(R.string.action_choose_folder));
        }
    }

    private void setImportStatus(String message) {
        if (importStatus != null) importStatus.setText(message);
    }

    private void deleteRecursively(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        if (!file.delete()) throw new IOException(getString(R.string.delete_failed, file.getName()));
    }

    @Override
    protected void onDestroy() {
        if (shaderBuilder != null) {
            shaderBuilder.cancel();
        }
        importer.shutdown();
        super.onDestroy();
    }
}
