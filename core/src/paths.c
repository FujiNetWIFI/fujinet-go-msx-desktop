/*
 * msxsession path layout: XDG config/data directories, the openMSX runtime
 * share tree, and first-run provisioning of the FujiNet runtime tree
 * (fnconfig.ini, data/, SD/) from the installed share directory or the
 * development build output.
 *
 * Unlike the sibling repos there is no ROM materialisation here: C-BIOS is
 * bundled via openMSX's own share tree (tools/openmsx/build-openmsx-
 * desktop.sh overlays it into machines/), not as per-app embedded ROM blobs
 * this layer would write out. Manufacturer ROM import lands in M4.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

#if defined(_WIN32)
#include <direct.h> /* _getcwd */
#include <windows.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

/* Compiled-in development fallbacks (set by CMake to paths in the source /
 * build trees) so a git checkout runs without an install step. */
#ifndef MSX_DEV_OPENMSX_SHARE
#define MSX_DEV_OPENMSX_SHARE ""
#endif
#ifndef MSX_DEV_FUJINET_OUT
#define MSX_DEV_FUJINET_OUT ""
#endif
#ifndef MSX_INSTALL_DATADIR
#define MSX_INSTALL_DATADIR ""
#endif
#ifndef MSX_INSTALL_DATADIR_SHARE
#define MSX_INSTALL_DATADIR_SHARE ""
#endif
#ifndef MSX_INSTALL_LIBDIR
#define MSX_INSTALL_LIBDIR ""
#endif

/* Directory holding the running executable, or "" when it cannot be
 * determined. A Windows install is a folder you copy around -- the exe and
 * fujinet.dll side by side -- so that folder is the first place to look for
 * the runtime. Elsewhere the install/dev directories baked in at configure
 * time answer the question. */
static const char *exe_dir(void)
{
    static char dir[MSX_PATH_MAX];
#if defined(_WIN32)
    static int resolved;
    char *p;

    if (!resolved) {
        resolved = 1;
        if (GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir)) == 0) {
            dir[0] = '\0';
        } else {
            dir[sizeof(dir) - 1] = '\0';
            p = strrchr(dir, '\\');
            if (!p)
                p = strrchr(dir, '/');
            if (p)
                *p = '\0';
            else
                dir[0] = '\0';
        }
    }
#endif
    return dir;
}

/* mkdir differs: POSIX takes a mode, the Windows CRT does not. */
static int make_dir(const char *path)
{
#if defined(_WIN32)
    return mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

static int is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

int mkdir_p(const char *path)
{
    char buf[MSX_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (is_sep(*p)) {
            char save = *p;
            *p = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) return -1;
            *p = save;
        }
    }
    if (make_dir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;
    int rc = 0;

    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}

static int copy_tree(const char *src, const char *dst)
{
    DIR *d;
    struct dirent *e;
    int rc = 0;

    if (mkdir_p(dst) != 0) return -1;
    d = opendir(src);
    if (!d) return -1;
    while ((e = readdir(d)) != NULL) {
        char from[MSX_PATH_MAX], to[MSX_PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        if (stat(from, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            rc |= copy_tree(from, to);
        else if (S_ISREG(st.st_mode))
            rc |= copy_file(from, to);
    }
    closedir(d);
    return rc;
}

/* Per-user config/data directory. On Windows this is %APPDATA% (config)
 * or %LOCALAPPDATA% (data); elsewhere the XDG variable, then $HOME/suffix. */
static void default_dir(char *dst, size_t dstsz, const char *xdg_env,
                        const char *win_env, const char *home_suffix)
{
#if defined(_WIN32)
    const char *v = getenv(win_env);
    (void)xdg_env;
    (void)home_suffix;
    snprintf(dst, dstsz, "%s\\fujinet-go-msx", (v && *v) ? v : ".");
#else
    const char *v = getenv(xdg_env);
    (void)win_env;
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-msx", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-msx", home ? home : ".",
                 home_suffix);
    }
#endif
}

/* Is this path already rooted? On Windows a leading separator is not enough:
 * "\foo" is relative to the current drive. */
static int is_absolute(const char *p)
{
    if (!p || !*p) return 0;
#if defined(_WIN32)
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
        p[1] == ':' && is_sep(p[2]))
        return 1;
    return is_sep(p[0]) && is_sep(p[1]); /* \\server\share */
#else
    return p[0] == '/';
#endif
}

/* Rewrite a relative directory in place against the current working
 * directory.
 *
 * More important here than on the ADAM/Apple II targets, same reasoning as
 * the CoCo port: the FujiNet entry wrapper chdir()s into its runtime root
 * before calling main_setup, and on THIS target FujiNet starts before
 * openMSX (msxsession_start starts FujiNet first) -- so a relative
 * OPENMSX_SYSTEM_DATA or fujinet path could silently resolve against the
 * wrong directory by the time it matters. Resolving once, at startup, means
 * nothing downstream has to care who chdir'd. */
static void make_absolute(char *path, size_t size)
{
    char cwd[MSX_PATH_MAX];
    char joined[MSX_PATH_MAX];

    if (is_absolute(path))
        return;
    if (!getcwd(cwd, sizeof(cwd)))
        return;
    snprintf(joined, sizeof(joined), "%s/%s", cwd, path);
    snprintf(path, size, "%s", joined);
}

/* Resolves the openMSX runtime share tree (init.tcl + scripts/ + machines/
 * incl. C-BIOS + extensions/ incl. FujiNet.xml). Search order: caller
 * override, $MSX_OPENMSX_SHARE, beside the executable (a packaged install
 * lays "openmsx/" there), the install datadir baked in at configure time,
 * then the dev build's raw script output. */
static int resolve_openmsx_share(msxsession *s, const char *override_dir)
{
    const char *env;
    char probe[MSX_PATH_MAX];

    if (override_dir && *override_dir) {
        snprintf(probe, sizeof(probe), "%s/init.tcl", override_dir);
        if (is_file(probe)) {
            snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s",
                     override_dir);
            return 0;
        }
    }

    env = getenv("MSX_OPENMSX_SHARE");
    if (env && *env) {
        snprintf(probe, sizeof(probe), "%s/init.tcl", env);
        if (is_file(probe)) {
            snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s", env);
            return 0;
        }
    }

    if (exe_dir()[0]) {
        snprintf(probe, sizeof(probe), "%s/openmsx/init.tcl", exe_dir());
        if (is_file(probe)) {
            snprintf(s->openmsx_share, sizeof(s->openmsx_share),
                     "%s/openmsx", exe_dir());
            return 0;
        }
    }

    snprintf(probe, sizeof(probe), "%s/init.tcl", MSX_INSTALL_DATADIR_SHARE);
    if (is_file(probe)) {
        snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s",
                 MSX_INSTALL_DATADIR_SHARE);
        return 0;
    }

    snprintf(probe, sizeof(probe), "%s/init.tcl", MSX_DEV_OPENMSX_SHARE);
    if (is_file(probe)) {
        snprintf(s->openmsx_share, sizeof(s->openmsx_share), "%s",
                 MSX_DEV_OPENMSX_SHARE);
        return 0;
    }

    return -1;
}

int paths_init(msxsession *s, const msxsession_paths *p)
{
    if (p && p->config_dir && *p->config_dir)
        snprintf(s->config_dir, sizeof(s->config_dir), "%s", p->config_dir);
    else
        default_dir(s->config_dir, sizeof(s->config_dir), "XDG_CONFIG_HOME",
                    "APPDATA", ".config");

    if (p && p->data_dir && *p->data_dir)
        snprintf(s->data_dir, sizeof(s->data_dir), "%s", p->data_dir);
    else
        default_dir(s->data_dir, sizeof(s->data_dir), "XDG_DATA_HOME",
                    "LOCALAPPDATA", ".local/share");

    make_absolute(s->config_dir, sizeof(s->config_dir));
    make_absolute(s->data_dir, sizeof(s->data_dir));

    if (mkdir_p(s->config_dir) != 0 || mkdir_p(s->data_dir) != 0)
        return -1;

    snprintf(s->settings_file, sizeof(s->settings_file), "%s/settings.ini",
             s->config_dir);

    if (resolve_openmsx_share(s, p ? p->openmsx_share : NULL) != 0) {
        session_set_error(s,
            "Could not find the openMSX runtime share tree (init.tcl). "
            "Set MSX_OPENMSX_SHARE, or build tools/openmsx/"
            "build-openmsx-desktop.sh first for a dev checkout.");
        return -1;
    }

    /* Cartridge images (.rom/.ccc) the user imports (M4). */
    snprintf(s->carts_dir, sizeof(s->carts_dir), "%s/carts", s->data_dir);
    mkdir_p(s->carts_dir);

    /* Manufacturer/turboR ROMs the user imports (M4). Never C-BIOS -- that
     * is always the openMSX share tree's own machines/, not this
     * directory. */
    {
        const char *env = getenv("MSX_ROM_DIR");
        if (env && *env)
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s", env);
        else
            snprintf(s->roms_dir, sizeof(s->roms_dir), "%s/roms", s->data_dir);
    }
    make_absolute(s->roms_dir, sizeof(s->roms_dir));
    mkdir_p(s->roms_dir);

    /* The FujiNet runtime tree. These are pure derivations of data_dir, so
     * they are settled here rather than in paths_provision_fujinet -- that
     * runs only when the runtime is actually started, and a session with
     * FujiNet disabled would otherwise have an empty SD path. */
    snprintf(s->fujinet_root, sizeof(s->fujinet_root), "%s/fujinet",
             s->data_dir);
    snprintf(s->fujinet_config, sizeof(s->fujinet_config), "%s/fnconfig.ini",
             s->fujinet_root);
    snprintf(s->fujinet_sd, sizeof(s->fujinet_sd), "%s/SD", s->fujinet_root);
    snprintf(s->fujinet_data, sizeof(s->fujinet_data), "%s/data",
             s->fujinet_root);

    if (p && p->fujinet_lib) /* may be "" = explicitly disabled */
        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", p->fujinet_lib);
    else
        s->fujinet_lib[0] = '\0'; /* resolved in paths_provision_fujinet */

    if (p && p->fujinet_runtime_src && *p->fujinet_runtime_src)
        snprintf(s->fujinet_src, sizeof(s->fujinet_src), "%s",
                 p->fujinet_runtime_src);
    else
        s->fujinet_src[0] = '\0';

    snprintf(s->webui_url, sizeof(s->webui_url), "http://127.0.0.1:%d/",
             MSXSESSION_WEBUI_PORT);
    return 0;
}

/* Locate libfujinet.so and provision the runtime tree on first run. The
 * runtime tree (fnconfig.ini + data/ + SD/) is copied from the newest
 * available source: the installed share dir or the dev build output. */
int paths_provision_fujinet(msxsession *s)
{
    const char *env;
    char src_root[MSX_PATH_MAX];
    char probe[MSX_PATH_MAX];

    /* Resolve the shared library unless the caller pinned/disabled it.
     * Every platform's name is probed so this path layer is shared
     * unchanged (.so Linux, .dylib macOS, .dll Windows). */
    if (!s->fujinet_lib[0]) {
        static const char *const names[] = {
            "libfujinet.so", "libfujinet.dylib", "fujinet.dll"};
        const char *const dirs[] = {exe_dir(), MSX_INSTALL_LIBDIR,
                                    MSX_DEV_FUJINET_OUT};
        const size_t nnames = sizeof(names) / sizeof(names[0]);
        const size_t ndirs = sizeof(dirs) / sizeof(dirs[0]);
        size_t di, ni;
        env = getenv("FUJINET_LIB");
        if (env && is_file(env)) {
            snprintf(s->fujinet_lib, sizeof(s->fujinet_lib), "%s", env);
        } else {
            for (di = 0; di < ndirs && !s->fujinet_lib[0]; di++)
                for (ni = 0; ni < nnames && !s->fujinet_lib[0]; ni++) {
                    if (!dirs[di][0])
                        continue;
                    snprintf(probe, sizeof(probe), "%s/%s", dirs[di],
                             names[ni]);
                    if (is_file(probe))
                        snprintf(s->fujinet_lib, sizeof(s->fujinet_lib),
                                 "%s", probe);
                }
        }
    }
    if (!s->fujinet_lib[0])
        return -1; /* no runtime available; session runs without FujiNet */

    /* Provision the runtime tree once (keep user data on later runs). */
    if (!is_file(s->fujinet_config)) {
        src_root[0] = '\0';
        if (s->fujinet_src[0]) {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini", s->fujinet_src);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s", s->fujinet_src);
        }
        /* Beside the executable, the layout a Windows install ships. */
        if (!src_root[0] && exe_dir()[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     exe_dir());
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet", exe_dir());
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fujinet/fnconfig.ini",
                     MSX_INSTALL_DATADIR);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s/fujinet",
                         MSX_INSTALL_DATADIR);
        }
        if (!src_root[0]) {
            snprintf(probe, sizeof(probe), "%s/fnconfig.ini",
                     MSX_DEV_FUJINET_OUT);
            if (is_file(probe))
                snprintf(src_root, sizeof(src_root), "%s",
                         MSX_DEV_FUJINET_OUT);
        }
        if (!src_root[0]) {
            session_set_error(s, "FujiNet runtime data not found (looked in "
                              "%s and %s)", MSX_INSTALL_DATADIR,
                              MSX_DEV_FUJINET_OUT);
            s->fujinet_lib[0] = '\0';
            return -1;
        }
        mkdir_p(s->fujinet_root);
        snprintf(probe, sizeof(probe), "%s/fnconfig.ini", src_root);
        copy_file(probe, s->fujinet_config);
        snprintf(probe, sizeof(probe), "%s/data", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_data);
        snprintf(probe, sizeof(probe), "%s/SD", src_root);
        if (is_dir(probe)) copy_tree(probe, s->fujinet_sd);
    }
    mkdir_p(s->fujinet_sd);
    mkdir_p(s->fujinet_data);
    return 0;
}
