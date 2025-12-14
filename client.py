import socket

HOST = "127.0.0.1"
PORT = 5000

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect((HOST, PORT))

sock.sendall(b"Hello World!")
data = sock.recv(1024)

print("Получено:", data.decode())
sock.close()
