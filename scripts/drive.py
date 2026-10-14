# Drive QEMU over QMP: python3 drive.py <steps...>
#   wait:S  click:X,Y[,right]  move:X,Y  scroll:X,Y,N  type:text  key:name  shot:name  unplug:usbN  plug:IMAGE
# DISKS=a.img,b.img attaches raw images as USB sticks usb0, usb1... (on a qemu-xhci).
# NVME=a.img,... attaches NVMe SSDs; SATA=a.img,... SATA disks on the q35's AHCI controller.
# NIC=virtio-net-pci (default) / e1000 / e1000e / none: the network card, on QEMU's NAT network.
# QEMU_EXTRA="..." adds arguments.
# NO_CD=1 boots from the first disk instead of viralos.iso (an installed system).
import json, shlex, socket, subprocess, sys, time, os
W, H = 1280, 800
os.makedirs("build", exist_ok=True)
q = subprocess.Popen(["qemu-system-x86_64", "-M", "q35", "-cpu", "max", "-smp", os.environ.get("SMP", "4"), "-m", "512M",
    "-rtc", "base=localtime"]
    # NO_CD=1: no ISO, start from the first disk (an installed viralOS).
    + (["-boot", "order=c"] if os.environ.get("NO_CD") else ["-cdrom", "viralos.iso", "-boot", "order=d"])
    + ["-display", "none",
    "-serial", "file:build/serial.log",
    "-audiodev", "wav,id=snd0,path=build/audio.wav", "-device", "ich9-intel-hda", "-device", "hda-output,audiodev=snd0", "-qmp", "unix:/tmp/qmp.sock,server,wait=off"]
    + (["-device", "qemu-xhci,id=xhci"] if os.environ.get("DISKS") else [])
    + [a for i, img in enumerate(filter(None, os.environ.get("DISKS", "").split(","))) for a in
       ("-drive", f"if=none,id=disk{i},file={img},format=raw", "-device", f"usb-storage,bus=xhci.0,drive=disk{i},id=usb{i}")]
    + [a for i, img in enumerate(filter(None, os.environ.get("NVME", "").split(","))) for a in
       ("-drive", f"if=none,id=nv{i},file={img},format=raw", "-device", f"nvme,serial=NVME000{i},drive=nv{i}")]
    + [a for i, img in enumerate(filter(None, os.environ.get("SATA", "").split(","))) for a in
       ("-drive", f"if=none,id=sd{i},file={img},format=raw",
        "-device", f"ide-hd,drive=sd{i},bus=ide.{3 + i},model=KINGSTON SA400S37240G,serial=50026B77{i:08d}")]
    + (["-nic", "none"] if os.environ.get("NIC", "virtio-net-pci") == "none" else
       ["-netdev", "user,id=net0" + os.environ.get("NETDEV_EXTRA", ""), "-device", os.environ.get("NIC", "virtio-net-pci") + ",netdev=net0"])
    + shlex.split(os.environ.get("QEMU_EXTRA", "")))
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
KEYS = {' ': 'spc', '-': 'minus', '.': 'dot', '/': 'slash', '\n': 'ret', '>': ('shift', 'dot'), '|': ('shift','backslash'),
        '_': ('shift', 'minus'), ':': ('shift', 'semicolon'), '~': ('shift', 'grave_accent'), "'": 'apostrophe',
        '"': ('shift', 'apostrophe'), ',': 'comma', '*': ('shift', '8'), '(': ('shift', '9'), ')': ('shift', '0'),
        '=': 'equal', '&': ('shift', '7'), '<': ('shift', 'comma'), '+': ('shift', 'equal'), '%': ('shift', '5')}
def key(names):
    names = names if isinstance(names, tuple) else (names,)
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
    elif k == "scroll":   # scroll:X,Y,N  (N wheel steps down; negative: up)
        x, y, n = map(int, v.split(","))
        cmd("input-send-event", events=abs_ev(x, y)); time.sleep(0.15)
        for _ in range(abs(n)):
            b = "wheel-down" if n > 0 else "wheel-up"
            cmd("input-send-event", events=[btn(b, True)]); time.sleep(0.05)
            cmd("input-send-event", events=[btn(b, False)]); time.sleep(0.05)
    elif k == "path":   # path:X1,Y1,X2,Y2,...  (press at the first point, move through the rest, release)
        pts = list(map(int, v.split(",")))
        pts = list(zip(pts[0::2], pts[1::2]))
        cmd("input-send-event", events=abs_ev(*pts[0])); time.sleep(0.15)
        cmd("input-send-event", events=[btn("left", True)]); time.sleep(0.1)
        for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
            for t in range(1, 9):
                cmd("input-send-event", events=abs_ev(x1 + (x2 - x1) * t // 8, y1 + (y2 - y1) * t // 8)); time.sleep(0.03)
        cmd("input-send-event", events=[btn("left", False)]); time.sleep(0.15)
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
    elif k == "unplug":
        cmd("device_del", id=v); time.sleep(0.5)
    elif k == "plug":   # plug:IMAGE  (a new USB stick on the DISKS controller)
        n = sum(1 for x in sys.argv if x.startswith("plug:") and sys.argv.index(x) <= sys.argv.index(step))
        cmd("blockdev-add", driver="raw", **{"node-name": f"hot{n}", "file": {"driver": "file", "filename": v}})
        cmd("device_add", driver="usb-storage", bus="xhci.0", drive=f"hot{n}", id=f"hot{n}"); time.sleep(0.5)
    elif k == "shot":
        cmd("screendump", filename=f"build/{v}.png", format="png"); time.sleep(0.5)
try:
    cmd("quit")
except Exception:
    print("QEMU already gone")
q.wait()
