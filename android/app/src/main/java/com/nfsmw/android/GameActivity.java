package com.nfsmw.android;

import android.app.AlertDialog;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.util.TypedValue;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;

public final class GameActivity extends SDLActivity
        implements TouchControlsView.Host, InputManager.InputDeviceListener, SensorEventListener {
    private static final String TAG = "NFSMW";
    private static final String SETTINGS = "nfsmw.toml";

    private String gameRoot;
    private String userRoot;
    private String cacheRoot;
    private TouchControlsView controls;
    private LinearLayout toolbar;
    private InputManager inputManager;
    private SensorManager sensorManager;
    private Sensor gravity;

    private static native void nativeSetStretch(boolean stretch);

    @Override
    protected void attachBaseContext(Context context) {
        super.attachBaseContext(LauncherLocale.apply(context));
    }

    @Override
    protected void onCreate(Bundle state) {
        File appRoot = new File(getFilesDir(), "nfsmw");
        gameRoot = new File(Environment.getExternalStorageDirectory(), MainActivity.GAME_FOLDER_NAME).getAbsolutePath();
        userRoot = new File(appRoot, "user").getAbsolutePath();
        cacheRoot = new File(appRoot, "cache").getAbsolutePath();
        new File(userRoot).mkdirs();
        new File(cacheRoot).mkdirs();
        // The native side keeps nfsmw.toml, the pipeline cache and its logs in the "executable folder", which
        // for app_process would be /system/bin (see GetExecutableFolder in filesystem_posix.cpp).
        try {
            Os.setenv("REX_APP_FOLDER", userRoot, true);
        } catch (ErrnoException e) {
            Log.w(TAG, "REX_APP_FOLDER", e);
        }
        installSettings();
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(state);
        useWholeScreen();

        ViewGroup root = findViewById(android.R.id.content);
        controls = new TouchControlsView(this);
        controls.setHost(this);
        root.addView(controls, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT, Gravity.FILL));
        toolbar = buildToolbar();
        toolbar.setVisibility(View.GONE);
        // In the middle of the screen, the one area the default layout leaves free, so no control hides under it.
        FrameLayout.LayoutParams barParams = new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT, Gravity.CENTER);
        root.addView(toolbar, barParams);

        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        if (sensorManager != null) {
            gravity = sensorManager.getDefaultSensor(Sensor.TYPE_GRAVITY);
            if (gravity == null) {
                gravity = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            }
        }
    }

    // SDL picks an orientation from the window size when it creates its window; the game is landscape only.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    // The image and the controls use all of the panel, camera cutout included, at its highest refresh rate
    // when the FPS limit asks for more than 60.
    private void useWholeScreen() {
        WindowManager.LayoutParams attrs = getWindow().getAttributes();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            attrs.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }
        int fps = Integer.parseInt(GameOptions.get(this, GameOptions.FPS));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode best = null;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() != display.getMode().getPhysicalWidth()) {
                    continue;
                }
                // The lowest mode that still shows every frame the game makes.
                if (mode.getRefreshRate() + 0.5f >= fps &&
                        (best == null || mode.getRefreshRate() < best.getRefreshRate())) {
                    best = mode;
                }
            }
            if (best != null) {
                attrs.preferredDisplayModeId = best.getModeId();
            }
        }
        getWindow().setAttributes(attrs);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (inputManager != null) {
            inputManager.registerInputDeviceListener(this, null);
            refreshGamepads();
        }
        updateTiltListener();
        applyStretch(controls.stretch);
    }

    private static void applyStretch(boolean stretch) {
        try {
            nativeSetStretch(stretch);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "libmain not loaded", e);
        }
    }

    @Override
    protected void onPause() {
        if (inputManager != null) {
            inputManager.unregisterInputDeviceListener(this);
        }
        if (sensorManager != null) {
            sensorManager.unregisterListener(this);
        }
        if (controls.isEditing()) {
            controls.setEditing(false);
        }
        super.onPause();
    }

    @Override
    protected String[] getArguments() {
        List<String> args = new ArrayList<>();
        args.add("--game_data_root=" + gameRoot);
        args.add("--user_data_root=" + userRoot);
        args.add("--cache_root=" + cacheRoot);
        // The launcher's graphics options win over nfsmw.toml.
        args.addAll(GameOptions.arguments(this));
        if (getSharedPreferences("nfsmw_controls", MODE_PRIVATE).getBoolean("stretch", true)) {
            args.add("--present_letterbox=false");
            args.add("--present_safe_area_x=100");
            args.add("--present_safe_area_y=100");
        }
        return args.toArray(new String[0]);
    }

    // ---- Physical gamepads ---------------------------------------------------------------------------------

    private static boolean isGamepad(InputDevice device) {
        if (device == null || device.isVirtual()) {
            return false;
        }
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD ||
                (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private void refreshGamepads() {
        boolean any = false;
        for (int id : inputManager.getInputDeviceIds()) {
            any |= isGamepad(inputManager.getInputDevice(id));
        }
        if (any) {
            controls.onGamepadActive();
        } else {
            controls.onGamepadGone();
        }
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
        refreshGamepads();
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        refreshGamepads();
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
        refreshGamepads();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (isGamepad(event.getDevice())) {
            controls.onGamepadActive();
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (isGamepad(event.getDevice())) {
            controls.onGamepadActive();
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // ---- Tilt steering -------------------------------------------------------------------------------------

    private void updateTiltListener() {
        if (sensorManager == null || gravity == null) {
            return;
        }
        sensorManager.unregisterListener(this);
        if (controls.tiltSteering) {
            sensorManager.registerListener(this, gravity, SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    public void onSensorChanged(SensorEvent event) {
        // The "up" of the world in screen coordinates; turning the phone like a wheel rotates it.
        float gx = event.values[0], gy = event.values[1];
        int rotation = getWindowManager().getDefaultDisplay().getRotation();
        float ux, uy;
        if (rotation == Surface.ROTATION_270) {
            ux = -gx;
            uy = -gy;
        } else {
            ux = gx;
            uy = gy;
        }
        double degrees = Math.toDegrees(Math.atan2(uy, ux));
        controls.setTilt((float) (degrees / 30.0));
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) {
    }

    // ---- Layout editor -------------------------------------------------------------------------------------

    @Override
    public void onEditModeChanged(boolean editing) {
        toolbar.setVisibility(editing ? View.VISIBLE : View.GONE);
    }

    private LinearLayout buildToolbar() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setGravity(Gravity.CENTER_HORIZONTAL);
        bar.setPadding(dp(10), dp(6), dp(10), dp(8));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xE6121A22);
        bg.setCornerRadius(dp(14));
        bg.setStroke(dp(1), 0x55FFFFFF);
        bar.setBackground(bg);

        TextView hint = new TextView(this);
        hint.setText(getString(R.string.controls_hint));
        hint.setTextColor(0xCCFFFFFF);
        hint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        hint.setGravity(Gravity.CENTER);
        bar.addView(hint);

        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.addView(toolButton(getString(R.string.tb_shrink), v -> controls.resizeSelected(1f / 1.12f)));
        row.addView(toolButton(getString(R.string.tb_grow), v -> controls.resizeSelected(1.12f)));
        row.addView(toolButton(getString(R.string.tb_visibility), v -> controls.toggleSelectedVisible()));
        row.addView(toolButton(getString(R.string.tb_settings), v -> showSettings()));
        row.addView(toolButton(getString(R.string.tb_reset), v -> confirmReset()));
        Button done = toolButton(getString(R.string.tb_done), v -> controls.setEditing(false));
        ((GradientDrawable) done.getBackground()).setColor(0xFFE07B00);
        row.addView(done);
        bar.addView(row);
        return bar;
    }

    private Button toolButton(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setMinHeight(0);
        b.setMinimumHeight(0);
        b.setPadding(dp(12), dp(6), dp(12), dp(6));
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF2A3440);
        bg.setCornerRadius(dp(10));
        b.setBackground(bg);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMargins(dp(4), dp(6), dp(4), 0);
        b.setLayoutParams(lp);
        b.setOnClickListener(onClick);
        return b;
    }

    private void confirmReset() {
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.reset_controls_title))
                .setMessage(getString(R.string.reset_controls_msg))
                .setPositiveButton(getString(R.string.action_reset), (d, w) -> controls.resetLayout())
                .setNegativeButton(getString(R.string.action_cancel), null)
                .show();
    }

    private void showSettings() {
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(8), dp(20), dp(8));

        panel.addView(toggle(getString(R.string.toggle_stretch), controls.stretch, on -> {
            controls.stretch = on;
            applyStretch(on);
        }));
        panel.addView(toggle(getString(R.string.toggle_haptics), controls.haptics, on -> controls.haptics = on));
        panel.addView(toggle(getString(R.string.toggle_hide_pad), controls.hideWithGamepad,
                on -> controls.hideWithGamepad = on));
        panel.addView(toggle(getString(R.string.toggle_tilt), controls.tiltSteering, on -> {
            controls.tiltSteering = on;
            updateTiltListener();
        }));
        panel.addView(slider(getString(R.string.slider_tilt), 50, 250,
                Math.round(controls.tiltSensitivity * 100), v -> controls.tiltSensitivity = v / 100f));
        panel.addView(slider(getString(R.string.slider_opacity), 15, 100,
                Math.round(controls.opacity * 100), v -> {
                    controls.opacity = v / 100f;
                    controls.invalidate();
                }));

        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle(getString(R.string.controls_settings_title))
                .setView(panel)
                .setPositiveButton(getString(R.string.action_close), (d, w) -> controls.save())
                .setOnDismissListener(d -> controls.save())
                .show();
    }

    private interface OnToggle {
        void set(boolean on);
    }

    private interface OnValue {
        void set(int value);
    }

    private View toggle(String label, boolean value, OnToggle onToggle) {
        Switch s = new Switch(this);
        s.setText(label);
        s.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        s.setChecked(value);
        s.setPadding(0, dp(8), 0, dp(8));
        s.setOnCheckedChangeListener((v, on) -> onToggle.set(on));
        return s;
    }

    private View slider(String label, int min, int max, int value, OnValue onValue) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(0, dp(8), 0, dp(4));
        TextView title = new TextView(this);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        box.addView(title);
        SeekBar bar = new SeekBar(this);
        bar.setMax(max - min);
        bar.setProgress(Math.max(0, Math.min(max - min, value - min)));
        title.setText(getString(R.string.percent_value, label, value));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                title.setText(getString(R.string.percent_value, label, progress + min));
                onValue.set(progress + min);
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        box.addView(bar);
        return box;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    // ---- Settings file -------------------------------------------------------------------------------------

    // Copies the Android settings from the APK. A copy the player edited is kept: it is only replaced while
    // it is still the one a previous APK installed.
    private void installSettings() {
        File target = new File(userRoot, SETTINGS);
        SharedPreferences prefs = getSharedPreferences("nfsmw", MODE_PRIVATE);
        try {
            byte[] bundled = readAll(getAssets().open(SETTINGS));
            String bundledHash = sha256(bundled);
            String installedHash = prefs.getString("settings_sha256", "");
            if (target.isFile()) {
                String currentHash = sha256(readAll(new FileInputStream(target)));
                if (currentHash.equals(bundledHash) || !currentHash.equals(installedHash)) {
                    return;
                }
            }
            try (OutputStream out = new FileOutputStream(target)) {
                out.write(bundled);
            }
            prefs.edit().putString("settings_sha256", bundledHash).apply();
        } catch (IOException | NoSuchAlgorithmException e) {
            Log.w(TAG, "Could not install " + SETTINGS, e);
        }
    }

    private static byte[] readAll(InputStream in) throws IOException {
        try (InputStream stream = in; ByteArrayOutputStream out = new ByteArrayOutputStream()) {
            byte[] buffer = new byte[16384];
            for (int n; (n = stream.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static String sha256(byte[] data) throws NoSuchAlgorithmException {
        StringBuilder hex = new StringBuilder();
        for (byte b : MessageDigest.getInstance("SHA-256").digest(data)) {
            hex.append(String.format("%02x", b));
        }
        return hex.toString();
    }
}
