package com.selinuxbridge.app;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.Button;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

/**
 * Launcher activity. Its real job is just to start BridgeService as a
 * foreground service so Android grants it a normal app process (real UID,
 * real SELinux domain) -- that's what unlocks MediaCodec/DMA-BUF access
 * that the PRoot/Termux shell is denied.
 *
 * The UI below exists only so a human glancing at the screen (this app has
 * to stay open/visible to stay alive) can immediately tell it's alive and
 * roughly what it's doing, instead of staring at a blank white TextView.
 * Built entirely with programmatic views/drawables (GradientDrawable,
 * Canvas-drawn icons) rather than XML layout/drawable resources, so it
 * doesn't touch build.sh's single-flat-file aapt2 resource link step.
 */
public class MainActivity extends Activity {

    private static final int BG_TOP = Color.parseColor("#0F172A");    // slate-900
    private static final int BG_BOTTOM = Color.parseColor("#1E293B"); // slate-800
    private static final int ACCENT = Color.parseColor("#22D3EE");    // cyan-400
    private static final int ACCENT_2 = Color.parseColor("#A78BFA"); // violet-400
    private static final int OK_GREEN = Color.parseColor("#4ADE80"); // green-400
    private static final int CARD_BG = Color.parseColor("#1E293B");
    private static final int CARD_STROKE = Color.parseColor("#334155");
    private static final int TEXT_MAIN = Color.parseColor("#F1F5F9");
    private static final int TEXT_DIM = Color.parseColor("#94A3B8");

    private TextView powerStatus;
    private TextView powerAction;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackground(diagonalGradient(BG_TOP, BG_BOTTOM));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(24);
        root.setPadding(pad, dp(48), pad, pad);

        root.addView(buildHeader());
        root.addView(spacer(24));
        View staleBanner = buildStaleBanner();
        if (staleBanner != null) {
            root.addView(staleBanner);
            root.addView(spacer(14));
        }
        root.addView(buildStatusCard());
        root.addView(spacer(14));
        root.addView(buildPowerCard());
        root.addView(spacer(28));
        root.addView(sectionTitle("How it works"));
        root.addView(spacer(12));
        root.addView(new FlowDiagramView(this));
        root.addView(spacer(28));
        root.addView(sectionTitle("What this gives you"));
        root.addView(spacer(12));
        root.addView(featureRow("\u26A1", ACCENT,
                "Real hardware codec",
                "Reaches Qualcomm c2.qti.* MediaCodec encode/decode directly \u2014 not a software fallback."));
        root.addView(spacer(14));
        root.addView(featureRow("\uD83D\uDD12", ACCENT_2,
                "Loopback only",
                "Listens on 127.0.0.1:7878 only \u2014 never reachable from the network."));
        root.addView(spacer(14));
        root.addView(featureRow("\uD83D\uDCCB", OK_GREEN,
                "No adb required",
                "Ask it anything with `bridge_client info` or `tools/bridge-status`."));
        root.addView(spacer(28));
        root.addView(footer());

        scroll.addView(root, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);

        Intent svc = new Intent(this, BridgeService.class);
        startForegroundService(svc);
    }

    // ---- sections -----------------------------------------------------

    private View buildHeader() {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        row.addView(new ChipLogoView(this), new LinearLayout.LayoutParams(dp(56), dp(56)));

        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding(dp(16), 0, 0, 0);

        TextView title = new TextView(this);
        title.setText("SELinux Hardware Bridge");
        title.setTextColor(TEXT_MAIN);
        title.setTypeface(title.getTypeface(), android.graphics.Typeface.BOLD);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22);

        TextView subtitle = new TextView(this);
        subtitle.setText("Gives your Termux/PRoot shell hardware access it can't reach alone");
        subtitle.setTextColor(TEXT_DIM);
        subtitle.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);

        titles.addView(title);
        titles.addView(subtitle);
        row.addView(titles);
        return row;
    }

    private View buildStatusCard() {
        LinearLayout card = card();
        card.setOrientation(LinearLayout.VERTICAL);

        LinearLayout statusLine = new LinearLayout(this);
        statusLine.setOrientation(LinearLayout.HORIZONTAL);
        statusLine.setGravity(Gravity.CENTER_VERTICAL);
        statusLine.addView(new PulseDotView(this), new LinearLayout.LayoutParams(dp(12), dp(12)));

        TextView status = new TextView(this);
        status.setText("Listening on 127.0.0.1:7878");
        status.setTextColor(TEXT_MAIN);
        status.setTypeface(status.getTypeface(), android.graphics.Typeface.BOLD);
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        status.setPadding(dp(10), 0, 0, 0);
        statusLine.addView(status);

        TextView caption = new TextView(this);
        caption.setText("Foreground service active \u2014 keep this app open (or in recent apps) to keep the bridge alive.");
        caption.setTextColor(TEXT_DIM);
        caption.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        caption.setPadding(0, dp(8), 0, 0);

        card.addView(statusLine);
        card.addView(caption);
        return card;
    }

    /**
     * Doze exemption (gap #4, mitigation 2).
     *
     * A foreground service is not immune to Doze on every OEM ROM, and this
     * one is by design invisible for long stretches -- the Debian side may
     * not talk to it for hours. Offering the exemption up front, with its
     * current state visible, means the user does not have to discover
     * Settings > Battery > Unrestricted after the first mysterious stall.
     *
     * The direct REQUEST_IGNORE_BATTERY_OPTIMIZATIONS dialog is tried first
     * and falls back to the plain settings list, because some ROMs refuse
     * the direct intent.
     */
    private View buildPowerCard() {
        LinearLayout cardView = card();
        cardView.setOrientation(LinearLayout.VERTICAL);

        powerStatus = new TextView(this);
        powerStatus.setTextColor(TEXT_MAIN);
        powerStatus.setTypeface(powerStatus.getTypeface(), android.graphics.Typeface.BOLD);
        powerStatus.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);

        TextView caption = new TextView(this);
        caption.setText("Survives reboots and in-place updates automatically. A force-stop "
                + "(swipe away from recents) still needs a manual launch \u2014 Android "
                + "allows no way around that without root.");
        caption.setTextColor(TEXT_DIM);
        caption.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        caption.setPadding(0, dp(8), 0, 0);

        powerAction = new TextView(this);
        powerAction.setTextColor(ACCENT);
        powerAction.setTypeface(powerAction.getTypeface(), android.graphics.Typeface.BOLD);
        powerAction.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        powerAction.setPadding(0, dp(14), 0, 0);
        powerAction.setOnClickListener(v -> requestBatteryExemption());

        cardView.addView(powerStatus);
        cardView.addView(caption);
        cardView.addView(powerAction);
        refreshPowerCard();
        return cardView;
    }

    /**
     * An in-place update that fails to restart the service leaves this
     * process running the previous build's code, which is otherwise
     * completely silent -- the install succeeds, the bridge answers, and
     * only the protocol version gives it away. Say so at the top of the
     * screen, where it cannot be missed.
     */
    private View buildStaleBanner() {
        String warning = BridgeService.staleProcessWarning(this);
        if (warning == null) return null;

        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int p = dp(16);
        box.setPadding(p, p, p, p);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(0xFF8A3A12);
        bg.setCornerRadius(dp(14));
        box.setBackground(bg);

        TextView tv = new TextView(this);
        tv.setText("\u26A0  Restart needed\n\nA newer build is installed, but this process is "
                 + "still running the previous one. Installing an APK does not reliably "
                 + "restart a running service, and nothing else reloads the code.");
        tv.setTextColor(0xFFFFFFFF);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        box.addView(tv);

        Button restart = new Button(this);
        restart.setText("Restart now");
        restart.setAllCaps(false);
        restart.setTextColor(0xFF8A3A12);
        GradientDrawable btnBg = new GradientDrawable();
        btnBg.setColor(0xFFFFFFFF);
        btnBg.setCornerRadius(dp(10));
        restart.setBackground(btnBg);
        restart.setOnClickListener(v -> {
            // Killing the process mid-transcode would abort somebody's stream
            // with no warning, so refuse while any codec is in use.
            int busy = BridgeService.activeSessions();
            if (busy > 0) {
                Toast.makeText(this, busy + " transcode(s) still running \u2014 "
                        + "try again when the bridge is idle.", Toast.LENGTH_LONG).show();
                return;
            }
            // The service is START_STICKY, so Android restarts it after the
            // process dies, and that restart loads the new code. This is the
            // only thing short of a force-stop that actually reloads classes.
            Toast.makeText(this, "Restarting\u2026 reopen the app in a moment.",
                    Toast.LENGTH_LONG).show();
            new android.os.Handler(getMainLooper()).postDelayed(
                    () -> System.exit(0), 600);
        });
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        blp.topMargin = dp(14);
        box.addView(restart, blp);
        return box;
    }

    private boolean isBatteryExempt() {
        PowerManager pm = getSystemService(PowerManager.class);
        return pm != null && pm.isIgnoringBatteryOptimizations(getPackageName());
    }

    private void refreshPowerCard() {
        if (powerStatus == null || powerAction == null) return;
        if (isBatteryExempt()) {
            powerStatus.setText("\u2713  Exempt from battery optimisation");
            powerAction.setText("Review in system settings \u203A");
        } else {
            powerStatus.setText("\u26A0  Battery optimisation is active");
            powerAction.setText("Allow the bridge to keep running \u203A");
        }
    }

    private void requestBatteryExemption() {
        if (!isBatteryExempt()) {
            try {
                startActivity(new Intent(
                        Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                        Uri.parse("package:" + getPackageName())));
                return;
            } catch (Exception ignored) {
                // Fall through to the settings list below.
            }
        }
        try {
            startActivity(new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS));
        } catch (Exception ignored) {
            // No such settings screen on this ROM; nothing further to offer.
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // The exemption is granted in a system dialog, so the only chance to
        // notice the change is on the way back into this activity.
        refreshPowerCard();
    }

    private View featureRow(String glyph, int badgeColor, String title, String desc) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);

        TextView badge = new TextView(this);
        badge.setText(glyph);
        badge.setGravity(Gravity.CENTER);
        badge.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        GradientDrawable circle = new GradientDrawable();
        circle.setShape(GradientDrawable.OVAL);
        circle.setColor(withAlpha(badgeColor, 40));
        badge.setBackground(circle);
        LinearLayout.LayoutParams badgeLp = new LinearLayout.LayoutParams(dp(40), dp(40));
        row.addView(badge, badgeLp);

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(dp(14), 0, 0, 0);

        TextView t = new TextView(this);
        t.setText(title);
        t.setTextColor(TEXT_MAIN);
        t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);

        TextView d = new TextView(this);
        d.setText(desc);
        d.setTextColor(TEXT_DIM);
        d.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        d.setPadding(0, dp(2), 0, 0);

        texts.addView(t);
        texts.addView(d);
        row.addView(texts);
        return row;
    }

    private View sectionTitle(String text) {
        TextView tv = new TextView(this);
        tv.setText(text.toUpperCase(java.util.Locale.US));
        tv.setTextColor(TEXT_DIM);
        tv.setTypeface(tv.getTypeface(), android.graphics.Typeface.BOLD);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tv.setLetterSpacing(0.08f);
        return tv;
    }

    private View footer() {
        TextView tv = new TextView(this);
        tv.setText("github.com/skyv04/gpucodec");
        tv.setTextColor(TEXT_DIM);
        tv.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        tv.setGravity(Gravity.CENTER);
        return tv;
    }

    // ---- small helpers --------------------------------------------------

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(this);
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(CARD_BG);
        bg.setCornerRadius(dp(16));
        bg.setStroke(dp(1), CARD_STROKE);
        card.setBackground(bg);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        return card;
    }

    private View spacer(int heightDp) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(heightDp)));
        return v;
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private static int withAlpha(int color, int alpha) {
        return Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color));
    }

    private static GradientDrawable diagonalGradient(int top, int bottom) {
        GradientDrawable gd = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[]{top, bottom});
        return gd;
    }

    /** Small stylised "chip" glyph: a rounded square with pins, drawn on Canvas. */
    private static class ChipLogoView extends View {
        private final Paint body = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint pin = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint core = new Paint(Paint.ANTI_ALIAS_FLAG);

        ChipLogoView(Activity ctx) {
            super(ctx);
            pin.setColor(ACCENT);
            core.setColor(Color.parseColor("#0B1220"));
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            body.setShader(new LinearGradient(0, 0, w, h, ACCENT, ACCENT_2, Shader.TileMode.CLAMP));
            RectF outer = new RectF(w * 0.18f, h * 0.18f, w * 0.82f, h * 0.82f);
            c.drawRoundRect(outer, w * 0.12f, w * 0.12f, body);
            RectF inner = new RectF(w * 0.30f, h * 0.30f, w * 0.70f, h * 0.70f);
            c.drawRoundRect(inner, w * 0.05f, w * 0.05f, core);
            // pins on all four sides
            float pinLen = w * 0.10f, pinW = Math.max(2f, w * 0.045f);
            for (int i = -1; i <= 1; i += 2) {
                float y = h / 2f + i * h * 0.2f;
                c.drawRect(0, y - pinW / 2, w * 0.18f, y + pinW / 2, pin);
                c.drawRect(w * 0.82f, y - pinW / 2, w, y + pinW / 2, pin);
                float x = w / 2f + i * w * 0.2f;
                c.drawRect(x - pinW / 2, 0, x + pinW / 2, h * 0.18f, pin);
                c.drawRect(x - pinW / 2, h * 0.82f, x + pinW / 2, h, pin);
            }
        }
    }

    /** Small green dot that gently pulses, signalling "alive" at a glance. */
    private static class PulseDotView extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private float phase = 0f;

        PulseDotView(Activity ctx) {
            super(ctx);
            paint.setColor(OK_GREEN);
            postInvalidateDelayed(0);
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float pulse = 0.75f + 0.25f * (float) Math.sin(phase);
            paint.setAlpha((int) (255 * pulse));
            c.drawCircle(w / 2f, h / 2f, Math.min(w, h) / 2f, paint);
            phase += 0.15f;
            postInvalidateDelayed(80);
        }
    }

    /** Draws the "PRoot shell -> Bridge :7878 -> Hardware codec" data flow. */
    private class FlowDiagramView extends View {
        private final Paint boxPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint strokePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint subTextPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint arrowPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String[] labels = {"Debian /\nPRoot shell", "Bridge\n:7878", "Hardware\ncodec"};
        private final String[] subLabels = {"bridge_client", "MediaCodec", "c2.qti.*"};

        FlowDiagramView(Activity ctx) {
            super(ctx);
            boxPaint.setColor(withAlpha(ACCENT, 30));
            strokePaint.setColor(ACCENT);
            strokePaint.setStyle(Paint.Style.STROKE);
            strokePaint.setStrokeWidth(dp(2));
            textPaint.setColor(TEXT_MAIN);
            textPaint.setTextAlign(Paint.Align.CENTER);
            textPaint.setTextSize(spToPx(13));
            textPaint.setFakeBoldText(true);
            subTextPaint.setColor(TEXT_DIM);
            subTextPaint.setTextAlign(Paint.Align.CENTER);
            subTextPaint.setTextSize(spToPx(11));
            arrowPaint.setColor(ACCENT_2);
            arrowPaint.setStrokeWidth(dp(3));
            setLayoutParams(new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, dp(120)));
        }

        private float spToPx(float sp) {
            return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, sp, getResources().getDisplayMetrics());
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            float boxW = w * 0.27f, boxH = h * 0.62f;
            float gap = (w - boxW * 3) / 2f;
            float top = h * 0.12f;

            for (int i = 0; i < 3; i++) {
                float left = i * (boxW + gap);
                RectF r = new RectF(left, top, left + boxW, top + boxH);
                c.drawRoundRect(r, dp(10), dp(10), boxPaint);
                c.drawRoundRect(r, dp(10), dp(10), strokePaint);

                float cx = r.centerX();
                float lineH = textPaint.getTextSize();
                String[] lines = labels[i].split("\n");
                float ty = r.centerY() - (lines.length * lineH) / 2f + lineH * 0.6f - dp(6);
                for (String line : lines) {
                    c.drawText(line, cx, ty, textPaint);
                    ty += lineH;
                }
                c.drawText(subLabels[i], cx, r.bottom - dp(8), subTextPaint);

                if (i < 2) {
                    float ay = r.centerY();
                    float ax1 = r.right + dp(4);
                    float ax2 = ax1 + gap - dp(8);
                    c.drawLine(ax1, ay, ax2, ay, arrowPaint);
                    Path arrow = new Path();
                    arrow.moveTo(ax2, ay);
                    arrow.lineTo(ax2 - dp(8), ay - dp(6));
                    arrow.lineTo(ax2 - dp(8), ay + dp(6));
                    arrow.close();
                    arrowPaint.setStyle(Paint.Style.FILL);
                    c.drawPath(arrow, arrowPaint);
                    arrowPaint.setStyle(Paint.Style.STROKE);
                }
            }
        }
    }
}
