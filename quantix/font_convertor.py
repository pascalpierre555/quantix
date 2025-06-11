from PIL import Image, ImageDraw, ImageFont

# Configuration
FONT_PATH = "/home/peng/Downloads/tamzen-font/ttf/Tamzen8x16r.ttf"
FONT_SIZE = 16
CANVAS_WIDTH = 11
CANVAS_HEIGHT = 16
START_CHAR = 32
END_CHAR = 126

# Load font
font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
CHARS = [chr(c) for c in range(START_CHAR, END_CHAR + 1)]

# Get font baseline (ascent)
ascent, descent = font.getmetrics()
baseline = ascent

# Output lines
output_lines = []
offset = 0

for c in CHARS:
    # Create blank canvas
    image = Image.new("L", (CANVAS_WIDTH, CANVAS_HEIGHT), 0)
    draw = ImageDraw.Draw(image)

    # Get character bounding box
    bbox = font.getbbox(c)
    x0, y0, x1, y1 = bbox
    w = x1 - x0
    h = y1 - y0

    # Align character so that its baseline aligns with canvas bottom - descent
    xpos = (CANVAS_WIDTH - w) // 2 - x0
    ypos = CANVAS_HEIGHT - descent - baseline

    # Draw character
    draw.text((xpos, ypos), c, fill=255, font=font)

    # Convert to 2-byte rows
    output_lines.append(f"  // @{offset} '{c}' ({CANVAS_WIDTH} pixels wide)")
    for y in range(CANVAS_HEIGHT):
        byte1 = 0
        byte2 = 0
        for x in range(CANVAS_WIDTH):
            pixel = image.getpixel((x, y))
            if pixel > 128:
                if x < 8:
                    byte1 |= 1 << (7 - x)
                else:
                    byte2 |= 1 << (15 - x)
        output_lines.append(f"  0x{byte1:02X}, 0x{byte2:02X},")
        offset += 2

# Wrap in C array
header = "#include \"fonts.h\" \n const uint8_t Font16_Table[] = \n{\n"
footer = "};\n sFONT Font16 = {Font16_Table, 11,16,};"
c_output = header + "\n".join(output_lines) + "\n" + footer

# Save to file
output_path = "/home/peng/quantix/quantix/components/EPD_2in9/Fonts/font16.c"
with open(output_path, "w") as f:
    f.write(c_output)

output_path
