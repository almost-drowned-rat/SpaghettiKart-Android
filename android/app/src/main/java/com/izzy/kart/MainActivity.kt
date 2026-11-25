package com.izzy.kart

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

/**
 * The game itself.
 *
 * Over the SDL surface: the touch buttons (TouchControls / TouchOverlay; Automatic
 * hides them while a controller is used, the icon at the top centre opens their
 * settings), and the button to the mods and saves manager, shown while
 * libultraship's menu is open. Touches off the buttons still work as a mouse in
 * the menus.
 *
 * [LauncherActivity] guarantees mk64.o2r exists before this activity starts.
 */
class MainActivity : SDLActivity() {

    private lateinit var modsButton: Button
    private lateinit var modsOverlay: View

    private var menuOpen = false

    private var touch: TouchControls? = null

    /** RetroArch shaders: the packs and the choice (ShaderLibrary), drawn by ShaderHost through ShaderSurface. */
    private var shaders: ShaderLibrary? = null

    private val handler = Handler(Looper.getMainLooper())
    private val menuWatcher = object : Runnable {
        override fun run() {
            syncMenuState()
            handler.postDelayed(this, MENU_POLL_MS)
        }
    }

    // org/libsdl/app is kept byte-identical to the SDL release libultraship
    // pins, so the game library is named here rather than patched in there.
    // SDLActivity refuses to start if the Java glue and libSDL2.so disagree on
    // their version, so both have to move together.
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "Spaghettify")

    private external fun isMenuOpen(): Boolean

    /** With a shader chosen, SDL draws into the shader presenter's buffer (ShaderSurface) instead of the screen. */
    override fun createSDLSurface(context: Context): SDLSurface =
        if (ShaderHost.wanted(context)) ShaderSurface(context) else super.createSDLSurface(context)

    @Deprecated("Activity result API of the platform")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (shaders?.onActivityResult(requestCode, resultCode, data) == true) return
        super.onActivityResult(requestCode, resultCode, data)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        fillScreen()
        setupModsOverlay()
        touch = TouchControls(this, findViewById(android.R.id.content),
            TouchOverlay.Sink { b, x, y -> TouchNative.nativeTouchPad(b, x, y) },
            object : TouchControls.GameState {
                override fun frameStats(): FloatArray? = runCatching { TouchNative.nativeFrameStats() }.getOrNull()
                override fun running() = TouchNative.nativeGameRunning()
                override fun menuOpen() = runCatching { isMenuOpen() }.getOrDefault(false)
                // the engine's menu toggles on Back (AC_BACK in SDL, see SpaghettiGui.cpp)
                override fun openGameMenu() {
                    onNativeKeyDown(KeyEvent.KEYCODE_BACK)
                    onNativeKeyUp(KeyEvent.KEYCODE_BACK)
                }
            })
        touch?.let { t -> shaders = ShaderLibrary(this, t, ShaderHost.TARGET).also { t.setShaderMenu(it) } }
        // Back opens the game's card menu (MenuCards.cpp); its Touch controls / Shaders / FPS come from TouchControls
        touch?.setCardMenu(true)
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean =
        touch?.onKey(event) == true || super.dispatchKeyEvent(event)

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean =
        touch?.onMotion(event) == true || super.dispatchGenericMotionEvent(event)

    override fun dispatchTouchEvent(event: MotionEvent): Boolean =
        touch?.onTouch(event) == true || super.dispatchTouchEvent(event)

    override fun onResume() {
        super.onResume()
        fillScreen()
        // Re-sync immediately as well as on the timer, so coming back from the
        // mods manager never leaves the button showing a stale menu state.
        syncMenuState()
        handler.removeCallbacks(menuWatcher)
        handler.postDelayed(menuWatcher, MENU_POLL_MS)
        touch?.onResume()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // Also after a dialog, the file picker or another app had the screen.
        if (hasFocus) fillScreen()
    }

    /**
     * Full screen without the system bars (handheld fork, as in the DK64 and
     * Wind Waker forks; adapted from GreenNaugahyde's MainActivity.hideSystemBars,
     * MPL-2.0). SDL only sets the old setSystemUiVisibility flags, which left the
     * navigation bar's space as a black strip at the bottom on the AYN Odin 2
     * Portal and cut off the bottom of the picture. A swipe from the edge shows
     * the bars for a moment.
     */
    private fun fillScreen() {
        val window = window ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes = window.attributes.apply {
                layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
            }
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_FULLSCREEN)
        }
    }

    private val rehideBars = Runnable { fillScreen() }

    // Bars shown by something else (SDL's window style, the system): hidden
    // again after a moment.
    @Deprecated("Deprecated in Android, still called by SDL's listener")
    override fun onSystemUiVisibilityChange(visibility: Int) {
        @Suppress("DEPRECATION")
        super.onSystemUiVisibilityChange(visibility)
        @Suppress("DEPRECATION")
        if (visibility and View.SYSTEM_UI_FLAG_HIDE_NAVIGATION == 0 && hasWindowFocus()) {
            window.decorView.removeCallbacks(rehideBars)
            window.decorView.postDelayed(rehideBars, 2000)
        }
    }

    override fun onPause() {
        handler.removeCallbacks(menuWatcher)
        touch?.onPause()
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacks(menuWatcher)
        touch?.destroy()
        super.onDestroy()
    }

    /**
     * Shows the Mods button while the engine's menu is open.
     *
     * The menu is opened and closed by Android's Back (system bar or gesture),
     * B, a keyboard, or the menu dismissing itself, so the engine is asked
     * rather than tracking it here.
     */
    private fun syncMenuState() {
        val open = runCatching { isMenuOpen() }.getOrDefault(false)
        if (open == menuOpen) return

        menuOpen = open
        // Managing mods is a settings action, so it stays out of reach until
        // the menu is up.
        // the settings menu has a Mods tab now (openModsFromMenu), so the floating button stays hidden
        modsButton.visibility = View.GONE
    }

    private fun setupModsOverlay() {
        val inflater = getSystemService(LAYOUT_INFLATER_SERVICE) as LayoutInflater
        val overlay = inflater.inflate(R.layout.mods_overlay, null).apply {
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        }
        findViewById<ViewGroup>(android.R.id.content).addView(overlay)
        modsOverlay = overlay

        modsButton = overlay.findViewById(R.id.buttonMods)
        modsButton.setOnClickListener { startActivity(Intent(this, ModsActivity::class.java)) }
    }

    private companion object {
        /** Fast enough to feel immediate on a menu toggle, cheap enough to ignore. */
        const val MENU_POLL_MS = 150L
    }

    /** The settings menu's Mods tab (src/port/ui/Menu.cpp, through AndroidBridge.cpp; any thread). */
    @Suppress("unused")
    fun openModsFromMenu() {
        runOnUiThread { startActivity(Intent(this, ModsActivity::class.java)) }
    }
}
