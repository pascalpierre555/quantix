from flask import Flask, request, jsonify, render_template_string, g, Response
import jwt                       # 🔹 pyjwt 套件，用來產生/解析 token
import datetime                  # 🔹 處理過期時間
from functools import wraps     # 🔹 保留函式原名的裝飾器工具
from dotenv import load_dotenv
import finnhub
import requests                 # 🔹 用來發 HTTP 請求
import secrets
import time
import qrcode
import os
import json  # 載入/儲存 authorized_users 使用
from PIL import Image, ImageDraw, ImageFont
import base64
import numpy as np


def generate_session_token():
    return secrets.token_urlsafe(32)  # 一個很難猜的 token


AUTHORIZED_USERS_FILE = "authorized_users.json"
# 暫存 session tokens：key = token, value = 到期時間
session_tokens = {}
# key: username, value: dict (session_token, google info)
# authorized_users 將由 load_authorized_users 初始化


def load_authorized_users():
    global authorized_users
    try:
        if os.path.exists(AUTHORIZED_USERS_FILE):
            with open(AUTHORIZED_USERS_FILE, 'r') as f:
                data = json.load(f)
                if isinstance(data, dict):
                    authorized_users = data
                    return
        authorized_users = {}
    except (IOError, json.JSONDecodeError) as e:
        print(f"錯誤：無法載入 {AUTHORIZED_USERS_FILE}: {e}。將使用空的 authorized_users。")
        authorized_users = {}


def save_authorized_users():
    with open(AUTHORIZED_USERS_FILE, 'w') as f:
        json.dump(authorized_users, f, indent=4)


def store_session_token(token, valid_seconds=300):
    expires_at = time.time() + valid_seconds
    session_tokens[token] = expires_at


def is_token_valid(token):
    if token in session_tokens:
        if time.time() < session_tokens[token]:
            return True
        else:
            del session_tokens[token]  # 過期就刪
    return False


def qr_to_c_array(data, box_size=1, border=0):
    qr = qrcode.QRCode(
        version=4,
        error_correction=qrcode.constants.ERROR_CORRECT_L,
        box_size=box_size,
        border=border,
    )
    qr.add_data(data)
    qr.make(fit=True)
    img = qr.make_image(fill_color="black", back_color="white").convert('1')
    arr = np.array(img)

    # 轉成每8個bit一個byte
    h, w = arr.shape
    bytes_per_row = (w + 7) // 8
    c_array = []
    for y in range(h):
        row = arr[y]
        for b in range(bytes_per_row):
            byte = 0
            for bit in range(8):
                x = b * 8 + bit
                if x < w and row[x] == 0:  # 黑點為0
                    byte |= (1 << (7 - bit))
            c_array.append(byte)
    return c_array, w, h


app = Flask(__name__)

authorized_users = {}  # 先定義
load_authorized_users()  # 再從檔案載入

load_dotenv()
SECRET_KEY = os.getenv('SECRET_KEY')
QUANTIX_USERNAME = os.getenv('QUANTIX_USERNAME')
QUANTIX_PASSWORD = os.getenv('QUANTIX_PASSWORD')


def token_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        token = request.headers.get('Authorization')
        if not token or not token.startswith("Bearer "):
            return jsonify({'error': 'Token missing'}), 403

        try:
            token = token.split(" ")[1]
            payload = jwt.decode(token, SECRET_KEY, algorithms=['HS256'])
            g.current_user = payload.get('user')
        except jwt.ExpiredSignatureError:
            return jsonify({'error': 'Token expired'}), 401
        except jwt.InvalidTokenError:
            return jsonify({'error': 'Invalid token'}), 403

        return f(*args, **kwargs)
    return decorated


def ensure_google_token_valid(username):
    google_info = authorized_users.get(username, {}).get('google')
    if not google_info:
        print(f"No Google token found for {username}")
        return False
    # 檢查是否過期
    if time.time() > google_info.get('expires_at', 0):
        # 嘗試 refresh
        if not google_info.get('refresh_token'):
            return False
        response = requests.post(
            "https://oauth2.googleapis.com/token",
            data={
                "client_id": os.getenv("GOOGLE_CLIENT_ID"),
                "client_secret": os.getenv("GOOGLE_CLIENT_SECRET"),
                "refresh_token": google_info['refresh_token'],
                "grant_type": "refresh_token"
            }
        )
        print(
            f"Refreshing token for {username}, status: {response.status_code}")
        if response.status_code == 200:
            print(f"Token refresh response: {response.text}")
            token_data = response.json()
            google_info['access_token'] = token_data['access_token']
            google_info['expires_at'] = time.time(
            ) + token_data.get("expires_in", 3600)
            save_authorized_users()  # 持久化更新後的 token
            print(f"Token refreshed for {username}")
            return True
        else:
            print(
                f"Failed to refresh token for {username}, status: {response.status_code}, detail: {response.text}")
            return False
    return True


def ensure_valid_google_token(f):
    @wraps(f)
    def wrapper(*args, **kwargs):
        username = kwargs.get('username') or g.get('current_user')
        if not username:
            return jsonify({'error': 'No username found'}), 400
        if not ensure_google_token_valid(username):
            return jsonify({'error': 'Google token invalid or refresh failed'}), 401
        return f(*args, **kwargs)
    return wrapper


def generate_font_char_data_dict(text_to_convert: str, font_size: int):
    """
    將指定 TrueType 字體文件中的字符轉換為一個字典，
    其中鍵是字符的 UTF-8 十六進制表示，值是其字形數據的字節列表。

    參數:
        text_to_convert (str): 包含所有需要轉換字符的 UTF-8 十六進制編碼串接字符串 (例如 "e4bda0e5a5bde4b896e7958c" 代表 "你好世界")。
        font_size (int): 要使用的字體大小（像素）。


    返回:
        dict | None: 一個字典，格式為 {"utf8_hex_char": [byte_data_list]}，
                     例如 {"e4bda0": [0x20, 0x88, ...]}。
                     如果字體加載失敗或發生嚴重錯誤，則返回 None。
    """
    # 1. 將font_path寫死
    font_path = "./unifont_jp-16.0.04.otf"

    if not os.path.exists(font_path):
        print(f"錯誤: 字體文件未找到於 '{font_path}'")
        return None

    try:
        font = ImageFont.truetype(font_path, font_size)
    except IOError:
        print(f"錯誤: 無法加載字體 '{font_path}'. 請確保它是有效的 TTF/OTF 文件。")
        return None
    except Exception as e:
        print(f"加載字體時發生未知錯誤: {e}")
        return None

    # Determine actual glyph width and height
    ascent, descent = font.getmetrics()
    actual_glyph_height = ascent + descent

    determined_width = 0
    char_to_measure = 'M'  # Default character for measurement

    if text_to_convert:
        first_hex_sequence = text_to_convert[0:6]
        if len(first_hex_sequence) == 6:
            try:
                first_char_bytes = bytes.fromhex(first_hex_sequence)
                char_to_measure = first_char_bytes.decode('utf-8')
            except (ValueError, UnicodeDecodeError) as e:
                print(
                    f"警告: 無法解碼第一個十六進制序列 '{first_hex_sequence}': {e}。將使用 'M' 進行寬度測量。")
        else:
            print(f"警告: text_to_convert 開頭不是一個完整的6字符十六進制序列。將使用 'M' 進行寬度測量。")

    # Measure width using char_to_measure
    try:
        # 對於等寬字體，任何字符的寬度應該都一樣
        # getlength() 通常給出字符的推進寬度 (advance width)
        determined_width = font.getlength(char_to_measure)
    except AttributeError:  # Pillow < 8.0.0 的後備方案
        bbox = font.getbbox(char_to_measure)
        determined_width = bbox[2] - bbox[0]  # 使用墨跡寬度
    except Exception as e:  # 其他可能的錯誤
        print(f"獲取字符 '{char_to_measure}' 寬度時出錯: {e}")
        # 再次嘗試獲取 'M' 的邊界框作為最終後備
        try:
            bbox = font.getbbox('M')
            determined_width = bbox[2] - bbox[0]
        except Exception as e_fallback:
            print(f"獲取 'M' 字符寬度作為後備時也出錯: {e_fallback}")
            # 如果一切都失敗了，就用 font_size
            determined_width = font_size

    actual_glyph_width = int(determined_width)
    if actual_glyph_width == 0:
        print(
            f"警告: 無法確定字體寬度。將使用 FONT_SIZE ({font_size}) 作為寬度。")
        actual_glyph_width = font_size

    if actual_glyph_width <= 0:  # 確保寬度為正
        print(f"錯誤: 計算出的字形寬度 ({actual_glyph_width}) 無效。")
        return None
    if actual_glyph_height <= 0:  # 確保高度為正
        print(f"錯誤: 計算出的字形高度 ({actual_glyph_height}) 無效。")
        return None

    bytes_per_row = (actual_glyph_width + 7) // 8

    print(f"字體: {font_path}, 大小: {font_size}")
    print(
        f"計算出的字形尺寸: 寬度 = {actual_glyph_width}px, 高度 = {actual_glyph_height}px")
    print(f"每行字節數: {bytes_per_row}")

    char_data_map = {}

    if not text_to_convert:
        print("警告: text_to_convert 字符串為空，將生成空的字體數據字典。")
        return {}  # 返回空字典

    # 以6個字符為一組（一個UTF-8字符的hex表示）進行迭代
    for i in range(0, len(text_to_convert), 6):
        hex_key = text_to_convert[i:i+6]

        if len(hex_key) < 6:
            print(f"警告: 發現不完整的十六進制序列 '{hex_key}'，將跳過。")
            continue

        try:
            char_bytes_to_render = bytes.fromhex(hex_key)
            char_code_to_render = char_bytes_to_render.decode('utf-8')
        except (ValueError, UnicodeDecodeError) as e:
            print(f"錯誤: 無法將十六進制 '{hex_key}' 解碼為UTF-8字符: {e}。將跳過此字符。")
            continue

        image = Image.new(
            "L", (actual_glyph_width, actual_glyph_height), 0)
        draw = ImageDraw.Draw(image)
        draw.text((0, 0), char_code_to_render, fill=255, font=font)

        char_byte_list = []
        for y in range(actual_glyph_height):
            current_row_bytes = [0] * bytes_per_row
            for x in range(actual_glyph_width):
                pixel = image.getpixel((x, y))
                if pixel > 128:
                    byte_index = x // 8
                    bit_index_in_byte = 7 - (x % 8)  # MSB first
                    current_row_bytes[byte_index] |= (1 << bit_index_in_byte)

            char_byte_list.extend(current_row_bytes)

        # hex_key 已經是當前處理的字符的十六進制表示
        char_data_map[hex_key] = char_byte_list

    print(f"字體數據已生成，共 {len(char_data_map)} 個字符。")
    return char_data_map


@app.route("/ping")
def ping():
    return jsonify({"status": "OK"})


@app.route('/login', methods=['POST'])
def login():
    data = request.json
    if not data or data.get('username') != os.getenv('QUANTIX_USERNAME') or data.get('password') != os.getenv('QUANTIX_PASSWORD'):
        return jsonify({'error': 'Invalid credentials'}), 401
    token = jwt.encode({
        'user': data['username'],
        'exp': datetime.datetime.utcnow() + datetime.timedelta(minutes=60)
    }, os.getenv('SECRET_KEY'), algorithm='HS256')
    # 註冊 user
    if data['username'] not in authorized_users:
        authorized_users[data['username']] = {}
    authorized_users[data['username']]['jwt'] = token
    save_authorized_users()  # 持久化
    return jsonify({'token': token})


@app.route('/settings', methods=['POST'])
@token_required
def settings():
    session_token = generate_session_token()
    store_session_token(session_token)
    username = g.current_user
    authorized_users[username]['session_token'] = session_token
    save_authorized_users()  # 持久化

    auth_url = f"https://peng-pc.tail941dce.ts.net/oauth/setup?session_token={session_token}"
    c_array, w, h = qr_to_c_array(auth_url, box_size=1, border=0)
    # 轉成 bytes 再 base64
    c_bytes = bytes(c_array)
    c_b64 = base64.b64encode(c_bytes).decode('utf-8')

    return jsonify({
        "qr_c_array_base64": c_b64,
        "width": w,
        "height": h
    })


@app.route('/oauth/setup')
def oauth_setup():
    session_token = request.args.get("session_token", "")
    if session_token not in session_tokens or session_tokens[session_token] < time.time():
        return "Invalid or expired token", 403

    # 用 HTML 產生 Login with Google 按鈕
    login_url = (
        "https://accounts.google.com/o/oauth2/v2/auth"
        "?client_id=" + os.getenv("GOOGLE_CLIENT_ID") +
        "&response_type=code"
        "&scope=email%20profile%20https://www.googleapis.com/auth/calendar.readonly"
        "&redirect_uri=" + os.getenv("GOOGLE_REDIRECT_URI") +
        "&state=" + session_token +
        "&access_type=offline"
        "&prompt=consent"
    )

    html = f"""
    <h2>登入你的 Google 帳號</h2>
    <a href="{login_url}">
        <button>Login with Google</button>
    </a>
    """
    return render_template_string(html)


# Google 回呼路徑
@app.route('/oauth/callback')
def oauth_callback():
    code = request.args.get('code')
    state = request.args.get('state')  # session_token

    # 找到 username
    username = None
    for user, info in authorized_users.items():
        if info.get('session_token') == state:
            username = user
            break
    if not username:
        return "Invalid or expired session", 403

    token_response = requests.post(
        "https://oauth2.googleapis.com/token",
        data={
            "code": code,
            "client_id": os.getenv("GOOGLE_CLIENT_ID"),
            "client_secret": os.getenv("GOOGLE_CLIENT_SECRET"),
            "redirect_uri": os.getenv("GOOGLE_REDIRECT_URI"),
            "grant_type": "authorization_code"
        }
    )

    if token_response.status_code != 200:
        return "Failed to get token from Google", 400

    token_data = token_response.json()
    access_token = token_data['access_token']
    refresh_token = token_data.get('refresh_token', '')

    user_info = requests.get(
        "https://www.googleapis.com/oauth2/v1/userinfo",
        params={"access_token": access_token}
    ).json()

    # 存進 username 下
    authorized_users[username]['google'] = {
        "email": user_info.get("email"),
        "access_token": access_token,
        "refresh_token": refresh_token,
        "expires_at": time.time() + token_data.get("expires_in", 3600)  # expires_in 通常是 3600
    }
    save_authorized_users()  # 持久化
    return f"""
    <h3>登入成功：{user_info.get("email")}</h3>
    <p>你可以關閉這個頁面</p>
    """


@app.route('/check_auth_result', methods=['GET'])
@token_required
def check_result():
    username = request.args.get('username')
    if not username or username not in authorized_users:
        print(f"Unauthorized access attempt by {username}")
        return jsonify({"status": "not_found"})
    google_info = authorized_users[username].get('google')
    if google_info:
        return jsonify(google_info)
    return jsonify({"status": "pending"})


@app.route('/api/stock')
@token_required
def get_stock():
    return jsonify({'stock': '2330.TW', 'price': 799.5})


@app.route('/api/calendar', methods=['POST'])
@token_required
@ensure_valid_google_token
def get_calendar_events():
    username = g.current_user

    google_info = authorized_users[username].get('google')
    if not google_info:
        return jsonify({'error': 'Google 未授權'}), 403

    data = request.json
    date_str = data.get('date')  # 期望格式：'YYYY-MM-DD'
    if not date_str:
        print(f"Unauthorized access attempt by {username}")
        return jsonify({'error': '請提供日期'}), 400

    # 產生 RFC3339 格式的開始與結束時間
    try:
        from datetime import datetime, timedelta
        start_dt = datetime.strptime(date_str, "%Y-%m-%d")
        end_dt = start_dt + timedelta(days=1)
        time_min = start_dt.isoformat() + 'Z'
        time_max = end_dt.isoformat() + 'Z'
    except Exception as e:
        return jsonify({'error': '日期格式錯誤，請用 YYYY-MM-DD'}), 400

    # 呼叫 Google Calendar API
    resp = requests.get(
        "https://www.googleapis.com/calendar/v3/calendars/primary/events",
        params={
            "timeMin": time_min,
            "timeMax": time_max,
            "singleEvents": True,
            "orderBy": "startTime"
        },
        headers={
            "Authorization": f"Bearer {google_info['access_token']}"
        }
    )

    if resp.status_code != 200:
        print(
            f"Google Calendar API error: {resp.status_code}, detail: {resp.text}")
        return jsonify({'error': 'Google Calendar API 失敗', 'detail': resp.text}), 500

    events = resp.json().get('items', [])
    # 只回傳必要欄位
    result = []
    for event in events:
        result.append({
            "summary": event.get("summary"),
            "start": event.get("start", {}).get("dateTime") or event.get("start", {}).get("date"),
            "end": event.get("end", {}).get("dateTime") or event.get("end", {}).get("date"),
        })
    print(f"Found {len(result)} events for {date_str} in {username}'s calendar")
    return jsonify({"events": result})


@app.route("/font")
def get_font():
    chars = request.args.get("chars", "")
    # font_size = int(request.args.get("size", 16)) # 允許客戶端指定字體大小，預設為16
    result = generate_font_char_data_dict(chars, 16)
    if result is None:
        return jsonify({"error": "Failed to generate font data"}), 500

    # 將 Python 字典轉換為緊湊的 JSON 字符串 (無多餘空格和換行)
    compact_json_string = json.dumps(result, separators=(',', ':'))
    # 計算 JSON 字符串的字節長度 (通常為 UTF-8 編碼)
    response_byte_length = len(compact_json_string.encode('utf-8'))
    print(f"回傳的 JSON 字節長度: {response_byte_length}")

    return Response(compact_json_string, mimetype='application/json')


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=443, ssl_context=('cert.pem', 'key.pem'))
