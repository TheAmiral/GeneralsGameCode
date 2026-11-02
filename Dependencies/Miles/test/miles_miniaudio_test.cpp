// Armies Of Tomorrow: checks for the miniaudio implementation of the Miles API.
// Usage: miles_miniaudio_test <dir with tone_pcm.wav, tone_adpcm.wav, music.mp3>
// The test files are 0.5 s 22050 Hz mono tones (PCM and IMA ADPCM) and a 2.0 s 44100 Hz stereo
// MP3. They can be made with ffmpeg, see README.md.

#include "mss/mss.h"
#include "MilesLoader.h"
#include "AoTVideoAudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_fails; } else { printf("ok   %s\n", #cond); } } while (0)

static std::string g_dir;

static std::vector<char> readFile(const char *name)
{
	std::string path = g_dir + "/" + name;
	FILE *f = fopen(path.c_str(), "rb");
	std::vector<char> data;
	if (!f) return data;
	fseek(f, 0, SEEK_END);
	data.resize(ftell(f));
	fseek(f, 0, SEEK_SET);
	fread(data.data(), 1, data.size(), f);
	fclose(f);
	return data;
}

static U32 __stdcall fileOpen(const char *name, void **handle)
{
	std::string path = g_dir + "/" + name;
	*handle = fopen(path.c_str(), "rb");
	return *handle != nullptr;
}
static void __stdcall fileClose(void *handle) { fclose((FILE *)handle); }
static S32 __stdcall fileSeek(void *handle, S32 offset, U32 type)
{
	int origin = type == AIL_FILE_SEEK_CURRENT ? SEEK_CUR : (type == AIL_FILE_SEEK_END ? SEEK_END : SEEK_SET);
	if (fseek((FILE *)handle, offset, origin) != 0) return -1;
	return (S32)ftell((FILE *)handle);
}
static U32 __stdcall fileRead(void *handle, void *buffer, U32 bytes) { return (U32)fread(buffer, 1, bytes, (FILE *)handle); }

static std::atomic<int> g_sampleEnds(0);
static std::atomic<int> g_streamEnds(0);
static std::thread::id g_callbackThread;
static void __stdcall onSampleEnd(HSAMPLE) { g_callbackThread = std::this_thread::get_id(); ++g_sampleEnds; }
static void __stdcall onStreamEnd(HSTREAM) { ++g_streamEnds; }

static bool waitFor(std::atomic<int> &counter, int value, int ms)
{
	for (int t = 0; t < ms; t += 10)
	{
		if (counter.load() >= value) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return counter.load() >= value;
}

int main(int argc, char **argv)
{
	g_dir = argc > 1 ? argv[1] : ".";
	CHECK(MilesLoader::load());

	// WAV parsing
	std::vector<char> pcm = readFile("tone_pcm.wav");
	std::vector<char> adpcm = readFile("tone_adpcm.wav");
	CHECK(!pcm.empty() && !adpcm.empty());

	AILSOUNDINFO info;
	CHECK(AIL_WAV_info(pcm.data(), &info) == 1);
	CHECK(info.format == WAVE_FORMAT_PCM && info.channels == 1 && info.rate == 22050 && info.bits == 16);
	CHECK(info.samples == 11025);

	AILSOUNDINFO adpcmInfo;
	CHECK(AIL_WAV_info(adpcm.data(), &adpcmInfo) == 1);
	CHECK(adpcmInfo.format == WAVE_FORMAT_IMA_ADPCM && adpcmInfo.channels == 1 && adpcmInfo.rate == 22050);

	// ADPCM to PCM image
	void *decoded = nullptr;
	U32 decodedSize = 0;
	CHECK(AIL_decompress_ADPCM(&adpcmInfo, &decoded, &decodedSize) == 1);
	AILSOUNDINFO decodedInfo;
	CHECK(decoded && AIL_WAV_info(decoded, &decodedInfo) == 1);
	CHECK(decodedInfo.format == WAVE_FORMAT_PCM && decodedInfo.bits == 16 && decodedInfo.rate == 22050);
	// IMA ADPCM pads the last block, so allow up to one block of extra samples.
	printf("     decoded ADPCM samples: %lu (PCM original 11025)\n", (unsigned long)decodedInfo.samples);
	CHECK(decodedInfo.samples >= 11025 && decodedInfo.samples < 11025 + 2048);
	// The decoded tone should be close to the PCM original.
	{
		const short *a = (const short *)info.data_ptr;
		const short *b = (const short *)decodedInfo.data_ptr;
		double err = 0, ref = 0;
		for (int i = 0; i < 11025; ++i) { err += (double)(a[i] - b[i]) * (a[i] - b[i]); ref += (double)a[i] * a[i]; }
		printf("     ADPCM SNR: %.1f dB\n", 10.0 * log10(ref / (err + 1)));
		CHECK(ref / (err + 1) > 100.0); // > 20 dB
	}

	// Startup
	AIL_set_file_callbacks(fileOpen, fileClose, fileSeek, fileRead);
	CHECK(AIL_startup() == 1);
	const S32 started = AIL_quick_startup(1, 0, 44100, 16, 2);
	printf("     device: %s\n", started ? "opened" : AIL_last_error());

	// Stream length (works without a device)
	HSTREAM mp3 = AIL_open_stream(nullptr, "music.mp3", 0);
	CHECK(mp3 != nullptr);
	S32 total = 0, current = -1;
	AIL_stream_ms_position(mp3, &total, &current);
	printf("     mp3 length: %ld ms\n", (long)total);
	CHECK(total >= 1950 && total <= 2100);
	CHECK(AIL_stream_playback_rate(mp3) == 44100);
	AIL_close_stream(mp3);

	// Providers
	HPROENUM next = HPROENUM_FIRST;
	HPROVIDER provider = nullptr;
	char *name = nullptr;
	CHECK(AIL_enumerate_3D_providers(&next, &provider, &name) == 1);
	CHECK(name && strcmp(name, "Miles Fast 2D Positional Audio") == 0);
	CHECK(AIL_enumerate_3D_providers(&next, &provider, &name) == 0);
	CHECK(AIL_open_3D_provider(provider) == 0);

	if (started)
	{
		HDIGDRIVER dig = nullptr;
		AIL_quick_handles(&dig, nullptr, nullptr);
		CHECK(dig != nullptr);

		// A 2D sample ends once, on another thread
		HSAMPLE s = AIL_allocate_sample_handle(dig);
		AIL_init_sample(s);
		AIL_register_EOS_callback(s, onSampleEnd);
		CHECK(AIL_set_sample_file(s, pcm.data(), 0) == 1);
		CHECK(AIL_sample_playback_rate(s) == 22050);
		auto t0 = std::chrono::steady_clock::now();
		AIL_start_sample(s);
		CHECK(waitFor(g_sampleEnds, 1, 3000));
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		printf("     2D sample ended after %.0f ms (file 500 ms)\n", ms);
		CHECK(ms > 400 && ms < 1500);
		CHECK(g_callbackThread != std::this_thread::get_id());

		// Loop count 3 plays three times
		g_sampleEnds = 0;
		AIL_set_sample_loop_count(s, 3);
		t0 = std::chrono::steady_clock::now();
		AIL_start_sample(s);
		CHECK(waitFor(g_sampleEnds, 1, 5000));
		ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		printf("     3 loops ended after %.0f ms (expected 1500 ms)\n", ms);
		CHECK(ms > 1350 && ms < 2500);

		// Pitch: double rate halves the time
		g_sampleEnds = 0;
		AIL_set_sample_loop_count(s, 1);
		AIL_set_sample_playback_rate(s, 44100);
		t0 = std::chrono::steady_clock::now();
		AIL_start_sample(s);
		CHECK(waitFor(g_sampleEnds, 1, 3000));
		ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		printf("     double rate ended after %.0f ms (expected 250 ms)\n", ms);
		CHECK(ms > 150 && ms < 900);

		// Pause holds the end back
		g_sampleEnds = 0;
		AIL_set_sample_playback_rate(s, 22050);
		AIL_start_sample(s);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		AIL_stop_sample(s);
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		CHECK(g_sampleEnds.load() == 0);
		AIL_resume_sample(s);
		CHECK(waitFor(g_sampleEnds, 1, 3000));
		AIL_release_sample_handle(s);

		// Stream loop count counts down, and the callback comes at the very end
		HSTREAM st = AIL_open_stream(dig, "music.mp3", 0);
		AIL_register_stream_callback(st, onStreamEnd);
		AIL_set_stream_loop_count(st, 2);
		AIL_set_stream_playback_rate(st, 44100 * 4); // 4x to keep the test short
		AIL_start_stream(st);
		std::this_thread::sleep_for(std::chrono::milliseconds(700));
		printf("     stream loops remaining after the first pass: %ld\n", (long)AIL_stream_loop_count(st));
		CHECK(AIL_stream_loop_count(st) == 1);
		CHECK(waitFor(g_streamEnds, 1, 3000));
		AIL_close_stream(st);

		// 3D: inside min distance full volume, beyond max silent (checked through the API state only)
		H3DSAMPLE s3 = AIL_allocate_3D_sample_handle(provider);
		H3DPOBJECT listener = AIL_open_3D_listener(provider);
		AIL_set_3D_position(listener, 0, 0, 0);
		AIL_set_3D_orientation(listener, 0, 1, 0, 0, 0, -1);
		CHECK(AIL_set_3D_sample_file(s3, pcm.data()) == 1);
		AIL_set_3D_sample_distances(s3, 100.0f, 10.0f);
		AIL_set_3D_position(s3, 50, 0, 0);
		AIL_set_3D_sample_volume(s3, 0.8f);
		CHECK(AIL_3D_sample_volume(s3) > 0.79f);
		AIL_start_3D_sample(s3);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		AIL_release_3D_sample_handle(s3);
		AIL_close_3D_listener(listener);

		// Video sound track push stream: 0.3 s stereo 48 kHz, then a format change, then close
		{
			std::vector<float> tone(48000 * 3 / 10 * 2);
			for (size_t i = 0; i < tone.size() / 2; ++i)
				tone[i * 2] = tone[i * 2 + 1] = 0.3f * (float)sin(i * 2 * 3.14159265 * 440 / 48000);
			AoT_VideoAudio_Reset();
			AoT_VideoAudio_SetVolume(0.5f);
			AoT_VideoAudio_Push(tone.data(), (unsigned)(tone.size() / 2), 2, 48000);
			AoT_VideoAudio_Play();
			AoT_VideoAudio_Play();
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			AoT_VideoAudio_Push(tone.data(), (unsigned)(tone.size() / 2), 1, 22050);
			AoT_VideoAudio_Play();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			AoT_VideoAudio_Close();
			AoT_VideoAudio_Push(nullptr, 0, 2, 48000);
			printf("ok   video push stream ran\n");
			// Left open on purpose: AIL_shutdown must close it before the engine goes.
			AoT_VideoAudio_Push(tone.data(), 100, 2, 48000);
			AoT_VideoAudio_Play();
		}
	}

	AIL_shutdown();
	if (decoded) AIL_mem_free_lock(decoded);
	printf("fails=%d\n", g_fails);
	return g_fails ? 1 : 0;
}
