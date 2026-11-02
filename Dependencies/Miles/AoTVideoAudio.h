// Armies Of Tomorrow: PCM push stream for video sound tracks in the miniaudio Miles build.
// Bink played its own sound through Miles. The FFmpeg video player decodes the sound itself
// and pushes it here. One stream exists at a time, since one video plays at a time.
#pragma once

#ifdef AOT_MILES_MINIAUDIO

// Drops queued audio and stops playback. Call when a new video opens.
void AoT_VideoAudio_Reset();
// Queues interleaved float samples. Format changes restart the stream.
void AoT_VideoAudio_Push(const float *interleaved, unsigned frames, unsigned channels, unsigned sampleRate);
// Starts playback once audio is queued. Safe to call every frame.
void AoT_VideoAudio_Play();
// 0..1
void AoT_VideoAudio_SetVolume(float volume);
// Stops playback and frees the stream.
void AoT_VideoAudio_Close();

#endif
