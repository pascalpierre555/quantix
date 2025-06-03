from flask import Flask, request, jsonify
import jwt                       # 🔹 pyjwt 套件，用來產生/解析 token
import datetime                  # 🔹 處理過期時間
from functools import wraps     # 🔹 保留函式原名的裝飾器工具
import finnhub
import secrets
import time


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


app = Flask(__name__)
SECRET_KEY = 'your-secret-key'  # ✅ 建議寫進環境變數
USERNAME = 'esp32'
PASSWORD = 'supersecret'


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
def settings():
    token = generate_session_token()
    store_session_token(token)

    auth_url = f"https://peng-pc.tail941dce.ts.net/oauth/setup?session_token={token}"
    return jsonify({"auth_url": auth_url})


@app.route('/oauth/setup')
def oauth_setup():
    token = request.args.get("session_token", "")
    if not is_token_valid(token):
        return "Invalid or expired session token.", 403

    # 有效的話，就顯示 Google 授權頁面或 redirect
    return "<h1>開始 Google 授權流程...</h1>"


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


@app.route('/api/stock')
@token_required
def get_stock():
    return jsonify({'stock': '2330.TW', 'price': 799.5})


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=443, ssl_context=('cert.pem', 'key.pem'))
