package io.github.joeyport;

import android.app.AlertDialog;
import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.text.InputFilter;
import android.text.InputType;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.EditText;

import org.libsdl.app.SDLActivity;

/** The game: SDL runs SDL_main (host/main.c) in libmain.so with the game folder as its argument. */
public class JoeyActivity extends SDLActivity {
    private static AlertDialog textDialog;
    private static boolean textReported;
    private WifiManager.MulticastLock lanLock;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (Build.VERSION.SDK_INT >= 28)        /* use the whole screen, camera cutout included: the game stays centered */
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        /* LAN duels find each other with UDP broadcasts: many Wi-Fi drivers drop them unless an app holds this lock */
        try {
            WifiManager wm = (WifiManager) getApplicationContext().getSystemService(Context.WIFI_SERVICE);
            if (wm != null) { lanLock = wm.createMulticastLock("joey-lan"); lanLock.setReferenceCounted(false); lanLock.acquire(); }
        } catch (Exception e) { lanLock = null; }
    }

    @Override
    protected void onDestroy() {
        if (lanLock != null && lanLock.isHeld()) lanLock.release();
        super.onDestroy();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "SDL2_ttf", "main" };
    }

    @Override
    protected String[] getArguments() {
        return new String[] { ImportActivity.gameDir(this).getAbsolutePath() };
    }

    /* ------------------------------------------------------------------ text fields (host/textinput.c) */
    static native void nativeTextResult(String text, boolean ok);

    private static void report(String text, boolean ok) {
        if (textReported) return;
        textReported = true;
        textDialog = null;
        nativeTextResult(text, ok);
    }

    /** Called from the game thread when a text field gets focus (LAN name, deck export, replay title). */
    public static void showTextInput(final String initial, final int maxLength) {
        final SDLActivity a = (SDLActivity) mSingleton;
        if (a == null) { nativeTextResult(null, false); return; }
        a.runOnUiThread(() -> {
            if (textDialog != null) { textDialog.dismiss(); report(null, false); }
            textReported = false;
            final EditText et = new EditText(a);
            et.setSingleLine(true);
            et.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
            et.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            et.setFilters(new InputFilter[] { new InputFilter.LengthFilter(maxLength) });
            et.setText(initial);
            et.setSelectAllOnFocus(true);
            final AlertDialog d = new AlertDialog.Builder(a)
                .setTitle("Joey the Passion")
                .setView(et)
                .setPositiveButton(android.R.string.ok, (dlg, w) -> report(et.getText().toString(), true))
                .setNegativeButton(android.R.string.cancel, (dlg, w) -> report(null, false))
                .setOnCancelListener(dlg -> report(null, false))
                .create();
            et.setOnEditorActionListener((v, action, ev) -> {
                if (action == EditorInfo.IME_ACTION_DONE) { report(et.getText().toString(), true); d.dismiss(); return true; }
                return false;
            });
            d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            textDialog = d;
            d.show();
            et.requestFocus();
        });
    }

    /** The game closed the field while the dialog was open. */
    public static void dismissTextInput() {
        final SDLActivity a = (SDLActivity) mSingleton;
        if (a == null) return;
        a.runOnUiThread(() -> {
            if (textDialog != null) textDialog.dismiss();
            report(null, false);
        });
    }
}
