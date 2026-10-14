# Users and the login screen: logs in (no password), sets one in Settings > Users, locks with
# Ctrl+Alt+L, tries a wrong then the right password, adds an account, logs out and in as it.
# Screenshots: build/users-*.png. Run from os-source/ in WSL:  bash scripts/tests/users.sh
python3 scripts/drive.py wait:24 shot:users-login key:ret wait:8 \
  click:262,758 wait:5 click:230,169 wait:4 click:845,531 type:pass click:845,577 type:pass click:892,629 wait:6 \
  key:ret wait:2 key:ctrl-alt-l wait:3 type:wrong key:ret wait:6 shot:users-wrong type:pass key:ret wait:6 \
  click:316,758 wait:4 "type:echo alex:x:1001:1001:Alex:/home/alex:/usr/bin/shell >> /etc/passwd" key:ret wait:2 \
  click:46,758 wait:2 click:380,692 wait:2 click:268,544 wait:10 shot:users-logout \
  click:83,696 wait:2 key:ret wait:8 click:316,758 wait:4 "type:whoami -l" key:ret wait:2 shot:users-alex >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
