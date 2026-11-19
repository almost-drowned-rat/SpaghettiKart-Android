// ShaderLibrary - the RetroArch shaders in the menu Back opens. Nothing is shipped: the player picks a shader pack
// (a .zip like https://github.com/libretro/slang-shaders' "Download ZIP"), it is unpacked into the app's folder
// (<external files>/shaders), and any .slangp in it can then be chosen by browsing its folders. The choice is kept
// in SharedPreferences "shader". The shader presenter (ShaderHost) runs only when a shader was chosen when the game
// started; a change between shaders applies at once, from "Off" to a shader at the next start.
package com.izzy.kart;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.widget.Toast;

import java.io.BufferedOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

final class ShaderLibrary implements TouchControls.ShaderMenu {
    static final int PICK_ZIP = 0x5ad;
    private static final String TAG = "ShaderHost";

    private final Activity activity;
    private final TouchControls touch;
    private volatile boolean importing;

    ShaderLibrary(Activity activity, TouchControls touch) {
        this.activity = activity;
        this.touch = touch;
    }

    static File folder(Context c) { return new File(c.getExternalFilesDir(null), "shaders"); }

    private static SharedPreferences prefs(Context c) { return c.getSharedPreferences("shader", Context.MODE_PRIVATE); }

    /** The chosen preset (absolute path), or "" for none. */
    static String chosen(Context c) { return prefs(c).getString("preset", ""); }

    /** From the activity's createSDLSurface: whether this run draws through the presenter. */
    static boolean wanted(Context c) {
        if (Build.VERSION.SDK_INT < 29) return false;
        String p = chosen(c);
        if (p.isEmpty() || !new File(p).isFile()) return false;
        try { return ShaderHost.nativeAvailable(); } catch (Throwable t) { Log.w(TAG, "presenter: " + t); return false; }
    }

    /** After the surface exists: hands the chosen preset to the presenter. */
    static void applyChosen(Context c) {
        if (ShaderHost.active) {
            try { ShaderHost.nativeSetPreset(chosen(c)); } catch (Throwable ignored) { }
        }
    }

    // ---------------------------------------------------------------- the menu

    @Override public String current() {
        String p = chosen(activity);
        if (p.isEmpty()) return "Off";
        String name = new File(p).getName();
        return name.endsWith(".slangp") ? name.substring(0, name.length() - 7) : name;
    }

    @Override public SideMenu.Page page() { return folderPage(folder(activity), "Shader"); }

    private SideMenu.Page folderPage(final File dir, final String title) {
        final boolean top = dir.equals(folder(activity));
        return new SideMenu.Page() {
            public String title() { return title; }
            public List<SideMenu.Item> items() {
                List<SideMenu.Item> l = new ArrayList<>();
                if (top) {
                    l.add(new SideMenu.Item("Off") { @Override void run() { choose(""); } });
                    l.add(new SideMenu.Item(importing ? "Importing shader pack..." : "Import shader pack (.zip)") {
                        @Override void run() { if (!importing) pickZip(); }
                    });
                }
                File[] files = dir.listFiles();
                if (files != null) {
                    Arrays.sort(files, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
                    for (final File f : files) {
                        if (f.isDirectory() && containsPresets(f)) {
                            l.add(new SideMenu.Item(f.getName() + "  ›") {
                                @Override void run() { touch.pushPage(folderPage(f, f.getName())); }
                            });
                        }
                    }
                    for (final File f : files) {
                        if (f.isFile() && f.getName().endsWith(".slangp")) {
                            final String name = f.getName().substring(0, f.getName().length() - 7);
                            final boolean isChosen = f.getAbsolutePath().equals(chosen(activity));
                            l.add(new SideMenu.Item((isChosen ? "• " : "") + name) {
                                @Override void run() { choose(f.getAbsolutePath()); }
                            });
                        }
                    }
                }
                if (top && l.size() == 2) {
                    l.add(new SideMenu.Item("(no shaders yet: import a pack, e.g. libretro's slang-shaders .zip)") { });
                }
                return l;
            }
        };
    }

    private static boolean containsPresets(File dir) {
        File[] files = dir.listFiles();
        if (files == null) return false;
        for (File f : files) {
            if (f.isFile() && f.getName().endsWith(".slangp")) return true;
            if (f.isDirectory() && containsPresets(f)) return true;
        }
        return false;
    }

    private void choose(String path) {
        prefs(activity).edit().putString("preset", path).apply();
        if (ShaderHost.active) {
            try { ShaderHost.nativeSetPreset(path); } catch (Throwable ignored) { }
        } else if (!path.isEmpty()) {
            toast("The shader is used from the next start of the game");
        }
        touch.refreshPage();
    }

    // ---------------------------------------------------------------- importing a pack

    private void pickZip() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[] { "application/zip", "application/x-zip-compressed", "application/octet-stream" });
        try {
            activity.startActivityForResult(i, PICK_ZIP);
        } catch (Exception e) {
            toast("No file picker available");
        }
    }

    /** From the activity's onActivityResult. True when it was the shader pack's. */
    boolean onActivityResult(int request, int result, Intent data) {
        if (request != PICK_ZIP) return false;
        if (result == Activity.RESULT_OK && data != null && data.getData() != null) importZip(data.getData());
        return true;
    }

    private void importZip(final Uri uri) {
        importing = true;
        touch.refreshPage();
        toast("Importing the shader pack...");
        new Thread(() -> {
            String message;
            File target = folder(activity);
            File temp = new File(activity.getExternalFilesDir(null), "shaders.importing");
            try {
                deleteTree(temp);
                int presets = unzip(uri, temp);
                if (presets == 0) {
                    deleteTree(temp);
                    message = "No shaders found in that file (.slangp presets)";
                } else {
                    File old = new File(activity.getExternalFilesDir(null), "shaders.old");
                    deleteTree(old);
                    if (target.exists() && !target.renameTo(old)) throw new IOException("can't replace the old shaders");
                    if (!temp.renameTo(target)) throw new IOException("can't move the shaders in place");
                    deleteTree(old);
                    message = presets + " shaders imported";
                    // a shader chosen from the old pack that is gone: Off
                    String p = chosen(activity);
                    if (!p.isEmpty() && !new File(p).isFile()) prefs(activity).edit().putString("preset", "").apply();
                }
            } catch (Exception e) {
                Log.w(TAG, "import: " + e);
                deleteTree(temp);
                message = "The shader pack couldn't be imported: " + e.getMessage();
            }
            final String m = message;
            new Handler(Looper.getMainLooper()).post(() -> {
                importing = false;
                toast(m);
                touch.refreshPage();
            });
        }, "ShaderImport").start();
    }

    /** Unpacks the zip into dir, without a single top folder (GitHub's "slang-shaders-master/"); returns the
     *  number of .slangp presets. */
    private int unzip(Uri uri, File dir) throws IOException {
        String prefix = null;
        try (InputStream in = activity.getContentResolver().openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String n = e.getName();
                int slash = n.indexOf('/');
                String first = slash > 0 ? n.substring(0, slash + 1) : "";
                if (prefix == null) prefix = first;
                else if (!prefix.equals(first)) { prefix = ""; break; }
            }
        }
        if (prefix == null) prefix = "";
        int presets = 0;
        String root = dir.getCanonicalPath() + File.separator;
        byte[] buf = new byte[64 * 1024];
        try (InputStream in = activity.getContentResolver().openInputStream(uri); ZipInputStream z = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = z.getNextEntry()) != null) {
                String n = e.getName();
                if (!prefix.isEmpty() && n.startsWith(prefix)) n = n.substring(prefix.length());
                if (n.isEmpty()) continue;
                File f = new File(dir, n);
                if (!f.getCanonicalPath().startsWith(root)) continue;   // no paths out of the folder
                if (e.isDirectory()) { f.mkdirs(); continue; }
                File parent = f.getParentFile();
                if (parent != null) parent.mkdirs();
                try (OutputStream out = new BufferedOutputStream(new FileOutputStream(f))) {
                    int r;
                    while ((r = z.read(buf)) > 0) out.write(buf, 0, r);
                }
                if (n.endsWith(".slangp")) presets++;
            }
        }
        return presets;
    }

    private static void deleteTree(File f) {
        if (f == null || !f.exists()) return;
        File[] children = f.listFiles();
        if (children != null) for (File c : children) deleteTree(c);
        f.delete();
    }

    private void toast(String text) {
        activity.runOnUiThread(() -> Toast.makeText(activity, text, Toast.LENGTH_LONG).show());
    }
}
