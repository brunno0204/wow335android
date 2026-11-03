package org.wowdecomp.android;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.SoundPool;
import android.os.Handler;
import android.os.Looper;
import java.io.File;
import java.io.FileOutputStream;
import java.util.HashMap;
import java.util.Map;
import java.util.Random;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Game sounds from the MPQs: zone music (random file of the set, then the set's silence
 * interval), looping ambience and one-shot interface/spell sounds. Files are extracted once
 * into the cache directory. Commands come from the Lua game layer (__AndroidAudio).
 */
final class GameAudio {
    interface Reader { byte[] read(String archivePath); }
    private final File cache;
    private final Reader reader;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Random random = new Random();
    private final Map<String, File> files = new HashMap<>();
    private final Map<String, Integer> soundIds = new HashMap<>();
    private SoundPool pool;
    private MediaPlayer music, ambience;
    private String[] musicSet = new String[0];
    private int silenceMin, silenceMax, musicSerial;
    private float master = 1, musicVolume = .4f, sfxVolume = 1, ambienceVolume = .6f, musicBase = 1, ambienceBase = 1;
    private boolean enabled = true, paused;

    GameAudio(File cacheDir, Reader reader) {
        this.cache = new File(cacheDir, "sound");
        this.reader = reader;
        //noinspection ResultOfMethodCallIgnored
        cache.mkdirs();
        AudioAttributes attributes = new AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_GAME).setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build();
        pool = new SoundPool.Builder().setMaxStreams(12).setAudioAttributes(attributes).build();
        pool.setOnLoadCompleteListener((p, id, status) -> {
            if (status == 0 && enabled && !paused) {
                Float volume = pendingVolumes.remove(id);
                if (volume != null) p.play(id, volume, volume, 1, 0, 1);
            }
        });
    }
    private final Map<Integer, Float> pendingVolumes = new HashMap<>();

    /** Lines from pollAudio(); called on the main thread. */
    void handle(String commands) {
        if (commands == null || commands.isEmpty()) return;
        for (String line : commands.split("\n")) {
            String[] f = line.split("\\|", -1);
            if (f.length == 0) continue;
            try {
                switch (f[0]) {
                    case "volume":
                        if (f.length >= 6) {
                            master = Float.parseFloat(f[1]); musicVolume = Float.parseFloat(f[2]);
                            sfxVolume = Float.parseFloat(f[3]); ambienceVolume = Float.parseFloat(f[4]);
                            enabled = !f[5].equals("0");
                            applyVolumes();
                        }
                        break;
                    case "music":
                        if (f.length >= 3) {
                            musicBase = Float.parseFloat(f[1]);
                            musicSet = f[2].split(";");
                            silenceMin = f.length > 3 ? parse(f[3]) : 0;
                            silenceMax = f.length > 4 ? parse(f[4]) : 0;
                            startMusic(++musicSerial, 0);
                        }
                        break;
                    case "ambience":
                        if (f.length >= 3) { ambienceBase = Float.parseFloat(f[1]); startAmbience(f[2]); }
                        break;
                    case "sound":
                        if (f.length >= 3) playSound(Float.parseFloat(f[1]), f[2]);
                        break;
                    case "stop":
                        if (f.length >= 2 && f[1].equals("music")) { ++musicSerial; musicSet = new String[0]; release(true); }
                        if (f.length >= 2 && f[1].equals("ambience")) release(false);
                        break;
                    default:
                        break;
                }
            } catch (NumberFormatException ignored) {
            }
        }
    }
    private static int parse(String text) { try { return Integer.parseInt(text.trim()); } catch (NumberFormatException e) { return 0; } }

    private interface FileReady { void run(File file); }
    /** Extracts the archive file once (io thread), then continues on the main thread. */
    private void withFile(String path, FileReady then) {
        String key = path.toLowerCase().replace('/', '\\');
        File known = files.get(key);
        if (known != null) { then.run(known); return; }
        io.execute(() -> {
            String name = Integer.toHexString(key.hashCode()) + "_" + key.substring(key.lastIndexOf('\\') + 1).replaceAll("[^a-z0-9._-]", "_");
            File out = new File(cache, name);
            if (!out.exists()) {
                byte[] bytes = reader.read(path);
                if (bytes == null || bytes.length == 0) return;
                File tmp = new File(cache, name + ".tmp");
                try (FileOutputStream stream = new FileOutputStream(tmp)) {
                    stream.write(bytes);
                } catch (Exception e) {
                    return;
                }
                //noinspection ResultOfMethodCallIgnored
                tmp.renameTo(out);
            }
            main.post(() -> { files.put(key, out); then.run(out); });
        });
    }
    private float level(float base, float channel) { return enabled ? Math.max(0, Math.min(1, master * channel * base)) : 0; }
    private void applyVolumes() {
        if (music != null) { float v = level(musicBase, musicVolume); music.setVolume(v, v); }
        if (ambience != null) { float v = level(ambienceBase, ambienceVolume); ambience.setVolume(v, v); }
    }
    private MediaPlayer player(File file, boolean loop, float volume) {
        try {
            MediaPlayer p = new MediaPlayer();
            p.setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build());
            p.setDataSource(file.getAbsolutePath());
            p.setLooping(loop);
            p.setVolume(volume, volume);
            p.prepare();
            return p;
        } catch (Exception e) {
            return null;
        }
    }
    private void release(boolean isMusic) {
        MediaPlayer p = isMusic ? music : ambience;
        if (p != null) { try { p.release(); } catch (Exception ignored) { } }
        if (isMusic) music = null; else ambience = null;
    }
    private void startMusic(int serial, long delayMs) {
        if (musicSet.length == 0) return;
        main.postDelayed(() -> {
            if (serial != musicSerial || musicSet.length == 0) return;
            String path = musicSet[random.nextInt(musicSet.length)];
            withFile(path, file -> {
                if (serial != musicSerial) return;
                release(true);
                music = player(file, false, level(musicBase, musicVolume));
                if (music == null) return;
                music.setOnCompletionListener(p -> {
                    // The set's silence interval (ms) before the next piece.
                    int span = Math.max(0, silenceMax - silenceMin);
                    startMusic(serial, Math.max(2000, silenceMin + (span > 0 ? random.nextInt(span) : 0)));
                });
                if (!paused) music.start();
            });
        }, delayMs);
    }
    private void startAmbience(String path) {
        withFile(path, file -> {
            release(false);
            ambience = player(file, true, level(ambienceBase, ambienceVolume));
            if (ambience != null && !paused) ambience.start();
        });
    }
    private void playSound(float volume, String path) {
        if (!enabled || paused || sfxVolume <= 0 || master <= 0) return;
        withFile(path, file -> {
            float v = Math.max(0, Math.min(1, master * sfxVolume * volume));
            Integer id = soundIds.get(file.getName());
            if (id != null) { pool.play(id, v, v, 1, 0, 1); return; }
            int loaded = pool.load(file.getAbsolutePath(), 1);
            soundIds.put(file.getName(), loaded);
            pendingVolumes.put(loaded, v);
        });
    }
    void pause() {
        paused = true;
        if (music != null && music.isPlaying()) music.pause();
        if (ambience != null && ambience.isPlaying()) ambience.pause();
    }
    void resume() {
        paused = false;
        if (music != null) music.start();
        if (ambience != null) ambience.start();
    }
    /** Leaving the world: silence. */
    void stopAll() {
        ++musicSerial;
        musicSet = new String[0];
        release(true);
        release(false);
    }
}
