package org.wowdecomp.android;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

/** Overlay fills only the GL surface; touch positions share the surface's origin. */
final class TouchControlsView extends View {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint namePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private Typeface nameFont; // UNIT_NAME_FONT (Fonts\FRIZQT__.TTF) from the game data
    /** Loads the original unit name font from the game archives (background thread). */
    void loadNameFont(java.io.File cacheDir, GameAudio.Reader reader) {
        if (nameFont != null) return;
        new Thread(() -> {
            try {
                byte[] bytes = reader.read("Fonts\\FRIZQT__.TTF");
                if (bytes == null || bytes.length == 0) return;
                java.io.File file = new java.io.File(cacheDir, "FRIZQT__.TTF");
                try (java.io.FileOutputStream out = new java.io.FileOutputStream(file)) { out.write(bytes); }
                Typeface font = Typeface.createFromFile(file);
                post(() -> { nameFont = font; invalidate(); });
            } catch (Exception ignored) {
            }
        }, "name-font").start();
    }
    private final TouchController controller;
    private final float density;
    // In-world HUD: player/target frames and nameplates (from native overlay lines).
    private String overlay = "", playerName = "";
    private boolean originalUi; // original FrameXML frames replace the simple unit frames
    void setOriginalUi(boolean on) { if (on != originalUi) { originalUi = on; invalidate(); } }
    private int playerLevel, playerHp, playerMaxHp, playerPower, playerMaxPower, playerPowerType;
    void setOverlay(String text) {
        String value = text == null ? "" : text;
        if (!value.equals(overlay)) { overlay = value; invalidate(); }
    }
    void setPlayer(String name, int level, int hp, int maxHp, int power, int maxPower, int powerType) {
        if (name.equals(playerName) && level == playerLevel && hp == playerHp && maxHp == playerMaxHp &&
            power == playerPower && maxPower == playerMaxPower && powerType == playerPowerType) return;
        playerName = name; playerLevel = level; playerHp = hp; playerMaxHp = maxHp;
        playerPower = power; playerMaxPower = maxPower; playerPowerType = powerType;
        invalidate();
    }
    private static int powerColor(int type) {
        switch (type) {
            case 1: return Color.rgb(200, 30, 30);    // rage
            case 2: return Color.rgb(230, 130, 40);   // focus
            case 3: return Color.rgb(230, 210, 40);   // energy
            case 6: return Color.rgb(0, 190, 230);    // runic power
            default: return Color.rgb(40, 90, 230);   // mana
        }
    }
    private void bar(Canvas canvas, float x, float y, float w, float h, int value, int max, int color, String label) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb(170, 0, 0, 0));
        canvas.drawRect(x, y, x + w, y + h, paint);
        float k = max > 0 ? Math.max(0, Math.min(1, value / (float)max)) : 0;
        paint.setColor(color);
        canvas.drawRect(x + 1, y + 1, x + 1 + (w - 2) * k, y + h - 1, paint);
        paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(h * .78f);
        canvas.drawText(label, x + w / 2, y + h * .78f, paint);
    }
    private void unitFrame(Canvas canvas, float x, float y, String name, int level, int hp, int maxHp,
                           int power, int maxPower, int powerType, boolean showPower) {
        float w = 170 * density, h = 46 * density;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb(150, 10, 12, 20));
        canvas.drawRoundRect(x, y, x + w, y + h, 6 * density, 6 * density, paint);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(1.5f * density);
        paint.setColor(Color.argb(200, 200, 170, 90));
        canvas.drawRoundRect(x, y, x + w, y + h, 6 * density, 6 * density, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.rgb(255, 210, 0)); paint.setTextAlign(Paint.Align.LEFT); paint.setTextSize(12 * density);
        canvas.drawText(name, x + 6 * density, y + 14 * density, paint);
        if (level > 0) {
            paint.setTextAlign(Paint.Align.RIGHT); paint.setColor(Color.WHITE);
            canvas.drawText(String.valueOf(level), x + w - 6 * density, y + 14 * density, paint);
        }
        bar(canvas, x + 5 * density, y + 18 * density, w - 10 * density, 12 * density, hp, maxHp,
            Color.rgb(30, 170, 40), maxHp > 0 ? hp + " / " + maxHp : "");
        if (showPower && maxPower > 0)
            bar(canvas, x + 5 * density, y + 31 * density, w - 10 * density, 11 * density, power, maxPower,
                powerColor(powerType), power + " / " + maxPower);
    }
    private void drawHud(Canvas canvas) {
        if (!originalUi && (playerMaxHp > 0 || !playerName.isEmpty()))
            unitFrame(canvas, 8 * density, 8 * density, playerName, playerLevel, playerHp, playerMaxHp,
                playerPower, playerMaxPower, playerPowerType, true);
        float w = getWidth(), h = getHeight();
        paint.setTextAlign(Paint.Align.CENTER);
        for (String line : overlay.split("\n")) {
            String[] f = line.split("\\|", -1);
            try {
                if (f.length >= 8 && f[0].equals("N")) {
                    // Unit names as the client draws them: UNIT_NAME_FONT, reaction color
                    // (CGGameUI::GetSelectionHighlightColor), dark outline, constant size.
                    float x = Float.parseFloat(f[1]) * w, y = Float.parseFloat(f[2]) * h;
                    int color = 0xff000000 | Integer.parseInt(f[6], 16);
                    String name = f[7];
                    namePaint.setTypeface(nameFont != null ? nameFont : Typeface.DEFAULT_BOLD);
                    namePaint.setTextAlign(Paint.Align.CENTER);
                    namePaint.setTextSize(Math.max(11 * density, h * .027f));
                    namePaint.setStyle(Paint.Style.STROKE);
                    namePaint.setStrokeJoin(Paint.Join.ROUND);
                    namePaint.setStrokeWidth(namePaint.getTextSize() * .14f);
                    namePaint.setColor(Color.argb(220, 0, 0, 0));
                    canvas.drawText(name, x, y, namePaint);
                    namePaint.setStyle(Paint.Style.FILL);
                    namePaint.setColor(color);
                    canvas.drawText(name, x, y, namePaint);
                } else if (!originalUi && f.length >= 9 && f[0].equals("T")) {
                    unitFrame(canvas, 186 * density, 8 * density, f[8], Integer.parseInt(f[1]),
                        Integer.parseInt(f[2]), Integer.parseInt(f[3]), Integer.parseInt(f[4]),
                        Integer.parseInt(f[5]), Integer.parseInt(f[6]), true);
                }
            } catch (NumberFormatException ignored) {
            }
        }
    }
    TouchControlsView(Context context, TouchController.Sink sink) {
        super(context);
        density = getResources().getDisplayMetrics().density;
        controller = new TouchController(sink);
        setClickable(true);
        setContentDescription("Joystick esquerdo: andar. Arraste: câmera. Pinça: zoom. Botão direito: pular. Toque: selecionar alvo.");
    }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        controller.layout(w, h, density);
    }
    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        drawHud(canvas);
        if (controller.stickActive()) { // floating stick: only while a finger holds it
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(80, 12, 20, 30));
            canvas.drawCircle(controller.stickX, controller.stickY, controller.radius, paint);
            paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(2 * density);
            paint.setColor(Color.argb(170, 225, 230, 235));
            canvas.drawCircle(controller.stickX, controller.stickY, controller.radius, paint);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(170, 220, 225, 230));
            canvas.drawCircle(controller.stickX + controller.knobX * controller.radius * .62f,
                controller.stickY + controller.knobY * controller.radius * .62f, controller.radius * .33f, paint);
        }
        paint.setColor(controller.jumping ? Color.argb(220, 220, 175, 80) : Color.argb(110, 12, 20, 30));
        canvas.drawCircle(controller.jumpX, controller.jumpY, controller.jumpRadius, paint);
        paint.setColor(Color.WHITE); paint.setTextAlign(Paint.Align.CENTER); paint.setTextSize(12 * density);
        canvas.drawText("PULAR", controller.jumpX, controller.jumpY + 4 * density, paint);
        paint.setColor(controller.freeLook ? Color.argb(220, 90, 160, 220) : Color.argb(110, 12, 20, 30));
        canvas.drawCircle(controller.eyeX, controller.eyeY, controller.eyeRadius, paint);
        paint.setColor(Color.WHITE); paint.setTextSize(9 * density);
        canvas.drawText("CÂMERA", controller.eyeX, controller.eyeY - 1 * density, paint);
        canvas.drawText(controller.freeLook ? "LIVRE" : "SEGUE", controller.eyeX, controller.eyeY + 9 * density, paint);
        if (!originalUi) { // the hint would cover the original action bar
            paint.setColor(Color.argb(130, 235, 235, 235)); paint.setTextSize(10 * density);
            canvas.drawText("Arraste: câmera · Pinça: zoom · Toque: alvo · 2º toque: interagir · Segurar: botão direito", getWidth() * .5f, getHeight() - 8 * density, paint);
        }
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            getParent().requestDisallowInterceptTouchEvent(true);
            controller.down(event.getPointerId(index), event.getX(index), event.getY(index));
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < event.getPointerCount(); ++i)
                controller.move(event.getPointerId(i), event.getX(i), event.getY(i));
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            controller.up(event.getPointerId(index), event.getX(index), event.getY(index));
            if (action == MotionEvent.ACTION_UP) performClick();
        } else if (action == MotionEvent.ACTION_CANCEL) controller.cancel();
        invalidate();
        return true;
    }
    @Override public boolean performClick() { super.performClick(); return true; }
    void cancelTouches() { controller.cancel(); invalidate(); }
    @Override protected void onDetachedFromWindow() { cancelTouches(); super.onDetachedFromWindow(); }
}
