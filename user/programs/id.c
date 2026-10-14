/* id: who a user is to the system (the one this program runs for, or the one named):
   uid=1000(user) gid=1000(user) groups=1000(user),27(sudo) */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <users.h>

#define MAX_GROUPS 64

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : user_name();
    static struct user_account a;
    if (!users_find(name, &a)) {
        printf("id: '%s': no such user\n", name);
        return 1;
    }
    static struct user_group groups[MAX_GROUPS];
    int n = users_groups_load(groups, MAX_GROUPS);
    const char *own = a.name;
    for (int i = 0; i < n; i++) {
        if (groups[i].gid == a.gid) {
            own = groups[i].name;
        }
    }
    printf("uid=%d(%s) gid=%d(%s) groups=%d(%s)", a.uid, a.name, a.gid, own, a.gid, own);
    for (int i = 0; i < n; i++) {
        if (groups[i].gid != a.gid && users_in_group(a.name, groups[i].name)) {
            printf(",%d(%s)", groups[i].gid, groups[i].name);
        }
    }
    printf("\n");
    return 0;
}
