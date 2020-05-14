#include <sys/sysmacros.h>
#include <sys/mount.h>
#include <sys/types.h>

#include <errno.h>
#include <libgen.h>
#undef basename /* Use the GNU version of basename. */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>

#include "nvc_internal.h"

#include "error.h"
#include "options.h"
#include "utils.h"
#include "xfuncs.h"
#include "jetson_mount.h"
#define stringify(s__) stringify__(s__)
#define stringify__(s__) #s__
static const char *hostlibdir = stringify(HOST_LIBDIR) "/";

char **
mount_jetson_files(struct error *err, const char *root, const struct nvc_container *cnt, const char *dir, char *paths[], size_t size)
{
        char src[PATH_MAX];
        char dst[PATH_MAX];
        mode_t mode;
        char **mnt, **ptr;
        char *file;

        mnt = ptr = array_new(err, size + 1); /* NULL terminated. */
        if (mnt == NULL)
                return (NULL);

        for (size_t i = 0; i < size; ++i) {
                int samepath = 0;
                if (path_new(err, src, root) < 0)
                        goto fail;
                if (dir != NULL) {
                        size_t hostlibdirlen = strlen(hostlibdir);
                        /*
                         * Special hackery to handle the case where the host does
                         * *not* use debian multiarch by default but has files located
                         * under the /usr/lib/aarch64-linux-gnu directory to satisfy
                         * some compiled-in hard-coded paths in NVIDIA binaries that
                         * assume debian multiarch.
                         *
                         * Also don't move to the container's libdir if the path of
                         * the file we're mounting is *not* under the host libdir.
                         */
                        if (!str_has_prefix(paths[i], hostlibdir) ||
                            (strcmp(hostlibdir, dir) != 0 && str_has_prefix(paths[i], dir))) {
                                samepath = 1;
                                if (path_new(err, dst, cnt->cfg.rootfs) < 0)
                                        goto fail;
                        } else if (str_has_prefix(paths[i], hostlibdir) &&
                                   strchr(paths[i]+hostlibdirlen, '/') != NULL) {
                                char tmp[PATH_MAX], sub[PATH_MAX], *cp;
                                strcpy(sub, paths[i]+hostlibdirlen);
                                for (cp = sub + strlen(sub); cp > sub && *cp != '/'; cp--);
                                *cp = '\0';
                                log_infof("%s: %s subpath: %s", __func__, paths[i], sub);
                                if (path_join(err, tmp, dir, sub) < 0)
                                        goto fail;
                                if (path_resolve_full(err, dst, cnt->cfg.rootfs, tmp) < 0)
                                        goto fail;
                        } else {
                                if (path_resolve_full(err, dst, cnt->cfg.rootfs, dir) < 0)
                                        goto fail;
                        }
                } else {
                        samepath = 1;
                        if (path_new(err, dst, cnt->cfg.rootfs) < 0)
                                goto fail;
                }

                file = basename(paths[i]);
                if (path_append(err, src, paths[i]) < 0)
                        goto fail;
                if (path_append(err, dst, (samepath ? paths[i] : file)) < 0)
                        goto fail;

                if (file_mode(err, src, &mode) < 0)
                        goto fail;
                if (file_create(err, dst, NULL, cnt->uid, cnt->gid, mode) < 0)
                        goto fail;

                log_infof("mounting %s at %s", src, dst);
                if (xmount(err, src, dst, NULL, MS_BIND, NULL) < 0)
                        goto fail;
                if (xmount(err, NULL, dst, NULL, MS_BIND|MS_REMOUNT | MS_RDONLY|MS_NODEV|MS_NOSUID, NULL) < 0)
                        goto fail;

                if ((*ptr++ = xstrdup(err, dst)) == NULL)
                        goto fail;
        }
        return (mnt);

 fail:
        for (size_t i = 0; i < size; ++i)
                unmount(mnt[i]);
        array_free(mnt, size);
        return (NULL);
}

int
create_jetson_symlinks(struct error *err, const char *root, const struct nvc_container *cnt, char *paths[], size_t size)
{
        char src[PATH_MAX];
        char src_lnk[PATH_MAX];
        char dst[PATH_MAX];
        char *file;

        for (size_t i = 0; i < size; ++i) {
                file = basename(paths[i]);
                if (path_new(err, src, root) < 0)
                        return (-1);
                if (path_append(err, src, paths[i]) < 0)
                        return (-1);

                if (resolve_symlink(err, src, src_lnk) < 0)
                        return (-1);

                if (str_has_prefix(file, "lib")) {
                        if (path_resolve_full(err, dst, cnt->cfg.rootfs, cnt->cfg.libs_dir) < 0)
                                return (-1);
                        if (path_append(err, dst, file) < 0)
                                return (-1);
                } else {
                        if (path_new(err, dst, cnt->cfg.rootfs) < 0)
                                return (-1);
                        if (path_append(err, dst, paths[i]) < 0)
                                return (-1);
                }

                printf("src: %s, src_lnk: %s, dst: %s\n", src, src_lnk, dst);
                if (remove(dst) < 0 && errno != ENOENT)
                        return (-1);

                log_infof("symlinking %s to %s", dst, src_lnk);
                if (file_create(err, dst, src_lnk, cnt->uid, cnt->gid, MODE_LNK(0777)) < 0)
                        return (-1);
        }

        return (0);
}

int resolve_symlink(struct error *err, const char *src, char *dst) {
        ssize_t n;

        n = readlink(src, dst, PATH_MAX);
        if (n < 0 || n >= PATH_MAX)
                return -1;

        dst[n] = '\0';

        return (0);
}
