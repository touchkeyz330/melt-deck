#!/usr/bin/env python3
"""meltskin.py - Meltdown Empire skins for the MPC: real-plugin look + Q-Links that make sense.

For every plugin it writes plugins/<Name>/skins/ (which build.py copies verbatim):
  * TUI.json      - the plugin's pages (from ui.json) on rendered hardware-style panels: brushed panel,
                    one raised plate per row of controls with a section title, brand + logo, screws.
  * Q-Links.json  - every row of four controls is one Q-Link bank (RowTopDown), so on a 4-knob MPC
                    (Live III / Live II / One) touching a control puts its whole row under the knobs,
                    top knob = leftmost control. Program-mode Q-Links get the plugin's 16 most useful
                    controls (the PERFORM page), most important four first.
  * PNG artwork   - film-strip knobs (127 frames, metal cap, value ring with glow), faders, switches,
                    segment buttons and a background per page, all in the plugin's own colour theme.
Pages beyond six function keys become sub-tabs (press the same key again).

Usage: python tools/meltskin.py [plugin names...]   (default: every plugin with a ui.json)
       --preview DIR   also write a PNG mock-up of every page (what the MPC will show)
"""
import argparse
import json
import math
import os
import random
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_ui as G  # noqa: E402

ROOT = os.path.dirname(HERE)
FONT_DIR = "/usr/share/fonts/opentype/inter"
def font(size, weight="Bold"):
    for name in ("Inter-%s.otf" % weight, "InterDisplay-%s.otf" % weight, "Inter-Regular.otf"):
        p = os.path.join(FONT_DIR, name)
        if os.path.isfile(p):
            return ImageFont.truetype(p, size)
    return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", size)

# ------------------------------------------------------------------------------------------------
# Themes (all original colourways)
# ------------------------------------------------------------------------------------------------
def hx(s):
    s = s.lstrip("#"); return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))

BASE = {"panel": "1b1a1c", "panel2": "121113", "plate": "26252a", "cap": "black", "text": "ffffff", "muted": "a8a2a0"}
THEMES = {
    "Melt Deck":    {"accent": "ff5a1f"},
    "Melt Quad":    {"accent": "ff5a1f"},
    "Melt Rack":    {"accent": "2ec4ff", "panel": "141b24", "panel2": "0c1118", "plate": "1c2532", "cap": "silver"},
    "Melt FM":      {"accent": "9be15d", "panel": "161816", "plate": "222622"},
    "Melt Crate":   {"accent": "ffb02e", "panel": "201c18", "panel2": "15120f", "plate": "2c2620", "cap": "cream", "muted": "b9ab98"},
    "Melt Sweep":   {"accent": "ff3d8b"},
    "Melt Shift":   {"accent": "b57bff", "panel": "1a1622", "panel2": "100d16", "plate": "251f31"},
    "Melt Bus":     {"accent": "e8432e", "panel": "232830", "panel2": "171b21", "plate": "2e3540", "cap": "silver"},
    "Melt Pre":     {"accent": "e8432e", "panel": "232830", "panel2": "171b21", "plate": "2e3540", "cap": "silver"},
    "Melt Max":     {"accent": "ffc83a", "panel": "1e1f22", "plate": "2b2c31", "cap": "silver"},
    "Melt Stutter": {"accent": "ff2e88"},
    "Melt Venom":   {"accent": "39ff7a", "panel": "141814", "panel2": "0b0e0b", "plate": "1d241d"},
    "Melt Micro":   {"accent": "ff9e2e", "panel": "221a14", "panel2": "17110d", "plate": "2f241b", "cap": "cream", "muted": "bba892"},
    "Melt Stack":   {"accent": "20d6c0", "panel": "151b1b", "plate": "1f2828"},
    "Melt MS":      {"accent": "ff7a2e", "panel": "1d1d1d", "plate": "2a2a2a", "cap": "silver"},
    "Melt Clean":   {"accent": "3fd8ff", "panel": "17202a", "panel2": "0f151c", "plate": "202c38", "cap": "silver"},
    "Melt Flip":    {"accent": "ff5a1f", "cap": "cream"},
    "Melt-Tune":    {"accent": "5ee0ff", "panel": "151a20", "plate": "1f2730"},
    "Silk Opto":    {"accent": "f2c18d", "panel": "221d19", "panel2": "16120f", "plate": "2e2721", "cap": "cream", "muted": "bcae9f"},
    "Forge EQ":     {"accent": "ff6a3d", "panel": "23262b", "plate": "30343b", "cap": "silver"},
    "Font Box":     {"accent": "c88cff"},
    "Vault":        {"accent": "e6c35c", "panel": "1d1b17", "plate": "2a2721", "cap": "silver"},
    "Wave Relay":   {"accent": "4dd2ff", "panel": "14181f", "plate": "1e2430"},
}

def theme_for(name):
    t = dict(BASE); t.update(THEMES.get(name, {"accent": "ff5a1f"}))
    return t

# ------------------------------------------------------------------------------------------------
# PERFORM pages: the 16 controls the Q-Links reach first (rows of 4, most important row first)
# ------------------------------------------------------------------------------------------------
PERFORM = {
    "Melt Crate": [["Cutoff", "Reso", "Filt Env", "Drive"], ["Attack", "Decay", "Sustain", "Release"],
                   ["Transpose", "Start", "Character", "Bits"], ["Chorus", "Delay Mix", "Reverb", "Volume"]],
    "Melt FM":    [["Algorithm", "Feedback", "Cutoff", "Reso"], ["Op2 Level", "Op3 Level", "Op4 Level", "Op1 Ratio"],
                   ["Op1 Attack", "Op1 Decay", "Op1 Sustain", "Op1 Release"], ["LFO Rate", "LFO>Pitch", "Chorus Level", "Reverb Level"]],
    "Melt Flip":  [["Filter", "Pitch", "Key Shift", "Volume"], ["Attack", "Release", "Length", "Chop"],
                   ["Song", "Start Bar", "Play Mode", "Choke"], ["Tempo Sync", "BPM Fix", "Keys Mode", "Key Pad"]],
    "Melt MS":    [["LP Cutoff", "LP Peak", "HP Cutoff", "HP Peak"], ["EG2 Attack", "EG2 Decay", "EG2 Sustain", "EG2 Release"],
                   ["LP EG2 Mod", "MG Freq", "MG Freq Mod", "Drive"], ["VCO2 Pitch", "VCO2 Level", "Portamento", "Volume"]],
    "Melt Micro": [["Cutoff", "Resonance", "EG1 Int", "Distortion"], ["EG2 Attack", "EG2 Decay", "EG2 Sustain", "EG2 Release"],
                   ["Osc1 Ctrl1", "Osc1 Ctrl2", "LFO1 Rate", "LFO2 Rate"], ["Mod Depth", "Delay Mix", "Portamento", "Level"]],
    "Melt Rack":  [["T1 Cutoff", "T2 Cutoff", "T3 Cutoff", "T4 Cutoff"], ["T1 Level", "T2 Level", "T3 Level", "T4 Level"],
                   ["T1 Attack", "T1 Release", "T2 Attack", "T2 Release"], ["EFX Amount", "Chorus Level", "Reverb Level", "Volume"]],
    "Melt Stack": [["Cutoff", "Resonance", "Env Amount", "Spread"], ["Attack", "Decay", "Sustain", "Release"],
                   ["Kaoss X", "Kaoss Y", "LFO Rate", "LFO Amount"], ["Chorus", "Delay", "Reverb", "Volume"]],
    "Melt Venom": [["Cutoff", "Resonance", "Env Amount", "Drive"], ["Attack", "Decay", "Sustain", "Release"],
                   ["Osc Bal", "Uni Detune", "FM", "Osc1 Table"], ["Chorus", "Delay", "Reverb", "Volume"]],
    "Vault":      [["Cutoff", "Resonance", "Env Amount", "Drive"], ["Attack", "Decay", "Sustain", "Release"],
                   ["Tune", "Start", "Bits", "LFO>Filter"], ["LFO Rate", "Wheel Vib", "Velocity", "Volume"]],
    "Wave Relay": [["Cutoff", "Resonance", "Env Amount", "Vector"], ["Attack", "Decay", "Sustain", "Release"],
                   ["Step Time", "Crossfade", "Steps", "Mix"], ["Drone Det", "Chorus", "Velocity", "Volume"]],
    # these already open on a performance page: no extra page, but the Program Q-Links use this list
    "Melt Quad":  [["A Level", "B Level", "C Level", "D Level"], ["A Cutoff", "B Cutoff", "C Cutoff", "D Cutoff"],
                   ["A Reverb Send", "B Reverb Send", "A Delay Send", "B Delay Send"], ["Master", "Width", "Reverb Return", "Delay Return"]],
    "Melt Deck":  [["Crossfader", "A Volume", "B Volume", "Master"], ["A Filter", "A Low", "A Mid", "A High"],
                   ["B Filter", "B Low", "B Mid", "B High"], ["A Pitch", "B Pitch", "A Echo", "B Echo"]],
}
NO_PERFORM_PAGE = {"Melt Quad", "Melt Deck"}

# Function-key grouping (page names; later names in a group are sub-tabs on the same key)
KEYS = {
    "Melt Rack":  [["PERFORM"], ["PATCH", "FX"], ["TONE 1", "T1 ENV"], ["TONE 2", "T2 ENV"], ["TONE 3", "T3 ENV"], ["TONE 4", "T4 ENV"]],
    "Melt FM":    [["PERFORM"], ["PATCH", "FX"], ["OP 1"], ["OP 2"], ["OP 3"], ["OP 4"]],
    "Melt Stack": [["PERFORM"], ["OSC"], ["MIX/FX"], ["FILT/ENV"], ["LAYERS", "STEPS"], ["SEQ/KAOSS"]],
    "Melt Venom": [["PERFORM"], ["OSC", "MIX"], ["FILTER"], ["ENV"], ["LFO"], ["ARP/FX"]],
    "Wave Relay": [["PERFORM"], ["SEQ", "STEPS"], ["WAVES"], ["DRONE"], ["FILTER"], ["AMP"]],
}

# ------------------------------------------------------------------------------------------------
# Artwork
# ------------------------------------------------------------------------------------------------
SS = 4  # supersampling

def lerp_col(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))

def shade(c, f):
    return tuple(max(0, min(255, int(round(v * f)))) for v in c)

def mix(c, d, t):
    return lerp_col(c, d, t)

CAPS = {  # (rim dark, rim light, cap top, cap bottom, pointer)
    "black":  ((18, 18, 20), (70, 70, 76), (64, 64, 70), (26, 26, 29), (245, 245, 245)),
    "silver": ((60, 62, 66), (205, 208, 214), (214, 216, 222), (132, 135, 142), (25, 25, 28)),
    "cream":  ((70, 62, 52), (225, 214, 192), (236, 226, 204), (168, 156, 134), (40, 30, 24)),
}

def radial_disc(size, r, top, bottom, center=None, highlight=0.35):
    """A disc with a vertical gradient and a soft specular highlight, as an RGBA image (size x size)."""
    cx = cy = size / 2 if center is None else center
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2)
    t = np.clip((y - (cy - r)) / (2 * r), 0, 1)[..., None]
    col = np.array(top, np.float32) * (1 - t) + np.array(bottom, np.float32) * t
    spec = np.exp(-(((x - (cx - r * 0.35)) ** 2 + (y - (cy - r * 0.45)) ** 2) / (2 * (r * 0.42) ** 2)))[..., None]
    col = col + highlight * 255 * spec * 0.5
    alpha = np.clip(r - d + 0.5, 0, 1) * 255
    img = np.dstack([np.clip(col, 0, 255), alpha]).astype(np.uint8)
    return Image.fromarray(img, "RGBA")

def knob_strip(theme, bipolar, size=90, frames=127):
    acc = hx(theme["accent"]); rim_d, rim_l, cap_t, cap_b, ptr = CAPS[theme["cap"]]
    S = size * SS; c = S / 2
    r_ring = S * 0.455; ring_w = S * 0.055
    r_body = S * 0.355; r_cap = S * 0.27
    # static layers
    base = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    # ring groove
    box = [c - r_ring, c - r_ring, c + r_ring, c + r_ring]
    d.arc(box, 135, 405, fill=(8, 8, 9, 255), width=int(ring_w))
    d.arc([b + (1 if i < 2 else -1) * SS for i, b in enumerate(box)], 135, 405, fill=(40, 40, 44, 255), width=int(SS))
    # tick marks
    for i in range(11):
        a = math.radians(135 + i * 27)
        r0, r1 = r_ring + ring_w * 0.75, r_ring + ring_w * 1.25
        d.line([c + r0 * math.cos(a), c + r0 * math.sin(a), c + r1 * math.cos(a), c + r1 * math.sin(a)],
               fill=(110, 108, 106, 200), width=int(SS * 0.9))
    # body shadow
    sh = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([c - r_body, c - r_body + S * 0.03, c + r_body, c + r_body + S * 0.04], fill=(0, 0, 0, 200))
    sh = sh.filter(ImageFilter.GaussianBlur(S * 0.03))
    base = Image.alpha_composite(base, sh)
    skirt = radial_disc(S, r_body, rim_l, rim_d, highlight=0.15)
    cap = radial_disc(S, r_cap, cap_t, cap_b, highlight=0.45)
    frames_out = []
    for f in range(frames):
        v = f / (frames - 1)
        ang = 135 + 270 * v
        img = base.copy()
        dd = ImageDraw.Draw(img)
        # value arc + glow
        glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        gd = ImageDraw.Draw(glow)
        a0, a1 = (270, ang) if bipolar else (135, ang)
        if a1 < a0: a0, a1 = a1, a0
        if a1 - a0 > 0.4:
            gd.arc(box, a0, a1, fill=acc + (255,), width=int(ring_w * 1.6))
            glow = glow.filter(ImageFilter.GaussianBlur(S * 0.025))
            img = Image.alpha_composite(img, Image.eval(glow, lambda p: p) if False else glow)
            dd = ImageDraw.Draw(img)
            dd.arc(box, a0, a1, fill=acc + (255,), width=int(ring_w * 0.75))
        # skirt with knurling that turns with the knob
        sk = skirt.copy(); kd = ImageDraw.Draw(sk)
        for i in range(36):
            a = math.radians(ang + i * 10)
            r0, r1 = r_cap * 1.04, r_body * 0.97
            kd.line([c + r0 * math.cos(a), c + r0 * math.sin(a), c + r1 * math.cos(a), c + r1 * math.sin(a)],
                    fill=shade(rim_d, 0.75) + (150,), width=int(SS * 0.9))
        img = Image.alpha_composite(img, sk)
        img = Image.alpha_composite(img, cap)
        dd = ImageDraw.Draw(img)
        a = math.radians(ang)
        p0, p1 = r_cap * 0.25, r_body * 0.93
        dd.line([c + p0 * math.cos(a), c + p0 * math.sin(a), c + p1 * math.cos(a), c + p1 * math.sin(a)],
                fill=ptr + (255,), width=int(SS * 2.6))
        dot = r_ring
        frames_out.append(img.resize((size, size), Image.LANCZOS))
    strip = Image.new("RGBA", (size, size * frames), (0, 0, 0, 0))
    for i, fimg in enumerate(frames_out):
        strip.paste(fimg, (0, i * size))
    return strip

def fader_strip(theme, bipolar, w=248, h=36, frames=127):
    acc = hx(theme["accent"]); rim_d, rim_l, cap_t, cap_b, ptr = CAPS[theme["cap"]]
    S = SS; W, H = w * S, h * S
    x0, x1 = 14 * S, (w - 14) * S; cy = H // 2
    base = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)
    d.rounded_rectangle([x0 - 3 * S, cy - 4 * S, x1 + 3 * S, cy + 4 * S], radius=4 * S, fill=(6, 6, 7, 255), outline=(48, 48, 52, 255), width=S)
    for i in range(11):
        x = x0 + (x1 - x0) * i / 10
        hgt = 5 * S if i in (0, 5, 10) else 3 * S
        d.line([x, 1 * S, x, 1 * S + hgt], fill=(120, 116, 112, 200), width=S)
        d.line([x, H - 1 * S - hgt, x, H - 1 * S], fill=(120, 116, 112, 200), width=S)
    out = Image.new("RGBA", (w, h * frames), (0, 0, 0, 0))
    for f in range(frames):
        v = f / (frames - 1)
        img = base.copy(); dd = ImageDraw.Draw(img)
        xv = x0 + (x1 - x0) * v
        xa, xb = ((x0 + x1) / 2, xv) if bipolar else (x0, xv)
        if xb < xa: xa, xb = xb, xa
        if xb - xa > S:
            glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
            ImageDraw.Draw(glow).rounded_rectangle([xa, cy - 4 * S, xb, cy + 4 * S], radius=3 * S, fill=acc + (230,))
            img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(2.5 * S)))
            dd = ImageDraw.Draw(img)
            dd.rounded_rectangle([xa, cy - 2.5 * S, xb, cy + 2.5 * S], radius=2 * S, fill=acc + (255,))
        # cap
        cw, ch = 13 * S, 30 * S
        shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(shadow).rounded_rectangle([xv - cw + S, cy - ch / 2 + 2 * S, xv + cw + S, cy + ch / 2 + 2 * S], radius=3 * S, fill=(0, 0, 0, 170))
        img = Image.alpha_composite(img, shadow.filter(ImageFilter.GaussianBlur(1.5 * S)))
        capi = Image.new("RGBA", (W, H), (0, 0, 0, 0)); cd = ImageDraw.Draw(capi)
        for i in range(int(ch)):
            t = i / ch
            col = lerp_col(cap_t, cap_b, t)
            cd.line([xv - cw, cy - ch / 2 + i, xv + cw, cy - ch / 2 + i], fill=col + (255,))
        mask = Image.new("L", (W, H), 0)
        ImageDraw.Draw(mask).rounded_rectangle([xv - cw, cy - ch / 2, xv + cw, cy + ch / 2], radius=3 * S, fill=255)
        capi.putalpha(mask)
        img = Image.alpha_composite(img, capi)
        dd = ImageDraw.Draw(img)
        for k in (-6, -3, 3, 6):
            dd.line([xv - cw + 3 * S, cy + k * S, xv + cw - 3 * S, cy + k * S], fill=shade(cap_b, 0.7) + (180,), width=S)
        dd.line([xv - cw + 2 * S, cy, xv + cw - 2 * S, cy], fill=ptr + (255,), width=int(1.6 * S))
        out.paste(img.resize((w, h), Image.LANCZOS), (0, f * h))
    return out

def pill(w, h, theme, on):
    acc = hx(theme["accent"]); S = SS; W, H = w * S, h * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if on:
        g = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(g).rounded_rectangle([3 * S, 3 * S, W - 3 * S, H - 3 * S], radius=H / 2, fill=acc + (160,))
        img = Image.alpha_composite(img, g.filter(ImageFilter.GaussianBlur(2 * S)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2 * S, 2 * S, W - 2 * S, H - 2 * S], radius=H / 2,
                        fill=(acc if on else (14, 14, 16)) + (255,), outline=(60, 58, 56, 255) if not on else None, width=S)
    r = H / 2 - 5 * S
    cx = W - H / 2 if on else H / 2
    k = radial_disc(W, r, (235, 235, 238), (150, 150, 156), center=None, highlight=0.4)
    knob = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    kk = radial_disc(int(2 * r + 4), r, (235, 235, 238), (150, 150, 156))
    knob.paste(kk, (int(cx - r - 2), int(H / 2 - r - 2)), kk)
    img = Image.alpha_composite(img, knob)
    return img.resize((w, h), Image.LANCZOS)

def segment(w, h, theme, on):
    acc = hx(theme["accent"]); S = SS; W, H = w * S, h * S
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if on:
        g = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(g).rounded_rectangle([3 * S, 3 * S, W - 3 * S, H - 3 * S], radius=6 * S, fill=acc + (170,))
        img = Image.alpha_composite(img, g.filter(ImageFilter.GaussianBlur(2.5 * S)))
    body = Image.new("RGBA", (W, H), (0, 0, 0, 0)); bd = ImageDraw.Draw(body)
    top, bot = (shade(acc, 1.08), shade(acc, 0.72)) if on else ((50, 49, 53), (28, 27, 30))
    for i in range(H):
        bd.line([0, i, W, i], fill=lerp_col(top, bot, i / H) + (255,))
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([2 * S, 2 * S, W - 2 * S, H - 2 * S], radius=6 * S, fill=255)
    body.putalpha(mask)
    img = Image.alpha_composite(img, body)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([2 * S, 2 * S, W - 2 * S, H - 2 * S], radius=6 * S, outline=(8, 8, 9, 255) if not on else shade(acc, 0.5) + (255,), width=S)
    d.line([8 * S, 4 * S, W - 8 * S, 4 * S], fill=(255, 255, 255, 70 if on else 28), width=S)
    return img.resize((w, h), Image.LANCZOS)

def noise_panel(w, h, top, bottom, seed):
    rng = np.random.default_rng(seed)
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    col = np.array(top, np.float32) * (1 - t) + np.array(bottom, np.float32) * t
    col = np.broadcast_to(col, (h, w, 3)).copy()
    grain = rng.normal(0, 2.2, (h, w, 1)).astype(np.float32)
    streak = np.repeat(rng.normal(0, 1.6, (h, 1, 1)).astype(np.float32), w, axis=1)   # brushed lines
    col = np.clip(col + grain + streak, 0, 255).astype(np.uint8)
    return Image.fromarray(col, "RGB").convert("RGBA")

FAMILY = [
    ({"attack", "decay", "sustain", "release", "hold", "delay"}, "ENVELOPE"),
    ({"cutoff", "reso", "resonance", "peak", "filter", "env", "key", "track", "follow", "drive"}, "FILTER"),
    ({"chorus", "reverb", "delay", "echo", "fdbk", "feedback", "damp", "size", "time", "mod"}, "EFFECTS"),
    ({"level", "volume", "pan", "send", "mix", "master", "width", "output", "gain"}, "MIX"),
    ({"lfo", "rate", "shape", "sync", "depth", "vib", "wave"}, "MODULATION"),
]

def row_title(names, page_name):
    toks = [n.split() for n in names if n]
    if not toks:
        return ""
    first = {t[0] for t in toks if len(t) > 1}
    prefix = ""
    if len(first) == 1 and all(len(t) > 1 for t in toks):
        p = first.pop()
        prefix = ("PART " + p.upper()) if (len(p) == 1 and p.isalpha()) else p.upper().replace(">", " ")
        toks = [t[1:] for t in toks]
    fam = family_title(toks)
    if prefix:
        return prefix + ("  ·  " + fam if fam else "")
    return fam

def family_title(toks):
    words = [w.lower().strip(">") for t in toks for w in t]
    best, score = "", 0
    for keys, title in FAMILY:
        s = sum(1 for w in words if any(w.startswith(k) for k in keys))
        if s > score:
            best, score = title, s
    return best if score >= max(2, len(toks) // 2) else ""

SLOT_X, SLOT_Y, SW, SH = G.SLOT_X, G.SLOT_Y, G.SLOT_W, G.SLOT_H

def page_background(theme, plugin_name, page, page_label, perform):
    W, H = 1280, 628
    pan, pan2 = hx(theme["panel"]), hx(theme["panel2"])
    img = noise_panel(W, H, shade(pan, 1.12), pan2, seed=hash(plugin_name + page_label) & 0xffff)
    d = ImageDraw.Draw(img)
    acc = hx(theme["accent"]); plate = hx(theme["plate"]); muted = hx(theme["muted"])
    ftitle, fbrand, flogo = font(15, "Bold"), font(12, "SemiBold"), font(26, "ExtraBold")
    rows = {}
    for i, ctl in enumerate(page.controls):
        r, cidx = divmod(i, 4)
        if ctl.widget != "empty":
            rows.setdefault(r, []).append((cidx, ctl))
    for r, items in sorted(rows.items()):
        cols = [c for c, _ in items]
        x0, x1 = SLOT_X[min(cols)] - 10, SLOT_X[max(cols)] + SW + 10
        y0, y1 = SLOT_Y[r] - 5, SLOT_Y[r] + SH + 5
        # plate shadow + plate + bevel
        sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle([x0 + 2, y0 + 5, x1 + 2, y1 + 7], radius=14, fill=(0, 0, 0, 150))
        img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(6)))
        pl = noise_panel(x1 - x0, y1 - y0, shade(plate, 1.1), shade(plate, 0.86), seed=r * 7 + len(plugin_name))
        m = Image.new("L", pl.size, 0); ImageDraw.Draw(m).rounded_rectangle([0, 0, pl.size[0] - 1, pl.size[1] - 1], radius=14, fill=255)
        pl.putalpha(m); img.paste(pl, (x0, y0), pl)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([x0, y0, x1, y1], radius=14, outline=shade(plate, 0.55) + (255,), width=1)
        d.line([x0 + 14, y0 + 1, x1 - 14, y0 + 1], fill=shade(plate, 1.45) + (255,), width=1)
        # dividers between the controls of a row
        for cidx in cols[:-1]:
            xx = SLOT_X[cidx] + SW + 21
            d.line([xx, y0 + 14, xx, y1 - 14], fill=shade(plate, 0.65) + (255,), width=1)
            d.line([xx + 1, y0 + 14, xx + 1, y1 - 14], fill=shade(plate, 1.25) + (255,), width=1)
        # section title in the gap above the plate
        if perform:
            title = "Q-LINK BANK %d" % (r + 1)
        else:
            title = row_title([c.param.name if c.param else (c.text or "") for _, c in items], page_label)
        if title:
            ty = y0 - 26
            d.rounded_rectangle([x0 + 4, ty + 4, x0 + 8, ty + 18], radius=2, fill=acc + (255,))
            d.text((x0 + 15, ty + 1), title, font=ftitle, fill=muted + (255,))
    # logo + brand
    d = ImageDraw.Draw(img)
    logo = plugin_name.upper()
    tw = d.textlength(logo, font=flogo)
    d.text((W - 24 - tw, 4), logo, font=flogo, fill=acc + (255,))
    brand = "MELTDOWN EMPIRE"
    bw = d.textlength(brand, font=fbrand)
    d.text((W - 24 - tw - 14 - bw, 14), brand, font=fbrand, fill=muted + (200,))
    for sx, sy in ((8, 8), (W - 9, H - 9), (8, H - 9)):
        d.ellipse([sx - 4, sy - 4, sx + 4, sy + 4], fill=(60, 60, 64, 255), outline=(10, 10, 10, 255))
        d.line([sx - 3, sy, sx + 3, sy], fill=(20, 20, 22, 255), width=1)
    return img.convert("RGB")

# ------------------------------------------------------------------------------------------------
# Skin assembly
# ------------------------------------------------------------------------------------------------
def get_key(o, k):
    for kk, v in o:
        if kk == k:
            return v
    raise KeyError(k)

def set_key(o, k, val):
    for i, (kk, v) in enumerate(o):
        if kk == k:
            o[i] = (kk, val); return
    raise KeyError(k)

def argb(hex6, a="ff"):
    return a + hex6.lower()

def build(plugin, preview_dir=None, quiet=False):
    proj = os.path.join(ROOT, "plugins", plugin)
    params = G.load_params(os.path.join(proj, "build", "params.json"))
    with open(os.path.join(proj, "build", "params.json")) as f:
        readonly = {p["index"] for p in json.load(f)["params"] if not p.get("automatable", True)}
    table = G.ParamTable(params)
    pages, style, _, _, _ = G.load_ui(os.path.join(proj, "ui.json"), table)
    # choice switches whose captions cannot fit their segments become a value bar with a menu
    for pg in pages:
        for c in pg.controls:
            if c.widget == "switch" and c.param is not None and c.param.kind != "bool" and c.param.choices:
                n = len(c.param.choices); room = {2: 9, 3: 6, 4: 4}.get(n, 0)
                if max(len(ch) for ch in c.param.choices) > room:
                    c.widget, c.overlay = "slider", "menu"
    theme = theme_for(plugin)
    style.update({"background": argb(theme["panel2"]), "accent": argb(theme["accent"]),
                  "text": argb(theme["text"]), "muted": argb(theme["muted"])})

    # PERFORM page
    existing = {}
    for pg in pages:
        for c in pg.controls:
            if c.param is not None:
                existing.setdefault(c.param.name, c)
    perform_rows = PERFORM.get(plugin)
    perform_page = None
    if perform_rows:
        ctls = []
        for row in perform_rows:
            for name in row:
                p = table.lookup(name, plugin + " PERFORM")
                proto = existing.get(p.name)
                if proto is not None:
                    ctls.append(G.Control(proto.widget, param=p, overlay=proto.overlay, polarity=proto.polarity))
                else:
                    ctls.append(G.Control("switch" if p.kind == "bool" else "knob", param=p))
        if plugin not in NO_PERFORM_PAGE:
            perform_page = G.Page("PERFORM", ctls)
            pages = [perform_page] + pages
        program = [c.param.index for c in ctls]
    else:
        program = []
        for pg in pages:
            for c in pg.controls:
                if c.param is not None and c.param.index not in program and c.param.index not in readonly:
                    program.append(c.param.index)
        for p in params:   # top up with anything else that is automatable
            if len(program) >= 16: break
            if p.index not in program and p.index not in readonly:
                program.append(p.index)
    program = program[:16]

    # function keys
    groups = KEYS.get(plugin)
    names = [p.name for p in pages]
    keymap = {}
    if groups:
        for k, grp in enumerate(groups):
            for s, nm in enumerate(grp):
                keymap[nm] = (k, s)
        missing = [n for n in names if n not in keymap]
        if missing: raise SystemExit("%s: pages not placed on a key: %s" % (plugin, missing))
    else:
        if len(pages) > 6: raise SystemExit("%s: %d pages need a KEYS grouping" % (plugin, len(pages)))
        keymap = {n: (i, 0) for i, n in enumerate(names)}
    order = sorted(range(len(pages)), key=lambda i: keymap[names[i]])
    pages = [pages[i] for i in order]

    out = os.path.join(proj, "skins")
    if os.path.isdir(out):
        for f in os.listdir(out): os.remove(os.path.join(out, f))
    os.makedirs(out, exist_ok=True)

    # artwork with the kit's file names, sizes and frame counts
    knob_strip(theme, False).save(os.path.join(out, "knob_uni.png"), optimize=True)
    knob_strip(theme, True).save(os.path.join(out, "knob_bi.png"), optimize=True)
    fader_strip(theme, False).save(os.path.join(out, "fader_h_uni.png"), optimize=True)
    fader_strip(theme, True).save(os.path.join(out, "fader_h_bi.png"), optimize=True)
    pill(64, 32, theme, True).save(os.path.join(out, "toggle_on.png"))
    pill(64, 32, theme, False).save(os.path.join(out, "toggle_off.png"))
    for n, (wseg, _) in G.SEGMENTS.items():
        segment(wseg, 40, theme, True).save(os.path.join(out, "seg%d_on.png" % n))
        segment(wseg, 40, theme, False).save(os.path.join(out, "seg%d_off.png" % n))
    kit_assets = {"knob_uni": ("knob_uni.png", 127, (90, 90)), "knob_bi": ("knob_bi.png", 127, (90, 90)),
                  "fader_h_uni": ("fader_h_uni.png", 127, (248, 36)), "fader_h_bi": ("fader_h_bi.png", 127, (248, 36)),
                  "toggle_on": ("toggle_on.png", 1, (64, 32)), "toggle_off": ("toggle_off.png", 1, (64, 32))}
    for n in (2, 3, 4):
        kit_assets["seg%d_on" % n] = ("seg%d_on.png" % n, 1, (G.SEGMENTS[n][0], 40))
        kit_assets["seg%d_off" % n] = ("seg%d_off.png" % n, 1, (G.SEGMENTS[n][0], 40))

    tui = G.build_tui(pages, params, style, None, 127, kit_assets)
    tabs = get_key(get_key(tui, "pageData"), "tabs")
    q_entries = []
    for ti, (tab, page) in enumerate(zip(tabs, pages)):
        k, s = keymap[page.name]
        set_key(tab, "fnKeyIndex", k)
        set_key(tab, "fnKeySubIndex", s)
        bg = "bg_%02d.jpg" % (ti + 1)
        page_background(theme, plugin, page, page.name, page is perform_page).save(os.path.join(out, bg), quality=92, subsampling=0)
        cd = get_key(tab, "componentDefinition")
        children = get_key(cd, "componentsData")
        children.insert(0, G.entry("panel", "Image", G.obj(("version", 2), ("imageType", "Regular"), ("colour", "0"), ("image", bg)),
                                   "0 0 1280 628", "No", "Show", "Always"))
        rows_used = sorted({i // 4 for i, c in enumerate(page.controls) if c.param is not None})
        set_key(tab, "qlinkBoundsData", ["8 %d 1264 %d" % (SLOT_Y[r] - 9, SH + 18) for r in range(4)])
        assigned = {}
        for i, c in enumerate(page.controls):
            if c.param is not None:
                r, col = divmod(i, 4)
                assigned[G.slot_qlink(r, col)] = c.param.index
        q_entries.append(G.obj(("Tab", k + 1), ("SubTab", s + 1), ("Bank Direction", "RowTopDown"),
                               ("Q-Links", G.Obj(("Q-Link %d" % q, assigned.get(q, -1)) for q in range(1, 17)))))
    # Program mode: most important four on the first column bank (13, 9, 5, 1), and so on
    order_q = [13, 9, 5, 1, 14, 10, 6, 2, 15, 11, 7, 3, 16, 12, 8, 4]
    prog = {q: (program[i] if i < len(program) else -1) for i, q in enumerate(order_q)}
    qlinks = G.obj(("version", 4), ("info", G.obj(("version", 1), ("type", "CompleteDescription"))),
                   ("Screen Mode Q-Links", G.obj(("version", 4), ("map", q_entries))),
                   ("Program Mode Q-Links", G.Obj(("Q-Link %d" % q, prog[q]) for q in range(1, 17))))
    with open(os.path.join(out, "TUI.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(tui) + "\n")
    with open(os.path.join(out, "Q-Links.json"), "w", encoding="utf-8", newline="\n") as f:
        f.write(G.dump_json(qlinks) + "\n")
    if preview_dir:
        make_previews(out, plugin, pages, theme, params, preview_dir, keymap)
    if not quiet:
        print("meltskin: %-13s %d pages on %d keys, PERFORM=%s, program Q-Links: %s"
              % (plugin, len(pages), len({keymap[p.name][0] for p in pages}), "yes" if perform_page else "no",
                 ", ".join(params[i].name for i in program[:4]) + " ..."))

# ------------------------------------------------------------------------------------------------
# Preview mock-ups (approximate: the MPC draws the text itself)
# ------------------------------------------------------------------------------------------------
def make_previews(skins, plugin, pages, theme, params, outdir, keyof):
    keyfirst = [p for p in pages if keyof[p.name][1] == 0]
    os.makedirs(outdir, exist_ok=True)
    knob = {b: Image.open(os.path.join(skins, "knob_%s.png" % b)) for b in ("uni", "bi")}
    fader = {b: Image.open(os.path.join(skins, "fader_h_%s.png" % b)) for b in ("uni", "bi")}
    fn, fv, ftab = font(22, "Medium"), font(26, "SemiBold"), font(20, "Bold")
    txt, muted, acc = hx(theme["text"]), hx(theme["muted"]), hx(theme["accent"])
    for ti, page in enumerate(pages):
        canvas = Image.new("RGB", (1280, 800), (10, 10, 11))
        bg = Image.open(os.path.join(skins, "bg_%02d.jpg" % (ti + 1)))
        canvas.paste(bg, (0, 110))
        d = ImageDraw.Draw(canvas)
        d.rectangle([0, 0, 1280, 109], fill=(24, 24, 26)); d.text((24, 36), "MPC  ·  %s" % plugin, font=ftab, fill=(200, 200, 200))
        d.rectangle([0, 738, 1280, 800], fill=(24, 24, 26))
        for k, pg in enumerate(keyfirst):
            on = keyof[pg.name][0] == keyof[page.name][0]
            label = page.name if on else pg.name
            d.rounded_rectangle([12 + k * 211, 748, 12 + k * 211 + 200, 792], radius=8, fill=(60, 60, 64) if on else (36, 36, 40))
            d.text((24 + k * 211, 758), label, font=ftab, fill=(255, 255, 255))
        for i, c in enumerate(page.controls):
            if c.param is None: continue
            r, col = divmod(i, 4); x, y = SLOT_X[col], SLOT_Y[r] + 110
            p = c.param
            v = 0.5 if c.bipolar else (p.default if isinstance(p.default, (int, float)) else 0.5)
            v = max(0.0, min(1.0, float(v)))
            disp = p.display or ""
            if c.widget == "knob":
                fr = int(round(v * 126)); k = knob["bi" if c.bipolar else "uni"].crop((0, fr * 90, 90, fr * 90 + 90))
                canvas.paste(k, (x + 8, y + 8), k)
                d.text((x + 110, y + 14), p.name, font=fn, fill=muted); d.text((x + 110, y + 52), disp, font=fv, fill=txt)
            elif c.widget == "fader":
                fr = int(round(v * 126)); k = fader["bi" if c.bipolar else "uni"].crop((0, fr * 36, 248, fr * 36 + 36))
                canvas.paste(k, (x + 15, y + 54), k)
                d.text((x + 15, y + 10), p.name, font=fn, fill=muted)
                tw = d.textlength(disp, font=fv); d.text((x + 263 - tw, y + 8), disp, font=fv, fill=txt)
            elif c.widget == "switch":
                d.text((x + 15, y + 10), p.name, font=fn, fill=muted)
                n = len(p.choices or ["Off", "On"])
                if p.kind == "bool" or n == 1:
                    t = Image.open(os.path.join(skins, "toggle_%s.png" % ("on" if v >= 0.5 else "off")))
                    canvas.paste(t, (x + 196, y + 38), t); d.text((x + 15, y + 52), disp, font=fv, fill=txt)
                else:
                    wseg, xs = G.SEGMENTS[n]; sel = int(round(v * (n - 1)))
                    for ci, (ch, xx) in enumerate(zip(p.choices, xs)):
                        t = Image.open(os.path.join(skins, "seg%d_%s.png" % (n, "on" if ci == sel else "off")))
                        canvas.paste(t, (x + xx, y + 54), t)
                        tw = d.textlength(ch, font=font(18, "SemiBold")); d.text((x + xx + (wseg - tw) / 2, y + 62), ch, font=font(18, "SemiBold"), fill=(255, 255, 255))
            else:
                d.text((x + 15, y + 14), p.name, font=fn, fill=muted)
                d.rounded_rectangle([x + 15, y + 60, x + 263, y + 92], radius=6, fill=(30, 30, 34)); d.text((x + 24, y + 62), disp, font=fv, fill=txt)
        canvas.save(os.path.join(outdir, "%s - %d %s.png" % (plugin, ti + 1, page.name.replace("/", "-"))))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("plugins", nargs="*")
    ap.add_argument("--preview")
    a = ap.parse_args()
    names = a.plugins or sorted(n for n in os.listdir(os.path.join(ROOT, "plugins"))
                                if os.path.isfile(os.path.join(ROOT, "plugins", n, "ui.json")))
    for n in names:
        build(n, a.preview)

if __name__ == "__main__":
    main()
