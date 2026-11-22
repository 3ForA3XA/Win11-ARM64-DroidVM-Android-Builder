/* makegpt.c — create a protective-MBR + GPT partition table on a raw image file
 * Layout (4KB-aligned LBAs, 512B sector):
 *   LBA0   : protective MBR
 *   LBA1   : GPT header
 *   LBA2.. : partition entries (128 x 128B = 16KB)
 *   p1: EFI System Partition  (FAT32)  — esp_mb
 *   p2: Microsoft reserved    (MSR)    — 16MB
 *   p3: Microsoft basic data  (NTFS)   — rest
 *   last 33 LBAs: backup entries + backup header
 *
 * Usage: makegpt <image> <total_mb> <esp_mb>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

static uint32_t crc32_table[256];
static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc32_table[i] = c;
    }
}
static uint32_t crc32_buf(const void *buf, size_t len) {
    uint32_t c = 0xFFFFFFFFu;
    const unsigned char *p = buf;
    for (size_t i = 0; i < len; i++) c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static const unsigned char GUID_ESP[16]  = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const unsigned char GUID_MSR[16]  = {0x16,0xE3,0xC9,0xE3,0x5C,0x0B,0xB8,0x4D,0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE};
static const unsigned char GUID_MSD[16]  = {0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7};

static void put16(unsigned char *p, uint16_t v) { p[0]=v&0xFF; p[1]=v>>8; }
static void put32(unsigned char *p, uint32_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void put64(unsigned char *p, uint64_t v) { for(int i=0;i<8;i++) p[i]=(v>>(8*i))&0xFF; }
static void guid_mix(unsigned char *g, uint32_t salt) {
    /* derive a pseudo-unique partition GUID from type guid + salt */
    for (int i = 0; i < 4; i++) g[8+i] = (unsigned char)(salt >> (8*i));
    g[12] ^= (unsigned char)salt; g[13] ^= (unsigned char)(salt>>8);
}

static void write_entry(unsigned char *e, const unsigned char *type, uint64_t start, uint64_t end, uint32_t salt, const char *name16le) {
    memcpy(e, type, 16);
    for (int i = 0; i < 16; i++) e[16+i] = type[i] ^ 0x5A; /* unique-ish */
    guid_mix(e + 16, salt);
    put64(e + 32, start);
    put64(e + 40, end);
    put64(e + 48, 0); /* attributes */
    for (int i = 0; i < 36; i++) { /* 72 bytes UTF-16LE name */
        unsigned short ch = (unsigned char)name16le[i];
        if (!ch) { put16(e + 56 + 2*i, 0); continue; }
        put16(e + 56 + 2*i, ch);
    }
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: makegpt <image> <total_mb> <esp_mb>\n"); return 2; }
    crc32_init();
    const char *img = argv[1];
    uint64_t total_mb = strtoull(argv[2], NULL, 10);
    uint64_t esp_mb   = strtoull(argv[3], NULL, 10);
    const uint64_t SECT = 512;
    uint64_t total_sect = total_mb * 1024 * 1024 / SECT;
    if (total_sect < 2048 + 34 * 2) { fprintf(stderr, "image too small\n"); return 1; }

    uint64_t esp_start = 2048;                       /* 1MB aligned */
    uint64_t esp_end   = esp_start + esp_mb * 1024 * 1024 / SECT - 1;
    uint64_t msr_start = esp_end + 1;
    uint64_t msr_end   = msr_start + 16 * 1024 * 1024 / SECT - 1;
    uint64_t ntfs_start = msr_end + 1;
    uint64_t backup_end = total_sect - 1;            /* backup GPT header LBA */
    uint64_t backup_ent_start = backup_end - 32;     /* 32 LBAs of backup entries */
    uint64_t ntfs_end = backup_ent_start - 1;
    if (ntfs_end <= ntfs_start) { fprintf(stderr, "NTFS partition too small\n"); return 1; }

    int fd = open(img, O_RDWR | O_CREAT, 0644);
    if (fd < 0) { perror("open"); return 1; }
    unsigned char buf[SECT];

    /* truncate to full size */
    if (ftruncate(fd, total_sect * SECT) != 0) { perror("ftruncate"); return 1; }

    /* protective MBR */
    memset(buf, 0, SECT);
    buf[446] = 0x00; /* not bootable */
    buf[450] = 0xEE;
    put32(buf + 454, 1);
    put32(buf + 458, (uint32_t)(total_sect - 1));
    buf[510] = 0x55; buf[511] = 0xAA;
    if (pwrite(fd, buf, SECT, 0) != SECT) { perror("mbr"); return 1; }

    /* partition entries: primary + backup */
    unsigned char *ents = calloc(1, 128 * 128);
    write_entry(ents + 0*128,   GUID_ESP, esp_start, esp_end,  0x11110001, "ESP");
    write_entry(ents + 1*128,   GUID_MSR, msr_start, msr_end,  0x22220002, "MSR");
    write_entry(ents + 2*128,   GUID_MSD, ntfs_start, ntfs_end, 0x33330003, "DroidVM");
    unsigned char *bents = calloc(1, 128 * 128);
    memcpy(bents, ents, 128 * 128);

    /* GPT header (primary) */
    unsigned char hdr[SECT]; memset(hdr, 0, SECT);
    memcpy(hdr, "EFI PART", 8);
    put32(hdr + 8, 0x00010000);      /* version 1.0 */
    put32(hdr + 12, 92);             /* header size */
    put32(hdr + 16, 0);              /* CRC placeholder, patched later */
    put32(hdr + 20, 0);              /* reserved */
    put64(hdr + 24, 1);              /* current LBA */
    put64(hdr + 32, backup_end);     /* backup LBA */
    put64(hdr + 40, esp_start);      /* first usable */
    put64(hdr + 48, backup_ent_start - 1); /* last usable */
    memcpy(hdr + 56, "\xBD\xAA\x4B\xC8\xE8\xDA\x49\xC6\x90\xE6\x44\x57\x74\xBC\x69\xF4", 16); /* disk GUID */
    put64(hdr + 72, 2);              /* entries start LBA */
    put32(hdr + 80, 128);            /* number of entries */
    put32(hdr + 84, 128);            /* entry size */
    put32(hdr + 88, crc32_buf(ents, 128 * 128));
    put32(hdr + 16, crc32_buf(hdr, 92));
    if (pwrite(fd, hdr, SECT, 1 * SECT) != SECT) { perror("hdr"); return 1; }
    if (pwrite(fd, ents, 128 * 128, 2 * SECT) != 128 * 128) { perror("ents"); return 1; }

    /* backup */
    if (pwrite(fd, bents, 128 * 128, backup_ent_start * SECT) != 128 * 128) { perror("bents"); return 1; }
    unsigned char bhdr[SECT]; memset(bhdr, 0, SECT);
    memcpy(bhdr, hdr, SECT);
    put64(bhdr + 24, backup_end);
    put64(bhdr + 32, 1);
    put32(bhdr + 16, 0);
    put32(bhdr + 88, crc32_buf(bents, 128 * 128));
    put32(bhdr + 16, crc32_buf(bhdr, 92));
    if (pwrite(fd, bhdr, SECT, backup_end * SECT) != SECT) { perror("bhdr"); return 1; }

    close(fd);
    printf("GPT OK: ESP LBA %llu-%llu, MSR %llu-%llu, NTFS %llu-%llu\n",
        (unsigned long long)esp_start, (unsigned long long)esp_end,
        (unsigned long long)msr_start, (unsigned long long)msr_end,
        (unsigned long long)ntfs_start, (unsigned long long)ntfs_end);
    return 0;
}
