# FTC1 clip format (Tab5 playout)

The Tab5 (ESP32-P4) has no H.264 decoder we can use, so clips are transcoded
on the Mac into a container it can play with nothing but a memcpy-class
decoder. Video is lossless RGB888 (bit-exact to what the Mac engine sends the
panels from the same source), audio is raw PCM.

Measured on Fly/Pixel/Tennis (20 s each, 288x128@25):
LZ4 on the XOR-delta to the previous frame = 19-29 KB/frame = 0.5-0.7 MB/s.
Plain RGB888 would be 2.76 MB/s, plain LZ4 1-2 MB/s.

All integers little-endian.

```
offset  size  field
0       4     magic "FTC1"
4       2     width            (288)
6       2     height           (128)
8       2     fps_num          (25)
10      2     fps_den          (1)
12      4     frame_count
16      4     audio_rate       (48000, 0 = no audio)
20      2     audio_channels   (2)
22      2     audio_bits       (16, signed, interleaved)
24      8     audio_offset     (absolute, 4096-aligned)
32      8     audio_bytes
40      8     index_offset     (absolute; frame_count x u32 frame offsets)
48      2     keyframe_interval
50      14    reserved (0)
```

Layout: `header | pad | audio PCM | pad | frames | index`

Each frame record starts 4-byte aligned:

```
0  4  payload_len
4  1  type: 0 = key   (LZ4 block of the RGB888 frame)
            1 = delta (LZ4 block of frame XOR previous frame)
            2 = repeat (identical to previous, payload_len = 0)
5  3  reserved
8  n  payload (LZ4 *block* format, no frame header, no size prefix)
```

A key frame every `keyframe_interval` frames (default 50 = 2 s) so a dropped or
corrupt frame heals within 2 s. Playback always starts at frame 0; the index
exists for diagnostics and for seeking later.

Audio and video are read through two independent file handles; the audio
clock is the master (same as the Mac engine): video frame = samples_played *
fps / audio_rate.
