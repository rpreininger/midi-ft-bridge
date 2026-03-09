Place video clips here (mp4, avi, mkv, mov).

These are mapped to MIDI notes via config.json.
Resolution should match panel size (default: 128x64).

Example config.json mapping:
  { "note": 36, "panel": "A", "clip": "fire.mp4" }

Clips are deployed to the Pi via: deploy/deploy.sh pi@<ip>
