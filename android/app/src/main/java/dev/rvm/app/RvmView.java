/*
 * RvmView -- the serial console: a VT subset drawn with Canvas.
 *
 * Not a WebView and not a TextView.  One fixed 80xN character grid, painted
 * straight onto a Canvas, which is the cheapest thing that renders a login
 * prompt at 60 Hz on a phone.  No shadows, no gradients, no animation apart
 * from the cursor blink.
 *
 * The parser keeps its state between write() calls.  That is the whole point:
 * output arrives in 1 KiB batches, so an escape sequence can be split across
 * two of them, and a parser that only looks inside one buffer prints the tail
 * as text -- "[6n" at the prompt, a stray letter here, a dropped one there.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.InputType;
import android.util.AttributeSet;
import android.view.GestureDetector;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.PopupMenu;
import android.widget.Toast;

/** The console: a scrolling character grid, a cursor and a selection. */
public final class RvmView extends View {

    /* The guest's tty is 80 columns wide and nothing resizes it: there is no
     * pty here to put a TIOCSWINSZ on, so the kernel keeps its default.  The
     * grid therefore is 80 columns too and the font shrinks to fit them.
     * Deriving the column count from the screen width instead made the app wrap
     * a long line somewhere the guest had not wrapped it, which is what reads
     * to the user as characters appearing where nobody typed them. */
    public static final int COLS = 80;

    private static final int SCROLLBACK = 2000;
    private static final int ROWS_START = 24;

    private final char[][] grid = new char[SCROLLBACK][COLS];
    private final byte[][] attr = new byte[SCROLLBACK][COLS];
    private final int[] len = new int[SCROLLBACK];
    private int head;                 /* ring index of the oldest line kept */
    private int used;                 /* lines kept, always >= rows */
    private long serial;              /* lines ever appended, for selection */
    private int rows = ROWS_START;

    private int curRow, curCol;
    private int savedRow, savedCol;
    private boolean cursorVisible = true;
    private int scroll;               /* lines the view is showing above the top */

    private final Paint fg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bg = new Paint();
    private final Paint mark = new Paint();

    private float charW = 12f;
    private float lineH = 24f;
    private float textBase = 18f;

    /* Parser state, kept between write() calls. */
    private static final int S_TEXT = 0, S_ESC = 1, S_CSI = 2, S_STRING = 3, S_STRING_ESC = 4;
    private int state;
    private final StringBuilder csi = new StringBuilder(24);
    private boolean priv;
    private final int[] argv = new int[8];
    private int argc;
    private byte pen = DEF_ATTR;

    /* Selection, in absolute line numbers so it survives scrolling. */
    private long selFrom = -1, selTo = -1;
    private boolean selecting;

    /** What a key press turns into on the guest console. */
    public interface KeyListener { void onKey(byte b); }

    private KeyListener keys;

    /* 16 ANSI colours: a Debian prompt expects them, and ls --color is the
     * only thing in a guest that asks.  bg 0 is the black page, so only a real
     * background colour is ever painted. */
    private static final int[] PAL = {
        0xFF000000, 0xFFCD3131, 0xFF0DBC79, 0xFFE3E18F,
        0xFF3687CC, 0xFF8B57B5, 0xFF2AC2DE, 0xFFCCCCCC,
        0xFF666666, 0xFFF14C4C, 0xFF23D18B, 0xFFF2F212,
        0xFF3B8EEA, 0xFF9638C7, 0xFF4CD1E0, 0xFFFFFFFF,
    };
    private static final byte DEF_ATTR = 0x07;

    public RvmView(Context c) { this(c, null); }

    public RvmView(Context c, AttributeSet a) { this(c, a, 0); }

    public RvmView(Context c, AttributeSet a, int defStyle) {
        super(c, a, defStyle);
        bg.setColor(0xFF000000);          /* Termux black, not the paper of PLAN */
        fg.setTypeface(Typeface.MONOSPACE);
        fg.setColor(PAL[7]);
        mark.setColor(0x553B8EEA);
        setFocusable(true);
        setFocusableInTouchMode(true);
        setBackgroundColor(0xFF000000);
        cursor.setColor(0xFFFFFFFF);
        paint.setTextSize(11f);
        paint.setColor(0xFFE3E18F);
        fitRows();

        gestures = new GestureDetector(c, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onSingleTapUp(MotionEvent e) {
                if (selFrom >= 0) { selFrom = selTo = -1; invalidate(); }
                requestFocus();
                showIme(true);
                return true;
            }

            @Override public void onLongPress(MotionEvent e) {
                int r = (int) (e.getY() - getPaddingTop()) / (int) lineH;
                int col = (int) ((e.getX() - getPaddingLeft()) / charW);
                beginSelection(r, col);
                openMenu();
            }

            @Override public boolean onDoubleTap(MotionEvent e) {
                int r = (int) (e.getY() - getPaddingTop()) / (int) lineH;
                selectLine(r);
                copySelection(true);
                return true;
            }

            @Override public boolean onDown(MotionEvent e) { return true; }
        });
    }

    private final GestureDetector gestures;

    public void setKeyListener(KeyListener l) { keys = l; }

    /* ------------------------------------------------------------- parsing */

    /** Appends decoded console bytes.  Called from the UI thread only. */
    public void write(byte[] buf, int n) {
        for (int i = 0; i < n; i++)
            feed(buf[i] & 0xFF);
        if (n > 0)
            scroll = 0;         /* new output: the live bottom wins */
        blinkOn = true;
        invalidate();
    }

    private void feed(int c) {
        switch (state) {
        case S_ESC:
            state = S_TEXT;
            if (c == '[') { state = S_CSI; csi.setLength(0); priv = false; argc = 0; return; }
            if (c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X') { state = S_STRING; return; }
            if (c == '7') { savedRow = curRow; savedCol = curCol; return; }
            if (c == '8') { curRow = clampRow(savedRow); curCol = clampCol(savedCol); return; }
            if (c == 'D') { newline(); return; }             /* index */
            if (c == 'E') { curCol = 0; newline(); return; } /* next line */
            if (c == 'M') { revNewline(); return; }          /* reverse index */
            if (c == 'c') { hardReset(); return; }           /* RIS */
            return;                                /* ( ) # and friends: ignored */
        case S_STRING:
            if (c == 0x07) state = S_TEXT;
            else if (c == 0x1B) state = S_STRING_ESC;
            return;
        case S_STRING_ESC:
            state = S_TEXT;                        /* the ST that follows an ESC */
            return;
        case S_CSI:
            csiFeed(c);
            return;
        default:
            if (c == 0x1B) { state = S_ESC; return; }
            ordinary(c);
            return;
        }
    }

    private void csiFeed(int c) {
        if (c >= 0x20 && c <= 0x2F) {              /* intermediates and private */
            if (c == '?')
                priv = true;
            else
                csi.append((char) c);
            return;
        }
        if (c >= 0x30 && c <= 0x3B) { csi.append((char) c); return; }   /* 0-9 : ; */
        if (c >= 0x3C && c <= 0x3F) return;                             /* < = > */
        if (c == 0x1B) { csi.setLength(0); priv = false; return; }      /* restart */
        state = S_TEXT;
        if (c >= 0x40 && c <= 0x7E)
            csiApply((char) c);
    }

    private void parseArgs() {
        argc = 0;
        int cur = 0, seen = 0;
        String s = csi.toString();
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (ch >= '0' && ch <= '9') {
                cur = cur * 10 + (ch - '0');
                seen = 1;
            } else if (ch == ';' || ch == ':') {
                if (argc < argv.length) argv[argc++] = seen != 0 ? cur : 0;
                cur = 0;
                seen = 0;
            }
        }
        if (argc < argv.length) argv[argc++] = seen != 0 ? cur : 0;
    }

    private int arg(int i) { return i < argc ? argv[i] : 0; }
    private int arg(int i, int dflt) { int v = arg(i); return v != 0 ? v : dflt; }

    private void csiApply(char fin) {
        parseArgs();
        switch (fin) {
        case 'A': move(-1, 0, arg(0, 1)); break;
        case 'B': move(1, 0, arg(0, 1)); break;
        case 'C': move(0, 1, arg(0, 1)); break;
        case 'D': move(0, -1, arg(0, 1)); break;
        case 'E': curCol = 0; move(1, 0, arg(0, 1)); break;
        case 'F': curCol = 0; move(-1, 0, arg(0, 1)); break;
        case 'G': curCol = clampCol(arg(0, 1) - 1); break;
        case 'd': curRow = clampRow(arg(0, 1) - 1); break;
        case 'H': case 'f':
            curRow = clampRow(arg(0, 1) - 1);
            curCol = clampCol(arg(1, 1) - 1);
            break;
        case 'J': eraseDisplay(arg(0)); break;
        case 'K': eraseLine(arg(0)); break;
        case 'L': insertLines(curRow, arg(0, 1)); break;
        case 'M': deleteLines(curRow, arg(0, 1)); break;
        case '@': insertBlanks(arg(0, 1)); break;
        case 'P': deleteChars(arg(0, 1)); break;
        case 'X': eraseChars(arg(0, 1)); break;
        case 'S': deleteLines(0, arg(0, 1)); break;
        case 'T': insertLines(0, arg(0, 1)); break;
        case 'm': if (!priv) sgr(); break;
        case 'h': case 'l': setMode(fin == 'h'); break;
        case 's': savedRow = curRow; savedCol = curCol; break;
        case 'u': curRow = clampRow(savedRow); curCol = clampCol(savedCol); break;
        case 'n':
            /* DSR: "where is the cursor" (6) and "are you there" (5).  A real
             * terminal answers these, and the guest blocks waiting for the
             * reply -- so the keystrokes typed in the meantime are read as the
             * answer instead of as input.  That is the whole of "typing puts
             * stray characters on my line": the query was being swallowed here. */
            if (arg(0, 6) == 6)
                reportCursor(priv);
            else if (arg(0, 6) == 5)
                report(new byte[] {0x1b, '[', '0', 'n'});
            break;
        case 'c':
            /* Device attributes, waited on the same way.  Answer VT102 with all
             * the options, which is what a plain "ESC [ c" expects to hear. */
            report(priv ? new byte[] {0x1b, '[', '?', '6', '0', ';', '1', ';', '2', 'c'}
                        : new byte[] {0x1b, '[', '?', '6', 'c'});
            break;
        case 'r': case 't': default:
            /* Scrolling regions and window ops: nothing a prompt can wait on. */
            break;
        }
    }

    /** Hand bytes back to the guest, the way a terminal's reply arrives. */
    private void report(byte[] b) {
        if (keys == null)
            return;
        for (byte value : b)
            keys.onKey(value);
    }

    /** The cursor report, in the 1-based decimal form the guest parses. */
    private void reportCursor(boolean decPrivate) {
        StringBuilder sb = new StringBuilder(16);
        sb.append((char) 0x1b).append('[');
        if (decPrivate)
            sb.append('?');
        sb.append(curRow + 1).append(';').append(curCol + 1).append('R');
        byte[] raw = new byte[sb.length()];
        for (int i = 0; i < sb.length(); i++)
            raw[i] = (byte) sb.charAt(i); /* every character here is ASCII */
        report(raw);
    }

    private void setMode(boolean on) {
        for (int i = 0; i < argc; i++) {
            int m = arg(i);
            if (!priv)
                continue;
            if (m == 25)
                cursorVisible = on;
            else if (m == 1049 || m == 1047 || m == 47)
                eraseWholeScreen();     /* alt screen: start clean, no restore */
        }
    }

    private void sgr() {
        for (int i = 0; i < argc; i++) {
            int v = arg(i);
            if (v == 0) { pen = DEF_ATTR; continue; }
            if (v == 1) { pen = (byte) ((pen & 0xF0) | ((pen & 0x0F) | 8)); continue; }
            if (v == 22) { pen = (byte) ((pen & 0xF0) | (pen & 0x07)); continue; }
            if (v >= 30 && v <= 37) { pen = (byte) ((pen & 0xF0) | (v - 30)); continue; }
            if (v >= 90 && v <= 97) { pen = (byte) ((pen & 0xF0) | (v - 90 + 8)); continue; }
            if (v == 39) { pen = (byte) ((pen & 0xF0) | 7); continue; }
            if (v >= 40 && v <= 47) { pen = (byte) (((v - 40) << 4) | (pen & 0x0F)); continue; }
            if (v == 49) { pen = (byte) (pen & 0x0F); continue; }
            /* 4 underline, 5 blink and 7 inverse are parsed and dropped: on a
             * monospace phone canvas they buy nothing and cost a plane. */
        }
    }

    /* ------------------------------------------------------------- editing */

    private void ordinary(int c) {
        switch (c) {
        case '\r': curCol = 0; return;
        case '\n': case 0x0B: case 0x0C: newline(); return;
        case '\b': if (curCol > 0) curCol--; return;
        case 0x7F: if (curCol > 0) curCol--; return;
        case '\t': curCol = Math.min(COLS - 1, ((curCol + 8) / 8) * 8); return;
        case 0x07: return;                       /* bell: no sound, no flash */
        default: break;
        }
        if (c < 0x20)
            return;
        putChar(c);
    }

    private void putChar(int c) {
        if (curCol >= COLS) {
            curCol = 0;
            newline();
        }
        int ri = ring(curRow);
        grid[ri][curCol] = (char) c;
        attr[ri][curCol] = pen;
        if (curCol + 1 > len[ri])
            len[ri] = curCol + 1;
        curCol++;
    }

    private void newline() {
        if (curRow >= rows - 1)
            appendLine();
        else
            curRow++;
    }

    private void revNewline() {
        if (curRow == 0)
            insertLines(0, 1);
        else
            curRow--;
    }

    private void move(int dr, int dc, int n) {
        curRow = clampRow(curRow + dr * n);
        curCol = clampCol(curCol + dc * n);
    }

    private int clampRow(int r) { return r < 0 ? 0 : (r >= rows ? rows - 1 : r); }
    private int clampCol(int c) { return c < 0 ? 0 : (c >= COLS ? COLS - 1 : c); }

    private void eraseDisplay(int m) {
        if (m == 2) { eraseWholeScreen(); return; }
        if (m == 3) {
            for (int i = 0; i < SCROLLBACK; i++)
                len[i] = 0;
            head = 0;
            used = rows;
            serial = rows;
            curRow = 0;
            curCol = 0;
            selFrom = selTo = -1;
            return;
        }
        int li = ring(curRow);
        if (m == 0) {
            if (curCol < len[li]) len[li] = curCol;
            for (int r = curRow + 1; r < rows; r++)
                len[ring(r)] = 0;
        } else {
            for (int r = 0; r < curRow; r++)
                len[ring(r)] = 0;
            blank(li, 0, curCol + 1);
        }
    }

    private void eraseWholeScreen() {
        for (int r = 0; r < rows; r++)
            len[ring(r)] = 0;
        curRow = 0;
        curCol = 0;
    }

    private void eraseLine(int m) {
        int li = ring(curRow);
        if (m == 2) {
            len[li] = 0;
        } else if (m == 1) {
            blank(li, 0, curCol + 1);
        } else if (curCol < len[li]) {
            len[li] = curCol;
        }
    }

    private void insertLines(int at, int n) {
        if (at >= rows) return;
        if (n > rows - at) n = rows - at;
        for (int r = rows - 1; r >= at + n; r--)
            copyLine(r - n, r);
        for (int r = at; r < at + n && r < rows; r++)
            len[ring(r)] = 0;
    }

    private void deleteLines(int at, int n) {
        if (at >= rows) return;
        if (n > rows - at) n = rows - at;
        for (int r = at; r < rows - n; r++)
            copyLine(r + n, r);
        for (int r = rows - n; r < rows; r++)
            len[ring(r)] = 0;
    }

    private void insertBlanks(int n) {
        int ri = ring(curRow);
        if (curCol >= COLS) return;
        if (n > COLS - curCol) n = COLS - curCol;
        /* A line can be shorter than the cursor column, and COLS - n can cut
         * the run: clamp before handing a length to arraycopy. */
        int moved = Math.min(len[ri], COLS - n) - curCol;
        if (moved < 0) moved = 0;
        if (moved > 0) {
            System.arraycopy(grid[ri], curCol, grid[ri], curCol + n, moved);
            System.arraycopy(attr[ri], curCol, attr[ri], curCol + n, moved);
        }
        blank(ri, curCol, curCol + n);
        len[ri] = Math.min(COLS, Math.max(len[ri], curCol + n));
    }

    private void deleteChars(int n) {
        int ri = ring(curRow);
        if (curCol >= len[ri]) return;
        if (n > len[ri] - curCol) n = len[ri] - curCol;
        System.arraycopy(grid[ri], curCol + n, grid[ri], curCol, len[ri] - curCol - n);
        System.arraycopy(attr[ri], curCol + n, attr[ri], curCol, len[ri] - curCol - n);
        len[ri] -= n;
    }

    private void eraseChars(int n) {
        int ri = ring(curRow);
        if (curCol + n > len[ri]) n = len[ri] - curCol;
        blank(ri, curCol, curCol + n);
    }

    private void blank(int ri, int from, int to) {
        if (to > COLS) to = COLS;
        if (from > to) return;
        for (int i = from; i < to; i++) {
            grid[ri][i] = ' ';
            attr[ri][i] = DEF_ATTR;
        }
    }

    private void hardReset() {
        for (int i = 0; i < SCROLLBACK; i++) {
            len[i] = 0;
            java.util.Arrays.fill(attr[i], DEF_ATTR);
        }
        head = 0;
        used = 0;
        serial = 0;
        curRow = curCol = savedRow = savedCol = 0;
        pen = DEF_ATTR;
        state = S_TEXT;
        scroll = 0;
        cursorVisible = true;
        selFrom = selTo = -1;
        fitRows();
    }

    /* --------------------------------------------------------------- lines */

    private int ring(int screenRow) {
        int idx = used - rows + screenRow;
        if (idx < 0) idx = 0;
        return (head + idx) % SCROLLBACK;
    }

    /** The absolute line number a screen row shows, scrolled or not. */
    private long absOf(int screenRow) {
        return serial - rows + screenRow - scroll;
    }

    /** The ring slot for an absolute line, or -1 when it has been dropped. */
    private int ringOfAbs(long abs) {
        long idx = abs - (serial - used);
        if (idx < 0 || idx >= used)
            return -1;
        return (head + (int) idx) % SCROLLBACK;
    }

    private void copyLine(int fromRow, int toRow) {
        int f = ring(fromRow), t = ring(toRow);
        if (f == t) return;
        System.arraycopy(grid[f], 0, grid[t], 0, COLS);
        System.arraycopy(attr[f], 0, attr[t], 0, COLS);
        len[t] = len[f];
    }

    private void appendLine() {
        if (used < SCROLLBACK)
            used++;
        else
            head = (head + 1) % SCROLLBACK;
        serial++;
        int ri = (head + used - 1) % SCROLLBACK;
        len[ri] = 0;
        java.util.Arrays.fill(attr[ri], DEF_ATTR);
        curRow = rows - 1;
    }

    private void fitRows() {
        while (used < rows)
            appendLine();
        curRow = clampRow(curRow);
        curCol = clampCol(curCol);
    }

    public void clear() {
        hardReset();
        invalidate();
    }

    /* -------------------------------------------------------------- drawing */

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        super.onSizeChanged(w, h, ow, oh);
        measureFont();
    }

    private void measureFont() {
        int w = getWidth() - getPaddingLeft() - getPaddingRight();
        int h = getHeight() - getPaddingTop() - getPaddingBottom();
        if (w <= 0 || h <= 0)
            return;
        /* Pick the biggest monospace size whose 80 advances still fit, then lay
         * the cells out on an exact grid so no column can drift. */
        float size = h / 26f;
        for (int i = 0; i < 40; i++) {
            fg.setTextSize(size);
            if (fg.measureText("M") * COLS <= w || size <= 5f)
                break;
            size *= 0.92f;
        }
        Paint.FontMetrics fm = fg.getFontMetrics();
        float adv = fg.measureText("M");
        charW = adv > 0 ? Math.min(w / (float) COLS, adv) : size * 0.6f;
        lineH = (fm.descent - fm.ascent) * 1.12f;
        textBase = (lineH - (fm.descent - fm.ascent)) / 2f - fm.ascent;
        int want = Math.max(6, (int) (h / lineH));
        if (want != rows) {
            rows = want;
            fitRows();
        }
    }

    @Override
    protected void onDraw(Canvas cv) {
        if (charW <= 0f)
            measureFont();
        cv.drawRect(0, 0, getWidth(), getHeight(), bg);
        float x0 = getPaddingLeft();
        float top = getPaddingTop();
        long lo = absOf(0), hi = absOf(rows - 1);
        for (int i = 0; i < rows; i++) {
            int ri = ringOfAbs(absOf(i));
            if (ri < 0)
                continue;
            int n = Math.min(len[ri], COLS);
            float y = top + i * lineH;
            if (selFrom >= 0)
                drawSelection(cv, ri, n, absOf(i), lo, hi, x0, y);
            drawLine(cv, ri, n, x0, y + textBase);
        }
        if (scroll == 0 && cursorVisible && blinkOn) {
            float y = top + (curRow + scroll) * lineH;
            cv.drawRect(x0 + curCol * charW, y, x0 + (curCol + 1) * charW, y + lineH, cursor);
        }
        if (scroll > 0) {
            cv.drawText("scrollback -" + scroll, x0, top + lineH * (rows - 1) + textBase, paint);
        }
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cursor = new Paint();

    private void drawLine(Canvas cv, int ri, int n, float x, float y) {
        /* Draw in runs of equal colour: 80 drawText calls a line is how a
         * terminal gets its frame time back. */
        int start = 0;
        while (start < n) {
            byte a = attr[ri][start];
            int end = start + 1;
            while (end < n && attr[ri][end] == a)
                end++;
            fg.setColor(PAL[a & 0x0F]);
            cv.drawText(grid[ri], start, end, x + start * charW, y, fg);
            start = end;
        }
    }

    private void drawSelection(Canvas cv, int ri, int n, long abs, long lo, long hi,
                               float x0, float y) {
        long a = Math.min(selFrom, selTo), b = Math.max(selFrom, selTo);
        if (abs < a || abs > b)
            return;
        int from = (abs == a) ? selFromCol : 0;
        int to = (abs == b) ? selToCol : n;
        if (to > from)
            cv.drawRect(x0 + from * charW, y, x0 + to * charW, y + lineH, mark);
    }

    private int selFromCol, selToCol;

    /* The one animation in the app: a terminal cursor that blinks.  It stops
     * with the view, so a backgrounded guest costs nothing. */
    private boolean blinkOn = true;
    private final Runnable blink = new Runnable() {
        @Override public void run() {
            blinkOn = !blinkOn;
            invalidate();
            postDelayed(this, 530);
        }
    };

    @Override protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        postDelayed(blink, 530);
    }

    @Override protected void onDetachedFromWindow() {
        removeCallbacks(blink);
        super.onDetachedFromWindow();
    }

    public int getCols() { return COLS; }
    public int getRows() { return rows; }

    /* ------------------------------------------------------------ scrolling */

    /** Page up/down through the scrollback; the guest is not involved. */
    public void scrollPages(int dir) {
        int max = used - rows;
        if (max < 0) max = 0;
        int next = scroll + dir * Math.max(1, rows - 2);
        scroll = next < 0 ? 0 : (next > max ? max : next);
        invalidate();
    }

    public void scrollLines(int dir) {
        int max = used - rows;
        scroll = Math.max(0, Math.min(max < 0 ? 0 : max, scroll + dir));
        invalidate();
    }

    public void scrollToBottom() {
        if (scroll != 0) { scroll = 0; invalidate(); }
    }

    public boolean atBottom() { return scroll == 0; }

    /* ------------------------------------------------------------- selection */

    private void beginSelection(int screenRow, int col) {
        long abs = absOf(clampRow(screenRow));
        int ri = ringOfAbs(abs);
        selFrom = selTo = abs;
        selFromCol = 0;
        selToCol = ri >= 0 ? len[ri] : COLS;
        selecting = true;
        invalidate();
    }

    private void selectLine(int screenRow) {
        beginSelection(screenRow, 0);
    }

    private void extendSelection(int screenRow, int col) {
        if (!selecting)
            return;
        long abs = absOf(clampRow(screenRow));
        int ri = ringOfAbs(abs);
        selTo = abs;
        selToCol = ri >= 0 ? len[ri] : COLS;
        invalidate();
    }

    public void selectAll() {
        if (used == 0)
            return;
        selFrom = serial - used;
        selTo = serial - 1;
        selFromCol = 0;
        selToCol = COLS;
        selecting = false;
        invalidate();
    }

    private String selectionText() {
        long a = Math.min(selFrom, selTo), b = Math.max(selFrom, selTo);
        StringBuilder sb = new StringBuilder();
        for (long abs = a; abs <= b; abs++) {
            int ri = ringOfAbs(abs);
            if (ri < 0)
                continue;
            int from = (abs == a) ? selFromCol : 0;
            int to = (abs == b) ? selToCol : len[ri];
            to = Math.min(to, len[ri]);
            while (to > from && grid[ri][to - 1] == ' ')
                to--;
            for (int i = from; i < to; i++)
                sb.append(grid[ri][i] == 0 ? ' ' : grid[ri][i]);
            if (abs != b)
                sb.append('\n');
        }
        return sb.toString();
    }

    private void copySelection(boolean quietIfEmpty) {
        if (selFrom < 0)
            selectAll();
        String text = selectionText();
        ClipboardManager cm = (ClipboardManager)
            getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null)
            return;
        if (text.length() == 0) {
            if (!quietIfEmpty)
                Toast.makeText(getContext(), "nothing selected", Toast.LENGTH_SHORT).show();
            return;
        }
        cm.setPrimaryClip(ClipData.newPlainText("rvm", text));
        Toast.makeText(getContext(), "copied " + text.length() + " chars", Toast.LENGTH_SHORT).show();
    }

    private void pasteClipboard() {
        ClipboardManager cm = (ClipboardManager)
            getContext().getSystemService(Context.CLIPBOARD_SERVICE);
        if (cm == null || !cm.hasPrimaryClip() || keys == null) {
            Toast.makeText(getContext(), "clipboard empty", Toast.LENGTH_SHORT).show();
            return;
        }
        ClipData clip = cm.getPrimaryClip();
        if (clip == null || clip.getItemCount() == 0)
            return;
        CharSequence text = clip.getItemAt(0).coerceToText(getContext());
        if (text == null)
            return;
        scrollToBottom();
        int n = Math.min(text.length(), 8192);
        for (int i = 0; i < n; i++) {
            char ch = text.charAt(i);
            if (ch == '\n')
                keys.onKey((byte) '\r');
            else if (ch < 128)
                keys.onKey((byte) ch);
        }
    }

    private void openMenu() {
        PopupMenu menu = new PopupMenu(getContext(), this);
        menu.getMenu().add("Paste");
        menu.getMenu().add("Copy");
        menu.getMenu().add("Select all");
        menu.getMenu().add("Clear");
        menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
            @Override public boolean onMenuItemClick(android.view.MenuItem item) {
                String t = String.valueOf(item.getTitle());
                if ("Paste".equals(t)) pasteClipboard();
                else if ("Copy".equals(t)) copySelection(false);
                else if ("Select all".equals(t)) { selectAll(); copySelection(false); }
                else if ("Clear".equals(t)) clear();
                selecting = false;
                return true;
            }
        });
        menu.show();
    }

    /* -------------------------------------------------------------- keyboard */

    @Override
    public boolean onKeyDown(int code, KeyEvent ev) {
        if (keys == null) return super.onKeyDown(code, ev);
        switch (code) {
            case KeyEvent.KEYCODE_ENTER:      keys.onKey((byte) '\r'); return true;
            case KeyEvent.KEYCODE_DEL:        keys.onKey((byte) 0x7F); return true;
            case KeyEvent.KEYCODE_FORWARD_DEL: keys.onKey((byte) 0x1b); keys.onKey((byte) '['); keys.onKey((byte) '3'); keys.onKey((byte) '~'); return true;
            case KeyEvent.KEYCODE_TAB:        keys.onKey((byte) '\t'); return true;
            case KeyEvent.KEYCODE_ESCAPE:     keys.onKey((byte) 27);   return true;
            case KeyEvent.KEYCODE_DPAD_UP:    sendCsi('A'); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN:  sendCsi('B'); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: sendCsi('C'); return true;
            case KeyEvent.KEYCODE_DPAD_LEFT:  sendCsi('D'); return true;
            default: break;
        }
        int u = ev.getUnicodeChar();
        if (u >= 32 && u < 127) { keys.onKey((byte) u); return true; }
        return super.onKeyDown(code, ev);
    }

    private void sendCsi(char what) {
        keys.onKey((byte) 0x1b);
        keys.onKey((byte) '[');
        keys.onKey((byte) what);
    }

    /** Send a raw escape sequence from the helper keys. */
    public void sendBytes(byte[] seq) {
        if (keys == null || seq == null) return;
        scrollToBottom();
        for (byte b : seq) keys.onKey(b);
    }

    @Override
    public boolean onCheckIsTextEditor() { return true; }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo out) {
        out.inputType = InputType.TYPE_CLASS_TEXT
                      | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                      | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD;
        out.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN
                       | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                       | EditorInfo.IME_ACTION_NONE;
        out.privateImeOptions = null;
        return new BaseInputConnection(this, false) {
            @Override
            public boolean commitText(CharSequence text, int newCursorPosition) {
                if (text == null) return true;
                scrollToBottom();
                int n = Math.min(text.length(), 512);
                for (int i = 0; i < n; i++) {
                    char ch = text.charAt(i);
                    if (ch == '\n') keys.onKey((byte) '\r');
                    else if (ch < 128) keys.onKey((byte) ch);
                }
                return true;
            }

            @Override
            public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                /* Backspace is one DEL, not one per character the IME thinks it
                 * removed: the guest owns the line and echoes what it deleted. */
                if (keys != null && beforeLength > 0)
                    keys.onKey((byte) 0x7F);
                return true;
            }

            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                if (event.getAction() == KeyEvent.ACTION_DOWN)
                    return RvmView.this.onKeyDown(event.getKeyCode(), event);
                return true;
            }

            @Override
            public boolean setComposingText(CharSequence text, int newCursorPosition) {
                return true;      /* no composing: a terminal has no such thing */
            }
        };
    }

    private void showIme(boolean show) {
        InputMethodManager imm = (InputMethodManager)
            getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
        if (imm == null)
            return;
        if (show) imm.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        else imm.hideSoftInputFromWindow(getWindowToken(), 0);
    }

    public void hideIme() { showIme(false); }

    /** A tap raises the keyboard; a drag while selecting extends the selection. */
    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (selecting) {
            switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
                extendSelection((int) ((e.getY() - getPaddingTop()) / lineH),
                                (int) ((e.getX() - getPaddingLeft()) / charW));
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                selecting = false;
                openMenu();
                return true;
            default:
                return true;
            }
        }
        gestures.onTouchEvent(e);
        return true;
    }
}
