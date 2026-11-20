package com.phrecomp.ph;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.MessageDigest;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The start screen: bring in your own Phantom Hourglass ROM (and, if you like, a save from another emulator), then
 * play. The ROM and the save are copied into the app's own storage (files/rom/azee.nds, files/saves/azee.sav); the
 * originals are only read. A save is never overwritten without a dated backup next to it.
 */
public final class LauncherActivity extends Activity {
    private static final int PICK_ROM = 1, PICK_SAVE = 2, EXPORT_SAVE = 3, PICK_MOD_ZIP = 4;
    // The dumps the game can be recompiled for (SHA-1): USA and Europe, clean or D-pad patch.
    private static final String[] KNOWN = {
            "4c8f52dd719918bbcd46e73a8bae8628139c1b85",
            "7e0d26d143992f1eee182b24f1abb9e7f038eacf",
            "02be55db55cf254bd064d2b3eb368b92a5b4156d",
            "8e3bbda6056c47f06fecf1a06d9f144a19283d88",
    };
    private static final int SAVE_SIZE = 512 * 1024;  // the cartridge's 4 Mbit flash

    private File rom() { return new File(getFilesDir(), "rom/azee.nds"); }

    private File save() { return new File(getFilesDir(), "saves/azee.sav"); }

    private File modsDir() { return new File(getFilesDir(), "mods"); }

    /** The resume state the game writes when it quits or goes to the background (frontend main.cpp). */
    private File resume(String ext) { return new File(getFilesDir(), "states/resume." + ext); }

    /** Intent extra from the game (GameActivity.restartWithResume): continue it again at once, in a fresh process. */
    static final String EXTRA_RELAUNCH = "relaunch";

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    /** Continue, once the old game process has exited (SDL's native side starts once per process), or after 4 s. */
    private void relaunchWhenGameGone(long until) {
        boolean alive = false;
        final android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
        final java.util.List<android.app.ActivityManager.RunningAppProcessInfo> procs = am.getRunningAppProcesses();
        if (procs != null)
            for (android.app.ActivityManager.RunningAppProcessInfo p : procs)
                if (p.processName.equals(getPackageName() + ":game")) alive = true;
        if (alive && System.currentTimeMillis() < until) {
            getWindow().getDecorView().postDelayed(() -> relaunchWhenGameGone(until), 100);
            return;
        }
        startActivity(new Intent(this, GameActivity.class).putExtra(GameActivity.EXTRA_CONTINUE, true));
    }

    // ---- per game ----
    private static final String TITLE = "The Legend of Zelda\nPhantom Hourglass";
    private static final String SUBTITLE = "Nintendo DS · native Android port";
    private static final String ROM_HELP = "Choose your own dump of Phantom Hourglass (USA or Europe, clean or D-pad patch). "
            + "It must match the region this APK was built for. It is copied into the app's storage; nothing is downloaded. "
            + "A save from melonDS (.sav) can be imported afterwards under Save file.";
    private static final String SAVE_NAME = "PhantomHourglass.sav";
    private static final boolean GPU_DEFAULT = true;   // GameActivity: prefs.getBoolean("gpu", ...)

    private LauncherLook look;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        look = new LauncherLook(this);
        setContentView(look.root);
        refresh();
    }

    @Override
    protected void onResume() {
        super.onResume();
        look.hideSystemBars();
        refresh();
        if (getIntent() != null && getIntent().getBooleanExtra(EXTRA_RELAUNCH, false)) {
            getIntent().removeExtra(EXTRA_RELAUNCH);
            if (resume("state").isFile()) relaunchWhenGameGone(System.currentTimeMillis() + 4000);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) look.hideSystemBars();
    }

    @Override
    public boolean dispatchKeyEvent(android.view.KeyEvent e) {
        return look.onKey(e) || super.dispatchKeyEvent(e);
    }

    private SharedPreferences prefs() { return getSharedPreferences(DsPad.PREFS, MODE_PRIVATE); }

    /** Builds the start screen (LauncherLook: the KH Birth by Sleep port's style). */
    private void refresh() {
        final LinearLayout column = look.begin(TITLE, SUBTITLE, getDrawable(R.drawable.launcher_background));
        if (!rom().isFile()) {
            column.addView(look.text("First-time setup", 20, Color.WHITE, true));
            final TextView info = look.text(ROM_HELP, 15, 0xFFDDDDDD, false);
            info.setPadding(0, look.dp(8), 0, look.dp(20));
            column.addView(info);
            look.button(column, "Choose game ROM (.nds)…", null, () -> pick(PICK_ROM)).requestFocus();
            return;
        }
        final View first;
        if (resume("state").isFile()) {
            first = look.button(column, "Continue", "Where you last quit · " + resumeTime(), () ->
                    startActivity(new Intent(this, GameActivity.class).putExtra(GameActivity.EXTRA_CONTINUE, true)));
            look.button(column, "Start game", "From the title screen (in-game saves)",
                    () -> startActivity(new Intent(this, GameActivity.class)));
        } else {
            first = look.button(column, "Start game", "From the title screen",
                    () -> startActivity(new Intent(this, GameActivity.class)));
        }
        final LinearLayout row = look.row(column);
        look.small(row, "Save file", this::saveFile);
        look.small(row, "Mods", this::manageMods);
        look.small(row, "Trophies", () -> {
            final File achFile = new File(getFilesDir(), "saves/achievements.txt");
            PhAchievementsUi.showDialog(this, achFile, false, this::refresh);
        });
        look.small(row, "Settings", () -> look.settings(prefs(), GPU_DEFAULT, this::refresh));
        look.small(row, "Game ROM", this::gameRom);
        look.note(column, LauncherLook.summary(prefs(), GPU_DEFAULT) + "  ·  Save states: in the game's ≡ menu");
        first.requestFocus();
    }

    private void saveFile() {
        final String when = save().isFile() ? "Last saved: " + new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.US)
                .format(new Date(save().lastModified())) : "No save yet (the game makes one when you save in it).";
        look.dialog().setTitle("Save file").setMessage(when + "\n\nImport a save from melonDS (.sav): the save "
                        + "already here is kept as a dated backup. Export copies the save out.")
                .setPositiveButton("Import…", (d, w) -> pick(PICK_SAVE))
                .setNeutralButton("Export…", (d, w) -> {
                    final Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT);
                    i.addCategory(Intent.CATEGORY_OPENABLE);
                    i.setType("application/octet-stream");
                    i.putExtra(Intent.EXTRA_TITLE, SAVE_NAME);
                    startActivityForResult(i, EXPORT_SAVE);
                })
                .setNegativeButton("Back", null).show();
    }

    private void gameRom() {
        look.dialog().setTitle("Game ROM").setMessage("Ready: " + rom().getName() + " ("
                        + (rom().length() >> 20) + " MB)\n\nChoose the ROM again to replace it. Saves and save "
                        + "states stay.")
                .setPositiveButton("Choose ROM…", (d, w) -> pick(PICK_ROM))
                .setNegativeButton("Back", null).show();
    }

    /** When the resume state was written ("MM-DD HH:MM", from resume.txt). */
    private String resumeTime() {
        try (BufferedReader r = new BufferedReader(new FileReader(resume("txt")))) {
            final String line = r.readLine();
            if (line != null && !line.isEmpty()) return line.trim();
        } catch (IOException ignored) {
            // (no time: the file's date instead)
        }
        return new SimpleDateFormat("MM-dd HH:mm", Locale.US).format(new Date(resume("state").lastModified()));
    }


    private void pick(int request) {
        final Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        if (request == LauncherLook.REQUEST_BACKGROUND) {
            final String error = look.importBackground(uri);
            refresh();
            if (error != null) look.dialog().setMessage(error).setPositiveButton("OK", null).show();
            return;
        }
        if (request == LauncherLook.REQUEST_TEXPACK) {
            new Thread(() -> {
                final String message = look.importTexturePack(uri, prefs());
                runOnUiThread(() -> {
                    refresh();
                    look.dialog().setMessage(message).setPositiveButton("OK", null).show();
                });
            }).start();
            return;
        }
        // Large copies off the UI thread.
        new Thread(() -> {
            String message;
            try {
                if (request == PICK_ROM) message = importRom(uri);
                else if (request == PICK_SAVE) message = importSave(uri);
                else if (request == PICK_MOD_ZIP) message = importModZip(uri);
                else message = exportSave(uri);
            } catch (IOException e) {
                message = "That did not work: " + e.getMessage();
            }
            final String m = message;
            runOnUiThread(() -> {
                refresh();
                new AlertDialog.Builder(this).setMessage(m).setPositiveButton("OK", null).show();
            });
        }).start();
    }

    private String importRom(Uri uri) throws IOException {
        final File dest = rom(), tmp = new File(dest.getPath() + ".tmp");
        //noinspection ResultOfMethodCallIgnored
        dest.getParentFile().mkdirs();
        final String sha1 = copy(uri, tmp);
        for (String k : KNOWN) {
            if (k.equals(sha1)) {
                if (!tmp.renameTo(dest)) throw new IOException("cannot store the ROM");
                return "Game ROM ready. Press Play.";
            }
        }
        //noinspection ResultOfMethodCallIgnored
        tmp.delete();
        return "That file is not a supported Phantom Hourglass dump (USA or Europe, clean or D-pad patch).\nSHA-1: " + sha1;
    }

    private String importSave(Uri uri) throws IOException {
        final File dest = save(), tmp = new File(dest.getPath() + ".tmp");
        //noinspection ResultOfMethodCallIgnored
        dest.getParentFile().mkdirs();
        copy(uri, tmp);
        if (tmp.length() != SAVE_SIZE) {
            final long n = tmp.length();
            //noinspection ResultOfMethodCallIgnored
            tmp.delete();
            return "That save is " + n + " bytes; a Phantom Hourglass save is " + SAVE_SIZE + " bytes (melonDS's .sav).";
        }
        if (dest.isFile()) {
            final String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
            if (!dest.renameTo(new File(dest.getPath() + ".bak-" + stamp))) throw new IOException("cannot back up the old save");
        }
        if (!tmp.renameTo(dest)) throw new IOException("cannot store the save");
        return "Save imported (the previous one, if any, is kept as a backup).";
    }

    private String exportSave(Uri uri) throws IOException {
        if (!save().isFile()) return "There is no save yet.";
        try (InputStream in = new FileInputStream(save()); OutputStream out = getContentResolver().openOutputStream(uri)) {
            if (out == null) throw new IOException("cannot write there");
            final byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
        return "Save exported.";
    }

    // ---- mods management & .zip import -------------------------------------------------------------------------------

    private Map<String, Boolean> readModsIni() {
        final Map<String, Boolean> map = new LinkedHashMap<>();
        final File ini = new File(modsDir(), "mods.ini");
        if (!ini.isFile()) return map;
        try (BufferedReader r = new BufferedReader(new FileReader(ini))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                final int eq = line.indexOf('=');
                if (eq <= 0) continue;
                final String k = line.substring(0, eq).trim();
                final String v = line.substring(eq + 1).trim();
                map.put(k, "1".equals(v) || "true".equalsIgnoreCase(v) || "on".equalsIgnoreCase(v));
            }
        } catch (IOException ignored) {
        }
        return map;
    }

    private void writeModsIni(Map<String, Boolean> map) {
        final File dir = modsDir();
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        final StringBuilder sb = new StringBuilder("# Phantom Hourglass Recomp mod enable states\n");
        for (Map.Entry<String, Boolean> e : map.entrySet())
            sb.append(e.getKey()).append('=').append(e.getValue() ? '1' : '0').append('\n');
        try (FileOutputStream out = new FileOutputStream(new File(dir, "mods.ini"))) {
            out.write(sb.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    private static String extractJsonString(String json, String key, String fallback) {
        final String pat = "\"" + key + "\"";
        final int k = json.indexOf(pat);
        if (k < 0) return fallback;
        final int colon = json.indexOf(':', k + pat.length());
        if (colon < 0) return fallback;
        final int q1 = json.indexOf('"', colon + 1);
        if (q1 < 0) return fallback;
        final int q2 = json.indexOf('"', q1 + 1);
        return q2 > q1 ? json.substring(q1 + 1, q2) : fallback;
    }

    private static String readSmallText(File f) {
        if (!f.isFile() || f.length() > 256 * 1024) return "";
        try (FileInputStream in = new FileInputStream(f)) {
            final byte[] buf = new byte[(int) f.length()];
            final int n = in.read(buf);
            return n > 0 ? new String(buf, 0, n, java.nio.charset.StandardCharsets.UTF_8) : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static String sanitizeModId(String raw) {
        final StringBuilder sb = new StringBuilder();
        for (int i = 0; i < raw.length(); i++) {
            final char c = Character.toLowerCase(raw.charAt(i));
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') sb.append(c);
            else if (c == ' ') sb.append('-');
        }
        return sb.length() > 0 ? sb.toString() : "mod";
    }

    private void manageMods() {
        final File dir = modsDir();
        final List<File> modFolders = new ArrayList<>();
        final File[] children = dir.listFiles();
        if (children != null) {
            Arrays.sort(children, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File c : children)
                if (c.isDirectory() && new File(c, "mod.json").isFile()) modFolders.add(c);
        }
        final Map<String, Boolean> states = readModsIni();
        final List<String> labels = new ArrayList<>();
        final List<String> ids = new ArrayList<>();
        for (File mf : modFolders) {
            final String json = readSmallText(new File(mf, "mod.json"));
            final String id = sanitizeModId(extractJsonString(json, "id", mf.getName()));
            final String name = extractJsonString(json, "name", mf.getName());
            final String ver = extractJsonString(json, "version", "1.0.0");
            final boolean on = states.containsKey(id) ? Boolean.TRUE.equals(states.get(id)) : true;
            ids.add(id);
            labels.add((on ? "✓ " : "   ") + name + " v" + ver);
        }
        if (labels.isEmpty()) {
            look.dialog().setTitle("Mods")
                    .setMessage("No mods installed yet.\n\nImport a mod bundle (.phmod or .zip containing mod.json) to install it into app storage.")
                    .setPositiveButton("Import mod (.phmod / .zip)…", (d, w) -> pick(PICK_MOD_ZIP))
                    .setNegativeButton("Back", null).show();
            return;
        }
        final String[] shown = labels.toArray(new String[0]);
        look.dialog().setTitle("Mods (tap to enable/disable)")
                .setItems(shown, (d, which) -> {
                    final String id = ids.get(which);
                    final boolean cur = states.containsKey(id) ? Boolean.TRUE.equals(states.get(id)) : true;
                    states.put(id, !cur);
                    writeModsIni(states);
                    manageMods();
                })
                .setPositiveButton("Import mod (.phmod / .zip)…", (d, w) -> pick(PICK_MOD_ZIP))
                .setNeutralButton("Remove…", (d, w) -> removeModDialog(modFolders, labels))
                .setNegativeButton("Done", null).show();
    }

    private void removeModDialog(List<File> modFolders, List<String> labels) {
        final String[] names = new String[labels.size()];
        for (int i = 0; i < labels.size(); i++) names[i] = labels.get(i).replaceFirst("^[✓ ]+", "");
        look.dialog().setTitle("Remove which mod?")
                .setItems(names, (d, which) -> {
                    deleteRecursive(modFolders.get(which));
                    manageMods();
                })
                .setNegativeButton("Cancel", null).show();
    }

    private static void deleteRecursive(File f) {
        if (f.isDirectory()) {
            final File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) deleteRecursive(k);
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }

    /** Extracts a mod .zip (with mod.json at root or one folder deep) safely into files/mods/<id>/. */
    private String importModZip(Uri uri) throws IOException {
        final File tmpRoot = new File(getCacheDir(), "mod-import-" + System.currentTimeMillis());
        if (!tmpRoot.mkdirs()) throw new IOException("cannot create temp directory");
        try {
            final String canonRoot = tmpRoot.getCanonicalPath() + File.separator;
            int fileCount = 0;
            long totalBytes = 0;
            try (InputStream raw = getContentResolver().openInputStream(uri)) {
                if (raw == null) throw new IOException("cannot open the selected file");
                try (ZipInputStream zis = new ZipInputStream(raw)) {
                    ZipEntry entry;
                    final byte[] buf = new byte[1 << 16];
                    while ((entry = zis.getNextEntry()) != null) {
                        final String name = entry.getName().replace('\\', '/');
                        if (name.startsWith("/") || name.contains(".."))
                            throw new IOException("unsafe path inside zip: " + name);
                        final File out = new File(tmpRoot, name);
                        if (!out.getCanonicalPath().startsWith(canonRoot))
                            throw new IOException("zip path escapes target directory: " + name);
                        if (entry.isDirectory()) {
                            //noinspection ResultOfMethodCallIgnored
                            out.mkdirs();
                        } else {
                            //noinspection ResultOfMethodCallIgnored
                            out.getParentFile().mkdirs();
                            try (FileOutputStream fos = new FileOutputStream(out)) {
                                int n;
                                while ((n = zis.read(buf)) > 0) {
                                    totalBytes += n;
                                    if (totalBytes > 256L << 20) throw new IOException("mod archive exceeds 256 MB");
                                    fos.write(buf, 0, n);
                                }
                            }
                            fileCount++;
                        }
                        zis.closeEntry();
                    }
                }
            }
            if (fileCount == 0) return "That archive is empty.";

            // Locate mod.json at root or inside a single top-level directory.
            File modRoot = null;
            if (new File(tmpRoot, "mod.json").isFile()) {
                modRoot = tmpRoot;
            } else {
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
            if (modRoot == null) return "No mod.json found in that zip archive.";

            final String json = readSmallText(new File(modRoot, "mod.json"));
            final String id = sanitizeModId(extractJsonString(json, "id", modRoot.getName()));
            final String name = extractJsonString(json, "name", id);
            final File dest = new File(modsDir(), id);
            if (dest.exists()) deleteRecursive(dest);
            //noinspection ResultOfMethodCallIgnored
            modsDir().mkdirs();
            copyDirectory(modRoot, dest);
            final Map<String, Boolean> states = readModsIni();
            states.put(id, true);
            writeModsIni(states);
            return "Installed mod \"" + name + "\" (" + fileCount + " files).";
        } finally {
            deleteRecursive(tmpRoot);
        }
    }

    private static void copyDirectory(File src, File dst) throws IOException {
        if (src.isDirectory()) {
            //noinspection ResultOfMethodCallIgnored
            dst.mkdirs();
            final File[] kids = src.listFiles();
            if (kids != null) for (File k : kids) copyDirectory(k, new File(dst, k.getName()));
        } else {
            try (InputStream in = new FileInputStream(src); OutputStream out = new FileOutputStream(dst)) {
                final byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
        }
    }

    /** Copies a picked file to `to` and returns its SHA-1 (lowercase hex). */
    private String copy(Uri uri, File to) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri); OutputStream out = new FileOutputStream(to)) {
            if (in == null) throw new IOException("cannot open the file");
            final MessageDigest sha = MessageDigest.getInstance("SHA-1");
            final byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                sha.update(buf, 0, n);
            }
            final StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest()) hex.append(String.format(Locale.US, "%02x", b));
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
    }
}
