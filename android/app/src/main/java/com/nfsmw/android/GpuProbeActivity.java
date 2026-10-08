package com.nfsmw.android;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Process;
import android.widget.TextView;

/** Fresh process for each driver test: loader hooks cannot be safely switched in a running process. */
public final class GpuProbeActivity extends Activity {
    private boolean finished;
    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        TextView status = new TextView(this);
        status.setText("Comprobando el driver Vulkan…");
        status.setTextColor(0xFFFFFFFF); status.setTextSize(18);
        status.setGravity(android.view.Gravity.CENTER); status.setBackgroundColor(0xFF0B0F14);
        setContentView(status);
        Handler main = new Handler(Looper.getMainLooper());
        main.postDelayed(() -> {
            if (!finished) {
                finishReport("{\"probeError\":\"La prueba del driver excedió 20 segundos\",\"driverLoadFailed\":true}");
            }
        }, 20000);
        new Thread(() -> {
            String result;
            try { result = Diagnostics.probeSelectedDriver(this); }
            catch (Exception | LinkageError error) {
                result = "{\"probeError\":\"No se pudo cargar la biblioteca de diagnóstico\",\"driverLoadFailed\":true}";
            }
            String report = result;
            main.post(() -> finishReport(report));
        }, "VulkanDriverProbe").start();
    }
    private void finishReport(String report) {
        if (finished) return;
        finished = true;
        setResult(RESULT_OK, new Intent().putExtra("gpu_report", report));
        finish();
    }
    @Override protected void onDestroy() {
        super.onDestroy();
        // Vulkan handles and injected namespace hooks are owned by this disposable process.
        Process.killProcess(Process.myPid());
    }
}
