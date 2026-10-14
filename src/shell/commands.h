#pragma once

/* Shared between the shell's source files. */

#include <stdint.h>

struct vnode;

/* The shell's current directory (holds a reference). */
extern struct vnode *shell_cwd;

/* Split off the first space-separated word; returns the rest (or ""). */
char *next_word(char *s, char **word);
/* Parse a decimal number; returns `fallback` if `s` doesn't start with a digit. */
uint64_t parse_u64(const char *s, uint64_t fallback);

/* cmd_system.c */
void cmd_help(char *args);
void cmd_clear(char *args);
void cmd_uptime(char *args);
void cmd_ps(char *args);
void cmd_kill(char *args);
void cmd_sleep(char *args);
void cmd_spin(char *args);
void cmd_demo(char *args);
void cmd_meminfo(char *args);
void cmd_memmap(char *args);
void cmd_crash(char *args);

/* cmd_fs.c */
void cmd_cd(char *args);
void cmd_pwd(char *args);
void cmd_tree(char *args);
void cmd_mkdir(char *args);
void cmd_touch(char *args);
void cmd_rm(char *args);
void cmd_df(char *args);

/* run.c: start a program from /usr/bin (or a path), handling > >> < and &. */
void run_program(char *line);
