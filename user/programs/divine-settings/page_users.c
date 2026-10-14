/*
 * Users: the account logged in (its name, picture and password), the other accounts, and adding
 * one (libusers: /etc/passwd and /etc/shadow). Pictures to choose from: the default and the
 * pictures in the user's Pictures folder.
 *
 * Administrators are the members of the "sudo" group: they may use sudo, and only they can add and
 * remove accounts and make others administrators here. There's always at least one.
 */

#include <image.h>
#include <stdio.h>
#include <string.h>
#include <users.h>

#include "settings.h"

#define MAX_PICTURES 12
#define CHOICE       56

static struct user_account me, others[USERS_MAX];
static int other_count;
static bool me_admin, others_admin[USERS_MAX], add_admin;
static bool can_manage;          /* the user is an administrator (or root) */
static int admin_count;
static struct gfx_surface *my_picture, *other_pictures[USERS_MAX];
static struct field display, current_pw, new_pw, confirm_pw;
static struct field add_name, add_display, add_pw;

static char picture_paths[MAX_PICTURES][OS_PATH_MAX];
static struct gfx_surface *picture_thumbs[MAX_PICTURES];
static int picture_count;
static bool pictures_scanned;

static void scan_pictures(void) {
    for (int i = 0; i < picture_count; i++) {
        gfx_surface_destroy(picture_thumbs[i]);
    }
    picture_count = 0;
    strlcpy(picture_paths[picture_count++], "", sizeof(picture_paths[0]));   /* the default */
    char dir[OS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Pictures", home_dir());
    int fd = open(dir, O_RDONLY);
    struct os_file_info info;
    for (uint64_t i = 0; fd >= 0 && readdir(fd, i, &info) == 0 && picture_count < MAX_PICTURES; i++) {
        if (info.type == OS_FILE && image_supported_name(info.name)) {
            snprintf(picture_paths[picture_count++], sizeof(picture_paths[0]), "%s/%s", dir, info.name);
        }
    }
    if (fd >= 0) {
        close(fd);
    }
    for (int i = 0; i < picture_count; i++) {
        picture_thumbs[i] = NULL;   /* (made a piece at a time: page_users_work) */
    }
    pictures_scanned = true;
}

void page_users_show(void) {
    if (!users_find(user_name(), &me)) {
        memset(&me, 0, sizeof(me));
        strlcpy(me.name, user_name(), sizeof(me.name));
    }
    gfx_surface_destroy(my_picture);
    my_picture = users_picture(&me, 64);
    field_set(&display, me.display[0] ? me.display : me.name);
    current_pw.secret = new_pw.secret = confirm_pw.secret = add_pw.secret = true;
    field_set(&current_pw, "");
    field_set(&new_pw, "");
    field_set(&confirm_pw, "");

    for (int i = 0; i < other_count; i++) {
        gfx_surface_destroy(other_pictures[i]);
    }
    static struct user_account all[USERS_MAX];
    int n = users_load(all, USERS_MAX);
    other_count = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(all[i].name, me.name) != 0) {
            others[other_count] = all[i];
            others_admin[other_count] = users_in_group(all[i].name, USERS_ADMIN_GROUP);
            other_pictures[other_count] = users_picture(&all[i], 36);
            other_count++;
        }
    }
    me_admin = users_in_group(me.name, USERS_ADMIN_GROUP);
    can_manage = me_admin || strcmp(me.name, "root") == 0;
    admin_count = me_admin;
    for (int i = 0; i < other_count; i++) {
        admin_count += others_admin[i];
    }
    if (!pictures_scanned) {
        scan_pictures();
    }
}

bool page_users_work(void) {
    for (int i = 0; i < picture_count; i++) {
        if (!picture_thumbs[i]) {
            struct user_account a;
            memset(&a, 0, sizeof(a));
            strlcpy(a.picture, picture_paths[i], sizeof(a.picture));
            picture_thumbs[i] = users_picture(&a, CHOICE);
            if (!picture_thumbs[i]) {
                picture_thumbs[i] = gfx_surface_create(1, 1);   /* (unreadable: an empty spot) */
            }
            return true;
        }
    }
    return false;
}

/* Make `name` an administrator or not (the toggle already shows the new state in *shown). */
static void set_admin(const char *name, bool *shown, bool mine) {
    bool admin = *shown;
    if (!admin && admin_count <= 1) {
        *shown = true;
        settings_message(GUI_ICON_WARNING, "Can't remove the last administrator",
                         "Someone has to be able to manage this computer: make another user an "
                         "administrator first.");
        return;
    }
    if (!admin && mine && !settings_confirm("Stop being an administrator?",
                                            "You won't be able to use sudo or change other users' accounts. "
                                            "Another administrator can make you one again.", "Stop")) {
        *shown = true;
        return;
    }
    int r = admin ? users_group_add(USERS_ADMIN_GROUP, name) : users_group_remove(USERS_ADMIN_GROUP, name);
    if (r < 0 && r != -EEXIST) {
        settings_message(GUI_ICON_ERROR, "Couldn't change that", strerror(r));
    }
    page_users_show();
}

static void change_password(void) {
    if (users_has_password(&me) && !users_check_password(&me, current_pw.text)) {
        settings_message(GUI_ICON_ERROR, "Wrong password", "The current password isn't right.");
        return;
    }
    if (strcmp(new_pw.text, confirm_pw.text) != 0) {
        settings_message(GUI_ICON_ERROR, "The passwords don't match", "Type the new password twice, the same way.");
        return;
    }
    int r = users_set_password(me.name, new_pw.text);
    if (r < 0) {
        settings_message(GUI_ICON_ERROR, "Couldn't change the password", strerror(r));
        return;
    }
    settings_message(GUI_ICON_INFO, new_pw.len ? "Password changed" : "Password removed",
                     new_pw.len ? "Use it the next time you log in or unlock." : "Logging in won't ask for one.");
    page_users_show();
}

static void add_user(void) {
    if (!users_valid_name(add_name.text)) {
        settings_message(GUI_ICON_ERROR, "Not an account name",
                         "Use lowercase letters, digits, - and _ (it's also the home folder's name).");
        return;
    }
    int r = users_add(add_name.text, add_display.len ? add_display.text : add_name.text, add_pw.text);
    if (r == -EEXIST) {
        settings_message(GUI_ICON_ERROR, "That name is taken", "Choose another account name.");
        return;
    }
    if (r < 0) {
        settings_message(GUI_ICON_ERROR, "Couldn't add the user", strerror(r));
        return;
    }
    if (add_admin) {
        users_group_add(USERS_ADMIN_GROUP, add_name.text);
    }
    add_admin = false;
    field_set(&add_name, "");
    field_set(&add_display, "");
    field_set(&add_pw, "");
    page_users_show();
}

void page_users(void) {
    /* Who's logged in. */
    ui_card_begin();
    struct gfx_rect r = ui_row(96);
    if (my_picture && !ui.measuring) {
        gfx_blit(ui.s, r.x + 18, r.y + 16, my_picture, NULL);
    }
    ui_text(FONT_TITLE, true, r.x + 98, r.y + 44, INK, me.display[0] ? me.display : me.name);
    char sub[384];
    snprintf(sub, sizeof(sub), "%s  -  %s  -  %s  -  %s", me.name, me.home[0] ? me.home : home_dir(),
             me_admin ? "Administrator" : "Standard user", users_has_password(&me) ? "has a password" : "no password");
    ui_text_clipped(FONT_SMALL, false, r.x + 98, r.y + 66, r.w - 120, INK_SOFT, sub);
    if (ui_field_row("Display name", &display, me.name, true)) {
        int e = users_set_display(me.name, display.text);
        if (e < 0) {
            settings_message(GUI_ICON_ERROR, "Couldn't change the name", strerror(e));
        }
        ui.focus = NULL;
        page_users_show();
    }
    if (ui_toggle_row_enabled("Administrator", "Can use sudo, and add and change other users", &me_admin,
                              can_manage)) {
        set_admin(me.name, &me_admin, true);
    }
    ui_card_end();
    ui_note("Press Enter to save the name. It's shown on the login screen and in the apps menu.");

    /* The picture. */
    ui_heading("Picture");
    ui_card_begin();
    int per_row = (ui.width - 32 + 14) / (CHOICE + 14);
    per_row = per_row < 1 ? 1 : per_row;
    int rows = (picture_count + per_row - 1) / per_row;
    struct gfx_rect grid = ui_row(rows * (CHOICE + 16) + 18);
    for (int i = 0; i < picture_count; i++) {
        struct gfx_rect t = gfx_rect(grid.x + 16 + (i % per_row) * (CHOICE + 14), grid.y + 16 + (i / per_row) * (CHOICE + 16),
                                     CHOICE, CHOICE);
        bool selected = strcmp(picture_paths[i], me.picture) == 0;
        if (ui_clicked(t, picture_paths[i])) {
            users_set_picture(me.name, picture_paths[i]);
            page_users_show();
            ui.dirty = true;
        }
        if (!ui.measuring) {
            if (selected) {
                gfx_fill_circle(ui.s, t.x + CHOICE / 2.0f, t.y + CHOICE / 2.0f, CHOICE / 2.0f + 4, ACCENT);
                gfx_fill_circle(ui.s, t.x + CHOICE / 2.0f, t.y + CHOICE / 2.0f, CHOICE / 2.0f + 1.5f, CARD);
            }
            if (picture_thumbs[i] && picture_thumbs[i]->width > 1) {
                gfx_blit(ui.s, t.x, t.y, picture_thumbs[i], NULL);
            } else {
                gfx_fill_circle(ui.s, t.x + CHOICE / 2.0f, t.y + CHOICE / 2.0f, CHOICE / 2.0f, CONTROL);
            }
        }
    }
    ui_card_end();
    ui_note("Put pictures in your Pictures folder to use them here.");

    /* The password. */
    ui_heading("Password");
    ui_card_begin();
    if (users_has_password(&me)) {
        ui_field_row("Current password", &current_pw, "", true);
    }
    ui_field_row("New password", &new_pw, users_has_password(&me) ? "Empty: no password" : "", true);
    ui_field_row("Confirm", &confirm_pw, "", true);
    if (ui_button_row(users_has_password(&me) ? "Change the password" : "Set a password",
                      "For logging in and unlocking the screen", "Change Password", true)) {
        change_password();
    }
    ui_card_end();

    /* Everyone else. */
    if (other_count) {
        ui_heading("Other users");
        ui_card_begin();
        for (int i = 0; i < other_count; i++) {
            struct gfx_rect row = ui_row(56);
            if (other_pictures[i] && !ui.measuring) {
                gfx_blit(ui.s, row.x + 16, row.y + 10, other_pictures[i], NULL);
            }
            ui_text(FONT_BODY, true, row.x + 64, row.y + 26, INK, others[i].display[0] ? others[i].display : others[i].name);
            char who[OS_USER_MAX + 32];
            snprintf(who, sizeof(who), "%s  -  %s", others[i].name, others_admin[i] ? "Administrator" : "Standard user");
            ui_text(FONT_SMALL, false, row.x + 64, row.y + 44, INK_SOFT, who);
            struct gfx_rect admin_r = gfx_rect(row.x + row.w - 106 - 16 - 40, row.y + 16, 40, 24);
            if (!ui.measuring) {
                float lw = ui_text_width(FONT_SMALL, false, "Admin");
                ui_text(FONT_SMALL, false, admin_r.x - 8 - lw, admin_r.y + 16, INK_SOFT, "Admin");
            }
            if (ui_toggle_enabled(admin_r, &others_admin[i], can_manage)) {
                set_admin(others[i].name, &others_admin[i], false);
            }
            if (ui_button(gfx_rect(row.x + row.w - 106, row.y + 14, 90, 28), others[i].name, false, can_manage)) {
                char question[128];
                snprintf(question, sizeof(question), "Remove %s?", others[i].display[0] ? others[i].display : others[i].name);
                if (settings_confirm(question, "The account goes; its home folder in /home stays.", "Remove")) {
                    users_remove(others[i].name);
                    page_users_show();
                }
            }
            if (!ui.measuring) {
                /* (The button's key is the name; its label says Remove.) */
                struct gfx_rect b = gfx_rect(row.x + row.w - 106, row.y + 14, 90, 28);
                gfx_fill_rounded_rect(ui.s, b, 7, !can_manage ? GFX_RGB(0xEE, 0xEE, 0xF1)
                                                  : ui_hover(b) ? CONTROL_HOT : CONTROL);
                float tw = ui_text_width(FONT_BODY - 0.5f, false, "Remove");
                ui_text(FONT_BODY - 0.5f, false, b.x + (b.w - tw) / 2, b.y + 19,
                        can_manage ? DANGER : GFX_RGB(0xB8, 0xB8, 0xBE), "Remove");
            }
        }
        ui_card_end();
    }

    ui_heading("Add a user");
    ui_card_begin();
    ui_field_row("Account name", &add_name, "e.g. alex", can_manage);
    ui_field_row("Display name", &add_display, "e.g. Alex", can_manage);
    ui_field_row("Password", &add_pw, "Optional", can_manage);
    ui_toggle_row_enabled("Administrator", "Can use sudo, and add and change other users", &add_admin, can_manage);
    if (ui_button_row_enabled("Add the user", "They get a home folder: /home/<account name>", "Add User", false,
                              can_manage)) {
        add_user();
    }
    ui_card_end();
    ui_note(can_manage ? "Accounts keep everyone's files and desktop in their own home folder. viralOS doesn't stop "
                         "users from opening each other's files yet."
                       : "Only administrators can add and remove users, or make someone an administrator.");
}
