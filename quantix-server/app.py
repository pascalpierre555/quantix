from flask import Flask, request, jsonify
import jwt                       # 🔹 pyjwt 套件，用來產生/解析 token
import datetime                  # 🔹 處理過期時間
from functools import wraps     # 🔹 保留函式原名的裝飾器工具
import finnhub
import secrets
import time
import qrcode
import io
import base64
import numpy as np


def generate_session_token():
    return secrets.token_urlsafe(32)  # 一個很難猜的 token


# 暫存 session tokens：key = token, value = 到期時間
session_tokens = {}


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
SECRET_KEY = 'your-secret-key'  # ✅ 建議寫進環境變數
USERNAME = 'esp32'
PASSWORD = 'supersecret'


def token_required(f):
    @wraps(f)
    def decorated(*args, **kwargs):
        token = request.headers.get('Authorization')
        if not token or not token.startswith("Bearer "):
            return jsonify({'error': 'Token missing'}), 403

        try:
            token = token.split(" ")[1]
            jwt.decode(token, SECRET_KEY, algorithms=['HS256'])
        except jwt.ExpiredSignatureError:
            return jsonify({'error': 'Token expired'}), 401
        except jwt.InvalidTokenError:
            return jsonify({'error': 'Invalid token'}), 403

        return f(*args, **kwargs)
    return decorated


@app.route("/ping")
def ping():
    return jsonify({"status": "OK"})


@app.route('/login', methods=['POST'])
def login():
    data = request.json
    if not data or data.get('username') != USERNAME or data.get('password') != PASSWORD:
        return jsonify({'error': 'Invalid credentials'}), 401
    token = jwt.encode({
        'user': data['username'],
        'exp': datetime.datetime.utcnow() + datetime.timedelta(minutes=60)
    }, SECRET_KEY, algorithm='HS256')

    return jsonify({'token': token})


@app.route('/settings', methods=['POST'])
@token_required
def settings():
    token = generate_session_token()
    store_session_token(token)

    auth_url = f"https://peng-pc.tail941dce.ts.net/oauth/setup?session_token={token}"

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
@token_required
def oauth_setup():
    token = request.args.get("session_token", "")
    if not is_token_valid(token):
        return "Invalid or expired session token.", 403

    # 有效的話，就顯示 Google 授權頁面或 redirect
    return "<h1>開始 Google 授權流程...</h1>"


@app.route('/api/stock')
@token_required
def get_stock():
    return jsonify({'stock': '2330.TW', 'price': 799.5})


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=443, ssl_context=('cert.pem', 'key.pem'))
