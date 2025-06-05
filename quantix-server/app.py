from flask import Flask, request, jsonify, render_template_string, g
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
import base64
import numpy as np


def generate_session_token():
    return secrets.token_urlsafe(32)  # 一個很難猜的 token


# 暫存 session tokens：key = token, value = 到期時間
session_tokens = {}
# key: username, value: dict (session_token, google info)
authorized_users = {}


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
        if response.status_code == 200:
            token_data = response.json()
            google_info['access_token'] = token_data['access_token']
            google_info['expires_at'] = time.time(
            ) + token_data.get("expires_in", 3600)
            return True
        else:
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
    return jsonify({'token': token})


@app.route('/settings', methods=['POST'])
@token_required
def settings():
    session_token = generate_session_token()
    store_session_token(session_token)
    username = g.current_user
    authorized_users[username]['session_token'] = session_token

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

    return jsonify({"events": result})


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=443, ssl_context=('cert.pem', 'key.pem'))
