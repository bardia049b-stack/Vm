/*
 * RvmNative -- the whole JNI surface of the app.
 *
 * Threading model: the VM runs on one dedicated thread and owns the cpu struct
 * outright, so the core needs no locks at all.  Two things do cross threads:
 *
 *   - console output, produced on the VM thread and painted on the UI thread;
 *   - keystrokes, produced on the UI thread and consumed on the VM thread.
 *
 * Both go through a single-producer/single-consumer ring buffer implemented
 * with C11 atomics in rvm_jni.c.  That is why there is no monitor, no
 * synchronized block and no lock anywhere in this app.
 *
 * SPDX-License-Identifier: MIT
 */
package dev.rvm.app;

public final class RvmNative {

    static {
        System.loadLibrary("rvm_jni");
    }

    private RvmNative() {}

    /* ---------------------------------------------------------- native API */

    /**
     * One line of RVM's own log.  level is the C rvm_loglevel: 0 trace,
     * 1 debug, 2 info, 3 warn, 4 error.  Called on the VM thread.
     */
    public interface LogSink { void onLog(int level, String line); }

    public static volatile LogSink logSink;

    /** Entry point for rvm_jni.c; see proguard-rules.pro. */
    static void onLog(int level, String line) {
        LogSink s = logSink;
        if (s != null) s.onLog(level, line);
    }

    /** Creates a VM.  debug turns RVM's own log on for this VM.
     *  Returns an opaque handle, or 0 on failure. */
    public static native long vmCreate(String kernel, String disk, String initrd,
                                       int ramMib, String bootargs, boolean debug);

    /** Runs the VM on the calling thread until it stops.  Never call on the UI thread. */
    public static native int vmRun(long handle);

    /** Asks the run loop to stop at the next instruction boundary. */
    public static native void vmStop(long handle);

    /** Releases everything the handle owns. */
    public static native void vmFree(long handle);

    /** Queues host keystrokes for the guest's serial FIFO. */
    public static native void vmInput(long handle, byte[] data, int len);

    /** Injects a Ctrl-C (ETX) into the guest console. */
    public static native void vmInterrupt(long handle);

    /** Retired instruction count and MIPS, for the status line. */
    public static native long vmInsns(long handle);

    /* ------------------------------------------------- callbacks from C */
    /*
     * These three are looked up by name from rvm_jni.c.  They are always
     * invoked on the VM thread; each one only appends to a queue that the UI
     * thread drains, so they must stay cheap and must not touch a View.
     */

    static volatile ConsoleSink consoleSink;
    static volatile FrameSink frameSink;
    static volatile ExitSink exitSink;

    public interface ConsoleSink { void onOutput(byte[] buf, int len); }
    public interface FrameSink   { void onFrame(byte[] rgba, int w, int h, int stride); }
    public interface ExitSink    { void onExit(int code); }

    @SuppressWarnings("unused") /* called from C */
    private static void onConsoleOutput(byte[] buf, int len) {
        ConsoleSink s = consoleSink;
        if (s != null) s.onOutput(buf, len);
    }

    @SuppressWarnings("unused") /* called from C */
    private static void onFrame(byte[] rgba, int w, int h, int stride) {
        FrameSink s = frameSink;
        if (s != null) s.onFrame(rgba, w, h, stride);
    }

    @SuppressWarnings("unused") /* called from C */
    private static void onVmExit(int code) {
        ExitSink s = exitSink;
        if (s != null) s.onExit(code);
    }
}
