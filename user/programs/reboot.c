/* reboot: restart the computer. */

#include <sound.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    system_sound_wait("Shutdown");
    int err = power(OS_POWER_RESTART);
    printf("reboot: %s\n", strerror(err));
    return 1;
}
