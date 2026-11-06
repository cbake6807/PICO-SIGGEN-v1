package com.gatedpulse.app;

// GatedPulse remote (a Stan's Legacy bench instrument, stanslegacy.com):
// finds the Pico on the LAN and hosts its own web UI.
//
// The UI is deliberately NOT re-implemented natively. The firmware's page
// (webui.h) is the single source of truth for every control -- ranges,
// commands, the poll loop -- and a second copy here would drift the first time
// a knob is added. What the app adds is what a browser tab can't do well:
//
//   - finds the board by mDNS (Android browsers don't resolve .local)
//   - remembers it, and reconnects on launch
//   - keeps the screen on while connected, rotates without reloading
//
// Framework APIs only, no AndroidX: the APK stays tiny and builds with the
// bare SDK tools (see build.sh), no Gradle.

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.PorterDuff;
import android.graphics.drawable.GradientDrawable;
import android.graphics.Insets;
import android.graphics.Typeface;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.Uri;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.provider.Settings;
import android.text.InputType;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import android.graphics.Outline;
import android.view.ViewOutlineProvider;
import android.util.TypedValue;
import android.window.OnBackInvokedCallback;
import android.window.OnBackInvokedDispatcher;

import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.Inet6Address;
import java.net.InetAddress;
import java.net.URL;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends Activity {

    // Android 17 (API 37). A string, not Manifest.permission.*, so the meaning
    // is obvious and the guard below is the only thing that knows the level.
    private static final String PERM_LAN = "android.permission.ACCESS_LOCAL_NETWORK";
    private static final int    API_LAN  = 37;
    private static final int    REQ_LAN  = 1;

    private static final String SERVICE     = "_http._tcp.";
    private static final String NAME_PREFIX = "gatedpulse";    // MDNS_NAME in the sketch
    // The page checks for this to hide its "get the app" link.
    private static final String UA_TAG      = " GatedPulseApp/1";
    private static final int    PROBE_MS    = 2500;
    private static final int    RETRY_MS    = 4000;
    // Heartbeat while connected: GET /state every BEAT_MS; two misses in a row
    // and the connection is called lost (one miss is just Wi-Fi being Wi-Fi).
    // While lost it checks faster, so recovery shows up within a second or two.
    private static final int    BEAT_MS      = 3000;
    private static final int    BEAT_LOST_MS = 1500;
    private static final int    LOST_AFTER   = 2;
    private static final int    LIVE = 0xFF5CC088;      // standby green
    private static final int    MAX_RECENT  = 6;

    // Stan's Legacy dark palette (stanslegacy.com): vellum, sheet, rule, ink,
    // graphite, cobalt, amber. Same tokens as the board's web UI.
    private static final int BG = 0xFF14181E, CARD = 0xFF1C222A, LINE = 0xFF2A323C,
                             FG = 0xFFE1E5EA, DIM = 0xFF98A3B1, GO = 0xFF2F8F5B,
                             ACC = 0xFF2E62D9, ACC2 = 0xFF7FA8F2, AMBER = 0xFFD7A343;
    private static final String SITE = "https://stanslegacy.com";

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newCachedThreadPool();
    private SharedPreferences prefs;

    private WebView web;
    private View connectPane;
    private TextView barTitle, barName, status;
    private View splash;
    private String fwMajor;          // "2" from the board's fw "2.0.0", once known
    private ProgressBar spinner;
    private LinearLayout foundList, recentList;
    private EditText addr;
    private Button reloadBtn, permBtn;

    private NsdManager nsd;
    private NsdManager.DiscoveryListener discovery;
    private final ArrayDeque<NsdServiceInfo> toResolve = new ArrayDeque<>();
    private boolean resolving;
    private final Set<String> seen = new HashSet<>();

    private String host;          // connected host[:port], null on the picker
    private boolean autoConnect;  // false once the user asked to pick by hand
    // The one host a background attempt may connect to: the board the user
    // last chose. null only before anything has ever been chosen, when the
    // first board discovery finds is taken. Discovery never switches boards
    // by itself -- a dropped connection retries this host, it does not fall
    // over to whatever else answers mDNS.
    private String target;

    // ---- connection health, while connected ----
    private TextView banner, lostText;
    private View lostPane;
    private int misses;
    private boolean lost, frameFailed, paused;
    private long lostAt;
    private String lastBuild;
    // mDNS service name per address, so a board that comes back on a NEW
    // address (DHCP) can be recognised by name and followed there.
    private final Map<String, String> names = new HashMap<>();
    private ConnectivityManager.NetworkCallback netCb;

    private final Runnable beat = this::heartbeat;

    private final Runnable retry = new Runnable() {
        @Override public void run() {
            if (host != null || !autoConnect || target == null) return;
            probe(target, true);
            ui.postDelayed(this, RETRY_MS);
        }
    };

    // ------------------------------------------------------------------ setup

    @Override
    @SuppressWarnings("deprecation")       // bar colours: only on API < 35
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        prefs = getSharedPreferences("gp", MODE_PRIVATE);
        nsd = (NsdManager) getSystemService(NSD_SERVICE);

        if (Build.VERSION.SDK_INT < 35) {        // 35+ is edge-to-edge regardless
            getWindow().setStatusBarColor(BG);
            getWindow().setNavigationBarColor(BG);
        }
        FrameLayout frame = new FrameLayout(this);
        frame.addView(buildUi());
        // Built always, shown on a cold start (not a rotation), and on demand:
        // tap the avatar to bring it up, tap anywhere to put it away.
        splash = buildSplash();
        splash.setOnClickListener(v -> hideSplash());
        frame.addView(splash);
        if (saved == null) ui.postDelayed(splashOff, 2200);
        else splash.setVisibility(View.GONE);
        setContentView(frame);

        if (Build.VERSION.SDK_INT >= 33) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                    OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                    (OnBackInvokedCallback) this::goBack);
        }

        // Wi-Fi back (or the phone switched networks): check the board now
        // instead of waiting out the heartbeat interval.
        try {
            ConnectivityManager cm = (ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE);
            netCb = new ConnectivityManager.NetworkCallback() {
                @Override public void onAvailable(Network n) {
                    ui.post(() -> { if (host != null) heartbeat(); });
                }
            };
            cm.registerDefaultNetworkCallback(netCb);
        } catch (Exception e) { netCb = null; }

        if (needsLanPermission()) {
            requestPermissions(new String[]{PERM_LAN}, REQ_LAN);
        } else {
            start(true);
        }
    }

    private boolean needsLanPermission() {
        return Build.VERSION.SDK_INT >= API_LAN
                && checkSelfPermission(PERM_LAN) != PackageManager.PERMISSION_GRANTED;
    }

    @Override
    public void onRequestPermissionsResult(int code, String[] perms, int[] res) {
        if (code != REQ_LAN) return;
        start(true);
    }

    private void start(boolean auto) {
        autoConnect = auto;
        String last = prefs.getString("host", "");
        target = last.isEmpty() ? null : last;
        showPicker(needsLanPermission()
                ? "Local network access is off, so this app can't reach the generator. "
                  + "Allow \"Nearby devices\" in the app's permissions."
                : auto && target != null
                ? "Connecting to " + target + "… (or pick another below)"
                : auto ? "Looking for GatedPulse on this network…"
                : "Pick a generator, or enter its address.");
        if (needsLanPermission()) return;
        ui.removeCallbacks(retry);
        if (auto && target != null) ui.post(retry);
        startDiscovery();
    }

    // An explicit tap: this board and no other, from now on.
    private void pick(String h) {
        target = h;
        autoConnect = true;          // keep retrying it if it doesn't answer yet
        probe(h, false);
    }

    // ------------------------------------------------------------- connecting

    // GET /state on a worker thread. auto=true is a background attempt (saved
    // host, discovery hit) and stays quiet on failure; a tap reports it.
    private void probe(final String h, final boolean auto) {
        if (!auto) setStatus("Trying " + h + "…", true);
        io.execute(() -> {
            final boolean ok = looksLikeGatedPulse(h);
            ui.post(() -> {
                if (host != null) return;                  // already connected
                if (ok) {
                    // Background hits only take the chosen board (or, before
                    // anything was chosen, the first one found).
                    if (auto && !(autoConnect && (target == null || target.equals(h)))) return;
                    connect(h);
                }
                else if (!auto) setStatus("No GatedPulse answered at " + h
                        + ". Same Wi-Fi network as the generator?", false);
            });
        });
    }

    private static boolean looksLikeGatedPulse(String h) {
        return fetchState(h) != null;
    }

    // The board's /state JSON, or null if it did not answer like a GatedPulse.
    private static String fetchState(String h) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL("http://" + h + "/state").openConnection();
            c.setConnectTimeout(PROBE_MS);
            c.setReadTimeout(PROBE_MS);
            c.setUseCaches(false);
            if (c.getResponseCode() != 200) return null;
            InputStream in = c.getInputStream();
            byte[] buf = new byte[8192];
            int n = 0, r;
            while (n < buf.length && (r = in.read(buf, n, buf.length - n)) > 0) n += r;
            String s = new String(buf, 0, n, "UTF-8").trim();
            return s.startsWith("{") ? s : null;
        } catch (Exception e) {
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    // Firmware build stamp from /state, or null on firmware older than 2.0.
    private static String buildOf(String json) { return fieldOf(json, "build"); }

    // A top-level string field out of /state -- no JSON library needed for two keys.
    private static String fieldOf(String json, String key) {
        String k = "\"" + key + "\":\"";
        int i = json.indexOf(k);
        if (i < 0) return null;
        i += k.length();
        int j = json.indexOf('"', i);
        return j > i ? json.substring(i, j) : null;
    }

    private void connect(String h) {
        host = h;
        target = h;
        SharedPreferences.Editor e = prefs.edit().putString("host", h);
        if (names.containsKey(h)) e.putString("name", names.get(h));
        e.apply();
        remember(h);
        ui.removeCallbacks(retry);
        stopDiscovery();
        connectPane.setVisibility(View.GONE);
        web.setVisibility(View.VISIBLE);
        reloadBtn.setVisibility(View.VISIBLE);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        misses = 0; lost = false; frameFailed = false; lastBuild = null;
        banner.setVisibility(View.GONE);
        lostPane.setVisibility(View.GONE);
        paintTitle();
        web.loadUrl("http://" + h + "/");
        ui.removeCallbacks(beat);
        ui.postDelayed(beat, BEAT_MS);
    }

    // ------------------------------------------------------------- heartbeat
    //
    // The page has its own poll, but a page can only dim itself -- it cannot
    // tell you the board is gone when the board is what serves it, and before
    // this the app only noticed a drop if a whole page load failed, then threw
    // you back to the picker. This watches the board directly, says so in the
    // app's own chrome the moment it stops answering, keeps retrying the SAME
    // board, and puts you back where you were when it returns.
    private void heartbeat() {
        ui.removeCallbacks(beat);
        if (host == null || paused) return;
        final String h = host;
        io.execute(() -> {
            final String st = fetchState(h);
            ui.post(() -> {
                if (!h.equals(host)) return;            // switched boards meanwhile
                if (st != null) {
                    String fw = fieldOf(st, "fw");
                    String maj = fw != null && fw.indexOf('.') > 0 ? fw.substring(0, fw.indexOf('.')) : null;
                    if (maj != null && !maj.equals(fwMajor)) { fwMajor = maj; paintTitle(); }
                    beatOk(buildOf(st));
                } else beatMiss();
                ui.removeCallbacks(beat);
                if (!paused) ui.postDelayed(beat, lost ? BEAT_LOST_MS : BEAT_MS);
            });
        });
    }

    private void beatOk(String build) {
        misses = 0;
        // New firmware (an OTA, or a reflash) means a new page: reload it, or
        // the old page goes on driving a board whose commands have changed.
        boolean newFw = build != null && lastBuild != null && !build.equals(lastBuild);
        if (build != null) lastBuild = build;
        if (lost) {
            setLost(false);
            buzz(new long[]{0, 40});
        }
        if (frameFailed || newFw) {
            frameFailed = false;
            lostPane.setVisibility(View.GONE);
            web.loadUrl("http://" + host + "/");
        }
    }

    private void beatMiss() {
        misses++;
        if (!lost && misses >= LOST_AFTER) setLost(true);
        if (lost) paintBanner();
    }

    private void setLost(boolean l) {
        lost = l;
        if (l) {
            lostAt = System.currentTimeMillis();
            buzz(new long[]{0, 60, 90, 60});
            paintBanner();
            banner.setVisibility(View.VISIBLE);
            // The board may have come back on another address: listen for its
            // mDNS name while we wait (found() follows it if it turns up).
            if (!prefs.getString("name", "").isEmpty()) startDiscovery();
        } else {
            banner.setVisibility(View.GONE);
            stopDiscovery();
        }
        paintTitle();
    }

    private void paintBanner() {
        long secs = (System.currentTimeMillis() - lostAt) / 1000;
        String t = "Connection to " + host + " lost \u2014 reconnecting"
                + (secs >= 3 ? " (" + secs + " s)" : "\u2026");
        banner.setText(t);
        lostText.setText(t);
    }

    // "● GatedPulse · host", the dot green when the board answers, amber when not.
    private void paintTitle() {
        barName.setText("Stan\u2019s Legacy \u00b7 " + productName());
        if (host == null) { barTitle.setText("not connected"); return; }
        SpannableString t = new SpannableString("\u25CF  " + host);
        t.setSpan(new ForegroundColorSpan(lost ? AMBER : LIVE), 0, 1,
                  Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        barTitle.setText(t);
    }

    @SuppressWarnings("deprecation")       // Vibrator: VibratorManager is API 31+
    private void buzz(long[] pattern) {
        try {
            Vibrator v = (Vibrator) getSystemService(VIBRATOR_SERVICE);
            if (v == null || !v.hasVibrator()) return;
            if (Build.VERSION.SDK_INT >= 26) v.vibrate(VibrationEffect.createWaveform(pattern, -1));
            else v.vibrate(pattern, -1);
        } catch (Exception ignored) {}
    }

    private void disconnect(String why, boolean auto) {
        host = null;
        ui.removeCallbacks(beat);
        lost = false; frameFailed = false; misses = 0;
        banner.setVisibility(View.GONE);
        lostPane.setVisibility(View.GONE);
        web.stopLoading();
        web.loadUrl("about:blank");
        web.clearHistory();
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        start(auto);
        if (why != null) setStatus(why, auto || discovery != null);
    }

    // ---------------------------------------------------------------- recents

    private java.util.List<String> recents() {
        java.util.List<String> l = new java.util.ArrayList<>();
        for (String s : prefs.getString("recent", "").split("\n"))
            if (!s.isEmpty() && !l.contains(s)) l.add(s);
        String last = prefs.getString("host", "");          // pre-1.3 installs
        if (!last.isEmpty() && !l.contains(last)) l.add(0, last);
        return l;
    }

    private void saveRecents(java.util.List<String> l) {
        while (l.size() > MAX_RECENT) l.remove(l.size() - 1);
        prefs.edit().putString("recent", String.join("\n", l)).apply();
    }

    private void remember(String h) {
        java.util.List<String> l = recents();
        l.remove(h);
        l.add(0, h);
        saveRecents(l);
    }

    private void forget(String h) {
        java.util.List<String> l = recents();
        l.remove(h);
        SharedPreferences.Editor e = prefs.edit();
        if (h.equals(prefs.getString("host", ""))) {
            e.putString("host", l.isEmpty() ? "" : l.get(0));
            target = l.isEmpty() ? null : l.get(0);
        }
        e.putString("recent", String.join("\n", l)).apply();
        fillRecents();
    }

    private void fillRecents() {
        while (recentList.getChildCount() > 1) recentList.removeViewAt(1);
        java.util.List<String> l = recents();
        String last = prefs.getString("host", "");
        for (final String h : l) {
            Button b = button(h.equals(last) ? h + "   · last used" : h,
                    h.equals(last) ? ACC : CARD);
            b.setOnClickListener(v -> pick(h));
            b.setOnLongClickListener(v -> { forget(h); return true; });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = dp(6);
            recentList.addView(b, lp);
        }
        if (!l.isEmpty()) {
            TextView hint = text("Long-press to forget one.", 11, DIM);
            recentList.addView(hint);
        }
        recentList.setVisibility(l.isEmpty() ? View.GONE : View.VISIBLE);
    }

    // -------------------------------------------------------------- discovery

    private void startDiscovery() {
        if (discovery != null || nsd == null) return;
        discovery = new NsdManager.DiscoveryListener() {
            @Override public void onDiscoveryStarted(String t) {}
            @Override public void onDiscoveryStopped(String t) {}
            @Override public void onStartDiscoveryFailed(String t, int err) {
                ui.post(() -> {
                    discovery = null;
                    setStatus("Network search failed (" + err + "). Enter the address below.", false);
                });
            }
            @Override public void onStopDiscoveryFailed(String t, int err) {}
            @Override public void onServiceFound(NsdServiceInfo si) {
                if (si.getServiceName().toLowerCase(Locale.ROOT).startsWith(NAME_PREFIX))
                    ui.post(() -> { toResolve.add(si); resolveNext(); });
            }
            @Override public void onServiceLost(NsdServiceInfo si) {}
        };
        try {
            nsd.discoverServices(SERVICE, NsdManager.PROTOCOL_DNS_SD, discovery);
        } catch (RuntimeException e) {
            discovery = null;
            setStatus("Network search unavailable. Enter the address below.", false);
        }
    }

    private void stopDiscovery() {
        if (discovery == null) return;
        try { nsd.stopServiceDiscovery(discovery); } catch (RuntimeException ignored) {}
        discovery = null;
        toResolve.clear();
    }

    // NsdManager allows one resolve at a time (FAILURE_ALREADY_ACTIVE
    // otherwise), so hits queue. resolveService is deprecated from API 34 in
    // favour of a callback API that does not exist below it; it still works.
    @SuppressWarnings("deprecation")
    private void resolveNext() {
        if (resolving || toResolve.isEmpty() || discovery == null) return;
        resolving = true;
        nsd.resolveService(toResolve.poll(), new NsdManager.ResolveListener() {
            @Override public void onResolveFailed(NsdServiceInfo si, int err) {
                ui.post(() -> { resolving = false; resolveNext(); });
            }
            @Override public void onServiceResolved(NsdServiceInfo si) {
                final String name = si.getServiceName();
                final String h = hostPort(si.getHost(), si.getPort());
                ui.post(() -> {
                    resolving = false;
                    if (h != null) found(name, h);
                    resolveNext();
                });
            }
        });
    }

    private static String hostPort(InetAddress a, int port) {
        if (a == null) return null;
        String ip = a.getHostAddress();
        if (a instanceof Inet6Address) {
            int pct = ip.indexOf('%');                 // URLs can't carry a scope id
            ip = "[" + (pct >= 0 ? ip.substring(0, pct) : ip) + "]";
        }
        return (port == 80 || port <= 0) ? ip : ip + ":" + port;
    }

    private void found(String name, final String h) {
        names.put(h, name);
        // Connected but lost, and the SAME board (by mDNS name) has turned up
        // on a different address: follow it there.
        if (host != null) {
            if (lost && !h.equals(host)
                    && name.equalsIgnoreCase(prefs.getString("name", ""))) {
                final String old = host;
                io.execute(() -> {
                    final boolean ok = looksLikeGatedPulse(h);
                    ui.post(() -> {
                        if (!ok || !lost || !old.equals(host)) return;
                        host = null;                    // connect() expects none
                        connect(h);
                        buzz(new long[]{0, 40});
                    });
                });
            }
            return;
        }
        if (!seen.add(h)) return;
        Button b = button(name + "   " + h, GO);
        b.setOnClickListener(v -> pick(h));
        foundList.addView(b, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        foundList.setVisibility(View.VISIBLE);
        probe(h, true);
    }

    // ------------------------------------------------------------------- UI

    @SuppressWarnings("deprecation")       // inset getters: only on API < 30
    private View buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);
        // Edge-to-edge (forced from API 35): pad for bars, cutouts and the
        // keyboard ourselves, or the page's sticky header hides under the clock.
        root.setOnApplyWindowInsetsListener((v, in) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                Insets i = in.getInsets(WindowInsets.Type.systemBars()
                        | WindowInsets.Type.displayCutout() | WindowInsets.Type.ime());
                v.setPadding(i.left, i.top, i.right, i.bottom);
                return WindowInsets.CONSUMED;
            }
            v.setPadding(in.getSystemWindowInsetLeft(), in.getSystemWindowInsetTop(),
                    in.getSystemWindowInsetRight(), in.getSystemWindowInsetBottom());
            return in.consumeSystemWindowInsets();
        });

        // -- slim bar: which board, reload, switch
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(dp(12), dp(4), dp(6), dp(4));
        bar.setBackgroundColor(CARD);
        LinearLayout.LayoutParams mlp = new LinearLayout.LayoutParams(dp(36), dp(36));
        mlp.rightMargin = dp(10);
        ImageView badge = stanBadge(36);
        badge.setOnClickListener(v -> toggleSplash());
        bar.addView(badge, mlp);
        // Two lines: who we are, then which board and whether it is answering.
        LinearLayout tl = new LinearLayout(this);
        tl.setOrientation(LinearLayout.VERTICAL);
        barName = text("Stan\u2019s Legacy \u00b7 GatedPulse Pico", 14, FG);
        barName.setTypeface(Typeface.DEFAULT_BOLD);
        barName.setMaxLines(1);
        // Shrinks to fit rather than clipping "v2" on a narrow phone.
        if (Build.VERSION.SDK_INT >= 26)
            barName.setAutoSizeTextTypeUniformWithConfiguration(10, 15, 1, TypedValue.COMPLEX_UNIT_SP);
        else barName.setEllipsize(android.text.TextUtils.TruncateAt.END);
        tl.addView(barName);
        barTitle = text("not connected", 12, DIM);
        barTitle.setTypeface(Typeface.MONOSPACE);
        barTitle.setSingleLine(true);
        tl.addView(barTitle);
        bar.addView(tl, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        reloadBtn = iconButton("\u21BB", "Reload");
        reloadBtn.setOnClickListener(v -> web.reload());
        reloadBtn.setVisibility(View.GONE);
        bar.addView(reloadBtn);
        Button dev = iconButton("\u21C4", "Switch device");
        dev.setOnClickListener(v -> disconnect("Pick a generator, or enter its address.", false));
        bar.addView(dev);
        root.addView(bar);
        View rule = new View(this);
        rule.setBackgroundColor(LINE);
        root.addView(rule, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));

        // Shown only while the connected board is not answering. In the app's
        // own chrome, not the page's, because the page is served by the very
        // board that has stopped answering.
        banner = text("", 13, 0xFF14181E);
        banner.setBackgroundColor(AMBER);
        banner.setTypeface(Typeface.DEFAULT_BOLD);
        banner.setPadding(dp(14), dp(8), dp(14), dp(8));
        banner.setVisibility(View.GONE);
        banner.setOnClickListener(v -> heartbeat());       // tap: check right now
        root.addView(banner, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        FrameLayout body = new FrameLayout(this);
        root.addView(body, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        // Debug builds only (build.sh DEBUG=1): lets chrome://inspect and
        // scripted demos drive the page. Release builds stay closed.
        if ((getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0)
            WebView.setWebContentsDebuggingEnabled(true);
        web = new WebView(this);
        web.setBackgroundColor(BG);
        web.setVisibility(View.GONE);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);           // the page remembers its tab
        s.setUserAgentString(s.getUserAgentString() + UA_TAG);
        web.setWebChromeClient(new WebChromeClient());   // confirm() for "known state" etc.
        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                if (host != null && host.equalsIgnoreCase(u.getAuthority())) return false;
                try { startActivity(new Intent(Intent.ACTION_VIEW, u)); } catch (Exception ignored) {}
                return true;                     // anything off-board opens in the browser
            }
            @Override
            public void onReceivedError(WebView v, WebResourceRequest r, WebResourceError e) {
                // The page itself did not load. Stay on this board -- cover the
                // error page and let the heartbeat bring it back -- rather than
                // dropping to the picker as if the board had been forgotten.
                if (!r.isForMainFrame() || host == null) return;
                frameFailed = true;
                if (!lost) setLost(true);
                paintBanner();
                lostPane.setVisibility(View.VISIBLE);
                heartbeat();
            }
        });
        body.addView(web, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // Covers the WebView when the PAGE itself failed to load, instead of
        // leaving Chromium's error page up. The page comes back on its own.
        LinearLayout lp = new LinearLayout(this);
        lp.setOrientation(LinearLayout.VERTICAL);
        lp.setGravity(Gravity.CENTER);
        lp.setBackgroundColor(BG);
        lp.setPadding(dp(28), dp(28), dp(28), dp(28));
        lp.addView(new ProgressBar(this));
        lostText = text("", 15, FG);
        lostText.setGravity(Gravity.CENTER);
        lostText.setPadding(0, dp(16), 0, dp(6));
        lp.addView(lostText);
        TextView hint = text("It reconnects by itself when the board answers. "
                + "Device (top right) picks another one.", 13, DIM);
        hint.setGravity(Gravity.CENTER);
        lp.addView(hint);
        lp.setVisibility(View.GONE);
        lp.setClickable(true);                 // nothing underneath takes taps
        lostPane = lp;
        body.addView(lp, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        // -- picker
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        LinearLayout p = new LinearLayout(this);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setPadding(dp(20), dp(24), dp(20), dp(24));
        scroll.addView(p);

        p.addView(brandHeader());

        TextView h1 = text("Connect to generator", 18, FG);
        h1.setTypeface(Typeface.DEFAULT_BOLD);
        h1.setPadding(0, dp(22), 0, 0);
        p.addView(h1);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(14), 0, dp(14));
        spinner = new ProgressBar(this, null, android.R.attr.progressBarStyleSmall);
        row.addView(spinner);
        status = text("", 14, DIM);
        status.setPadding(dp(10), 0, 0, 0);
        row.addView(status, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        p.addView(row);

        permBtn = button("Open app permissions", ACC);
        permBtn.setOnClickListener(v -> startActivity(new Intent(
                Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", getPackageName(), null))));
        p.addView(permBtn);

        recentList = new LinearLayout(this);
        recentList.setOrientation(LinearLayout.VERTICAL);
        recentList.setVisibility(View.GONE);
        recentList.addView(label("Recent"));
        p.addView(recentList);

        foundList = new LinearLayout(this);
        foundList.setOrientation(LinearLayout.VERTICAL);
        foundList.setVisibility(View.GONE);
        foundList.addView(label("Found on this network"));
        p.addView(foundList);

        p.addView(label("Or enter its address"));
        addr = new EditText(this);
        addr.setHint("192.168.1.9");
        addr.setSingleLine(true);
        addr.setTextColor(FG);
        addr.setHintTextColor(DIM);
        addr.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        addr.setImeOptions(EditorInfo.IME_ACTION_GO);
        addr.setOnEditorActionListener((v, id, ev) -> { manual(); return true; });
        p.addView(addr);
        Button go = button("Connect", ACC);
        go.setOnClickListener(v -> manual());
        p.addView(go);

        TextView note = text("The phone must be on the same Wi-Fi as the generator. "
                + "Its address is printed on the serial console at boot, and shown "
                + "by the P command.", 12, DIM);
        note.setPadding(0, dp(18), 0, 0);
        p.addView(note);

        TextView site = text("stanslegacy.com", 13, ACC2);
        site.setTypeface(Typeface.MONOSPACE);
        site.setGravity(Gravity.CENTER);
        site.setPadding(0, dp(28), 0, dp(8));
        site.setOnClickListener(v -> openSite());
        p.addView(site, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        connectPane = scroll;
        body.addView(scroll, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        return root;
    }

    // Stan's line art is white on transparent, which at avatar size is a few
    // thin grey lines on a dark bar. Dark lines on a light disc read at any
    // size, and the disc gives the mark a shape of its own.
    private ImageView stanBadge(int sizeDp) {
        // The site's own chalk drawing, background and all: thick white lines
        // on the blueprint sheet. Cutting Stan out of it thinned the lines to
        // nothing at avatar size, and inverting the cut-out only made the
        // jagged edges darker.
        ImageView m = new ImageView(this);
        m.setImageResource(R.drawable.stan_chalk);
        m.setScaleType(ImageView.ScaleType.CENTER_CROP);
        // The ring is a FOREGROUND, drawn over the picture: as a background
        // behind a padded square image, the square's corners covered it at
        // the diagonals and it showed as four separate arcs.
        int ring = Math.max(dp(2), dp(sizeDp) / 36);
        GradientDrawable disc = new GradientDrawable();
        disc.setShape(GradientDrawable.OVAL);
        disc.setColor(0xFF131822);
        m.setBackground(disc);
        GradientDrawable edge = new GradientDrawable();
        edge.setShape(GradientDrawable.OVAL);
        edge.setColor(0x00000000);
        edge.setStroke(ring, AMBER);
        if (Build.VERSION.SDK_INT >= 23) m.setForeground(edge);
        m.setOutlineProvider(new ViewOutlineProvider() {
            @Override public void getOutline(View v, Outline o) {
                o.setOval(0, 0, v.getWidth(), v.getHeight());
            }
        });
        m.setClipToOutline(true);
        m.setContentDescription("Stan's Legacy");
        return m;
    }

    private String productName() {
        return "GatedPulse Pico" + (fwMajor != null ? " v" + fwMajor : "");
    }

    // Cold-start splash: the mark and the name for two seconds while the
    // connection is made underneath, then a short fade.
    private View buildSplash() {
        LinearLayout sp = new LinearLayout(this);
        sp.setOrientation(LinearLayout.VERTICAL);
        sp.setGravity(Gravity.CENTER);
        sp.setBackgroundColor(BG);
        sp.setClickable(true);
        sp.addView(stanBadge(148), new LinearLayout.LayoutParams(dp(148), dp(148)));
        sp.setPadding(dp(24), dp(24), dp(24), dp(24));
        TextView k = text("THE STANLEY MEYER ARCHIVE", 11, DIM);
        k.setTypeface(Typeface.MONOSPACE);
        k.setLetterSpacing(0.14f);
        k.setPadding(0, dp(26), 0, dp(6));
        centre(sp, k);
        TextView n = text("Stan\u2019s Legacy", 34, FG);
        n.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        centre(sp, n);
        View rule = new View(this);
        rule.setBackgroundColor(AMBER);
        LinearLayout.LayoutParams rl = new LinearLayout.LayoutParams(dp(56), dp(3));
        rl.topMargin = dp(12); rl.bottomMargin = dp(12);
        sp.addView(rule, rl);
        TextView p = text("GatedPulse Pico v2", 18, DIM);
        p.setTypeface(Typeface.create(Typeface.SERIF, Typeface.ITALIC));
        centre(sp, p);
        // The credit, as on the stream name card: name, then role.
        TextView by = text("Chris Bake", 16, FG);
        by.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        by.setPadding(0, dp(36), 0, 0);
        centre(sp, by);
        TextView role = text("FOUNDER", 11, DIM);
        role.setTypeface(Typeface.MONOSPACE);
        role.setLetterSpacing(0.18f);
        role.setPadding(0, dp(3), 0, 0);
        centre(sp, role);
        TextView u = text("stanslegacy.com", 13, ACC2);
        u.setTypeface(Typeface.MONOSPACE);
        u.setPadding(0, dp(28), 0, 0);
        centre(sp, u);
        return sp;
    }

    // Each line full width with centred text: a TextView added to a vertical
    // LinearLayout is MATCH_PARENT by default, so the layout's own gravity
    // never moves it -- its text has to be centred itself.
    private void centre(LinearLayout parent, TextView t) {
        t.setGravity(Gravity.CENTER_HORIZONTAL);
        parent.addView(t, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    // One instance: a fresh this::hideSplash would never match the posted one.
    private final Runnable splashOff = this::hideSplash;

    private void toggleSplash() {
        if (splash.getVisibility() == View.VISIBLE) { hideSplash(); return; }
        ui.removeCallbacks(splashOff);   // a tap during the startup hold wins
        splash.animate().cancel();
        splash.setAlpha(0f);
        splash.setVisibility(View.VISIBLE);
        splash.animate().alpha(1f).setDuration(250).start();
    }

    private void hideSplash() {
        final View v = splash;
        if (v == null || v.getVisibility() != View.VISIBLE) return;
        // GONE only once the fade has finished. Hiding it as the fade started
        // is what made it cut out instead of fading.
        v.animate().alpha(0f).setDuration(450)
                .withEndAction(() -> v.setVisibility(View.GONE)).start();
    }

    // The stanslegacy.com masthead, as on the site's share card: Stan in chalk
    // line-art, a mono kicker, the name, an amber rule, an italic serif line.
    private View brandHeader() {
        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        ImageView stan = new ImageView(this);
        stan.setImageResource(R.drawable.stan_full);
        stan.setAdjustViewBounds(true);
        stan.setContentDescription("Stan");
        row.addView(stan, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(124)));

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setPadding(dp(16), 0, 0, 0);
        TextView kicker = text("THE STANLEY MEYER ARCHIVE", 10, DIM);
        kicker.setTypeface(Typeface.MONOSPACE);
        kicker.setLetterSpacing(0.12f);
        col.addView(kicker);
        TextView name = text("Stan’s Legacy", 26, FG);
        name.setTypeface(Typeface.create("sans-serif", Typeface.BOLD));
        name.setPadding(0, dp(4), 0, dp(8));
        col.addView(name);
        View rule = new View(this);
        rule.setBackgroundColor(AMBER);
        col.addView(rule, new LinearLayout.LayoutParams(dp(44), dp(3)));
        TextView sub = text("GatedPulse — bench pulse generator", 14, DIM);
        sub.setTypeface(Typeface.create(Typeface.SERIF, Typeface.ITALIC));
        sub.setPadding(0, dp(8), 0, 0);
        col.addView(sub);
        row.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        row.setOnClickListener(v -> openSite());
        return row;
    }

    private void openSite() {
        try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(SITE))); }
        catch (Exception ignored) {}
    }

    private void manual() {
        String h = addr.getText().toString().trim()
                .replaceFirst("(?i)^https?://", "").replaceFirst("/.*$", "");
        if (h.isEmpty()) return;
        pick(h);
    }

    private void showPicker(String msg) {
        connectPane.setVisibility(View.VISIBLE);
        web.setVisibility(View.GONE);
        reloadBtn.setVisibility(View.GONE);
        paintTitle();
        seen.clear();
        while (foundList.getChildCount() > 1) foundList.removeViewAt(1);
        foundList.setVisibility(View.GONE);
        fillRecents();
        String last = prefs.getString("host", "");
        if (addr.getText().length() == 0) addr.setText(last);
        permBtn.setVisibility(needsLanPermission() ? View.VISIBLE : View.GONE);
        setStatus(msg, !needsLanPermission());
    }

    private void setStatus(String msg, boolean busy) {
        status.setText(msg);
        spinner.setVisibility(busy ? View.VISIBLE : View.GONE);
    }

    private TextView text(String s, int sp, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        return t;
    }

    private TextView label(String s) {
        TextView t = text(s.toUpperCase(Locale.ROOT), 11, DIM);
        t.setLetterSpacing(0.08f);
        t.setPadding(0, dp(18), 0, dp(6));
        return t;
    }

    private Button button(String s, int color) {
        Button b = new Button(this);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(color);
        return b;
    }

    // A glyph instead of a word, to leave the title its room. Long-press says
    // what it does; the label is also the screen-reader description.
    private Button iconButton(String glyph, final String label) {
        Button b = smallButton(glyph);
        b.setTextSize(22);
        b.setContentDescription(label);
        b.setPadding(dp(10), 0, dp(10), 0);
        b.setOnLongClickListener(v -> {
            Toast.makeText(this, label, Toast.LENGTH_SHORT).show();
            return true;
        });
        return b;
    }

    private Button smallButton(String s) {
        Button b = new Button(this, null, android.R.attr.borderlessButtonStyle);
        b.setText(s);
        b.setAllCaps(false);
        b.setTextColor(ACC2);
        b.setTextSize(13);
        b.setMinWidth(0);
        b.setMinimumWidth(0);
        return b;
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    // ------------------------------------------------------------- lifecycle

    private void goBack() {
        if (host != null && web.canGoBack()) web.goBack();
        else finish();
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent ev) {      // API < 33 back
        if (code == KeyEvent.KEYCODE_BACK && Build.VERSION.SDK_INT < 33) { goBack(); return true; }
        return super.onKeyDown(code, ev);
    }

    @Override
    protected void onResume() {
        super.onResume();
        paused = false;
        if (host != null) heartbeat();    // back from the background: check at once
        web.onResume();
        web.resumeTimers();               // the page's 2 s /state poll
        // Coming back from the permissions screen with access now granted.
        if (host == null && discovery == null && !needsLanPermission()) start(autoConnect);
    }

    @Override
    protected void onPause() {
        paused = true;
        ui.removeCallbacks(beat);
        web.onPause();
        web.pauseTimers();                // no point polling the board from a pocket
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        ui.removeCallbacks(beat);
        if (netCb != null) {
            try {
                ((ConnectivityManager) getSystemService(CONNECTIVITY_SERVICE))
                        .unregisterNetworkCallback(netCb);
            } catch (Exception ignored) {}
        }
        ui.removeCallbacks(retry);
        stopDiscovery();
        io.shutdownNow();
        web.destroy();
        super.onDestroy();
    }
}
