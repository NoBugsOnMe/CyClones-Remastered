import numpy as np, cv2
def dequant(rgb, sigma=2.0, delta=4.0, flat=28):
    a = rgb.astype(np.float32)
    b = cv2.GaussianBlur(a, (0, 0), sigma)
    k = np.ones((7, 7), np.uint8)
    rng = (cv2.dilate(a, k) - cv2.erode(a, k)).max(axis=2)
    w = np.clip((flat - rng) / (flat * 0.5), 0, 1)[..., None]   # 1 in flat areas, 0 at edges
    d = np.clip(b - a, -delta, delta)
    return a + d * w
