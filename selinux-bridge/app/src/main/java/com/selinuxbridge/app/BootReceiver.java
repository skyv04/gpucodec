package com.selinuxbridge.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

/**
 * Brings the bridge back without anyone having to open the app.
 *
 * The bridge is a foreground service, not a Linux daemon: it survives
 * backgrounding but not a reboot, a force-stop, or an in-place update. Two
 * of those three are recoverable automatically, and this receiver is how.
 *
 *   BOOT_COMPLETED / QUICKBOOT_POWERON
 *       After a reboot the Debian side would otherwise find nothing
 *       listening on 127.0.0.1:7878 until the user remembered to tap the
 *       launcher icon -- which, for a bridge that is invisible in normal
 *       use, is exactly the sort of thing nobody remembers.
 *
 *   MY_PACKAGE_REPLACED
 *       Installing a new build stops the old service. Restarting here means
 *       `./build.sh` + install tap leaves a working bridge behind rather
 *       than a silently dead one.
 *
 * A force-stop (swipe-away from recents, or Settings > Force stop) still
 * requires a manual restart: Android deliberately puts a force-stopped
 * package into a state where none of its receivers fire again until the
 * user launches it. That is the irreducible part of gap #4.
 */
public class BootReceiver extends BroadcastReceiver {
    private static final String TAG = "SELinuxBridge";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent != null ? intent.getAction() : null;
        Log.i(TAG, "BootReceiver: " + action);
        try {
            Intent svc = new Intent(context, BridgeService.class);
            context.startForegroundService(svc);
        } catch (Exception e) {
            // startForegroundService throws if the system considers the app
            // background-restricted. Nothing useful to do about it here, and
            // throwing out of a receiver would show an ANR-style crash for a
            // service the user may not even be trying to use right now.
            Log.e(TAG, "BootReceiver: could not start the bridge service", e);
        }
    }
}
