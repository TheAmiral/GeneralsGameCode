# Miles on miniaudio: test

`miles_miniaudio_test.cpp` checks the miniaudio implementation of the Miles API (`MilesMiniaudio.cpp`).
It covers:

- WAV parsing
- IMA ADPCM decoding
- MP3 stream length
- end callbacks on a separate thread
- loop counts
- playback rate
- pause and resume
- stream loop countdown

Make the test files with ffmpeg:

```
ffmpeg -f lavfi -i "sine=frequency=440:duration=0.5:sample_rate=22050" -ac 1 -c:a pcm_s16le tone_pcm.wav
ffmpeg -f lavfi -i "sine=frequency=440:duration=0.5:sample_rate=22050" -ac 1 -c:a adpcm_ima_wav tone_adpcm.wav
ffmpeg -f lavfi -i "sine=frequency=440:duration=2.0:sample_rate=44100" -ac 2 -c:a libmp3lame -b:a 128k music.mp3
```

Then build and run it, here with MinGW-w64 and Wine:

```
x86_64-w64-mingw32-g++ -O2 -std=c++17 -IDependencies/Miles -IDependencies/Miles/mss \
  Dependencies/Miles/test/miles_miniaudio_test.cpp <build>/Dependencies/Miles/libdeps_miles.a \
  -static -lole32 -lwinmm -o t.exe
wine t.exe <dir with the test files>   # prints fails=0
```
