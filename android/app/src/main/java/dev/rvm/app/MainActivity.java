/*
 * MainActivity -- the whole UI.
 *
 * Extends android.app.Activity on purpose: no AppCompat, no Material, no
 * Compose, no Fragment manager.  The screen is one vertical LinearLayout with
 * the console, an (initially hidden) framebuffer, the helper-key row and a
 * three-button toolbar.  Nothing animates and nothing casts a shadow.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.Button;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class MainActivity extends Activity {

    private static final int PICK_KERNEL = 1;
    private static final int PICK_DISK = 2;

    private RvmView console;
    private GfxView gfx;
    private KeyBar keys;
    private TextView status;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile long vm;
    private volatile Thread vmThread;
    private volatile boolean sawOutput;
    private boolean debug;
    private File kernel, disk;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        setContentView(R.layout.activity_main);

        console = findViewById(R.id.console);
        gfx = findViewById(R.id.gfx);
        keys = findViewById(R.id.keys);
        status = findViewById(R.id.status);

        kernel = new File(getFilesDir(), "vmlinux");
        disk = new File(getFilesDir(), "disk.img");

        debug = getSharedPreferences("rvm", MODE_PRIVATE).getBoolean("debug", false);
        applyLogSink();
        applyInsets();

        /* The one hidden gesture in the app, and the only way to get a log
         * out of a phone without adb: hold the RVM title. */
        findViewById(R.id.title).setOnLongClickListener(new View.OnLongClickListener() {
            @Override public boolean onLongClick(View v) {
                debug = !debug;
                getSharedPreferences("rvm", MODE_PRIVATE).edit()
                    .putBoolean("debug", debug).apply();
                applyLogSink();
                toast(debug ? "debug log on: boot again and it will be captured"
                            : "debug log off");
                return true;
            }
        });

        wireKeys();
        wireToolbar();

        console.setKeyListener(new RvmView.KeyListener() {
            @Override public void onKey(byte ch) { send(new byte[] { ch }); }
        });

        gfx.setPointerListener(new GfxView.PointerListener() {
            @Override public void onMove(float nx, float ny) { /* virtio-input, PLAN step 10 */ }
            @Override public void onButton(int button, boolean down) { /* ditto */ }
        });

        RvmNative.consoleSink = new RvmNative.ConsoleSink() {
            @Override public void onOutput(final byte[] buf, final int len) {
                sawOutput = true;
                final byte[] copy = new byte[len];
                System.arraycopy(buf, 0, copy, 0, len);
                ui.post(new Runnable() {
                    @Override public void run() { console.write(copy, len); }
                });
            }
        };
        RvmNative.frameSink = new RvmNative.FrameSink() {
            @Override public void onFrame(final byte[] rgba, final int w, final int h, final int s) {
                ui.post(new Runnable() {
                    @Override public void run() { gfx.setFrame(rgba, w, h, s); }
                });
            }
        };
        RvmNative.exitSink = new RvmNative.ExitSink() {
            @Override public void onExit(final int code) {
                ui.post(new Runnable() {
                    @Override public void run() {
                        status.setText("guest exited (" + code + ")");
                    }
                });
            }
        };

        console.clear();
        if (!kernel.exists()) {
            banner("No kernel yet.\n\nTap 'Kernel' and pick a riscv64 kernel, either the "
                 + "ELF vmlinux or the PE/EFI Image that Debian ships as /boot/vmlinux-*, "
                 + "then 'Disk' and pick a Debian ext4 image built with tools/mkrootfs.sh, "
                 + "then 'Boot'.\n\nLong-press the RVM title for a debug log.");
        } else {
            banner("Ready.\n  kernel: " + human(kernel.length())
                 + (disk.exists() ? "\n  disk:   " + human(disk.length()) : "\n  disk:   (none)")
                 + "\n\nTap 'Boot'.  Long-press the RVM title for a debug log.");
        }
    }

    /*
     * The header is ink and the status bar is ink, so the header's top padding
     * has to grow by the status bar height or the title sits under the clock.
     * fitsSystemWindows on the root would pad the whole window in paper and
     * leave a pale seam above the bar, hence doing it by hand.
     */
    private void applyInsets() {
        final View header = findViewById(R.id.header);
        final View keyscroll = findViewById(R.id.keyscroll);
        header.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets in) {
                v.setPadding(v.getPaddingLeft(), in.getSystemWindowInsetTop(),
                             v.getPaddingRight(), v.getPaddingBottom());
                return in;
            }
        });
        keyscroll.setOnApplyWindowInsetsListener(new View.OnApplyWindowInsetsListener() {
            @Override public WindowInsets onApplyWindowInsets(View v, WindowInsets in) {
                v.setPadding(v.getPaddingLeft(), v.getPaddingTop(),
                             v.getPaddingRight(), in.getSystemWindowInsetBottom());
                return in;
            }
        });
    }

    /**
     * RVM's own log, straight into the console above the guest output.  The
     * guest console and the emulator log share one scrollback on purpose:
     * when a boot goes wrong the interesting part is where they meet.
     */
    private void applyLogSink() {
        RvmNative.logSink = debug ? new RvmNative.LogSink() {
            @Override public void onLog(final int level, final String line) {
                final char t = level <= 0 ? 'T' : level == 1 ? 'D' : level == 2 ? 'I'
                                 : level == 3 ? 'W' : 'E';
                final byte[] b = ("[rvm " + t + "] " + line + "\n").getBytes();
                ui.post(new Runnable() {
                    @Override public void run() { console.write(b, b.length); }
                });
            }
        } : null;
    }

    /* -------------------------------------------------------------- wiring */

    private void wireKeys() {
        keys.setSender(new KeyBar.Sender() {
            @Override public void send(byte[] seq) {
                if (keys.isCtrlLatched() && seq.length == 1 && seq[0] >= 'a' && seq[0] <= 'z') {
                    seq = new byte[] { (byte) (seq[0] - 'a' + 1) };
                    keys.releaseCtrl();
                }
                MainActivity.this.send(seq);
            }
        });
        keys.toggleIme = new Runnable() {
            @Override public void run() {
                InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
                if (imm == null) return;
                if (imm.isActive(console)) imm.hideSoftInputFromWindow(console.getWindowToken(), 0);
                else { console.requestFocus(); imm.showSoftInput(console, 0); }
            }
        };
    }

    private void wireToolbar() {
        Button boot = findViewById(R.id.btn_boot);
        Button pickKernel = findViewById(R.id.btn_kernel);
        Button pickDisk = findViewById(R.id.btn_disk);
        Button stop = findViewById(R.id.btn_stop);

        boot.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { startVm(); }
        });
        stop.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { stopVm(); }
        });
        pickKernel.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pick(PICK_KERNEL); }
        });
        pickDisk.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { pick(PICK_DISK); }
        });
    }

    private void send(byte[] seq) {
        long h = vm;
        if (h == 0 || seq == null || seq.length == 0) return;
        RvmNative.vmInput(h, seq, seq.length);
    }

    /* ------------------------------------------------------------ run/stop */

    private void startVm() {
        if (vmThread != null) { toast("already running"); return; }
        if (!kernel.exists()) { toast("pick a kernel first"); return; }

        console.clear();
        sawOutput = false;
        status.setText(debug ? "starting, debug log on…" : "starting…");
        ui.postDelayed(new Runnable() {
            @Override public void run() {
                if (vm != 0 && !sawOutput) {
                    status.setText("running, no output from the guest yet");
                }
            }
        }, 8000);
        final String k = kernel.getAbsolutePath();
        final String d = disk.exists() ? disk.getAbsolutePath() : null;
        final String bootargs = "console=ttyS0 earlycon=ns16550a,mmio32,0x10000000 root=/dev/vda rootwait rw init=/bin/sh";

        vmThread = new Thread(new Runnable() {
            @Override public void run() {
                long h = RvmNative.vmCreate(k, d, null, 512, bootargs, debug);
                if (h == 0) {
                    ui.post(new Runnable() {
                        @Override public void run() {
                            status.setText("failed to create the VM");
                            vmThread = null;
                        }
                    });
                    return;
                }
                vm = h;
                ui.post(new Runnable() {
                    @Override public void run() { status.setText("running"); }
                });
                final int code = RvmNative.vmRun(h);
                vm = 0;
                RvmNative.vmFree(h);
                ui.post(new Runnable() {
                    @Override public void run() {
                        status.setText("stopped (" + code + ")");
                        vmThread = null;
                    }
                });
            }
        }, "rvm-vm");
        /* NORM, not MAX: the VM thread never blocks, and at MAX_PRIORITY it
         * starves the UI thread on a phone's few cores, which reads as the
         * whole app freezing. */
        vmThread.setPriority(Thread.NORM_PRIORITY);
        vmThread.start();
    }

    private void stopVm() {
        long h = vm;
        if (h != 0) RvmNative.vmStop(h);
        else toast("not running");
    }

    /* --------------------------------------------------------- file picking */

    private void pick(int what) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, what);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (res != RESULT_OK || data == null || data.getData() == null) return;
        final Uri uri = data.getData();
        final File dest = (req == PICK_KERNEL) ? kernel : disk;
        status.setText("copying " + dest.getName() + "…");
        new Thread(new Runnable() {
            @Override public void run() {
                final boolean ok = copyIn(uri, dest);
                final long size = dest.length();
                ui.post(new Runnable() {
                    @Override public void run() {
                        status.setText(ok ? dest.getName() + ": " + human(size)
                                          : "copy failed");
                    }
                });
            }
        }, "rvm-copy").start();
    }

    private boolean copyIn(Uri uri, File dest) {
        InputStream in = null;
        OutputStream out = null;
        try {
            in = getContentResolver().openInputStream(uri);
            if (in == null) return false;
            File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
            out = new FileOutputStream(tmp);
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.close();
            out = null;
            if (dest.exists() && !dest.delete()) return false;
            return tmp.renameTo(dest);
        } catch (Exception e) {
            return false;
        } finally {
            close(in); close(out);
        }
    }

    /* ---------------------------------------------------------------- misc */

    private void banner(String s) { console.write(s.getBytes(), s.getBytes().length); }

    private void toast(String s) {
        Toast t = Toast.makeText(this, s, Toast.LENGTH_SHORT);
        t.setGravity(Gravity.CENTER, 0, 0);
        t.show();
    }

    private static String human(long n) {
        if (n >= 1L << 30) return String.format("%.1f GiB", n / (double) (1L << 30));
        if (n >= 1L << 20) return String.format("%.1f MiB", n / (double) (1L << 20));
        if (n >= 1L << 10) return String.format("%.0f KiB", n / (double) (1L << 10));
        return n + " B";
    }

    private static void close(java.io.Closeable c) {
        if (c != null) try { c.close(); } catch (Exception ignored) { }
    }

    @Override
    protected void onDestroy() {
        stopVm();
        RvmNative.logSink = null;
        RvmNative.consoleSink = null;
        RvmNative.frameSink = null;
        RvmNative.exitSink = null;
        super.onDestroy();
    }
}
