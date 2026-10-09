/*
 * KeyBar -- the row of helper keys a soft keyboard does not provide.
 *
 * Plain LinearLayout with plain Buttons: no Material, no AppCompat, no
 * animation.  The keys are small but stay above 40dp so they remain touchable,
 * and the icons are single-stroke vectors.
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

    private Sender sender;

    /** What a key press turns into on the guest console. */
    public interface Sender { void send(byte[] seq); }

    public KeyBar(Context c) { this(c, null); }

    public KeyBar(Context c, AttributeSet a) {
        super(c, a);
        setOrientation(HORIZONTAL);
        setGravity(Gravity.CENTER_VERTICAL);
        setBackgroundColor(0xFFEDEBE6);
        setPadding(dp(4), dp(2), dp(4), dp(2));

        add(textKey("Ctrl", new byte[] { 0 }, true));
        add(textKey("Esc", new byte[] { 27 }, false));
        add(textKey("Tab", new byte[] { '\t' }, false));
        add(arrowKey(R.drawable.ic_arrow_up, new byte[] { 27, '[', 'A' }));
        add(arrowKey(R.drawable.ic_arrow_down, new byte[] { 27, '[', 'B' }));
        add(arrowKey(R.drawable.ic_arrow_left, new byte[] { 27, '[', 'D' }));
        add(arrowKey(R.drawable.ic_arrow_right, new byte[] { 27, '[', 'C' }));
        add(textKey("^C", new byte[] { 3 }, false));
        add(textKey("^D", new byte[] { 4 }, false));
        add(textKey("^L", new byte[] { 12 }, false));
        add(iconKey(R.drawable.ic_keyboard, null));
    }

    public void setSender(Sender s) { sender = s; }

    private void emit(final byte[] seq) {
        if (sender != null && seq != null) sender.send(seq);
    }

    private Button base() {
        Button b = new Button(getContext());
        LayoutParams lp = new LayoutParams(LayoutParams.WRAP_CONTENT, dp(42));
        lp.setMargins(dp(2), 0, dp(2), 0);
        b.setLayoutParams(lp);
        b.setMinWidth(dp(44));
        b.setMinHeight(dp(42));
        b.setPadding(dp(8), 0, dp(8), 0);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        b.setTextColor(0xFF2B2B28);
        b.setBackgroundColor(0xFFF5F4F0);
        b.setGravity(Gravity.CENTER);
        return b;
    }

    private View textKey(String label, final byte[] seq, final boolean toggle) {
        final Button b = base();
        b.setText(label);
        b.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) {
                if (toggle) {
                    b.setActivated(!b.isActivated());
                    b.setBackgroundColor(b.isActivated() ? 0xFF2B2B28 : 0xFFF5F4F0);
                    b.setTextColor(b.isActivated() ? 0xFFF5F4F0 : 0xFF2B2B28);
                    /* Ctrl stays latched: the next letter is sent as a control
                     * code by MainActivity, which reads isCtrlLatched(). */
                } else {
                    emit(seq);
                }
            }
        });
        return b;
    }

    private View arrowKey(int icon, final byte[] seq) {
        Button b = base();
        Drawable d = getContext().getDrawable(icon);
        if (d != null) {
            d.setBounds(0, 0, dp(20), dp(20));
            b.setCompoundDrawables(d, null, null, null);
        }
        b.setMinWidth(dp(42));
        b.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) { emit(seq); }
        });
        return b;
    }

    private View iconKey(int icon, final byte[] seq) {
        Button b = base();
        Drawable d = getContext().getDrawable(icon);
        if (d != null) {
            d.setBounds(0, 0, dp(22), dp(22));
            b.setCompoundDrawables(d, null, null, null);
        }
        b.setOnClickListener(new OnClickListener() {
            @Override public void onClick(View v) {
                if (seq != null) { emit(seq); return; }
                if (toggleIme != null) toggleIme.run();
            }
        });
        return b;
    }

    /** Set by MainActivity to raise/lower the soft keyboard. */
    public Runnable toggleIme;

    /** True while the Ctrl key is latched down. */
    public boolean isCtrlLatched() {
        for (int i = 0; i < getChildCount(); i++) {
            View v = getChildAt(i);
            if (v instanceof Button && ((Button) v).isActivated()) return true;
        }
        return false;
    }

    public void releaseCtrl() {
        for (int i = 0; i < getChildCount(); i++) {
            View v = getChildAt(i);
            if (v instanceof Button && ((Button) v).isActivated()) {
                v.setActivated(false);
                v.setBackgroundColor(0xFFF5F4F0);
                ((Button) v).setTextColor(0xFF2B2B28);
            }
        }
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }
}
