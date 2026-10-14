# netctl changes and ping's Ctrl+C summary. Screenshots: build/netctl-*.png.
# Run from os-source/ in WSL:  bash scripts/tests/netctl.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
  "type:netctl d modify eth0 ipv4.method manual ipv4.addresses 10.0.2.50/24 ipv4.gateway 10.0.2.2 ipv4.dns 10.0.2.3" key:ret wait:3 \
  "type:ping 10.0.2.2" key:ret wait:3.5 key:ctrl-c wait:1 shot:netctl-1 \
  "type:clear" key:ret wait:1 \
  "type:netctl d down eth0" key:ret wait:2 "type:netctl" key:ret wait:2 \
  "type:netctl d up eth0" key:ret wait:4 shot:netctl-2 \
  "type:clear" key:ret wait:1 \
  "type:netctl d modify eth0 ipv4.method auto ipv4.dns none" key:ret wait:4 \
  "type:cat /etc/network.conf" key:ret wait:2 "type:netctl" key:ret wait:2 shot:netctl-3 >/dev/null 2>&1
grep -E "\[net\]|PANIC|panic" build/serial.log
