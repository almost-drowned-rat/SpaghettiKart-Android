// ShaderHost - the Java side of the shader presenter (ShaderHost.cpp in the game's native library): SDL draws into
// a buffer, the presenter draws it through the chosen RetroArch shader onto the screen.
package com.izzy.kart;

import android.content.Context;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.io.File;

final class ShaderHost {
    private ShaderHost() { }

    /** True while this run draws through the presenter (decided when the activity creates its surface). */
    static boolean active;

    static native boolean nativeAvailable();
    static native boolean nativeShadersAvailable();
    /** The screen surface (re)appeared: returns the Surface SDL draws into, or null if the presenter can't run. */
    static native Surface nativeAttach(Surface screen, int width, int height);
    static native void nativeDetach();
    /** The .slangp to draw with; "" for none. */
    static native void nativeSetPreset(String path);
    static native boolean nativeCompiling();

    /** For ShaderLibrary: a choice is used at once while the presenter runs (from "Off": at the next start). */
    static final ShaderLibrary.Target TARGET = new ShaderLibrary.Target() {
        @Override public boolean live() { return active; }
        @Override public void use(String path) { nativeSetPreset(path); }
        @Override public boolean compiling() { return active && nativeCompiling(); }
    };

    /** From the activity's createSDLSurface: whether this run draws through the presenter (a shader was chosen). */
    static boolean wanted(Context c) {
        if (Build.VERSION.SDK_INT < 29) return false;
        String p = ShaderLibrary.chosen(c);
        if (p.isEmpty() || !new File(p).isFile()) return false;
        try { return nativeAvailable(); } catch (Throwable t) { Log.w("ShaderHost", "presenter: " + t); return false; }
    }

    /** After the surface exists: hands the chosen preset to the presenter. */
    static void applyChosen(Context c) {
        if (active) {
            try { nativeSetPreset(ShaderLibrary.chosen(c)); } catch (Throwable ignored) { }
        }
    }
}
