/*
 * GfxView -- the guest framebuffer, drawn on a SurfaceView.
 *
 * virtio-gpu hands us RGBA8888 scanouts; we blit them onto a Surface locked
 * canvas.  A SurfaceView rather than a TextureView because it composites on a
 * separate hardware layer, which costs no extra memory and no extra pass --
 * exactly what "lightweight" means here.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

public final class GfxView extends SurfaceView implements SurfaceHolder.Callback {

    private final Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG);
    private Bitmap frame;
    private int frameW, frameH;
    private boolean ready;
    private boolean visible = true;

    private PointerListener pointer;

    /** Normalised pointer events, forwarded to virtio-input in the guest. */
    public interface PointerListener {
        void onMove(float nx, float ny);   /* 0..1 across the surface */
        void onButton(int button, boolean down);
    }

    public GfxView(Context c) { this(c, null); }

    public GfxView(Context c, AttributeSet a) {
        super(c, a);
        getHolder().addCallback(this);
        setWillNotDraw(false);
        /* A framebuffer is only useful once the guest has one; stay out of the
         * way until then so the console owns the screen. */
        setVisibility(GONE);
    }

    public void setPointerListener(PointerListener l) { pointer = l; }

    /* ---------------------------------------------------------- frames in */

    /** Called from the VM thread with a fresh RGBA8888 scanout. */
    public void setFrame(byte[] rgba, int w, int h, int stride) {
        if (w <= 0 || h <= 0) return;
        if (frame == null || frameW != w || frameH != h) {
            if (frame != null) frame.recycle();
            frame = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            frameW = w;
            frameH = h;
        }
        /* Android wants packed ARGB ints; the guest gives little-endian RGBA
         * bytes, which is the same word on both sides, so a bulk copy works. */
        int[] px = new int[w * h];
        int n = Math.min(px.length * 4, rgba.length);
        for (int i = 0; i + 3 < n; i += 4) {
            int r = rgba[i] & 0xFF, g = rgba[i + 1] & 0xFF;
            int b = rgba[i + 2] & 0xFF, a = rgba[i + 3] & 0xFF;
            px[i >> 2] = (a << 24) | (r << 16) | (g << 8) | b;
        }
        frame.setPixels(px, 0, w, 0, 0, w, h);
        if (stride != w * 4) { /* padded scanout: nothing to do, we copied w*4 */ }

        if (!visible) {
            visible = true;
            post(new Runnable() { public void run() { setVisibility(VISIBLE); } });
        }
        draw();
    }

    private void draw() {
        if (!ready || frame == null) return;
        Canvas cv = null;
        SurfaceHolder h = getHolder();
        try {
            cv = h.lockCanvas();
            if (cv == null) return;
            cv.drawColor(0xFF2B2B28);
            Rect src = new Rect(0, 0, frameW, frameH);
            Rect dst = fit(frameW, frameH, cv.getWidth(), cv.getHeight());
            cv.drawBitmap(frame, src, dst, paint);
        } finally {
            if (cv != null) {
                try { h.unlockCanvasAndPost(cv); } catch (RuntimeException ignored) { }
            }
        }
    }

    /** Letterbox: keep the guest's aspect ratio, never stretch. */
    private static Rect fit(int sw, int sh, int dw, int dh) {
        float s = Math.min((float) dw / sw, (float) dh / sh);
        int w = (int) (sw * s), h = (int) (sh * s);
        int x = (dw - w) / 2, y = (dh - h) / 2;
        return new Rect(x, y, x + w, y + h);
    }

    /* -------------------------------------------------------- surface cb */

    @Override public void surfaceCreated(SurfaceHolder holder) { ready = true; draw(); }
    @Override public void surfaceChanged(SurfaceHolder h, int fmt, int w, int hh) { ready = true; draw(); }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        ready = false;
        if (frame != null) { frame.recycle(); frame = null; }
    }

    /* ---------------------------------------------------------- pointer */

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (pointer == null) return false;
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                pointer.onButton(1, true);
                pointer.onMove(e.getX() / w, e.getY() / h);
                return true;
            case MotionEvent.ACTION_MOVE:
                pointer.onMove(e.getX() / w, e.getY() / h);
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                pointer.onButton(1, false);
                return true;
            default:
                return false;
        }
    }
}
