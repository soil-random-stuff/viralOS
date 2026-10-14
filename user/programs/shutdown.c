/* shutdown: turn the computer off. */

#include <sound.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    system_sound_wait("Shutdown");
    int err = power(OS_POWER_OFF);
    printf("shutdown: %s\n", strerror(err));
    return 1;
}
