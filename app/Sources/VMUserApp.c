#include "VMUserApp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vm_user_app_plan {
    vm_user_app_metadata_t metadata;
    rootfs_work_entry_t entries[ROOTFS_WORK_MAX_ENTRIES];
    size_t count;
    uint64_t bytes;
    uint8_t digest[32];
};

static bool fail(char *detail, size_t capacity, const char *message) {
    if (detail && capacity) (void)snprintf(detail, capacity, "%s", message);
    return false;
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[3] | ((uint32_t)p[2] << 8) |
           ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 24);
}

bool vm_user_app_validate_version(const char *version) {
    if (!version || !*version) return false;
    unsigned parts[3] = {0u, 0u, 0u};
    const char *p = version;
    for (unsigned i = 0u; i < 3u; i++) {
        if (*p < '0' || *p > '9') return false;
        do {
            parts[i] = parts[i] * 10u + (unsigned)(*p - '0');
            if (parts[i] > 255u) return false;
            p++;
        } while (*p >= '0' && *p <= '9');
        if (!*p) break;
        if (*p++ != '.' || i == 2u) return false;
    }
    return !*p && parts[0] > 0u &&
           ((parts[0] << 16) | (parts[1] << 8) | parts[2]) <= 0x00030103u;
}

static bool thin_macho(const uint8_t *b, size_t n, bool main_executable,
                       char *detail, size_t capacity) {
    if (n < 28u || le32(b) != 0xfeedfaceu || le32(b + 4u) != 12u ||
        (le32(b + 8u) & 0x00ffffffu) != 6u)
        return fail(detail, capacity, "This app needs a 32-bit ARMv6 executable for the iPhone 3G.");
    uint32_t type = le32(b + 12u), commands = le32(b + 16u), command_bytes = le32(b + 20u);
    if ((main_executable && type != 2u) || (!main_executable && type != 2u && type != 6u && type != 8u))
        return fail(detail, capacity, "The app contains an unsupported Mach-O file type.");
    if ((uint64_t)28u + command_bytes > n || commands > command_bytes / 8u)
        return fail(detail, capacity, "The executable's load-command table is truncated.");
    size_t offset = 28u, end = 28u + (size_t)command_bytes;
    for (uint32_t i = 0u; i < commands; i++) {
        if (end - offset < 8u) return fail(detail, capacity, "A Mach-O load command is truncated.");
        uint32_t command = le32(b + offset), size = le32(b + offset + 4u);
        if (size < 8u || (size & 3u) || size > end - offset)
            return fail(detail, capacity, "An executable load command has an invalid size.");
        if (command == 0x21u || command == 0x2cu) {
            if (size < 20u || le32(b + offset + 16u) != 0u)
                return fail(detail, capacity, "This app is encrypted. Import an unencrypted app you own; S5LBox does not remove DRM.");
            uint32_t crypt_offset = le32(b + offset + 8u), crypt_size = le32(b + offset + 12u);
            if ((uint64_t)crypt_offset + crypt_size > n)
                return fail(detail, capacity, "The executable encryption range is outside its slice.");
        }
        if (command == 0x24u || command == 0x2fu || command == 0x30u)
            return fail(detail, capacity, "This executable targets a different Apple platform.");
        if (command == 0x25u) {
            if (size < 16u || le32(b + offset + 8u) > 0x00030103u)
                return fail(detail, capacity, "This executable requires an iOS version newer than 3.1.3.");
        }
        if (command == 0x32u) {
            if (size < 24u || le32(b + offset + 8u) != 2u || le32(b + offset + 12u) > 0x00030103u)
                return fail(detail, capacity, "This executable's platform or minimum OS is incompatible with iPhone OS 3.1.3.");
        }
        offset += size;
    }
    return offset == end || fail(detail, capacity, "The executable load-command count does not match its size.");
}

bool vm_user_app_validate_macho(const uint8_t *b, size_t n, bool main_executable,
                               char *detail, size_t capacity) {
    if (!b || n < 4u) return fail(detail, capacity, "The app executable is missing or truncated.");
    if (be32(b) != 0xcafebabeu) return thin_macho(b, n, main_executable, detail, capacity);
    if (n < 8u) return fail(detail, capacity, "The universal executable header is truncated.");
    uint32_t count = be32(b + 4u);
    if (!count || count > 32u || 8u + (uint64_t)count * 20u > n)
        return fail(detail, capacity, "The universal executable has an invalid slice table.");
    const uint8_t *selected = NULL;
    size_t selected_size = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        const uint8_t *arch = b + 8u + (size_t)i * 20u;
        uint32_t off = be32(arch + 8u), size = be32(arch + 12u), alignment = be32(arch + 16u);
        if (off < 8u + count * 20u || size < 28u || (uint64_t)off + size > n ||
            alignment > 30u || ((uint64_t)off & (((uint64_t)1u << alignment) - 1u)))
            return fail(detail, capacity, "The universal executable has an invalid slice range.");
        for (uint32_t j = 0u; j < i; j++) {
            const uint8_t *previous = b + 8u + (size_t)j * 20u;
            uint64_t prior_off = be32(previous + 8u), prior_size = be32(previous + 12u);
            if ((uint64_t)off < prior_off + prior_size && prior_off < (uint64_t)off + size)
                return fail(detail, capacity, "Universal executable slices overlap.");
        }
        if (be32(arch) == 12u && (be32(arch + 4u) & 0x00ffffffu) == 6u) {
            if (selected) return fail(detail, capacity, "The universal executable has duplicate ARMv6 slices.");
            selected = b + off;
            selected_size = size;
        }
    }
    return selected ? thin_macho(selected, selected_size, main_executable, detail, capacity)
                    : fail(detail, capacity, "This app has no ARMv6 slice for the iPhone 3G.");
}

static bool safe_name(const char *name, bool directory) {
    size_t n = strlen(name), start = 0u, depth = 0u;
    if (!n || name[0] == '/' || n >= VMFW_ZIP_MAX_NAME) return false;
    if (directory) n--;
    if (!n) return false;
    for (size_t i = 0u; i <= n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (i == n || c == '/') {
            size_t length = i - start;
            if (!length || (length == 1u && name[start] == '.') ||
                (length == 2u && name[start] == '.' && name[start + 1u] == '.') ||
                ++depth > ROOTFS_WORK_MAX_PATH_DEPTH) return false;
            start = i + 1u;
        } else if (c < 32u || c > 126u || c == '\\' || c == ':') return false;
    }
    return true;
}

typedef struct {
    vmfw_zip_entry_t entries[ROOTFS_WORK_MAX_ENTRIES];
    size_t count;
    uint64_t total;
    char prefix[VMFW_ZIP_MAX_NAME];
    char *detail;
    size_t capacity;
    bool ok;
} archive_t;

static bool collect_entry(void *context, const vmfw_zip_entry_t *entry, uint32_t index) {
    archive_t *a = context;
    (void)index;
    if (a->count == ROOTFS_WORK_MAX_ENTRIES || !safe_name(entry->name, entry->is_directory))
        return a->ok = fail(a->detail, a->capacity, "The IPA has too many entries or an unsafe/unsupported path. Paths must use printable ASCII.");
    size_t length = strlen(entry->name) - (entry->is_directory ? 1u : 0u);
    for (size_t i = 0u; i < a->count; i++) {
        size_t prior = strlen(a->entries[i].name) - (a->entries[i].is_directory ? 1u : 0u);
        if (length == prior && !memcmp(entry->name, a->entries[i].name, length))
            return a->ok = fail(a->detail, a->capacity, "The IPA contains duplicate file or directory names.");
    }
    if (entry->uncompressed_size > ROOTFS_WORK_MAX_ENTRY_BYTES ||
        a->total + entry->uncompressed_size > VM_USER_APP_MAX_CONTENT ||
        (entry->is_directory && entry->uncompressed_size))
        return a->ok = fail(a->detail, a->capacity, "The IPA exceeds this importer's limits: 16 MiB per file and 64 MiB expanded.");
    a->total += entry->uncompressed_size;
    if (!strncmp(entry->name, "Payload/", 8u) && entry->name[8]) {
        const char *slash = strchr(entry->name + 8u, '/');
        if (!slash || slash - entry->name < 13 || memcmp(slash - 4, ".app", 4u))
            return a->ok = fail(a->detail, a->capacity, "Payload must contain exactly one .app bundle.");
        size_t prefix_length = (size_t)(slash - entry->name) + 1u;
        if (!a->prefix[0]) {
            memcpy(a->prefix, entry->name, prefix_length);
            a->prefix[prefix_length] = '\0';
        } else if (strlen(a->prefix) != prefix_length || memcmp(a->prefix, entry->name, prefix_length))
            return a->ok = fail(a->detail, a->capacity, "Import one app at a time; this IPA contains multiple bundles.");
    }
    a->entries[a->count++] = *entry;
    return true;
}

/* The IPSW ZIP API intentionally discards UNIX attributes. App import must
 * reject symlinks/special files instead of mistaking their targets for data.
 * Cross-check the central and local names too; extraction uses central names. */
static bool audit_headers(const vmfw_zip_t *zip, const archive_t *a, char *detail, size_t capacity) {
    uint64_t offset = zip->cd_offset;
    for (size_t i = 0u; i < a->count; i++) {
        uint8_t h[46], local[30], name[VMFW_ZIP_MAX_NAME];
        if (zip->pread(zip->ctx, offset, h, sizeof h) != sizeof h)
            return fail(detail, capacity, "Could not read IPA file attributes.");
        uint32_t attributes = le32(h + 38u), mode = attributes >> 16;
        uint16_t name_size = le16(h + 28u);
        bool directory = a->entries[i].is_directory;
        if ((h[5] == 3u && (mode & 0170000u) && (mode & 0170000u) != (directory ? 0040000u : 0100000u)) ||
            ((attributes & 0x10u) && !directory))
            return fail(detail, capacity, "IPA symlinks and special files are not supported.");
        if (zip->pread(zip->ctx, a->entries[i].local_header_offset, local, sizeof local) != sizeof local ||
            le32(local) != 0x04034b50u || le16(local + 26u) != name_size || name_size >= sizeof name ||
            zip->pread(zip->ctx, a->entries[i].local_header_offset + 30u, name, name_size) != name_size ||
            memcmp(name, a->entries[i].name, name_size))
            return fail(detail, capacity, "The IPA's local and central file names disagree.");
        offset += 46u + (uint64_t)name_size + le16(h + 30u) + le16(h + 32u);
    }
    return true;
}

typedef struct { uint8_t *bytes; size_t size, used; } sink_t;
static bool sink(void *context, const uint8_t *bytes, size_t n) {
    sink_t *s = context;
    if (n > s->size - s->used) return false;
    memcpy(s->bytes + s->used, bytes, n);
    s->used += n;
    return true;
}
static uint8_t *extract(const vmfw_zip_t *zip, const vmfw_zip_entry_t *entry, char *detail, size_t capacity) {
    size_t n = (size_t)entry->uncompressed_size;
    uint8_t *b = malloc(n ? n : 1u);
    if (!b) { fail(detail, capacity, "Not enough memory to prepare this app."); return NULL; }
    sink_t s = {b, n, 0u};
    vmfw_zip_status_t status = vmfw_zip_extract(zip, entry, sink, &s);
    if (status != VMFW_ZIP_OK || s.used != n) {
        fail(detail, capacity, vmfw_zip_strerror(status)); free(b); return NULL;
    }
    return b;
}

static bool add_entry(vm_user_app_plan_t *p, const char *path, bool directory,
                      uint8_t *content, size_t size, bool executable,
                      char *detail, size_t capacity) {
    for (size_t i = 0u; i < p->count; i++) if (!strcmp(path, p->entries[i].path)) {
        if (directory && p->entries[i].kind == ROOTFS_WORK_ENTRY_DIRECTORY) return true;
        return fail(detail, capacity, "The app has conflicting file and directory paths.");
    }
    if (p->count == ROOTFS_WORK_MAX_ENTRIES)
        return fail(detail, capacity, "The app requires more than 1024 filesystem entries.");
    size_t n = strlen(path) + 1u;
    char *owned_path = malloc(n);
    if (!owned_path) return fail(detail, capacity, "Not enough memory for the app's file names.");
    memcpy(owned_path, path, n);
    rootfs_work_entry_t *e = &p->entries[p->count++];
    e->path = owned_path;
    e->kind = directory ? ROOTFS_WORK_ENTRY_DIRECTORY : ROOTFS_WORK_ENTRY_FILE;
    e->content = content;
    e->content_size = size;
    e->permissions = directory || executable ? 0755u : 0644u;
    p->bytes += size;
    return true;
}

static bool identifier_safe(const char *s) {
    if (!s || !*s || *s == '.' || !strcmp(s, ".") || !strcmp(s, "..")) return false;
    for (; *s; s++) if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
        (*s >= '0' && *s <= '9') || *s == '.' || *s == '-')) return false;
    return true;
}

/* The digest names exactly the paths and bytes the transaction writes. */
static void plan_rehash(vm_user_app_plan_t *p) {
    ios3_sha256_context_t hash;
    (void)ios3_sha256_init(&hash);
    for (size_t i = 0u; i < p->count; i++) {
        const rootfs_work_entry_t *e = &p->entries[i];
        (void)ios3_sha256_update(&hash, e->path, strlen(e->path) + 1u);
        (void)ios3_sha256_update(&hash, e->content, e->content_size);
    }
    (void)ios3_sha256_final(&hash, p->digest);
}

vm_user_app_plan_t *vm_user_app_plan_open(vmfw_pread_fn read, void *read_context, uint64_t archive_size,
    vm_user_app_plist_fn parse, void *plist_context, char *detail, size_t capacity) {
    if (detail && capacity) detail[0] = '\0';
    if (!read || !parse || !archive_size || archive_size > VM_USER_APP_MAX_ARCHIVE) {
        fail(detail, capacity, "Choose an IPA file no larger than 128 MiB."); return NULL;
    }
    vmfw_zip_t zip;
    vmfw_zip_status_t status = vmfw_zip_open(&zip, read, read_context, archive_size);
    if (status != VMFW_ZIP_OK) { fail(detail, capacity, vmfw_zip_strerror(status)); return NULL; }
    archive_t *a = calloc(1u, sizeof *a);
    vm_user_app_plan_t *p = calloc(1u, sizeof *p);
    if (!a || !p) { fail(detail, capacity, "Not enough memory to inspect this IPA."); free(a); free(p); return NULL; }
    a->ok = true; a->detail = detail; a->capacity = capacity;
    status = vmfw_zip_iterate(&zip, collect_entry, a);
    if (status != VMFW_ZIP_OK) fail(detail, capacity, vmfw_zip_strerror(status));
    if (status != VMFW_ZIP_OK || !a->ok || !audit_headers(&zip, a, detail, capacity)) goto failed;
    if (!a->prefix[0]) { fail(detail, capacity, "This IPA has no Payload/*.app bundle."); goto failed; }
    size_t prefix_size = strlen(a->prefix);
    uint8_t *plist = NULL;
    size_t plist_size = 0u;
    for (size_t i = 0u; i < a->count; i++) {
        vmfw_zip_entry_t *e = &a->entries[i];
        if (!strncmp(e->name, a->prefix, prefix_size) && !strcmp(e->name + prefix_size, "Info.plist")) {
            if (e->is_directory || !e->uncompressed_size || e->uncompressed_size > VM_USER_APP_MAX_PLIST) {
                fail(detail, capacity, "Info.plist is missing, empty, or larger than 1 MiB."); goto failed;
            }
            plist = extract(&zip, e, detail, capacity); plist_size = (size_t)e->uncompressed_size; break;
        }
    }
    if (!plist) { fail(detail, capacity, "The app's Info.plist could not be read."); goto failed; }
    bool parsed = parse(plist_context, plist, plist_size, &p->metadata, detail, capacity);
    free(plist);
    if (!parsed) goto failed;
    if (!memchr(p->metadata.identifier, 0, sizeof p->metadata.identifier) ||
        !memchr(p->metadata.executable, 0, sizeof p->metadata.executable) ||
        !memchr(p->metadata.display_name, 0, sizeof p->metadata.display_name) ||
        !memchr(p->metadata.minimum_os, 0, sizeof p->metadata.minimum_os) ||
        !p->metadata.iphone_application || !identifier_safe(p->metadata.identifier) ||
        !safe_name(p->metadata.executable, false) || strchr(p->metadata.executable, '/') ||
        !vm_user_app_validate_version(p->metadata.minimum_os)) {
        fail(detail, capacity, "The app must declare a valid iPhone bundle, executable, and minimum OS no newer than 3.1.3."); goto failed;
    }
    char base[ROOTFS_WORK_MAX_PATH];
    (void)snprintf(base, sizeof base, "/Applications/%s.app", p->metadata.identifier);
    if (!add_entry(p, base, true, NULL, 0u, false, detail, capacity)) goto failed;
    bool found_executable = false;
    for (size_t i = 0u; i < a->count; i++) {
        vmfw_zip_entry_t *e = &a->entries[i];
        if (strncmp(e->name, a->prefix, prefix_size) || !e->name[prefix_size]) continue;
        const char *relative = e->name + prefix_size;
        if (!strncmp(relative, "PlugIns/", 8u) || !strncmp(relative, "Frameworks/", 11u)) {
            fail(detail, capacity, "Embedded app extensions/frameworks are outside this iPhone OS 3 importer's support."); goto failed;
        }
        char path[ROOTFS_WORK_MAX_PATH];
        int wrote = snprintf(path, sizeof path, "%s/%s", base, relative);
        if (wrote < 0 || (size_t)wrote >= sizeof path) { fail(detail, capacity, "An app path is too long."); goto failed; }
        if (e->is_directory) path[strlen(path) - 1u] = '\0';
        for (char *slash = strchr(path + strlen(base) + 1u, '/'); slash; slash = strchr(slash + 1u, '/')) {
            *slash = '\0';
            bool added = add_entry(p, path, true, NULL, 0u, false, detail, capacity);
            *slash = '/';
            if (!added) goto failed;
        }
        if (e->is_directory) {
            if (!add_entry(p, path, true, NULL, 0u, false, detail, capacity)) goto failed;
            continue;
        }
        uint8_t *content = extract(&zip, e, detail, capacity);
        if (!content) goto failed;
        size_t size = (size_t)e->uncompressed_size;
        bool main = !strcmp(relative, p->metadata.executable);
        bool macho = size >= 4u && (le32(content) == 0xfeedfaceu || le32(content) == 0xfeedfacfu ||
                     be32(content) == 0xcafebabeu || be32(content) == 0xcafebabfu);
        if ((main || macho) && !vm_user_app_validate_macho(content, size, main, detail, capacity)) { free(content); goto failed; }
        if (main) found_executable = true;
        if (!add_entry(p, path, false, content, size, main || macho, detail, capacity)) { free(content); goto failed; }
    }
    if (!found_executable) { fail(detail, capacity, "CFBundleExecutable does not name a file in this app."); goto failed; }
    plan_rehash(p);
    free(a);
    return p;
failed:
    free(a);
    vm_user_app_plan_close(&p);
    return NULL;
}

void vm_user_app_plan_close(vm_user_app_plan_t **slot) {
    if (!slot || !*slot) return;
    vm_user_app_plan_t *p = *slot;
    for (size_t i = 0u; i < p->count; i++) {
        free((void *)p->entries[i].path);
        free((void *)p->entries[i].content);
    }
    free(p); *slot = NULL;
}
static bool ascii_case_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return *a == *b;
}

/* entries[0] is the bundle directory itself; a file is "<bundle>/<relative>". */
static rootfs_work_entry_t *plan_find(const vm_user_app_plan_t *p, const char *relative) {
    if (!p || !relative || !*relative || !p->count) return NULL;
    const size_t base = strlen(p->entries[0].path);
    for (size_t i = 1u; i < p->count; i++) {
        rootfs_work_entry_t *e = (rootfs_work_entry_t *)&p->entries[i];
        if (e->kind != ROOTFS_WORK_ENTRY_FILE || strncmp(e->path, p->entries[0].path, base) ||
            e->path[base] != '/')
            continue;
        if (ascii_case_equal(e->path + base + 1u, relative)) return e;
    }
    return NULL;
}

bool vm_user_app_plan_file(const vm_user_app_plan_t *p, const char *relative,
                           const uint8_t **bytes, size_t *size) {
    const rootfs_work_entry_t *e = plan_find(p, relative);
    if (!e || !bytes || !size) return false;
    *bytes = e->content;
    *size = e->content_size;
    return true;
}

bool vm_user_app_plan_replace_file(vm_user_app_plan_t *p, const char *relative,
                                   uint8_t *content, size_t size) {
    rootfs_work_entry_t *e = plan_find(p, relative);
    if (!e || !content || !size || size > ROOTFS_WORK_MAX_ENTRY_BYTES ||
        p->bytes - e->content_size + size > VM_USER_APP_MAX_CONTENT)
        return false;
    p->bytes = p->bytes - e->content_size + size;
    free((void *)e->content);
    e->content = content;
    e->content_size = size;
    plan_rehash(p);
    return true;
}

const vm_user_app_metadata_t *vm_user_app_plan_metadata(const vm_user_app_plan_t *p) { return p ? &p->metadata : NULL; }
const rootfs_work_entry_t *vm_user_app_plan_entries(const vm_user_app_plan_t *p) { return p ? p->entries : NULL; }
size_t vm_user_app_plan_entry_count(const vm_user_app_plan_t *p) { return p ? p->count : 0u; }
uint64_t vm_user_app_plan_content_bytes(const vm_user_app_plan_t *p) { return p ? p->bytes : 0u; }
bool vm_user_app_plan_digest(const vm_user_app_plan_t *p, uint8_t digest[32]) {
    if (!p || !digest) return false;
    memcpy(digest, p->digest, 32u); return true;
}
