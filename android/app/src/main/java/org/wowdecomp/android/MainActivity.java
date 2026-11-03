package org.wowdecomp.android;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.net.Uri;
import android.opengl.GLSurfaceView;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;
import org.json.JSONObject;

/** Login/realm/character screens from the original Glue resources, then the in-world view. */
public final class MainActivity extends Activity {
    static { System.loadLibrary("wow_android"); }
    private static native String networkConnect(String server, String account, char[] password);
    private static native String networkPoll();
    private static native String networkChoose(int kind, int index);
    private static native String networkDisconnect();
    private static native String openGlueResources(String root);
    private static native byte[] readGlueResource(String path);
    private static native void closeGlueResources();
    private static native boolean initializeGraphics();
    private static native void resizeGraphics(int width, int height);
    private static native void drawFrame(long milliseconds);
    private static native String loadWorldTerrain(String directory);
    private static native String loadLoginScene(String directory);
    private static native String loadSelectScene(String directory, int[] look);
    private static native String creatorCommand(String directory, int op, int arg, int arg2);
    private static native void rotateGlueScene(float dx);
    private static native void clearGlueScene();
    private static native void clearWorldScene();
    private static native String networkCreateCharacter(String name, int[] look);
    private static native String networkDeleteCharacter(int index);
    private static native String extractGlueMusic(String directory, String cacheDir);
    private static native void initializeInput();
    private static native void inputFocus(boolean focused);
    private static native void touchMovement(float right, float forward);
    private static native void touchCamera(boolean held, float dx, float dy);
    private static native void touchJump(boolean held);
    private static native void touchFreeLook(boolean on);
    private static native String pollAudio();
    private static native void uiBackPressed();
    private String worldStatusText = "";
    private static native byte[] readArchiveFile(String path);
    private GameAudio gameAudio;
    private static native void touchTap(int x, int y);
    private static native void touchPointer(int x, int y);
    private static native void touchDown(int x, int y);
    private static native void touchMove(int x, int y);
    private static native void touchUp(int x, int y);
    private static native void touchCancel();
    private static native String[] openedArchiveFiles();
    private static native void touchWheel(int x, int y, int delta);
    private static native boolean uiPointerCaptured();
    private static native boolean uiTextWanted();
    private static native void textInput(String text);
    private static native void keyInput(int which);
    private static native void touchZoom(float factor);
    private static native void worldTap(float x, float y, float width, float height);
    private static native String worldOverlay();
    private static native void setOriginalUi(boolean enabled);
    private static native int originalUiState();
    private static native void installCrashReport(String path);
    private static native void setUiSkippedFiles(String list);
    private static native String originalUiInfo();
    private static native String originalUiLog();
    private String crashSkippedFile;
    private UiKeyboard keyboard;

    private static final Object glueLock = new Object();
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final ScheduledExecutorService network = Executors.newSingleThreadScheduledExecutor();
    private GLSurfaceView surface;
    private TouchControlsView controls;
    private GlueScreenView glueScreen;
    private TextView worldHud;
    private Button worldExit;
    private File dataDirectory;
    private String loadedGlueRoot, worldRoot;
    private boolean glueLoading, loginPreparing, waitingForStorage, worldLoading, askedPermission;
    private volatile boolean inWorld; // read by the GL thread
    private volatile int fps;
    // The 3D world renders at a reduced resolution (scaled up by the compositor): the
    // fill rate of full-HD+ phone screens is the main GPU cost.
    private static final float WORLD_RENDER_SCALE = .8f;
    // GL surface size (smaller than the view while the world renders scaled): touch → surface pixels.
    private volatile int surfacePixelsW, surfacePixelsH;
    private int sx(int x) { int w = controls.getWidth(); return w > 0 && surfacePixelsW > 0 ? (int)((long)x * surfacePixelsW / w) : x; }
    private int sy(int y) { int h = controls.getHeight(); return h > 0 && surfacePixelsH > 0 ? (int)((long)y * surfacePixelsH / h) : y; }
    // Glue 3D scenes: latest request wins; creator commands run in order on the worker.
    private final java.util.concurrent.atomic.AtomicReference<String> sceneRequest = new java.util.concurrent.atomic.AtomicReference<>("");
    private String sceneKey = "";
    private boolean sceneLoaded, scenesDisabled, musicDisabled, originalUiDisabled;
    private android.media.MediaPlayer music;

    // Glue music (SoundEntries GS_* kit) extracted from the MPQs and looped while the
    // login/character screens are visible.
    private void startMusic(String root) {
        if (musicDisabled || music != null) return;
        final String cache = getCacheDir().getAbsolutePath();
        worker.execute(() -> {
            final String path = extractGlueMusic(root, cache);
            runOnUiThread(() -> {
                if (path == null || isDestroyed() || musicDisabled || music != null) return;
                try {
                    android.media.MediaPlayer player = new android.media.MediaPlayer();
                    player.setDataSource(path);
                    player.setLooping(true);
                    player.setVolume(.7f, .7f);
                    player.prepare();
                    music = player;
                    if (!inWorld && hasWindowFocus()) player.start();
                } catch (Exception error) { glueScreen.status("Música do login indisponível: " + error.getMessage()); }
            });
        });
    }
    private void pauseMusic() { if (music != null && music.isPlaying()) music.pause(); }
    private void resumeMusic() { if (music != null && !inWorld && !musicDisabled) music.start(); }
    private void stopMusic() { if (music != null) { music.release(); music = null; } }
    // Set before a native 3D scene load and cleared a few seconds after it rendered. If the app
    // dies in between, the next start disables the 3D scenes so the login stays usable.
    private void sceneGuard(boolean on) {
        getPreferences(MODE_PRIVATE).edit().putBoolean("sceneCrashGuard", on).commit();
    }
    private org.json.JSONArray looks = new org.json.JSONArray();
    private int expansion = 2, stage = 0, selectedCharacter = -1;
    private long resultSerial = -1;
    private int[] creatorLook;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        File crashFile = new File(getFilesDir(), "crash.txt");
        String crashText = readCrashReport(crashFile);
        installCrashReport(crashFile.getAbsolutePath());
        initializeInput();
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        String initialPath = new File(Environment.getExternalStorageDirectory(), "Download/WoW/Data").getAbsolutePath();
        dataDirectory = new File(getPreferences(MODE_PRIVATE).getString("dataPath", initialPath));
        scenesDisabled = getPreferences(MODE_PRIVATE).getBoolean("scenesDisabled", false);
        musicDisabled = getPreferences(MODE_PRIVATE).getBoolean("musicDisabled", false);
        originalUiDisabled = getPreferences(MODE_PRIVATE).getBoolean("originalUiDisabled", false);
        android.content.SharedPreferences prefs = getPreferences(MODE_PRIVATE);
        // FrameXML files that crashed an earlier run are skipped (list reset by each new version).
        int versionCode = 0;
        try {
            android.content.pm.PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            versionCode = Build.VERSION.SDK_INT >= 28 ? (int) info.getLongVersionCode() : info.versionCode;
        } catch (Exception ignored) {}
        if (prefs.getInt("uiSkipVersion", 0) != versionCode)
            prefs.edit().putString("uiSkipFiles", "").putInt("uiSkipVersion", versionCode).commit();
        String skipFiles = prefs.getString("uiSkipFiles", "");
        crashSkippedFile = null;
        if (crashText != null) {
            // The report says where it died; it replaces the coarse guard below.
            prefs.edit().putBoolean("uiCrashGuard", false).commit();
            String marker = "Última etapa: UI boot: FrameXML ";
            int at = crashText.indexOf(marker);
            String file = null;
            if (at >= 0) {
                int end = crashText.indexOf('\n', at);
                file = crashText.substring(at + marker.length(), end < 0 ? crashText.length() : end).trim();
                if (file.isEmpty() || file.equals("(toc)")) file = null;
            }
            int skipped = skipFiles.isEmpty() ? 0 : skipFiles.split("\n").length;
            if (file != null && skipped < 12) {
                skipFiles = skipFiles.isEmpty() ? file : skipFiles + "\n" + file;
                prefs.edit().putString("uiSkipFiles", skipFiles).commit();
                crashSkippedFile = file;
            } else if (crashText.contains("Última etapa: UI")) {
                originalUiDisabled = true;
                prefs.edit().putBoolean("originalUiDisabled", true).commit();
            }
        }
        // The original interface boots inside the world; if the app died there, start without it.
        if (prefs.getBoolean("uiCrashGuard", false)) {
            originalUiDisabled = true;
            prefs.edit().putBoolean("originalUiDisabled", true).putBoolean("uiCrashGuard", false).commit();
        }
        setUiSkippedFiles(skipFiles);
        setOriginalUi(!originalUiDisabled);
        if (getPreferences(MODE_PRIVATE).getBoolean("sceneCrashGuard", false)) {
            scenesDisabled = true;
            getPreferences(MODE_PRIVATE).edit().putBoolean("scenesDisabled", true).putBoolean("sceneCrashGuard", false).commit();
        }

        gameAudio = new GameAudio(getCacheDir(), MainActivity::readArchiveFile);
        surface = new GLSurfaceView(this);
        surface.setEGLContextClientVersion(2);
        surface.setRenderer(new GLSurfaceView.Renderer() {
            @Override public void onSurfaceCreated(GL10 unused, EGLConfig config) {
                if (!initializeGraphics()) runOnUiThread(() -> glueScreen.status("Falha ao iniciar o OpenGL ES."));
            }
            @Override public void onSurfaceChanged(GL10 unused, int width, int height) {
                surfacePixelsW = width; surfacePixelsH = height;
                resizeGraphics(width, height);
            }
            private int frames, fpsFrames;
            private long fpsStart;
            @Override public void onDrawFrame(GL10 unused) {
                final long now = SystemClock.elapsedRealtime();
                drawFrame(now);
                if (fpsStart == 0) fpsStart = now;
                ++fpsFrames;
                if (now - fpsStart >= 1000) { fps = fpsFrames * 1000 / (int)(now - fpsStart); fpsFrames = 0; fpsStart = now; }
                // Nameplates/target frame follow the camera: refresh the overlay every other frame.
                if (inWorld && (frames & 7) == 0) {
                    final String audio = pollAudio();
                    if (audio != null && !audio.isEmpty()) {
                        if (audio.contains("app|logout")) network.execute(() -> showNetwork(networkDisconnect()));
                        if (gameAudio != null) controls.post(() -> gameAudio.handle(audio));
                    }
                }
                if (inWorld && (++frames & 1) == 0) {
                    final String overlay = worldOverlay();
                    final int ui = originalUiState();
                    final boolean typing = ui == 1 && uiTextWanted();
                    controls.post(() -> {
                        controls.setOverlay(overlay); controls.setOriginalUi(ui == 1); uiStateChanged(ui);
                        keyboard.setWanted(typing && inWorld);
                    });
                }
            }
        });
        controls = new TouchControlsView(this, new TouchController.Sink() {
            @Override public void movement(float right, float forward) { touchMovement(right, forward); }
            @Override public void camera(boolean held, float dx, float dy) { touchCamera(held, dx, dy); }
            @Override public void jump(boolean held) { touchJump(held); }
            @Override public void pointer(int x, int y) { touchPointer(sx(x), sy(y)); }
            @Override public void pointerDown(int x, int y) { touchDown(sx(x), sy(y)); }
            @Override public void pointerMove(int x, int y) { touchMove(sx(x), sy(y)); }
            @Override public void pointerUp(int x, int y) { touchUp(sx(x), sy(y)); }
            @Override public void pointerCancel() { touchCancel(); }
            @Override public boolean pointerOnUi() { return inWorld && uiPointerCaptured(); }
            @Override public void wheel(int x, int y, int delta) { touchWheel(sx(x), sy(y), delta); }
            @Override public void tap(int x, int y) {
                if (inWorld) worldTap(x, y, controls.getWidth(), controls.getHeight());
            }
            @Override public void zoom(float factor) { touchZoom(factor); }
            @Override public void freeLook(boolean on) { touchFreeLook(on); }
        });
        glueScreen = new GlueScreenView(this, new GlueScreenView.Actions() {
            @Override public void login(String user, char[] secret) { MainActivity.this.login(user, secret); }
            @Override public void realm(int index) { network.execute(() -> showNetwork(networkChoose(0, index))); }
            @Override public void enter(int index) { network.execute(() -> showNetwork(networkChoose(1, index))); }
            @Override public void disconnect() { network.execute(() -> showNetwork(networkDisconnect())); }
            @Override public void settings() { MainActivity.this.settings(); }
            @Override public void permission() { requestDataAccess(); }
            @Override public void exit() { finish(); }
            @Override public void selection(int index) { selectedCharacter = index; if (!glueScreen.creating()) updateScene(); }
            @Override public void rotate(float dx) { rotateGlueScene(dx); }
            @Override public void openCreator() { glueScreen.openCreator(); creator(0, expansion, 0); }
            @Override public void creator(int op, int arg, int arg2) { MainActivity.this.creator(op, arg, arg2); }
            @Override public void create(String name) {
                final int[] look = creatorLook;
                if (look == null) return;
                network.execute(() -> showNetwork(networkCreateCharacter(name, look)));
            }
            @Override public void delete(int index) { confirmDelete(index); }
            @Override public void closeCreator() { glueScreen.closeCreator(); sceneKey = ""; updateScene(); }
        });
        worldHud = new TextView(this);
        worldHud.setTextColor(Color.argb(200, 255, 255, 255)); worldHud.setTextSize(10);
        worldHud.setShadowLayer(3, 1, 1, Color.BLACK); worldHud.setPadding(16, 8, 16, 8);
        worldHud.setOnClickListener(view -> showUiLog());
        worldExit = new Button(this); worldExit.setText("Sair do mundo");
        worldExit.setOnClickListener(view -> network.execute(() -> showNetwork(networkDisconnect())));

        keyboard = new UiKeyboard(this, new UiKeyboard.Sink() {
            @Override public void text(String text) { textInput(text); }
            @Override public void key(int which) { keyInput(which); }
        });
        FrameLayout screen = new FrameLayout(this);
        screen.addView(keyboard, new FrameLayout.LayoutParams(1, 1));
        screen.setBackgroundColor(Color.rgb(9, 14, 23));
        screen.setOnApplyWindowInsetsListener((view, insets) -> {
            view.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            return insets;
        });
        screen.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        // Soft keyboard (adjustResize) shrinks the view: keep the scaled world buffer in proportion.
        surface.addOnLayoutChangeListener((view, l, t, r, b, ol, ot, or, ob) -> {
            if (inWorld && (r - l != or - ol || b - t != ob - ot) && r > l && b > t)
                surface.getHolder().setFixedSize(Math.max(1, (int)((r - l) * WORLD_RENDER_SCALE)),
                    Math.max(1, (int)((b - t) * WORLD_RENDER_SCALE)));
        });
        screen.addView(controls, new FrameLayout.LayoutParams(-1, -1));
        screen.addView(glueScreen, new FrameLayout.LayoutParams(-1, -1));
        screen.addView(worldHud, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL));
        screen.addView(worldExit, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.RIGHT));
        setContentView(screen);
        showGlue();
        network.scheduleWithFixedDelay(() -> showNetwork(networkPoll()), 50, 50, TimeUnit.MILLISECONDS);
        if (crashText != null) showCrashReport(crashText);
    }

    // ---- Original Glue screens -------------------------------------------------------------

    private void showGlue() {
        controls.cancelTouches(); controls.setVisibility(View.GONE);
        worldHud.setVisibility(View.GONE); worldExit.setVisibility(View.GONE);
        inputFocus(false);
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        glueScreen.setVisibility(View.VISIBLE);
    }

    // ---- Glue 3D scenes --------------------------------------------------------------------

    private String lookKey(int index) {
        org.json.JSONArray look = looks.optJSONArray(index);
        return look == null ? "select:none" : "select:" + look;
    }

    /** Login scene before character list; selected character's scene on character select. */
    private void updateScene() {
        if (loadedGlueRoot == null || inWorld || glueScreen.creating()) return;
        final String root = loadedGlueRoot;
        if (stage == 4) {
            final String key = lookKey(selectedCharacter);
            if (key.equals(sceneKey)) return;
            sceneKey = key;
            org.json.JSONArray values = looks.optJSONArray(selectedCharacter);
            // Appearance (8) + equipment display IDs (19) + weapon inventory types (3).
            final int[] look = new int[values != null && values.length() >= 30 ? 30 : 8];
            if (values != null) for (int i = 0; i < look.length; i++) look[i] = values.optInt(i);
            loadScene(key, () -> loadSelectScene(root, look));
        } else if (stage <= 3 || stage == 7) {
            if (sceneKey.equals("login") || (stage == 3 && sceneKey.startsWith("select"))) return;
            sceneKey = "login";
            loadScene("login", () -> loadLoginScene(root));
        }
    }

    private void loadScene(String key, java.util.function.Supplier<String> loader) {
        if (scenesDisabled) return;
        sceneRequest.set(key);
        worker.execute(() -> {
            if (!key.equals(sceneRequest.get())) return; // superseded
            sceneGuard(true);
            final String error = loader.get();
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                surface.postDelayed(() -> sceneGuard(false), 4000);
                if (error == null || error.startsWith("Aviso")) sceneLoaded = true;
                glueScreen.setScene3d(sceneLoaded);
                if (error != null) glueScreen.status(error);
            });
        });
    }

    private void creator(int op, int arg, int arg2) {
        final String root = loadedGlueRoot;
        if (root == null) return;
        if (scenesDisabled) {
            glueScreen.status("A criação de personagem usa o cenário 3D, que está desativado em Configuração.");
            return;
        }
        sceneKey = "create";
        sceneRequest.set("create");
        worker.execute(() -> {
            final String json = creatorCommand(root, op, arg, arg2);
            runOnUiThread(() -> {
                if (isDestroyed() || !glueScreen.creating()) return;
                try {
                    JSONObject state = new JSONObject(json);
                    org.json.JSONArray look = state.optJSONArray("look");
                    if (look != null) {
                        creatorLook = new int[8];
                        for (int i = 0; i < 8; i++) creatorLook[i] = look.getInt(i);
                    }
                    if (state.has("races")) { sceneLoaded = true; glueScreen.setScene3d(true); }
                    glueScreen.creatorState(state);
                } catch (Exception error) { glueScreen.status("Resposta inválida da criação de personagem."); }
            });
        });
    }

    private void confirmDelete(int index) {
        new AlertDialog.Builder(this).setTitle("Apagar personagem")
            .setMessage("Apagar este personagem permanentemente?")
            .setPositiveButton("Apagar", (dialog, which) -> network.execute(() -> showNetwork(networkDeleteCharacter(index))))
            .setNegativeButton("Cancelar", null).show();
    }

    private void loadGlue() {
        if (glueLoading || isDestroyed()) return;
        final String root = readableDataRoot();
        if (root == null) return;
        glueLoading = true; glueScreen.loading("Lendo a interface original dos MPQs…");
        worker.execute(() -> {
            GlueAssets result = null; String failure = null;
            synchronized (glueLock) {
                try {
                    String error = openGlueResources(root);
                    if (error != null) throw new IllegalArgumentException(error);
                    result = GlueAssets.load(this, MainActivity::readGlueResource);
                } catch (Exception error) { failure = "Não foi possível carregar a interface: " + error.getMessage(); }
                finally { closeGlueResources(); }
            }
            final GlueAssets value = result; final String error = failure;
            runOnUiThread(() -> {
                if (isDestroyed()) return;
                glueLoading = false;
                if (value == null) { glueScreen.loadFailed(error); return; }
                loadedGlueRoot = root; glueScreen.loaded(value); sceneKey = ""; updateScene(); startMusic(root);
                if (scenesDisabled) glueScreen.status("Cenários 3D desativados (o app fechou ao carregá-los). Reative em Configuração.");
                if (!value.missing.isEmpty()) glueScreen.status("Recursos não encontrados: " + value.missing.size() + ".");
            });
        });
    }

    private void settings() {
        LinearLayout fields = new LinearLayout(this); fields.setOrientation(LinearLayout.VERTICAL); fields.setPadding(24, 8, 24, 8);
        TextView help = new TextView(this);
        help.setText("Pasta Data com os MPQs do cliente 3.3.5a. Servidor e porta vêm do realmlist.wtf (Data/<idioma>, Data ou pasta do cliente; porta padrão 3724).");
        fields.addView(help);
        EditText directory = new EditText(this); directory.setSingleLine(true); directory.setHint("Pasta Data");
        directory.setText(dataDirectory == null ? "" : dataDirectory.getPath()); fields.addView(directory);
        EditText realmlist = new EditText(this); realmlist.setSingleLine(true); realmlist.setHint("realmlist.wtf (vazio = automático)");
        realmlist.setText(getPreferences(MODE_PRIVATE).getString("realmlistPath", "")); fields.addView(realmlist);
        android.widget.CheckBox scenes = new android.widget.CheckBox(this);
        scenes.setText("Cenários 3D nas telas de login e personagens");
        scenes.setChecked(!scenesDisabled); fields.addView(scenes);
        android.widget.CheckBox musicBox = new android.widget.CheckBox(this);
        musicBox.setText("Música das telas de login e personagens");
        musicBox.setChecked(!musicDisabled); fields.addView(musicBox);
        android.widget.CheckBox uiBox = new android.widget.CheckBox(this);
        uiBox.setText("Interface original do WoW no mundo (FrameXML, beta)");
        uiBox.setChecked(!originalUiDisabled); fields.addView(uiBox);
        Runnable save = () -> {
            String value = directory.getText().toString().trim();
            dataDirectory = value.isEmpty() ? null : new File(value);
            scenesDisabled = !scenes.isChecked();
            musicDisabled = !musicBox.isChecked();
            originalUiDisabled = !uiBox.isChecked();
            setOriginalUi(!originalUiDisabled);
            getPreferences(MODE_PRIVATE).edit().putBoolean("originalUiDisabled", originalUiDisabled).apply();
            getPreferences(MODE_PRIVATE).edit().putBoolean("musicDisabled", musicDisabled).apply();
            if (musicDisabled) stopMusic(); else if (loadedGlueRoot != null) { if (music == null) startMusic(loadedGlueRoot); else resumeMusic(); }
            getPreferences(MODE_PRIVATE).edit().putString("dataPath", value)
                .putString("realmlistPath", realmlist.getText().toString().trim())
                .putBoolean("scenesDisabled", scenesDisabled).apply();
            if (scenesDisabled) { clearGlueScene(); sceneLoaded = false; glueScreen.setScene3d(false); }
            sceneKey = "";
        };
        new AlertDialog.Builder(this).setTitle("Configuração").setView(fields)
            .setPositiveButton("Carregar interface", (dialog, which) -> { save.run(); loadGlue(); })
            .setNeutralButton("Permitir arquivos", (dialog, which) -> { save.run(); requestDataAccess(); })
            .setNegativeButton(inWorld ? "Sair do mundo" : "Fechar", (dialog, which) -> {
                if (inWorld) network.execute(() -> showNetwork(networkDisconnect()));
            }).show();
    }

    private void login(String user, char[] secret) {
        if (loginPreparing) { Arrays.fill(secret, '\0'); return; }
        final String root = readableDataRoot();
        if (root == null) { Arrays.fill(secret, '\0'); glueScreen.loginFailed("Configure a pasta Data."); return; }
        final String selected = getPreferences(MODE_PRIVATE).getString("realmlistPath", "");
        loginPreparing = true; glueScreen.beginLogin();
        network.execute(() -> {
            try {
                if (isFinishing() || isDestroyed()) return;
                RealmlistConfig.Target target = RealmlistConfig.load(new File(root), selected);
                final String response = networkConnect(target.endpoint, user, secret);
                runOnUiThread(() -> { loginPreparing = false; showNetwork(response); });
            } catch (Exception error) {
                final String disconnected = networkDisconnect();
                final String message = "Não foi possível usar o realmlist.wtf: " + error.getMessage();
                runOnUiThread(() -> {
                    if (isDestroyed()) return;
                    loginPreparing = false; showNetwork(disconnected); glueScreen.loginFailed(message);
                });
            } finally { Arrays.fill(secret, '\0'); }
        });
    }

    private void showNetwork(String text) {
        runOnUiThread(() -> {
            if (isDestroyed() || text == null || loginPreparing) return;
            try {
                JSONObject result = new JSONObject(text);
                int nextStage = result.getInt("stage");
                if (nextStage == 4) {
                    looks = result.optJSONArray("looks") == null ? new org.json.JSONArray() : result.getJSONArray("looks");
                    expansion = result.optInt("expansion", 2);
                }
                long serial = result.optLong("resultSerial", 0);
                boolean created = serial != resultSerial && resultSerial >= 0 && result.optInt("result") == 47;
                resultSerial = serial;
                stage = nextStage;
                glueScreen.networkState(result);
                if (created && glueScreen.creating()) { glueScreen.closeCreator(); sceneKey = ""; }
                if (!inWorld) updateScene();
                if (result.getInt("stage") == 6 && result.has("world"))
                    handleWorld(result.getJSONObject("world"), result.getString("message"));
                else if (inWorld) leaveWorld();
            } catch (Exception error) { glueScreen.status("Resposta inválida da conexão nativa."); }
        });
    }

    // ---- In-world view ---------------------------------------------------------------------

    private void handleWorld(JSONObject world, String message) throws Exception {
        long serial = world.getLong("serial"), loaded = world.getLong("loaded"), adopted = world.getLong("adopted");
        if (worldRoot == null) worldRoot = readableDataRoot();
        if (worldRoot != null && (serial > loaded || world.optInt("work") > 0) && !worldLoading) {
            worldLoading = true; final String root = worldRoot;
            worker.execute(() -> {
                final String result = loadWorldTerrain(root);
                runOnUiThread(() -> { worldLoading = false; if (result != null && !inWorld) glueScreen.status(result); });
            });
        }
        if (adopted > 0 && !inWorld) enterWorld();
        if (inWorld) {
            String uiInfo = originalUiInfo();
            worldStatusText = fps + " FPS · " + message + (uiInfo == null ? "" : "\n" + uiInfo);
            if (lastUiState == 1 && uiInfo != null) {
                // Original UI running: one short line, the rest in the log (tap) so the HUD does not
                // cover the interface (world map title, dropdowns, close buttons).
                String short_ = uiInfo.contains("erro") ? uiInfo.replaceAll("·.*", "").replace("UI original:", "UI:").trim() : "UI ok";
                worldHud.setText(fps + " FPS · " + short_ + " ⓘ");
            } else
                worldHud.setText(worldStatusText + (uiInfo == null ? "" : " (toque aqui: log)"));
            controls.setPlayer(world.optString("name"), world.optInt("level"), world.optInt("hp"),
                world.optInt("maxhp"), world.optInt("power"), world.optInt("maxpower"), world.optInt("powertype"));
        }
    }

    private static String readCrashReport(File file) {
        if (!file.isFile()) return null;
        try {
            byte[] bytes = java.nio.file.Files.readAllBytes(file.toPath());
            return new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
        } catch (Exception error) {
            return null;
        } finally {
            file.delete();
        }
    }
    private void showCrashReport(String text) {
        TextView body = new TextView(this);
        String version = "?";
        try { version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName; } catch (Exception ignored) {}
        body.setText(text + "\nVersão: " + version);
        body.setTextIsSelectable(true);
        body.setTextSize(11);
        body.setPadding(32, 16, 32, 16);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.addView(body);
        String note = crashSkippedFile != null
            ? "A interface original vai pular o arquivo " + crashSkippedFile + " na próxima vez. Envie este relatório:"
            : originalUiDisabled && text.contains("Última etapa: UI")
                ? "A interface original foi desligada (Configuração). Envie este relatório:" : "Envie este relatório:";
        new AlertDialog.Builder(this).setTitle("O jogo fechou na última vez")
            .setMessage(note)
            .setView(scroll)
            .setPositiveButton("Copiar", (dialog, which) -> {
                android.content.ClipboardManager clipboard = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(android.content.ClipData.newPlainText("crash", body.getText()));
            })
            .setNegativeButton("Fechar", null).show();
    }

    private void showUiLog() {
        String text = originalUiLog();
        if (text == null || text.isEmpty()) text = "(vazio)";
        TextView body = new TextView(this);
        body.setText(worldStatusText + "\n\n" + originalUiInfo() + "\n\n" + text);
        body.setTextIsSelectable(true);
        body.setTextSize(10);
        body.setPadding(32, 16, 32, 16);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        Button export = new Button(this);
        export.setText("Exportar arquivos de teste");
        export.setOnClickListener(v -> exportTestFiles(export));
        content.addView(export);
        content.addView(body);
        scroll.addView(content);
        new AlertDialog.Builder(this).setTitle("Log da interface original").setView(scroll)
            .setPositiveButton("Copiar", (dialog, which) -> {
                android.content.ClipboardManager clipboard = (android.content.ClipboardManager) getSystemService(CLIPBOARD_SERVICE);
                clipboard.setPrimaryClip(android.content.ClipData.newPlainText("ui-log", body.getText()));
            })
            .setNeutralButton(originalUiDisabled ? "Ligar UI" : "Desligar UI", (dialog, which) -> {
                originalUiDisabled = !originalUiDisabled;
                setOriginalUi(!originalUiDisabled);
                getPreferences(MODE_PRIVATE).edit().putBoolean("originalUiDisabled", originalUiDisabled).apply();
            })
            .setNegativeButton("Fechar", null).show();
    }

    /** Zips the game files this run opened (models, textures, DBCs, UI) in ~25 MB parts so the
     *  developer can reproduce what the phone shows; sounds and music are left out. */
    private void exportTestFiles(Button button) {
        button.setEnabled(false);
        button.setText("Exportando…");
        new Thread(() -> {
            String result;
            try {
                String[] names = openedArchiveFiles();
                if (names == null || names.length == 0) throw new java.io.IOException("nenhum arquivo lido ainda; entre no mundo antes");
                java.io.File base = null;
                String root = dataDirectory != null ? dataDirectory.getAbsolutePath() : null;
                java.util.List<java.io.File> candidates = new java.util.ArrayList<>();
                if (root != null && new java.io.File(root).getParentFile() != null)
                    candidates.add(new java.io.File(new java.io.File(root).getParentFile(), "teste-port"));
                candidates.add(new java.io.File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), "teste-port"));
                java.io.File own = getExternalFilesDir(null);
                if (own != null) candidates.add(new java.io.File(own, "teste-port"));
                for (java.io.File dir : candidates) {
                    dir.mkdirs();
                    java.io.File probe = new java.io.File(dir, ".probe");
                    try (java.io.FileOutputStream out = new java.io.FileOutputStream(probe)) { out.write(0); base = dir; probe.delete(); break; }
                    catch (java.io.IOException ignored) { }
                }
                if (base == null) throw new java.io.IOException("sem permissão de escrita");
                java.io.File[] old = base.listFiles();
                if (old != null) for (java.io.File f : old) if (f.getName().startsWith("parte-")) f.delete();
                final long partLimit = 25L * 1024 * 1024;
                int part = 0, files = 0;
                java.util.zip.ZipOutputStream zip = null;
                java.io.FileOutputStream raw = null;
                StringBuilder list = new StringBuilder();
                for (String name : names) {
                    String lower = name.toLowerCase(java.util.Locale.ROOT);
                    if (lower.startsWith("sound\\") || lower.startsWith("music\\") || lower.endsWith(".mp3") ||
                        lower.endsWith(".wav") || lower.endsWith(".ogg") || lower.endsWith(".avi")) continue;
                    byte[] bytes = readArchiveFile(name);
                    if (bytes == null) continue;
                    if (zip == null || raw.getChannel().position() > partLimit) {
                        if (zip != null) zip.close();
                        ++part;
                        raw = new java.io.FileOutputStream(new java.io.File(base, String.format(java.util.Locale.ROOT, "parte-%02d.zip", part)));
                        zip = new java.util.zip.ZipOutputStream(raw);
                        zip.setLevel(9);
                    }
                    zip.putNextEntry(new java.util.zip.ZipEntry(name.replace('\\', '/')));
                    zip.write(bytes);
                    zip.closeEntry();
                    ++files;
                    list.append(name).append('\n');
                }
                if (zip != null) {
                    zip.putNextEntry(new java.util.zip.ZipEntry("arquivos.txt"));
                    zip.write(list.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
                    zip.closeEntry();
                    zip.close();
                }
                result = files + " arquivos em " + part + " parte(s) em\n" + base.getAbsolutePath();
            } catch (Exception error) {
                result = "Falhou: " + error.getMessage();
            }
            final String message = result;
            runOnUiThread(() -> {
                button.setEnabled(true);
                button.setText("Exportar arquivos de teste");
                new AlertDialog.Builder(this).setTitle("Arquivos de teste").setMessage(message)
                    .setPositiveButton("OK", null).show();
            });
        }, "export-test-files").start();
    }

    private int lastUiState = -1;
    private final android.os.Handler uiGuardHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private void uiStateChanged(int state) {
        if (state == lastUiState) return;
        lastUiState = state;
        // With the original UI, leaving is the game menu (Back → Logout) or the status log.
        if (inWorld) worldExit.setVisibility(state == 1 ? View.GONE : View.VISIBLE);
        if (state == 2) {
            getPreferences(MODE_PRIVATE).edit().putBoolean("uiCrashGuard", false).commit();
            glueScreen.status("A interface original não iniciou; usando a interface simples.");
        }
    }
    private void enterWorld() {
        inWorld = true;
        if (!originalUiDisabled) {
            getPreferences(MODE_PRIVATE).edit().putBoolean("uiCrashGuard", true).commit();
            uiGuardHandler.postDelayed(() -> getPreferences(MODE_PRIVATE).edit().putBoolean("uiCrashGuard", false).commit(), 30000);
        }
        pauseMusic();
        clearGlueScene(); sceneKey = "";
        glueScreen.clearPassword(); glueScreen.hideKeyboard(); glueScreen.setVisibility(View.GONE);
        controls.setVisibility(View.VISIBLE);
        controls.loadNameFont(getCacheDir(), MainActivity::readArchiveFile);
        worldHud.setVisibility(View.VISIBLE); worldExit.setVisibility(lastUiState == 1 ? View.GONE : View.VISIBLE);
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        if (surface.getWidth() > 0 && surface.getHeight() > 0)
            surface.getHolder().setFixedSize(Math.max(1, (int)(surface.getWidth() * WORLD_RENDER_SCALE)),
                Math.max(1, (int)(surface.getHeight() * WORLD_RENDER_SCALE)));
        inputFocus(hasWindowFocus());
    }

    private void leaveWorld() {
        inWorld = false; worldLoading = false; worldRoot = null;
        if (gameAudio != null) gameAudio.stopAll();
        surface.getHolder().setSizeFromLayout();
        clearWorldScene(); sceneKey = "";
        showGlue();
        resumeMusic();
        updateScene();
    }

    // ---- Storage access --------------------------------------------------------------------

    private boolean hasDataAccess() {
        return Build.VERSION.SDK_INT >= 30 ? Environment.isExternalStorageManager()
            : checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestDataAccess() {
        if (hasDataAccess()) { loadGlue(); return; }
        if (Build.VERSION.SDK_INT >= 30) {
            waitingForStorage = true;
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
            } catch (ActivityNotFoundException | SecurityException error) {
                try { startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)); }
                catch (ActivityNotFoundException | SecurityException missing) {
                    waitingForStorage = false;
                    glueScreen.loadFailed("Permita o acesso a todos os arquivos para este app nas configurações do Android.");
                }
            }
        } else requestPermissions(new String[]{Manifest.permission.READ_EXTERNAL_STORAGE}, 410);
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(code, permissions, results);
        if (code != 410) return;
        if (hasDataAccess()) loadGlue();
        else glueScreen.loadFailed("Leitura não autorizada. Toque em Configuração > Permitir arquivos.");
    }

    private String readableDataRoot() {
        String failure = null;
        if (!hasDataAccess()) failure = "Permita a leitura dos arquivos em Configuração > Permitir arquivos.";
        else if (dataDirectory == null || !dataDirectory.isAbsolute()) failure = "Informe o caminho completo da pasta Data em Configuração.";
        else if (!dataDirectory.isDirectory()) failure = "Pasta não encontrada: " + dataDirectory + "\nInforme a pasta Data, não um arquivo .MPQ.";
        else if (!dataDirectory.canRead()) failure = "O Android não liberou a leitura desta pasta: " + dataDirectory;
        if (failure == null) return dataDirectory.getAbsolutePath();
        glueScreen.loadFailed(failure);
        return null;
    }

    // ---- Lifecycle -------------------------------------------------------------------------

    @Override public void onBackPressed() {
        if (inWorld && lastUiState == 1) { uiBackPressed(); return; } // Escape: close windows / game menu
        super.onBackPressed();
    }
    @Override protected void onResume() {
        super.onResume(); surface.onResume();
        resumeMusic();
        if (gameAudio != null) gameAudio.resume();
        if (waitingForStorage) waitingForStorage = false;
        boolean access = hasDataAccess();
        glueScreen.needsPermission(!access);
        if (loadedGlueRoot == null && !glueLoading && access) loadGlue();
        else if (!access) {
            glueScreen.loadFailed("O app precisa ler a pasta Data do WoW. Toque em \"Permitir acesso aos arquivos\" e ative a opção para este app.");
            // Ask once per launch, like the first screen of the previous versions.
            if (!askedPermission) { askedPermission = true; surface.post(this::requestDataAccess); }
        }
    }
    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (!focused) controls.cancelTouches();
        inputFocus(focused && inWorld);
    }
    @Override protected void onPause() {
        glueScreen.clearPassword();
        network.execute(() -> showNetwork(networkDisconnect()));
        controls.cancelTouches();
        inputFocus(false);
        pauseMusic();
        if (gameAudio != null) gameAudio.pause();
        surface.onPause();
        super.onPause();
    }
    @Override protected void onDestroy() { stopMusic(); worker.shutdown(); network.shutdown(); super.onDestroy(); }
}
