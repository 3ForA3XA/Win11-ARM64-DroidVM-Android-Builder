package com.dvmw.builder;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebSettings;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;

public class MainActivity extends Activity {
    private WebView web;
    private Handler ui;
    private Process buildProc;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        ui = new Handler(Looper.getMainLooper());
        web = new WebView(this);
        setContentView(web);
        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setAllowFileAccess(true);
        web.addJavascriptInterface(new Bridge(), "Dvmw");
        web.loadUrl("file:///android_asset/index.html");
    }

    private void pushLog(String line) {
        String json = line.replace("\\", "\\\\").replace("'", "\\'").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
        ui.post(() -> web.evaluateJavascript(
            "javascript:window.__log && window.__log('" + json + "')", null));
    }

    private void pushEvent(String name, String value) {
        String json = value.replace("\\", "\\\\").replace("'", "\\'").replace("\"", "\\\"").replace("\n", "\\n").replace("\r", "");
        ui.post(() -> web.evaluateJavascript(
            "javascript:window.__event && window.__event('" + name + "','" + json + "')", null));
    }

    class Bridge {
        @JavascriptInterface
        public String checkEnv() {
            // returns JSON: {root:bool, tools:bool, nativeDir:path}
            boolean root = false;
            try {
                Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", "id -u"});
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line = r.readLine();
                p.waitFor();
                root = line != null && line.trim().equals("0");
            } catch (Exception e) { root = false; }
            String ndir = getApplicationInfo().nativeLibraryDir;
            boolean tools = new File(ndir, "lib_busybox.so").canExecute();
            return "{\"root\":" + root + ",\"tools\":" + tools + ",\"nativeDir\":\"" + ndir + "\"}";
        }

        @JavascriptInterface
        public String scanIso() {
            // find candidate iso/zip on shared storage via root find
            StringBuilder sb = new StringBuilder("[");
            try {
                Process p = Runtime.getRuntime().exec(new String[]{"su", "-c",
                    "find /storage/emulated/0 -maxdepth 4 -type f \\( -iname '*.iso' -o -iname '*.zip' \\) -size +10M 2>/dev/null | head -40"});
                BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line; boolean first = true;
                while ((line = r.readLine()) != null) {
                    if (!first) sb.append(",");
                    sb.append("\"").append(line.replace("\"", "\\\"")).append("\"");
                    first = false;
                }
                p.waitFor();
            } catch (Exception e) { /* ignore */ }
            return sb.append("]").toString();
        }

        @JavascriptInterface
        public boolean start(String cfgJson) {
            // cfgJson: simple key=value lines encoded in one string ('|k=v|...')
            try {
                File work = new File(getFilesDir(), "work");
                File out = new File(work, "files");
                out.mkdirs();
                // parse cfg: lines "KEY=VALUE"
                String[] lines = cfgJson.split("\n");
                String iso = "", drv = "", user = "USER", pass = "USER";
                String diskMb = "40960", espMb = "512", idx = "", outPath =
                    "/storage/emulated/0/DroidVM/Windows.raw";
                for (String l : lines) {
                    l = l.trim(); if (l.isEmpty()) continue;
                    String[] kv = l.split("=", 2);
                    if (kv.length < 2) continue;
                    switch (kv[0]) {
                        case "ISO_PATH": iso = kv[1]; break;
                        case "DRIVERS_ZIP": drv = kv[1]; break;
                        case "USERNAME": user = kv[1].isEmpty() ? "USER" : kv[1]; break;
                        case "PASSWORD": pass = kv[1].isEmpty() ? "USER" : kv[1]; break;
                        case "DISK_MB": diskMb = kv[1]; break;
                        case "ESP_MB": espMb = kv[1]; break;
                        case "IMG_INDEX": idx = kv[1]; break;
                        case "OUT_IMG": outPath = kv[1]; break;
                    }
                }
                // stage assets
                copyAsset("engine.sh", new File(getFilesDir(), "engine.sh"), false, null, null);
                copyAsset("unattend.xml", new File(out, "unattend.xml"), true, user, pass);
                copyAsset("SetupComplete.cmd", new File(out, "SetupComplete.cmd"), false, null, null);

                // config.env
                File cfgF = new File(getFilesDir(), "config.env");
                OutputStreamWriter w = new OutputStreamWriter(new FileOutputStream(cfgF), StandardCharsets.UTF_8);
                String ndir = getApplicationInfo().nativeLibraryDir;
                w.write("BIN=" + ndir + "\n");
                w.write("WORK_DIR=" + getFilesDir() + "/work/files\n");
                w.write("OUT_IMG=" + outPath + "\n");
                w.write("ISO_PATH=" + iso + "\n");
                if (!drv.isEmpty()) w.write("DRIVERS_ZIP=" + drv + "\n");
                w.write("DISK_MB=" + diskMb + "\n");
                w.write("ESP_MB=" + espMb + "\n");
                if (!idx.isEmpty()) w.write("IMG_INDEX=" + idx + "\n");
                w.write("USERNAME=" + user + "\n");
                w.write("PASSWORD=" + pass + "\n");
                w.write("ASSET_DIR=" + getFilesDir() + "/files\n");
                w.close();

                // ensure output dir exists & writable via root
                execRoot("mkdir -p '" + new File(outPath).getParent() + "'");

                String cmd = "sh '" + getFilesDir() + "/engine.sh' '" + cfgF.getAbsolutePath() + "' 2>&1";
                final Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
                buildProc = p;
                new Thread(() -> {
                    try {
                        BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
                        String line;
                        while ((line = r.readLine()) != null) pushLog(line);
                        int code = p.waitFor();
                        pushEvent("done", String.valueOf(code));
                    } catch (Exception e) {
                        pushEvent("done", "-1");
                    }
                }).start();
                return true;
            } catch (Exception e) {
                pushLog("[apk] start failed: " + e);
                return false;
            }
        }

        @JavascriptInterface
        public void cancel() {
            if (buildProc != null) buildProc.destroy();
        }
    }

    private void execRoot(String cmd) throws Exception {
        Process p = Runtime.getRuntime().exec(new String[]{"su", "-c", cmd});
        p.waitFor();
    }

    private void copyAsset(String name, File dst, boolean subst, String user, String pass) throws Exception {
        InputStream in = getAssets().open(name);
        BufferedReader r = new BufferedReader(new InputStreamReader(in, StandardCharsets.UTF_8));
        StringBuilder sb = new StringBuilder();
        String line;
        while ((line = r.readLine()) != null) {
            if (subst) {
                line = line.replace("%USERNAME%", user == null ? "USER" : user)
                           .replace("%PASSWORD%", pass == null ? "USER" : pass);
            }
            sb.append(line).append("\n");
        }
        r.close();
        OutputStreamWriter w = new OutputStreamWriter(new FileOutputStream(dst), StandardCharsets.UTF_8);
        w.write(sb.toString());
        w.close();
        dst.setExecutable(true, false);
    }
}
