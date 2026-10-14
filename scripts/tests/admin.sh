# Settings > Users administrator toggles: turning off the only admin is refused, adds "sam" as a
# standard user, makes him an admin, then the user stops being one (confirm dialog): their controls
# grey out and sudo refuses. Screenshots: build/ad-last.png, ad-6.png (greyed page), ad-7.png (terminal).
# Run from os-source/ in WSL:  bash scripts/tests/admin.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:262,758 wait:4 click:230,169 wait:4 \
  click:945,283 wait:3 shot:ad-last key:ret wait:2 \
  scroll:680,500,20 wait:2 click:845,373 "type:sam" click:921,575 wait:6 scroll:680,500,20 wait:2 click:839,267 wait:3 \
  scroll:680,400,-30 wait:2 click:945,283 wait:3 click:719,347 wait:3 scroll:680,500,20 wait:2 shot:ad-6 \
  click:316,758 wait:4 "type:id" key:ret wait:1 "type:id sam" key:ret wait:1 "type:sudo id" key:ret wait:2 shot:ad-7 >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
