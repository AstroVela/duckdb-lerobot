# LeRobot video fixtures

`long-20701.mp4` is a 16x16 H.264 video with 20,701 black frames at 30 FPS.
It is intentionally longer than the decoder's cluster-span threshold while
remaining small enough for the SQLLogic regression suite.

It was generated with FFmpeg 6.1.1:

```bash
ffmpeg -f lavfi -i color=c=black:s=16x16:r=30 -frames:v 20701 \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 300 \
  -keyint_min 300 -sc_threshold 0 -pix_fmt yuv420p -movflags +faststart \
  long-20701.mp4
```

SHA-256: `053d1b0e8e7260a782794cb7a85cb273755a52762c861fd65ab2e3e195da0bb5`.
