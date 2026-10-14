# sudo, su, useradd, passwd, userdel and id from the terminal: makes an admin "bob" with a password,
# su's to him, tries sudo with a wrong then the right password, su to the locked root, sudo -s,
# then removes bob. Screenshots: build/su-1.png, su-2.png, su-3.png.
# Run from os-source/ in WSL:  bash scripts/tests/sudo.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 "type:id" key:ret wait:1 "type:useradd bob" key:ret wait:1 \
  "type:sudo useradd -c Bob -G sudo bob" key:ret wait:2 "type:sudo passwd bob" key:ret wait:2 "type:bobpw" key:ret wait:1 \
  "type:bobpw" key:ret wait:4 "type:id bob" key:ret wait:1 "type:passwd -S bob" key:ret wait:1 shot:su-1 key:ctrl-l \
  "type:su bob" key:ret wait:2 "type:bobpw" key:ret wait:4 "type:whoami -l" key:ret wait:1 "type:sudo whoami" key:ret wait:2 \
  "type:nope" key:ret wait:4 "type:bobpw" key:ret wait:5 "type:sudo -k" key:ret wait:1 "type:exit" key:ret wait:2 \
  "type:su" key:ret wait:2 "type:x" key:ret wait:3 shot:su-2 key:ctrl-l "type:sudo -s" key:ret wait:3 "type:whoami -l" key:ret \
  wait:1 "type:id" key:ret wait:1 "type:exit" key:ret wait:2 "type:sudo userdel -r bob" key:ret wait:3 "type:ls /home" key:ret \
  wait:1 "type:cat /etc/group" key:ret wait:2 shot:su-3 >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
