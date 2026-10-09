import cv2

cap = cv2.VideoCapture('input.mp4')
with open('video.bin', 'wb') as f:
    while True:
        ret, frame = cap.read()
        if not ret: break
        
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        resized = cv2.resize(gray, (128, 64))
        _, bw = cv2.threshold(resized, 128, 255, cv2.THRESH_BINARY | cv2.THRESH_OTSU)
        
        buf = bytearray(1024)
        for y in range(64):
            for x in range(128):
                if bw[y, x] > 0:
                    buf[x + (y // 8) * 128] |= (1 << (y % 8))
        f.write(buf)
cap.release()