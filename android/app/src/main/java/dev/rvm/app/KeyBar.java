/*
 * KeyBar -- the helper keys a soft keyboard does not have.
 *
 * Plain LinearLayout with plain Buttons: no Material, no AppCompat, no
 * animation.  The bar is hidden unless the IME is up, which is what Termux
 * does and the only way to keep an 80-column console readable on a phone: the
 * keys are there when you are typing and gone when you are reading.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;

public final class KeyBar extends LinearLayout {

    /** Where a key press goes: bytes to the guest, or a scroll of the view. */
    public interface Sink {
        void send(byte[] seq);
        void scrollPages(int dir);
        void hideIme();
    }

    private Sink sink;
    private Button ctrl, alt;

    public KeyBar(Context c) { this(c, null); }

    public KeyBar(Context c, AttributeSet a) {
        super(c, a);
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setPadding(dp(4), 0, dp(4), 0);

        ctrl = (Button) add(text("Ctrl", null));
        alt = (Button) add(text("Alt", null));
        add(text("Esc", new byte[] { 27 }));
        add(text("Tab", new byte[] { '\t' }));
        add(text("PgUp", null, SCROLL_UP));
        add(text("Home", null, TO_BOTTOM));
        add(arrow(R.drawable.ic_arrow_up, new byte[] { 27, '[', 'A' }));
        add(arrow(R.drawable.ic_arrow_down, new byte[] { 27, '[', 'B' }));
        add(arrow(R.drawable.ic_arrow_left, new byte[] { 27, '[', 'D' }));
        add(arrow(R.drawable.ic_arrow_right, new byte[] { 27, '[', 'C' }));
        add(text("End", null, TO_BOTTOM));
        add(text("PgDn", null, SCROLL_DOWN));
        add(text("^C", new byte[] { 3 }, true));
        add(text("^D", new byte[] { 4 }, true));
        add(text("^L", new byte[] { 12 }, true));
        add(icon(R.drawable.ic_keyboard, null, HIDE_IME));
    }

    public void setSink(Sink s) { sink = s; }

    private static final int SCROLL_UP = 1, SCROLL_DOWN = 2, TO_BOTTOM = 3, HIDE_IME = 4;

    /** True while Ctrl is latched; MainActivity turns the next letter into ^X. */
    public boolean isCtrlLatched() { return ctrl != null && ctrl.isActivated(); }

    /** True while Alt is latched; the next key goes out as ESC followed by it. */
    public boolean isAltLatched() { return alt != null && alt.isActivated(); }

    public void releaseLatches() {
        latch(ctrl, false);
        latch(alt, false);
    }

    private void latch(Button b, boolean on) {
        if (b == null) return;
        b.setActivated(on);
        b.setTextColor(on ? 0xFFFFFFFF : 0xFFB0B0B0);
    }

    private View make(String label, int icon, final byte[] seq, final int special,
                      final boolean clearLatches) {
        Button b = new Button(getContext());
        LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, dp(42));
        lp.setMargins(dp(2), dp(4), dp(2), dp(4));
        b.setLayoutParams(lp);
        b.setBackgroundResource(R.drawable.bg_key);
        b.setMinWidth(dp(46));
        b.setMinHeight(dp(42));
        b.setPadding(dp(9), 0, dp(9), 0);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12.5f);
        b.setTextColor(0xFFB0B0B0);
        b.setGravity(Gravity.CENTER);
        b.setStateListAnimator(null);
        b.setElevation(0);
        if (label != null) b.setText(label);
        if (icon != 0) {
            Drawable d = getContext().getDrawable(icon);
            if (d != null) {
                d.setBounds(0, 0, dp(19), dp(19));
                b.setCompoundDrawables(d, null, null, null);
                b.setMinWidth(dp(40));
            }
        }
        b.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) {
                if (sink == null) return;
                if (v == ctrl || v == alt) {
                    boolean on = !v.isActivated();
                    latch(ctrl == v ? ctrl : alt, on);
                    return;
                }
                if (clearLatches) {
                    boolean wasCtrl = isCtrlLatched();
                    boolean wasAlt = isAltLatched();
                    releaseLatches();
                    if (seq == null) return;
                    if (wasAlt) sink.send(new byte[] { 27 });
                    if (wasCtrl && seq.length == 1 && seq[0] >= 'a' && seq[0] <= 'z') {
                        sink.send(new byte[] { (byte) (seq[0] - 'a' + 1) });
                        return;
                    }
                }
                switch (special) {
                case SCROLL_UP:   sink.scrollPages(-1); return;
                case SCROLL_DOWN: sink.scrollPages(1); return;
                case TO_BOTTOM:   sink.scrollPages(0); return;
                case HIDE_IME:    sink.hideIme(); return;
                default: break;
                }
                if (seq != null) sink.send(seq);
            }
        });
        return b;
    }

    private View text(String label, byte[] seq) { return text(label, seq, 0, false); }
    private View text(String label, byte[] seq, int special) { return text(label, seq, special, false); }
    private View text(String label, byte[] seq, boolean clear) {
        return text(label, seq, 0, clear);
    }

    private View text(String label, byte[] seq, int special, boolean clearLatches) {
        return make(label, 0, seq, special, clearLatches);
    }

    private View arrow(int icon, byte[] seq) {
        return make(null, icon, seq, 0, true);
    }

    private View icon(int icon, byte[] seq, int special) {
        return make(null, icon, seq, special, false);
    }

    private View add(View v) {
        addView(v);
        return v;
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
