# The Calculator, the System Monitor and the Snipping Tool.
#   Calculator: 1234+5678= then *2= (build/ap-calc.png), and 5/0= (ap-calc-err.png).
#   System Monitor: the Processes list (ap-mon-1.png), Performance > Memory (ap-mon-2.png).
#   Snipping Tool: Print Screen, a rectangular snip, highlighter and pen marks, Ctrl+S
#     (ap-snip.png), then the saved PNG opened in Photos (ap-snip-photos.png).
# Run from os-source/ in WSL:  bash scripts/tests/apps.sh
python3 scripts/drive.py wait:24 key:ret wait:8 \
  click:46,758 wait:2 click:79,395 wait:3 "type:1234+5678=" "type:*2=" wait:1 shot:ap-calc \
  key:esc "type:5/0=" wait:1 shot:ap-calc-err key:esc "type:12*12=" \
  click:46,758 wait:2 click:355,395 wait:5 shot:ap-mon-1 click:392,97 wait:3 click:310,250 wait:2 shot:ap-mon-2 \
  click:961,56 wait:2 \
  key:print wait:4 move:380,30 wait:1 drag:380,30,740,570 wait:3 \
  click:772,70 wait:1 drag:640,230,735,230 wait:1 click:675,70 wait:1 drag:445,560,560,600 wait:1 \
  key:ctrl-s wait:3 shot:ap-snip \
  click:316,758 wait:4 "type:cd ~/Pictures/Screenshots" key:ret "type:divine-photos Scr" key:tab key:ret wait:5 \
  shot:ap-snip-photos >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
