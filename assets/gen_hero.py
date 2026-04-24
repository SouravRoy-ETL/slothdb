"""Generate assets/hero.svg - pixel-art SLOTHDB wordmark with sloth kicking duck.

Single combined hero banner: SLOTHDB text on top, animated sloth-kicks-duck
scene underneath. Replaces the previous text-only banner.
"""

FONT = {
    "S": ["01111","10000","10000","01110","00001","00001","11110"],
    "L": ["10000","10000","10000","10000","10000","10000","11111"],
    "O": ["01110","10001","10001","10001","10001","10001","01110"],
    "T": ["11111","00100","00100","00100","00100","00100","00100"],
    "H": ["10001","10001","10001","11111","10001","10001","10001"],
    "D": ["11110","10001","10001","10001","10001","10001","11110"],
    "B": ["11110","10001","10001","11110","10001","10001","11110"],
}

WORD = "SLOTHDB"
P = 16                 # pixel block size for the wordmark
LETTER_W = 5 * P       # 80
LETTER_H = 7 * P       # 112
GAP = P                # 16
WIDTH = 1200
HEIGHT = 380

text_width = len(WORD) * LETTER_W + (len(WORD) - 1) * GAP
text_x0 = (WIDTH - text_width) // 2
text_y0 = 30

# Palette
BG_FROM = "#0F0B23"
BG_TO = "#1E1B4B"
GROUND = "#4C1D95"
MAIN = "#C4B5FD"
ACCENT = "#8B5CF6"
SHADOW = "#4C1D95"
SLOTH_BODY = "#8B6F47"
SLOTH_FUR = "#A0826D"
DUCK_BODY = "#FDB515"
DUCK_WING = "#E89F0B"
DUCK_BEAK = "#F97316"
BLACK = "#0F0B23"

# ---- SLOTHDB wordmark ----
wordmark_rects = []
wordmark_shadow = []
for i, ch in enumerate(WORD):
    grid = FONT[ch]
    lx = text_x0 + i * (LETTER_W + GAP)
    for ry, row in enumerate(grid):
        for cx, v in enumerate(row):
            if v == "1":
                px = lx + cx * P
                py = text_y0 + ry * P
                wordmark_shadow.append(f'<rect x="{px+4}" y="{py+4}" width="{P}" height="{P}" fill="{SHADOW}"/>')
                wordmark_rects.append(f'<rect x="{px}" y="{py}" width="{P}" height="{P}" fill="{MAIN}"/>')

# ---- Sloth kicking duck scene (below the wordmark) ----
SCENE_Y = text_y0 + LETTER_H + 40  # 30 + 112 + 40 = 182
GROUND_Y = HEIGHT - 30  # 350

def px_rect(x, y, w, h, color, opacity=1.0):
    op = f' opacity="{opacity}"' if opacity != 1.0 else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{color}"{op}/>'

# Sloth on the left
SX, SY = 260, SCENE_Y + 25  # sloth top-left
S = 5
sloth = []
# Body (torso)
for rx, ry in [
    (2,3),(3,3),(4,3),(5,3),(6,3),(7,3),
    (1,4),(2,4),(3,4),(4,4),(5,4),(6,4),(7,4),(8,4),
    (1,5),(2,5),(3,5),(4,5),(5,5),(6,5),(7,5),(8,5),
    (1,6),(2,6),(3,6),(4,6),(5,6),(6,6),(7,6),(8,6),
    (2,7),(3,7),(4,7),(5,7),(6,7),(7,7),
]:
    sloth.append(px_rect(SX+rx*S, SY+ry*S, S, S, SLOTH_BODY))
# Head
for rx, ry in [
    (3,0),(4,0),(5,0),(6,0),
    (2,1),(3,1),(4,1),(5,1),(6,1),(7,1),
    (2,2),(3,2),(4,2),(5,2),(6,2),(7,2),
]:
    sloth.append(px_rect(SX+rx*S, SY+ry*S, S, S, SLOTH_FUR))
# Eyes (two pixels wide for visibility)
sloth.append(px_rect(SX+3*S, SY+1*S, S, S, BLACK))
sloth.append(px_rect(SX+6*S, SY+1*S, S, S, BLACK))
# Mouth
sloth.append(px_rect(SX+4*S, SY+2*S, S, S, BLACK))
sloth.append(px_rect(SX+5*S, SY+2*S, S, S, BLACK))
# Left arm (hanging)
sloth.append(px_rect(SX+0*S, SY+4*S, S, S, SLOTH_BODY))
sloth.append(px_rect(SX+0*S, SY+5*S, S, S, SLOTH_BODY))
# Feet (standing)
sloth.append(px_rect(SX+3*S, SY+8*S, S, S, SLOTH_BODY))
sloth.append(px_rect(SX+4*S, SY+8*S, S, S, SLOTH_BODY))
sloth.append(px_rect(SX+6*S, SY+8*S, S, S, SLOTH_BODY))
sloth.append(px_rect(SX+7*S, SY+8*S, S, S, SLOTH_BODY))

# Animated kicking arm - rotates around shoulder (right side)
shoulder_cx = SX + 9*S
shoulder_cy = SY + 5*S
kick_arm = f'''
<g id="kickarm">
  <animateTransform attributeName="transform"
    type="rotate"
    values="0 {shoulder_cx} {shoulder_cy}; 0 {shoulder_cx} {shoulder_cy}; -75 {shoulder_cx} {shoulder_cy}; 0 {shoulder_cx} {shoulder_cy}; 0 {shoulder_cx} {shoulder_cy}"
    keyTimes="0; 0.4; 0.5; 0.62; 1"
    dur="4s"
    repeatCount="indefinite"/>
  {px_rect(SX+9*S, SY+4*S, S, S, SLOTH_BODY)}
  {px_rect(SX+9*S, SY+5*S, S, S, SLOTH_BODY)}
  {px_rect(SX+10*S, SY+5*S, S, S, SLOTH_BODY)}
  {px_rect(SX+11*S, SY+5*S, S*2, S, SLOTH_FUR)}
</g>
'''

# Duck in mid-scene; animates away after kick
DX_INIT, DY_INIT = 440, SCENE_Y + 40
D = 4
duck_pixels = []
for rx, ry in [
    (2,3),(3,3),(4,3),(5,3),(6,3),(7,3),
    (1,4),(2,4),(3,4),(4,4),(5,4),(6,4),(7,4),(8,4),
    (1,5),(2,5),(3,5),(4,5),(5,5),(6,5),(7,5),(8,5),
    (2,6),(3,6),(4,6),(5,6),(6,6),(7,6),
]:
    duck_pixels.append(px_rect(rx*D, ry*D, D, D, DUCK_BODY))
for rx, ry in [
    (5,0),(6,0),(7,0),
    (4,1),(5,1),(6,1),(7,1),(8,1),
    (4,2),(5,2),(6,2),(7,2),(8,2),
]:
    duck_pixels.append(px_rect(rx*D, ry*D, D, D, DUCK_BODY))
duck_pixels.append(px_rect(6*D, 1*D, D, D, BLACK))  # eye
duck_pixels.append(px_rect(8*D, 2*D, D*2, D, DUCK_BEAK))  # beak
duck_pixels.append(px_rect(9*D, 3*D, D, D, DUCK_BEAK))
for rx, ry in [(3,4),(4,4),(5,4),(3,5),(4,5),(5,5)]:
    duck_pixels.append(px_rect(rx*D, ry*D, D, D, DUCK_WING))
duck_pixels.append(px_rect(3*D, 7*D, D, D, DUCK_BEAK))
duck_pixels.append(px_rect(6*D, 7*D, D, D, DUCK_BEAK))

# "DuckDB" label below duck (pre-kick only)
duck_label = f'''
<text x="{DX_INIT+22}" y="{DY_INIT+70}" text-anchor="middle"
      font-family="'Courier New', monospace" font-size="10" fill="{DUCK_BEAK}"
      opacity="0.75">
  DuckDB
  <animate attributeName="opacity"
    values="0.75; 0.75; 0.75; 0; 0; 0.75"
    keyTimes="0; 0.48; 0.5; 0.52; 0.97; 1"
    dur="4s"
    repeatCount="indefinite"/>
</text>
'''

duck_group = f'''
<g id="duck">
  <animateTransform attributeName="transform"
    type="translate"
    values="{DX_INIT},{DY_INIT}; {DX_INIT},{DY_INIT}; {DX_INIT+30},{DY_INIT-30}; {WIDTH+80},{-60}; {WIDTH+80},{-60}; {DX_INIT},{DY_INIT}"
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

# Impact lines radiating from duck at kick moment
impact = []
for dx, dy in [(-15, -5), (-10, -15), (5, -20), (15, -10), (20, 5)]:
    impact.append(f'''<line x1="{DX_INIT-5}" y1="{DY_INIT+20}" x2="{DX_INIT-5+dx*1.5}" y2="{DY_INIT+20+dy*1.5}" stroke="{ACCENT}" stroke-width="3" opacity="0"><animate attributeName="opacity" values="0;0;0;1;0;0" keyTimes="0;0.48;0.5;0.53;0.58;1" dur="4s" repeatCount="indefinite"/></line>''')

# POW! at impact
pow_text = f'''
<text x="{DX_INIT+20}" y="{DY_INIT-5}" font-family="'Courier New', monospace"
      font-weight="900" font-size="30" fill="{DUCK_BEAK}" opacity="0"
      transform="rotate(-10 {DX_INIT+20} {DY_INIT-5})">
  POW!
  <animate attributeName="opacity"
    values="0; 0; 0; 1; 1; 0; 0"
    keyTimes="0; 0.48; 0.5; 0.55; 0.65; 0.72; 1"
    dur="4s"
    repeatCount="indefinite"/>
</text>
'''

# Finish line + winner text after duck exits
winner_x = 760
winner_text = f'''
<g opacity="0">
  <animate attributeName="opacity"
    values="0; 0; 0; 0.95; 0.95; 0"
    keyTimes="0; 0.78; 0.8; 0.85; 0.97; 1"
    dur="4s"
    repeatCount="indefinite"/>
  <text x="{winner_x}" y="{SCENE_Y + 40}" font-family="'Courier New', monospace"
        font-weight="bold" font-size="22" fill="{MAIN}" letter-spacing="2">
    6.6x FASTER
  </text>
  <text x="{winner_x}" y="{SCENE_Y + 65}" font-family="'Courier New', monospace"
        font-size="13" fill="{ACCENT}" letter-spacing="1">
    on every benchmark.
  </text>
</g>
'''

# Ground line
ground = f'<rect x="120" y="{GROUND_Y}" width="{WIDTH-240}" height="3" fill="{GROUND}"/>'

# Corner pixel decoration
corners = []
for dx in range(0, 60, 10):
    for yspot in (20, HEIGHT - 28):
        corners.append(px_rect(20+dx, yspot, 6, 6, ACCENT, 0.5))
        corners.append(px_rect(WIDTH-26-dx, yspot, 6, 6, ACCENT, 0.5))

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" preserveAspectRatio="xMidYMid meet">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="0%" y2="100%">
      <stop offset="0%" stop-color="{BG_FROM}"/>
      <stop offset="100%" stop-color="{BG_TO}"/>
    </linearGradient>
    <pattern id="dots" width="24" height="24" patternUnits="userSpaceOnUse">
      <rect width="2" height="2" fill="{ACCENT}" opacity="0.15"/>
    </pattern>
  </defs>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#bg)"/>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#dots)"/>
  {chr(10).join(corners)}
  {chr(10).join(wordmark_shadow)}
  {chr(10).join(wordmark_rects)}
  {ground}
  {chr(10).join(sloth)}
  {kick_arm}
  {duck_group}
  {duck_label}
  {chr(10).join(impact)}
  {pow_text}
  {winner_text}
</svg>
'''

with open("assets/hero.svg", "w", encoding="utf-8") as f:
    f.write(svg)
print(f"Wrote assets/hero.svg")
