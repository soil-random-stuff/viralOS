# HTTPS downloads: boots on QEMU's NAT network (needs internet on the host), opens a terminal and
# runs wget against real sites: a page over TLS, a redirect, a bad certificate (expired.badssl.com,
# wrong.host.badssl.com), a missing page, plain http, a 5 MB file (sha256sum) and Ctrl+C in the middle
# of one (the unfinished file goes). Screenshots: build/https-*.png.
# Run from os-source/ in WSL:  bash scripts/tests/https.sh
python3 scripts/drive.py wait:24 key:ret wait:8 click:316,758 wait:4 \
  "type:wget https://example.com/" key:ret wait:15 shot:https-1 \
  "type:clear" key:ret wait:1 \
  "type:wget -O - https://example.com/" key:ret wait:12 shot:https-2 \
  "type:clear" key:ret wait:1 \
  "type:wget https://github.com/soil-random-stuff/viralOS/raw/main/LICENSE" key:ret wait:25 shot:https-3 \
  "type:clear" key:ret wait:1 \
  "type:wget https://expired.badssl.com/" key:ret wait:12 \
  "type:wget https://wrong.host.badssl.com/" key:ret wait:12 \
  "type:wget https://example.com/nope" key:ret wait:12 shot:https-4 \
  "type:clear" key:ret wait:1 \
  "type:wget -O plain.html http://example.com/" key:ret wait:12 \
  "type:wget https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2" key:ret \
  wait:4 shot:https-5 wait:40 "type:clear" key:ret "type:sha256sum mbedtls-3.6.7.tar.bz2" key:ret wait:3 \
  "type:ls" key:ret wait:2 shot:https-6 \
  "type:clear" key:ret wait:1 "type:wget -O big.bin https://speed.cloudflare.com/__down?bytes=90000000" key:ret \
  wait:3 key:ctrl-c wait:2 "type:ls" key:ret wait:2 shot:https-7 >/dev/null 2>&1
grep -iE "panic|killed|fault" build/serial.log || true
