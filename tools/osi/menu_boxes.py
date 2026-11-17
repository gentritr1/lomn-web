"""M2a-3 T8: the I2/F22 checks of `make osi-menu` on lomn_view's `hud box` stdout lines (hud_pass's boxes: the quads'
transformed vertices, which are pixels at 640x480 under the HUD's ortho camera, hud.md §3.3).

  menu_boxes.py RUN SUMMARY      RUN = M1 | M2; exit 1 on a failed check

Checked (FAIL): the cursor quad's centre is the mouse pixel of that frame (I2); M1: every menu string's ink starts at
x >= 400 (the script hit box's left edge, F22) and "Play Game" is above "Options"; M2: the eight Options entries, each
drawn twice, top to bottom. Reported only (INFO): whether the
ink top lies in [Y, Y + 20) -- hud.md §4.11 open item (DrawJust 4 centres the string on Y, the click box is Y+1..Y+19).
"""
import sys

# frame -> the mouse position MOUSESPEC holds there (the Makefile's OSI_M1 / OSI_M2)
OPTIONS = ["Sound", "Graphics", "Language", "Glossary", "Print", "Credits", "Controls", "Back"]
MOUSE = {"M1": {665: (450, 330), 904: (450, 370)}, "M2": {1520: (450, 370)}}


def parse(path):
    boxes = {}
    for line in open(path, encoding="latin-1"):
        if not line.startswith("hud box "):
            continue
        # hud box <frame> <who> cell x0 y0 x1 y1 ink x0 y0 x1 y1 <text...>
        t = line.rstrip("\n").split(" ")
        frame, who = int(t[2]), t[3]
        cell, ink, text = tuple(map(float, t[5:9])), tuple(map(float, t[10:14])), " ".join(t[14:])
        boxes.setdefault(frame, []).append((who, cell, ink, text))
    return boxes


def main(run, path):
    boxes, bad = parse(path), 0

    def check(ok, msg):
        nonlocal bad
        print(("OK   " if ok else "FAIL ") + msg)
        bad += not ok

    for frame, (mx, my) in MOUSE[run].items():
        items = boxes.get(frame)
        check(bool(items), f"{run} frame {frame}: hud box lines present")
        if not items:
            continue
        cur = [b for b in items if b[3] == "<sprite>" and b[0].endswith(":0000009a")]
        check(len(cur) == 1, f"{run} frame {frame}: one cursor quad")
        if cur:
            x0, y0, x1, y1 = cur[0][1]
            check(((x0 + x1) / 2, (y0 + y1) / 2) == (mx, my), f"{run} frame {frame}: cursor centre ({(x0 + x1) / 2:g},{(y0 + y1) / 2:g}) == mouse ({mx},{my})")
        texts = [b for b in items if b[3] != "<sprite>"]
        if run == "M2":  # the Options screen: a left column (pen x 50), so the main menu's x >= 400 box does not apply
            order = [t for _, _, _, t in sorted(texts, key=lambda b: b[2][1])][::2]
            check(order == OPTIONS, f"{run} frame {frame}: Options entries top to bottom {order}")
        if run == "M1":
            for who, cell, ink, text in texts:
                check(ink[0] >= 400, f"{run} frame {frame}: {text!r} {who} ink x0 {ink[0]:g} >= 400")
            top = {b[3]: b[2][1] for b in texts}
            check(top.get("Play Game", 1e9) < top.get("Options", -1e9), f"{run} frame {frame}: 'Play Game' above 'Options'")
            for name, y in (("Play Game", 320), ("Options", 360), ("Quit", 400)):
                if name in top:
                    print(f"INFO {run} frame {frame}: {name!r} ink top {top[name]:g} vs hit box y [{y}, {y + 20}) "
                          f"({'inside' if y <= top[name] < y + 20 else 'outside'}; hud.md §4.11 open item)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
