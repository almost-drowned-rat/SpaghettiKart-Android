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
    /** The preset's parameters, one per line: name, description, value, min, max, step (tabs). */
    static native String nativeParams();
    static native void nativeSetParam(String name, float value);
    static native void nativeSetResolution(int lowResMode, int lines);
    static native void nativeSetEngineMenuOpen(boolean open);

    /** For ShaderLibrary: a choice is used at once while the presenter runs (from "Off": at the next start). */
    static final ShaderLibrary.Target TARGET = new ShaderLibrary.Target() {
        @Override public boolean live() { return active; }
        @Override public void use(String path) { nativeSetPreset(path); }
        @Override public boolean compiling() { return active && nativeCompiling(); }
        @Override public String params() { return active ? nativeParams() : ""; }
        @Override public void setParam(String name, float value) { if (active) nativeSetParam(name, value); }
        // Shader Resolution: Original / 2x - the game draws in libultraship's low resolution mode (240p / 480p
        // widescreen) and the shader works on that picture. setResolution(2, false): the shader gets the frame as it
        // is and the game's setting is left alone; (mode, false) after Original / 2x: the low resolution mode off
        // again. Only called while the game runs.
        @Override public boolean hasResolution() { return true; }
        @Override public void setResolution(int mode, boolean shaderOn) {
            int lowRes = shaderOn ? (mode == 0 ? 2 : 3) : (mode == 2 ? -1 : 0);   // -1: not touched
            int lines = shaderOn ? (mode == 0 ? 240 : 480) : 0;
            try { nativeSetResolution(lowRes, lines); } catch (Throwable t) { Log.w("ShaderHost", "resolution: " + t); }
        }
        @Override public void setEngineMenuOpen(boolean open) {
            if (active) { try { nativeSetEngineMenuOpen(open); } catch (Throwable ignored) { } }
        }
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
            try {
                String p = ShaderLibrary.chosen(c);
                nativeSetPreset(p);
                ShaderLibrary.applySaved(c, TARGET, p);
            } catch (Throwable ignored) { }
        }
    }
}
