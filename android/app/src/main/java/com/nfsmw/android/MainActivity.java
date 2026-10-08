package com.nfsmw.android;

import android.app.Activity;
import android.app.AlertDialog;
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
                    .setTitle("Actualizaciones").setMessage("Tienes la versión " + ReleaseUpdates.installed(this)
                            + ". No hay una versión estable más reciente en GitHub.")
                    .setPositiveButton("Aceptar", null).show();
            return;
        }
        android.content.SharedPreferences updates = ReleaseUpdates.prefs(this);
        if (!manual && result.tag.equals(updates.getString("notified_tag", ""))) return;
        updates.edit().putString("notified_tag", result.tag).apply();
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Nueva versión disponible: " + result.tag)
                .setMessage("Puedes actualizar desde GitHub o seguir jugando con esta versión. Instala el APK "
                        + "encima del anterior para conservar tus partidas y ajustes.")
                .setPositiveButton("Actualizar", (dialog, which) -> {
                    try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(result.url))); }
                    catch (ActivityNotFoundException error) { reportError("No se encontró una app para abrir GitHub."); }
                })
                .setNegativeButton("Más tarde", null).show();
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

        TextView subtitle = label("Xbox 360 · recompilado para Android", 13, 0x99FFFFFF, false);
        subtitle.setGravity(Gravity.CENTER);
        left.addView(subtitle, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        LinearLayout card = card();
        LinearLayout.LayoutParams cardParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        cardParams.topMargin = dp(12);
        left.addView(card, cardParams);
        card.addView(label("ARCHIVOS DEL JUEGO", 12, ACCENT, true));
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

        launchGame = actionButton("JUGAR", true);
        launchGame.setVisibility(View.GONE);
        launchGame.setOnClickListener(view -> play());
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, dp(62));
        playParams.topMargin = dp(12);
        left.addView(launchGame, playParams);

        selectFolder = actionButton("Elegir carpeta del juego", false);
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
        right.addView(label("OPCIONES GRÁFICAS", 12, ACCENT, true));
        TextView note = label("Se aplican al iniciar el juego.", 12, 0x88FFFFFF, false);
        note.setPadding(0, dp(2), 0, dp(4));
        right.addView(note);
        ScrollView scroll = new ScrollView(this);
        optionsList = new LinearLayout(this);
        optionsList.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(optionsList);
        right.addView(scroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));
        sendReport = actionButton("Enviar crash o log", false);
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
                            .setTitle("Edición del juego no compatible")
                            .setMessage("Tu default.xex es de la edición " + edition.describe() + ". Esta versión "
                                    + "de la app está recompilada a partir de la edición " + GameEdition.SUPPORTED_NAME
                                    + " y con otra edición el juego se cierra al arrancar.\n\nUsa una copia de la edición "
                                    + GameEdition.SUPPORTED_NAME + " (el SHA-256 de su default.xex empieza por "
                                    + GameEdition.SUPPORTED_SHA256.substring(0, 12) + ").")
                            .setPositiveButton("Volver", null)
                            .setNeutralButton("Jugar igualmente", (dialog, which) -> continuePlay(problem))
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
                .setTitle("Probar otro renderizador")
                .setMessage(problem + "\n\nPuedes probar el modo de compatibilidad experimental. Puede tener errores gráficos o funcionar más lento.")
                .setPositiveButton("Probar compatibilidad", (dialog, which) -> {
                    GameOptions.set(this, GameOptions.RENDERER, "xenos");
                    refreshOptions();
                    playCompatibleGame();
                })
                .setNeutralButton("Enviar diagnóstico", (dialog, which) -> chooseReportDestination())
                .setNegativeButton("Volver", null).show();
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
                .setTitle("Enviar crash o log")
                .setMessage("El informe incluye modelo, Android, GPU, ajustes y registros de esta app. Añade qué ocurrió antes de enviarlo.")
                .setPositiveButton("Correo", (dialog, which) -> prepareReport(0))
                .setNeutralButton("GitHub", (dialog, which) -> prepareReport(1))
                .setNegativeButton("Guardar ZIP", (dialog, which) -> prepareReport(2)).show();
    }

    private void reportError(String message) {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Diagnóstico").setMessage(message).setPositiveButton("Aceptar", null).show();
    }

    private void prepareReport(int destination) {
        sendReport.setEnabled(false);
        sendReport.setText("Preparando diagnóstico…");
        importer.execute(() -> {
            try {
                File report = Diagnostics.createReport(this);
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    sendReport.setEnabled(true);
                    sendReport.setText("Enviar crash o log");
                    if (destination == 0) shareReportByEmail(report);
                    else saveReport(report, destination == 1);
                });
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (isFinishing() || isDestroyed()) return;
                    sendReport.setEnabled(true);
                    sendReport.setText("Enviar crash o log");
                    reportError("No se pudo preparar el informe: " + error.getMessage());
                });
            }
        });
    }

    private void shareReportByEmail(File report) {
        Uri uri = FileProvider.getUriForFile(this, getPackageName() + ".reports", report);
        Intent email = new Intent(Intent.ACTION_SEND);
        email.setType("application/zip");
        email.putExtra(Intent.EXTRA_EMAIL, new String[]{Diagnostics.EMAIL});
        email.putExtra(Intent.EXTRA_SUBJECT, "NFSMW Android: " + Build.MODEL + " — diagnóstico");
        email.putExtra(Intent.EXTRA_TEXT, Diagnostics.summary(this) + "\nQué ocurrió:\n\nPasos para reproducirlo:\n\nEdición del juego:\n\nAdjunto el diagnóstico.");
        email.putExtra(Intent.EXTRA_STREAM, uri);
        email.setClipData(ClipData.newUri(getContentResolver(), "Diagnóstico NFSMW", uri));
        email.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try { startActivity(Intent.createChooser(email, "Enviar a " + Diagnostics.EMAIL)); }
        catch (ActivityNotFoundException error) { saveReport(report, false); }
    }

    private void saveReport(File report, boolean github) {
        pendingReport = report;
        pendingGithub = github;
        if (github) {
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Reportar en GitHub")
                    .setMessage("Primero guarda el ZIP. Después se abrirá el issue: describe el problema y adjunta el ZIP guardado.")
                    .setPositiveButton("Guardar y abrir GitHub", (dialog, which) -> chooseReportFile())
                    .setNegativeButton("Cancelar", (dialog, which) -> pendingReport = null).show();
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
            reportError("No hay una aplicación para guardar archivos. Prueba la opción Correo.");
        }
    }

    private void finishReportSave(Uri destination) {
        File report = pendingReport;
        boolean github = pendingGithub;
        pendingReport = null;
        if (report == null || !report.isFile()) {
            reportError("El informe ya no está disponible. Genera uno nuevo.");
            return;
        }
        importer.execute(() -> {
            try (InputStream input = new FileInputStream(report);
                 java.io.OutputStream output = getContentResolver().openOutputStream(destination, "w")) {
                if (output == null) throw new IOException("No se pudo abrir el archivo de destino.");
                byte[] buffer = new byte[16384];
                for (int count; (count = input.read(buffer)) != -1;) output.write(buffer, 0, count);
            } catch (Exception error) {
                mainHandler.post(() -> {
                    if (!isFinishing() && !isDestroyed()) reportError("No se pudo guardar el ZIP: " + error.getMessage());
                });
                return;
            }
            mainHandler.post(() -> {
                if (isFinishing() || isDestroyed()) return;
                if (github) {
                    String body = "### Qué ocurrió\n\n### Pasos para reproducirlo\n\n### Edición del juego\n\n### Dispositivo\n"
                            + Diagnostics.summary(this) + "\n### Diagnóstico\nAdjunta aquí el archivo **" + report.getName() + "** que acabas de guardar.\n";
                    Uri issue = Uri.parse(Diagnostics.ISSUES).buildUpon()
                            .appendQueryParameter("title", "[Android] " + Build.MODEL + ": ")
                            .appendQueryParameter("body", body).build();
                    try { startActivity(new Intent(Intent.ACTION_VIEW, issue)); }
                    catch (ActivityNotFoundException error) { reportError("ZIP guardado. Abre " + Diagnostics.ISSUES + " y adjúntalo al issue."); }
                } else reportError("ZIP guardado. Puedes adjuntarlo a un issue o enviarlo a " + Diagnostics.EMAIL + ".");
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
        shaderText.setText("Generando los shaders del renderizador nativo (solo la primera vez)…");
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
                shaderText.setText("No se pudieron generar los shaders: " + message);
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
        GpuDrivers.Driver driver = GpuDrivers.selected(this);
        optionsList.addView(optionRow("Driver Vulkan", driver == null ? "Del sistema" : driver.name,
                this::chooseDriver));
        optionsList.addView(optionRow("Probar driver", "Comprobar GPU y funciones Vulkan", () -> probeDriver(false)));
        optionsList.addView(optionRow("Buscar actualizaciones",
                checkingUpdates ? "Comprobando GitHub…" : "Versión " + ReleaseUpdates.installed(this),
                () -> checkUpdates(true)));
        for (GameOptions.Option option : GameOptions.ALL) {
            String value = GameOptions.get(this, option.key);
            optionsList.addView(optionRow(option.title, option.label(value), () -> chooseOption(option)));
        }
        SharedPreferences controls = getSharedPreferences("nfsmw_controls", MODE_PRIVATE);
        boolean stretch = controls.getBoolean("stretch", true);
        optionsList.addView(optionRow("Formato de imagen",
                stretch ? "Estirada a toda la pantalla" : "Original 16:9", () -> {
                    controls.edit().putBoolean("stretch", !stretch).apply();
                    refreshOptions();
                }));
    }

    private void chooseDriver() {
        java.util.List<GpuDrivers.Driver> drivers = GpuDrivers.list(this);
        String[] names = new String[drivers.size() + 1];
        names[0] = "Driver del sistema";
        int selected = 0;
        for (int i = 0; i < drivers.size(); ++i) {
            names[i + 1] = drivers.get(i).name;
            if (drivers.get(i).id.equals(GpuDrivers.selectedId(this))) selected = i + 1;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("Driver Vulkan")
                .setSingleChoiceItems(names, selected, (dialog, which) -> {
                    GpuDrivers.select(this, which == 0 ? "" : drivers.get(which - 1).id);
                    refreshOptions(); dialog.dismiss();
                })
                .setPositiveButton("Importar ZIP", (dialog, which) -> {
                    Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT).setType("*/*")
                            .addCategory(Intent.CATEGORY_OPENABLE);
                    startActivityForResult(picker, REQUEST_DRIVER_ZIP);
                })
                .setNeutralButton("Eliminar seleccionado", (dialog, which) -> {
                    GpuDrivers.Driver driver = GpuDrivers.selected(this);
                    if (driver == null) return;
                    try { GpuDrivers.remove(this, driver); refreshOptions(); }
                    catch (IOException error) { reportError(error.getMessage()); }
                })
                .setNegativeButton("Volver", null).show();
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
            String text = "GPU: " + gpu.optString("gpu", "no disponible")
                    + "\nVulkan: " + gpu.optString("vulkan", "no disponible")
                    + "\nConjuntos de descriptores: " + gpu.optInt("maxBoundDescriptorSets")
                    + "\n" + (gpu.optBoolean("compatible") ? "Funciones del renderizador nativo disponibles."
                            : "Funciones faltantes: " + gpu.optJSONArray("missing"));
            if (failed) text = "No se pudo probar el driver. " + gpu.optString("probeError", "Error al cargar Vulkan.");
            if (playAfterProbe && !failed) { playAfterProbe = false; playChecked(); return; }
            playAfterProbe = false;
            new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("Prueba del driver Vulkan").setMessage(text)
                    .setPositiveButton("Aceptar", null)
                    .setNeutralButton("Usar sistema", (dialog, which) -> {
                        GpuDrivers.select(this, ""); refreshOptions();
                    }).show();
        } catch (Exception error) { playAfterProbe = false; reportError("Diagnóstico no válido: " + error.getMessage()); }
    }

    private void chooseOption(GameOptions.Option option) {
        String current = GameOptions.get(this, option.key);
        int checked = 0;
        for (int i = 0; i < option.values.length; i++) {
            if (option.values[i].equals(current)) checked = i;
        }
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(option.title)
                .setSingleChoiceItems(option.labels, checked, (dialog, which) -> {
                    GameOptions.set(this, option.key, option.values[which]);
                    dialog.dismiss();
                    refreshOptions();
                })
                .setNegativeButton("Cancelar", null)
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
        addCheck("default.xex", new File(folder, "default.xex").isFile(), true);
        addCheck("Carpeta NFS", new File(folder, "NFS").isDirectory(), true);
        addCheck("Carpeta Movies", new File(folder, "Movies").isDirectory(), true);
        addCheck("Shaders del renderizador nativo", ShaderBuilder.hasLibrary(folder), false);
    }

    private void addCheck(String name, boolean ok, boolean required) {
        String mark = ok ? "✓  " : (required ? "✗  " : "!  ");
        String warning = ok || required ? "" : " · se generarán al pulsar JUGAR";
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
                    : "{\"probeError\":\"La prueba se cerró o fue cancelada. Puedes volver al driver del sistema.\",\"driverLoadFailed\":true}");
            return;
        }
        if (requestCode == REQUEST_DRIVER_ZIP) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
            Uri zip = data.getData();
            setImportStatus("Importando driver Vulkan…");
            importer.execute(() -> {
                try {
                    GpuDrivers.Driver driver = GpuDrivers.importZip(this, zip);
                    mainHandler.post(() -> {
                        if (isFinishing() || isDestroyed()) return;
                        GpuDrivers.select(this, driver.id); refreshOptions();
                        setImportStatus("Driver importado: " + driver.name + ". Usa Probar driver para comprobarlo.");
                    });
                } catch (Exception error) {
                    mainHandler.post(() -> reportError("No se pudo importar el driver: " + error.getMessage()));
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
                setImportStatus("Activa el permiso de almacenamiento para usar Memoria interna/" + GAME_FOLDER_NAME + ".");
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
            setImportStatus("No se pudo guardar el permiso de la carpeta: " + error.getMessage());
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
                setImportStatus("Activa el permiso de almacenamiento para usar Memoria interna/" + GAME_FOLDER_NAME + ".");
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
            setImportStatus("Para usar Memoria interna/" + GAME_FOLDER_NAME + ", activa el acceso a archivos que solicita Android.");
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
        setImportStatus("Revisando el juego en la memoria interna...");
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
                    mainHandler.post(() -> setImportStatus("Moviendo el juego a Memoria interna/" + GAME_FOLDER_NAME + "..."));
                    deleteRecursively(staging);
                    deleteRecursively(previous);
                    copyDirectory(oldPrivateRoot, staging, staging.getCanonicalFile(), new long[]{0});
                    if (!isValidGameFolder(staging)) throw new IOException("La copia no pasó la verificación de archivos.");
                    installStagedGame(staging, gameRoot, previous);
                    deleteRecursively(oldPrivateRoot);
                    deleteRecursively(previous);
                    mainHandler.post(() -> showSharedGameFolder("Juego movido desde el almacenamiento privado."));
                    return;
                }

                if (!gameRoot.mkdirs() && !gameRoot.isDirectory()) {
                    throw new IOException("No se pudo crear " + gameRoot.getAbsolutePath());
                }
                mainHandler.post(() -> showSharedGameFolder(null));
            } catch (Exception error) {
                try { deleteRecursively(staging); } catch (IOException ignored) {}
                mainHandler.post(() -> {
                    setImportStatus("No se pudo preparar la carpeta del juego: " + error.getMessage());
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
        setImportStatus("Copiando el juego a Memoria interna/" + GAME_FOLDER_NAME + "...");
        importer.execute(() -> {
            File gameRoot = sharedGameRoot();
            File parent = gameRoot.getParentFile();
            File staging = new File(parent, "." + GAME_FOLDER_NAME + ".importing");
            File previous = new File(parent, "." + GAME_FOLDER_NAME + ".previous");
            try {
                deleteRecursively(staging);
                deleteRecursively(previous);
                DocumentFile source = DocumentFile.fromTreeUri(this, tree);
                if (source == null || !source.isDirectory()) throw new IOException("No se pudo abrir la carpeta seleccionada.");
                copyDirectory(source, staging, staging.getCanonicalFile(), new long[]{0});
                if (!isValidGameFolder(staging)) {
                    throw new IOException("La carpeta debe contener default.xex, NFS y Movies.");
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
                mainHandler.post(() -> setImportStatus("Error al importar: " + error.getMessage()));
            } finally {
                mainHandler.post(() -> selectFolder.setEnabled(shaderBuilder == null));
            }
        });
    }

    private void copyDirectory(DocumentFile source, File destination, File safeRoot, long[] copiedBytes) throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException("La carpeta del juego contiene una ruta no válida.");
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException("No se pudo crear " + destination);
        for (DocumentFile child : source.listFiles()) {
            String name = child.getName();
            if (name == null || name.isEmpty() || name.equals(".") || name.equals("..") ||
                    name.contains("/") || name.contains("\\")) {
                throw new IOException("La carpeta del juego contiene un nombre no válido.");
            }
            File target = new File(destination, name);
            if (child.isDirectory()) {
                copyDirectory(child, target, safeRoot, copiedBytes);
            } else if (child.isFile()) {
                File parent = target.getParentFile();
                if (parent == null || !parent.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
                    throw new IOException("Un archivo intenta salir de la carpeta de importación.");
                }
                if (!parent.mkdirs() && !parent.isDirectory()) throw new IOException("No se pudo crear una carpeta del juego.");
                try (InputStream input = getContentResolver().openInputStream(child.getUri());
                     FileOutputStream output = new FileOutputStream(target)) {
                    if (input == null) throw new IOException("No se pudo leer " + name);
                    byte[] buffer = new byte[1024 * 1024];
                    int count;
                    while ((count = input.read(buffer)) != -1) {
                        output.write(buffer, 0, count);
                        copiedBytes[0] += count;
                        if ((copiedBytes[0] & ((32L * 1024 * 1024) - 1)) < count) {
                            long total = copiedBytes[0];
                            mainHandler.post(() -> setImportStatus("Copiados " + (total / (1024 * 1024)) + " MiB..."));
                        }
                    }
                    output.getFD().sync();
                }
            }
        }
    }

    private void copyDirectory(File source, File destination, File safeRoot, long[] copiedBytes) throws IOException {
        if (!destination.getCanonicalFile().toPath().startsWith(safeRoot.toPath())) {
            throw new IOException("La carpeta del juego contiene una ruta no válida.");
        }
        if (!destination.mkdirs() && !destination.isDirectory()) throw new IOException("No se pudo crear " + destination);
        File[] children = source.listFiles();
        if (children == null) throw new IOException("No se pudo leer " + source.getAbsolutePath());
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
                            mainHandler.post(() -> setImportStatus("Movidos " + (total / (1024 * 1024)) + " MiB..."));
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
            setImportStatus((notice == null ? "" : notice + "\n") + "Memoria interna/" + GAME_FOLDER_NAME);
            launchGame.setVisibility(View.VISIBLE);
            selectFolder.setText("Cambiar carpeta del juego");
        } else {
            setImportStatus("Elige la carpeta extraída del juego (con default.xex, NFS y Movies). Se copiará a " +
                    "Memoria interna/" + GAME_FOLDER_NAME + ".");
            launchGame.setVisibility(View.GONE);
            selectFolder.setText("Elegir carpeta del juego");
        }
    }

    private void setImportStatus(String message) {
        if (importStatus != null) importStatus.setText(message);
    }

    private static void deleteRecursively(File file) throws IOException {
        if (!file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteRecursively(child);
        if (!file.delete()) throw new IOException("No se pudo borrar " + file.getName());
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
