// ShaderSurface - SDL's surface view, but SDL draws into the shader presenter's buffer (ShaderHost) while the view's
// own surface is the presenter's screen. Used instead of SDLSurface when a shader is chosen (ShaderLibrary).
package com.izzy.kart;

import android.content.Context;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;

import org.libsdl.app.SDLSurface;

final class ShaderSurface extends SDLSurface {
    private Surface buffer;

    ShaderSurface(Context context) { super(context); }

    @Override
    public Surface getNativeSurface() {
        return buffer != null ? buffer : super.getNativeSurface();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        try {
            buffer = ShaderHost.nativeAttach(holder.getSurface(), width, height);
        } catch (Throwable t) {
            Log.w("ShaderHost", "presenter not available: " + t);
            buffer = null;
        }
        ShaderHost.active = buffer != null;
        ShaderHost.applyChosen(getContext());
        super.surfaceChanged(holder, format, width, height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        super.surfaceDestroyed(holder);
        try { ShaderHost.nativeDetach(); } catch (Throwable ignored) { }
    }
}
