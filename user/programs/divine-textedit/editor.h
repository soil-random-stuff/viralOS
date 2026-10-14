#pragma once

/*
 * Text Editor: the shared state of its parts.
 *
 *   buffer.c     the document: lines of text, inserting/deleting, undo and redo, files
 *   textfield.c  a one-line text box (find bar, dialogs)
 *   dialogs.c    open / save as, go to line, "save changes?", messages
 *   main.c       the window: toolbar, find bar, the text, status bar, keys and mouse
 */

#include <gui.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <paths.h>

#define PATH_MAX        256
#define TEXT_CLIPBOARD  _PATH_TMP "/.clipboard-text"
#define TAB_WIDTH       4

/* Colours shared by all the parts. */
#define WHITE       GFX_RGB(255, 255, 255)
#define INK         GFX_RGB(0x1C, 0x1C, 0x22)
#define INK_SOFT    GFX_RGB(0x78, 0x78, 0x82)
#define INK_FAINT   GFX_RGB(0xC0, 0xC0, 0xC8)
#define LINE_COLOR  GFX_RGB(0xEC, 0xEC, 0xF0)
#define FIELD       GFX_RGB(0xF4, 0xF5, 0xF8)
#define ACCENT      GFX_RGB(0x2F, 0x6F, 0xEB)
#define DANGER      GFX_RGB(0xE0, 0x3E, 0x3E)
#define SELECTION   GFX_RGB(0xC8, 0xDA, 0xFB)
#define UI_SIZE     13.0f

/* A place in the document: line number and byte offset in that line. */
struct pos {
    int line, col;
};

struct line {
    char *text;              /* always NUL-terminated */
    int len, cap;
};

/* One undoable change. Changes with the same group are undone together. */
struct change {
    bool insert;             /* else a deletion */
    struct pos a, b;         /* where it starts, and where the inserted/deleted text ended */
    char *text;
    int len;
    int group;
    uint64_t time;
};

struct textfield {
    char text[256];
    int len;
    int cursor, anchor;      /* byte offsets; anchor != cursor: a selection */
    float scroll;            /* pixels the text is shifted left when it doesn't fit */
};

enum dialog_kind {
    DIALOG_NONE, DIALOG_OPEN, DIALOG_SAVE_AS, DIALOG_GOTO, DIALOG_SAVE_CHANGES, DIALOG_REPLACE_FILE,
    DIALOG_MESSAGE,
};

/* What to do once the "save changes?" question is answered. */
enum pending { PENDING_NONE, PENDING_NEW, PENDING_OPEN, PENDING_QUIT };

struct dialog_entry {
    char name[OS_NAME_MAX];
    bool folder;
};

struct dialog {
    enum dialog_kind kind;
    char title[96];
    char message[200];
    struct textfield field;          /* file name / line number */
    char dir[PATH_MAX];              /* open / save as: the folder shown */
    struct dialog_entry *entries;
    int count, selected, scroll;
    char target[PATH_MAX];           /* replace file: the path to save to */
    int hover_button, pressed_button;
    uint64_t last_click_ms;
    int last_click_index;
};

enum focus { FOCUS_TEXT, FOCUS_FIND, FOCUS_REPLACE };

struct editor {
    struct gui *g;
    struct gui_window *w;
    struct ttf_font *mono, *ui, *ui_bold;

    /* The document. */
    struct line *lines;
    int count, cap;
    char path[PATH_MAX];             /* "" until saved */
    bool modified;

    struct pos cursor, anchor;       /* anchor != cursor: a selection */
    int goal_cells;                  /* column Up/Down try to stay in */
    int scroll_line;
    float scroll_x;

    float font_size;
    float cell_w, cell_h, ascent;

    /* Undo and redo stacks. */
    struct change *undo, *redo;
    int undo_len, undo_cap, redo_len, redo_cap;
    int group, open_group;           /* open_group: changes being collected into one step */
    int saved_undo_len;              /* undo depth when last saved (-1: never matches) */
    bool merge_blocked;              /* the next change starts a new step */

    /* Find and replace. */
    bool find_open, replace_open, match_case;
    struct textfield find, replace;
    enum focus focus;

    struct dialog dialog;
    enum pending pending;

    /* Mouse. */
    bool selecting;
    uint64_t last_click_ms;
    int clicks;
    struct pos last_click_pos;
    int hover_button, pressed_button;
    bool focused;

    char status[160];
};

extern struct editor ed;

/* buffer.c */
void doc_reset(void);
int doc_load(const char *path);                 /* 0 or -errno */
int doc_save(const char *path);
int pos_cmp(struct pos a, struct pos b);
struct pos pos_clamp(struct pos p);
struct pos pos_next(struct pos p);               /* one character on (wraps to the next line) */
struct pos pos_prev(struct pos p);
struct pos doc_end(void);
int col_to_cells(int line, int col);
int cells_to_col(int line, int cells);
struct pos doc_insert(struct pos at, const char *text, int len);   /* returns the end */
void doc_delete(struct pos a, struct pos b);
char *doc_get(struct pos a, struct pos b, int *len);              /* malloc'd */
void undo_begin_group(void);
void undo_end_group(void);
void undo_break(void);
bool doc_undo(void);
bool doc_redo(void);
void doc_mark_saved(void);

/* textfield.c */
void field_set(struct textfield *f, const char *text);
bool field_key(struct textfield *f, const struct gui_event *ev);   /* true if it changed the text */
void field_click(struct textfield *f, struct gfx_rect box, int x, bool extend);
void field_draw(struct gfx_surface *s, struct textfield *f, struct gfx_rect box, bool focused,
                const char *placeholder);

/* dialogs.c */
void dialog_open_file(enum dialog_kind kind);   /* DIALOG_OPEN / DIALOG_SAVE_AS */
void dialog_goto(void);
void dialog_save_changes(enum pending then);
void dialog_message(const char *title, const char *message);
void dialog_close(void);
void dialog_draw(struct gfx_surface *s);
void dialog_event(const struct gui_event *ev);

/* main.c */
void editor_open_path(const char *path);
bool editor_save_to(const char *path);
void editor_after_pending(void);
void editor_new(void);
void editor_goto_line(int line);
void update_title(void);
bool clipboard_read(char **text, int *len);
void clipboard_write(const char *text, int len);
