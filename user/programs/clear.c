/* clear: clear the screen (ANSI: erase display, cursor to the top left). */

#include <stdio.h>

int main(void) {
    printf("\x1b[2J\x1b[H");
    return 0;
}
