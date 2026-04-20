"""Generate assets/vs_duck.svg — animated pixel sloth-vs-duck banner."""

WIDTH = 900
HEIGHT = 260
BG_FROM = "#0F0B23"
BG_TO = "#1E1B4B"
GROUND = "#4C1D95"
SLOTH_BODY = "#8B6F47"
SLOTH_FUR = "#A0826D"
DUCK_BODY = "#FDB515"
DUCK_WING = "#E89F0B"
DUCK_BEAK = "#F97316"
PURPLE = "#C4B5FD"
ACCENT = "#8B5CF6"
BLACK = "#0F0B23"

# Pixel helpers — draw rectangles at pixel-grid positions.
def px(x, y, w, h, color, opacity=1.0):
    op = f' opacity="{opacity}"' if opacity != 1.0 else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color}"{op}/>'


# ---- SLOTH (left side, ~150px wide, 120px tall) -----------------------------
# Anchored at (110, 90). Body is a soft blob with two eyes and a half-smile.
sloth = []
SX, SY = 110, 95
S = 6  # pixel size
# Body
for rx, ry in [
    (2, 3), (3, 3), (4, 3), (5, 3), (6, 3), (7, 3),
    (1, 4), (2, 4), (3, 4), (4, 4), (5, 4), (6, 4), (7, 4), (8, 4),
    (1, 5), (2, 5), (3, 5), (4, 5), (5, 5), (6, 5), (7, 5), (8, 5),
    (1, 6), (2, 6), (3, 6), (4, 6), (5, 6), (6, 6), (7, 6), (8, 6),
    (2, 7), (3, 7), (4, 7), (5, 7), (6, 7), (7, 7),
]:
    sloth.append(px(SX + rx*S, SY + ry*S, S, S, SLOTH_BODY))
# Head (lighter fur)
for rx, ry in [
    (3, 0), (4, 0), (5, 0), (6, 0),
    (2, 1), (3, 1), (4, 1), (5, 1), (6, 1), (7, 1),
    (2, 2), (3, 2), (4, 2), (5, 2), (6, 2), (7, 2),
]:
    sloth.append(px(SX + rx*S, SY + ry*S, S, S, SLOTH_FUR))
# Eyes (black pixels)
sloth.append(px(SX + 3*S, SY + 1*S, S, S, BLACK))
sloth.append(px(SX + 6*S, SY + 1*S, S, S, BLACK))
# Mouth (slight smile — two pixels)
sloth.append(px(SX + 4*S, SY + 2*S, S, S, BLACK))
sloth.append(px(SX + 5*S, SY + 2*S, S, S, BLACK))
# Arms (left arm down, right arm as "kicking leg/paw" that we animate)
sloth.append(px(SX + 0*S, SY + 4*S, S, S, SLOTH_BODY))
sloth.append(px(SX + 0*S, SY + 5*S, S, S, SLOTH_BODY))

# Animated kick arm: a group that rotates around the shoulder.
kick_arm = f'''
<g id="kickarm">
  <animateTransform attributeName="transform"
    type="rotate"
    values="0 {SX+8*S} {SY+5*S}; 0 {SX+8*S} {SY+5*S}; -75 {SX+8*S} {SY+5*S}; 0 {SX+8*S} {SY+5*S}; 0 {SX+8*S} {SY+5*S}"
    keyTimes="0; 0.4; 0.5; 0.62; 1"
    dur="4s"
    repeatCount="indefinite"/>
  {px(SX+9*S, SY+4*S, S, S, SLOTH_BODY)}
  {px(SX+9*S, SY+5*S, S, S, SLOTH_BODY)}
  {px(SX+10*S, SY+5*S, S, S, SLOTH_BODY)}
  {px(SX+11*S, SY+5*S, S*2, S, SLOTH_FUR)}
</g>
'''

# Feet
sloth.append(px(SX + 3*S, SY + 8*S, S, S, SLOTH_BODY))
sloth.append(px(SX + 4*S, SY + 8*S, S, S, SLOTH_BODY))
sloth.append(px(SX + 6*S, SY + 8*S, S, S, SLOTH_BODY))
sloth.append(px(SX + 7*S, SY + 8*S, S, S, SLOTH_BODY))

# ---- DUCK (starts center, gets launched right) -----------------------------
# Anchor origin for animation: duck starts at (DX, DY) baseline.
DX, DY = 360, 130
D = 5  # duck pixel size
duck_pixels = []
# Body
for rx, ry in [
    (2, 3), (3, 3), (4, 3), (5, 3), (6, 3), (7, 3),
    (1, 4), (2, 4), (3, 4), (4, 4), (5, 4), (6, 4), (7, 4), (8, 4),
    (1, 5), (2, 5), (3, 5), (4, 5), (5, 5), (6, 5), (7, 5), (8, 5),
    (2, 6), (3, 6), (4, 6), (5, 6), (6, 6), (7, 6),
]:
    duck_pixels.append(px(rx*D, ry*D, D, D, DUCK_BODY))
# Head
for rx, ry in [
    (5, 0), (6, 0), (7, 0),
    (4, 1), (5, 1), (6, 1), (7, 1), (8, 1),
    (4, 2), (5, 2), (6, 2), (7, 2), (8, 2),
]:
    duck_pixels.append(px(rx*D, ry*D, D, D, DUCK_BODY))
# Eye
duck_pixels.append(px(6*D, 1*D, D, D, BLACK))
# Beak
duck_pixels.append(px(8*D, 2*D, D*2, D, DUCK_BEAK))
duck_pixels.append(px(9*D, 3*D, D, D, DUCK_BEAK))
# Wing
for rx, ry in [
    (3, 4), (4, 4), (5, 4),
    (3, 5), (4, 5), (5, 5),
]:
    duck_pixels.append(px(rx*D, ry*D, D, D, DUCK_WING))
# Feet
duck_pixels.append(px(3*D, 7*D, D, D, DUCK_BEAK))
duck_pixels.append(px(6*D, 7*D, D, D, DUCK_BEAK))

# Duck animation: idle, then after kick flies across, spinning and fading.
duck_group = f'''
<g id="duck">
  <animateTransform attributeName="transform"
    type="translate"
    values="{DX},{DY}; {DX},{DY}; {DX+20},{DY-20}; {WIDTH+50},{-20}; {WIDTH+50},{-20}; {DX},{DY}"
    keyTimes="0; 0.5; 0.55; 0.78; 0.97; 1"
    dur="4s"
    repeatCount="indefinite"/>
  <animateTransform attributeName="transform"
    additive="sum"
    type="rotate"
    values="0; 0; 30; 1080; 1080; 0"
    keyTimes="0; 0.5; 0.55; 0.78; 0.97; 1"
    dur="4s"
    repeatCount="indefinite"/>
  {"".join(duck_pixels)}
</g>
'''

# ---- IMPACT lines radiating from duck at kick moment ----
impact = []
for angle, length in [(-30, 25), (-10, 30), (15, 28), (35, 22)]:
    impact.append(f'''
<line x1="{DX-5}" y1="{DY+20}" x2="{DX-5+int(length*(angle/30+1))}" y2="{DY+20+angle}"
      stroke="{ACCENT}" stroke-width="3" opacity="0">
  <animate attributeName="opacity"
    values="0; 0; 0; 1; 0; 0"
    keyTimes="0; 0.48; 0.5; 0.53; 0.58; 1"
    dur="4s"
    repeatCount="indefinite"/>
</line>''')

# ---- POW! text ----
pow_text = f'''
<text x="{DX+10}" y="{DY-10}" font-family="'Courier New', monospace" font-weight="900"
      font-size="36" fill="#F97316" opacity="0" transform="rotate(-10 {DX+10} {DY-10})">
  POW!
  <animate attributeName="opacity"
    values="0; 0; 0; 1; 1; 0; 0"
    keyTimes="0; 0.48; 0.5; 0.55; 0.65; 0.72; 1"
    dur="4s"
    repeatCount="indefinite"/>
</text>
'''

# ---- Trophy / speedup text appears after duck exits ----
winner_text = f'''
<text x="{WIDTH//2}" y="60" text-anchor="middle"
      font-family="'Courier New', monospace" font-weight="bold"
      font-size="22" fill="{PURPLE}" opacity="0" letter-spacing="3">
  SLOTHDB WINS — 6.6x FASTER
  <animate attributeName="opacity"
    values="0; 0; 0; 1; 1; 0"
    keyTimes="0; 0.78; 0.8; 0.85; 0.97; 1"
    dur="4s"
    repeatCount="indefinite"/>
</text>
'''

# ---- Background: retro grid + ground ----
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" preserveAspectRatio="xMidYMid meet">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="{BG_FROM}"/>
      <stop offset="100%" stop-color="{BG_TO}"/>
    </linearGradient>
    <pattern id="dots" width="24" height="24" patternUnits="userSpaceOnUse">
      <rect width="2" height="2" fill="{ACCENT}" opacity="0.2"/>
    </pattern>
  </defs>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#bg)"/>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#dots)"/>
  <!-- ground line -->
  <rect x="0" y="{HEIGHT-30}" width="{WIDTH}" height="4" fill="{GROUND}"/>
  <!-- sloth (static body + animated kick arm) -->
  {"".join(sloth)}
  {kick_arm}
  <!-- duck (animated) -->
  {duck_group}
  <!-- impact lines -->
  {"".join(impact)}
  <!-- POW! -->
  {pow_text}
  <!-- Winner text -->
  {winner_text}
  <!-- Corner pixel decoration -->
  <rect x="20" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>
  <rect x="32" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.4"/>
  <rect x="{WIDTH-28}" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>
  <rect x="{WIDTH-40}" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.4"/>
</svg>
'''

with open("assets/vs_duck.svg", "w", encoding="utf-8") as f:
    f.write(svg)
print(f"Wrote assets/vs_duck.svg ({len(sloth)} sloth pixels, {len(duck_pixels)} duck pixels)")
