package org.wowdecomp.android;

import android.content.Context;
import android.graphics.*;
import android.text.InputFilter;
import android.text.InputType;
import android.util.TypedValue;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.*;

import org.json.*;

import java.util.*;
import java.util.function.Supplier;

/**
 * Original MPQ art and XML geometry, with explicit native bindings for the supported flow. This is
 * not a full FrameXML/Lua runtime. The 3D ModelFFX scenes are drawn natively behind this view.
 */
final class GlueScreenView extends FrameLayout {
    interface Actions {
        void login(String account, char[] password);

        void realm(int index);

        void enter(int index);

        void disconnect();

        void settings();

        void permission();

        void exit();

        void selection(int index);

        void rotate(float dx);

        void openCreator();

        void creator(int op, int arg, int arg2);

        void create(String name);

        void delete(int index);

        void closeCreator();
    }

    private final Actions actions;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Map<View, Supplier<GlueLayout.Box>> placed = new LinkedHashMap<>();
    private GlueAssets assets;
    private EditText account, password;
    private int stage = 0, selected = -1;
    private String message = "Carregue a interface original da sua pasta Data.", realmName = "";
    private final List<String> realms = new ArrayList<>(), characters = new ArrayList<>();
    private boolean loading = false;
    private float scale = 1, logicalWidth = 1024;
    private static final int GOLD = 0xffffd100;
    private int presentation = -1;
    private boolean scene3d, creating, needsPermission;
    private JSONObject creatorState;
    private EditText nameEdit;
    private String pendingName;
    private float dragX = Float.NaN;
    private String lastNetworkMessage = "";
    // GlueXML CharacterCreate.lua RACE_ICON_TCOORDS / CLASS_ICON_TCOORDS.
    private static final Map<String, float[]> RACE_ICONS = new HashMap<>();
    private static final Map<String, float[]> CLASS_ICONS = new HashMap<>();
    static {
        String[] races = {"HUMAN", "DWARF", "GNOME", "NIGHTELF", "TAUREN", "SCOURGE", "TROLL", "ORC"};
        for (int i = 0; i < 8; i++) {
            float l = (i % 4) * .125f, t = (i / 4) * .25f;
            RACE_ICONS.put(races[i] + "_MALE", new float[] {l, l + .125f, t, t + .25f});
            RACE_ICONS.put(races[i] + "_FEMALE", new float[] {l, l + .125f, t + .5f, t + .75f});
        }
        RACE_ICONS.put("DRAENEI_MALE", new float[] {.5f, .625f, 0, .25f});
        RACE_ICONS.put("BLOODELF_MALE", new float[] {.5f, .625f, .25f, .5f});
        RACE_ICONS.put("DRAENEI_FEMALE", new float[] {.5f, .625f, .5f, .75f});
        RACE_ICONS.put("BLOODELF_FEMALE", new float[] {.5f, .625f, .75f, 1});
        CLASS_ICONS.put("WARRIOR", new float[] {0, .25f, 0, .25f});
        CLASS_ICONS.put("MAGE", new float[] {.25f, .49609375f, 0, .25f});
        CLASS_ICONS.put("ROGUE", new float[] {.49609375f, .7421875f, 0, .25f});
        CLASS_ICONS.put("DRUID", new float[] {.7421875f, .98828125f, 0, .25f});
        CLASS_ICONS.put("HUNTER", new float[] {0, .25f, .25f, .5f});
        CLASS_ICONS.put("SHAMAN", new float[] {.25f, .49609375f, .25f, .5f});
        CLASS_ICONS.put("PRIEST", new float[] {.49609375f, .7421875f, .25f, .5f});
        CLASS_ICONS.put("WARLOCK", new float[] {.7421875f, .98828125f, .25f, .5f});
        CLASS_ICONS.put("PALADIN", new float[] {0, .25f, .5f, .75f});
        CLASS_ICONS.put("DEATHKNIGHT", new float[] {.25f, .49609375f, .5f, .75f});
    }
    static final String RACES_TEXTURE = "Interface/Glues/CharacterCreate/UI-CharacterCreate-Races";
    static final String CLASSES_TEXTURE = "Interface/Glues/CharacterCreate/UI-CharacterCreate-Classes";

    GlueScreenView(Context context, Actions actions) {
        super(context);
        this.actions = actions;
        setWillNotDraw(false);
        setBackgroundColor(0xff080b11);
        setFocusableInTouchMode(true);
        setClipChildren(false);
        rebuild();
    }

    void loading(String text) {
        loading = true;
        message = text;
        rebuild();
    }

    void loadFailed(String text) {
        loading = false;
        message = text;
        rebuild();
    }

    void loaded(GlueAssets value) {
        assets = value;
        loading = false;
        message = "";
        rebuild();
    }

    void beginLogin() {
        stage = 1;
        message = "Conectando ao servidor…";
        rebuild();
    }

    void loginFailed(String text) {
        stage = 0;
        message = text;
        rebuild();
    }

    void clearPassword() {
        if (password != null) password.setText("");
    }

    void status(String text) {
        message = text;
        invalidate();
    }

    /** No storage access yet: show the permission button instead of the Data setup. */
    void needsPermission(boolean value) {
        if (needsPermission == value) return;
        needsPermission = value;
        rebuild();
    }

    /** True once a native ModelFFX scene is visible behind this view. */
    void setScene3d(boolean value) {
        if (scene3d == value) return;
        scene3d = value;
        setBackgroundColor(value ? Color.TRANSPARENT : 0xff080b11);
        invalidate();
    }

    void openCreator() {
        creating = true;
        creatorState = null;
        pendingName = null;
        message = "Carregando criação de personagem…";
        rebuild();
    }

    void closeCreator() {
        creating = false;
        creatorState = null;
        hideKeyboard();
        rebuild();
        actions.selection(selected);
    }

    boolean creating() {
        return creating;
    }

    void creatorState(JSONObject state) {
        if (!creating) return;
        String typed = nameEdit == null ? pendingName : nameEdit.getText().toString();
        creatorState = state;
        String suggested = state.optString("name", "");
        pendingName = !suggested.isEmpty() ? suggested : typed;
        String status = state.optString("status", state.optString("error", ""));
        message = status;
        rebuild();
    }

    void networkState(JSONObject state) throws JSONException {
        int next = state.getInt("stage");
        String nextMessage = state.getString("message");
        boolean changed = next != stage;
        // Idle polling must not erase asset-loading/validation errors; the creator keeps its
        // own status until the native connection reports something new.
        if ((next != 0 || stage != 0) && (!creating || !nextMessage.equals(lastNetworkMessage)))
            message = nextMessage;
        lastNetworkMessage = nextMessage;
        if (next == 2) {
            List<String> list = names(state.getJSONArray("realms"));
            changed |= !list.equals(realms);
            realms.clear();
            realms.addAll(list);
        }
        if (next == 4) {
            List<String> list = names(state.getJSONArray("characters"));
            boolean listChanged = !list.equals(characters);
            if (listChanged) {
                selected = list.isEmpty() ? -1 : 0;
                changed = true;
            }
            characters.clear();
            characters.addAll(list);
            if (listChanged || stage != 4) actions.selection(selected);
        }
        if (next == 0 || next == 7) {
            characters.clear();
            realms.clear();
            selected = -1;
            creating = false;
        }
        stage = next;
        int desired = screen();
        if (changed || desired != presentation) rebuild();
        else invalidate();
    }

    private static List<String> names(JSONArray array) throws JSONException {
        List<String> values = new ArrayList<>();
        for (int i = 0; i < Math.min(1024, array.length()); i++) values.add(array.getString(i));
        return values;
    }

    // Keep realm/character lists visible while their next request is pending.
    private int screen() {
        if (creating && (stage == 3 || stage == 4)) return 3;
        return stage >= 4 && stage <= 6 ? 2 : (stage == 2 || stage == 3 ? 1 : 0);
    }

    private boolean busy() {
        return loading || stage == 1 || stage == 3 || stage == 5 || stage == 6;
    }

    private GlueLayout.Box box(String name) {
        return assets.layout.box(name, logicalWidth, 768);
    }

    private void add(View v, Supplier<GlueLayout.Box> rect) {
        placed.put(v, rect);
        addView(v);
    }

    private void named(View v, String name) {
        add(v, () -> box(name));
    }

    private void rebuild() {
        if (nameEdit != null) pendingName = nameEdit.getText().toString();
        nameEdit = null;
        String user = account == null ? "" : account.getText().toString();
        // Rebuilds never copy passwords into immutable Java Strings.
        clearPassword();
        account = null;
        password = null;
        removeAllViews();
        placed.clear();
        presentation = screen();
        if (presentation != 3)
            add(
                    button("Configuração", actions::settings),
                    () -> new GlueLayout.Box(logicalWidth / 2 - 80, 95, 160, 48));
        if (assets != null && presentation == 3) {
            buildCreator();
        } else if (assets == null) {
            Button load =
                    needsPermission
                            ? button("Permitir acesso aos arquivos", actions::permission)
                            : button(loading ? "Carregando…" : "Configurar pasta Data", actions::settings);
            load.setEnabled(needsPermission || !loading);
            add(load, () -> new GlueLayout.Box(logicalWidth / 2 - 180, 380, 360, 64));
        } else if (presentation == 0) {
            account = edit(false);
            account.setText(user);
            named(account, "AccountLoginAccountEdit");
            password = edit(true);
            named(password, "AccountLoginPasswordEdit");
            password.setImeOptions(EditorInfo.IME_ACTION_GO | EditorInfo.IME_FLAG_NO_EXTRACT_UI);
            password.setOnEditorActionListener(
                    (v, id, event) -> {
                        if (id == EditorInfo.IME_ACTION_GO) {
                            submit();
                            return true;
                        }
                        return false;
                    });
            Button login = button("Conectar", this::submit);
            login.setEnabled(!busy());
            named(login, "AccountLoginLoginButton");
            account.setEnabled(!busy());
            password.setEnabled(!busy());
            named(button("Sair", actions::exit), "AccountLoginExitButton");
        } else if (presentation == 1) {
            ScrollView scroll = new ScrollView(getContext());
            LinearLayout rows = new LinearLayout(getContext());
            rows.setOrientation(LinearLayout.VERTICAL);
            for (int i = 0; i < realms.size(); i++) {
                final int index = i;
                Button row =
                        button(
                                realms.get(i),
                                () -> {
                                    realmName = realms.get(index);
                                    stage = 3;
                                    message = "Conectando ao reino…";
                                    rebuild();
                                    actions.realm(index);
                                });
                row.setEnabled(!busy());
                rows.addView(
                        row, new LinearLayout.LayoutParams(-1, Math.max(48, (int) (54 * scale))));
            }
            scroll.addView(rows);
            add(scroll, () -> new GlueLayout.Box(logicalWidth / 2 - 300, 230, 600, 340));
            add(
                    button("Cancelar", actions::disconnect),
                    () -> new GlueLayout.Box(logicalWidth / 2 - 100, 590, 200, 64));
        } else {
            for (int i = 0; i < characters.size(); i++) {
                final int index = i;
                OriginalButton row =
                        new OriginalButton(
                                characters.get(i),
                                () -> {
                                    if (selected == index) return;
                                    selected = index;
                                    rebuild();
                                    actions.selection(index);
                                },
                                true,
                                index == selected);
                row.setEnabled(stage == 4);
                named(row, "CharSelectCharacterButton" + (i + 1));
            }
            Button enter =
                    button(
                            "Entrar no mundo",
                            () -> {
                                if (selected < 0 || selected >= characters.size() || stage != 4)
                                    return;
                                stage = 5;
                                message = "Solicitando entrada do personagem…";
                                rebuild();
                                actions.enter(selected);
                            });
            enter.setEnabled(stage == 4 && selected >= 0);
            named(enter, "CharSelectEnterWorldButton");
            named(button("Voltar", actions::disconnect), "CharacterSelectBackButton");
            Button change =
                    button(
                            "Trocar reino",
                            () -> {
                                // The auth session is consumed on realm choice; a new login is
                                // required.
                                actions.disconnect();
                            });
            named(change, "CharSelectChangeRealmButton");
            Button create = button("Criar personagem", actions::openCreator);
            create.setEnabled(stage == 4 && characters.size() < 10);
            add(create, () -> {
                GlueLayout.Box f = box("CharacterSelectCharacterFrame");
                return new GlueLayout.Box(f.x + 20, f.y + f.h + 6, f.w - 40, 44);
            });
            Button delete = button("Apagar personagem", () -> {
                if (stage == 4 && selected >= 0 && selected < characters.size()) actions.delete(selected);
            });
            delete.setEnabled(stage == 4 && selected >= 0);
            add(delete, () -> {
                GlueLayout.Box b = box("CharacterSelectBackButton");
                return new GlueLayout.Box(b.x - b.w - 16, b.y, b.w, b.h);
            });
        }
        requestLayout();
        invalidate();
    }

    private static final String[] CUSTOM_LABELS = {"Pele", "Rosto", "Estilo de cabelo", "Cor do cabelo", ""};

    private static String featureLabel(String token) {
        switch (token.toUpperCase(Locale.ROOT)) {
            case "BEARD": return "Barba";
            case "EARRINGS": return "Brincos";
            case "FEATURES": return "Traços";
            case "HORNS": return "Chifres";
            case "PIERCINGS": return "Piercings";
            case "TUSKS": return "Presas";
            case "MARKINGS": return "Marcas";
            case "HAIR": return "Cabelo";
            case "NONE": return "";
            default: return "Traços faciais";
        }
    }

    private void buildCreator() {
        final JSONObject state = creatorState;
        final float lw = logicalWidth;
        boolean ready = state != null && state.has("races");
        if (ready) {
            try {
                JSONArray look = state.getJSONArray("look");
                final int race = look.getInt(0), cls = look.getInt(1), sex = look.getInt(2);
                JSONArray races = state.getJSONArray("races");
                int[] column = {0, 0};
                for (int i = 0; i < races.length(); i++) {
                    JSONObject r = races.getJSONObject(i);
                    final int id = r.getInt("id");
                    int faction = Math.max(0, Math.min(1, r.getInt("faction")));
                    final int row = column[faction]++;
                    String file = r.getString("file").toUpperCase(Locale.ROOT);
                    IconButton b = new IconButton(r.getString("name"), RACES_TEXTURE,
                            RACE_ICONS.get(file + (sex == 1 ? "_FEMALE" : "_MALE")), id == race,
                            () -> actions.creator(1, id, 0));
                    b.setEnabled(r.getBoolean("enabled"));
                    final float x = faction == 0 ? 16 : lw - 216;
                    add(b, () -> new GlueLayout.Box(x, 70 + row * 58, 200, 52));
                }
                JSONArray classes = state.getJSONArray("classes");
                final int count = classes.length();
                for (int i = 0; i < count; i++) {
                    JSONObject c = classes.getJSONObject(i);
                    final int id = c.getInt("id"), index = i;
                    IconButton b = new IconButton(c.getString("name"), CLASSES_TEXTURE,
                            CLASS_ICONS.get(c.getString("file").toUpperCase(Locale.ROOT)), id == cls,
                            () -> actions.creator(2, id, 0));
                    b.setEnabled(c.getBoolean("enabled"));
                    b.compact = true;
                    add(b, () -> new GlueLayout.Box(lw / 2 - count * 39 + index * 78, 590, 74, 74));
                }
                String[] sexes = {"Masculino", "Feminino"};
                for (int i = 0; i < 2; i++) {
                    final int value = i;
                    IconButton b = new IconButton(sexes[i], null, null, sex == i, () -> actions.creator(3, value, 0));
                    add(b, () -> new GlueLayout.Box(16 + value * 102, 370, 98, 40));
                }
                JSONArray counts = state.getJSONArray("counts");
                String facial = featureLabel(state.optString("facial", ""));
                int rowIndex = 0;
                for (int i = 0; i < 5; i++) {
                    String label = i == 4 ? facial : CUSTOM_LABELS[i];
                    if (label.isEmpty() || counts.getInt(i) <= 1) continue;
                    final int type = i, row = rowIndex++;
                    add(arrow("<", () -> actions.creator(4, type, -1)), () -> new GlueLayout.Box(16, 420 + row * 34, 34, 30));
                    add(arrow(">", () -> actions.creator(4, type, 1)), () -> new GlueLayout.Box(182, 420 + row * 34, 34, 30));
                    TextView text = new TextView(getContext());
                    text.setText(label);
                    text.setTextColor(GOLD);
                    text.setGravity(android.view.Gravity.CENTER);
                    text.setTypeface(assets.font);
                    text.setShadowLayer(2, 1, 1, Color.BLACK);
                    add(text, () -> new GlueLayout.Box(52, 420 + row * 34, 128, 30));
                }
                add(button("Aleatório", () -> actions.creator(5, 0, 0)), () -> new GlueLayout.Box(16, 600, 200, 44));
            } catch (JSONException error) {
                message = "Estado inválido da criação de personagem.";
            }
        }
        nameEdit = edit(false);
        nameEdit.setFilters(new InputFilter[] {new InputFilter.LengthFilter(24)});
        nameEdit.setHint("Nome");
        nameEdit.setHintTextColor(0xff888888);
        nameEdit.setGravity(android.view.Gravity.CENTER);
        if (pendingName != null) nameEdit.setText(pendingName);
        nameEdit.setEnabled(ready && stage == 4);
        add(nameEdit, () -> new GlueLayout.Box(lw / 2 - 130, 684, 260, 40));
        add(button("Nome aleatório", () -> actions.creator(6, 0, 0)), () -> new GlueLayout.Box(lw / 2 + 140, 682, 170, 44));
        Button accept = button("Criar personagem", () -> {
            if (nameEdit == null) return;
            String name = nameEdit.getText().toString().trim();
            if (name.isEmpty()) { status("Digite um nome."); return; }
            hideKeyboard();
            actions.create(name);
        });
        accept.setEnabled(ready && stage == 4);
        add(accept, () -> new GlueLayout.Box(lw - 236, 700, 220, 52));
        add(button("Voltar", actions::closeCreator), () -> new GlueLayout.Box(16, 700, 200, 52));
    }

    private Button arrow(String label, Runnable run) {
        Button b = button(label, run);
        return b;
    }

    /** Race/class/gender choice: original icon atlas region (if loaded) plus label. */
    private final class IconButton extends Button {
        private final String label, texture;
        private final float[] coords;
        private final boolean chosen;
        boolean compact;

        IconButton(String label, String texture, float[] coords, boolean chosen, Runnable action) {
            super(GlueScreenView.this.getContext());
            this.label = label;
            this.texture = texture;
            this.coords = coords;
            this.chosen = chosen;
            setBackgroundColor(Color.TRANSPARENT);
            setContentDescription(label);
            setPadding(0, 0, 0, 0);
            setMinWidth(0);
            setMinimumWidth(0);
            setMinHeight(0);
            setMinimumHeight(0);
            setSoundEffectsEnabled(false);
            setOnClickListener(v -> { if (!this.chosen) action.run(); });
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(chosen ? 0xc0402a0a : (isPressed() ? 0xa0301e08 : 0x90100c08));
            c.drawRoundRect(new RectF(0, 0, w, h), 6 * scale, 6 * scale, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth((chosen ? 3 : 1.5f) * scale);
            paint.setColor(chosen ? GOLD : 0xff6b5a35);
            c.drawRoundRect(new RectF(1, 1, w - 1, h - 1), 6 * scale, 6 * scale, paint);
            paint.setStyle(Paint.Style.FILL);
            float icon = compact ? Math.min(w, h) - 8 * scale : h - 8 * scale;
            Bitmap b = texture == null || assets == null ? null : assets.texture(texture);
            boolean drawn = false;
            if (b != null && coords != null) {
                Rect src = new Rect(Math.round(coords[0] * b.getWidth()), Math.round(coords[2] * b.getHeight()),
                        Math.round(coords[1] * b.getWidth()), Math.round(coords[3] * b.getHeight()));
                RectF dst = compact ? new RectF((w - icon) / 2, 4 * scale, (w + icon) / 2, 4 * scale + icon)
                        : new RectF(4 * scale, 4 * scale, 4 * scale + icon, 4 * scale + icon);
                paint.setAlpha(isEnabled() ? 255 : 90);
                c.drawBitmap(b, src, dst, paint);
                paint.setAlpha(255);
                drawn = true;
            }
            paint.setTypeface(assets == null ? Typeface.SERIF : assets.font);
            paint.setShadowLayer(2 * scale, scale, scale, Color.BLACK);
            paint.setColor(isEnabled() ? (chosen ? Color.WHITE : GOLD) : 0xff777777);
            if (compact) {
                paint.setTextSize(11 * scale);
                paint.setTextAlign(Paint.Align.CENTER);
                if (!drawn) c.drawText(fit(label, w - 4 * scale), w / 2, h / 2, paint);
            } else {
                paint.setTextSize(15 * scale);
                paint.setTextAlign(drawn ? Paint.Align.LEFT : Paint.Align.CENTER);
                float x = drawn ? icon + 10 * scale : w / 2;
                c.drawText(fit(label, w - x - 4 * scale), x, h / 2 - (paint.ascent() + paint.descent()) / 2, paint);
            }
            paint.clearShadowLayer();
        }
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent e) {
        // Drag on the empty scene rotates the 3D character (SetCharacterCreateFacing).
        if (presentation != 2 && presentation != 3) return super.onTouchEvent(e);
        switch (e.getActionMasked()) {
            case android.view.MotionEvent.ACTION_DOWN:
                dragX = e.getX();
                return true;
            case android.view.MotionEvent.ACTION_MOVE:
                if (!Float.isNaN(dragX)) {
                    actions.rotate((e.getX() - dragX) / Math.max(.1f, scale));
                    dragX = e.getX();
                }
                return true;
            default:
                dragX = Float.NaN;
                return true;
        }
    }

    private void submit() {
        if (busy() || password == null || account == null) return;
        int length = password.length();
        String user = account.getText().toString().trim();
        if (user.isEmpty() || length == 0) {
            status("Preencha sua conta e senha.");
            return;
        }
        char[] secret = new char[length];
        password.getText().getChars(0, length, secret, 0);
        password.setText("");
        hideKeyboard();
        actions.login(user, secret);
    }

    void hideKeyboard() {
        InputMethodManager k =
                (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (k != null) k.hideSoftInputFromWindow(getWindowToken(), 0);
        clearFocus();
    }

    private EditText edit(boolean secret) {
        EditText e = new EditText(getContext());
        e.setSingleLine(true);
        e.setTextColor(Color.WHITE);
        e.setBackgroundColor(Color.TRANSPARENT);
        e.setInputType(
                InputType.TYPE_CLASS_TEXT
                        | (secret
                                ? InputType.TYPE_TEXT_VARIATION_PASSWORD
                                : InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS));
        e.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI);
        e.setFilters(new InputFilter[] {new InputFilter.LengthFilter(255)});
        e.setSaveEnabled(false);
        e.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        e.setSelectAllOnFocus(true);
        e.setContentDescription(secret ? "Senha da conta" : "Nome da conta");
        return e;
    }

    private Button button(String label, Runnable run) {
        return new OriginalButton(label, run, false, false);
    }

    private final class OriginalButton extends Button {
        private final String label;
        private final boolean row, chosen;

        OriginalButton(String label, Runnable action, boolean row, boolean chosen) {
            super(GlueScreenView.this.getContext());
            this.label = label;
            this.row = row;
            this.chosen = chosen;
            setBackgroundColor(Color.TRANSPARENT);
            setText(label);
            setContentDescription(label);
            setAllCaps(false);
            setPadding(0, 0, 0, 0);
            setMinWidth(0);
            setMinimumWidth(0);
            setMinHeight(0);
            setMinimumHeight(0);
            setOnClickListener(v -> action.run());
            setSoundEffectsEnabled(false);
        }

        @Override
        protected void onDraw(Canvas c) {
            RectF r = new RectF(0, 0, getWidth(), getHeight());
            paint.setAlpha(255);
            if (row) {
                if (chosen || isPressed()) {
                    paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.ADD));
                    image(
                            c,
                            "Interface/Glues/CharacterSelect/Glue-CharacterSelect-Highlight",
                            new RectF(-20 * scale, -8 * scale, 236 * scale, 66 * scale),
                            0,
                            1);
                    paint.setXfermode(null);
                }
                paint.setTypeface(assets == null ? Typeface.SERIF : assets.font);
                paint.setTextSize(16 * scale);
                paint.setColor(chosen ? Color.WHITE : GOLD);
                paint.setTextAlign(Paint.Align.LEFT);
                String[] parts = label.split(" — ", 2);
                c.drawText(fit(parts[0], r.width() - 28 * scale), 12 * scale, 23 * scale, paint);
                if (parts.length == 2) {
                    paint.setTextSize(12 * scale);
                    paint.setColor(0xffddd5bd);
                    c.drawText(parts[1], 12 * scale, 42 * scale, paint);
                }
            } else {
                String art =
                        GlueAssets.COMMON
                                + (isEnabled()
                                        ? (isPressed()
                                                ? "Glues-BigButton-Down"
                                                : "Glues-BigButton-Up")
                                        : "Glues-EditBox-Background");
                if (!image(c, art, r, .14453125f, .85546875f)) {
                    paint.setColor(isEnabled() ? 0xff5b1717 : 0xff25262a);
                    c.drawRect(r, paint);
                    paint.setStyle(Paint.Style.STROKE);
                    paint.setStrokeWidth(2);
                    paint.setColor(0xffa89458);
                    c.drawRect(r, paint);
                    paint.setStyle(Paint.Style.FILL);
                }
                if (isEnabled() && assets != null) {
                    paint.setXfermode(new PorterDuffXfermode(PorterDuff.Mode.ADD));
                    image(c, GlueAssets.COMMON + "Glues-BigButton-Glow", r, .14453125f, .85546875f);
                    if (isPressed() || isFocused())
                        image(
                                c,
                                GlueAssets.COMMON + "Glues-BigButton-Highlight",
                                r,
                                .14453125f,
                                .85546875f);
                    paint.setXfermode(null);
                }
                paint.setTypeface(assets == null ? Typeface.SERIF : assets.font);
                paint.setTextSize((r.width() / scale < 170 ? 13 : 16) * scale);
                paint.setTextAlign(Paint.Align.CENTER);
                paint.setColor(isEnabled() ? GOLD : 0xff888888);
                paint.setShadowLayer(2 * scale, scale, scale, Color.BLACK);
                c.drawText(
                        fit(label, r.width() - 16 * scale),
                        r.centerX(),
                        r.centerY() - (paint.ascent() + paint.descent()) / 2,
                        paint);
                paint.clearShadowLayer();
            }
        }
    }

    @Override
    protected void onMeasure(int ws, int hs) {
        int w = MeasureSpec.getSize(ws), h = MeasureSpec.getSize(hs);
        scale = Math.max(.1f, h / 768f);
        logicalWidth = w / scale;
        setMeasuredDimension(w, h);
        for (Map.Entry<View, Supplier<GlueLayout.Box>> p : placed.entrySet()) {
            GlueLayout.Box b = p.getValue().get();
            View v = p.getKey();
            v.measure(
                    MeasureSpec.makeMeasureSpec(
                            Math.max(0, Math.round(b.w * scale)), MeasureSpec.EXACTLY),
                    MeasureSpec.makeMeasureSpec(
                            Math.max(0, Math.round(b.h * scale)), MeasureSpec.EXACTLY));
            if (v instanceof EditText) {
                EditText e = (EditText) v;
                e.setTypeface(assets.font);
                e.setTextSize(TypedValue.COMPLEX_UNIT_PX, 16 * scale);
                e.setPadding((int) (12 * scale), 0, (int) (8 * scale), 0);
            }
        }
    }

    @Override
    protected void onLayout(boolean changed, int l, int t, int r, int b) {
        for (Map.Entry<View, Supplier<GlueLayout.Box>> p : placed.entrySet()) {
            GlueLayout.Box box = p.getValue().get();
            int x = Math.round(box.x * scale), y = Math.round(box.y * scale);
            p.getKey()
                    .layout(
                            x,
                            y,
                            x + p.getKey().getMeasuredWidth(),
                            y + p.getKey().getMeasuredHeight());
        }
    }

    private boolean image(Canvas c, String path, RectF r, float left, float right) {
        Bitmap b = assets == null ? null : assets.texture(path);
        if (b == null) return false;
        paint.setColor(Color.WHITE);
        paint.setAlpha(255);
        c.drawBitmap(
                b,
                new Rect(
                        Math.round(left * b.getWidth()),
                        0,
                        Math.round(right * b.getWidth()),
                        b.getHeight()),
                r,
                paint);
        return true;
    }

    private RectF rect(GlueLayout.Box b) {
        return new RectF(b.x, b.y, b.x + b.w, b.y + b.h);
    }

    private String fit(String text, float width) {
        int n = paint.breakText(text, true, Math.max(0, width), null);
        return n < text.length() && n > 1 ? text.substring(0, n - 1) + "…" : text.substring(0, n);
    }

    private void text(Canvas c, String s, float x, float y, float size, int color) {
        paint.setTypeface(assets == null ? Typeface.SERIF : assets.font);
        paint.setTextSize(size);
        paint.setColor(color);
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setShadowLayer(1.5f, 1, 1, Color.BLACK);
        c.drawText(s, x, y, paint);
        paint.clearShadowLayer();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        Canvas c = canvas;
        c.save();
        c.scale(scale, scale);
        if (!scene3d) {
            // Fallback when the native ModelFFX scene is unavailable.
            paint.setShader(
                    new LinearGradient(
                            0,
                            0,
                            0,
                            768,
                            new int[] {0xff05070b, 0xff12202d, 0xff080a0e},
                            null,
                            Shader.TileMode.CLAMP));
            c.drawRect(0, 0, logicalWidth, 768, paint);
            paint.setShader(null);
        }
        if (assets == null) {
            text(c, "WORLD OF WARCRAFT", logicalWidth / 2, 280, 28, GOLD);
            text(c, "Interface dos seus arquivos MPQ", logicalWidth / 2, 320, 18, Color.WHITE);
        } else {
            String logo = presentation >= 2 ? "CharacterSelectLogo" : "AccountLoginLogo";
            String path = assets.layout.texture(logo);
            if (presentation != 3) image(c, path, rect(box(logo)), 0, 1);
            if (presentation == 3) {
                text(c, "Aliança", 116, 58, 18, 0xff8fb4ff);
                text(c, "Horda", logicalWidth - 116, 58, 18, 0xffff6a5a);
                text(c, "Arraste para girar o personagem", logicalWidth / 2, 576, 13, 0xffd8d0b8);
            } else if (presentation == 0) {
                for (String name :
                        new String[] {"AccountLoginAccountEdit", "AccountLoginPasswordEdit"}) {
                    GlueLayout.Box b = box(name);
                    image(
                            c,
                            GlueAssets.COMMON + "Glues-EditBox-Background",
                            rect(b),
                            .14453125f,
                            .85546875f);
                    text(
                            c,
                            name.contains("Password") ? "Senha" : "Nome da conta",
                            b.x + b.w / 2,
                            b.y - 6,
                            16,
                            GOLD);
                }
                image(
                        c,
                        "Interface/Glues/Mainmenu/Glues-BlizzardLogo",
                        new RectF(logicalWidth / 2 - 50, 660, logicalWidth / 2 + 50, 760),
                        0,
                        1);
            } else if (presentation == 1) {
                text(c, "Seleção de reino", logicalWidth / 2, 215, 24, GOLD);
                if (realms.isEmpty())
                    text(c, "Nenhum reino disponível", logicalWidth / 2, 300, 18, Color.WHITE);
            } else {
                GlueLayout.Box realm = box("CharSelectRealmName");
                text(c, realmName, realm.x + realm.w / 2, realm.y + 13, 16, GOLD);
                if (characters.isEmpty()) {
                    GlueLayout.Box area = box("CharacterSelectCharacterFrame");
                    text(c, "Nenhum personagem", area.x + area.w / 2, area.y + 110, 16, GOLD);
                }
                if (selected >= 0 && selected < characters.size())
                    text(
                            c,
                            characters.get(selected).split(" — ", 2)[0],
                            logicalWidth / 2,
                            682,
                            24,
                            GOLD);
            }
        }
        if (presentation != 3)
            text(c, "3.3.5a  •  auth 12340  •  Android ARM64", 150, 752, 12, 0xffb8aa85);
        if (!message.isEmpty()) {
            paint.setTextSize(16);
            List<String> lines = wrap(message, Math.min(620, logicalWidth - 300));
            float y = presentation == 0 ? 565 : (presentation == 3 ? 520 : 590);
            for (String line : lines) {
                text(c, line, logicalWidth / 2, y, 16, Color.WHITE);
                y += 21;
            }
        }
        c.restore();
    }

    private List<String> wrap(String text, float width) {
        List<String> lines = new ArrayList<>();
        for (String part : text.split("\n")) {
            while (!part.isEmpty() && lines.size() < 5) {
                int n = paint.breakText(part, true, Math.max(200, width), null);
                if (n < 1) break;
                lines.add(part.substring(0, n));
                part = part.substring(n);
            }
        }
        return lines;
    }
}
