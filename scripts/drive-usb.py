# Drive QEMU over QMP: python3 drive.py <steps...>
#   wait:S  click:X,Y[,right]  move:X,Y  type:text  key:name  shot:name
import json, socket, subprocess, sys, time, os
W, H = 1280, 800
os.makedirs("build", exist_ok=True)
q = subprocess.Popen(["qemu-system-x86_64", "-M", "q35,i8042=off", "-cpu", "max", "-m", "512M",
    "-rtc", "base=localtime", "-cdrom", "viralos.iso", "-display", "none",
    "-serial", "file:build/serial.log",
    "-audiodev", "wav,id=snd0,path=build/audio.wav", "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0",
    "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0,port=1,id=ukbd",
    "-device", "usb-mouse,bus=xhci.0,port=2,id=umouse", "-device", "usb-hub,bus=xhci.0,port=3,id=uhub",
    "-device", "usb-kbd,bus=xhci.0,port=3.2,id=ukbd2", "-qmp", "unix:/tmp/qmp.sock,server,wait=off"])
time.sleep(1.5)
s = socket.socket(socket.AF_UNIX); s.connect("/tmp/qmp.sock"); f = s.makefile("rw")
def cmd(c, **a):
    f.write(json.dumps({"execute": c, "arguments": a} if a else {"execute": c}) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r:
            if "error" in r: print(r)
            return r
f.readline(); cmd("qmp_capabilities")
def abs_ev(x, y):
    return [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}]
def btn(b, down): return {"type": "btn", "data": {"down": down, "button": b}}
KEYS = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', '\n': 'ret', '>': ('shift', 'dot'), '|': ('shift','backslash')}
def key(names):
    names = names if isinstance(names, tuple) else (names,)
    if False:
        for down in (True, False):
            for n in (names if down else reversed(names)):
                cmd("input-send-event", device=os.environ["KBD"], events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": n}}}])
            time.sleep(0.03)
        time.sleep(0.05)
    else:
        cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names]); time.sleep(0.08)
for step in sys.argv[1:]:
    k, _, v = step.partition(":")
    if k == "wait": time.sleep(float(v))
    elif k in ("click", "move", "dclick"):
        p = v.split(","); x, y = int(p[0]), int(p[1]); b = "right" if len(p) > 2 else "left"
        cmd("input-send-event", events=abs_ev(x, y)); time.sleep(0.15)
        if k != "move":
            for _ in range(2 if k == "dclick" else 1):
                cmd("input-send-event", events=[btn(b, True)]); time.sleep(0.08)
                cmd("input-send-event", events=[btn(b, False)]); time.sleep(0.12)
    elif k == "drag":
        x1, y1, x2, y2 = map(int, v.split(","))
        cmd("input-send-event", events=abs_ev(x1, y1)); time.sleep(0.15)
        cmd("input-send-event", events=[btn("left", True)]); time.sleep(0.1)
        for t in range(1, 6):
            cmd("input-send-event", events=abs_ev(x1 + (x2 - x1) * t // 5, y1 + (y2 - y1) * t // 5)); time.sleep(0.08)
        cmd("input-send-event", events=[btn("left", False)]); time.sleep(0.15)
    elif k == "type":
        for c in v.replace("\n", "\n"):
            if c.isupper(): key(("shift", c.lower()))
            elif c.isalnum(): key(c)
            else: key(KEYS[c])
    elif k == "key": key(tuple(v.split("-")) if "-" in v else v)
    elif k == "uclick":   # click with the USB mouse where it is
        cmd("input-send-event", events=[btn("left", True)]); time.sleep(0.1)
        cmd("input-send-event", events=[btn("left", False)]); time.sleep(0.15)
    elif k == "urel":   # move the USB mouse: urel:dx,dy
        dx, dy = map(int, v.split(","))
        cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}},
                                                         {"type": "rel", "data": {"axis": "y", "value": dy}}])
        time.sleep(0.1)
    elif k == "shot":
        cmd("screendump", filename=f"build/{v}.png", format="png"); time.sleep(0.5)
try:
    cmd("quit")
except Exception:
    print("QEMU already gone")
q.wait()
