import socket

HOST = "0.0.0.0"
PORT = 5000

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.bind((HOST, PORT))
sock.listen(1)

print(f"Server на {HOST}:{PORT}")

conn, addr = sock.accept()
print(f"Соединение по {addr}")

data = conn.recv(1024)
print("Получено:", data.decode())

conn.sendall(b"Hello World!")
conn.close()
sock.close()
