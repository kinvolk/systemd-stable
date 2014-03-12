/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <blkid.h>

#ifdef HAVE_LINUX_BTRFS_H
#include <linux/btrfs.h>
#endif

#include "path-util.h"
#include "util.h"
#include "mkdir.h"
#include "missing.h"
#include "sd-id128.h"
#include "libudev.h"
#include "udev-util.h"
#include "special.h"
#include "unit-name.h"
#include "virt.h"

/* TODO:
 *
 * - Properly handle cryptsetup partitions
 * - Define new partition type for encrypted swap
 * - Make /home automount rather than mount
 *
 */

#define GPT_SWAP SD_ID128_MAKE(06,57,fd,6d,a4,ab,43,c4,84,e5,09,33,c8,4b,4f,4f)
#define GPT_HOME SD_ID128_MAKE(93,3a,c7,e1,2e,b4,4f,13,b8,44,0e,14,e2,ae,f9,15)

static const char *arg_dest = "/tmp";

DEFINE_TRIVIAL_CLEANUP_FUNC(blkid_probe, blkid_free_probe);
#define _cleanup_blkid_freep_probe_ _cleanup_(blkid_free_probep)

static int verify_gpt_partition(const char *node, sd_id128_t *type, unsigned *nr, char **fstype) {
        _cleanup_blkid_freep_probe_ blkid_probe b = NULL;
        const char *v;
        int r;

        errno = 0;
        b = blkid_new_probe_from_filename(node);
        if (!b)
                return errno != 0 ? -errno : -ENOMEM;

        blkid_probe_enable_superblocks(b, 1);
        blkid_probe_set_superblocks_flags(b, BLKID_SUBLKS_TYPE);
        blkid_probe_enable_partitions(b, 1);
        blkid_probe_set_partitions_flags(b, BLKID_PARTS_ENTRY_DETAILS);

        errno = 0;
        r = blkid_do_safeprobe(b);
        if (r == -2 || r == 1) /* no result or uncertain */
                return -EBADSLT;
        else if (r != 0)
                return errno ? -errno : -EIO;

        errno = 0;
        r = blkid_probe_lookup_value(b, "PART_ENTRY_SCHEME", &v, NULL);
        if (r != 0)
                /* return 0 if we're not on GPT */
                return errno ? -errno : 0;

        if (strcmp(v, "gpt") != 0)
                return 0;

        if (type) {
                errno = 0;
                r = blkid_probe_lookup_value(b, "PART_ENTRY_TYPE", &v, NULL);
                if (r != 0)
                        return errno ? -errno : -EIO;

                r = sd_id128_from_string(v, type);
                if (r < 0)
                        return r;
        }

        if (nr) {
                errno = 0;
                r = blkid_probe_lookup_value(b, "PART_ENTRY_NUMBER", &v, NULL);
                if (r != 0)
                        return errno ? -errno : -EIO;

                r = safe_atou(v, nr);
                if (r < 0)
                        return r;
        }


        if (fstype) {
                errno = 0;
                r = blkid_probe_lookup_value(b, "TYPE", &v, NULL);
                if (r != 0)
                        *fstype = NULL;
                else {
                        char *fst;

                        fst = strdup(v);
                        if (!fst)
                                return -ENOMEM;

                        *fstype = fst;
                }
        }

        return 1;
}

static int add_swap(const char *path, const char *fstype) {
        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        log_debug("Adding swap: %s %s", path, fstype);

        name = unit_name_from_path(path, ".swap");
        if (!name)
                return log_oom();

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "DefaultDependencies=no\n"
                "Conflicts=" SPECIAL_UMOUNT_TARGET "\n"
                "Before=" SPECIAL_UMOUNT_TARGET " " SPECIAL_SWAP_TARGET "\n\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET ".wants/", name, NULL);
        if (!lnk)
                return log_oom();

        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0) {
                log_error("Failed to create symlink %s: %m", lnk);
                return -errno;
        }

        return 0;
}

static int add_home(const char *path, const char *fstype) {
        _cleanup_free_ char *unit = NULL, *lnk = NULL, *fsck = NULL;
        _cleanup_fclose_ FILE *f = NULL;

        if (dir_is_empty("/home") <= 0)
                return 0;

        log_debug("Adding home: %s %s", path, fstype);

        unit = strappend(arg_dest, "/home.mount");
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                log_error("Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fsck = unit_name_from_path_instance("systemd-fsck", path, ".service");
        if (!fsck)
                return log_oom();

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "DefaultDependencies=no\n"
                "Requires=%s\n"
                "After=" SPECIAL_LOCAL_FS_PRE_TARGET " %s\n"
                "Conflicts=" SPECIAL_UMOUNT_TARGET "\n"
                "Before=" SPECIAL_UMOUNT_TARGET " " SPECIAL_LOCAL_FS_TARGET "\n\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=/home\n"
                "Type=%s\n",
                fsck, fsck, path, fstype);

        fflush(f);
        if (ferror(f)) {
                log_error("Failed to write unit file %s: %m", unit);
                return -errno;
        }

        lnk = strjoin(arg_dest, "/" SPECIAL_LOCAL_FS_TARGET ".requires/home.mount", NULL);
        if (!lnk)
                return log_oom();


        mkdir_parents_label(lnk, 0755);
        if (symlink(unit, lnk) < 0) {
                log_error("Failed to create symlink %s: %m", lnk);
                return -errno;
        }

        return 0;
}

static int enumerate_partitions(struct udev *udev, dev_t dev) {
        struct udev_device *parent = NULL;
        _cleanup_udev_enumerate_unref_ struct udev_enumerate *e = NULL;
        _cleanup_udev_device_unref_ struct udev_device *d = NULL;
        struct udev_list_entry *first, *item;
        unsigned home_nr = (unsigned) -1;
        _cleanup_free_ char *home = NULL, *home_fstype = NULL;
        int r;

        e = udev_enumerate_new(udev);
        if (!e)
                return log_oom();

        d = udev_device_new_from_devnum(udev, 'b', dev);
        if (!d)
                return log_oom();

        parent = udev_device_get_parent(d);
        if (!parent)
                return 0;

        r = udev_enumerate_add_match_parent(e, parent);
        if (r < 0)
                return log_oom();

        r = udev_enumerate_add_match_subsystem(e, "block");
        if (r < 0)
                return log_oom();

        r = udev_enumerate_scan_devices(e);
        if (r < 0) {
                log_error("Failed to enumerate partitions on /dev/block/%u:%u: %s",
                          major(dev), minor(dev), strerror(-r));
                return r;
        }

        first = udev_enumerate_get_list_entry(e);
        udev_list_entry_foreach(item, first) {
                _cleanup_free_ char *fstype = NULL;
                const char *node = NULL;
                _cleanup_udev_device_unref_ struct udev_device *q;
                sd_id128_t type_id;
                unsigned nr;

                q = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                if (!q)
                        return log_oom();

                if (udev_device_get_devnum(q) == udev_device_get_devnum(d))
                        continue;

                if (udev_device_get_devnum(q) == udev_device_get_devnum(parent))
                        continue;

                node = udev_device_get_devnode(q);
                if (!node)
                        return log_oom();

                r = verify_gpt_partition(node, &type_id, &nr, &fstype);
                if (r < 0) {
                        /* skip child devices which are not detected properly */
                        if (r == -EBADSLT)
                                continue;
                        log_error("Failed to verify GPT partition %s: %s",
                                  node, strerror(-r));
                        return r;
                }
                if (r == 0)
                        continue;

                if (sd_id128_equal(type_id, GPT_SWAP))
                        add_swap(node, fstype);
                else if (sd_id128_equal(type_id, GPT_HOME)) {
                        if (!home || nr < home_nr) {
                                free(home);
                                home = strdup(node);
                                if (!home)
                                        return log_oom();

                                home_nr = nr;

                                free(home_fstype);
                                home_fstype = fstype;
                                fstype = NULL;
                        }
                }
        }

        if (home && home_fstype)
                add_home(home, home_fstype);

        return r;
}

static int get_btrfs_block_device(const char *path, dev_t *dev) {
        struct btrfs_ioctl_fs_info_args fsi = {};
        _cleanup_close_ int fd = -1;
        uint64_t id;

        assert(path);
        assert(dev);

        fd = open(path, O_DIRECTORY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (ioctl(fd, BTRFS_IOC_FS_INFO, &fsi) < 0)
                return -errno;

        /* We won't do this for btrfs RAID */
        if (fsi.num_devices != 1)
                return 0;

        for (id = 1; id <= fsi.max_id; id++) {
                struct btrfs_ioctl_dev_info_args di = {
                        .devid = id,
                };
                struct stat st;

                if (ioctl(fd, BTRFS_IOC_DEV_INFO, &di) < 0) {
                        if (errno == ENODEV)
                                continue;

                        return -errno;
                }

                if (stat((char*) di.path, &st) < 0)
                        return -errno;

                if (!S_ISBLK(st.st_mode))
                        return -ENODEV;

                if (major(st.st_rdev) == 0)
                        return -ENODEV;

                *dev = st.st_rdev;
                return 1;
        }

        return -ENODEV;
}

static int get_block_device(const char *path, dev_t *dev) {
        struct stat st;
        struct statfs sfs;

        assert(path);
        assert(dev);

        if (lstat("/", &st))
                return -errno;

        if (major(st.st_dev) != 0) {
                *dev = st.st_dev;
                return 1;
        }

        if (statfs("/", &sfs) < 0)
                return -errno;

        if (F_TYPE_EQUAL(sfs.f_type, BTRFS_SUPER_MAGIC))
                return get_btrfs_block_device(path, dev);

        return 0;
}

static int devno_to_devnode(struct udev *udev, dev_t devno, char **ret) {
        _cleanup_udev_device_unref_ struct udev_device *d;
        const char *t;
        char *n;

        d = udev_device_new_from_devnum(udev, 'b', devno);
        if (!d)
                return log_oom();

        t = udev_device_get_devnode(d);
        if (!t)
                return -ENODEV;

        n = strdup(t);
        if (!n)
                return -ENOMEM;

        *ret = n;
        return 0;
}

int main(int argc, char *argv[]) {
        _cleanup_free_ char *node = NULL;
        _cleanup_udev_unref_ struct udev *udev = NULL;
        dev_t devno;
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                r = -EINVAL;
                goto finish;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        if (in_initrd()) {
                log_debug("In initrd, exiting.");
                goto finish;
        }

        if (detect_container(NULL) > 0) {
                log_debug("In a container, exiting.");
                goto finish;
        }

        r = get_block_device("/", &devno);
        if (r < 0) {
                log_error("Failed to determine block device of root file system: %s", strerror(-r));
                goto finish;
        }
        if (r == 0) {
                log_debug("Root file system not on a (single) block device.");
                goto finish;
        }

        udev = udev_new();
        if (!udev) {
                r = log_oom();
                goto finish;
        }

        r = devno_to_devnode(udev, devno, &node);
        if (r < 0) {
                log_error("Failed to determine block device node from major/minor: %s", strerror(-r));
                goto finish;
        }

        log_debug("Root device %s.", node);

        r = verify_gpt_partition(node, NULL, NULL, NULL);
        if (r < 0) {
                log_error("Failed to verify GPT partition %s: %s", node, strerror(-r));
                goto finish;
        }
        if (r == 0)
                goto finish;

        r = enumerate_partitions(udev, devno);

finish:
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
