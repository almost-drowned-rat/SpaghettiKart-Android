// ShaderHost - the Java side of the shader presenter (ShaderHost.cpp in the game's native library): SDL draws into
// a buffer, the presenter draws it through the chosen RetroArch shader onto the screen.
package com.izzy.kart;

import android.view.Surface;

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
}
