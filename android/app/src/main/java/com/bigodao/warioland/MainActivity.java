package com.bigodao.warioland;

import android.Manifest;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * Atividade do jogo. O SDLActivity faz todo o trabalho (janela, áudio,
 * controles, toque) e chama o main() nativo de libmain.so.
 *
 * Aqui ficam só as pontes que o código nativo chama:
 *  - seletor de arquivos do launcher (requestFilePick / pollPickedFile);
 *  - pasta "Bigodao" do usuário para mods e log (requestUserFolderAccess,
 *    userFolderAccessState, userFolderPath).
 */
public class MainActivity extends SDLActivity {
    private static final String TAG = "Bigodao";
    private static final int REQUEST_PICK = 0x5A1;
    private static final int REQUEST_STORAGE = 0x5A2;

    /* Resposta do seletor para o nativo: "<kind>\n<caminho>" (caminho vazio = cancelado). */
    private static volatile String sPickedResult = null;
    private static volatile int sPendingKind = 0;

    /* Acesso à pasta do usuário: 0 = não pedido, 1 = perguntando, 2 = liberado, 3 = negado. */
    private static volatile int sAccessState = 0;
    private static volatile boolean sWaitingManageResult = false;

    @Override
    protected String[] getLibraries() {
        // c++_shared antes, para aparelhos antigos que não resolvem dependências sozinhos.
        // "main" precisa ser o último: é a biblioteca com o main() do jogo.
        return new String[] { "c++_shared", "SDL2", "main" };
    }

    // ------------------------------------------------------------------
    // Pasta do usuário: /storage/emulated/0/Bigodao/{mods,log}
    // ------------------------------------------------------------------

    private static boolean hasUserFolderAccess(Context context) {
        if (Build.VERSION.SDK_INT >= 30) {
            return Environment.isExternalStorageManager();
        }
        if (Build.VERSION.SDK_INT >= 23) {
            return context.checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE)
                    == PackageManager.PERMISSION_GRANTED;
        }
        return true;
    }

    /**
     * Chamado pelo nativo ao iniciar. Pede a permissão só uma vez na vida do
     * app; se o usuário negar, o jogo usa a pasta interna do app.
     */
    public static int requestUserFolderAccess() {
        final SDLActivity activity = mSingleton;
        if (activity == null) return 0;
        if (hasUserFolderAccess(activity)) {
            sAccessState = 2;
            return 2;
        }
        SharedPreferences prefs = activity.getSharedPreferences("bigodao", Context.MODE_PRIVATE);
        if (prefs.getBoolean("askedFolderAccess", false)) {
            sAccessState = 3;
            return 3;
        }
        prefs.edit().putBoolean("askedFolderAccess", true).apply();
        sAccessState = 1;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    if (Build.VERSION.SDK_INT >= 30) {
                        sWaitingManageResult = true;
                        Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                Uri.parse("package:" + activity.getPackageName()));
                        try {
                            activity.startActivity(intent);
                        } catch (Exception e) {
                            activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                        }
                    } else if (Build.VERSION.SDK_INT >= 23) {
                        activity.requestPermissions(new String[] {
                                Manifest.permission.WRITE_EXTERNAL_STORAGE,
                                Manifest.permission.READ_EXTERNAL_STORAGE }, REQUEST_STORAGE);
                    } else {
                        sAccessState = 2;
                    }
                } catch (Exception e) {
                    Log.e(TAG, "Não foi possível pedir acesso aos arquivos", e);
                    sWaitingManageResult = false;
                    sAccessState = 3;
                }
            }
        });
        return 1;
    }

    public static int userFolderAccessState() {
        return sAccessState;
    }

    /** Cria Bigodao/mods e Bigodao/log e devolve o caminho de Bigodao (ou null). */
    @SuppressWarnings("deprecation")
    public static String userFolderPath() {
        try {
            File root = new File(Environment.getExternalStorageDirectory(), "Bigodao");
            File mods = new File(root, "mods");
            File log = new File(root, "log");
            if (!mods.isDirectory()) mods.mkdirs();
            if (!log.isDirectory()) log.mkdirs();
            return (root.isDirectory() && log.isDirectory()) ? root.getAbsolutePath() : null;
        } catch (Exception e) {
            Log.e(TAG, "Não foi possível criar a pasta Bigodao", e);
            return null;
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Voltou da tela "Acesso a todos os arquivos" (Android 11+)
        if (sWaitingManageResult) {
            sWaitingManageResult = false;
            sAccessState = hasUserFolderAccess(this) ? 2 : 3;
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQUEST_STORAGE) {
            sAccessState = hasUserFolderAccess(this) ? 2 : 3;
            return;
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    // ------------------------------------------------------------------
    // Seletor de arquivos do launcher
    // ------------------------------------------------------------------

    /** Chamado pelo código nativo (thread do SDL). */
    public static void requestFilePick(final int kind) {
        final SDLActivity activity = mSingleton;
        if (activity == null) {
            sPickedResult = kind + "\n";
            return;
        }
        sPendingKind = kind;
        activity.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                try {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    intent.addCategory(Intent.CATEGORY_OPENABLE);
                    intent.setType("*/*");   // .gb/.ips não têm MIME padrão
                    activity.startActivityForResult(intent, REQUEST_PICK);
                } catch (Exception e) {
                    Log.e(TAG, "Seletor de arquivos indisponível", e);
                    sPickedResult = kind + "\n";
                }
            }
        });
    }

    /** Chamado pelo código nativo a cada frame enquanto há um pedido pendente. */
    public static String pollPickedFile() {
        String result = sPickedResult;
        sPickedResult = null;
        return result;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_PICK) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        final int kind = sPendingKind;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            sPickedResult = kind + "\n";
            return;
        }
        final Uri uri = data.getData();
        new Thread(new Runnable() {
            @Override
            public void run() {
                String path = copyToInternal(uri);
                sPickedResult = kind + "\n" + (path != null ? path : "");
            }
        }, "wl-import").start();
    }

    private String displayName(Uri uri) {
        String name = null;
        Cursor cursor = null;
        try {
            cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
            if (cursor != null && cursor.moveToFirst()) {
                name = cursor.getString(0);
            }
        } catch (Exception ignored) {
        } finally {
            if (cursor != null) cursor.close();
        }
        if (name == null || name.isEmpty()) name = "imported.bin";
        return name.replaceAll("[\\\\/:*?\"<>|]", "_");
    }

    private String copyToInternal(Uri uri) {
        File dir = new File(getFilesDir(), "imports");
        if (!dir.isDirectory() && !dir.mkdirs()) {
            Log.e(TAG, "Não foi possível criar " + dir);
            return null;
        }
        File dest = new File(dir, displayName(uri));
        InputStream in = null;
        OutputStream out = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return null;
            out = new FileOutputStream(dest);
            byte[] buffer = new byte[16384];
            int n;
            while ((n = in.read(buffer)) > 0) {
                out.write(buffer, 0, n);
            }
            return dest.getAbsolutePath();
        } catch (Exception e) {
            Log.e(TAG, "Falha ao copiar " + uri, e);
            return null;
        } finally {
            try { if (in != null) in.close(); } catch (Exception ignored) { }
            try { if (out != null) out.close(); } catch (Exception ignored) { }
        }
    }
}
