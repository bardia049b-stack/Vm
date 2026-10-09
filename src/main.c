/*
 * main.c -- the rvm command line front-end.
 *
 * SPDX-License-Identifier: MIT
 */
#include "rvm.h"
#include "ui/ui.h"
#include "vm/vm.h"

#include <errno.h>
#include <getopt.h>

#define RVM_VERSION "0.1.0"

static void usage(const char *prog) {
    printf("rvm %s -- a small RV64GC system emulator\n"
           "\n"
           "usage: %s [options]\n"
           "\n"
           "machine\n"
           "  -m, --mem MIB        guest RAM in MiB (default 1024, min 8, max 8192)\n"
           "  -k, --kernel PATH    kernel image: ELF64 vmlinux, or raw Image with --raw\n"
           "      --raw            treat --kernel as a raw Linux Image, not an ELF\n"
           "  -e, --entry HEX      override the kernel entry address\n"
           "  -t, --dtb PATH       use an external DTB (default: build one in-process)\n"
           "      --dump-dtb PATH  build the device tree, write it to PATH and exit\n"
           "  -i, --initrd PATH    initrd/initramfs image\n"
           "  -d, --disk PATH      virtio-blk backing file; becomes /dev/vda\n"
           "      --create-disk    create the disk file if missing\n"
           "      --disk-size MIB  size used together with --create-disk (default 2048)\n"
           "  -B, --bootargs STR   kernel command line\n"
           "      --isa STR        ISA string in the DTB (default rv64imafdc)\n"
           "      --mmu-type STR   DTB mmu-type (default riscv,sv57)\n"
           "\n"
           "control\n"
           "  -n, --insns N        stop after N instructions (0 = unlimited)\n"
           "      --stats          print counters on exit\n"
           "      --trace          log every retired instruction\n"
           "  -v, --verbose        debug logging\n"
           "  -q, --quiet          only warnings and errors\n"
           "      --version        print the version and exit\n"
           "  -h, --help           this message\n"
           "\n"
           "example\n"
           "  %s -m 1024 -k vmlinux -d disk.img --create-disk\n",
           RVM_VERSION, prog, prog);
}

static u64 parse_u64(const char *s, bool *ok) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (ok)
        *ok = (end && *end == '\0' && errno == 0);
    return (u64)v;
}

int main(int argc, char **argv) {
    vm_opts o;
    vm_opts_default(&o);

    char dump_dtb_path[1024] = {0};
    bool want_stats = false;
    bool raw = false;
    u64 disk_mib = 2048;
    bool mem_ok = true, insns_ok = true;

    static struct option longs[] = {{"mem", required_argument, 0, 'm'},
                                    {"kernel", required_argument, 0, 'k'},
                                    {"entry", required_argument, 0, 'e'},
                                    {"dtb", required_argument, 0, 't'},
                                    {"initrd", required_argument, 0, 'i'},
                                    {"disk", required_argument, 0, 'd'},
                                    {"bootargs", required_argument, 0, 'B'},
                                    {"insns", required_argument, 0, 'n'},
                                    {"verbose", no_argument, 0, 'v'},
                                    {"quiet", no_argument, 0, 'q'},
                                    {"help", no_argument, 0, 'h'},
                                    {"version", no_argument, 0, 1000},
                                    {"stats", no_argument, 0, 1001},
                                    {"trace", no_argument, 0, 1002},
                                    {"raw", no_argument, 0, 1003},
                                    {"create-disk", no_argument, 0, 1004},
                                    {"disk-size", required_argument, 0, 1005},
                                    {"dump-dtb", required_argument, 0, 1006},
                                    {"isa", required_argument, 0, 1007},
                                    {"mmu-type", required_argument, 0, 1008},
                                    {0, 0, 0, 0}};

    int c;
    while ((c = getopt_long(argc, argv, "m:k:e:t:i:d:B:n:vqh", longs, NULL)) != -1) {
        switch (c) {
        case 'm':
            o.ram_size = parse_u64(optarg, &mem_ok) << 20;
            break;
        case 'k':
            o.kernel_path = optarg;
            break;
        case 'e':
            o.entry_override = parse_u64(optarg, NULL);
            break;
        case 't':
            o.dtb_path = optarg;
            break;
        case 'i':
            o.initrd_path = optarg;
            break;
        case 'd':
            o.disk_path = optarg;
            break;
        case 'B':
            o.bootargs = optarg;
            break;
        case 'n':
            o.max_insns = parse_u64(optarg, &insns_ok);
            break;
        case 'v':
            o.log_level = RVM_LOG_DEBUG;
            break;
        case 'q':
            o.log_level = RVM_LOG_WARN;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        case 1000:
            printf("rvm %s\n", RVM_VERSION);
            return 0;
        case 1001:
            want_stats = true;
            break;
        case 1002:
            o.trace = true;
            o.log_level = RVM_LOG_TRACE;
            break;
        case 1003:
            raw = true;
            break;
        case 1004:
            o.create_disk = true;
            break;
        case 1005:
            disk_mib = parse_u64(optarg, NULL);
            break;
        case 1006:
            snprintf(dump_dtb_path, sizeof(dump_dtb_path), "%s", optarg);
            break;
        case 1007:
            o.isa = optarg;
            break;
        case 1008:
            o.mmu_type = optarg;
            break;
        default:
            usage(argv[0]);
            return 2;
        }
    }

    if (!mem_ok || !insns_ok) {
        fprintf(stderr, "rvm: bad numeric argument\n");
        return 2;
    }
    o.raw_kernel = raw;
    o.disk_size = disk_mib << 20;
    rvm_log_set_level(o.log_level);

    /* --dump-dtb: emit the device tree we would build, then stop. */
    if (dump_dtb_path[0]) {
        dtb_opts d;
        memset(&d, 0, sizeof(d));
        d.ram_base = RVM_RAM_BASE;
        d.ram_size = o.ram_size;
        d.model = "rvm,virt";
        d.bootargs = o.bootargs;
        d.isa = o.isa;
        d.mmu_type = o.mmu_type;
        d.n_virtio = RVM_VIRTIO_COUNT;
        d.timebase_hz = CLINT_TIMEBASE_HZ;
        d.stdout_path = "/soc/serial@10000000";
        u8 *blob = NULL;
        u32 len = 0;
        rvm_err e = dtb_build(&d, &blob, &len);
        if (e != RVM_OK) {
            fprintf(stderr, "rvm: dtb_build failed: %s\n", rvm_strerror(e));
            return 1;
        }
        FILE *fp = fopen(dump_dtb_path, "wb");
        if (!fp) {
            fprintf(stderr, "rvm: cannot write %s\n", dump_dtb_path);
            free(blob);
            return 1;
        }
        fwrite(blob, 1, len, fp);
        fclose(fp);
        printf("wrote %u bytes to %s\n", len, dump_dtb_path);
        free(blob);
        return 0;
    }

    if (!o.kernel_path) {
        fprintf(stderr, "rvm: no kernel given (try -k vmlinux, or --help)\n");
        return 2;
    }

    ui_stdio ui;
    if (ui_stdio_init(&ui) != RVM_OK) {
        fprintf(stderr, "rvm: cannot set up the console\n");
        return 1;
    }
    ui_stdio_attach(&o, &ui);

    vm v;
    rvm_err e = vm_new(&v, &o);
    if (e != RVM_OK) {
        fprintf(stderr, "rvm: vm_new failed: %s\n", rvm_strerror(e));
        ui_stdio_shutdown(&ui);
        return 1;
    }
    e = vm_load(&v);
    if (e != RVM_OK) {
        fprintf(stderr, "rvm: load failed: %s\n", rvm_strerror(e));
        vm_free(&v);
        ui_stdio_shutdown(&ui);
        return 1;
    }

    e = vm_run(&v);
    if (want_stats || e != RVM_OK)
        vm_print_stats(&v);

    u32 code = v.exit_code;
    vm_free(&v);
    ui_stdio_shutdown(&ui);
    return e == RVM_OK ? (int)code : 1;
}
