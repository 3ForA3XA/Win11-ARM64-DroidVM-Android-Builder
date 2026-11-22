/* hiveutil.c — minimal registry-hive editor on top of libhivex
 * for the Android Win11-ARM64 DroidVM image builder.
 *
 * Usage:
 *   hiveutil ls        <hive> [path]
 *   hiveutil setval    <hive> <keypath> <valuename> <sz|dword|qword|hex> <data>
 *                      data for sz is UTF-8 text; hex is hex string (even len)
 *   hiveutil delval    <hive> <keypath> <valuename>
 *   hiveutil delkey    <hive> <keypath>
 *   hiveutil addkey    <hive> <keypath>
 *   hiveutil copytree  <hive> <srcpath> <dstparentpath> <newname>
 * Key paths use '\' as separator, root = "\".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <hivex.h>

static void die(const char *msg) { fprintf(stderr, "hiveutil: %s\n", msg); exit(1); }

static char **split_path(const char *path, int *count) {
    static char *parts[64];
    int n = 0;
    char *dup = strdup(path);
    char *p = dup;
    while (*p == '\\') p++;
    char *tok = strtok(p, "\\");
    while (tok && n < 64) { parts[n++] = tok; tok = strtok(NULL, "\\"); }
    *count = n;
    return parts; /* NB: parts point into dup; caller keeps it alive */
}

static hive_node_h walk(hive_h *h, const char *path, int create_missing) {
    int n; char **parts = split_path(path, &n);
    hive_node_h node = hivex_root(h);
    if (!node) die("no root");
    if (!path || !strcmp(path, "\\")) return node;
    for (int i = 0; i < n; i++) {
        hive_node_h next = hivex_node_get_child(h, node, parts[i]);
        if (!next) {
            if (!create_missing) die("key not found");
            next = hivex_node_add_child(h, node, parts[i]);
            if (!next) die("add_child failed");
        }
        node = next;
    }
    return node;
}

static char *utf16_from_utf8(const char *in, size_t *outbytes) {
    size_t n = strlen(in);
    char *out = malloc((n + 1) * 2);
    size_t j = 0;
    for (size_t i = 0; i < n; ) {
        unsigned int cp = (unsigned char)in[i];
        int len = 1;
        if (cp < 0x80) len = 1;
        else if ((cp & 0xE0) == 0xC0) { cp = cp & 0x1F; len = 2; }
        else if ((cp & 0xF0) == 0xE0) { cp = cp & 0x0F; len = 3; }
        else { cp = cp & 0x07; len = 4; }
        for (int k = 1; k < len; k++) cp = (cp << 6) | (in[i + k] & 0x3F);
        i += len;
        if (cp < 0x10000) {
            out[j++] = cp & 0xFF; out[j++] = (cp >> 8) & 0xFF;
        } else {
            cp -= 0x10000;
            unsigned int v = 0xD800 + (cp >> 10);
            out[j++] = v & 0xFF; out[j++] = (v >> 8) & 0xFF;
            v = 0xDC00 + (cp & 0x3FF);
            out[j++] = v & 0xFF; out[j++] = (v >> 8) & 0xFF;
        }
    }
    out[j++] = 0; out[j++] = 0;
    *outbytes = j;
    return out;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void set_one_value(hive_h *h, hive_node_h node, const char *name,
                          const char *type_s, const char *data_s) {
    int t;
    char *buf; size_t len;
    if (!strcmp(type_s, "sz")) {
        t = 1; /* REG_SZ */
        buf = utf16_from_utf8(data_s, &len);
    } else if (!strcmp(type_s, "dword")) {
        t = 4; len = 4;
        buf = calloc(1, 4);
        *(uint32_t *)buf = (uint32_t)strtoul(data_s, NULL, 0);
    } else if (!strcmp(type_s, "qword")) {
        t = 11; len = 8;
        buf = calloc(1, 8);
        *(uint64_t *)buf = (uint64_t)strtoull(data_s, NULL, 0);
    } else if (!strcmp(type_s, "hex")) {
        t = 3; /* REG_BINARY */
        size_t n = strlen(data_s);
        if (n % 2) die("hex data odd length");
        len = n / 2; buf = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++)
            buf[i] = (char)((hexval(data_s[2*i]) << 4) | hexval(data_s[2*i+1]));
        if (!len) { len = 0; }
    } else die("unknown type");
    hive_set_value sv;
    sv.key = (char *)name;
    sv.t = (hive_type)t;
    sv.len = len;
    sv.value = buf;
    if (hivex_node_set_value(h, node, &sv, 0) == -1)
        die("set_value failed");
    free(buf);
}

static void copy_tree(hive_h *h, hive_node_h src, hive_node_h dstparent, const char *newname) {
    hive_node_h dst = hivex_node_add_child(h, dstparent, newname);
    if (!dst) die("copytree add_child failed");
    hive_value_h *vals = hivex_node_values(h, src);
    if (vals) {
        for (hive_value_h *v = vals; *v; v++) {
            char *key = hivex_value_key(h, *v);
            size_t len = 0;
            hive_type t = hive_t_none;
            char *data = hivex_value_value(h, *v, &t, &len);
            if (key && data) {
                if (len == 0) { static char z; data = &z; }
                hive_set_value sv2;
                sv2.key = key; sv2.t = t; sv2.len = len; sv2.value = data;
                hivex_node_set_value(h, dst, &sv2, 0);
            }
            free(key); free(data);
        }
        free(vals);
    }
    hive_node_h *kids = hivex_node_children(h, src);
    if (kids) {
        for (hive_node_h *k = kids; *k; k++) {
            char *nm = hivex_node_name(h, *k);
            if (nm) { copy_tree(h, *k, dst, nm); free(nm); }
        }
        free(kids);
    }
}

static hive_value_h find_value(hive_h *h, hive_node_h node, const char *name) {
    hive_value_h *vals = hivex_node_values(h, node);
    hive_value_h found = 0;
    if (vals) {
        for (hive_value_h *v = vals; *v; v++) {
            char *k = hivex_value_key(h, *v);
            if (k && !strcmp(k, name)) { found = *v; }
            free(k);
            if (found) break;
        }
        free(vals);
    }
    return found;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hiveutil <cmd> <hive> [args...]\ncmds: ls setval delval delkey addkey copytree bcdtree\n");
        return 2;
    }
    const char *cmd = argv[1];
    hive_h *h = hivex_open(argv[2], HIVEX_OPEN_WRITE);
    if (!h) die("cannot open hive (need write)");
    int ret = 0;

    if (!strcmp(cmd, "ls") && argc >= 3) {
        hive_node_h node = walk(h, argc > 3 ? argv[3] : "\\", 0);
        hive_node_h *kids = hivex_node_children(h, node);
        for (hive_node_h *k = kids; *k; k++) { char *nm = hivex_node_name(h, *k); printf("K %s\n", nm); free(nm); }
        free(kids);
        hive_value_h *vals = hivex_node_values(h, node);
        for (hive_value_h *v = vals; *v; v++) { char *k = hivex_value_key(h, *v); printf("V %s\n", k); free(k); }
        free(vals);
    } else if (!strcmp(cmd, "setval") && argc == 7) {
        hive_node_h node = walk(h, argv[3], 1);
        set_one_value(h, node, argv[4], argv[5], argv[6]);
    } else if (!strcmp(cmd, "delval") && argc == 5) {
        hive_node_h node = walk(h, argv[3], 0);
        /* rewrite the value list without the target */
        hive_value_h *vals = hivex_node_values(h, node);
        if (!vals) die("value not found");
        hive_set_value *keep = calloc(256, sizeof(hive_set_value));
        size_t nk = 0;
        for (hive_value_h *v = vals; *v && nk < 256; v++) {
            char *k = hivex_value_key(h, *v);
            if (k && strcmp(k, argv[4])) {
                hive_type t = hive_t_none; size_t len = 0;
                char *d = hivex_value_value(h, *v, &t, &len);
                if (!d) { static char zero; d = &zero; len = 0; }
                keep[nk].key = k; keep[nk].t = t; keep[nk].len = len; keep[nk].value = d;
                nk++;
            }
            free(k);
        }
        free(vals);
        if (hivex_node_set_values(h, node, nk, keep, 0) == -1) die("set_values failed");
    } else if (!strcmp(cmd, "delkey") && argc == 4) {
        hive_node_h node = walk(h, argv[3], 0);
        if (hivex_node_delete_child(h, node) == -1) die("delete failed");
    } else if (!strcmp(cmd, "addkey") && argc == 4) {
        walk(h, argv[3], 1);
    } else if (!strcmp(cmd, "bcdtree") && argc == 3) {
        hive_node_h objs = walk(h, "\\Objects", 0);
        hive_node_h *kids = hivex_node_children(h, objs);
        if (!kids) die("no Objects key");
        for (hive_node_h *k = kids; *k; k++) {
            char *nm = hivex_node_name(h, *k);
            printf("OBJ %s\n", nm);
            hive_node_h desc = hivex_node_get_child(h, *k, "Description");
            if (desc) {
                hive_value_h tv = hivex_node_get_value(h, desc, "Type");
                if (tv) {
                    int32_t d = hivex_value_dword(h, tv);
                    printf("  Type=0x%X\n", (unsigned)d);
                }
            }
            hive_node_h els = hivex_node_get_child(h, *k, "Elements");
            if (els) {
                hive_node_h *e = hivex_node_children(h, els);
                if (e) {
                    for (hive_node_h *x = e; *x; x++) {
                        char *en = hivex_node_name(h, *x);
                        printf("  EL %s\n", en);
                        free(en);
                    }
                    free(e);
                }
            }
            free(nm);
        }
        free(kids);
    } else if (!strcmp(cmd, "copytree") && argc == 6) {
        hive_node_h src = walk(h, argv[3], 0);
        hive_node_h dstp = walk(h, argv[4], 1);
        copy_tree(h, src, dstp, argv[5]);
    } else {
        fprintf(stderr, "hiveutil: bad args\n"); ret = 2;
    }

    if (ret == 0) hivex_commit(h, NULL, 0);
    hivex_close(h);
    return ret;
}
