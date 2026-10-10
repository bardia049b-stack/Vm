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
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.PopupMenu;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedInputStream;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.GZIPInputStream;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

public final class MainActivity extends Activity {

    private static final int PICK_KERNEL = 1;
    private static final int PICK_DISK = 2;
    private static final int PICK_INITRD = 3;

    private RvmView console;
    private GfxView gfx;
    private KeyBar keys;
    private TextView status;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private volatile long vm;
    private volatile Thread vmThread;
    private volatile boolean sawOutput;
    /* True while a picked file is still being streamed into our own directory.
     * Boot waits for it: starting a VM on a half written image is a hang that
     * looks like an emulator bug. */
    private volatile boolean copying;
    /* The machine, from the `ram=1536 cpu=1` line in menu -> Machine. */
    private int ramMib = 1536;
    private int harts = 1;
    private boolean shellFirst;
    private String specRaw = "ram=1536 cpu=1";
    private boolean debug;
    private File kernel, disk, initrd;

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
        initrd = new File(getFilesDir(), "initrd.img");
        ensureBuiltinInitrd();

        debug = getSharedPreferences("rvm", MODE_PRIVATE).getBoolean("debug", false);
        applySpec(loadSpec());
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
        wireMenu();
        trackIme();

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
            banner("No kernel yet.\n\nOpen the menu (top right) and pick Kernel: a riscv64 kernel, "
                 + "either the ELF vmlinux or the Image Debian ships as /boot/vmlinux-*. "
                 + "Then Disk, then Boot.  The initramfs is already in the app.\n\n"
                 + "Long-press the RVM title for a debug log.\n");
        } else {
            banner("Ready.\n  kernel: " + human(kernel.length())
                 + "\n  initrd: " + (initrd.exists() ? human(initrd.length()) : "(none)")
                 + "\n  disk:   " + (disk.exists() ? human(disk.length()) : "(none)")
                 + "\n\nMenu (top right) -> Boot.  Tap the screen for the keyboard, "
                 + "long-press to copy and paste.");
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
        keys.setSink(new KeyBar.Sink() {
            @Override public void send(byte[] seq) {
                boolean alt = keys.isAltLatched();
                boolean ctrl = keys.isCtrlLatched();
                keys.releaseLatches();
                if (alt) MainActivity.this.send(new byte[] { 27 });
                if (ctrl && seq.length == 1 && seq[0] >= 'a' && seq[0] <= 'z') {
                    MainActivity.this.send(new byte[] { (byte) (seq[0] - 'a' + 1) });
                    return;
                }
                MainActivity.this.send(seq);
            }

            @Override public void scrollPages(int dir) {
                if (dir == 0) console.scrollToBottom();
                else console.scrollPages(dir);
            }

            @Override public void hideIme() {
                setImeVisible(false);
            }
        });
    }

    /*
     * Termux keeps its extra-keys row attached to the keyboard, not to the
     * activity: keys while you type, the whole screen while you read.  There is
     * no API for "the IME is open" below API 30 that every device answers the
     * same way, so this measures the visible frame instead -- the window loses
     * more than a fifth of its height exactly when a keyboard slides in.
     */
    private void trackIme() {
        final View root = findViewById(R.id.root);
        root.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View v, int l, int t, int r, int b,
                                                 int ol, int ot, int or, int ob) {
                if (b == ot && r == or) return;
                android.graphics.Rect frame = new android.graphics.Rect();
                root.getWindowVisibleDisplayFrame(frame);
                int hidden = b - frame.bottom;
                boolean open = hidden > b / 4;
                if (open != imeOpen) {
                    imeOpen = open;
                    setKeysVisible(open);
                }
            }
        });
    }

    private boolean imeOpen;

    private void setKeysVisible(boolean show) {
        findViewById(R.id.keys).setVisibility(show ? View.VISIBLE : View.GONE);
        findViewById(R.id.keyscroll).setVisibility(show ? View.VISIBLE : View.GONE);
        findViewById(R.id.keyrule).setVisibility(show ? View.VISIBLE : View.GONE);
    }

    private void setImeVisible(boolean show) {
        InputMethodManager imm = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm == null) return;
        if (show) { console.requestFocus(); imm.showSoftInput(console, 0); }
        else imm.hideSoftInputFromWindow(console.getWindowToken(), 0);
    }

    /*
     * Everything that used to be a button in a row is a menu item now, plus a
     * couple that never had a home: Clear, and the debug-log switch that was
     * only reachable by a long-press nobody finds.
     */
    private void wireMenu() {
        findViewById(R.id.menu).setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { openMenu(); }
        });
    }

    private void openMenu() {
        PopupMenu menu = new PopupMenu(this, findViewById(R.id.menu));
        /* add(resId) alone would give every item id 0, so the resource doubles
         * as the item id: that is what onMenuItemClick compares on. */
        int act = (vmThread != null) ? R.string.stop : R.string.boot;
        int dbg = debug ? R.string.debug_on : R.string.debug_off;
        int order = 0;
        menu.getMenu().add(0, act, order++, act);
        menu.getMenu().add(0, R.string.kernel, order++, R.string.kernel);
        menu.getMenu().add(0, R.string.disk, order++, R.string.disk);
        menu.getMenu().add(0, R.string.initrd, order++, R.string.initrd);
        menu.getMenu().add(0, R.string.machine, order++, R.string.machine);
        menu.getMenu().add(0, R.string.clear, order++, R.string.clear);
        menu.getMenu().add(0, dbg, order++, dbg);
        menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
            @Override public boolean onMenuItemClick(android.view.MenuItem item) {
                int id = item.getItemId();
                if (id == R.string.boot || id == R.string.stop) {
                    if (vmThread != null) stopVm(); else startVm();
                } else if (id == R.string.kernel) pick(PICK_KERNEL);
                else if (id == R.string.disk) pick(PICK_DISK);
                else if (id == R.string.initrd) pick(PICK_INITRD);
                else if (id == R.string.machine) askMachine();
                else if (id == R.string.clear) console.clear();
                else if (id == R.string.debug_on || id == R.string.debug_off) {
                    debug = !debug;
                    getSharedPreferences("rvm", MODE_PRIVATE).edit()
                        .putBoolean("debug", debug).apply();
                    applyLogSink();
                    toast(debug ? "debug log on: boot again and it will be captured"
                                : "debug log off");
                }
                return true;
            }
        });
        menu.show();
    }

    private void send(byte[] seq) {
        long h = vm;
        if (h == 0 || seq == null || seq.length == 0) return;
        RvmNative.vmInput(h, seq, seq.length);
    }

    /* ------------------------------------------------------------ run/stop */

    /*
     * The APK ships a busybox initramfs as an asset so a kernel alone boots
     * to a shell with a working tty.  Copied out on first run; a file the
     * user picks later simply replaces it.
     */
    /* The initramfs is behaviour, not bulk: the lease script, the hand-off into
     * the disk and rvm.shell=root all live in its /init.  Unpacking it only when
     * the file is missing means an install that has ever booted keeps the /init of
     * whatever APK it first came with, and a flag that /init does not know about is
     * not rejected, it is simply not acted on - which is the worst possible
     * failure, because the app then looks like it is ignoring its own settings.
     * So the asset carries a revision and re-unpacks when they disagree: 1.6 MB
     * against a silent misboot. */
    private static final int INITRD_REV = 1;
    /* An initrd picked from storage is the user's file, not ours to refresh. */
    private static final int INITRD_PICKED = -1;

    private void ensureBuiltinInitrd() {
        final int rev = getSharedPreferences("rvm", MODE_PRIVATE).getInt("initrd_rev", 0);
        if (rev == INITRD_PICKED || (rev == INITRD_REV && initrd.exists()))
            return;
        final File tmp = new File(initrd.getParentFile(), initrd.getName() + ".new");
        boolean ok = false;
        java.io.InputStream in = null;
        java.io.OutputStream out = null;
        try {
            in = getAssets().open("initrd.img");
            out = new java.io.FileOutputStream(tmp);
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.close();
            out = null;
            /* rename(2) replaces, so the working copy is never absent: a Boot that
             * starts during this second either sees the old /init or the new one. */
            ok = tmp.renameTo(initrd);
        } catch (java.io.IOException e) {
            ok = false;
        } finally {
            close(in); close(out);
            if (!ok) tmp.delete(); /* and the previous copy is still in place */
        }
        if (ok)
            getSharedPreferences("rvm", MODE_PRIVATE).edit()
                .putInt("initrd_rev", INITRD_REV).apply();
    }

    private void startVm() {
        if (vmThread != null) { toast("already running"); return; }
        if (copying) {
            toast("a file is still being copied - the bar at the top shows how many bytes");
            return;
        }
        if (!kernel.exists()) { toast("pick a kernel first"); return; }
        if (disk.exists() && disk.length() < (1L << 20)) {
            toast("disk.img is " + human(disk.length()) + ", far too small - copy it again");
            return;
        }

        console.clear();
        banner("Ready.\n  kernel: " + human(kernel.length())
               + "\n  disk:   " + (disk.exists() ? human(disk.length()) : "none")
               + "\n  ram:    " + ramMib + " MiB\n  cpu:    " + harts
               + (shellFirst ? "\n  shell:  root - the disk's init scripts are skipped\n"
                              : "\n"));
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
        final String i = initrd.exists() ? initrd.getAbsolutePath() : null;
        /* Two additions to the plain boot line, both for a phone.
         *
         * rvm.time=virtual makes the emulator derive mtime from retired
         * instructions (see src/devices/clint.c): the guest then measures its own
         * progress instead of real time, so a boot that takes four minutes of
         * wall clock does not look to it like a CPU stuck for four minutes.
         *
         * nosoftlockup keeps the watchdog's own reports out of dmesg for the
         * cases virtual time cannot cover - the vCPU thread genuinely not being
         * scheduled, which is what happens while a two gigabyte file is still
         * being copied or the screen is off. */
        final String bootargs = "console=ttyS0 earlycon=ns16550a,mmio32,0x10000000 "
            + "root=/dev/vda rootwait rw nosoftlockup rvm.time=virtual"
            + (shellFirst ? " rvm.shell=root" : "");

        vmThread = new Thread(new Runnable() {
            @Override public void run() {
                long h = RvmNative.vmCreate(k, d, i, ramMib, bootargs, debug);
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
        startCopy(data.getData(), (req == PICK_KERNEL) ? kernel
                : (req == PICK_DISK) ? disk : initrd);
    }

    private boolean copyIn(Uri uri, File dest) {
        File tmp = new File(dest.getParentFile(), dest.getName() + ".part");
        boolean ok = false;
        InputStream raw = null, in = null;
        FileOutputStream out = null;
        try {
            raw = getContentResolver().openInputStream(uri);
            if (raw == null) return false;
            BufferedInputStream bin = new BufferedInputStream(raw, 1 << 16);
            bin.mark(4);
            int a = bin.read(), b = bin.read();
            bin.reset();
            /* gzip is sniffed rather than trusted from the file name, because the
             * document provider decides what the name is and a .gz that is not
             * gzipped is exactly as common as the other way round. */
            in = (a == 0x1f && b == 0x8b) ? (InputStream) new GZIPInputStream(bin, 1 << 16) : bin;
            out = new FileOutputStream(tmp);
            byte[] buf = new byte[1 << 16];
            long soFar = 0;
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                if ((soFar >>> 23) != ((soFar + n) >>> 23)) announce(dest, soFar + n); /* every 8 MiB */
                soFar += n;
            }
            out.getFD().sync();
            out.close();
            out = null;
            in.close();
            in = null;
            if (dest.exists() && !dest.delete()) return false;
            ok = tmp.renameTo(dest);
            return ok;
        } catch (Exception e) {
            return false;
        } finally {
            close(raw); close(in); close(out);
            if (!ok) tmp.delete(); /* never leave a half image where Boot can find it */
        }
    }

    /* ---------------------------------------------------------------- misc */

    /* ------------------------------------------------------- machine spec */

    static final class MachineSpec {
        int ramMib = 1536;
        int cpus = 1;
        boolean shell;
    }

    /**
     * Accepts `ram=1536 cpu=1`, `1536,1`, or a bare `1536`.  Clamped, because a
     * typo here either fails the mmap or starves the guest, and both look like an
     * emulator bug from the outside.
     */
    static MachineSpec parseSpec(String raw) {
        MachineSpec sp = new MachineSpec();
        if (raw == null) return sp;
        Matcher m = Pattern.compile("(?i)(?:ram\\s*=\\s*)?(\\d{3,4})(?:\\D+(?:cpu\\s*=\\s*)?(\\d+))?")
            .matcher(raw.trim());
        if (m.find()) {
            int ram = Integer.parseInt(m.group(1));
            sp.ramMib = Math.max(256, Math.min(4096, ram));
            if (m.group(2) != null)
                sp.cpus = Math.max(1, Integer.parseInt(m.group(2)));
        }
        /* `shell=1` (or `shell=root`) skips the disk's init scripts and keeps the
         * shell the hand-off would otherwise give to /sbin/init: on a phone the
         * runlevel-S work is minutes of silent console, which is indistinguishable
         * from a hang.  See the rvm.shell=root block in tools/mkinitrd.sh - it
         * travels in the same line as ram and cpu so a broken boot has one place
         * to look at, not two. */
        sp.shell = Pattern.compile("(?i)shell\\s*=\\s*(1|root)\\b").matcher(raw).find();
        /* One hart is all the emulator has today (PLAN step 2), so the parsed
         * count is folded back to 1 rather than ignored: the field already
         * exists, and the second core needs no new dialog. */
        sp.cpus = 1;
        return sp;
    }

    private String loadSpec() {
        return getSharedPreferences("rvm", MODE_PRIVATE)
            .getString("machine_spec", "ram=1536 cpu=1");
    }

    private void applySpec(String raw) {
        MachineSpec sp = parseSpec(raw);
        ramMib = sp.ramMib;
        harts = sp.cpus;
        shellFirst = sp.shell;
        specRaw = (raw == null || raw.trim().isEmpty()) ? "ram=" + ramMib + " cpu=1" : raw.trim();
    }

    private void saveSpec(String raw) {
        getSharedPreferences("rvm", MODE_PRIVATE).edit().putString("machine_spec", raw).apply();
    }

    private void askMachine() {
        final EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        field.setSingleLine(true);
        field.setTypeface(Typeface.MONOSPACE);
        field.setText(specRaw);
        field.setSelection(specRaw.length());
        new AlertDialog.Builder(this)
            .setTitle("Machine")
            .setMessage("ram=1536 cpu=1 - add shell=1 for a root shell with no init scripts")
            .setView(field)
            .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                @Override public void onClick(DialogInterface d, int w) {
                    String raw = field.getText().toString().trim();
                    applySpec(raw);
                    saveSpec(raw);
                    status.setText("ram " + ramMib + " MiB, cpu " + harts + " - boot to apply");
                }
            })
            .setNegativeButton(android.R.string.cancel, null)
            .show();
    }

    /* --------------------------------------------------------- asset copies */

    /**
     * Stream a picked file into our own directory, inflating it on the way in if
     * it is gzipped.
     *
     * The release ships disk.img.gz, and unzipping two gigabytes by hand on a
     * phone is where truncated images came from; reading the gzip here means the
     * phone moves 166 MB instead.  Boot waits for this (see startVm), because a
     * VM started on a half written image hangs mid-I/O and the guest then looks
     * broken rather than unfinished.
     */
    private void startCopy(final Uri uri, final File dest) {
        if (copying) { toast("still copying - wait for the size to show in the bar"); return; }
        /* No stopVm() here: ensureAsset already stopped the VM before the picker
         * opened, and stopVm blocks the calling thread on a join - which from the
         * UI thread is an ANR, because the runnable it waits for is the one that
         * needs this same thread. */
        long need = dest.getName().equals("disk.img") ? 3L << 30 : 64L << 20;
        long free = freeBytes();
        if (free > 0 && free < need) {
            status.setText("not enough room: " + human(free) + " free, about " + human(need) + " needed");
            toast("free up space, then pick the file again");
            return;
        }
        copying = true;
        status.setText("copying " + dest.getName() + "…");
        new Thread(new Runnable() {
            @Override public void run() {
                final boolean ok = copyIn(uri, dest);
                final long size = dest.length();
                copying = false;
                ui.post(new Runnable() {
                    @Override public void run() {
                        status.setText(ok ? dest.getName() + ": " + human(size)
                                          : "copy failed (not enough space?)");
                        /* Whatever is in that file now came from the picker, so the
                         * built-in asset stops claiming it (see ensureBuiltinInitrd). */
                        if (ok && dest == initrd)
                            getSharedPreferences("rvm", MODE_PRIVATE).edit()
                                .putInt("initrd_rev", INITRD_PICKED).apply();
                    }
                });
            }
        }, "rvm-copy").start();
    }

    private long freeBytes() {
        try {
            return new StatFs(getFilesDir().getAbsolutePath()).getAvailableBytes();
        } catch (Exception e) {
            return -1; /* no gate rather than a false refusal */
        }
    }

    private void announce(final File dest, final long soFar) {
        final long bytes = soFar;
        final String name = dest.getName();
        ui.post(new Runnable() {
            @Override public void run() { status.setText("copying " + name + ": " + human(bytes)); }
        });
    }

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
