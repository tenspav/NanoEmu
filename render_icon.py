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

def create_pixel_screen(w=60, h=104):
    """
    Renders the 60x104 pixel-art home screen with:
    - Status bar (signal, clock, battery)
    - 6 circular app icon dots (2 columns x 3 rows) with tiny retro glyphs
    - Page indicator dots at bottom
    """
    screen = Image.new("RGBA", (w, h), (10, 15, 26, 255))
    draw = ImageDraw.Draw(screen)

    # 1. Status bar
    # Signal bars (left)
    draw.rectangle([4, 8, 5, 9], fill=(0, 240, 255, 255))
    draw.rectangle([7, 6, 8, 9], fill=(0, 240, 255, 255))
    draw.rectangle([10, 4, 11, 9], fill=(0, 240, 255, 255))

    # Time: "9:41" minimal pixel font
    # '9'
    draw.point([(23, 5), (24, 5), (25, 5), (23, 6), (25, 6), (23, 7), (24, 7), (25, 7), (25, 8), (24, 9), (23, 9)], fill=(220, 240, 255, 255))
    # ':'
    draw.point([(27, 6), (27, 8)], fill=(0, 240, 255, 255))
    # '4'
    draw.point([(29, 5), (29, 6), (29, 7), (30, 7), (31, 5), (31, 6), (31, 7), (31, 8), (31, 9)], fill=(220, 240, 255, 255))
    # '1'
    draw.point([(34, 5), (35, 5), (35, 6), (35, 7), (35, 8), (35, 9)], fill=(220, 240, 255, 255))

    # Battery (right)
    # Body 8x5
    draw.rectangle([47, 5, 54, 9], outline=(140, 170, 200, 255))
    # Terminal nub
    draw.rectangle([55, 6, 55, 8], fill=(140, 170, 200, 255))
    # Charge fill (green/cyan)
    draw.rectangle([48, 6, 52, 8], fill=(0, 255, 170, 255))

    # 2. Six App Dots (2 cols x 3 rows)
    # Circle mask helper for 14x14
    def draw_pixel_circle(cx, cy, r, color):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                # Smooth pixel circle equation
                if dx*dx + dy*dy <= (r - 0.2)**2:
                    screen.putpixel((cx + dx, cy + dy), color)

    # 6 icons definition: (col, row, color, glyph_func)
    cols = [18, 42]
    rows = [27, 51, 75]

    # Icon 1: Music (Hot Pink / Magenta)
    draw_pixel_circle(cols[0], rows[0], 6, (255, 45, 85, 255))
    # Music note glyph (white)
    # Stem + note head + flag
    m_pts = [
        (cols[0] + 1, rows[0] - 3), (cols[0] + 2, rows[0] - 3),
        (cols[0] + 1, rows[0] - 2), (cols[0] + 3, rows[0] - 2),
        (cols[0] + 1, rows[0] - 1),
        (cols[0] + 1, rows[0]),
        (cols[0] + 1, rows[0] + 1),
        (cols[0], rows[0] + 2), (cols[0] + 1, rows[0] + 2),
        (cols[0] - 1, rows[0] + 2), (cols[0], rows[0] + 3),
    ]
    for p in m_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # Icon 2: Videos (Indigo / Violet)
    draw_pixel_circle(cols[1], rows[0], 6, (94, 92, 230, 255))
    # Play triangle glyph
    v_pts = [
        (cols[1] - 2, rows[0] - 3),
        (cols[1] - 2, rows[0] - 2), (cols[1] - 1, rows[0] - 2),
        (cols[1] - 2, rows[0] - 1), (cols[1] - 1, rows[0] - 1), (cols[1], rows[0] - 1),
        (cols[1] - 2, rows[0]), (cols[1] - 1, rows[0]), (cols[1], rows[0]), (cols[1] + 1, rows[0]), (cols[1] + 2, rows[0]),
        (cols[1] - 2, rows[0] + 1), (cols[1] - 1, rows[0] + 1), (cols[1], rows[0] + 1),
        (cols[1] - 2, rows[0] + 2), (cols[1] - 1, rows[0] + 2),
        (cols[1] - 2, rows[0] + 3),
    ]
    for p in v_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # Icon 3: Photos (Warm Amber / Gold)
    draw_pixel_circle(cols[0], rows[1], 6, (255, 179, 0, 255))
    # Sun / Flower center glyph
    p_pts = [
        (cols[0], rows[1] - 2),
        (cols[0] - 2, rows[1]), (cols[0] - 1, rows[1]), (cols[0], rows[1]), (cols[0] + 1, rows[1]), (cols[0] + 2, rows[1]),
        (cols[0], rows[1] + 2),
        (cols[0] - 1, rows[1] - 1), (cols[0] + 1, rows[1] - 1),
        (cols[0] - 1, rows[1] + 1), (cols[0] + 1, rows[1] + 1),
    ]
    for p in p_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # Icon 4: Podcasts (Purple)
    draw_pixel_circle(cols[1], rows[1], 6, (175, 82, 222, 255))
    # Broadcast / Antenna glyph
    pod_pts = [
        (cols[1], rows[1] - 1), (cols[1], rows[1]), (cols[1], rows[1] + 1), (cols[1], rows[1] + 2), (cols[1], rows[1] + 3),
        (cols[1] - 2, rows[1] - 2), (cols[1] + 2, rows[1] - 2),
        (cols[1] - 3, rows[1] - 1), (cols[1] + 3, rows[1] - 1),
        (cols[1] - 1, rows[1] + 3), (cols[1] + 1, rows[1] + 3),
    ]
    for p in pod_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # Icon 5: Games / Homebrew (Vivid Emerald / Mint)
    draw_pixel_circle(cols[0], rows[2], 6, (0, 230, 118, 255))
    # Gamepad D-pad '+' glyph
    g_pts = [
        (cols[0], rows[2] - 3), (cols[0], rows[2] - 2),
        (cols[0], rows[2] + 2), (cols[0], rows[2] + 3),
        (cols[0] - 3, rows[2]), (cols[0] - 2, rows[2]), (cols[0] - 1, rows[2]),
        (cols[0], rows[2]),
        (cols[0] + 1, rows[2]), (cols[0] + 2, rows[2]), (cols[0] + 3, rows[2]),
        (cols[0], rows[2] - 1), (cols[0], rows[2] + 1),
    ]
    for p in g_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # Icon 6: Radio / Settings (Electric Cyan / Aqua)
    draw_pixel_circle(cols[1], rows[2], 6, (0, 210, 230, 255))
    # Settings sliders / cog
    set_pts = [
        (cols[1] - 2, rows[2] - 2), (cols[1] - 1, rows[2] - 2), (cols[1], rows[2] - 2), (cols[1] + 1, rows[2] - 2), (cols[1] + 2, rows[2] - 2),
        (cols[1] - 1, rows[2] - 2), (cols[1] - 1, rows[2] - 3),
        (cols[1] - 2, rows[2] + 1), (cols[1] - 1, rows[2] + 1), (cols[1], rows[2] + 1), (cols[1] + 1, rows[2] + 1), (cols[1] + 2, rows[2] + 1),
        (cols[1] + 1, rows[2] + 1), (cols[1] + 1, rows[2] + 2),
    ]
    for p in set_pts:
        screen.putpixel(p, (255, 255, 255, 255))

    # 3. Page indicators
    # Page 1 (active)
    draw.rectangle([27, 93, 29, 95], fill=(240, 250, 255, 255))
    # Page 2 (inactive)
    draw.rectangle([32, 94, 33, 95], fill=(50, 70, 95, 255))

    return screen


def render_icon():
    W, H = 1024, 1024

    # We will build layers on an RGBA canvas
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))

    # 1. Dark Background: Rich deep navy gradient
    # Gradient from top-left (#0A1124) to bottom-right (#05070E)
    bg = Image.new("RGBA", (W, H))
    bg_draw = ImageDraw.Draw(bg)
    for y in range(H):
        t = y / (H - 1)
        r = int(10 * (1 - t) + 4 * t)
        g = int(18 * (1 - t) + 7 * t)
        b = int(36 * (1 - t) + 16 * t)
        bg_draw.line([(0, y), (W, y)], fill=(r, g, b, 255))

    # Subtle ambient cyan/teal radial glow behind the iPod
    glow_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    glow_draw = ImageDraw.Draw(glow_layer)
    # Centered at (512, 480)
    for rad in range(380, 40, -20):
        alpha = int(28 * (1 - rad / 380)**1.5)
        glow_draw.ellipse([512 - rad, 480 - int(rad*1.2), 512 + rad, 480 + int(rad*1.2)],
                          fill=(0, 210, 255, alpha))
    glow_layer = glow_layer.filter(ImageFilter.GaussianBlur(50))
    bg = Image.alpha_composite(bg, glow_layer)

    # 2. iPod nano 7G Silhouette geometry
    # Body dimensions
    body_w = 400
    body_h = 750
    body_r = 52
    bx0 = (W - body_w) // 2   # 312
    bx1 = bx0 + body_w        # 712
    by0 = (H - body_h) // 2   # 137
    by1 = by0 + body_h        # 887

    # Drop shadow under iPod nano
    shadow_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    shadow_draw = ImageDraw.Draw(shadow_layer)
    shadow_draw.rounded_rectangle([bx0 - 4, by0 + 16, bx1 + 4, by1 + 24],
                                  radius=body_r + 4, fill=(0, 0, 0, 150))
    shadow_layer = shadow_layer.filter(ImageFilter.GaussianBlur(28))
    canvas = Image.alpha_composite(bg, shadow_layer)

    # Outer subtle cyan aura / edge glow around the body
    body_glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    body_glow_draw = ImageDraw.Draw(body_glow)
    body_glow_draw.rounded_rectangle([bx0, by0, bx1, by1],
                                     radius=body_r, fill=(0, 235, 255, 90))
    body_glow = body_glow.filter(ImageFilter.GaussianBlur(24))
    canvas = Image.alpha_composite(canvas, body_glow)

    # iPod nano Chassis: Teal / Cyan gradient
    # Top: Vibrant Cyan (#00F0FF) blending to rich deep teal (#007F9E)
    chassis = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    chassis_draw = ImageDraw.Draw(chassis)

    # Create mask for rounded rect
    mask = Image.new("L", (W, H), 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.rounded_rectangle([bx0, by0, bx1, by1], radius=body_r, fill=255)

    # Fill chassis with vertical gradient
    chassis_grad = Image.new("RGBA", (W, H))
    c_draw = ImageDraw.Draw(chassis_grad)
    for y in range(by0, by1 + 1):
        t = (y - by0) / (by1 - by0)
        # Gradient: Mint Aqua (0, 245, 220) -> Electric Cyan (0, 220, 255) -> Deep Teal (0, 120, 150)
        if t < 0.3:
            st = t / 0.3
            r = 0
            g = int(245 * (1 - st) + 225 * st)
            b = int(220 * (1 - st) + 255 * st)
        else:
            st = (t - 0.3) / 0.7
            r = 0
            g = int(225 * (1 - st) + 115 * st)
            b = int(255 * (1 - st) + 150 * st)
        c_draw.line([(bx0, y), (bx1, y)], fill=(r, g, b, 255))

    # Composite gradient through mask
    chassis = Image.composite(chassis_grad, chassis, mask)

    # Subtle inner bevel / rim highlight for precision anodized aluminum feel
    rim = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    rim_draw = ImageDraw.Draw(rim)
    rim_draw.rounded_rectangle([bx0 + 2, by0 + 2, bx1 - 2, by1 - 2],
                               radius=body_r - 2, outline=(200, 255, 255, 75), width=2)
    rim_draw.rounded_rectangle([bx0 + 1, by0 + 1, bx1 - 1, by1 - 1],
                               radius=body_r - 1, outline=(255, 255, 255, 110), width=1)
    chassis = Image.alpha_composite(chassis, rim)
    canvas = Image.alpha_composite(canvas, chassis)

    # 3. Screen Bezel & Display
    # Screen size: 300 x 520 (scaled 5x from 60x104 pixel screen)
    sw, sh = 300, 520
    sx0 = (W - sw) // 2     # 362
    sx1 = sx0 + sw          # 662
    sy0 = by0 + 56          # 193
    sy1 = sy0 + sh          # 713
    screen_r = 16

    # Screen cutout depression / shadow
    screen_outer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    s_draw = ImageDraw.Draw(screen_outer)
    # Dark border frame around screen glass
    s_draw.rounded_rectangle([sx0 - 3, sy0 - 3, sx1 + 3, sy1 + 3],
                             radius=screen_r + 2, fill=(0, 35, 48, 255))
    canvas = Image.alpha_composite(canvas, screen_outer)

    # Render pixel-art home screen at 60x104, then scale up 5x using NEAREST
    pixel_surf = create_pixel_screen(60, 104)
    pixel_surf_scaled = pixel_surf.resize((sw, sh), Image.NEAREST)

    # Screen glass mask
    screen_mask = Image.new("L", (sw, sh), 0)
    sm_draw = ImageDraw.Draw(screen_mask)
    sm_draw.rounded_rectangle([0, 0, sw - 1, sh - 1], radius=screen_r, fill=255)

    # Apply mask to scaled screen
    screen_img = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    screen_img.paste(pixel_surf_scaled, (0, 0), screen_mask)

    # Inner glass reflection overlay (diagonal sleek modern sheen)
    sheen = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    sheen_draw = ImageDraw.Draw(sheen)
    sheen_pts = [(0, 0), (sw, 0), (sw, 110), (0, 220)]
    sheen_draw.polygon(sheen_pts, fill=(255, 255, 255, 14))
    screen_img = Image.alpha_composite(screen_img, sheen)

    # Subtle screen border stroke
    screen_border = Image.new("RGBA", (sw, sh), (0, 0, 0, 0))
    sb_draw = ImageDraw.Draw(screen_border)
    sb_draw.rounded_rectangle([0, 0, sw - 1, sh - 1], radius=screen_r,
                              outline=(0, 200, 230, 80), width=1)
    screen_img = Image.alpha_composite(screen_img, screen_border)

    # Paste screen onto canvas
    canvas.paste(screen_img, (sx0, sy0), screen_img)

    # 4. Circular Home Button
    # Chin area: from sy1 (713) to by1 (887) -> height = 174
    home_cy = sy1 + (by1 - sy1) // 2  # 800
    home_cx = W // 2                  # 512
    home_r = 30

    home_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    h_draw = ImageDraw.Draw(home_layer)

    # Button recess: darker teal/navy recessed circle
    h_draw.ellipse([home_cx - home_r, home_cy - home_r, home_cx + home_r, home_cy + home_r],
                   fill=(0, 65, 82, 230))
    # Soft bevel rim on home button
    h_draw.ellipse([home_cx - home_r, home_cy - home_r, home_cx + home_r, home_cy + home_r],
                   outline=(0, 180, 210, 160), width=2)
    # Bottom subtle highlight
    h_draw.arc([home_cx - home_r + 1, home_cy - home_r + 1, home_cx + home_r - 1, home_cy + home_r - 1],
               start=30, end=150, fill=(150, 255, 255, 180), width=1)

    # Classic iPod nano 7G rounded square / ring icon inside button
    # On 7G, it has a distinct white/cyan rounded ring
    icon_r = 11
    h_draw.rounded_rectangle([home_cx - icon_r, home_cy - icon_r, home_cx + icon_r, home_cy + icon_r],
                             radius=4, outline=(220, 255, 255, 220), width=2)

    canvas = Image.alpha_composite(canvas, home_layer)

    # Ensure output directories exist
    os.makedirs("/home/pavliukov/ipodwork/NanoRework/docs/images", exist_ok=True)

    # Save 1024x1024
    out_1024 = "/home/pavliukov/ipodwork/NanoRework/docs/images/icon.png"
    canvas.save(out_1024, "PNG", optimize=True)
    print(f"Saved {out_1024} (1024x1024)")

    # Save 256x256 downscale
    out_256 = "/home/pavliukov/ipodwork/NanoRework/docs/images/icon-256.png"
    canvas_256 = canvas.resize((256, 256), Image.LANCZOS)
    canvas_256.save(out_256, "PNG", optimize=True)
    print(f"Saved {out_256} (256x256)")

if __name__ == "__main__":
    render_icon()
