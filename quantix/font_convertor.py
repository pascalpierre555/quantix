from PIL import Image, ImageDraw, ImageFont
import os

# Configuration
# <--- 請修改為您的 unifont_jp.ttf 路徑
FONT_PATH = "/home/peng/Downloads/unifont_jp-16.0.04.otf"
FONT_SIZE = 16  # 假設 unifont_jp 使用16px大小
# CHARS 列表應包含您想要轉換的日文字符
CHARS = ['あ', 'い', 'う', 'え', 'お', '漢', '字', '!', 'A']  # 示例字符列表

# 輸出C文件名和變量名配置 (可選)
OUTPUT_C_FILENAME_BASE = "unifont_jp"  # 用於生成如 unifont_jp16.c
C_ARRAY_VAR_NAME_BASE = "UnifontJP"  # 用於生成如 UnifontJP16_Table
C_STRUCT_INSTANCE_NAME_BASE = "sUnifontJP"  # 用於生成如 sUnifontJP16
C_STRUCT_TYPE_NAME = "sFONT"  # 在 fonts.h 中定義的結構體類型

# Load font
font = ImageFont.truetype(FONT_PATH, FONT_SIZE)

# Determine actual glyph width and height
ascent, descent = font.getmetrics()
actual_glyph_height = ascent + descent

determined_width = 0
if CHARS:
    # 對於等寬字體，任何字符的寬度應該都一樣
    # 我們取第一個字符的寬度作為代表
    # getlength() 通常給出字符的推進寬度 (advance width)，這對等寬字體來說就是單元格寬度
    try:
        determined_width = font.getlength(CHARS[0])
    except AttributeError:  # Pillow < 8.0.0 的後備方案
        bbox = font.getbbox(CHARS[0])
        determined_width = bbox[2] - bbox[0]  # 使用墨跡寬度
    except Exception:  # 其他可能的錯誤
        bbox = font.getbbox(CHARS[0])
        determined_width = bbox[2] - bbox[0]
else:
    # 如果CHARS為空，嘗試使用 'M' 或空格來獲取寬度
    try:
        determined_width = font.getlength('M')  # 或者使用 ' ' (空格)
    except AttributeError:
        bbox = font.getbbox('M')
        determined_width = bbox[2] - bbox[0]
    except Exception:
        bbox = font.getbbox('M')
        determined_width = bbox[2] - bbox[0]

actual_glyph_width = int(determined_width)
if actual_glyph_width == 0:
    print(
        f"警告: 無法確定字體寬度，可能字體文件 '{FONT_PATH}' 有問題或CHARS列表為空且後備字符缺失。將使用 FONT_SIZE ({FONT_SIZE}) 作為寬度。")
    actual_glyph_width = FONT_SIZE

bytes_per_row = (actual_glyph_width + 7) // 8

print(f"字體: {FONT_PATH}, 大小: {FONT_SIZE}")
print(f"計算出的字形尺寸: 寬度 = {actual_glyph_width}px, 高度 = {actual_glyph_height}px")
print(f"每行字節數: {bytes_per_row}")

# Output lines
output_lines = []
offset = 0

for c in CHARS:
    # Create blank canvas
    image = Image.new(
        "L", (actual_glyph_width, actual_glyph_height), 0)  # 使用實際字形尺寸
    draw = ImageDraw.Draw(image)

    # Draw character
    # 對於等寬字體，且畫布大小即為單元格大小，通常在 (0,0) 繪製即可
    # Pillow的draw.text的(0,0)是基於字體的ascent/descent的佈局框的左上角
    draw.text((0, 0), c, fill=255, font=font)

    output_lines.append(
        f"  // @{offset} '{c}' ({actual_glyph_width} pixels wide, {actual_glyph_height} high)")
    for y in range(actual_glyph_height):
        current_row_bytes = [0] * bytes_per_row
        for x in range(actual_glyph_width):
            pixel = image.getpixel((x, y))
            if pixel > 128:
                byte_index = x // 8
                bit_index_in_byte = 7 - (x % 8)  # MSB first
                current_row_bytes[byte_index] |= (1 << bit_index_in_byte)

        hex_bytes_str = ", ".join([f"0x{b:02X}" for b in current_row_bytes])
        output_lines.append(f"  {hex_bytes_str},")
        offset += bytes_per_row

# Wrap in C array
c_array_name = f"{C_ARRAY_VAR_NAME_BASE}{FONT_SIZE}_Table"
c_struct_instance_name = f"{C_STRUCT_INSTANCE_NAME_BASE}{FONT_SIZE}"

header = f"#include \"fonts.h\" \nconst uint8_t {c_array_name}[] = \n{{\n"
footer = f"}};\n\n{C_STRUCT_TYPE_NAME} {c_struct_instance_name} = {{{c_array_name}, {actual_glyph_width}, {actual_glyph_height},}};"
c_output = header + "\n".join(output_lines) + "\n" + footer

# Save to file
output_c_file_path = f"{OUTPUT_C_FILENAME_BASE}{FONT_SIZE}.c"
# 確保輸出路徑是您期望的，例如與原腳本相同的目錄
# output_c_file_path = os.path.join(os.path.dirname(__file__), f"{OUTPUT_C_FILENAME_BASE}{FONT_SIZE}.c")
# 或者使用原腳本的固定路徑，但文件名動態化
original_dir = "/home/peng/Downloads/unifont_jp_c_files"
if not os.path.exists(original_dir):
    os.makedirs(original_dir, exist_ok=True)  # 如果目錄不存在則創建
output_c_file_path = os.path.join(
    original_dir, f"{OUTPUT_C_FILENAME_BASE}{FONT_SIZE}.c")


with open(output_c_file_path, "w", encoding="utf-8") as f:
    f.write(c_output)

print(f"字體已轉換並保存到: {output_c_file_path}")
