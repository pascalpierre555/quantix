from flask import Flask, request, jsonify
import jwt                       # 🔹 pyjwt 套件，用來產生/解析 token
import datetime                  # 🔹 處理過期時間
from functools import wraps     # 🔹 保留函式原名的裝飾器工具
import finnhub

finnhub_client = finnhub.Client(
    api_key="d0lgrlpr01qhb028eq80d0lgrlpr01qhb028eq8g")
print(finnhub_client.quote('AAPL'))


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

    print(data.get('username'), data.get('password'))
    token = jwt.encode({
        'user': data['username'],
        'exp': datetime.datetime.utcnow() + datetime.timedelta(minutes=60)
    }, SECRET_KEY, algorithm='HS256')

    return jsonify({'token': token})


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
