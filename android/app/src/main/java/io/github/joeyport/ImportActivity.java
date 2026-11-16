package io.github.joeyport;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Launcher. The APK carries no game data: on first launch the player picks the folder of their own installed
 * copy (Joey_Pc.exe, Data.dat, Voice.dat, Voice/ and optionally a save) and it is copied to the app's
 * external files folder (Android/data/io.github.joeyport/files/game), where it can also be copied by USB.
 * Once the files are there it goes straight to the game.
 */
public class ImportActivity extends Activity {
    private static final int PICK = 1;
    private TextView status;
    private ProgressBar bar;
    private Button pick;
    private volatile boolean copying;

    static File gameDir(Context c) {
        File ext = c.getExternalFilesDir(null);
        return new File(ext != null ? ext : c.getFilesDir(), "game");
    }

    private static boolean has(File dir, String name) {
        String[] l = dir.list();
        if (l != null) for (String n : l) if (n.equalsIgnoreCase(name)) return true;
        return false;
    }

    static boolean installed(Context c) {
        File d = gameDir(c);
        return has(d, "Joey_Pc.exe") && has(d, "Data.dat");
    }

    private static boolean es() { return "es".equals(Locale.getDefault().getLanguage()); }
    private static String t(String es, String en) { return es() ? es : en; }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        if (installed(this)) { launch(); return; }

        LinearLayout ll = new LinearLayout(this);
        ll.setOrientation(LinearLayout.VERTICAL);
        ll.setGravity(Gravity.CENTER_HORIZONTAL);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        ll.setPadding(pad, pad, pad, pad);
        ll.setBackgroundColor(Color.BLACK);

        TextView title = new TextView(this);
        title.setText("Joey the Passion");
        title.setTextSize(26);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        ll.addView(title);

        TextView info = new TextView(this);
        info.setTextSize(15);
        info.setTextColor(Color.LTGRAY);
        info.setPadding(0, pad / 2, 0, pad / 2);
        info.setText(t(
            "Este port no incluye los archivos del juego. Copia a este dispositivo la carpeta de tu juego instalado "
            + "(la que contiene Joey_Pc.exe, Data.dat, Voice.dat y la carpeta Voice; si quieres conservar tu partida, "
            + "incluye también Region.dat, registry.txt y la carpeta \"Yu-Gi-Oh! Power of Chaos Common\") y elígela abajo.\n\n"
            + "También puedes copiarla por USB a:\n" + gameDir(this).getAbsolutePath(),
            "This port does not include the game files. Copy the folder of your installed game to this device "
            + "(the one with Joey_Pc.exe, Data.dat, Voice.dat and the Voice folder; to keep your progress also include "
            + "Region.dat, registry.txt and the \"Yu-Gi-Oh! Power of Chaos Common\" folder) and choose it below.\n\n"
            + "You can also copy it over USB to:\n" + gameDir(this).getAbsolutePath()));
        ll.addView(info);

        pick = new Button(this);
        pick.setText(t("Elegir carpeta del juego", "Choose the game folder"));
        pick.setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            startActivityForResult(i, PICK);
        });
        ll.addView(pick);

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(ProgressBar.GONE);
        ll.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        status = new TextView(this);
        status.setTextSize(15);
        status.setTextColor(Color.WHITE);
        status.setPadding(0, pad / 2, 0, 0);
        ll.addView(status);

        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(Color.BLACK);
        sv.addView(ll);
        setContentView(sv);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!copying && status != null && installed(this)) launch();     /* copied over USB meanwhile */
    }

    private void launch() {
        startActivity(new Intent(this, JoeyActivity.class));
        finish();
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req != PICK || res != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        copying = true;
        pick.setEnabled(false);
        bar.setVisibility(ProgressBar.VISIBLE);
        status.setText(t("Revisando la carpeta…", "Checking the folder…"));
        new Thread(() -> importTree(tree)).start();
    }

    /* ------------------------------------------------------------------ copy */
    private static class Entry { String name; Uri uri; boolean dir; long size; List<Entry> kids = new ArrayList<>(); }

    private List<Entry> list(ContentResolver cr, Uri tree, String docId) {
        List<Entry> out = new ArrayList<>();
        Uri kids = DocumentsContract.buildChildDocumentsUriUsingTree(tree, docId);
        String[] cols = { Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE };
        try (Cursor c = cr.query(kids, cols, null, null, null)) {
            while (c != null && c.moveToNext()) {
                Entry e = new Entry();
                String id = c.getString(0);
                e.name = c.getString(1);
                e.dir = Document.MIME_TYPE_DIR.equals(c.getString(2));
                e.size = c.isNull(3) ? 0 : c.getLong(3);
                e.uri = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                if (e.dir) e.kids = list(cr, tree, id);
                out.add(e);
            }
        }
        return out;
    }

    private static boolean contains(List<Entry> l, String name) {
        for (Entry e : l) if (!e.dir && e.name.equalsIgnoreCase(name)) return true;
        return false;
    }
    private static long total(List<Entry> l) {
        long n = 0;
        for (Entry e : l) n += e.dir ? total(e.kids) : e.size;
        return n;
    }

    private long done, all;
    private int lastPermille = -1;

    private void copy(ContentResolver cr, List<Entry> l, File dst, byte[] buf) throws Exception {
        dst.mkdirs();
        for (Entry e : l) {
            File f = new File(dst, e.name);
            if (e.dir) { copy(cr, e.kids, f, buf); continue; }
            File tmp = new File(dst, e.name + ".part");
            try (InputStream in = cr.openInputStream(e.uri); OutputStream out = new FileOutputStream(tmp)) {
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                    done += n;
                    int pm = all > 0 ? (int) (done * 1000 / all) : 0;
                    if (pm != lastPermille) {
                        lastPermille = pm;
                        final String name = e.name;
                        runOnUiThread(() -> { bar.setProgress(pm); status.setText(t("Copiando ", "Copying ") + name + "  " + pm / 10 + "%"); });
                    }
                }
            }
            if (f.exists()) f.delete();
            if (!tmp.renameTo(f)) throw new Exception("rename " + f);
        }
    }

    private void importTree(Uri tree) {
        String err = null;
        try {
            ContentResolver cr = getContentResolver();
            List<Entry> root = list(cr, tree, DocumentsContract.getTreeDocumentId(tree));
            if (!contains(root, "Joey_Pc.exe") || !contains(root, "Data.dat")) {
                err = t("Esa carpeta no tiene Joey_Pc.exe y Data.dat. Elige la carpeta donde está instalado el juego.",
                        "That folder has no Joey_Pc.exe and Data.dat. Choose the folder where the game is installed.");
            } else {
                all = total(root); done = 0;
                copy(cr, root, gameDir(this), new byte[1 << 20]);
            }
        } catch (Exception ex) {
            err = t("Error al copiar: ", "Copy failed: ") + ex;
        }
        final String e = err;
        runOnUiThread(() -> {
            copying = false;
            if (e == null && installed(this)) { launch(); return; }
            pick.setEnabled(true);
            bar.setVisibility(ProgressBar.GONE);
            status.setText(e);
        });
    }
}
