# The Settings app: logs in, opens it from the panel and visits each page. Screenshots: build/settings-*.png.
# Sidebar rows (window at its default place): Network 133, Users 169, General 219, Sound 255,
# Displays 291, Wallpaper 327, Desktop & Panel 363, Mouse 413, Date & Time 449, Storage 485 (x 230).
# Run from os-source/ in WSL:  bash scripts/tests/settings.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:262,758 wait:4 shot:settings-network \
  click:230,169 wait:3 shot:settings-users \
  click:230,219 wait:2 shot:settings-general \
  click:230,255 wait:2 shot:settings-sound \
  click:230,291 wait:2 shot:settings-displays \
  click:230,327 wait:1 shot:settings-wallpaper0 wait:8 shot:settings-wallpaper \
  click:230,363 wait:2 shot:settings-desktop \
  click:230,413 wait:2 shot:settings-mouse \
  click:230,449 wait:2 shot:settings-datetime \
  click:230,485 wait:2 shot:settings-storage \
  click:230,82 "type:wall" wait:2 shot:settings-search >/dev/null 2>&1
grep -iE "killed|panic" build/serial.log
