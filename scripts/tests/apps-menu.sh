# The apps menu with more apps than fit above the panel: copies a launcher 20 times, logs out and in
# (the desktop reads the launchers when it starts), opens the menu, scrolls it with the wheel, then
# drags the scroll bar. Screenshots: build/am-*.png.
# Run from os-source/ in WSL:  bash scripts/tests/apps-menu.sh
steps=()
for i in $(seq 1 20); do
    steps+=("type:cp /usr/share/applications/divine-music.desktop /usr/share/applications/z$i.desktop" key:ret)
done
python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 "${steps[@]}" wait:2 \
  click:46,758 wait:2 click:380,692 wait:2 click:268,544 wait:10 key:ret wait:8 \
  click:46,758 wait:3 shot:am-1 scroll:200,400,3 wait:2 shot:am-2 move:410,400 wait:1 shot:am-3 \
  drag:410,400,410,90 wait:2 shot:am-4 click:79,100 wait:4 shot:am-5 >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
