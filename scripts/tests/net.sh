# Networking: boots on QEMU's NAT network (NIC=virtio-net-pci by default; NIC=e1000 / e1000e to try
# the others), opens a terminal and runs netctl and ping. Screenshots: build/net-*.png.
# Run from os-source/ in WSL:  bash scripts/tests/net.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
  "type:netctl" key:ret wait:2 \
  "type:ping -c 3 10.0.2.2" key:ret wait:5 shot:net-1 \
  "type:clear" key:ret wait:1 \
  "type:ping -c 3 example.com" key:ret wait:8 shot:net-2 \
  "type:clear" key:ret wait:1 \
  "type:netctl device show eth0" key:ret wait:2 shot:net-3 >/dev/null 2>&1
grep -E "\[net\]|virtio|e1000|r8169|PANIC|panic" build/serial.log
