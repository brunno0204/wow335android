package org.wowdecomp.android;

/** Pointer ownership and gestures independent of Android, tested on the host JVM. */
final class TouchController {
    interface Sink {
        void movement(float right, float forward);
        void camera(boolean held, float dx, float dy);
        void jump(boolean held);
        void pointer(int x, int y);
        void tap(int x, int y);
        // Original UI: the free finger is a held left button; the UI answers whether it took it.
        default void pointerDown(int x, int y) { pointer(x, y); }
        default void pointerMove(int x, int y) {}
        default void pointerUp(int x, int y) {}
        default void pointerCancel() {} // the finger became the movement stick
        default boolean pointerOnUi() { return false; }
        default void wheel(int x, int y, int delta) {}
        void zoom(float factor); // pinch: >1 zooms in
        default void freeLook(boolean on) {} // camera drags turn only the camera
    }
    private final Sink sink;
    private int stickId = -1, cameraId = -1, jumpId = -1, pinchId = -1;
    private float density = 1, width, height, cameraX, cameraY, startX, startY, pinchX, pinchY, pinchDistance, wheelY;
    private boolean dragging, uiDrag, stickCandidate; // candidate: left-half finger that may become the stick
    float stickX, stickY, radius, knobX, knobY, jumpX, jumpY, jumpRadius, eyeX, eyeY, eyeRadius;
    boolean jumping, freeLook;
    /** The floating stick is shown only while a finger holds it, anchored where it went down. */
    boolean stickActive() { return stickId != -1; }
    TouchController(Sink sink) { this.sink = sink; }
    void layout(float width, float height, float density) {
        cancel();
        this.width = Math.max(0, width);
        this.height = Math.max(0, height);
        this.density = Math.max(1, density);
        // Floating stick: invisible until a finger drags on the free (non-UI) left half of the
        // screen; it is anchored where that finger went down.
        radius = Math.min(38 * this.density, Math.min(this.width, this.height) * .12f);
        float padding = Math.min(22 * this.density, radius * .6f);
        stickX = this.width * .25f;
        stickY = this.height * .66f;
        jumpRadius = Math.min(34 * this.density, Math.min(this.width, this.height) * .11f);
        jumpX = this.width - padding - jumpRadius;
        jumpY = this.height - padding - jumpRadius;
        eyeRadius = jumpRadius * .72f;
        eyeX = jumpX;
        eyeY = jumpY - jumpRadius - padding * .6f - eyeRadius;
    }
    private static boolean finite(float x, float y) { return Float.isFinite(x) && Float.isFinite(y); }
    private static boolean inside(float x, float y, float cx, float cy, float r) {
        return Math.hypot(x - cx, y - cy) <= r;
    }
    void down(int id, float x, float y) {
        if (id < 0 || !finite(x, y) || radius <= 0 || x < 0 || y < 0 || x > width || y > height ||
            id == stickId || id == cameraId || id == jumpId || id == pinchId) return;
        // A finger retains its role even after moving over another control.
        if (cameraId != -1 && stickCandidate && !dragging && !uiDrag) {
            // Walking finger on the left plus a new finger: the first one is the stick.
            becomeStick();
        }
        if (inside(x, y, jumpX, jumpY, jumpRadius * 1.18f)) {
            if (jumpId == -1) { jumpId = id; jumping = true; sink.jump(true); }
        } else if (inside(x, y, eyeX, eyeY, eyeRadius * 1.15f) && cameraId == -1) {
            freeLook = !freeLook;
            sink.freeLook(freeLook);
        } else if (stickId == -1 && cameraId != -1 && x < width * .5f) {
            // The camera finger is busy: a left-half finger walks right away.
            anchorStick(x, y);
            stickId = id;
            updateStick(x, y);
        } else if (cameraId != -1 && pinchId == -1) {
            // Second free finger: pinch zoom with the camera finger.
            pinchId = id; pinchX = x; pinchY = y;
            pinchDistance = (float)Math.hypot(x - cameraX, y - cameraY);
            if (dragging) sink.camera(false, 0, 0);
            dragging = true; // a pinch never ends as a tap
        } else if (cameraId == -1) {
            cameraId = id;
            uiDrag = false;
            stickCandidate = stickId == -1 && x < width * .5f;
            sink.pointerDown((int)x, (int)y);
            cameraX = startX = x;
            cameraY = startY = y;
            dragging = false;
        }
    }
    void move(int id, float x, float y) {
        if (id < 0) return;
        if (!finite(x, y)) { cancel(); return; }
        if (id == stickId) updateStick(x, y);
        else if (pinchId != -1 && (id == cameraId || id == pinchId)) {
            if (id == cameraId) { cameraX = x; cameraY = y; } else { pinchX = x; pinchY = y; }
            float distance = (float)Math.hypot(pinchX - cameraX, pinchY - cameraY);
            if (pinchDistance > 8 * density && distance > 8 * density) {
                float factor = distance / pinchDistance;
                if (Math.abs(factor - 1) > .01f) { sink.zoom(factor); pinchDistance = distance; }
            } else pinchDistance = distance;
        } else if (id == cameraId && stickCandidate && !uiDrag && !dragging &&
                   Math.hypot(x - startX, y - startY) >= 8 * density && !sink.pointerOnUi()) {
            // Dragging on the free left half: the stick appears where the finger went down.
            becomeStick();
            updateStick(x, y);
        } else if (id == cameraId && (uiDrag || (!dragging && sink.pointerOnUi()))) {
            // Finger went down on a UI frame: it drags/scrolls the UI, not the camera.
            if (!uiDrag) wheelY = startY;
            uiDrag = true;
            sink.pointerMove((int)x, (int)y);
            // Vertical swipes on the UI also scroll (chat, lists) like the mouse wheel.
            float step = 22 * density;
            while (y - wheelY >= step) { sink.wheel((int)x, (int)y, 1); wheelY += step; }
            while (wheelY - y >= step) { sink.wheel((int)x, (int)y, -1); wheelY -= step; }
            cameraX = x; cameraY = y;
        } else if (id == cameraId) {
            if (!dragging && Math.hypot(x - startX, y - startY) >= 8 * density) {
                dragging = true;
                sink.camera(true, (x - startX) / density, (y - startY) / density);
            } else if (dragging) {
                sink.camera(true, (x - cameraX) / density, (y - cameraY) / density);
            }
            cameraX = x; cameraY = y;
        }
    }
    private void anchorStick(float x, float y) {
        stickX = Math.max(radius, Math.min(width - radius, x));
        stickY = Math.max(radius, Math.min(height - radius, y));
    }
    private void becomeStick() {
        sink.pointerCancel();
        anchorStick(startX, startY);
        stickId = cameraId;
        cameraId = -1;
        stickCandidate = false;
        dragging = uiDrag = false;
    }
    private void updateStick(float x, float y) {
        float dx = (x - stickX) / radius, dy = (y - stickY) / radius;
        float length = (float)Math.hypot(dx, dy);
        if (length > 1) { dx /= length; dy /= length; }
        knobX = dx; knobY = dy;
        if (length <= .16f) sink.movement(0, 0);
        else {
            float strength = Math.min(1, (Math.min(length, 1) - .16f) / .84f);
            sink.movement(dx / Math.min(length, 1) * strength, -dy / Math.min(length, 1) * strength);
        }
    }
    void up(int id, float x, float y) {
        if (id < 0) return;
        if (id == stickId) {
            stickId = -1; knobX = knobY = 0; sink.movement(0, 0);
        } else if (id == jumpId) {
            jumpId = -1; jumping = false; sink.jump(false);
        } else if (id == pinchId) {
            pinchId = -1;
        } else if (id == cameraId && pinchId != -1) {
            sink.pointerUp((int)cameraX, (int)cameraY);
            // The remaining pinch finger becomes the camera finger.
            cameraId = pinchId; pinchId = -1;
            cameraX = startX = pinchX; cameraY = startY = pinchY;
        } else if (id == cameraId) {
            boolean onUi = uiDrag || sink.pointerOnUi();
            if (finite(x, y)) sink.pointerUp((int)x, (int)y); else sink.pointerUp((int)cameraX, (int)cameraY);
            if (dragging) sink.camera(false, 0, 0);
            else if (!onUi && finite(x, y) && Math.hypot(x - startX, y - startY) < 8 * density &&
                     x >= 0 && y >= 0 && x < width && y < height) sink.tap((int)x, (int)y);
            cameraId = -1; dragging = false; uiDrag = false; stickCandidate = false;
        }
    }
    void cancel() {
        if (cameraId != -1) sink.pointerUp((int)cameraX, (int)cameraY);
        uiDrag = false; stickCandidate = false;
        stickId = cameraId = jumpId = pinchId = -1;
        knobX = knobY = 0; dragging = jumping = false;
        sink.movement(0, 0); sink.camera(false, 0, 0); sink.jump(false);
    }
}
