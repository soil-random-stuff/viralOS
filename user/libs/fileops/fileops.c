#include <fileops.h>
#include <audiofile.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <paths.h>

#define CLIPBOARD     _PATH_TMP "/.clipboard"
#define FILES_APP     _PATH_BIN "/divine-files"
#define TEXT_EDITOR   _PATH_BIN "/divine-textedit"
#define PHOTOS_APP    _PATH_BIN "/divine-photos"
#define MUSIC_APP     _PATH_BIN "/divine-music"
#define ICON_DIR      _PATH_DDE_RES "/Icons/FilesFolders/"

/* --- paths ------------------------------------------------------------------------------ */

void path_join(char *out, size_t size, const char *dir, const char *name) {
    snprintf(out, size, strcmp(dir, "/") == 0 ? "%s%s" : "%s/%s", dir, name);
}

const char *path_base(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

void path_parent(const char *path, char *out, size_t size) {
    strlcpy(out, path, size);
    char *slash = strrchr(out, '/');
    if (slash == out) {
        out[1] = '\0';
    } else if (slash) {
        *slash = '\0';
    }
}

const char *path_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && dot != name ? dot + 1 : "";
}

bool path_inside(const char *path, const char *dir) {
    size_t n = strlen(dir);
    return strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

/* --- kinds of files ------------------------------------------------------------------------ */

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static bool same_ignoring_case(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

int name_compare(const char *a, const char *b) {
    while (*a && *b) {
        if (is_digit(*a) && is_digit(*b)) {
            /* Whole numbers: skip leading zeros, then the longer one is bigger, else digit by digit. */
            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }
            int la = 0, lb = 0;
            while (is_digit(a[la])) {
                la++;
            }
            while (is_digit(b[lb])) {
                lb++;
            }
            if (la != lb) {
                return la - lb;
            }
            for (int i = 0; i < la; i++) {
                if (a[i] != b[i]) {
                    return a[i] - b[i];
                }
            }
            a += la;
            b += lb;
            continue;
        }
        int ca = lower(*a), cb = lower(*b);
        if (ca != cb) {
            return ca - cb;
        }
        a++;
        b++;
    }
    return lower(*a) - lower(*b);
}

bool name_is_text(const char *name) {
    static const char *const text[] = { "txt", "md", "log", "conf", "cfg", "ini", "c", "h", "s", "sh",
                                        "py", "json", "xml", "csv", "html", "css", "js" };
    const char *ext = path_extension(name);
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        if (same_ignoring_case(ext, text[i])) {
            return true;
        }
    }
    return strcmp(name, "README") == 0;
}

enum file_icon file_icon_for(const struct os_file_info *info) {
    if (info->type == OS_DIRECTORY) {
        return FILE_ICON_FOLDER;
    }
    if (image_supported_name(info->name)) {
        return FILE_ICON_PICTURE;
    }
    if (audio_supported_name(info->name)) {
        return FILE_ICON_AUDIO;
    }
    return name_is_text(info->name) || same_ignoring_case(path_extension(info->name), "desktop")
               ? FILE_ICON_TEXT : FILE_ICON_FILE;
}

const char *file_icon_path(enum file_icon icon) {
    switch (icon) {
    case FILE_ICON_FOLDER: return ICON_DIR "Folder.png";
    case FILE_ICON_TEXT:   return ICON_DIR "TextFile.png";
    case FILE_ICON_PICTURE: return ICON_DIR "Picture.png";
    case FILE_ICON_AUDIO:  return ICON_DIR "Audio.png";
    case FILE_ICON_EXTERNAL_DRIVE: return ICON_DIR "ExternalStorage.png";
    case FILE_ICON_INTERNAL_DRIVE: return ICON_DIR "InternalStorage.png";
    default:               return ICON_DIR "FileGeneric.png";
    }
}

const char *file_type_name(const char *path, const struct os_file_info *info) {
    if (info->type == OS_DIRECTORY) {
        return "Folder";
    }
    const char *ext = path_extension(info->name);
    if (image_supported_name(info->name)) {
        static char name[24];
        uint8_t head[16];
        long n = 0;
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, head, sizeof(head));
            close(fd);
        }
        const char *format = n > 0 ? image_format(head, (size_t)n) : NULL;
        snprintf(name, sizeof(name), "%s image", format ? format : "Broken");
        return name;
    }
    if (audio_supported_name(info->name)) {
        static char audio[24];
        uint8_t head[16];
        long n = 0;
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            n = read(fd, head, sizeof(head));
            close(fd);
        }
        const char *format = n > 0 ? audio_format(head, (size_t)n) : NULL;
        snprintf(audio, sizeof(audio), "%s audio", format ? format : "Unknown");
        return audio;
    }
    if (same_ignoring_case(ext, "ttf")) {
        return "TrueType font";
    }
    if (same_ignoring_case(ext, "desktop")) {
        return "App launcher";
    }
    if (same_ignoring_case(ext, "tar")) {
        return "Tar archive";
    }
    char magic[4];
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        long n = read(fd, magic, 4);
        close(fd);
        if (n == 4 && memcmp(magic, "\x7f" "ELF", 4) == 0) {
            return "Program";
        }
    }
    return name_is_text(info->name) ? "Text file" : "File";
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

bool app_file_read(const char *path, char *name, size_t name_size, char *icon, size_t icon_size,
                   char *exec, size_t exec_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char text[2048];
    long len = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (len <= 0) {
        return false;
    }
    text[len] = '\0';
    if (name) {
        name[0] = '\0';
    }
    if (icon) {
        icon[0] = '\0';
    }
    if (exec) {
        exec[0] = '\0';
    }
    for (char *line = text, *next; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(line), *value = trim(eq + 1);
        size_t n = strlen(value);
        if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
            value[n - 1] = '\0';
            value++;
        }
        if (name && strcmp(key, "Name") == 0) {
            strlcpy(name, value, name_size);
        } else if (icon && strcmp(key, "Icon") == 0) {
            strlcpy(icon, value, icon_size);
        } else if (exec && strcmp(key, "Exec") == 0) {
            strlcpy(exec, value, exec_size);
        }
    }
    return true;
}

int split_command(const char *line, char *buf, size_t size, const char **argv, int max) {
    int argc = 0;
    size_t used = 0;
    const char *p = line;
    while (*p && argc < max - 1 && used < size - 1) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        argv[argc++] = buf + used;
        bool quoted = false;
        while (*p && (quoted || (*p != ' ' && *p != '\t')) && used < size - 1) {
            if (*p == '"') {
                quoted = !quoted;
            } else {
                buf[used++] = *p;
            }
            p++;
        }
        buf[used++] = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

/* --- whole trees ------------------------------------------------------------------------------- */

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    if (in < 0) {
        return in;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return out;
    }
    static char buf[64 * 1024];
    long n;
    int err = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            err = -ENOSPC;
            break;
        }
    }
    if (n < 0) {
        err = (int)n;
    }
    close(in);
    close(out);
    return err;
}

int copy_tree(const char *from, const char *to) {
    struct os_file_info info;
    int err = stat(from, &info);
    if (err < 0) {
        return err;
    }
    if (info.type != OS_DIRECTORY) {
        return copy_file(from, to);
    }
    err = mkdir(to);
    if (err < 0) {
        return err;
    }
    int fd = open(from, O_RDONLY);
    if (fd < 0) {
        return fd;
    }
    struct os_file_info child;
    for (uint64_t i = 0; err == 0 && readdir(fd, i, &child) == 0; i++) {
        char a[FILE_PATH_MAX], b[FILE_PATH_MAX];
        path_join(a, sizeof(a), from, child.name);
        path_join(b, sizeof(b), to, child.name);
        err = copy_tree(a, b);
    }
    close(fd);
    return err;
}

int delete_tree(const char *path) {
    struct os_file_info info;
    int err = stat(path, &info);
    if (err < 0) {
        return err;
    }
    if (info.type == OS_DIRECTORY) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            return fd;
        }
        /* Always the first entry: each one disappears as it's deleted. */
        struct os_file_info child;
        while (err == 0 && readdir(fd, 0, &child) == 0) {
            char p[FILE_PATH_MAX];
            path_join(p, sizeof(p), path, child.name);
            err = delete_tree(p);
        }
        close(fd);
        if (err < 0) {
            return err;
        }
    }
    return unlink(path);
}

int move_tree(const char *from, const char *to) {
    int err = rename(from, to);
    if (err == -EXDEV) {
        err = copy_tree(from, to);
        if (err == 0) {
            err = delete_tree(from);
        }
    }
    return err;
}

void measure_tree(const char *path, uint64_t *bytes, int *files, int *folders) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    struct os_file_info child;
    for (uint64_t i = 0; readdir(fd, i, &child) == 0; i++) {
        if (child.type == OS_DIRECTORY) {
            char p[FILE_PATH_MAX];
            path_join(p, sizeof(p), path, child.name);
            (*folders)++;
            measure_tree(p, bytes, files, folders);
        } else {
            (*files)++;
            *bytes += child.size;
        }
    }
    close(fd);
}

void free_name(const char *dir, const char *name, bool copy, char *out, size_t size) {
    char stem[OS_NAME_MAX], ext[OS_NAME_MAX] = "";
    strlcpy(stem, name, sizeof(stem));
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem) {
        strlcpy(ext, dot, sizeof(ext));
        *dot = '\0';
    }
    struct os_file_info info;
    for (int n = copy ? 1 : 0; n < 1000; n++) {
        char candidate[OS_NAME_MAX + 16];
        if (n == 0) {
            strlcpy(candidate, name, sizeof(candidate));
        } else if (copy && n == 1) {
            snprintf(candidate, sizeof(candidate), "%s (copy)%s", stem, ext);
        } else if (copy) {
            snprintf(candidate, sizeof(candidate), "%s (copy %d)%s", stem, n, ext);
        } else {
            snprintf(candidate, sizeof(candidate), "%s (%d)%s", stem, n + 1, ext);
        }
        path_join(out, size, dir, candidate);
        if (stat(out, &info) < 0) {
            return;
        }
    }
}

void ensure_home_folders(void) {
    make_home_dirs(user_name());
}

int move_into(char (*paths)[FILE_PATH_MAX], int count, const char *dir, char (*names)[OS_NAME_MAX],
              char *failed, size_t failed_size) {
    int moved = 0;
    for (int i = 0; i < count; i++) {
        char from_dir[FILE_PATH_MAX], to[FILE_PATH_MAX];
        path_parent(paths[i], from_dir, sizeof(from_dir));
        if (strcmp(from_dir, dir) == 0 || path_inside(dir, paths[i])) {
            continue;   /* already there, or a folder into itself */
        }
        free_name(dir, path_base(paths[i]), false, to, sizeof(to));
        int err = move_tree(paths[i], to);
        if (err < 0) {
            strlcpy(failed, path_base(paths[i]), failed_size);
            return err;
        }
        if (names) {
            strlcpy(names[moved], path_base(to), OS_NAME_MAX);
        }
        moved++;
    }
    return moved;
}

/* --- the clipboard --------------------------------------------------------------------------------- */

void clipboard_files_set(const char *const *paths, int count, bool cut) {
    int fd = open(CLIPBOARD, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        return;
    }
    const char *kind = cut ? "cut\n" : "copy\n";
    write(fd, kind, strlen(kind));
    for (int i = 0; i < count; i++) {
        write(fd, paths[i], strlen(paths[i]));
        write(fd, "\n", 1);
    }
    close(fd);
}

void clipboard_files_clear(void) {
    unlink(CLIPBOARD);
}

int clipboard_files_get(char (*paths)[FILE_PATH_MAX], int max, bool *cut) {
    int fd = open(CLIPBOARD, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    static char buf[CLIPBOARD_MAX * FILE_PATH_MAX + 16];
    long len = 0, n;
    while (len < (long)sizeof(buf) - 1 && (n = read(fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
        len += n;
    }
    close(fd);
    buf[len > 0 ? len : 0] = '\0';
    char *line = buf, *next = strchr(line, '\n');
    if (!next) {
        return 0;
    }
    *next++ = '\0';
    *cut = strcmp(line, "cut") == 0;
    int count = 0;
    for (line = next; line && *line && count < max; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        struct os_file_info info;
        if (line[0] && stat(line, &info) == 0) {   /* gone since: skip it */
            strlcpy(paths[count++], line, FILE_PATH_MAX);
        }
    }
    return count;
}

bool clipboard_files_has(const char *path, bool *cut) {
    static char paths[CLIPBOARD_MAX][FILE_PATH_MAX];
    int n = clipboard_files_get(paths, CLIPBOARD_MAX, cut);
    for (int i = 0; i < n; i++) {
        if (strcmp(paths[i], path) == 0) {
            return true;
        }
    }
    return false;
}

int clipboard_files_paste(const char *dir, char *last, size_t last_size, char *failed, size_t failed_size) {
    static char paths[CLIPBOARD_MAX][FILE_PATH_MAX];
    bool cut;
    int n = clipboard_files_get(paths, CLIPBOARD_MAX, &cut);
    int done = 0;
    for (int i = 0; i < n; i++) {
        const char *from = paths[i];
        char from_dir[FILE_PATH_MAX], to[FILE_PATH_MAX];
        path_parent(from, from_dir, sizeof(from_dir));
        if (cut && strcmp(from_dir, dir) == 0) {
            strlcpy(last, path_base(from), last_size);   /* already here */
            done++;
            continue;
        }
        if (path_inside(dir, from)) {   /* a folder can't go inside itself */
            strlcpy(failed, path_base(from), failed_size);
            return -EINVAL;
        }
        free_name(dir, path_base(from), !cut && strcmp(from_dir, dir) == 0, to, sizeof(to));
        int err = cut ? move_tree(from, to) : copy_tree(from, to);
        if (err < 0) {
            strlcpy(failed, path_base(from), failed_size);
            return err;
        }
        strlcpy(last, path_base(to), last_size);
        done++;
    }
    if (cut) {
        clipboard_files_clear();   /* moved: nothing left to paste */
    }
    return done;
}

/* --- opening ------------------------------------------------------------------------------------- */

long open_in_text_editor(const char *path) {
    const char *argv[] = { "divine-textedit", path, NULL };
    return spawn_simple(TEXT_EDITOR, argv);
}

long open_file(const char *path) {
    struct os_file_info info;
    int err = stat(path, &info);
    if (err < 0) {
        return err;
    }
    if (info.type == OS_DIRECTORY) {
        const char *argv[] = { "divine-files", path, NULL };
        return spawn_simple(FILES_APP, argv);
    }
    if (same_ignoring_case(path_extension(info.name), "desktop")) {
        char exec[160], words[160];
        const char *argv[16];
        if (!app_file_read(path, NULL, 0, NULL, 0, exec, sizeof(exec)) ||
            split_command(exec, words, sizeof(words), argv, 16) < 1) {
            return -ENOEXEC;
        }
        const char *program = argv[0];
        argv[0] = path_base(program);
        return spawn_simple(program, argv);
    }
    if (audio_supported_name(info.name)) {
        const char *argv[] = { "divine-music", path, NULL };
        return spawn_simple(MUSIC_APP, argv);
    }
    if (image_supported_name(info.name)) {
        const char *argv[] = { "divine-photos", path, NULL };
        return spawn_simple(PHOTOS_APP, argv);
    }
    if (name_is_text(info.name)) {
        return open_in_text_editor(path);
    }
    return -ENOEXEC;
}
