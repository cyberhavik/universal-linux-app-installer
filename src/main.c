// ============================================================================
// Astro Universal App Installer (uapp / universal-app-installer)
// macOS-Style Drag & Drop Universal Linux Package Installer & Uninstaller
// Built in 100% Native C (GTK4 + Libadwaita) — Zero Python, Zero Root Prompts
//
// Supported Formats on ANY Linux Distro:
//   1. .rpm                  (Fedora, RHEL, openSUSE, CentOS)
//   2. .deb                  (Ubuntu, Debian, Linux Mint, Pop!_OS)
//   3. .AppImage / .appimage (Universal Linux AppImage Type 2)
//   4. .pkg.tar.zst / .xz    (Arch Linux, Manjaro, EndeavourOS, CachyOS)
//   5. .apk                  (Alpine Linux / Chimera Linux packages)
//   6. .flatpak              (Single-file Flatpak bundles)
//   7. .tar.gz / .tgz / .tar.xz / .tar.bz2 / .tar.zst / .zip (Portable tarballs)
// ============================================================================

#define _GNU_SOURCE
#include <adwaita.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// ----------------------------------------------------------------------------
// Package Format Enum & Metadata Structures
// ----------------------------------------------------------------------------

typedef enum {
    PKG_FMT_UNKNOWN = 0,
    PKG_FMT_RPM,
    PKG_FMT_DEB,
    PKG_FMT_APPIMAGE,
    PKG_FMT_ARCH,
    PKG_FMT_ALPINE,
    PKG_FMT_FLATPAK,
    PKG_FMT_ARCHIVE
} PackageFormat;

typedef struct {
    char *app_id;
    char *name;
    char *version;
    char *format_badge;
    char *summary;
    char *icon_path;
    char *exec_path;
    char *desktop_file;
    char *opt_dir;
    char *install_date;
    char *flatpak_ref;
    gint64 size_bytes;
} InstalledApp;

typedef struct {
    GtkApplication *app;
    AdwApplicationWindow *window;
    AdwToastOverlay *toast_overlay;
    GtkWidget *drop_box;
    GtkWidget *status_stack;
    GtkWidget *idle_page;
    GtkWidget *progress_page;
    GtkWidget *progress_title;
    GtkWidget *progress_subtitle;
    GtkProgressBar *progress_bar;
    GtkListBox *installed_list;
    GtkWidget *count_badge;
    guint pulse_timer_id;
    gboolean is_busy;
} AppUI;

typedef struct {
    AppUI *ui;
    char *pkg_path;
    char *app_id;
    char *app_name;
    char *app_version;
    char *format_badge;
    char *icon_path;
    char *error_msg;
    gboolean success;
    gboolean is_update;
} InstallTaskData;

// ----------------------------------------------------------------------------
// Global / Path Helpers
// ----------------------------------------------------------------------------

static char *get_uapps_base(void) {
    return g_build_filename(g_get_home_dir(), ".local", "opt", "uapps", NULL);
}

static char *get_legacy_rpm_base(void) {
    return g_build_filename(g_get_home_dir(), ".local", "opt", "rpm-apps", NULL);
}

static char *get_bin_dir(void) {
    return g_build_filename(g_get_home_dir(), ".local", "bin", NULL);
}

static char *get_applications_dir(void) {
    return g_build_filename(g_get_home_dir(), ".local", "share", "applications", NULL);
}

static char *get_icons_dir(void) {
    return g_build_filename(g_get_home_dir(), ".local", "share", "icons", "hicolor", "512x512", "apps", NULL);
}

static void free_installed_app(InstalledApp *info) {
    if (!info) return;
    g_free(info->app_id);
    g_free(info->name);
    g_free(info->version);
    g_free(info->format_badge);
    g_free(info->summary);
    g_free(info->icon_path);
    g_free(info->exec_path);
    g_free(info->desktop_file);
    g_free(info->opt_dir);
    g_free(info->install_date);
    g_free(info->flatpak_ref);
    g_free(info);
}

static const char *format_to_badge(PackageFormat fmt) {
    switch (fmt) {
        case PKG_FMT_RPM:      return "RPM";
        case PKG_FMT_DEB:      return "DEB";
        case PKG_FMT_APPIMAGE: return "AppImage";
        case PKG_FMT_ARCH:     return "Arch PKG";
        case PKG_FMT_ALPINE:   return "Alpine APK";
        case PKG_FMT_FLATPAK:  return "Flatpak";
        case PKG_FMT_ARCHIVE:  return "Portable";
        default:               return "Linux App";
    }
}

// Detect package format by filename extension and magic bytes
static PackageFormat detect_package_format(const char *path) {
    if (!path) return PKG_FMT_UNKNOWN;
    char *lower = g_ascii_strdown(path, -1);
    PackageFormat fmt = PKG_FMT_UNKNOWN;

    if (g_str_has_suffix(lower, ".rpm")) {
        fmt = PKG_FMT_RPM;
    } else if (g_str_has_suffix(lower, ".deb")) {
        fmt = PKG_FMT_DEB;
    } else if (g_str_has_suffix(lower, ".appimage") || g_str_has_suffix(lower, ".run")) {
        fmt = PKG_FMT_APPIMAGE;
    } else if (g_str_has_suffix(lower, ".pkg.tar.zst") ||
               g_str_has_suffix(lower, ".pkg.tar.xz") ||
               g_str_has_suffix(lower, ".pkg.tar.gz")) {
        fmt = PKG_FMT_ARCH;
    } else if (g_str_has_suffix(lower, ".apk")) {
        fmt = PKG_FMT_ALPINE;
    } else if (g_str_has_suffix(lower, ".flatpak")) {
        fmt = PKG_FMT_FLATPAK;
    } else if (g_str_has_suffix(lower, ".tar.gz") ||
               g_str_has_suffix(lower, ".tgz") ||
               g_str_has_suffix(lower, ".tar.xz") ||
               g_str_has_suffix(lower, ".txz") ||
               g_str_has_suffix(lower, ".tar.bz2") ||
               g_str_has_suffix(lower, ".tbz2") ||
               g_str_has_suffix(lower, ".tar.zst") ||
               g_str_has_suffix(lower, ".zip")) {
        fmt = PKG_FMT_ARCHIVE;
    }
    g_free(lower);

    // Fallback: inspect magic bytes
    if (fmt == PKG_FMT_UNKNOWN) {
        FILE *f = fopen(path, "rb");
        if (f) {
            unsigned char magic[16] = {0};
            size_t n = fread(magic, 1, sizeof(magic), f);
            fclose(f);
            if (n >= 4 && magic[0] == 0xed && magic[1] == 0xab && magic[2] == 0xee && magic[3] == 0xdb) {
                fmt = PKG_FMT_RPM;
            } else if (n >= 7 && memcmp(magic, "!<arch>", 7) == 0) {
                fmt = PKG_FMT_DEB;
            } else if (n >= 11 && magic[0] == 0x7f && memcmp(magic + 1, "ELF", 3) == 0 &&
                       magic[8] == 'A' && magic[9] == 'I') {
                fmt = PKG_FMT_APPIMAGE;
            }
        }
    }
    return fmt;
}

static char *sanitize_slug(const char *raw) {
    if (!raw || !*raw) return g_strdup("linux-app");
    GString *s = g_string_new("");
    for (const char *p = raw; *p; p++) {
        if (isalnum((unsigned char)*p)) {
            g_string_append_c(s, tolower((unsigned char)*p));
        } else if (*p == '-' || *p == '_' || *p == '.') {
            g_string_append_c(s, '-');
        }
    }
    while (s->len > 0 && s->str[0] == '-') g_string_erase(s, 0, 1);
    while (s->len > 0 && s->str[s->len - 1] == '-') g_string_truncate(s, s->len - 1);
    if (s->len == 0) g_string_assign(s, "linux-app");
    return g_string_free(s, FALSE);
}

// Strip package extensions & architecture suffixes from filename
static char *derive_clean_name_from_filename(const char *path) {
    char *base = g_path_get_basename(path);
    const char *exts[] = {
        ".pkg.tar.zst", ".pkg.tar.xz", ".pkg.tar.gz",
        ".tar.gz", ".tar.xz", ".tar.bz2", ".tar.zst",
        ".tgz", ".txz", ".tbz2", ".AppImage", ".appimage",
        ".flatpak", ".rpm", ".deb", ".apk", ".zip", ".run", NULL
    };
    for (int i = 0; exts[i]; i++) {
        if (g_str_has_suffix(base, exts[i])) {
            base[strlen(base) - strlen(exts[i])] = '\0';
            break;
        }
    }
    // Strip common arch suffixes like .x86_64, _amd64, -x86_64
    const char *archs[] = {".x86_64", "_x86_64", "-x86_64", "_amd64", "-amd64", ".amd64", ".aarch64", "_arm64", NULL};
    for (int i = 0; archs[i]; i++) {
        char *pos = strstr(base, archs[i]);
        if (pos) *pos = '\0';
    }
    // Strip trailing version segment like -1.2.3 or _26.1120
    for (char *p = base; *p; p++) {
        if ((*p == '-' || *p == '_') && isdigit((unsigned char)*(p + 1))) {
            *p = '\0';
            break;
        }
    }
    char *slug = sanitize_slug(base);
    g_free(base);
    return slug;
}

static char *format_size(gint64 bytes) {
    if (bytes <= 0) return g_strdup("Installed");
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb >= 1024.0) {
        return g_strdup_printf("%.2f GB", mb / 1024.0);
    }
    return g_strdup_printf("%.1f MB", mb);
}

static char *run_cmd_capture(const char *cmd, int *exit_code) {
    char *std_out = NULL;
    char *std_err = NULL;
    int status = 0;
    const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
    GError *err = NULL;

    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_DEFAULT, NULL, NULL,
                      &std_out, &std_err, &status, &err)) {
        if (err) g_error_free(err);
        if (exit_code) *exit_code = -1;
        g_free(std_out);
        g_free(std_err);
        return NULL;
    }
    g_free(std_err);
    if (exit_code) {
        if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
        else *exit_code = -1;
    }
    if (std_out) g_strstrip(std_out);
    return std_out;
}

static gint64 compute_dir_size_bytes(const char *dir_path) {
    char *q = g_shell_quote(dir_path);
    char *cmd = g_strdup_printf("du -sb %s 2>/dev/null | awk '{print $1}'", q);
    int code = 0;
    char *out = run_cmd_capture(cmd, &code);
    gint64 val = 0;
    if (out && *out) val = g_ascii_strtoll(out, NULL, 10);
    g_free(out);
    g_free(cmd);
    g_free(q);
    return val;
}

static void find_files_recursive(const char *base_dir, const char *ext, GPtrArray *results) {
    GDir *dir = g_dir_open(base_dir, 0, NULL);
    if (!dir) return;
    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full = g_build_filename(base_dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR) && !g_file_test(full, G_FILE_TEST_IS_SYMLINK)) {
            find_files_recursive(full, ext, results);
            g_free(full);
        } else {
            if (g_str_has_suffix(name, ext)) {
                g_ptr_array_add(results, full);
            } else {
                g_free(full);
            }
        }
    }
    g_dir_close(dir);
}

static gboolean copy_file_simple(const char *src, const char *dst) {
    GFile *f_src = g_file_new_for_path(src);
    GFile *f_dst = g_file_new_for_path(dst);
    gboolean ok = g_file_copy(f_src, f_dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL);
    g_object_unref(f_src);
    g_object_unref(f_dst);
    return ok;
}

static gboolean remove_dir_recursive(const char *path) {
    if (!path || strlen(path) < 10) return FALSE;
    char *quoted = g_shell_quote(path);
    char *cmd = g_strdup_printf("rm -rf %s", quoted);
    int code = 0;
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);
    g_free(quoted);
    return (code == 0);
}

static void refresh_desktop_databases(void) {
    char *apps_dir = get_applications_dir();
    char *icons_base = g_build_filename(g_get_home_dir(), ".local", "share", "icons", "hicolor", NULL);
    char *q_apps = g_shell_quote(apps_dir);
    char *q_icons = g_shell_quote(icons_base);

    char *cmd = g_strdup_printf(
        "update-desktop-database %s >/dev/null 2>&1 || true; "
        "gtk-update-icon-cache -f -t %s >/dev/null 2>&1 || true",
        q_apps, q_icons);
    int code = 0;
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);
    g_free(q_apps);
    g_free(q_icons);
    g_free(apps_dir);
    g_free(icons_base);
}

// ----------------------------------------------------------------------------
// Format-Specific Extraction & Metadata Parsers (Distro-Agnostic)
// ----------------------------------------------------------------------------

// Parse Debian control or Arch .PKGINFO or Alpine .PKGINFO key-value files
static void parse_colon_or_eq_kv(const char *content, const char *key, char **out_val) {
    if (!content || !key || *out_val) return;
    char **lines = g_strsplit(content, "\n", -1);
    size_t klen = strlen(key);
    for (int i = 0; lines[i]; i++) {
        char *line = lines[i];
        if (g_ascii_strncasecmp(line, key, klen) == 0) {
            const char *p = line + klen;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == ':' || *p == '=') {
                p++;
                while (*p == ' ' || *p == '\t') p++;
                *out_val = g_strdup(p);
                g_strstrip(*out_val);
                break;
            }
        }
    }
    g_strfreev(lines);
}

// 1. Extract .RPM on any distro (works with rpm2cpio, bsdtar, or 7z)
static gboolean extract_rpm_payload(const char *pkg_path, const char *root_stage,
                                    char **pkg_name, char **pkg_version, char **pkg_summary) {
    char *q_pkg = g_shell_quote(pkg_path);
    char *q_stage = g_shell_quote(root_stage);
    int code = 0;

    // Query RPM header if `rpm` is present
    char *meta_cmd = g_strdup_printf(
        "rpm -qp --queryformat '%%{NAME}\\n%%{VERSION}\\n%%{SUMMARY}' %s 2>/dev/null",
        q_pkg);
    char *meta_out = run_cmd_capture(meta_cmd, &code);
    g_free(meta_cmd);
    if (meta_out && *meta_out) {
        char **lines = g_strsplit(meta_out, "\n", 3);
        if (lines[0] && *lines[0]) *pkg_name = g_strdup(lines[0]);
        if (lines[0] && lines[1] && *lines[1]) *pkg_version = g_strdup(lines[1]);
        if (lines[0] && lines[1] && lines[2] && *lines[2]) *pkg_summary = g_strdup(lines[2]);
        g_strfreev(lines);
    }
    g_free(meta_out);

    // Extract via rpm2cpio | cpio, or fallback to bsdtar / 7z on Debian/Ubuntu/Arch
    char *cmd = g_strdup_printf(
        "cd %s && ("
        "(command -v rpm2cpio >/dev/null 2>&1 && rpm2cpio %s | cpio -idm --quiet 2>/dev/null) || "
        "(command -v bsdtar >/dev/null 2>&1 && bsdtar -xf %s 2>/dev/null) || "
        "(command -v 7z >/dev/null 2>&1 && 7z x -so %s 2>/dev/null | cpio -idm --quiet 2>/dev/null)"
        ")",
        q_stage, q_pkg, q_pkg, q_pkg);
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);
    g_free(q_pkg);
    g_free(q_stage);
    return (code == 0);
}

// 2. Extract .DEB on ANY distro (Fedora, Arch, Ubuntu) using pure `ar` + `tar` (no dpkg needed!)
static gboolean extract_deb_payload(const char *pkg_path, const char *stage_dir, const char *root_stage,
                                    char **pkg_name, char **pkg_version, char **pkg_summary) {
    char *deb_tmp = g_build_filename(stage_dir, "deb_ar", NULL);
    g_mkdir_with_parents(deb_tmp, 0755);

    char *q_pkg = g_shell_quote(pkg_path);
    char *q_debtmp = g_shell_quote(deb_tmp);
    char *q_root = g_shell_quote(root_stage);
    int code = 0;

    // Unpack outer ar archive and extract data.tar.* into root_stage + control.tar.* for metadata
    char *cmd = g_strdup_printf(
        "cd %s && ar x %s 2>/dev/null && "
        "for f in data.tar.*; do "
        "  case \"$f\" in "
        "    *.zst) tar --zstd -xf \"$f\" -C %s 2>/dev/null || zstd -dc \"$f\" | tar -xf - -C %s ;; "
        "    *.xz)  tar -xJf \"$f\" -C %s 2>/dev/null ;; "
        "    *.gz)  tar -xzf \"$f\" -C %s 2>/dev/null ;; "
        "    *.bz2) tar -xjf \"$f\" -C %s 2>/dev/null ;; "
        "    *)     tar -xf \"$f\" -C %s 2>/dev/null ;; "
        "  esac; "
        "done && "
        "for c in control.tar.*; do "
        "  case \"$c\" in "
        "    *.zst) tar --zstd -xf \"$c\" 2>/dev/null || zstd -dc \"$c\" | tar -xf - ;; "
        "    *)     tar -xf \"$c\" 2>/dev/null ;; "
        "  esac; "
        "done",
        q_debtmp, q_pkg, q_root, q_root, q_root, q_root, q_root, q_root);
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);

    // Parse Debian `control` file if extracted
    char *ctrl_file = g_build_filename(deb_tmp, "control", NULL);
    char *ctrl_text = NULL;
    if (g_file_get_contents(ctrl_file, &ctrl_text, NULL, NULL) && ctrl_text) {
        parse_colon_or_eq_kv(ctrl_text, "Package", pkg_name);
        parse_colon_or_eq_kv(ctrl_text, "Version", pkg_version);
        parse_colon_or_eq_kv(ctrl_text, "Description", pkg_summary);
        g_free(ctrl_text);
    }
    g_free(ctrl_file);
    remove_dir_recursive(deb_tmp);
    g_free(deb_tmp);
    g_free(q_pkg);
    g_free(q_debtmp);
    g_free(q_root);
    return (code == 0);
}

// 3. Extract .AppImage on any distro (preserves raw AppImage binary + extracts icon & .desktop)
static gboolean extract_appimage_payload(const char *pkg_path, const char *stage_dir, const char *root_stage,
                                         char **out_appimage_bin) {
    g_chmod(pkg_path, 0755);
    char *usr_bin = g_build_filename(root_stage, "usr", "bin", NULL);
    g_mkdir_with_parents(usr_bin, 0755);

    char *slug = derive_clean_name_from_filename(pkg_path);
    char *dest_ai = g_build_filename(usr_bin, g_strdup_printf("%s.AppImage", slug), NULL);
    if (!copy_file_simple(pkg_path, dest_ai)) {
        g_free(slug);
        g_free(dest_ai);
        g_free(usr_bin);
        return FALSE;
    }
    g_chmod(dest_ai, 0755);
    *out_appimage_bin = g_strdup_printf("usr/bin/%s.AppImage", slug);

    // Extract desktop & icon files from AppImage SquashFS
    char *sq_tmp = g_build_filename(stage_dir, "sq_meta", NULL);
    g_mkdir_with_parents(sq_tmp, 0755);
    char *q_ai = g_shell_quote(dest_ai);
    char *q_sq = g_shell_quote(sq_tmp);

    char *cmd = g_strdup_printf(
        "cd %s && ("
        "OFFSET=$(%s --appimage-offset 2>/dev/null || grep -b -a -o 'hsqs' %s 2>/dev/null | head -n1 | cut -d: -f1); "
        "if [ -n \"$OFFSET\" ] && command -v unsquashfs >/dev/null 2>&1; then "
        "  unsquashfs -f -d %s -o \"$OFFSET\" %s '*.desktop' '*.png' '*.svg' '.DirIcon' 'usr/share/applications/*' 'usr/share/icons/*' >/dev/null 2>&1 || true; "
        "else "
        "  %s --appimage-extract '*.desktop' >/dev/null 2>&1 || true; "
        "  %s --appimage-extract '*.png' >/dev/null 2>&1 || true; "
        "  %s --appimage-extract '*.svg' >/dev/null 2>&1 || true; "
        "  if [ -d squashfs-root ]; then cp -rn squashfs-root/* %s/ 2>/dev/null || true; rm -rf squashfs-root; fi; "
        "fi"
        ")",
        q_sq, q_ai, q_ai, q_sq, q_ai, q_ai, q_ai, q_ai, q_sq);
    int code = 0;
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);

    // Move extracted metadata (.desktop / icons) into root_stage/usr/share
    char *usr_share_apps = g_build_filename(root_stage, "usr", "share", "applications", NULL);
    char *usr_share_pix = g_build_filename(root_stage, "usr", "share", "pixmaps", NULL);
    g_mkdir_with_parents(usr_share_apps, 0755);
    g_mkdir_with_parents(usr_share_pix, 0755);

    GPtrArray *dts = g_ptr_array_new_with_free_func(g_free);
    find_files_recursive(sq_tmp, ".desktop", dts);
    for (guint i = 0; i < dts->len; i++) {
        const char *src = (const char *)g_ptr_array_index(dts, i);
        char *bn = g_path_get_basename(src);
        char *dst = g_build_filename(usr_share_apps, bn, NULL);
        copy_file_simple(src, dst);
        g_free(bn);
        g_free(dst);
    }
    g_ptr_array_free(dts, TRUE);

    GPtrArray *imgs = g_ptr_array_new_with_free_func(g_free);
    find_files_recursive(sq_tmp, ".png", imgs);
    find_files_recursive(sq_tmp, ".svg", imgs);
    for (guint i = 0; i < imgs->len; i++) {
        const char *src = (const char *)g_ptr_array_index(imgs, i);
        char *bn = g_path_get_basename(src);
        char *dst = g_build_filename(usr_share_pix, bn, NULL);
        copy_file_simple(src, dst);
        g_free(bn);
        g_free(dst);
    }
    g_ptr_array_free(imgs, TRUE);

    remove_dir_recursive(sq_tmp);
    g_free(usr_share_apps);
    g_free(usr_share_pix);
    g_free(sq_tmp);
    g_free(q_ai);
    g_free(q_sq);
    g_free(slug);
    g_free(dest_ai);
    g_free(usr_bin);
    return TRUE;
}

// 4. Extract Arch (.pkg.tar.zst/.xz), Alpine (.apk), or Generic Archive (.tar.*, .zip)
static gboolean extract_tar_or_zip_payload(const char *pkg_path, const char *root_stage,
                                           char **pkg_name, char **pkg_version, char **pkg_summary) {
    char *q_pkg = g_shell_quote(pkg_path);
    char *q_root = g_shell_quote(root_stage);
    int code = 0;

    char *lower = g_ascii_strdown(pkg_path, -1);
    char *cmd = NULL;
    if (g_str_has_suffix(lower, ".zip")) {
        cmd = g_strdup_printf("unzip -q -o %s -d %s 2>/dev/null", q_pkg, q_root);
    } else if (g_str_has_suffix(lower, ".zst")) {
        cmd = g_strdup_printf(
            "tar --zstd -xf %s -C %s 2>/dev/null || (zstd -dc %s 2>/dev/null | tar -xf - -C %s 2>/dev/null)",
            q_pkg, q_root, q_pkg, q_root);
    } else {
        cmd = g_strdup_printf("tar -xf %s -C %s 2>/dev/null", q_pkg, q_root);
    }
    g_free(lower);

    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);
    g_free(q_pkg);
    g_free(q_root);

    // Check for Arch / Alpine .PKGINFO file
    char *pkginfo_path = g_build_filename(root_stage, ".PKGINFO", NULL);
    char *pkginfo_text = NULL;
    if (g_file_get_contents(pkginfo_path, &pkginfo_text, NULL, NULL) && pkginfo_text) {
        parse_colon_or_eq_kv(pkginfo_text, "pkgname", pkg_name);
        parse_colon_or_eq_kv(pkginfo_text, "pkgver", pkg_version);
        parse_colon_or_eq_kv(pkginfo_text, "pkgdesc", pkg_summary);
        g_free(pkginfo_text);
    }
    g_free(pkginfo_path);
    return (code == 0);
}

// ----------------------------------------------------------------------------
// Universal Installation Engine (Supports all 7 Linux package formats)
// ----------------------------------------------------------------------------

static gboolean perform_universal_install(
    const char *pkg_path,
    char **out_app_id,
    char **out_name,
    char **out_version,
    char **out_format_badge,
    char **out_icon_path,
    gboolean *out_is_update,
    char **out_error
) {
    if (!g_file_test(pkg_path, G_FILE_TEST_IS_REGULAR)) {
        *out_error = g_strdup_printf("File not found: %s", pkg_path);
        return FALSE;
    }

    PackageFormat fmt = detect_package_format(pkg_path);
    if (fmt == PKG_FMT_UNKNOWN) {
        *out_error = g_strdup("Unsupported file format. Supported: .rpm, .deb, .AppImage, .pkg.tar.zst, .apk, .flatpak, .tar.*, .zip");
        return FALSE;
    }

    const char *badge = format_to_badge(fmt);
    if (out_format_badge) *out_format_badge = g_strdup(badge);

    // Special Case: Single-file .flatpak bundle -> install via `flatpak install --user -y --bundle`
    if (fmt == PKG_FMT_FLATPAK) {
        char *q_pkg = g_shell_quote(pkg_path);
        char *cmd = g_strdup_printf("flatpak install --user -y --or-update --bundle %s 2>&1", q_pkg);
        int code = 0;
        char *out = run_cmd_capture(cmd, &code);
        g_free(cmd);
        g_free(q_pkg);
        if (code != 0) {
            *out_error = g_strdup_printf("Flatpak bundle install failed: %s", out ? out : "unknown error");
            g_free(out);
            return FALSE;
        }
        g_free(out);
        char *slug = derive_clean_name_from_filename(pkg_path);
        *out_app_id = g_strdup(slug);
        *out_name = g_strdup(slug);
        if ((*out_name)[0]) (*out_name)[0] = toupper((unsigned char)(*out_name)[0]);
        *out_version = g_strdup("latest");
        *out_icon_path = NULL;
        if (out_is_update) *out_is_update = FALSE;
        g_free(slug);
        return TRUE;
    }

    // Standard Universal User-Space Extraction Pipeline
    char *uapps_base = get_uapps_base();
    g_mkdir_with_parents(uapps_base, 0755);

    char *stage_dir = g_build_filename(uapps_base, g_strdup_printf(".stage-%d-%u", (int)getpid(), g_random_int()), NULL);
    char *root_stage = g_build_filename(stage_dir, "root", NULL);
    g_mkdir_with_parents(root_stage, 0755);

    char *pkg_name = NULL;
    char *pkg_version = NULL;
    char *pkg_summary = NULL;
    char *explicit_exec_rel = NULL;
    gboolean extracted_ok = FALSE;

    if (fmt == PKG_FMT_RPM) {
        extracted_ok = extract_rpm_payload(pkg_path, root_stage, &pkg_name, &pkg_version, &pkg_summary);
    } else if (fmt == PKG_FMT_DEB) {
        extracted_ok = extract_deb_payload(pkg_path, stage_dir, root_stage, &pkg_name, &pkg_version, &pkg_summary);
    } else if (fmt == PKG_FMT_APPIMAGE) {
        extracted_ok = extract_appimage_payload(pkg_path, stage_dir, root_stage, &explicit_exec_rel);
    } else {
        extracted_ok = extract_tar_or_zip_payload(pkg_path, root_stage, &pkg_name, &pkg_version, &pkg_summary);
    }

    if (!extracted_ok) {
        remove_dir_recursive(stage_dir);
        *out_error = g_strdup_printf("Failed to extract %s package payload.", badge);
        g_free(uapps_base); g_free(stage_dir); g_free(root_stage);
        return FALSE;
    }

    // Locate .desktop file inside extracted payload
    GPtrArray *desktop_files = g_ptr_array_new_with_free_func(g_free);
    char *usr_share_apps = g_build_filename(root_stage, "usr", "share", "applications", NULL);
    if (g_file_test(usr_share_apps, G_FILE_TEST_IS_DIR)) {
        find_files_recursive(usr_share_apps, ".desktop", desktop_files);
    }
    g_free(usr_share_apps);
    if (desktop_files->len == 0) {
        find_files_recursive(root_stage, ".desktop", desktop_files);
    }

    char *display_name = NULL;
    char *desktop_comment = NULL;
    char *desktop_categories = NULL;
    char *desktop_mime = NULL;
    char *desktop_wmclass = NULL;
    char *desktop_exec_raw = NULL;

    if (desktop_files->len > 0) {
        const char *chosen_dt = (const char *)g_ptr_array_index(desktop_files, 0);
        for (guint i = 0; i < desktop_files->len; i++) {
            const char *cand = (const char *)g_ptr_array_index(desktop_files, i);
            if (!strstr(cand, "url-handler") && !strstr(cand, "deeplink")) {
                chosen_dt = cand;
                break;
            }
        }
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, chosen_dt, G_KEY_FILE_NONE, NULL)) {
            display_name = g_key_file_get_string(kf, "Desktop Entry", "Name", NULL);
            desktop_comment = g_key_file_get_string(kf, "Desktop Entry", "Comment", NULL);
            desktop_categories = g_key_file_get_string(kf, "Desktop Entry", "Categories", NULL);
            desktop_mime = g_key_file_get_string(kf, "Desktop Entry", "MimeType", NULL);
            desktop_wmclass = g_key_file_get_string(kf, "Desktop Entry", "StartupWMClass", NULL);
            desktop_exec_raw = g_key_file_get_string(kf, "Desktop Entry", "Exec", NULL);
            if (!pkg_version) {
                pkg_version = g_key_file_get_string(kf, "Desktop Entry", "X-AppImage-Version", NULL);
            }
        }
        g_key_file_free(kf);
    }
    g_ptr_array_free(desktop_files, TRUE);

    if (!pkg_name) {
        if (display_name && *display_name) {
            pkg_name = sanitize_slug(display_name);
        } else {
            pkg_name = derive_clean_name_from_filename(pkg_path);
        }
    }
    char *app_id = sanitize_slug(pkg_name);

    if (!display_name || !*display_name) {
        display_name = g_strdup(app_id);
        if (display_name[0]) display_name[0] = toupper((unsigned char)display_name[0]);
    }
    if (!pkg_version || !*pkg_version) {
        g_free(pkg_version);
        pkg_version = g_strdup("1.0");
    }
    if (!desktop_comment || !*desktop_comment) {
        g_free(desktop_comment);
        desktop_comment = pkg_summary ? g_strdup(pkg_summary) : g_strdup_printf("%s (%s Application)", display_name, badge);
    }
    if (!desktop_categories || !*desktop_categories) {
        g_free(desktop_categories);
        desktop_categories = g_strdup("Utility;Application;");
    }

    // Locate best application icon inside extracted payload
    char *found_icon_src = NULL;
    const char *icon_candidates[] = {
        "usr/share/pixmaps",
        "usr/share/icons/hicolor/512x512/apps",
        "usr/share/icons/hicolor/256x256/apps",
        "usr/share/icons/hicolor/128x128/apps",
        "usr/share/icons/hicolor/scalable/apps",
        NULL
    };
    for (int i = 0; icon_candidates[i] && !found_icon_src; i++) {
        char *idir = g_build_filename(root_stage, icon_candidates[i], NULL);
        if (g_file_test(idir, G_FILE_TEST_IS_DIR)) {
            GPtrArray *imgs = g_ptr_array_new_with_free_func(g_free);
            find_files_recursive(idir, ".png", imgs);
            find_files_recursive(idir, ".svg", imgs);
            if (imgs->len > 0) {
                found_icon_src = g_strdup((const char *)g_ptr_array_index(imgs, 0));
            }
            g_ptr_array_free(imgs, TRUE);
        }
        g_free(idir);
    }
    if (!found_icon_src) {
        GPtrArray *all_imgs = g_ptr_array_new_with_free_func(g_free);
        find_files_recursive(root_stage, ".png", all_imgs);
        find_files_recursive(root_stage, ".svg", all_imgs);
        for (guint i = 0; i < all_imgs->len; i++) {
            const char *p = (const char *)g_ptr_array_index(all_imgs, i);
            char *bn = g_path_get_basename(p);
            if (strstr(bn, app_id) || strstr(bn, "icon") || strstr(bn, "logo")) {
                found_icon_src = g_strdup(p);
                g_free(bn);
                break;
            }
            g_free(bn);
        }
        if (!found_icon_src && all_imgs->len > 0) {
            found_icon_src = g_strdup((const char *)g_ptr_array_index(all_imgs, 0));
        }
        g_ptr_array_free(all_imgs, TRUE);
    }

    const char *icon_ext = ".png";
    if (found_icon_src && g_str_has_suffix(found_icon_src, ".svg")) {
        icon_ext = ".svg";
    }
    char *staged_icon_path = g_build_filename(stage_dir, g_strdup_printf("icon%s", icon_ext), NULL);
    if (found_icon_src) {
        copy_file_simple(found_icon_src, staged_icon_path);
        g_free(found_icon_src);
    }
    g_free(staged_icon_path);

    // Determine main executable inside extracted payload
    char *rel_exec_target = explicit_exec_rel ? g_strdup(explicit_exec_rel) : NULL;

    if (!rel_exec_target) {
        const char *bin_dirs[] = {"usr/bin", "usr/games", "bin", NULL};
        for (int b = 0; bin_dirs[b] && !rel_exec_target; b++) {
            char *bdir = g_build_filename(root_stage, bin_dirs[b], NULL);
            if (g_file_test(bdir, G_FILE_TEST_IS_DIR)) {
                GDir *gd = g_dir_open(bdir, 0, NULL);
                if (gd) {
                    const char *bname;
                    while ((bname = g_dir_read_name(gd)) != NULL) {
                        if (g_ascii_strcasecmp(bname, app_id) == 0 || !rel_exec_target) {
                            g_free(rel_exec_target);
                            rel_exec_target = g_strdup_printf("%s/%s", bin_dirs[b], bname);
                            if (g_ascii_strcasecmp(bname, app_id) == 0) break;
                        }
                    }
                    g_dir_close(gd);
                }
            }
            g_free(bdir);
        }
    }

    // Check Exec= from .desktop file (handles /opt/<Vendor>/<App> common in .deb & .rpm)
    if (!rel_exec_target && desktop_exec_raw) {
        char **tokens = g_strsplit(desktop_exec_raw, " ", 2);
        if (tokens[0]) {
            const char *raw_bin = tokens[0];
            while (*raw_bin == '/' || *raw_bin == '"') raw_bin++;
            char *test_p = g_build_filename(root_stage, raw_bin, NULL);
            if (g_file_test(test_p, G_FILE_TEST_EXISTS)) {
                rel_exec_target = g_strdup(raw_bin);
            }
            g_free(test_p);
        }
        g_strfreev(tokens);
    }

    // Fallback for portable .tar.gz / .zip archives: scan top-level or 1-level deep for executable
    if (!rel_exec_target) {
        char *q_root = g_shell_quote(root_stage);
        char *find_cmd = g_strdup_printf(
            "cd %s && find . -maxdepth 3 -type f -executable "
            "! -name '*.so*' ! -name '*.sh' ! -name 'chrome-sandbox' ! -name '*crashpad*' "
            "2>/dev/null | head -n 1 | sed 's|^\\./||'",
            q_root);
        int code = 0;
        char *found_exec = run_cmd_capture(find_cmd, &code);
        g_free(find_cmd);
        g_free(q_root);
        if (found_exec && *found_exec) {
            rel_exec_target = found_exec;
        } else {
            g_free(found_exec);
            rel_exec_target = g_strdup_printf("usr/bin/%s", app_id);
        }
    }

    // Atomic swap into ~/.local/opt/uapps/<app_id> (Also check legacy ~/.local/opt/rpm-apps/<app_id>)
    char *app_dir = g_build_filename(uapps_base, app_id, NULL);
    char *legacy_base = get_legacy_rpm_base();
    char *legacy_dir = g_build_filename(legacy_base, app_id, NULL);

    gboolean is_update = g_file_test(app_dir, G_FILE_TEST_IS_DIR) || g_file_test(legacy_dir, G_FILE_TEST_IS_DIR);
    if (out_is_update) *out_is_update = is_update;

    if (g_file_test(legacy_dir, G_FILE_TEST_IS_DIR)) {
        remove_dir_recursive(legacy_dir);
    }
    if (g_file_test(app_dir, G_FILE_TEST_IS_DIR)) {
        remove_dir_recursive(app_dir);
    }
    g_free(legacy_dir);
    g_free(legacy_base);

    if (g_rename(stage_dir, app_dir) != 0) {
        remove_dir_recursive(stage_dir);
        *out_error = g_strdup("Failed to move staged package into ~/.local/opt/uapps.");
        g_free(uapps_base); g_free(app_dir); g_free(stage_dir); g_free(root_stage);
        return FALSE;
    }

    gint64 total_bytes = compute_dir_size_bytes(app_dir);

    // Generate Universal Distro-Agnostic Launcher Wrapper (handles Debian multiarch, Fedora lib64, opt/, AppImage)
    char *launcher_path = g_build_filename(app_dir, "launcher.sh", NULL);
    char *final_exec_in_root = g_build_filename(app_dir, "root", rel_exec_target, NULL);

    char *launcher_content = g_strdup_printf(
        "#!/usr/bin/env bash\n"
        "# Auto-generated by Astro Universal App Installer for %s (%s • v%s)\n"
        "set -euo pipefail\n"
        "APP_ROOT=\"%s/root\"\n"
        "export PATH=\"$APP_ROOT/usr/bin:$APP_ROOT/bin:$APP_ROOT/usr/games:$PATH\"\n"
        "export LD_LIBRARY_PATH=\"$APP_ROOT/usr/lib64:$APP_ROOT/usr/lib:$APP_ROOT/usr/lib/x86_64-linux-gnu:$APP_ROOT/lib/x86_64-linux-gnu:$APP_ROOT/usr/lib/%s:$APP_ROOT/opt/%s:${LD_LIBRARY_PATH:-}\"\n"
        "export XDG_DATA_DIRS=\"$APP_ROOT/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}\"\n"
        "TARGET=\"%s\"\n"
        "RESOLVED=\"$(readlink -f \"$TARGET\" 2>/dev/null || echo \"$TARGET\")\"\n"
        "exec \"$RESOLVED\" \"$@\"\n",
        display_name, badge, pkg_version, app_dir, app_id, app_id, final_exec_in_root
    );
    g_file_set_contents(launcher_path, launcher_content, -1, NULL);
    g_chmod(launcher_path, 0755);
    g_free(launcher_content);
    g_free(final_exec_in_root);

    // Symlink CLI command in ~/.local/bin/<app_id>
    char *bin_dir = get_bin_dir();
    g_mkdir_with_parents(bin_dir, 0755);
    char *cli_link = g_build_filename(bin_dir, app_id, NULL);
    unlink(cli_link);
    if (symlink(launcher_path, cli_link) != 0) {
        // non-fatal
    }
    g_free(cli_link);
    g_free(bin_dir);

    // Install icon in ~/.local/share/icons/hicolor/512x512/apps/
    char *final_icon_in_app = g_build_filename(app_dir, g_strdup_printf("icon%s", icon_ext), NULL);
    char *icons_dir = get_icons_dir();
    g_mkdir_with_parents(icons_dir, 0755);
    char *theme_icon_path = g_build_filename(icons_dir, g_strdup_printf("astro-uapp-%s%s", app_id, icon_ext), NULL);
    const char *desktop_icon_val = "application-x-executable";
    if (g_file_test(final_icon_in_app, G_FILE_TEST_IS_REGULAR)) {
        copy_file_simple(final_icon_in_app, theme_icon_path);
        desktop_icon_val = final_icon_in_app;
    }
    g_free(theme_icon_path);
    g_free(icons_dir);

    // Remove legacy astro-rpm-<app_id>.desktop if upgrading from rpm-app
    char *apps_dir = get_applications_dir();
    g_mkdir_with_parents(apps_dir, 0755);
    char *legacy_dt = g_build_filename(apps_dir, g_strdup_printf("astro-rpm-%s.desktop", app_id), NULL);
    g_unlink(legacy_dt);
    g_free(legacy_dt);

    // Write Desktop Entry in ~/.local/share/applications/astro-uapp-<app_id>.desktop
    char *desktop_filename = g_strdup_printf("astro-uapp-%s.desktop", app_id);
    char *desktop_dest = g_build_filename(apps_dir, desktop_filename, NULL);

    GString *dt = g_string_new("[Desktop Entry]\n");
    g_string_append(dt, "Version=1.0\n");
    g_string_append(dt, "Type=Application\n");
    g_string_append_printf(dt, "Name=%s\n", display_name);
    g_string_append_printf(dt, "Comment=%s\n", desktop_comment);
    g_string_append_printf(dt, "Exec=%s %%U\n", launcher_path);
    g_string_append_printf(dt, "Icon=%s\n", desktop_icon_val);
    g_string_append(dt, "Terminal=false\n");
    g_string_append(dt, "StartupNotify=true\n");
    g_string_append_printf(dt, "Categories=%s\n", desktop_categories);
    if (desktop_wmclass && *desktop_wmclass) {
        g_string_append_printf(dt, "StartupWMClass=%s\n", desktop_wmclass);
    } else {
        g_string_append_printf(dt, "StartupWMClass=%s\n", display_name);
    }
    if (desktop_mime && *desktop_mime) {
        g_string_append_printf(dt, "MimeType=%s\n", desktop_mime);
    }
    g_string_append_printf(dt, "X-Astro-UApp-Id=%s\n", app_id);
    g_string_append_printf(dt, "X-Astro-UApp-Format=%s\n", badge);
    g_string_append_printf(dt, "X-Astro-UApp-Version=%s\n", pkg_version);

    g_file_set_contents(desktop_dest, dt->str, -1, NULL);
    g_chmod(desktop_dest, 0755);
    g_string_free(dt, TRUE);

    // Save metadata.ini
    char *meta_ini_path = g_build_filename(app_dir, "metadata.ini", NULL);
    GDateTime *now = g_date_time_new_now_local();
    char *date_str = g_date_time_format(now, "%Y-%m-%d %H:%M");
    g_date_time_unref(now);

    GKeyFile *mkf = g_key_file_new();
    g_key_file_set_string(mkf, "App", "AppId", app_id);
    g_key_file_set_string(mkf, "App", "Name", display_name);
    g_key_file_set_string(mkf, "App", "Version", pkg_version);
    g_key_file_set_string(mkf, "App", "Format", badge);
    g_key_file_set_string(mkf, "App", "Summary", desktop_comment);
    g_key_file_set_string(mkf, "App", "IconPath", final_icon_in_app);
    g_key_file_set_string(mkf, "App", "ExecPath", launcher_path);
    g_key_file_set_string(mkf, "App", "DesktopFile", desktop_dest);
    g_key_file_set_string(mkf, "App", "InstallDate", date_str);
    g_key_file_set_int64(mkf, "App", "SizeBytes", total_bytes);
    g_key_file_save_to_file(mkf, meta_ini_path, NULL);
    g_key_file_free(mkf);

    refresh_desktop_databases();

    *out_app_id = g_strdup(app_id);
    *out_name = g_strdup(display_name);
    *out_version = g_strdup(pkg_version);
    *out_icon_path = g_strdup(final_icon_in_app);

    g_free(date_str);
    g_free(meta_ini_path);
    g_free(desktop_dest);
    g_free(desktop_filename);
    g_free(apps_dir);
    g_free(final_icon_in_app);
    g_free(launcher_path);
    g_free(rel_exec_target);
    g_free(explicit_exec_rel);
    g_free(display_name);
    g_free(desktop_comment);
    g_free(desktop_categories);
    g_free(desktop_mime);
    g_free(desktop_wmclass);
    g_free(desktop_exec_raw);
    g_free(uapps_base);
    g_free(app_dir);
    g_free(stage_dir);
    g_free(root_stage);
    g_free(pkg_name);
    g_free(pkg_version);
    g_free(pkg_summary);
    g_free(app_id);

    return TRUE;
}

// ----------------------------------------------------------------------------
// Dual-Mode Uninstall Engine (Keep User Data vs. Complete Purge)
// ----------------------------------------------------------------------------

static void purge_matching_subdirs(const char *parent_dir, const char *app_id, const char *app_name) {
    if (!parent_dir || !g_file_test(parent_dir, G_FILE_TEST_IS_DIR)) return;
    GDir *dir = g_dir_open(parent_dir, 0, NULL);
    if (!dir) return;

    char *id_lower = g_ascii_strdown(app_id ? app_id : "", -1);
    char *name_lower = g_ascii_strdown(app_name ? app_name : "", -1);
    GString *name_compact = g_string_new("");
    for (const char *p = name_lower; *p; p++) {
        if (!isspace((unsigned char)*p)) g_string_append_c(name_compact, *p);
    }

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (entry[0] == '.') continue;
        char *entry_lower = g_ascii_strdown(entry, -1);

        gboolean match = FALSE;
        if (*id_lower && strcmp(entry_lower, id_lower) == 0) match = TRUE;
        else if (*name_lower && strcmp(entry_lower, name_lower) == 0) match = TRUE;
        else if (name_compact->len > 2 && strcmp(entry_lower, name_compact->str) == 0) match = TRUE;
        else if (*id_lower && strlen(id_lower) >= 4) {
            char *dot_suffix = g_strdup_printf(".%s", id_lower);
            if (g_str_has_suffix(entry_lower, dot_suffix)) match = TRUE;
            g_free(dot_suffix);
        }

        if (match) {
            char *target = g_build_filename(parent_dir, entry, NULL);
            remove_dir_recursive(target);
            g_free(target);
        }
        g_free(entry_lower);
    }

    g_string_free(name_compact, TRUE);
    g_free(id_lower);
    g_free(name_lower);
    g_dir_close(dir);
}

static gboolean perform_universal_uninstall(const char *app_id, gboolean purge_data, char **out_error) {
    char *uapps_base = get_uapps_base();
    char *legacy_base = get_legacy_rpm_base();
    char *app_dir = g_build_filename(uapps_base, app_id, NULL);

    if (!g_file_test(app_dir, G_FILE_TEST_IS_DIR)) {
        g_free(app_dir);
        app_dir = g_build_filename(legacy_base, app_id, NULL);
        if (!g_file_test(app_dir, G_FILE_TEST_IS_DIR)) {
            if (out_error) *out_error = g_strdup_printf("Application '%s' is not installed.", app_id);
            g_free(uapps_base);
            g_free(legacy_base);
            g_free(app_dir);
            return FALSE;
        }
    }

    char *app_name = NULL;
    char *meta_path = g_build_filename(app_dir, "metadata.ini", NULL);
    if (g_file_test(meta_path, G_FILE_TEST_IS_REGULAR)) {
        GKeyFile *kf = g_key_file_new();
        if (g_key_file_load_from_file(kf, meta_path, G_KEY_FILE_NONE, NULL)) {
            app_name = g_key_file_get_string(kf, "App", "Name", NULL);
        }
        g_key_file_free(kf);
    }
    g_free(meta_path);
    if (!app_name) app_name = g_strdup(app_id);

    // Remove desktop entries (both astro-uapp-* and legacy astro-rpm-*)
    char *apps_dir = get_applications_dir();
    char *dt1 = g_build_filename(apps_dir, g_strdup_printf("astro-uapp-%s.desktop", app_id), NULL);
    char *dt2 = g_build_filename(apps_dir, g_strdup_printf("astro-rpm-%s.desktop", app_id), NULL);
    g_unlink(dt1);
    g_unlink(dt2);
    g_free(dt1);
    g_free(dt2);
    g_free(apps_dir);

    // Remove CLI symlink
    char *bin_dir = get_bin_dir();
    char *cli_link = g_build_filename(bin_dir, app_id, NULL);
    g_unlink(cli_link);
    g_free(cli_link);
    g_free(bin_dir);

    // Remove icons
    char *icons_dir = get_icons_dir();
    const char *ic_prefixes[] = {"astro-uapp-", "astro-rpm-", NULL};
    const char *ic_exts[] = {".png", ".svg", NULL};
    for (int p = 0; ic_prefixes[p]; p++) {
        for (int e = 0; ic_exts[e]; e++) {
            char *ic = g_build_filename(icons_dir, g_strdup_printf("%s%s%s", ic_prefixes[p], app_id, ic_exts[e]), NULL);
            g_unlink(ic);
            g_free(ic);
        }
    }
    g_free(icons_dir);

    remove_dir_recursive(app_dir);

    if (purge_data) {
        const char *home = g_get_home_dir();
        char *cfg_dir = g_build_filename(home, ".config", NULL);
        char *cache_dir = g_build_filename(home, ".cache", NULL);
        char *share_dir = g_build_filename(home, ".local", "share", NULL);
        char *state_dir = g_build_filename(home, ".local", "state", NULL);

        purge_matching_subdirs(cfg_dir, app_id, app_name);
        purge_matching_subdirs(cache_dir, app_id, app_name);
        purge_matching_subdirs(share_dir, app_id, app_name);
        purge_matching_subdirs(state_dir, app_id, app_name);

        if (app_id && strlen(app_id) >= 3) {
            char *dot_dir = g_build_filename(home, g_strdup_printf(".%s", app_id), NULL);
            if (g_file_test(dot_dir, G_FILE_TEST_IS_DIR)) {
                remove_dir_recursive(dot_dir);
            }
            g_free(dot_dir);
        }

        g_free(cfg_dir);
        g_free(cache_dir);
        g_free(share_dir);
        g_free(state_dir);
    }

    refresh_desktop_databases();

    g_free(app_name);
    g_free(uapps_base);
    g_free(legacy_base);
    g_free(app_dir);
    return TRUE;
}

static void scan_apps_in_dir(const char *base_dir, const char *default_badge, GPtrArray *list) {
    if (!base_dir || !g_file_test(base_dir, G_FILE_TEST_IS_DIR)) return;
    GDir *dir = g_dir_open(base_dir, 0, NULL);
    if (!dir) return;

    const char *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        if (entry[0] == '.') continue;
        char *app_dir = g_build_filename(base_dir, entry, NULL);
        char *meta_path = g_build_filename(app_dir, "metadata.ini", NULL);
        if (g_file_test(meta_path, G_FILE_TEST_IS_REGULAR)) {
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, meta_path, G_KEY_FILE_NONE, NULL)) {
                InstalledApp *info = g_new0(InstalledApp, 1);
                info->app_id = g_key_file_get_string(kf, "App", "AppId", NULL);
                info->name = g_key_file_get_string(kf, "App", "Name", NULL);
                info->version = g_key_file_get_string(kf, "App", "Version", NULL);
                info->format_badge = g_key_file_get_string(kf, "App", "Format", NULL);
                if (!info->format_badge) info->format_badge = g_strdup(default_badge);
                info->summary = g_key_file_get_string(kf, "App", "Summary", NULL);
                info->icon_path = g_key_file_get_string(kf, "App", "IconPath", NULL);
                info->exec_path = g_key_file_get_string(kf, "App", "ExecPath", NULL);
                info->desktop_file = g_key_file_get_string(kf, "App", "DesktopFile", NULL);
                info->install_date = g_key_file_get_string(kf, "App", "InstallDate", NULL);
                info->size_bytes = g_key_file_get_int64(kf, "App", "SizeBytes", NULL);
                info->opt_dir = g_strdup(app_dir);
                g_ptr_array_add(list, info);
            }
            g_key_file_free(kf);
        }
        g_free(meta_path);
        g_free(app_dir);
    }
    g_dir_close(dir);
}

static GPtrArray *load_installed_apps(void) {
    GPtrArray *list = g_ptr_array_new_with_free_func((GDestroyNotify)free_installed_app);
    char *uapps_base = get_uapps_base();
    char *legacy_base = get_legacy_rpm_base();
    scan_apps_in_dir(uapps_base, "Universal", list);
    scan_apps_in_dir(legacy_base, "RPM", list);
    g_free(uapps_base);
    g_free(legacy_base);
    return list;
}

// ----------------------------------------------------------------------------
// GUI Implementation (macOS Drag & Drop Style with GTK4 + Libadwaita)
// ----------------------------------------------------------------------------

static void refresh_installed_list(AppUI *ui);
static void start_async_install(AppUI *ui, const char *pkg_path);

static const char *CUSTOM_CSS =
    ".drop-zone {"
    "  border: 2px dashed alpha(@accent_color, 0.55);"
    "  border-radius: 22px;"
    "  background: linear-gradient(145deg, alpha(@accent_bg_color, 0.10), alpha(@card_bg_color, 0.85));"
    "  padding: 26px;"
    "  transition: all 200ms cubic-bezier(0.25, 0.46, 0.45, 0.94);"
    "}"
    ".drop-zone-active {"
    "  border: 2.5px solid @accent_color;"
    "  background: linear-gradient(145deg, alpha(@accent_bg_color, 0.28), alpha(@card_bg_color, 0.95));"
    "  box-shadow: 0 0 24px alpha(@accent_color, 0.35);"
    "}"
    ".mac-arrow {"
    "  font-size: 28px;"
    "  font-weight: 800;"
    "  color: @accent_color;"
    "}"
    ".drop-title {"
    "  font-size: 20px;"
    "  font-weight: 800;"
    "}"
    ".drop-subtitle {"
    "  font-size: 13px;"
    "  opacity: 0.80;"
    "}"
    ".fmt-pill {"
    "  background: alpha(@accent_bg_color, 0.18);"
    "  color: @accent_color;"
    "  border: 1px solid alpha(@accent_color, 0.35);"
    "  border-radius: 999px;"
    "  padding: 2px 10px;"
    "  font-weight: 700;"
    "  font-size: 11px;"
    "}"
    ".count-pill {"
    "  background: alpha(@accent_bg_color, 0.25);"
    "  color: @accent_color;"
    "  border-radius: 999px;"
    "  padding: 3px 12px;"
    "  font-weight: 700;"
    "  font-size: 12px;"
    "}"
    ".app-card-row {"
    "  padding: 6px 4px;"
    "}";

static void show_toast(AppUI *ui, const char *msg) {
    if (!ui || !ui->toast_overlay) return;
    AdwToast *toast = adw_toast_new(msg);
    adw_toast_set_timeout(toast, 4);
    adw_toast_overlay_add_toast(ui->toast_overlay, toast);
}

static gboolean on_progress_pulse(gpointer user_data) {
    AppUI *ui = (AppUI *)user_data;
    if (!ui->is_busy) return G_SOURCE_REMOVE;
    gtk_progress_bar_pulse(ui->progress_bar);
    return G_SOURCE_CONTINUE;
}

static void on_launch_app_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    const char *exec_path = (const char *)user_data;
    if (!exec_path || !*exec_path) return;
    char *quoted = g_shell_quote(exec_path);
    char *cmd = g_strdup_printf("(%s >/dev/null 2>&1 &)", quoted);
    int code = 0;
    char *out = run_cmd_capture(cmd, &code);
    g_free(out);
    g_free(cmd);
    g_free(quoted);
}

typedef struct {
    AppUI *ui;
    char *app_id;
    char *app_name;
} UninstallActionCtx;

static void on_uninstall_dialog_response(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(source_object);
    UninstallActionCtx *ctx = (UninstallActionCtx *)user_data;
    const char *choice = adw_alert_dialog_choose_finish(dialog, res);

    if (!choice || strcmp(choice, "cancel") == 0) {
        g_free(ctx->app_id);
        g_free(ctx->app_name);
        g_free(ctx);
        return;
    }

    gboolean purge_data = (strcmp(choice, "purge") == 0);
    char *err = NULL;

    if (perform_universal_uninstall(ctx->app_id, purge_data, &err)) {
        char *msg = purge_data
            ? g_strdup_printf("Completely uninstalled %s and wiped all user data & cache", ctx->app_name)
            : g_strdup_printf("Uninstalled %s (user data & settings preserved)", ctx->app_name);
        show_toast(ctx->ui, msg);
        g_free(msg);
        refresh_installed_list(ctx->ui);
    } else {
        char *msg = g_strdup_printf("Uninstall failed: %s", err ? err : "Unknown error");
        show_toast(ctx->ui, msg);
        g_free(msg);
        g_free(err);
    }

    g_free(ctx->app_id);
    g_free(ctx->app_name);
    g_free(ctx);
}

static void on_uninstall_app_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    UninstallActionCtx *btn_ctx = (UninstallActionCtx *)user_data;
    if (!btn_ctx || !btn_ctx->ui) return;

    char *heading = g_strdup_printf("Uninstall %s?", btn_ctx->app_name);
    char *body = g_strdup_printf(
        "Choose whether to keep your %s settings & login data (in ~/.config and ~/.local/share) or delete everything completely.",
        btn_ctx->app_name);

    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    g_free(heading);
    g_free(body);

    adw_alert_dialog_add_responses(
        ADW_ALERT_DIALOG(dlg),
        "cancel", "Cancel",
        "keep",   "Keep Data & Uninstall App",
        "purge",  "Uninstall Completely (Delete All Data)",
        NULL
    );

    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "keep", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "purge", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "keep");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "cancel");

    UninstallActionCtx *async_ctx = g_new0(UninstallActionCtx, 1);
    async_ctx->ui = btn_ctx->ui;
    async_ctx->app_id = g_strdup(btn_ctx->app_id);
    async_ctx->app_name = g_strdup(btn_ctx->app_name);

    adw_alert_dialog_choose(
        ADW_ALERT_DIALOG(dlg),
        GTK_WIDGET(btn_ctx->ui->window),
        NULL,
        on_uninstall_dialog_response,
        async_ctx
    );
}

static void free_gstr_closure(gpointer data, GClosure *closure) {
    (void)closure;
    g_free(data);
}

static void free_uninstall_ctx(gpointer data, GClosure *closure) {
    (void)closure;
    UninstallActionCtx *ctx = (UninstallActionCtx *)data;
    if (!ctx) return;
    g_free(ctx->app_id);
    g_free(ctx->app_name);
    g_free(ctx);
}

static void refresh_installed_list(AppUI *ui) {
    if (!ui || !ui->installed_list) return;

    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(ui->installed_list))) != NULL) {
        gtk_list_box_remove(ui->installed_list, child);
    }

    GPtrArray *apps = load_installed_apps();
    char *badge_txt = g_strdup_printf("%u Installed", apps->len);
    gtk_label_set_text(GTK_LABEL(ui->count_badge), badge_txt);
    g_free(badge_txt);

    if (apps->len == 0) {
        AdwActionRow *empty_row = ADW_ACTION_ROW(adw_action_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty_row), "No Applications Installed Yet");
        adw_action_row_set_subtitle(empty_row,
            "Drag & drop any .deb, .rpm, .AppImage, .pkg.tar.zst, .apk, .flatpak, or .tar.gz above.");
        GtkWidget *empty_icon = gtk_image_new_from_icon_name("package-x-generic-symbolic");
        gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 32);
        adw_action_row_add_prefix(empty_row, empty_icon);
        gtk_list_box_append(ui->installed_list, GTK_WIDGET(empty_row));
        g_ptr_array_free(apps, TRUE);
        return;
    }

    for (guint i = 0; i < apps->len; i++) {
        InstalledApp *app = (InstalledApp *)g_ptr_array_index(apps, i);
        AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
        gtk_widget_add_css_class(GTK_WIDGET(row), "app-card-row");

        char *title_escaped = g_markup_escape_text(app->name ? app->name : app->app_id, -1);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title_escaped);
        g_free(title_escaped);

        char *sz = format_size(app->size_bytes);
        char *sub = g_strdup_printf("[%s]  •  v%s  •  %s  •  CLI: %s",
                                    app->format_badge ? app->format_badge : "App",
                                    app->version ? app->version : "1.0",
                                    sz,
                                    app->app_id);
        char *sub_escaped = g_markup_escape_text(sub, -1);
        adw_action_row_set_subtitle(row, sub_escaped);
        g_free(sub_escaped);
        g_free(sub);
        g_free(sz);

        GtkWidget *icon_img = NULL;
        if (app->icon_path && g_file_test(app->icon_path, G_FILE_TEST_IS_REGULAR)) {
            icon_img = gtk_image_new_from_file(app->icon_path);
        } else {
            icon_img = gtk_image_new_from_icon_name("application-x-executable");
        }
        gtk_image_set_pixel_size(GTK_IMAGE(icon_img), 44);
        gtk_widget_set_margin_top(icon_img, 6);
        gtk_widget_set_margin_bottom(icon_img, 6);
        gtk_widget_set_margin_start(icon_img, 4);
        gtk_widget_set_margin_end(icon_img, 6);
        adw_action_row_add_prefix(row, icon_img);

        GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_valign(btn_box, GTK_ALIGN_CENTER);

        GtkWidget *launch_btn = gtk_button_new_with_label("Open");
        gtk_widget_add_css_class(launch_btn, "suggested-action");
        gtk_widget_add_css_class(launch_btn, "pill");
        g_signal_connect_data(launch_btn, "clicked",
                              G_CALLBACK(on_launch_app_clicked),
                              g_strdup(app->exec_path),
                              free_gstr_closure, 0);

        GtkWidget *uninstall_btn = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_tooltip_text(uninstall_btn, "Uninstall App (Keep Data or Complete Purge)");
        gtk_widget_add_css_class(uninstall_btn, "destructive-action");
        gtk_widget_add_css_class(uninstall_btn, "circular");

        UninstallActionCtx *uctx = g_new0(UninstallActionCtx, 1);
        uctx->ui = ui;
        uctx->app_id = g_strdup(app->app_id);
        uctx->app_name = g_strdup(app->name ? app->name : app->app_id);
        g_signal_connect_data(uninstall_btn, "clicked",
                              G_CALLBACK(on_uninstall_app_clicked),
                              uctx,
                              free_uninstall_ctx, 0);

        gtk_box_append(GTK_BOX(btn_box), launch_btn);
        gtk_box_append(GTK_BOX(btn_box), uninstall_btn);
        adw_action_row_add_suffix(row, btn_box);

        gtk_list_box_append(ui->installed_list, GTK_WIDGET(row));
    }

    g_ptr_array_free(apps, TRUE);
}

static void install_worker_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    InstallTaskData *td = (InstallTaskData *)task_data;

    td->success = perform_universal_install(
        td->pkg_path,
        &td->app_id,
        &td->app_name,
        &td->app_version,
        &td->format_badge,
        &td->icon_path,
        &td->is_update,
        &td->error_msg
    );
    g_task_return_boolean(task, td->success);
}

static void on_install_complete(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    (void)source_object;
    (void)res;
    InstallTaskData *td = (InstallTaskData *)user_data;
    AppUI *ui = td->ui;

    ui->is_busy = FALSE;
    if (ui->pulse_timer_id > 0) {
        g_source_remove(ui->pulse_timer_id);
        ui->pulse_timer_id = 0;
    }

    gtk_stack_set_visible_child_name(GTK_STACK(ui->status_stack), "idle");

    if (td->success) {
        char *msg = g_strdup_printf("✓ %s [%s] (%s v%s) installed to Applications!",
                                    td->app_name,
                                    td->format_badge ? td->format_badge : "App",
                                    td->is_update ? "Updated" : "Installed",
                                    td->app_version);
        show_toast(ui, msg);
        g_free(msg);
        refresh_installed_list(ui);

        char *q_title = g_shell_quote(td->app_name);
        char *q_icon = g_shell_quote(td->icon_path ? td->icon_path : "package-x-generic");
        char *ncmd = g_strdup_printf(
            "notify-send -a 'Universal App Installer' -i %s %s 'Installed and added to your Applications grid!' >/dev/null 2>&1 || true",
            q_icon, q_title);
        int code = 0;
        char *out = run_cmd_capture(ncmd, &code);
        g_free(out);
        g_free(ncmd);
        g_free(q_title);
        g_free(q_icon);
    } else {
        char *msg = g_strdup_printf("Installation failed: %s", td->error_msg ? td->error_msg : "Unknown error");
        show_toast(ui, msg);
        g_free(msg);
    }

    g_free(td->pkg_path);
    g_free(td->app_id);
    g_free(td->app_name);
    g_free(td->app_version);
    g_free(td->format_badge);
    g_free(td->icon_path);
    g_free(td->error_msg);
    g_free(td);
}

static void start_async_install(AppUI *ui, const char *pkg_path) {
    if (ui->is_busy) {
        show_toast(ui, "An installation is already in progress...");
        return;
    }

    PackageFormat fmt = detect_package_format(pkg_path);
    if (fmt == PKG_FMT_UNKNOWN) {
        show_toast(ui, "Unsupported file. Drop .deb, .rpm, .AppImage, .pkg.tar.zst, .apk, .flatpak, .tar.*, or .zip");
        return;
    }

    ui->is_busy = TRUE;
    char *bn = g_path_get_basename(pkg_path);
    char *title = g_strdup_printf("Installing %s [%s]…", bn, format_to_badge(fmt));
    gtk_label_set_text(GTK_LABEL(ui->progress_title), title);
    gtk_label_set_text(GTK_LABEL(ui->progress_subtitle),
                       "Extracting payload, configuring multi-distro runtime & registering desktop icon…");
    g_free(title);
    g_free(bn);

    gtk_stack_set_visible_child_name(GTK_STACK(ui->status_stack), "progress");
    ui->pulse_timer_id = g_timeout_add(80, on_progress_pulse, ui);

    InstallTaskData *td = g_new0(InstallTaskData, 1);
    td->ui = ui;
    td->pkg_path = g_strdup(pkg_path);

    GTask *task = g_task_new(NULL, NULL, on_install_complete, td);
    g_task_set_task_data(task, td, NULL);
    g_task_run_in_thread(task, install_worker_thread);
    g_object_unref(task);
}

// Drag & Drop Handlers
static gboolean on_drop_accept(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    AppUI *ui = (AppUI *)user_data;
    gtk_widget_remove_css_class(ui->drop_box, "drop-zone-active");

    if (G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) {
        GdkFileList *flist = g_value_get_boxed(value);
        GSList *files = gdk_file_list_get_files(flist);
        if (files && files->data) {
            GFile *gfile = G_FILE(files->data);
            char *path = g_file_get_path(gfile);
            if (path) {
                start_async_install(ui, path);
                g_free(path);
                return TRUE;
            }
        }
    } else if (G_VALUE_HOLDS(value, G_TYPE_FILE)) {
        GFile *gfile = g_value_get_object(value);
        if (gfile) {
            char *path = g_file_get_path(gfile);
            if (path) {
                start_async_install(ui, path);
                g_free(path);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static GdkDragAction on_drop_enter(GtkDropTarget *target, double x, double y, gpointer user_data) {
    (void)target; (void)x; (void)y;
    AppUI *ui = (AppUI *)user_data;
    gtk_widget_add_css_class(ui->drop_box, "drop-zone-active");
    return GDK_ACTION_COPY;
}

static void on_drop_leave(GtkDropTarget *target, gpointer user_data) {
    (void)target;
    AppUI *ui = (AppUI *)user_data;
    gtk_widget_remove_css_class(ui->drop_box, "drop-zone-active");
}

static void on_file_dialog_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    AppUI *ui = (AppUI *)user_data;
    GFile *file = gtk_file_dialog_open_finish(dialog, result, NULL);
    if (file) {
        char *path = g_file_get_path(file);
        if (path) {
            start_async_install(ui, path);
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void on_browse_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppUI *ui = (AppUI *)user_data;
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Linux Application Package (.deb, .rpm, .AppImage, .pkg.tar.zst, .flatpak, .tar.*)");

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "All Linux App Packages (.deb, .rpm, .AppImage, .pkg.tar.zst, .apk, .flatpak, .tar.*, .zip)");
    const char *patterns[] = {
        "*.rpm", "*.RPM", "*.deb", "*.DEB", "*.AppImage", "*.appimage",
        "*.pkg.tar.zst", "*.pkg.tar.xz", "*.apk", "*.flatpak",
        "*.tar.gz", "*.tgz", "*.tar.xz", "*.txz", "*.tar.bz2", "*.tar.zst", "*.zip", NULL
    };
    for (int i = 0; patterns[i]; i++) {
        gtk_file_filter_add_pattern(filter, patterns[i]);
    }

    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);

    char *dl_dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    if (g_file_test(dl_dir, G_FILE_TEST_IS_DIR)) {
        GFile *init_folder = g_file_new_for_path(dl_dir);
        gtk_file_dialog_set_initial_folder(dialog, init_folder);
        g_object_unref(init_folder);
    }
    g_free(dl_dir);

    gtk_file_dialog_open(dialog, GTK_WINDOW(ui->window), NULL, on_file_dialog_ready, ui);
    g_object_unref(dialog);
}

static char *PENDING_STARTUP_PKG = NULL;

static gboolean trigger_startup_pkg(gpointer user_data) {
    AppUI *ui = (AppUI *)user_data;
    if (PENDING_STARTUP_PKG) {
        start_async_install(ui, PENDING_STARTUP_PKG);
        g_free(PENDING_STARTUP_PKG);
        PENDING_STARTUP_PKG = NULL;
    }
    return G_SOURCE_REMOVE;
}

static void on_info_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    AppUI *ui = (AppUI *)user_data;

    AdwAboutDialog *about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(about, "Universal Linux App Installer");
    adw_about_dialog_set_application_icon(about, "com.astro.UniversalAppInstaller");
    adw_about_dialog_set_developer_name(about, "Saurav Kumar (@cyberhavik)");
    adw_about_dialog_set_version(about, "v1.0.0");
    adw_about_dialog_set_comments(about,
        "macOS-Style Drag & Drop Universal Linux Package Installer & Uninstaller (.deb, .rpm, .AppImage, .pkg.tar.zst, .apk, .flatpak, .tar.gz) — Built in Native C with GTK4 & Libadwaita.");
    adw_about_dialog_set_website(about, "https://github.com/cyberhavik/universal-linux-app-installer");
    adw_about_dialog_set_issue_url(about, "https://github.com/cyberhavik/universal-linux-app-installer/issues");
    adw_about_dialog_set_support_url(about, "https://github.com/cyberhavik");
    adw_about_dialog_set_copyright(about, "© 2026 Saurav Kumar (cyberhavik)");
    adw_about_dialog_set_license_type(about, GTK_LICENSE_MIT_X11);

    adw_about_dialog_add_link(about, "GitHub Profile (@cyberhavik)", "https://github.com/cyberhavik");
    adw_about_dialog_add_link(about, "Universal Linux App Installer Repository", "https://github.com/cyberhavik/universal-linux-app-installer");
    adw_about_dialog_add_link(about, "RPM App Installer Repository", "https://github.com/cyberhavik/rpm-app-installer");

    const char *devs[] = { "Saurav Kumar (cyberhavik) https://github.com/cyberhavik", NULL };
    adw_about_dialog_set_developers(about, devs);

    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(ui->window));
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    AppUI *ui = (AppUI *)user_data;
    ui->app = app;

    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, CUSTOM_CSS);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);

    ui->window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(ui->window), "Universal Linux App Installer");
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 700, 650);

    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());

    GtkWidget *header_open_btn = gtk_button_new_from_icon_name("document-open-symbolic");
    gtk_widget_set_tooltip_text(header_open_btn, "Browse Any Linux Package");
    g_signal_connect(header_open_btn, "clicked", G_CALLBACK(on_browse_clicked), ui);
    adw_header_bar_pack_start(header, header_open_btn);

    // Circled 'i' Info / About Developer & GitHub button
    GtkWidget *info_btn = gtk_button_new_from_icon_name("dialog-information-symbolic");
    gtk_widget_set_tooltip_text(info_btn, "About Developer & GitHub Repository (Saurav Kumar • @cyberhavik)");
    gtk_widget_add_css_class(info_btn, "circular");
    g_signal_connect(info_btn, "clicked", G_CALLBACK(on_info_clicked), ui);
    adw_header_bar_pack_end(header, info_btn);

    ui->count_badge = gtk_label_new("0 Installed");
    gtk_widget_add_css_class(ui->count_badge, "count-pill");
    adw_header_bar_pack_end(header, ui->count_badge);

    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    ui->toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    AdwClamp *clamp = ADW_CLAMP(adw_clamp_new());
    adw_clamp_set_maximum_size(clamp, 640);
    gtk_widget_set_margin_top(GTK_WIDGET(clamp), 20);
    gtk_widget_set_margin_bottom(GTK_WIDGET(clamp), 28);
    gtk_widget_set_margin_start(GTK_WIDGET(clamp), 20);
    gtk_widget_set_margin_end(GTK_WIDGET(clamp), 20);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);

    // 1. Universal macOS-Style Drag & Drop Zone
    ui->drop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(ui->drop_box, "drop-zone");

    ui->status_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui->status_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);

    ui->idle_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign(ui->idle_page, GTK_ALIGN_CENTER);

    GtkWidget *graphic_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 26);
    gtk_widget_set_halign(graphic_row, GTK_ALIGN_CENTER);

    GtkWidget *pkg_icon = gtk_image_new_from_icon_name("package-x-generic");
    gtk_image_set_pixel_size(GTK_IMAGE(pkg_icon), 64);

    GtkWidget *arrow_lbl = gtk_label_new("━━━▶");
    gtk_widget_add_css_class(arrow_lbl, "mac-arrow");

    GtkWidget *apps_icon = gtk_image_new_from_icon_name("system-software-install");
    gtk_image_set_pixel_size(GTK_IMAGE(apps_icon), 64);

    gtk_box_append(GTK_BOX(graphic_row), pkg_icon);
    gtk_box_append(GTK_BOX(graphic_row), arrow_lbl);
    gtk_box_append(GTK_BOX(graphic_row), apps_icon);

    GtkWidget *drop_title = gtk_label_new("Drag & Drop Any Linux App Here");
    gtk_widget_add_css_class(drop_title, "drop-title");

    // Supported format pills row
    GtkWidget *pills_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(pills_row, GTK_ALIGN_CENTER);
    const char *pill_labels[] = {".deb", ".rpm", ".AppImage", ".pkg.tar.zst", ".apk", ".flatpak", ".tar.gz / .zip", NULL};
    for (int i = 0; pill_labels[i]; i++) {
        GtkWidget *p = gtk_label_new(pill_labels[i]);
        gtk_widget_add_css_class(p, "fmt-pill");
        gtk_box_append(GTK_BOX(pills_row), p);
    }

    GtkWidget *drop_sub = gtk_label_new("Works across Fedora, Ubuntu, Debian, Arch & any Linux distro — zero password or manual setup");
    gtk_widget_add_css_class(drop_sub, "drop-subtitle");
    gtk_label_set_wrap(GTK_LABEL(drop_sub), TRUE);
    gtk_label_set_justify(GTK_LABEL(drop_sub), GTK_JUSTIFY_CENTER);

    GtkWidget *browse_btn = gtk_button_new_with_label("Or Choose Package File…");
    gtk_widget_add_css_class(browse_btn, "suggested-action");
    gtk_widget_add_css_class(browse_btn, "pill");
    gtk_widget_set_halign(browse_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(browse_btn, 4);
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_browse_clicked), ui);

    gtk_box_append(GTK_BOX(ui->idle_page), graphic_row);
    gtk_box_append(GTK_BOX(ui->idle_page), drop_title);
    gtk_box_append(GTK_BOX(ui->idle_page), pills_row);
    gtk_box_append(GTK_BOX(ui->idle_page), drop_sub);
    gtk_box_append(GTK_BOX(ui->idle_page), browse_btn);

    ui->progress_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_valign(ui->progress_page, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(ui->progress_page, 18);
    gtk_widget_set_margin_bottom(ui->progress_page, 18);

    ui->progress_title = gtk_label_new("Installing Application…");
    gtk_widget_add_css_class(ui->progress_title, "drop-title");

    ui->progress_subtitle = gtk_label_new("Extracting payload and registering icon…");
    gtk_widget_add_css_class(ui->progress_subtitle, "drop-subtitle");

    ui->progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_pulse_step(ui->progress_bar, 0.08);
    gtk_widget_set_margin_start(GTK_WIDGET(ui->progress_bar), 32);
    gtk_widget_set_margin_end(GTK_WIDGET(ui->progress_bar), 32);

    gtk_box_append(GTK_BOX(ui->progress_page), ui->progress_title);
    gtk_box_append(GTK_BOX(ui->progress_page), GTK_WIDGET(ui->progress_bar));
    gtk_box_append(GTK_BOX(ui->progress_page), ui->progress_subtitle);

    gtk_stack_add_named(GTK_STACK(ui->status_stack), ui->idle_page, "idle");
    gtk_stack_add_named(GTK_STACK(ui->status_stack), ui->progress_page, "progress");
    gtk_box_append(GTK_BOX(ui->drop_box), ui->status_stack);

    GtkDropTarget *drop_target = gtk_drop_target_new(G_TYPE_INVALID, GDK_ACTION_COPY);
    GType drop_types[2] = { GDK_TYPE_FILE_LIST, G_TYPE_FILE };
    gtk_drop_target_set_gtypes(drop_target, drop_types, 2);
    g_signal_connect(drop_target, "enter", G_CALLBACK(on_drop_enter), ui);
    g_signal_connect(drop_target, "leave", G_CALLBACK(on_drop_leave), ui);
    g_signal_connect(drop_target, "drop", G_CALLBACK(on_drop_accept), ui);
    gtk_widget_add_controller(ui->drop_box, GTK_EVENT_CONTROLLER(drop_target));

    gtk_box_append(GTK_BOX(main_vbox), ui->drop_box);

    // 2. Installed Applications List
    AdwPreferencesGroup *installed_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(installed_group, "Installed Applications");
    adw_preferences_group_set_description(installed_group,
        "Click Open to launch, or click the red Trash button to uninstall (Keep Data or Delete All Data).");

    ui->installed_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(ui->installed_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(ui->installed_list), "boxed-list");
    adw_preferences_group_add(installed_group, GTK_WIDGET(ui->installed_list));

    gtk_box_append(GTK_BOX(main_vbox), GTK_WIDGET(installed_group));

    adw_clamp_set_child(clamp, main_vbox);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), GTK_WIDGET(clamp));
    adw_toast_overlay_set_child(ui->toast_overlay, scrolled);
    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(ui->toast_overlay));
    adw_application_window_set_content(ui->window, GTK_WIDGET(toolbar_view));

    refresh_installed_list(ui);
    gtk_window_present(GTK_WINDOW(ui->window));

    if (PENDING_STARTUP_PKG) {
        g_idle_add(trigger_startup_pkg, ui);
    }
}

// ----------------------------------------------------------------------------
// CLI Mode + Entrypoint
// ----------------------------------------------------------------------------

static void print_cli_help(void) {
    printf("\033[1;36m╭──────────────────────────────────────────────────────────────────────╮\033[0m\n");
    printf("\033[1;36m│\033[0m  \033[1;35muapp\033[0m — Universal Linux Drag & Drop App Installer (Native C / GTK4)  \033[1;36m│\033[0m\n");
    printf("\033[1;36m╰──────────────────────────────────────────────────────────────────────╯\033[0m\n");
    printf("  \033[1;33mSupported Formats on ANY Linux Distro:\033[0m\n");
    printf("    • \033[1;32m.deb\033[0m          (Ubuntu, Debian, Mint, Pop!_OS — works even on Fedora/Arch!)\n");
    printf("    • \033[1;32m.rpm\033[0m          (Fedora, RHEL, openSUSE — works even on Ubuntu/Arch!)\n");
    printf("    • \033[1;32m.AppImage\033[0m     (Universal AppImage with desktop launcher & icon extraction)\n");
    printf("    • \033[1;32m.pkg.tar.zst\033[0m  (Arch Linux, Manjaro, CachyOS packages)\n");
    printf("    • \033[1;32m.apk\033[0m          (Alpine / Chimera Linux packages)\n");
    printf("    • \033[1;32m.flatpak\033[0m      (Single-file Flatpak bundles)\n");
    printf("    • \033[1;32m.tar.* / .zip\033[0m (Portable Linux application tarballs & archives)\n\n");
    printf("  \033[1;33mUsage:\033[0m\n");
    printf("    uapp                              Launch Universal Drag & Drop GUI\n");
    printf("    uapp <package_file>               Open GUI and auto-install package\n");
    printf("    uapp --cli <package_file>         Install package silently in terminal\n");
    printf("    uapp --list                       List all installed applications\n");
    printf("    uapp --uninstall <app_id>         Uninstall app (keep user data & login)\n");
    printf("    uapp --purge <app_id>             Uninstall completely (delete all user data)\n");
    printf("    uapp -h, --h, --help              Show this help message\n");
}

int main(int argc, char **argv) {
    if (argc >= 2) {
        const char *arg1 = argv[1];
        if (strcmp(arg1, "-h") == 0 || strcmp(arg1, "--h") == 0 ||
            strcmp(arg1, "--help") == 0 || strcmp(arg1, "help") == 0) {
            print_cli_help();
            return 0;
        }
        if (strcmp(arg1, "--list") == 0 || strcmp(arg1, "-l") == 0) {
            GPtrArray *apps = load_installed_apps();
            if (apps->len == 0) {
                printf("No applications installed yet.\n");
            } else {
                printf("\033[1;36m%-16s %-10s %-22s %-14s %s\033[0m\n", "APP ID", "FORMAT", "NAME", "VERSION", "EXEC");
                for (guint i = 0; i < apps->len; i++) {
                    InstalledApp *a = (InstalledApp *)g_ptr_array_index(apps, i);
                    printf("\033[1;32m%-16s\033[0m %-10s %-22s %-14s %s\n",
                           a->app_id ? a->app_id : "-",
                           a->format_badge ? a->format_badge : "App",
                           a->name ? a->name : "-",
                           a->version ? a->version : "-",
                           a->exec_path ? a->exec_path : "-");
                }
            }
            g_ptr_array_free(apps, TRUE);
            return 0;
        }
        if (strcmp(arg1, "--uninstall") == 0 || strcmp(arg1, "--remove") == 0 ||
            strcmp(arg1, "--purge") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: uapp %s <app_id> [--purge]\n", arg1);
                return 1;
            }
            gboolean purge = (strcmp(arg1, "--purge") == 0) ||
                             (argc >= 4 && strcmp(argv[3], "--purge") == 0);
            char *err = NULL;
            if (perform_universal_uninstall(argv[2], purge, &err)) {
                if (purge) {
                    printf("\033[1;32m[SUCCESS]\033[0m Completely uninstalled '%s' and deleted all user data & cache.\n", argv[2]);
                } else {
                    printf("\033[1;32m[SUCCESS]\033[0m Uninstalled '%s' (user data & settings preserved).\n", argv[2]);
                }
                return 0;
            } else {
                fprintf(stderr, "\033[1;31m[ERROR]\033[0m %s\n", err ? err : "Uninstall failed");
                g_free(err);
                return 1;
            }
        }
        if (strcmp(arg1, "--cli") == 0 || strcmp(arg1, "--install") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: uapp --cli <package_file>\n");
                return 1;
            }
            char *app_id = NULL, *name = NULL, *ver = NULL, *badge = NULL, *icon = NULL, *err = NULL;
            gboolean is_upd = FALSE;
            printf("\033[1;34m==>\033[0m Installing \033[1m%s\033[0m into user space (no root required)...\n", argv[2]);
            if (perform_universal_install(argv[2], &app_id, &name, &ver, &badge, &icon, &is_upd, &err)) {
                printf("\033[1;32m[SUCCESS]\033[0m %s %s [%s] (v%s) as desktop app!\n",
                       is_upd ? "Updated" : "Installed", name, badge ? badge : "App", ver);
                printf("  • App ID : %s\n  • Format : %s\n  • Icon   : %s\n  • Run    : %s\n",
                       app_id, badge ? badge : "App", icon ? icon : "default", app_id);
                g_free(app_id); g_free(name); g_free(ver); g_free(badge); g_free(icon);
                return 0;
            } else {
                fprintf(stderr, "\033[1;31m[ERROR]\033[0m %s\n", err ? err : "Install failed");
                g_free(err);
                return 1;
            }
        }

        if (g_file_test(arg1, G_FILE_TEST_IS_REGULAR)) {
            GFile *gf = g_file_new_for_commandline_arg(arg1);
            PENDING_STARTUP_PKG = g_file_get_path(gf);
            g_object_unref(gf);
        }
    }

    adw_init();
    AppUI ui = {0};
    GtkApplication *app = gtk_application_new("com.astro.UniversalAppInstaller", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &ui);

    int status = g_application_run(G_APPLICATION(app), 1, argv);
    g_object_unref(app);
    return status;
}
