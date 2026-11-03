package org.wowdecomp.android;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Typeface;

import java.io.File;
import java.io.FileOutputStream;
import java.util.*;

/** Reads the user's MPQs at runtime; no Blizzard art or fonts are bundled in the APK. */
final class GlueAssets {
    interface Reader { byte[] read(String logicalPath); }
    final GlueLayout layout = new GlueLayout();
    final Map<String, Bitmap> textures = new HashMap<>();
    Typeface font = Typeface.SERIF;
    final List<String> missing = new ArrayList<>();
    static final String COMMON = "Interface/Glues/Common/";

    static GlueAssets load(Context context, Reader reader) throws Exception {
        GlueAssets assets = new GlueAssets();
        for (String file :
                new String[] {"GlueButtons", "GlueTemplates", "AccountLogin", "CharacterSelect"}) {
            assets.layout.add(reader.read("Interface/GlueXML/" + file + ".xml"));
        }
        for (String name :
                new String[] {
                    "AccountLoginAccountEdit",
                    "AccountLoginPasswordEdit",
                    "AccountLoginLoginButton",
                    "CharSelectEnterWorldButton",
                    "CharSelectCharacterButton1",
                    "CharacterSelectBackButton"
                }) {
            if (!assets.layout.named.containsKey(name))
                throw new IllegalArgumentException("Layout incompatível: " + name);
        }
        String[] geometry = {
            "AccountLoginAccountEdit",
            "AccountLoginPasswordEdit",
            "AccountLoginLoginButton",
            "AccountLoginExitButton",
            "AccountLoginLogo",
            "CharacterSelectLogo",
            "CharacterSelectCharacterFrame",
            "CharSelectRealmName",
            "CharSelectChangeRealmButton",
            "CharSelectEnterWorldButton",
            "CharacterSelectBackButton"
        };
        for (float width : new float[] {1024, 1804}) {
            for (String name : geometry) assets.validate(name, width);
            for (int i = 1; i <= 10; i++) assets.validate("CharSelectCharacterButton" + i, width);
        }
        String[] paths = {
            COMMON + "Glues-WoW-Logo",
            COMMON + "Glues-WoW-WotLKLogo",
            "Interface/Glues/Mainmenu/Glues-BlizzardLogo",
            COMMON + "Glues-BigButton-Up",
            COMMON + "Glues-BigButton-Down",
            COMMON + "Glues-EditBox-Background",
            COMMON + "Glues-BigButton-Highlight",
            COMMON + "Glues-BigButton-Glow",
            "Interface/Glues/CharacterSelect/Glue-CharacterSelect-Highlight",
            "Interface/Glues/Common/Glue-Tooltip-Border",
            "Interface/Tooltips/UI-Tooltip-Background",
            "Interface/Buttons/UI-CheckBox-Up",
            "Interface/Buttons/UI-CheckBox-Check",
            GlueScreenView.RACES_TEXTURE,
            GlueScreenView.CLASSES_TEXTURE
        };
        long pixels = 0;
        for (String path : paths) {
            byte[] data = reader.read(path + ".blp");
            if (data == null) {
                assets.missing.add(path);
                continue;
            }
            GlueBlp.Image image = GlueBlp.decode(data);
            pixels += (long) image.width * image.height;
            if (pixels > 8 * 1024 * 1024)
                throw new IllegalArgumentException("Limite de texturas Glue");
            Bitmap bitmap =
                    Bitmap.createBitmap(
                            image.argb, image.width, image.height, Bitmap.Config.ARGB_8888);
            assets.textures.put(key(path), bitmap);
        }
        // The primary UI cannot silently degrade to generic Android buttons.
        if (!assets.textures.containsKey(key(COMMON + "Glues-BigButton-Up")))
            throw new IllegalArgumentException(
                    "Textura original dos botões não encontrada nos MPQs");
        byte[] font = reader.read("Fonts/FRIZQT__.TTF");
        if (font != null && font.length <= 2 * 1024 * 1024) {
            File temp = File.createTempFile("glue-font-", ".ttf", context.getCacheDir());
            try {
                try (FileOutputStream out = new FileOutputStream(temp)) {
                    out.write(font);
                }
                assets.font = Typeface.createFromFile(temp);
            } finally {
                temp.delete();
            }
        } else assets.missing.add("Fonts/FRIZQT__.TTF");
        return assets;
    }

    private void validate(String name, float width) {
        if (!layout.named.containsKey(name))
            throw new IllegalArgumentException("GlueXML não contém " + name);
        GlueLayout.Box b = layout.box(name, width, 768);
        for (float value : new float[] {b.x, b.y, b.w, b.h})
            if (!Float.isFinite(value) || Math.abs(value) > 16384)
                throw new IllegalArgumentException("Geometria GlueXML inválida");
    }

    static String key(String path) {
        return path.replace('\\', '/').toLowerCase(Locale.ROOT).replaceAll("\\.blp$", "");
    }

    Bitmap texture(String path) {
        return textures.get(key(path));
    }
}
