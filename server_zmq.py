import zmq
import json
import time

LOG_FILE = "android_messages.log"

def main():
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind("tcp://*:5555")

    print("ZMQ Server доступен по tcp://*:5555")
    counter = 0

    while True:
        data = socket.recv().decode("utf-8")
        counter += 1

        try:
            payload = json.loads(data)
        except json.JSONDecodeError:
            payload = {"raw": data}

        record = {
            "packet_id": counter,
            "payload": payload,
            "server_time": time.strftime("%Y-%m-%d %H:%M:%S")
        }

        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

        print(f"[{counter}] Получено:", payload)

        socket.send_string("Hello from Server!")

if __name__ == "__main__":
    main()
