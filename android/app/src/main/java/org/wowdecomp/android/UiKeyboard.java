package org.wowdecomp.android;

import android.content.Context;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/** Invisible target for the soft keyboard while an original UI EditBox (chat line) has focus. */
final class UiKeyboard extends View {
    interface Sink {
        void text(String text);
        void key(int which); // 0 Enter, 1 Backspace, 2 Escape, 3 Tab, 4 Left, 5 Right, 6 Up, 7 Down
    }
    private final Sink sink;
    private boolean shown;

    UiKeyboard(Context context, Sink sink) {
        super(context);
        this.sink = sink;
        setFocusable(true);
        setFocusableInTouchMode(true);
    }

    void setWanted(boolean wanted) {
        if (wanted == shown) return;
        shown = wanted;
        InputMethodManager manager = (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (manager == null) return;
        if (wanted) {
            requestFocus();
            manager.restartInput(this);
            manager.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        } else {
            manager.hideSoftInputFromWindow(getWindowToken(), 0);
            clearFocus();
        }
    }

    @Override public boolean onCheckIsTextEditor() { return true; }

    @Override public InputConnection onCreateInputConnection(EditorInfo info) {
        // Character-by-character commits (no composing text): the EditBox shows each letter at once.
        info.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD
            | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        info.imeOptions = EditorInfo.IME_ACTION_SEND | EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN;
        return new BaseInputConnection(this, false) {
            @Override public boolean commitText(CharSequence text, int newCursorPosition) {
                if (text == null) return true;
                String value = text.toString();
                if (value.equals("\n")) sink.key(0);
                else if (!value.isEmpty()) sink.text(value);
                return true;
            }
            @Override public boolean setComposingText(CharSequence text, int newCursorPosition) {
                return commitText(text, newCursorPosition);
            }
            @Override public boolean deleteSurroundingText(int before, int after) {
                for (int i = 0; i < Math.max(1, before); ++i) sink.key(1);
                return true;
            }
            @Override public boolean performEditorAction(int action) { sink.key(0); return true; }
            @Override public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN) handleKey(event.getKeyCode(), event);
                return true;
            }
        };
    }

    private boolean handleKey(int code, KeyEvent event) {
        switch (code) {
            case KeyEvent.KEYCODE_ENTER: case KeyEvent.KEYCODE_NUMPAD_ENTER: sink.key(0); return true;
            case KeyEvent.KEYCODE_DEL: sink.key(1); return true;
            case KeyEvent.KEYCODE_ESCAPE: sink.key(2); return true;
            case KeyEvent.KEYCODE_TAB: sink.key(3); return true;
            case KeyEvent.KEYCODE_DPAD_LEFT: sink.key(4); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: sink.key(5); return true;
            case KeyEvent.KEYCODE_DPAD_UP: sink.key(6); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: sink.key(7); return true;
            default:
                int unicode = event.getUnicodeChar();
                if (unicode > 0) { sink.text(new String(Character.toChars(unicode))); return true; }
                return false;
        }
    }

    @Override public boolean onKeyDown(int code, KeyEvent event) {
        return handleKey(code, event) || super.onKeyDown(code, event);
    }

    // Back closes the keyboard: leave the EditBox too (Escape), as closing the chat line does.
    @Override public boolean onKeyPreIme(int code, KeyEvent event) {
        if (shown && code == KeyEvent.KEYCODE_BACK && event.getAction() == KeyEvent.ACTION_UP) {
            sink.key(2);
            shown = false;
        }
        return super.onKeyPreIme(code, event);
    }
}
