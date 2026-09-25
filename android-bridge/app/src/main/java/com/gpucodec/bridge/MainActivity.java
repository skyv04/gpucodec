package com.gpucodec.bridge;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.TextView;

/**
 * Minimal launcher activity. Its only job is to start BridgeService as a
 * foreground service so Android grants it a normal app process (real UID,
 * real SELinux domain), which is what gives it working MediaCodec/DMA-BUF
 * access that the PRoot/Termux shell is denied.
 */
public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        TextView tv = new TextView(this);
        tv.setText("GPUCodec Bridge running.\nListening on 127.0.0.1:7878\nLeave this app open.");
        tv.setTextSize(18);
        tv.setPadding(40, 80, 40, 40);
        setContentView(tv);

        Intent svc = new Intent(this, BridgeService.class);
        startForegroundService(svc);
    }
}
