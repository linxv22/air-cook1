import socket

# 监听本地所有网卡
TCP_IP = "0.0.0.0"
TCP_PORT = 12345
OUTPUT_FILE = "captured_audio.pcm"

# 创建 TCP Socket
server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
# 允许端口复用（防止关闭后立刻重启报错端口占用）
server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server_socket.bind((TCP_IP, TCP_PORT))
server_socket.listen(1)

print(f"★ TCP 服务器已启动，正在监听端口 {TCP_PORT}...")
print("等待 ESP32 连接...（请在此时启动您的 ESP32）\n")

try:
    conn, addr = server_socket.accept()
    print(f"✔ 成功建立连接！设备 IP: {addr}")
    
    with open(OUTPUT_FILE, "wb") as f:
        print("-> 正在写入文件 captured_audio.pcm... 按 Ctrl+C 可以停止接收并保存。")
        while True:
            data = conn.recv(4096)
            if not data:
                print("\nESP32 断开了连接。")
                break
            f.write(data)
except KeyboardInterrupt:
    print("\n▲ 手动停止接收。")
finally:
    if 'conn' in locals():
        conn.close()
    server_socket.close()
    print(f"💾 数据已成功保存到: {OUTPUT_FILE}")