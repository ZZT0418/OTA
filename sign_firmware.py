import os
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric import utils

PRIVATE_KEY_PATH = r"D:\ESP_IDF\ESP32_Project\ota_private_key\ec_private.pem"

# ===== 首次运行：生成并保存密钥对（之后复用，不会重复生成）=====
if not os.path.exists(PRIVATE_KEY_PATH):
    private_key = ec.generate_private_key(ec.SECP256R1())
    # 保存私钥
    with open(PRIVATE_KEY_PATH, "wb") as f:
        f.write(private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
    # 保存公钥（这个给设备端用）
    with open("ec_public.pem", "wb") as f:
        f.write(private_key.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo))
    print("密钥对已生成：ec_private.pem / ec_public.pem")
else:
    # 复用已存在的私钥
    with open(PRIVATE_KEY_PATH, "rb") as f:
        private_key = serialization.load_pem_private_key(f.read(), password=None)

# ===== 对升级包签名（改成你实际的固件文件名）=====
INPUT_FILE = "build/OneNet.bin"          # 改成你的固件/差分包文件名
OUTPUT_FILE = "firmware_signed.bin"  # 输出：固件 + 签名

with open(INPUT_FILE, "rb") as f:
    data = f.read()

# 用私钥签名（ECDSA + SHA256）；构造固定 72 字节 DER 签名（r/s 各带前导 0x00 补足 33 字节）
der_sig = private_key.sign(data, ec.ECDSA(hashes.SHA256()))
r, s = utils.decode_dss_signature(der_sig)
r_bytes = b'\x00' + r.to_bytes(32, 'big')
s_bytes = b'\x00' + s.to_bytes(32, 'big')
signature = b'\x30\x46' + b'\x02\x21' + r_bytes + b'\x02\x21' + s_bytes   # 固定 72 字节

# 追加 DER 签名到文件末尾
with open(OUTPUT_FILE, "wb") as f:
    f.write(data)
    f.write(signature)

# ===== PC 端自验证：用原始 DER 签名验证 =====
private_key.public_key().verify(der_sig, data, ec.ECDSA(hashes.SHA256()))

print(f"签名完成：{len(data)} 字节固件 + {len(signature)} 字节签名")
print(f"输出文件：{OUTPUT_FILE}")
