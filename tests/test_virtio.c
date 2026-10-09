/*
 * test_virtio.c -- the virtio-mmio transport and an end-to-end block request.
 *
 * The block test builds a real split virtqueue in guest RAM (descriptors,
 * available ring, used ring) and drives it through the register interface
 * exactly as a Linux driver would, then checks the bytes that actually landed
 * in the backing file.  Nothing here is mocked.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../src/devices/virtio.h"
#include "../src/devices/virtio_blk.h"
#include "fixtures/harness.h"
#include "test.h"

#include <fcntl.h>
#include <unistd.h>

#define REG_MAGIC 0x000
#define REG_VERSION 0x004
#define REG_DEVICE_ID 0x008
#define REG_VENDOR_ID 0x00C
#define REG_DEVICE_FEATURES 0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES 0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL 0x030
#define REG_QUEUE_NUM_MAX 0x034
#define REG_QUEUE_NUM 0x038
#define REG_QUEUE_READY 0x044
#define REG_QUEUE_NOTIFY 0x050
#define REG_INTERRUPT_STATUS 0x060
#define REG_INTERRUPT_ACK 0x064
#define REG_STATUS 0x070
#define REG_QUEUE_DESC_LO 0x080
#define REG_QUEUE_DESC_HI 0x084
#define REG_QUEUE_DRIVER_LO 0x090
#define REG_QUEUE_DRIVER_HI 0x094
#define REG_QUEUE_DEVICE_LO 0x0A0
#define REG_QUEUE_DEVICE_HI 0x0A4
#define REG_CONFIG 0x100

#define VRING_NEXT 1
#define VRING_WRITE 2

static void test_raise(void *ud, u32 irq, bool level)
{
    plic *p = (plic *)ud;
    if (level) plic_raise(p, irq);
}

void test_virtio_transport(void)
{
    bus b;
    CHECK(bus_init(&b, 16ULL << 20) == RVM_OK);

    /* A slot with no backend must identify itself as empty. */
    u64 v = 0;
    CHECK(virtio_absent_load(NULL, REG_MAGIC, 4, &v));
    CHECK_U64(v, VIRTIO_MMIO_MAGIC);
    CHECK(virtio_absent_load(NULL, REG_VERSION, 4, &v));
    CHECK_U64(v, VIRTIO_VERSION);
    CHECK(virtio_absent_load(NULL, REG_DEVICE_ID, 4, &v));
    CHECK_U64(v, 0); /* <- this is what makes Linux skip the slot */

    /* Real backend: the block device. */
    const char *path = "/tmp/rvm-test-virtio-transport.img";
    unlink(path);
    virtio_blk blk;
    CHECK(virtio_blk_open(&blk, path, 16ULL << 20, true) == RVM_OK);
    virtio dev;
    CHECK(virtio_init(&dev, &b, virtio_blk_backend(&blk), 1) == RVM_OK);

    CHECK(virtio_load(&dev, REG_MAGIC, 4, &v));
    CHECK_U64(v, VIRTIO_MMIO_MAGIC);
    CHECK(virtio_load(&dev, REG_VERSION, 4, &v));
    CHECK_U64(v, VIRTIO_VERSION);
    CHECK(virtio_load(&dev, REG_DEVICE_ID, 4, &v));
    CHECK_U64(v, VIRTIO_ID_BLOCK);
    CHECK(virtio_load(&dev, REG_VENDOR_ID, 4, &v));
    CHECK_U64(v, VIRTIO_VENDOR);

    /* Device features are read through the selector register. */
    CHECK(virtio_store(&dev, REG_DEVICE_FEATURES_SEL, 4, 0));
    CHECK(virtio_load(&dev, REG_DEVICE_FEATURES, 4, &v));
    CHECK_U64(v & (1u << VIRTIO_BLK_F_BLK_SIZE), 1u << VIRTIO_BLK_F_BLK_SIZE);
    CHECK_U64(v & (1u << VIRTIO_BLK_F_FLUSH), 1u << VIRTIO_BLK_F_FLUSH);
    CHECK(virtio_store(&dev, REG_DEVICE_FEATURES_SEL, 4, 1));
    CHECK(virtio_load(&dev, REG_DEVICE_FEATURES, 4, &v));
    CHECK_U64(v, 1u << (VIRTIO_F_VERSION_1 - 32));

    /* Queue capacity. */
    CHECK(virtio_store(&dev, REG_QUEUE_SEL, 4, 0));
    CHECK(virtio_load(&dev, REG_QUEUE_NUM_MAX, 4, &v));
    CHECK_U64(v, VIRTQUEUE_NUM_MAX);

    /* Asking for a feature the device does not offer must be refused by
     * clearing FEATURES_OK, which is how the driver learns to back off. */
    CHECK(virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK));
    CHECK(virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER));
    CHECK(virtio_store(&dev, REG_DRIVER_FEATURES_SEL, 4, 0));
    CHECK(virtio_store(&dev, REG_DRIVER_FEATURES, 4, 0x80000000u)); /* bogus bit */
    CHECK(virtio_store(&dev, REG_STATUS, 4,
                       VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK));
    CHECK(virtio_load(&dev, REG_STATUS, 4, &v));
    CHECK_U64(v & VIRTIO_STATUS_FEATURES_OK, 0);

    /* A device reset must clear the negotiation state. */
    CHECK(virtio_store(&dev, REG_STATUS, 4, 0));
    CHECK(virtio_load(&dev, REG_STATUS, 4, &v));
    CHECK_U64(v, 0);
    CHECK_U64(dev.driver_features, 0);

    /* Config space exposes the capacity in 512-byte sectors. */
    u64 cap = 0;
    CHECK(virtio_load(&dev, REG_CONFIG, 4, &cap));
    CHECK(virtio_load(&dev, REG_CONFIG + 4, 4, &v));
    cap |= v << 32;
    CHECK_U64(cap, (16ULL << 20) / VIRTIO_BLK_SECTOR_SIZE);

    /* A 4-byte access is the only supported width. */
    CHECK(!virtio_load(&dev, REG_MAGIC, 2, &v));

    virtio_blk_close(&blk);
    unlink(path);
    bus_free(&b);
}

/* ---------------------------------------------------------------- e2e */

typedef struct {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} __attribute__((packed)) test_vring_desc;

static void put_desc(bus *b, u64 tbl, u32 i, u64 addr, u32 len, u16 flags, u16 next)
{
    test_vring_desc d = {addr, len, flags, next};
    bus_write_bytes(b, tbl + 16ULL * i, &d, sizeof(d));
}

void test_virtio_blk_roundtrip(void)
{
    bus b;
    CHECK(bus_init(&b, 16ULL << 20) == RVM_OK);
    plic p;
    CHECK(plic_init(&p) == RVM_OK);

    const char *path = "/tmp/rvm-test-virtio-blk.img";
    unlink(path);
    virtio_blk blk;
    CHECK(virtio_blk_open(&blk, path, 1ULL << 20, true) == RVM_OK);
    virtio dev;
    CHECK(virtio_init(&dev, &b, virtio_blk_backend(&blk), 1) == RVM_OK);
    virtio_set_irq(&dev, test_raise, &p);

    /* ---- driver initialisation sequence ---- */
    virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK);
    virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);
    virtio_store(&dev, REG_DRIVER_FEATURES_SEL, 4, 1);
    virtio_store(&dev, REG_DRIVER_FEATURES, 4, 1u << (VIRTIO_F_VERSION_1 - 32));
    virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                                          VIRTIO_STATUS_FEATURES_OK);
    u64 st = 0;
    virtio_load(&dev, REG_STATUS, 4, &st);
    CHECK_U64(st & VIRTIO_STATUS_FEATURES_OK, VIRTIO_STATUS_FEATURES_OK);

    /* ---- lay out the vring in guest RAM ---- */
    const u64 DESC = RVM_RAM_BASE + 0x200000ULL;
    const u64 AVAIL = RVM_RAM_BASE + 0x201000ULL;
    const u64 USED = RVM_RAM_BASE + 0x202000ULL;
    const u64 HDR = RVM_RAM_BASE + 0x203000ULL;
    const u64 DATA = RVM_RAM_BASE + 0x204000ULL;
    const u64 STATUS = RVM_RAM_BASE + 0x205000ULL;
    const u32 QNUM = 8;

    virtio_store(&dev, REG_QUEUE_SEL, 4, 0);
    virtio_store(&dev, REG_QUEUE_NUM, 4, QNUM);
    virtio_store(&dev, REG_QUEUE_DESC_LO, 4, (u32)DESC);
    virtio_store(&dev, REG_QUEUE_DESC_HI, 4, (u32)(DESC >> 32));
    virtio_store(&dev, REG_QUEUE_DRIVER_LO, 4, (u32)AVAIL);
    virtio_store(&dev, REG_QUEUE_DRIVER_HI, 4, (u32)(AVAIL >> 32));
    virtio_store(&dev, REG_QUEUE_DEVICE_LO, 4, (u32)USED);
    virtio_store(&dev, REG_QUEUE_DEVICE_HI, 4, (u32)(USED >> 32));
    virtio_store(&dev, REG_QUEUE_READY, 4, 1);
    virtio_store(&dev, REG_STATUS, 4, VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER |
                                          VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);
    CHECK(dev.driver_ok);

    /* ---- request 1: write one sector to the disk ---- */
    u8 pattern[512];
    for (u32 i = 0; i < sizeof(pattern); i++) pattern[i] = (u8)(i * 7 + 3);

    u8 hdr[16];
    memset(hdr, 0, sizeof(hdr));
    u32 type = VIRTIO_BLK_T_OUT; /* guest -> device */
    u64 sector = 0;
    memcpy(hdr + 0, &type, 4);
    memcpy(hdr + 8, &sector, 8);
    bus_write_bytes(&b, HDR, hdr, sizeof(hdr));
    bus_write_bytes(&b, DATA, pattern, sizeof(pattern));
    bus_store(&b, STATUS, 1, 0xEE);

    put_desc(&b, DESC, 0, HDR, 16, VRING_NEXT, 1);
    /* For a write request the data buffer is device-*readable*, so it must not
     * carry VRING_DESC_F_WRITE; only the status byte is written back. */
    put_desc(&b, DESC, 1, DATA, sizeof(pattern), VRING_NEXT, 2);
    put_desc(&b, DESC, 2, STATUS, 1, VRING_WRITE, 0);
    /* avail ring: flags, idx, ring[0] */
    bus_store(&b, AVAIL + 0, 2, 0);
    bus_store(&b, AVAIL + 2, 2, 1);
    bus_store(&b, AVAIL + 4, 2, 0);

    virtio_store(&dev, REG_QUEUE_NOTIFY, 4, 0);

    u64 used_idx = 0, elem_id = 0, elem_len = 0, status_byte = 0xEE;
    bus_load(&b, USED + 2, 2, &used_idx);
    bus_load(&b, USED + 4 + 0, 4, &elem_id);
    bus_load(&b, USED + 4 + 4, 4, &elem_len);
    bus_load(&b, STATUS, 1, &status_byte);
    CHECK_U64(used_idx, 1);
    CHECK_U64(elem_id, 0);
    CHECK_U64(elem_len, 1); /* only the status byte is written back */
    CHECK_U64(status_byte, VIRTIO_BLK_S_OK);
    CHECK_U64(blk.n_write, 1);

    /* The interrupt must have been latched in the PLIC. */
    CHECK_U64(dev.irq_status & 1, 1);
    CHECK_U64(p.pending[1 / 32] & (1u << 1), 1u << 1);

    /* And the bytes must really be in the file. */
    {
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        u8 readback[512];
        ssize_t n = pread(fd, readback, sizeof(readback), 0);
        CHECK_U64((u64)n, sizeof(readback));
        CHECK_MEM(readback, pattern, sizeof(pattern));
        close(fd);
    }

    /* ---- request 2: read the same sector back into a fresh buffer ---- */
    u8 scratch[512];
    memset(scratch, 0, sizeof(scratch));
    bus_write_bytes(&b, DATA, scratch, sizeof(scratch));
    memset(hdr, 0, sizeof(hdr));
    type = VIRTIO_BLK_T_IN; /* device -> guest */
    sector = 0;
    memcpy(hdr + 0, &type, 4);
    memcpy(hdr + 8, &sector, 8);
    bus_write_bytes(&b, HDR, hdr, sizeof(hdr));
    bus_store(&b, STATUS, 1, 0xEE);
    /* The chain is reused, but for a read the data buffer flips direction and
     * must be device-writable again. */
    put_desc(&b, DESC, 1, DATA, sizeof(scratch), VRING_NEXT | VRING_WRITE, 2);
    bus_store(&b, AVAIL + 2, 2, 2);
    bus_store(&b, AVAIL + 4 + 2 * 1, 2, 0); /* avail.ring[1] = head 0 */

    virtio_store(&dev, REG_QUEUE_NOTIFY, 4, 0);

    bus_load(&b, USED + 2, 2, &used_idx);
    bus_load(&b, USED + 4 + 8 * 1 + 4, 4, &elem_len);
    bus_load(&b, STATUS, 1, &status_byte);
    CHECK_U64(used_idx, 2);
    CHECK_U64(elem_len, 1 + sizeof(scratch));
    CHECK_U64(status_byte, VIRTIO_BLK_S_OK);
    CHECK_U64(blk.n_read, 1);
    bus_read_bytes(&b, DATA, scratch, sizeof(scratch));
    CHECK_MEM(scratch, pattern, sizeof(pattern));

    /* ---- request 3: flush ---- */
    memset(hdr, 0, sizeof(hdr));
    type = VIRTIO_BLK_T_FLUSH;
    memcpy(hdr + 0, &type, 4);
    bus_write_bytes(&b, HDR, hdr, sizeof(hdr));
    bus_store(&b, STATUS, 1, 0xEE);
    bus_store(&b, AVAIL + 2, 2, 3);
    bus_store(&b, AVAIL + 4 + 2 * 2, 2, 0);
    virtio_store(&dev, REG_QUEUE_NOTIFY, 4, 0);
    bus_load(&b, STATUS, 1, &status_byte);
    CHECK_U64(status_byte, VIRTIO_BLK_S_OK);
    CHECK_U64(blk.n_flush, 1);

    /* ---- an unsupported request type must report S_UNSUPP ---- */
    memset(hdr, 0, sizeof(hdr));
    type = 999;
    memcpy(hdr + 0, &type, 4);
    bus_write_bytes(&b, HDR, hdr, sizeof(hdr));
    bus_store(&b, STATUS, 1, 0);
    bus_store(&b, AVAIL + 2, 2, 4);
    bus_store(&b, AVAIL + 4 + 2 * 3, 2, 0);
    virtio_store(&dev, REG_QUEUE_NOTIFY, 4, 0);
    bus_load(&b, STATUS, 1, &status_byte);
    CHECK_U64(status_byte, VIRTIO_BLK_S_UNSUPP);

    /* Interrupt acknowledge clears the line. */
    virtio_store(&dev, REG_INTERRUPT_ACK, 4, 1);
    CHECK_U64(dev.irq_status & 1, 0);

    virtio_blk_close(&blk);
    unlink(path);
    bus_free(&b);
}
