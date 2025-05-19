from flask import Flask, request, jsonify, abort
import uuid

app = Flask(__name__)

# 模擬的資料庫
# device_id -> {'ip': ..., 'user_agent': ..., 'authorized': True}
REGISTERED_DEVICES = {}
DEVICE_SECRETS = {"esp32-abc123": "xyz789"}  # 預設裝置 secret


@app.route("/register", methods=["POST"])
def register():
    data = request.get_json()
    username = data.get("username")
    password = data.get("password")

    # 驗證金鑰
    if DEVICE_SECRETS.get(username) != password:
        abort(403)

    # 新增 device_id 並記住
    device_id = str(uuid.uuid4())
    REGISTERED_DEVICES[device_id] = {
        "ip": request.remote_addr,
        "user_agent": request.user_agent.string,
        "authorized": True,
    }

    return jsonify({"device_id": device_id})


@app.route("/login")
def hello_world():
    ua = request.user_agent.string
    ip = request.remote_addr
    print(f"來自 {ip} 使用 {ua}\n")

    return "<p>Hello, World!</p>"
