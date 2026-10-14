#pragma once

/*
 * libfileops: working with files the way the file manager and the desktop
 * both need to: copying and deleting whole folders, the shared file
 * clipboard, what kind of file something is, and opening it with the
 * right app.
 *
 * The clipboard is a small file (/tmp/.clipboard): "copy" or "cut"
 * on the first line, then one path per line. Any program can read it, so
 * cutting on the desktop and pasting in Files works.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#define FILE_PATH_MAX   OS_PATH_MAX   /* 1024 */
#define CLIPBOARD_MAX   64          /* paths the clipboard holds */
#define HOME_DIR        home_dir()      /* the user's (<unistd.h>) */
#define DESKTOP_DIR     desktop_dir()

/* --- paths --------------------------------------------------------------------------- */

void path_join(char *out, size_t size, const char *dir, const char *name);
const char *path_base(const char *path);                    /* "/a/b" -> "b" */
void path_parent(const char *path, char *out, size_t size); /* "/a/b" -> "/a", "/a" -> "/" */
const char *path_extension(const char *name);               /* "x.txt" -> "txt", "" if none */
/* Compare names the way people expect: ignoring case, with numbers by value ("file 2" < "file 10"). */
int name_compare(const char *a, const char *b);
bool path_inside(const char *path, const char *dir);        /* dir itself or anything under it */

/* --- kinds of files ------------------------------------------------------------------------ */

enum file_icon { FILE_ICON_FOLDER, FILE_ICON_TEXT, FILE_ICON_FILE, FILE_ICON_PICTURE, FILE_ICON_AUDIO,
                 FILE_ICON_EXTERNAL_DRIVE, FILE_ICON_INTERNAL_DRIVE, FILE_ICON_COUNT };

bool name_is_text(const char *name);
enum file_icon file_icon_for(const struct os_file_info *info);
const char *file_icon_path(enum file_icon icon);            /* PNG in /usr/share/dde-res/Icons/FilesFolders */
const char *file_type_name(const char *path, const struct os_file_info *info);

/* An app launcher (.desktop): its name, icon and command (any may be NULL). */
bool app_file_read(const char *path, char *name, size_t name_size, char *icon, size_t icon_size,
                   char *exec, size_t exec_size);
/* Split a command line into words ("quotes" group); returns the count, argv NULL-terminated. */
int split_command(const char *line, char *buf, size_t size, const char **argv, int max);

/* --- whole trees ----------------------------------------------------------------------------- */

int copy_tree(const char *from, const char *to);
int delete_tree(const char *path);
/* rename, or (between drives) copy then delete the original. */
int move_tree(const char *from, const char *to);
void measure_tree(const char *path, uint64_t *bytes, int *files, int *folders);
/* `name` in `dir`, or "name (copy).ext", "name (copy 2).ext"... if taken (copy: always a copy name). */
void free_name(const char *dir, const char *name, bool copy, char *out, size_t size);
/* Make the home folder's standard folders if they're missing. */
void ensure_home_folders(void);

/*
 * Move files and folders into `dir` (drag and drop). Ones already in `dir`
 * are left alone, as is a folder dropped into itself; a name that's taken
 * gets a number added. Returns how many moved, or -errno for the first
 * failure (with its name in `failed`). `names` (may be NULL) gets the new
 * names of the ones that moved.
 */
int move_into(char (*paths)[FILE_PATH_MAX], int count, const char *dir, char (*names)[OS_NAME_MAX],
              char *failed, size_t failed_size);

/* --- the clipboard ------------------------------------------------------------------------- */

void clipboard_files_set(const char *const *paths, int count, bool cut);
/* The paths still there (up to max); 0 if the clipboard is empty. */
int clipboard_files_get(char (*paths)[FILE_PATH_MAX], int max, bool *cut);
bool clipboard_files_has(const char *path, bool *cut);      /* is this path on it? */
void clipboard_files_clear(void);
/*
 * Paste into `dir`: copies, or moves if they were cut. Returns how many
 * made it, or -errno for the first failure (with its name in failed).
 * `last` gets the name of the last item pasted (to select it).
 */
int clipboard_files_paste(const char *dir, char *last, size_t last_size, char *failed, size_t failed_size);

/* --- opening ---------------------------------------------------------------------------------- */

/*
 * Open a file with the right app: folders in Files, text in the Text
 * Editor, .desktop launchers by running them. Returns the new process's pid,
 * or -ENOEXEC if no app handles that kind of file (or another -errno).
 */
long open_file(const char *path);
long open_in_text_editor(const char *path);
