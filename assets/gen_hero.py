"""Generate assets/hero.svg — a pixel-art 'SLOTHDB' banner."""

FONT = {
    "S": [
        "01111",
        "10000",
        "10000",
        "01110",
        "00001",
        "00001",
        "11110",
    ],
    "L": [
        "10000",
        "10000",
        "10000",
        "10000",
        "10000",
        "10000",
        "11111",
    ],
    "O": [
        "01110",
        "10001",
        "10001",
        "10001",
        "10001",
        "10001",
        "01110",
    ],
    "T": [
        "11111",
        "00100",
        "00100",
        "00100",
        "00100",
        "00100",
        "00100",
    ],
    "H": [
        "10001",
        "10001",
        "10001",
        "11111",
        "10001",
        "10001",
        "10001",
    ],
    "D": [
        "11110",
        "10001",
        "10001",
        "10001",
        "10001",
        "10001",
        "11110",
    ],
    "B": [
        "11110",
        "10001",
        "10001",
        "11110",
        "10001",
        "10001",
        "11110",
    ],
}

WORD = "SLOTHDB"
P = 18               # pixel size
LETTER_W = 5 * P     # 90
LETTER_H = 7 * P     # 126
GAP = P              # 18
WIDTH = 1200
HEIGHT = 300

text_width = len(WORD) * LETTER_W + (len(WORD) - 1) * GAP
text_x0 = (WIDTH - text_width) // 2
text_y0 = 60

MAIN = "#C4B5FD"        # light purple
ACCENT = "#8B5CF6"      # vivid purple
SHADOW = "#4C1D95"      # deep purple for shadow
BG_FROM = "#0F0B23"
BG_TO = "#1E1B4B"

rects = []
shadow_rects = []

for i, ch in enumerate(WORD):
    grid = FONT[ch]
    lx = text_x0 + i * (LETTER_W + GAP)
    for ry, row in enumerate(grid):
        for cx, v in enumerate(row):
            if v == "1":
                px = lx + cx * P
                py = text_y0 + ry * P
                shadow_rects.append(f'<rect x="{px+4}" y="{py+4}" width="{P}" height="{P}" fill="{SHADOW}"/>')
                rects.append(f'<rect x="{px}" y="{py}" width="{P}" height="{P}" fill="{MAIN}"/>')

# Retro corner pixel decoration
decor = []
for x in range(20, 120, 12):
    decor.append(f'<rect x="{x}" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>')
    decor.append(f'<rect x="{WIDTH-x-8}" y="20" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>')
    decor.append(f'<rect x="{x}" y="{HEIGHT-28}" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>')
    decor.append(f'<rect x="{WIDTH-x-8}" y="{HEIGHT-28}" width="8" height="8" fill="{ACCENT}" opacity="0.6"/>')

tagline_y = text_y0 + LETTER_H + 40

svg = f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" preserveAspectRatio="xMidYMid meet">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="{BG_FROM}"/>
      <stop offset="100%" stop-color="{BG_TO}"/>
    </linearGradient>
    <pattern id="dots" x="0" y="0" width="24" height="24" patternUnits="userSpaceOnUse">
      <rect x="0" y="0" width="2" height="2" fill="{ACCENT}" opacity="0.15"/>
    </pattern>
  </defs>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#bg)"/>
  <rect width="{WIDTH}" height="{HEIGHT}" fill="url(#dots)"/>
  {chr(10).join(decor)}
  {chr(10).join(shadow_rects)}
  {chr(10).join(rects)}
  <text x="{WIDTH//2}" y="{tagline_y}" text-anchor="middle" font-family="'Courier New', monospace" font-size="20" fill="{MAIN}" letter-spacing="3" font-weight="bold">EMBEDDED  ·  COLUMNAR  ·  1.1x-6.6x FASTER THAN DUCKDB</text>
</svg>
'''

with open("assets/hero.svg", "w", encoding="utf-8") as f:
    f.write(svg)

print(f"Wrote assets/hero.svg ({len(rects)} pixels)")
