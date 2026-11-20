package com.phrecomp.ph;

import android.app.Activity;
import android.app.AlertDialog;
import android.widget.EditText;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Online Mod Catalog browser & 1-click installer for the Android launcher.
 * Uses only Android built-in HTTP/JSON/SHA-256 APIs (zero external dependencies).
 */
final class PhModCatalogUi {
    static final String DEFAULT_CATALOG_URL =
            "https://raw.githubusercontent.com/chouette-cc/ph-recomp-mods/main/catalog.json";
    private static final int MAX_DOWNLOAD_BYTES = 32 * 1024 * 1024;
    private static final ExecutorService EXEC = Executors.newSingleThreadExecutor();

    static final class Entry {
        String id;
        String name;
        String version;
        String kind;
        String author;
        String description;
        String downloadUrl;
        String sha256;
    }

    static String resolveCatalogUrl(File modsDir) {
        final File f = new File(modsDir, "catalog.url");
        if (f.isFile()) {
            try (BufferedReader r = new BufferedReader(new FileReader(f))) {
                String line;
                while ((line = r.readLine()) != null) {
                    line = line.trim();
                    if (!line.isEmpty() && !line.startsWith("#")) return line;
                }
            } catch (IOException ignored) {
            }
        }
        return DEFAULT_CATALOG_URL;
    }

    static void saveCatalogUrl(File modsDir, String url) {
        //noinspection ResultOfMethodCallIgnored
        modsDir.mkdirs();
        final File f = new File(modsDir, "catalog.url");
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write((url.trim() + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    static void openBrowser(Activity activity, File modsDir, Runnable onModsChanged) {
        final String url = resolveCatalogUrl(modsDir);
        final AlertDialog loading = new AlertDialog.Builder(activity)
                .setTitle("Online Mods")
                .setMessage("Fetching catalog…\n" + url)
                .setNegativeButton("Cancel", null)
                .show();

        EXEC.execute(() -> {
            try {
                final byte[] raw = httpGet(url, 2 * 1024 * 1024);
                final JSONObject root = new JSONObject(new String(raw, StandardCharsets.UTF_8));
                final String catName = root.optString("name", "Community Mods");
                final JSONArray arr = root.optJSONArray("mods");
                final List<Entry> entries = new ArrayList<>();
                if (arr != null) {
                    for (int i = 0; i < arr.length(); i++) {
                        final JSONObject o = arr.optJSONObject(i);
                        if (o == null) continue;
                        final Entry e = new Entry();
                        e.id = sanitizeId(o.optString("id", ""));
                        e.name = o.optString("name", e.id);
                        e.version = o.optString("version", "1.0.0");
                        e.kind = o.optString("kind", "lua").toLowerCase(Locale.US);
                        e.author = o.optString("author", "");
                        e.description = o.optString("description", "");
                        e.downloadUrl = o.optString("download_url", "").trim();
                        e.sha256 = o.optString("sha256", "").trim().toLowerCase(Locale.US);
                        if (!e.id.isEmpty() && !e.downloadUrl.isEmpty()) entries.add(e);
                    }
                }
                activity.runOnUiThread(() -> {
                    if (loading.isShowing()) loading.dismiss();
                    showCatalogList(activity, modsDir, catName, entries, onModsChanged);
                });
            } catch (Exception ex) {
                final String msg = ex.getMessage() != null ? ex.getMessage() : ex.getClass().getSimpleName();
                activity.runOnUiThread(() -> {
                    if (loading.isShowing()) loading.dismiss();
                    new AlertDialog.Builder(activity)
                            .setTitle("Online Mods")
                            .setMessage("Could not load catalog:\n" + msg + "\n\nURL: " + url)
                            .setPositiveButton("Retry", (d, w) -> openBrowser(activity, modsDir, onModsChanged))
                            .setNeutralButton("Catalog URL…", (d, w) -> editCatalogUrl(activity, modsDir, onModsChanged))
                            .setNegativeButton("Close", null)
                            .show();
                });
            }
        });
    }

    private static void showCatalogList(Activity activity, File modsDir, String title,
                                        List<Entry> entries, Runnable onModsChanged) {
        if (entries.isEmpty()) {
            new AlertDialog.Builder(activity)
                    .setTitle(title)
                    .setMessage("No mods listed in this catalog yet.")
                    .setNeutralButton("Catalog URL…", (d, w) -> editCatalogUrl(activity, modsDir, onModsChanged))
                    .setNegativeButton("Close", null)
                    .show();
            return;
        }
        final Map<String, String> installed = scanInstalledVersions(modsDir);
        final String[] rows = new String[entries.size()];
        for (int i = 0; i < entries.size(); i++) {
            final Entry e = entries.get(i);
            final String tag = "native".equals(e.kind) ? "[C++ Native]"
                    : "asset".equals(e.kind) ? "[Asset]" : "[Lua]";
            final String curVer = installed.get(e.id);
            final String status = curVer == null ? "[Install]"
                    : compareVersions(e.version, curVer) > 0 ? "[Update " + curVer + " -> " + e.version + "]"
                    : "[Installed]";
            rows[i] = status + " " + e.name + " v" + e.version + "\n" + tag
                    + (e.author.isEmpty() ? "" : " by " + e.author);
        }
        new AlertDialog.Builder(activity)
                .setTitle(title)
                .setItems(rows, (d, which) -> showModDetails(activity, modsDir, entries.get(which),
                        installed.get(entries.get(which).id), onModsChanged))
                .setNeutralButton("Catalog URL…", (d, w) -> editCatalogUrl(activity, modsDir, onModsChanged))
                .setNegativeButton("Close", null)
                .show();
    }

    private static void showModDetails(Activity activity, File modsDir, Entry e,
                                       String installedVer, Runnable onModsChanged) {
        final StringBuilder msg = new StringBuilder();
        if (!e.description.isEmpty()) msg.append(e.description).append("\n\n");
        msg.append("Type: ").append(e.kind.toUpperCase(Locale.US));
        if (!e.author.isEmpty()) msg.append("\nAuthor: ").append(e.author);
        msg.append("\nVersion: ").append(e.version);
        if (installedVer != null) msg.append(" (installed: ").append(installedVer).append(")");
        if ("native".equals(e.kind)) {
            msg.append("\n\nWARNING: Native C++ mods run unsandboxed code. Only install from trusted authors.");
        }
        final String action = installedVer == null ? "Install"
                : compareVersions(e.version, installedVer) > 0 ? "Update" : "Reinstall";
        new AlertDialog.Builder(activity)
                .setTitle(e.name)
                .setMessage(msg.toString())
                .setPositiveButton(action, (d, w) -> installEntry(activity, modsDir, e, onModsChanged))
                .setNegativeButton("Back", (d, w) -> openBrowser(activity, modsDir, onModsChanged))
                .show();
    }

    private static void installEntry(Activity activity, File modsDir, Entry e, Runnable onModsChanged) {
        final AlertDialog prog = new AlertDialog.Builder(activity)
                .setTitle("Installing…")
                .setMessage("Downloading " + e.name + " v" + e.version + "…")
                .setCancelable(false)
                .show();

        EXEC.execute(() -> {
            try {
                final byte[] data = httpGet(e.downloadUrl, MAX_DOWNLOAD_BYTES);
                if (!e.sha256.isEmpty()) {
                    final String got = sha256Hex(data);
                    if (!got.equals(e.sha256)) {
                        throw new IOException("SHA-256 verification failed");
                    }
                }
                extractZipIntoMods(activity, modsDir, e.id, data);
                activity.runOnUiThread(() -> {
                    if (prog.isShowing()) prog.dismiss();
                    Toast.makeText(activity, "Installed " + e.name + " v" + e.version, Toast.LENGTH_LONG).show();
                    if (onModsChanged != null) onModsChanged.run();
                });
            } catch (Exception ex) {
                final String err = ex.getMessage() != null ? ex.getMessage() : ex.getClass().getSimpleName();
                activity.runOnUiThread(() -> {
                    if (prog.isShowing()) prog.dismiss();
                    new AlertDialog.Builder(activity)
                            .setTitle("Install failed")
                            .setMessage(err)
                            .setPositiveButton("OK", null)
                            .show();
                });
            }
        });
    }

    private static void editCatalogUrl(Activity activity, File modsDir, Runnable onModsChanged) {
        final EditText input = new EditText(activity);
        input.setSingleLine(true);
        input.setText(resolveCatalogUrl(modsDir));
        new AlertDialog.Builder(activity)
                .setTitle("Mod Catalog URL (HTTPS)")
                .setView(input)
                .setPositiveButton("Save & Reload", (d, w) -> {
                    final String v = input.getText().toString().trim();
                    saveCatalogUrl(modsDir, v.isEmpty() ? DEFAULT_CATALOG_URL : v);
                    openBrowser(activity, modsDir, onModsChanged);
                })
                .setNeutralButton("Reset Default", (d, w) -> {
                    saveCatalogUrl(modsDir, DEFAULT_CATALOG_URL);
                    openBrowser(activity, modsDir, onModsChanged);
                })
                .setNegativeButton("Cancel", null)
                .show();
    }

    private static byte[] httpGet(String urlStr, int maxBytes) throws IOException {
        if (!urlStr.startsWith("https://")) throw new IOException("Only https:// URLs are allowed");
        String current = urlStr;
        for (int hop = 0; hop < 6; hop++) {
            final HttpURLConnection conn = (HttpURLConnection) new URL(current).openConnection();
            conn.setInstanceFollowRedirects(false);
            conn.setConnectTimeout(8000);
            conn.setReadTimeout(20000);
            conn.setRequestProperty("User-Agent", "PhantomHourglassRecomp-Android/1.0");
            try {
                final int code = conn.getResponseCode();
                if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                    final String loc = conn.getHeaderField("Location");
                    if (loc == null || !loc.startsWith("https://")) throw new IOException("Invalid HTTPS redirect");
                    current = loc;
                    continue;
                }
                if (code < 200 || code >= 300) throw new IOException("HTTP " + code);
                try (InputStream in = new BufferedInputStream(conn.getInputStream())) {
                    final ByteArrayOutputStream out = new ByteArrayOutputStream();
                    final byte[] buf = new byte[16384];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        if (out.size() + n > maxBytes) throw new IOException("Download exceeds size limit");
                        out.write(buf, 0, n);
                    }
                    return out.toByteArray();
                }
            } finally {
                conn.disconnect();
            }
        }
        throw new IOException("Too many redirects");
    }

    private static void extractZipIntoMods(Activity activity, File modsDir, String modId, byte[] zipBytes) throws IOException {
        final File tmpRoot = new File(activity.getCacheDir(), "mod-dl-" + System.currentTimeMillis());
        if (!tmpRoot.mkdirs()) throw new IOException("Cannot create temp folder");
        try {
            final String canonRoot = tmpRoot.getCanonicalPath() + File.separator;
            int files = 0;
            long total = 0;
            try (ZipInputStream zis = new ZipInputStream(new java.io.ByteArrayInputStream(zipBytes))) {
                ZipEntry ze;
                final byte[] buf = new byte[16384];
                while ((ze = zis.getNextEntry()) != null) {
                    if (++files > 4096) throw new IOException("Too many files in archive");
                    final String name = ze.getName().replace('\\', '/');
                    if (name.startsWith("/") || name.contains("..")) throw new IOException("Unsafe path in archive");
                    final File dst = new File(tmpRoot, name);
                    if (!dst.getCanonicalPath().startsWith(canonRoot)) throw new IOException("Unsafe path in archive");
                    if (ze.isDirectory()) {
                        //noinspection ResultOfMethodCallIgnored
                        dst.mkdirs();
                    } else {
                        final File parent = dst.getParentFile();
                        if (parent != null) //noinspection ResultOfMethodCallIgnored
                            parent.mkdirs();
                        try (FileOutputStream fos = new FileOutputStream(dst)) {
                            int n;
                            while ((n = zis.read(buf)) > 0) {
                                total += n;
                                if (total > 64L * 1024 * 1024) throw new IOException("Extracted mod exceeds 64 MB");
                                fos.write(buf, 0, n);
                            }
                        }
                    }
                    zis.closeEntry();
                }
            }
            File modRoot = new File(tmpRoot, "mod.json").isFile() ? tmpRoot : null;
            if (modRoot == null) {
                final File[] kids = tmpRoot.listFiles();
                if (kids != null) {
                    for (File k : kids) {
                        if (k.isDirectory() && new File(k, "mod.json").isFile()) {
                            modRoot = k;
                            break;
                        }
                    }
                }
            }
            if (modRoot == null) throw new IOException("Archive does not contain mod.json");
            //noinspection ResultOfMethodCallIgnored
            modsDir.mkdirs();
            final File dest = new File(modsDir, sanitizeId(modId));
            deleteRecursive(dest);
            if (!modRoot.renameTo(dest)) {
                copyRecursive(modRoot, dest);
            }
        } finally {
            deleteRecursive(tmpRoot);
        }
    }

    private static Map<String, String> scanInstalledVersions(File modsDir) {
        final Map<String, String> out = new HashMap<>();
        final File[] kids = modsDir.listFiles();
        if (kids == null) return out;
        for (File k : kids) {
            if (!k.isDirectory()) continue;
            final File mf = new File(k, "mod.json");
            if (!mf.isFile()) continue;
            try (FileInputStream in = new FileInputStream(mf)) {
                final byte[] buf = new byte[(int) Math.min(mf.length(), 131072)];
                final int n = in.read(buf);
                if (n <= 0) continue;
                final JSONObject o = new JSONObject(new String(buf, 0, n, StandardCharsets.UTF_8));
                final String id = sanitizeId(o.optString("id", k.getName()));
                final String ver = o.optString("version", "1.0.0");
                if (!id.isEmpty()) out.put(id, ver);
            } catch (Exception ignored) {
            }
        }
        return out;
    }

    private static String sha256Hex(byte[] data) throws IOException {
        try {
            final byte[] d = MessageDigest.getInstance("SHA-256").digest(data);
            final StringBuilder sb = new StringBuilder(64);
            for (byte b : d) sb.append(String.format(Locale.US, "%02x", b & 0xFF));
            return sb.toString();
        } catch (Exception e) {
            throw new IOException("SHA-256 unavailable");
        }
    }

    private static int compareVersions(String a, String b) {
        final String[] pa = a.replaceFirst("^[vV]", "").split("[^0-9]+");
        final String[] pb = b.replaceFirst("^[vV]", "").split("[^0-9]+");
        final int n = Math.max(pa.length, pb.length);
        for (int i = 0; i < n; i++) {
            final int va = (i < pa.length && !pa[i].isEmpty()) ? Integer.parseInt(pa[i]) : 0;
            final int vb = (i < pb.length && !pb[i].isEmpty()) ? Integer.parseInt(pb[i]) : 0;
            if (va != vb) return Integer.compare(va, vb);
        }
        return 0;
    }

    private static String sanitizeId(String raw) {
        final StringBuilder sb = new StringBuilder();
        for (int i = 0; i < raw.length(); i++) {
            final char c = Character.toLowerCase(raw.charAt(i));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') sb.append(c);
            else if (c == ' ') sb.append('-');
        }
        return sb.toString();
    }

    private static void deleteRecursive(File f) {
        if (f.isDirectory()) {
            final File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteRecursive(k);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    private static void copyRecursive(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            //noinspection ResultOfMethodCallIgnored
            dst.mkdirs();
            final File[] kids = src.listFiles();
            if (kids != null) for (File k : kids) copyRecursive(k, new File(dst, k.getName()));
        } else {
            try (FileInputStream in = new FileInputStream(src); FileOutputStream out = new FileOutputStream(dst)) {
                final byte[] buf = new byte[16384];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
        }
    }
}
