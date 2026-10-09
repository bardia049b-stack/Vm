/*
 * RvmView -- the serial console, drawn with Canvas.
 *
 * Not a WebView and not a TextView: a fixed-grid character cell painted
 * straight into a Bitmap-free Canvas, which is the cheapest thing that can
 * render 80x24 at 60 Hz on a phone.  There are no shadows, no gradients and
 * no animation -- just a monospace font on a soft paper-coloured background.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

/** The console: a scrolling character grid plus a cursor. */
public final class RvmView extends View {

    /** Scrollback lines kept in memory.  2000 lines of 256 chars is ~1 MB. */
    private static final int SCROLLBACK = 2000;
    private static final int MAXCOLS = 256;

    private final char[][] grid = new char[SCROLLBACK][MAXCOLS];
    private final int[] len = new int[SCROLLBACK];
    private int head;          /* index of the oldest retained line */
    private int used;          /* how many lines are valid */
    private int curCol;        /* cursor column on the current line */

    private final Paint fg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bg = new Paint();
    private final Paint cursor = new Paint();

    private float charW = 12f;
    private float lineH = 24f;
    private int cols = 80;
    private int rows = 24;

    private KeyListener keys;

    public interface KeyListener { void onKey(byte b); }

    public RvmView(Context c) { this(c, null); }

    public RvmView(Context c, AttributeSet a) {
        this(c, a, 0);
    }

    public RvmView(Context c, AttributeSet a, int defStyle) {
        super(c, a, defStyle);
        bg.setColor(0xFFF5F4F0);      /* soft paper */
        fg.setColor(0xFF2B2B28);      /* soft ink   */
        fg.setTypeface(Typeface.MONOSPACE);
        cursor.setColor(0xFF2B2B28);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackgroundColor(0xFFF5F4F0);
    }

    public void setKeyListener(KeyListener l) { keys = l; }

    /* ------------------------------------------------------------- feeding */

    /** Appends decoded console bytes.  Called from the UI thread only. */
    public void write(byte[] buf, int n) {
        for (int i = 0; i < n; i++) {
            byte b = buf[i];
            if (b == '\n') {
                newline();
            } else if (b == '\r') {
                curCol = 0;
            } else if (b == '\b') {
                if (curCol > 0) curCol--;
            } else if (b == '\t') {
                curCol = ((curCol + 8) / 8) * 8;
                if (curCol >= cols) newline();
            } else if (b == 27) {
                /* A real ANSI parser is overkill for a login prompt; consume
                 * CSI sequences so they do not show up as garbage. */
                i = skipEscape(buf, i, n);
            } else if (b >= 32) {
                put((char) (b & 0xFF));
            }
        }
        invalidate();
    }

    private int skipEscape(byte[] buf, int i, int n) {
        if (i + 1 >= n) return i;
        char c = (char) (buf[i + 1] & 0xFF);
        if (c != '[' && c != '(' && c != ')') return i + 1;
        if (c == '(' || c == ')') return i + 2;
        int j = i + 2;
        while (j < n) {
            char d = (char) (buf[j] & 0xFF);
            if (d >= 0x40 && d <= 0x7E) return j;
            j++;
        }
        return n - 1;
    }

    private void put(char ch) {
        if (curCol >= cols) newline();
        int line = (head + used - 1) % SCROLLBACK;
        if (used == 0) { newline(); line = (head + used - 1) % SCROLLBACK; }
        grid[line][curCol] = ch;
        len[line] = Math.max(len[line], curCol + 1);
        curCol++;
    }

    private void newline() {
        if (used < SCROLLBACK) {
            used++;
        } else {
            head = (head + 1) % SCROLLBACK;   /* drop the oldest line */
        }
        int line = (head + used - 1) % SCROLLBACK;
        len[line] = 0;
        curCol = 0;
    }

    public void clear() {
        used = 0; head = 0; curCol = 0;
        for (int i = 0; i < SCROLLBACK; i++) len[i] = 0;
        invalidate();
    }

    /* ------------------------------------------------------------- drawing */

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        measureFont();
    }

    private void measureFont() {
        float size = Math.max(18f, getHeight() / 30f);
        fg.setTextSize(size);
        Paint.FontMetrics fm = fg.getFontMetrics();
        lineH = (float) Math.ceil(fm.descent - fm.ascent);
        charW = fg.measureText("M");
        if (charW <= 0f) charW = size * 0.6f;
        int c = (int) (getWidth() / charW);
        int r = (int) (getHeight() / lineH);
        cols = Math.max(20, Math.min(MAXCOLS, c));
        rows = Math.max(4, r);
    }

    @Override
    protected void onDraw(Canvas cv) {
        if (charW <= 0f) measureFont();
        cv.drawRect(0, 0, getWidth(), getHeight(), bg);

        Paint.FontMetrics fm = fg.getFontMetrics();
        float baseline = -fm.ascent;

        /* Show the last `rows` lines, oldest at the top. */
        int first = used > rows ? used - rows : 0;
        for (int i = first; i < used; i++) {
            int line = (head + i) % SCROLLBACK;
            int n = Math.min(len[line], cols);
            if (n <= 0) continue;
            float y = (i - first) * lineH + baseline;
            cv.drawText(grid[line], 0, n, 0f, y, fg);
            /* Only the live last line gets a cursor. */
            if (i == used - 1 && curCol <= cols) {
                cv.drawRect(curCol * charW, (i - first) * lineH,
                            (curCol + 1) * charW, (i - first + 1) * lineH, cursor);
            }
        }
        if (used == 0) {
            cv.drawRect(0f, 0f, charW, lineH, cursor);
        }
    }

    public int getCols() { return cols; }
    public int getRows() { return rows; }

    /* -------------------------------------------------------------- input */

    @Override
    public boolean onKeyDown(int code, KeyEvent ev) {
        if (keys == null) return super.onKeyDown(code, ev);
        switch (code) {
            case KeyEvent.KEYCODE_ENTER:      keys.onKey((byte) '\r'); return true;
            case KeyEvent.KEYCODE_DEL:        keys.onKey((byte) 127);  return true;
            case KeyEvent.KEYCODE_TAB:        keys.onKey((byte) '\t'); return true;
            case KeyEvent.KEYCODE_ESCAPE:     keys.onKey((byte) 27);   return true;
            case KeyEvent.KEYCODE_DPAD_UP:    keys.onKey((byte) 0x1b); keys.onKey((byte) 'A'); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN:  keys.onKey((byte) 0x1b); keys.onKey((byte) 'B'); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: keys.onKey((byte) 0x1b); keys.onKey((byte) 'C'); return true;
            case KeyEvent.KEYCODE_DPAD_LEFT:  keys.onKey((byte) 0x1b); keys.onKey((byte) 'D'); return true;
            default: break;
        }
        int u = ev.getUnicodeChar();
        if (u >= 32 && u < 127) { keys.onKey((byte) u); return true; }
        return super.onKeyDown(code, ev);
    }

    @Override
    public boolean onCheckIsTextEditor() { return true; }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        out.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
        out.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        return new BaseInputConnection(this, false);
    }

    /** A tap on the console raises the soft keyboard. */
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (e.getAction() == MotionEvent.ACTION_UP) {
            requestFocus();
            InputMethodManager imm =
                (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
            if (imm != null) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        }
        return true;
    }
}
