#!/usr/bin/env python3
import socket
import cv2
import numpy as np
import time

UDP_PORT = 3334
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", UDP_PORT))
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)

print(f"Слушаем UDP порт {UDP_PORT}...")

frames = {}  # frame_id -> {chunks}
fps_count = 0
fps_time = time.time()
fps = 0

while True:
    data, _ = sock.recvfrom(1500)
    if len(data) < 8:
        continue

    frame_id     = int.from_bytes(data[0:4], 'big')
    chunk_idx    = int.from_bytes(data[4:6], 'big')
    total_chunks = int.from_bytes(data[6:8], 'big')
    chunk_data   = data[8:]

    if frame_id not in frames:
        frames[frame_id] = {}
    frames[frame_id][chunk_idx] = chunk_data

    # Удаляем старые незавершённые кадры
    old_ids = [fid for fid in frames if fid < frame_id - 5]
    for fid in old_ids:
        del frames[fid]

    # Если получили все чанки — собираем кадр
    if len(frames[frame_id]) == total_chunks:
        jpeg = b''.join(frames[frame_id][i] for i in range(total_chunks))
        del frames[frame_id]

        frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
        if frame is None:
            continue

        fps_count += 1
        if time.time() - fps_time >= 1.0:
            fps = fps_count
            fps_count = 0
            fps_time = time.time()

        cv2.putText(frame, f"FPS: {fps}", (10, 30),
                    cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
        cv2.imshow("ESP32-S3", frame)

    if cv2.waitKey(1) & 0xFF == ord('q'):
        break

sock.close()
cv2.destroyAllWindows()