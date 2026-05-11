import camera
import numpy as np
import time
import cv2

WIDTH = 640
HEIGHT = 480


camera.open()

# Discard the first few frames — sensor needs a moment to stabilize
# exposure after controls are applied
for _ in range(5):
    camera.capture()

# Benchmark: how fast can we capture + demosaic?
N = 30
t0 = time.perf_counter()
for i in range(N):
    frame = camera.capture()
t1 = time.perf_counter()

# Wrap the memoryview as a numpy array — this is the zero-copy step.
# frombuffer does NOT copy — it creates an array whose data pointer
# IS the g_rgb buffer inside our C extension.
arr = np.frombuffer(frame, dtype=np.uint8).reshape(HEIGHT, WIDTH, 3)

print(f"Shape:    {arr.shape}")
print(f"Dtype:    {arr.dtype}")
print(f"Avg FPS:  {N / (t1 - t0):.1f}")
print(f"Min/max pixel: {arr.min()} / {arr.max()}")
print(f"Center pixel (R,G,B): {arr[240, 320]}")


frame = camera.capture()
arr = np.frombuffer(frame, dtype=np.uint8).reshape(HEIGHT, WIDTH, 3)

# 2. Convert RGB to BGR for OpenCV
img_bgr = cv2.cvtColor(arr, cv2.COLOR_RGB2BGR)

# 3. Save to disk
cv2.imwrite("capture.jpg", img_bgr)
print("Image saved as capture.jpg")

camera.close()
