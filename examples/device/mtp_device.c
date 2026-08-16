/**
 * Copyright (C) 2026 Jabez Winston
 *
 * SPDX-License-Identifier: MIT
 *
 * Date : 04-June-2026
 *
 * mtp_device.c - virtual USB MTP device over USB/IP (exports one or more directories).
 *
 *   ./mtp_device                          # exports a sample tree in /tmp
 *   ./mtp_device --dir ~/Music            # export your own directory (read-write!)
 *   ./mtp_device --dir ~/Music,~/Pictures # two directories = two MTP storages
 *   ./mtp_device --dir ~/Pictures --read-only
 *   attach it with a USB/IP client
 *   (Linux: sudo modprobe vhci-hcd ; sudo usbip attach -r 127.0.0.1 -b 1-1)
 *   then browse it in your OS's file manager
 *   (Linux headless: mtp-detect | mtp-files | mtp-getfile <id> out | mtp-sendfile in name)
 *
 * The MTP class (src/classes/device/mtp.c) does no filesystem access - this example
 * provides the storage backend (the FilesystemStore callbacks below; one per
 * directory passed via --dir), which is where all the open/mkdir/rmdir/stat/readdir
 * lives. The Microsoft-OS "MTP" descriptor is always advertised, so on Windows the
 * device shows up as an MTP portable device.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h> /* CreateDirectory/CreateFile/SetEndOfFile/GetDiskFreeSpaceExA */
#else
#include <sys/statvfs.h>
#endif
#include <unistd.h>

#include "_support/cmdline.h"
#include "_support/logging.h"
#include "classes/mtp.h"

#define SAMPLE_NAME "usbip_mtp_share"

/* Build a path to `name` in the OS temp directory: %TEMP% on Windows (which has
 * no /tmp), $TMPDIR or /tmp elsewhere. */
static const char *tmp_path(char *buf, size_t n, const char *name)
{
#ifdef _WIN32
    const char *dir = getenv("TEMP");
    if (!dir || !*dir)
        dir = getenv("TMP");
    if (!dir || !*dir)
        dir = ".";
    snprintf(buf, n, "%s\\%s", dir, name);
#else
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(buf, n, "%s/%s", dir, name);
#endif
    return buf;
}

/* ---- FilesystemStore: the per-storage backend (all filesystem access is here) ----
 * Each storage's context is its directory root; the class hands us relative
 * "/"-paths, where "" means that root. */
static void real_path(const char *root, const char *path, char *out, size_t n)
{
    if (path[0])
        snprintf(out, n, "%s/%s", root, path);
    else
        snprintf(out, n, "%s", root);
}

static int rmtree(const char *path)
{
    struct stat st;
#ifdef _WIN32
    if (stat(path, &st) != 0)
        return -1; /* no symlinks on Windows */
#else
    if (lstat(path, &st) != 0)
        return -1;
#endif
    if (S_ISDIR(st.st_mode))
    {
        DIR *dir = opendir(path);
        if (dir)
        {
            struct dirent *entry;
            while ((entry = readdir(dir)))
            {
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                    continue;
                char child[8192];
                snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
                rmtree(child);
            }
            closedir(dir);
        }
        return rmdir(path);
    }
    return remove(path);
}

static void store_listdir(void *store, const char *path, mtp_emit emit, void *ctx)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    DIR *dir = opendir(real);
    if (!dir)
        return;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            emit(ctx, entry->d_name);
    closedir(dir);
}

static int store_get_info(void *store, const char *path, int *is_dir, uint64_t *size, uint32_t *mtime)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    struct stat st;
    if (stat(real, &st) != 0)
        return -1;
    *is_dir = S_ISDIR(st.st_mode);
    *size = *is_dir ? 0 : (uint64_t)st.st_size;
    *mtime = (uint32_t)st.st_mtime;
    return 0;
}

static int store_read(void *store, const char *path, long off, uint8_t *buf, int want)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    FILE *fp = fopen(real, "rb");
    if (!fp)
        return 0;
    if (off)
        fseek(fp, off, SEEK_SET);
    int n_read = want > 0 ? (int)fread(buf, 1, (size_t)want, fp) : 0;
    fclose(fp);
    return n_read;
}

static int store_write(void *store, const char *path, const uint8_t *data, int len)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    FILE *fp = fopen(real, "wb");
    if (!fp)
        return -1;
    if (len > 0)
        fwrite(data, 1, (size_t)len, fp);
    fclose(fp);
    return 0;
}

static int store_make_dir(void *store, const char *path)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
#ifdef _WIN32
    return CreateDirectoryA(real, NULL) ? 0 : -1;
#else
    return mkdir(real, 0777);
#endif
}

static int store_remove(void *store, const char *path)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    return rmtree(real);
}

static int store_rename(void *store, const char *path, const char *newpath)
{
    char src[8192];
    char dst[8192];
    real_path(store, path, src, sizeof(src));
    real_path(store, newpath, dst, sizeof(dst));
    return rename(src, dst);
}

static int store_pwrite(void *store, const char *path, long off, const uint8_t *data, int len)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
    FILE *fp = fopen(real, "r+b");
    if (!fp)
        fp = fopen(real, "w+b");
    if (!fp)
        return -1;
    if (off)
        fseek(fp, off, SEEK_SET);
    if (len > 0)
        fwrite(data, 1, (size_t)len, fp);
    fclose(fp);
    return 0;
}

static int store_truncate(void *store, const char *path, uint64_t size)
{
    char real[8192];
    real_path(store, path, real, sizeof(real));
#ifdef _WIN32
    HANDLE handle = CreateFileA(real, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)size;
    int rc = (SetFilePointerEx(handle, li, NULL, FILE_BEGIN) && SetEndOfFile(handle)) ? 0 : -1;
    CloseHandle(handle);
    return rc;
#else
    return truncate(real, (off_t)size);
#endif
}

static void store_disk_usage(void *store, uint64_t *total, uint64_t *freeb)
{
#ifdef _WIN32
    ULARGE_INTEGER avail, tot;
    if (GetDiskFreeSpaceExA((const char *)store, &avail, &tot, NULL))
    {
        *total = (uint64_t)tot.QuadPart;
        *freeb = (uint64_t)avail.QuadPart;
    }
    else
    {
        *total = *freeb = 0;
    }
#else
    struct statvfs vfs;
    if (statvfs((const char *)store, &vfs) == 0)
    {
        *total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        *freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    }
    else
    {
        *total = *freeb = 0;
    }
#endif
}

static const char *store_description(void *store)
{
    const char *root = store, *base = strrchr(root, '/');
    return (base && base[1]) ? base + 1 : root; /* directory basename as the storage label */
}

static void write_file(const char *path, const char *data)
{
    FILE *fp = fopen(path, "wb");
    if (fp)
    {
        fputs(data, fp);
        fclose(fp);
    }
}

/* create a small browsable tree so there's something to see out of the box */
static void make_sample_tree(const char *root)
{
    char path[4096];
#ifdef _WIN32
    CreateDirectoryA(root, NULL);
    snprintf(path, sizeof(path), "%s/Documents", root);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof(path), "%s/Music", root);
    CreateDirectoryA(path, NULL);
#else
    mkdir(root, 0777);
    snprintf(path, sizeof(path), "%s/Documents", root);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/Music", root);
    mkdir(path, 0777);
#endif
    snprintf(path, sizeof(path), "%s/README.txt", root);
    write_file(path, "This is a usbip virtual MTP device.\n"
                     "Drop files in here (or delete them) and watch them appear on the host.\n");
    snprintf(path, sizeof(path), "%s/Documents/notes.txt", root);
    write_file(path, "hello from MTP\n");
    snprintf(path, sizeof(path), "%s/Music/track.txt", root);
    write_file(path, "(pretend this is an audio file)\n");
}

/* mtp-specific command-line options (the common ones live in _support/cmdline) */
struct mtp_cli
{
    const char *dir;
    const char *name;
    int read_only;
};

static int mtp_on_opt(int c, char *arg, void *u)
{
    struct mtp_cli *m = u;
    switch (c)
    {
        case 'd':
            m->dir = arg;
            return 1;

        case 'r':
            m->read_only = 1;
            return 1;

        case 'n':
            m->name = arg;
            return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct mtp_cli cli = {
        .name = "USBIP MTP",
    };
    char sample_dir[1024];
    tmp_path(sample_dir, sizeof(sample_dir), SAMPLE_NAME); /* OS temp dir (no /tmp on Windows) */

    static const cmdline_option options[] = {
        {"dir", CMDLINE_ARG_REQUIRED, 'd', "PATH[,PATH...]", "directory(ies) to export, each a separate MTP storage\n(default: a sample tree in the OS temp dir)"},
        {"read-only", CMDLINE_ARG_NONE, 'r', NULL, "forbid host writes/deletes"},
        {"name", CMDLINE_ARG_REQUIRED, 'n', "NAME", "device friendly name / model"},
        {0, 0, 0, 0, 0},
    };
    cmdline_opts cli_opts = cmdline_parse(argc, argv, &(cmdline_spec){
        .vid = 0x1209,
        .pid = 0x0010,
        .high_speed_opt = 1,
        .options = options,
        .on_opt = mtp_on_opt,
        .user = &cli,
    });

    /* each comma-separated directory becomes one MTP storage (its root is the context) */
    const char *roots[MTP_MAX_STORAGES];
    int n_stores = 0;
    if (!cli.dir)
    {
        make_sample_tree(sample_dir);
        roots[n_stores++] = sample_dir;
    }
    else
    {
        /* strtok mutates its input, and the tokens outlive this block (roots[] points
         * into them), so the copy is deliberately never freed. */
        char *dir_copy = strdup(cli.dir);
        for (char *tok = strtok(dir_copy, ","); tok; tok = strtok(NULL, ","))
        {
            if (n_stores == MTP_MAX_STORAGES)
            {
                fprintf(stderr, "--dir: at most %d directories, ignoring the rest\n",
                        MTP_MAX_STORAGES);
                break;
            }
            roots[n_stores++] = tok;
        }
    }

    usbip_logger logger;
    usbip_log_init(&logger, "mtp", cli_opts.verbose);

    //! [add]
    mtp_opts mopts = {
        .read_only = cli.read_only,
        .name = cli.name,
        .manufacturer = "USB over IP",
        .serial = NULL,
        .winusb = 1,        /* always advertise the MS-OS "MTP" Compatible ID */
        .n_stores = n_stores,
        .listdir = store_listdir,
        .get_info = store_get_info,
        .read = store_read,
        .write = store_write,
        .make_dir = store_make_dir,
        .remove = store_remove,
        .rename = store_rename,
        .pwrite = store_pwrite,
        .truncate = store_truncate,
        .disk_usage = store_disk_usage,
        .description = store_description,
        .on_event = usbip_log_cb,
        .user = &logger,
    };

    for (int i = 0; i < n_stores; i++)
        mopts.stores[i] = (void *)roots[i];

    usbip_device *dev = usbip_device_create(cli_opts.vid, cli_opts.pid);
    usbip_device_set_strings(dev, "USB over IP", cli.name, "000a");
    usb_speed speed = cli_opts.high_speed ? USB_SPEED_HIGH : USB_SPEED_FULL; /* HS = 512 B bulk */

    usbip_device_set_speed(dev, speed);
    mtp_func *mtp = mtp_add(dev, &mopts);   /* browsable by Explorer / libmtp / gphoto2 */
    //! [add]

    if (!mtp)
    {
        fprintf(stderr, "mtp_add failed\n");
        return 1;
    }

    fprintf(stderr, "[mtp] %d storage(s) (%s, MS-OS MTP)  (%04x:%04x, %s, on %s:%d)\n",
            n_stores, cli.read_only ? "read-only" : "read-write",
            cli_opts.vid, cli_opts.pid, cli_opts.high_speed ? "high speed" : "full speed", cli_opts.host, cli_opts.port);

    for (int i = 0; i < n_stores; i++)
        fprintf(stderr, "[mtp]   storage %d: %s\n", i + 1, roots[i]);

    fprintf(stderr, "[mtp] attach: sudo usbip attach -r 127.0.0.1 -b 1-1\n");
    fprintf(stderr, "[mtp] then browse it in your file manager (Linux headless: "
                    "mtp-detect | mtp-files | mtp-getfile <id> out | mtp-sendfile in name)\n");

    usb_transport *transport = usbip_transport(cli_opts.host, cli_opts.port);
    int rc = usbip_device_plug(dev, transport);

    if (rc != USB_SUCCESS)
    {
        fprintf(stderr, "usbip_device_plug failed (port %d in use?)\n", cli_opts.port);
        return 1;
    }
#ifdef _WIN32
    for (;;)
        sleep(1); /* no pause() on Windows; idle */
#else
    for (;;)
        pause();
#endif
    return 0;
}
