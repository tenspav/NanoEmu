#!/usr/bin/env python3
"""
Render modern app icon for NanoEmu (iPod nano 7G emulator).
Outputs:
  - docs/images/icon.png (1024x1024)
  - docs/images/icon-256.png (256x256)
"""

import math
import os
from PIL import Image, ImageDraw, ImageFilter

def create_pixel_screen(w=80, h=144):
    """
    Renders an 80x144 pixel-art home screen:
    - Exactly 1/3 resolution of the physical iPod nano 7G screen (240x432).
    - Status bar (cellular signal bars, clock '9:41', battery indicator).
    - 6 circular app icon dots (2 columns x 3 rows) scaled to ~30% screen width.
    - Pixel-art app glyphs: Music, Videos, Fitness, Podcasts, Games/Homebrew, Settings.
    - Page indicator dots at bottom.
    """
    screen = Image.new("RGBA", (w, h), (8, 12, 22, 255))
    draw = ImageDraw.Draw(screen)

    # -------------------------------------------------------------
    # 1. STATUS BAR (y = 5 .. 15)
    # -------------------------------------------------------------
    # Cellular signal bars (left, x=6..18)
    cyan_sig = (0, 240, 255, 255)
    dim_sig = (30, 60, 80, 255)
    draw.rectangle([6, 13, 7, 14], fill=cyan_sig)
    draw.rectangle([9, 11, 10, 14], fill=cyan_sig)
    draw.rectangle([12, 9, 13, 14], fill=cyan_sig)
    draw.rectangle([15, 7, 16, 14], fill=cyan_sig)
    draw.rectangle([18, 5, 19, 14], fill=dim_sig)  # 4/5 bars

    # Time: "9:41" centered at x=34..46, y=7..13
    time_color = (235, 248, 255, 255)
    # '9'
    draw.point([(34, 7), (35, 7), (36, 7),
                (34, 8),          (36, 8),
                (34, 9), (35, 9), (36, 9),
                                  (36, 10),
                (34, 11), (35, 11), (36, 11)], fill=time_color)
    # ':'
    draw.point([(38, 8), (38, 10)], fill=(0, 230, 255, 255))
    # '4'
    draw.point([(40, 7),          (42, 7),
                (40, 8),          (42, 8),
                (40, 9), (41, 9), (42, 9),
                                  (42, 10),
                                  (42, 11)], fill=time_color)
    # '1'
    draw.point([(44, 8), (45, 7), (45, 8), (45, 9), (45, 10), (45, 11),
                (44, 11), (46, 11)], fill=time_color)

    # Battery (right, x=62..74, y=7..13)
    # Outer frame
    draw.rectangle([62, 7, 72, 13], outline=(140, 175, 205, 255))
    # Terminal nub
    draw.rectangle([73, 9, 73, 11], fill=(140, 175, 205, 255))
    # Battery fill: 80% charge (bright green)
    draw.rectangle([64, 9, 70, 11], fill=(0, 245, 140, 255))

    # Subtle status bar separator line
    draw.line([(4, 18), (w - 5, 18)], fill=(20, 32, 50, 255))

    # -------------------------------------------------------------
    # 2. SIX CIRCULAR APP ICONS (2 cols x 3 rows)
    # -------------------------------------------------------------
    # Diameter = 24 pixels (radius 12), col centers: 24, 56; row centers: 39, 75, 111
    cols = [24, 56]
    rows = [39, 75, 111]
    radius = 11.5

    def draw_app_circle(cx, cy, base_color, highlight_color=None):
        """Draws a smooth pixelated circle with optional top subtle highlight."""
        for dy in range(-12, 13):
            for dx in range(-12, 13):
                dist_sq = dx*dx + dy*dy
                if dist_sq <= radius**2:
                    c = base_color
                    # Subtle top highlight rim for flat dimensional pop
                    if highlight_color and dy < -4 and dist_sq <= (radius - 1)**2:
                        c = highlight_color
                    screen.putpixel((cx + dx, cy + dy), c)

    # Helper to set glyph pixels
    def draw_glyph_pixels(pts, color=(255, 255, 255, 255)):
        for x, y in pts:
            if 0 <= x < w and 0 <= y < h:
                screen.putpixel((x, y), color)

    # --- Icon 1: Music (Hot Coral/Pink) ---
    c_music = (255, 45, 85, 255)
    c_music_hi = (255, 90, 120, 255)
    draw_app_circle(cols[0], rows[0], c_music, c_music_hi)
    # Connected eighth notes glyph
    cx, cy = cols[0], rows[0]
    music_pts = [
        # Top beam
        (cx - 4, cy - 5), (cx - 3, cy - 5), (cx - 2, cy - 5), (cx - 1, cy - 5),
        (cx, cy - 5), (cx + 1, cy - 5), (cx + 2, cy - 5), (cx + 3, cy - 5), (cx + 4, cy - 5),
        (cx - 4, cy - 4), (cx - 3, cy - 4), (cx - 2, cy - 4), (cx - 1, cy - 4),
        (cx, cy - 4), (cx + 1, cy - 4), (cx + 2, cy - 4), (cx + 3, cy - 4), (cx + 4, cy - 4),
        # Left stem
        (cx - 4, cy - 3), (cx - 4, cy - 2), (cx - 4, cy - 1), (cx - 4, cy),
        (cx - 4, cy + 1), (cx - 4, cy + 2),
        # Left note head
        (cx - 6, cy + 2), (cx - 5, cy + 2), (cx - 4, cy + 2),
        (cx - 7, cy + 3), (cx - 6, cy + 3), (cx - 5, cy + 3), (cx - 4, cy + 3),
        (cx - 6, cy + 4), (cx - 5, cy + 4),
        # Right stem
        (cx + 4, cy - 3), (cx + 4, cy - 2), (cx + 4, cy - 1), (cx + 4, cy),
        (cx + 4, cy + 1), (cx + 4, cy + 2), (cx + 4, cy + 3), (cx + 4, cy + 4),
        # Right note head
        (cx + 2, cy + 4), (cx + 3, cy + 4), (cx + 4, cy + 4),
        (cx + 1, cy + 5), (cx + 2, cy + 5), (cx + 3, cy + 5), (cx + 4, cy + 5),
        (cx + 2, cy + 6), (cx + 3, cy + 6)
    ]
    draw_glyph_pixels(music_pts)

    # --- Icon 2: Videos (Electric Indigo / Violet) ---
    c_vid = (94, 92, 230, 255)
    c_vid_hi = (130, 128, 250, 255)
    draw_app_circle(cols[1], rows[0], c_vid, c_vid_hi)
    # Clean play triangle glyph ▶
    cx, cy = cols[1], rows[0]
    vid_pts = []
    # Triangle points pointing right from x = cx-3 to cx+4
    for dy in range(-5, 6):
        width = 6 - abs(dy)
        for dx in range(width):
            vid_pts.append((cx - 3 + dx, cy + dy))
    draw_glyph_pixels(vid_pts)

    # --- Icon 3: Fitness / Pulse (Vivid Orange) ---
    c_fit = (255, 149, 0, 255)
    c_fit_hi = (255, 180, 50, 255)
    draw_app_circle(cols[0], rows[1], c_fit, c_fit_hi)
    # Heart rate / ECG pulse line glyph
    cx, cy = cols[0], rows[1]
    fit_pts = [
        (cx - 7, cy), (cx - 6, cy), (cx - 5, cy),
        (cx - 4, cy), (cx - 4, cy - 2),
        (cx - 3, cy - 4), (cx - 3, cy - 5),
        (cx - 2, cy - 3), (cx - 2, cy - 1), (cx - 2, cy + 1), (cx - 2, cy + 3),
        (cx - 1, cy + 5), (cx - 1, cy + 6),
        (cx, cy + 4), (cx, cy + 2), (cx, cy),
        (cx + 1, cy - 2), (cx + 1, cy - 4), (cx + 1, cy - 6),
        (cx + 2, cy - 3), (cx + 2, cy - 1),
        (cx + 3, cy), (cx + 4, cy), (cx + 5, cy), (cx + 6, cy), (cx + 7, cy)
    ]
    draw_glyph_pixels(fit_pts)

    # --- Icon 4: Podcasts (Rich Purple) ---
    c_pod = (175, 82, 222, 255)
    c_pod_hi = (205, 120, 245, 255)
    draw_app_circle(cols[1], rows[1], c_pod, c_pod_hi)
    # Radio tower / microphone waves glyph
    cx, cy = cols[1], rows[1]
    pod_pts = [
        # Center mic/dot
        (cx - 1, cy - 1), (cx, cy - 1), (cx + 1, cy - 1),
        (cx - 1, cy),     (cx, cy),     (cx + 1, cy),
        (cx, cy + 1), (cx, cy + 2), (cx, cy + 3), (cx, cy + 4), (cx, cy + 5),
        (cx - 2, cy + 5), (cx - 1, cy + 5), (cx, cy + 5), (cx + 1, cy + 5), (cx + 2, cy + 5),
        # Inner wave arc
        (cx - 3, cy - 2), (cx - 4, cy - 1), (cx - 4, cy), (cx - 3, cy + 1),
        (cx + 3, cy - 2), (cx + 4, cy - 1), (cx + 4, cy), (cx + 3, cy + 1),
        # Outer wave arc
        (cx - 6, cy - 3), (cx - 7, cy - 2), (cx - 7, cy - 1), (cx - 7, cy), (cx - 7, cy + 1), (cx - 6, cy + 2),
        (cx + 6, cy - 3), (cx + 7, cy - 2), (cx + 7, cy - 1), (cx + 7, cy), (cx + 7, cy + 1), (cx + 6, cy + 2),
    ]
    draw_glyph_pixels(pod_pts)

    # --- Icon 5: Games / Homebrew (Vivid Emerald Mint) ---
    # NanoEmu runs GBA / homebrew: a classic D-pad glyph!
    c_game = (0, 230, 118, 255)
    c_game_hi = (70, 255, 170, 255)
    draw_app_circle(cols[0], rows[2], c_game, c_game_hi)
    cx, cy = cols[0], rows[2]
    game_pts = []
    # Cross / D-pad
    for dx in range(-6, 7):
        for dy in range(-2, 3):
            game_pts.append((cx + dx, cy + dy))
    for dx in range(-2, 3):
        for dy in range(-6, 7):
            game_pts.append((cx + dx, cy + dy))
    draw_glyph_pixels(game_pts)
    # Center depression dot on d-pad
    screen.putpixel((cx, cy), (0, 180, 80, 255))

    # --- Icon 6: Radio / Settings (Electric Aqua / Cyan) ---
    c_set = (0, 210, 235, 255)
    c_set_hi = (90, 240, 255, 255)
    draw_app_circle(cols[1], rows[2], c_set, c_set_hi)
    cx, cy = cols[1], rows[2]
    # Retro radio tuner / settings cog glyph
    set_pts = [
        # Gear body
        (cx - 3, cy - 3), (cx - 2, cy - 3), (cx - 1, cy - 3), (cx, cy - 3), (cx + 1, cy - 3), (cx + 2, cy - 3), (cx + 3, cy - 3),
        (cx - 4, cy - 2), (cx + 4, cy - 2),
        (cx - 4, cy - 1), (cx + 4, cy - 1),
        (cx - 4, cy),     (cx + 4, cy),
        (cx - 4, cy + 1), (cx + 4, cy + 1),
        (cx - 4, cy + 2), (cx + 4, cy + 2),
        (cx - 3, cy + 3), (cx - 2, cy + 3), (cx - 1, cy + 3), (cx, cy + 3), (cx + 1, cy + 3), (cx + 2, cy + 3), (cx + 3, cy + 3),
        # Gear teeth
        (cx - 1, cy - 5), (cx, cy - 5), (cx + 1, cy - 5),
        (cx - 1, cy + 5), (cx, cy + 5), (cx + 1, cy + 5),
        (cx - 5, cy - 1), (cx - 5, cy), (cx - 5, cy + 1),
        (cx + 5, cy - 1), (cx + 5, cy), (cx + 5, cy + 1),
    ]
    draw_glyph_pixels(set_pts)
    # Center hole in gear
    for dx in range(-1, 2):
        for dy in range(-1, 2):
            screen.putpixel((cx + dx, cy + dy), (0, 160, 190, 255))

    # -------------------------------------------------------------
    # 3. PAGE INDICATOR DOTS (y = 132..134)
    # -------------------------------------------------------------
    # Page 1 active (bright white 3x3)
    draw.rectangle([36, 131, 38, 133], fill=(255, 255, 255, 255))
    # Page 2 inactive (dim slate 3x3)
    draw.rectangle([42, 131, 44, 133], fill=(45, 65, 88, 255))

    return screen


def render_icon():
    W, H = 1024, 1024

    # -------------------------------------------------------------
    # BACKGROUND: Deep Navy / Midnight Obsidian Radial Vignette
    # -------------------------------------------------------------
    bg = Image.new("RGBA", (W, H))
    bg_draw = ImageDraw.Draw(bg)

    # Base subtle dark vertical gradient
    for y in range(H):
        t = y / (H - 1)
        r = int(12 * (1 - t) + 4 * t)
        g = int(20 * (1 - t) + 7 * t)
        b = int(38 * (1 - t) + 16 * t)
        bg_draw.line([(0, y), (W, y)], fill=(r, g, b, 255))

    # Ambient deep radial warmth behind device
    radial_glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rad_draw = ImageDraw.Draw(radial_glow)
    for rad in range(500, 50, -25):
        factor = (1.0 - rad / 500) ** 1.8
        alpha = int(45 * factor)
        # Deep cyan / midnight navy luminescence
        rad_draw.ellipse([512 - rad, 500 - int(rad * 1.1),
                          512 + rad, 500 + int(rad * 1.1)],
                         fill=(0, 180, 230, alpha))
    radial_glow = radial_glow.filter(ImageFilter.GaussianBlur(60))
    bg = Image.alpha_composite(bg, radial_glow)

    # -------------------------------------------------------------
    # IPOD NANO 7G SILHOUETTE & DIMENSIONS
    # -------------------------------------------------------------
    # True proportions: device is tall & slim (1 : 1.83 ratio)
    body_w = 420
    body_h = 770
    body_r = 54
    bx0 = (W - body_w) // 2   # 302
    bx1 = bx0 + body_w        # 722
    by0 = (H - body_h) // 2   # 127
    by1 = by0 + body_h        # 897

    # 1. Outer ambient diffuse glow (wide, soft aura)
    wide_aura = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    w_draw = ImageDraw.Draw(wide_aura)
    w_draw.rounded_rectangle([bx0 - 10, by0 - 10, bx1 + 10, by1 + 10],
                             radius=body_r + 10, fill=(0, 220, 255, 30))
    wide_aura = wide_aura.filter(ImageFilter.GaussianBlur(70))
    canvas = Image.alpha_composite(bg, wide_aura)

    # 2. Drop shadow under device body (gives grounded floating presence)
    drop_shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ds_draw = ImageDraw.Draw(drop_shadow)
    ds_draw.rounded_rectangle([bx0, by0 + 20, bx1, by1 + 26],
                              radius=body_r, fill=(0, 0, 0, 180))
    drop_shadow = drop_shadow.filter(ImageFilter.GaussianBlur(28))
    canvas = Image.alpha_composite(canvas, drop_shadow)

    # 3. Vivid Cyan Rim Glow (tighter edge glow)
    edge_glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    eg_draw = ImageDraw.Draw(edge_glow)
    eg_draw.rounded_rectangle([bx0, by0, bx1, by1],
                              radius=body_r, fill=(0, 240, 255, 80))
    edge_glow = edge_glow.filter(ImageFilter.GaussianBlur(20))
    canvas = Image.alpha_composite(canvas, edge_glow)

    # 4. Anodized Aluminum Chassis (Teal / Cyan Gradient)
    chassis_mask = Image.new("L", (W, H), 0)
    cm_draw = ImageDraw.Draw(chassis_mask)
    cm_draw.rounded_rectangle([bx0, by0, bx1, by1], radius=body_r, fill=255)

    chassis_grad = Image.new("RGBA", (W, H))
    cg_draw = ImageDraw.Draw(chassis_grad)
    for y in range(by0, by1 + 1):
        t = (y - by0) / (by1 - by0)
        # Modern cyan spectrum:
        # Top: Radiant Aqua (#00F8E2)
        # Mid: Electric Cyan (#00D2F5)
        # Bottom: Deep Anodized Teal (#007A9E)
        if t < 0.35:
            st = t / 0.35
            r = 0
            g = int(248 * (1 - st) + 210 * st)
            b = int(226 * (1 - st) + 245 * st)
        else:
            st = (t - 0.35) / 0.65
            r = 0
            g = int(210 * (1 - st) + 122 * st)
            b = int(245 * (1 - st) + 158 * st)
        cg_draw.line([(bx0, y), (bx1, y)], fill=(r, g, b, 255))

    chassis = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    chassis = Image.composite(chassis_grad, chassis, chassis_mask)

    # Chamfer / rim bevel highlight on the aluminum edge
    rim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    r_draw = ImageDraw.Draw(rim)
    # Bright top highlight
    r_draw.rounded_rectangle([bx0 + 2, by0 + 2, bx1 - 2, by1 - 2],
                             radius=body_r - 2, outline=(230, 255, 255, 90), width=2)
    # Subtle inner bevel
    r_draw.rounded_rectangle([bx0 + 4, by0 + 4, bx1 - 4, by1 - 4],
                             radius=body_r - 4, outline=(0, 240, 255, 40), width=1)
    chassis = Image.alpha_composite(chassis, rim)
    canvas = Image.alpha_composite(canvas, chassis)

    # -------------------------------------------------------------
    # 5. SCREEN DISPLAY (320 x 576, 4x scaled from 80x144)
    # -------------------------------------------------------------
    # Screen width = 320 px (76.2% of body width, true to nano 7G)
    # Screen height = 576 px (exact 9:16 aspect ratio, identical to 240x432)
    sw, sh = 320, 576
    sx0 = (W - sw) // 2     # 352
    sx1 = sx0 + sw          # 672
    sy0 = by0 + 46          # 173 (clean, slim top bezel)
    sy1 = sy0 + sh          # 749
    screen_r = 16

    # Screen bezel recess / dark outer housing
    bezel_frame = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    bf_draw = ImageDraw.Draw(bezel_frame)
    bf_draw.rounded_rectangle([sx0 - 4, sy0 - 4, sx1 + 4, sy1 + 4],
                              radius=screen_r + 3, fill=(0, 25, 36, 255))
    canvas = Image.alpha_composite(canvas, bezel_frame)

    # Render pixel-art home screen at 80x144, scale 4x using NEAREST
    pixel_surf = create_pixel_screen(80, 144)
    pixel_surf_scaled = pixel_surf.resize((sw, sh), Image.NEAREST)

    # Screen corner rounded mask
    screen_mask = Image.new("L", (sw, sh), 0)
    sm_draw = ImageDraw.Draw(screen_mask)
    sm_draw.rounded_rectangle([0, 0, sw - 1, sh - 1], radius=screen_r, fill=255)

    screen_img = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    screen_img.paste(pixel_surf_scaled, (0, 0), screen_mask)

    # Glossy glass reflection (diagonal sheen)
    sheen = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    sheen_draw = ImageDraw.Draw(sheen)
    sheen_pts = [(0, 0), (sw, 0), (sw, 120), (0, 240)]
    sheen_draw.polygon(sheen_pts, fill=(255, 255, 255, 12))
    screen_img = Image.alpha_composite(screen_img, sheen)

    # Crisp glass edge border
    glass_border = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    gb_draw = ImageDraw.Draw(glass_border)
    gb_draw.rounded_rectangle([0, 0, sw - 1, sh - 1], radius=screen_r,
                              outline=(0, 220, 255, 60), width=1)
    screen_img = Image.alpha_composite(screen_img, glass_border)

    # Paste screen onto canvas
    canvas.paste(screen_img, (sx0, sy0), screen_img)

    # -------------------------------------------------------------
    # 6. CIRCULAR HOME BUTTON (iPod nano 7G iconic design)
    # -------------------------------------------------------------
    # Located in chin below screen: sy1 (749) to by1 (897), height = 148 px
    home_cy = sy1 + (by1 - sy1) // 2  # 823
    home_cx = W // 2                  # 512
    home_outer_r = 30                 # Outer button circle radius

    home_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    h_draw = ImageDraw.Draw(home_layer)

    # Recessed button body (deep teal/navy)
    h_draw.ellipse([home_cx - home_outer_r, home_cy - home_outer_r,
                    home_cx + home_outer_r, home_cy + home_outer_r],
                   fill=(0, 52, 68, 235))

    # Outer bevel rim around button
    h_draw.ellipse([home_cx - home_outer_r, home_cy - home_outer_r,
                    home_cx + home_outer_r, home_cy + home_outer_r],
                   outline=(0, 190, 220, 170), width=2)

    # Subtle bottom shadow on recess
    h_draw.arc([home_cx - home_outer_r + 1, home_cy - home_outer_r + 1,
                home_cx + home_outer_r - 1, home_cy + home_outer_r - 1],
               start=30, end=150, fill=(180, 255, 255, 200), width=1)

    # On iPod nano 7G: Circular ring glyph (NOT a square!)
    ring_r = 12
    h_draw.ellipse([home_cx - ring_r, home_cy - ring_r,
                    home_cx + ring_r, home_cy + ring_r],
                   outline=(230, 252, 255, 230), width=3)

    canvas = Image.alpha_composite(canvas, home_layer)

    # -------------------------------------------------------------
    # 7. SAVE OUTPUTS (1024x1024 and 256x256)
    # -------------------------------------------------------------
    os.makedirs("/home/pavliukov/ipodwork/NanoRework/docs/images", exist_ok=True)

    out_1024 = "/home/pavliukov/ipodwork/NanoRework/docs/images/icon.png"
    canvas.save(out_1024, "PNG", optimize=True)
    print(f"Saved {out_1024} (1024x1024)")

    out_256 = "/home/pavliukov/ipodwork/NanoRework/docs/images/icon-256.png"
    canvas_256 = canvas.resize((256, 256), Image.LANCZOS)
    canvas_256.save(out_256, "PNG", optimize=True)
    print(f"Saved {out_256} (256x256)")


if __name__ == "__main__":
    render_icon()
