/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 Armies Of Tomorrow
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// Armies Of Tomorrow: the Miles Sound System API, implemented on miniaudio.
//
// mss32.dll only exists for 32-bit Windows. This file implements the part of the Miles API that
// the engine uses (MilesAudioManager), so that a 64-bit build, and later macOS, has audio while the
// audio manager stays unchanged. It is built instead of the MilesLoader forwarders when
// AOT_MILES_MINIAUDIO is set.
//
// Behaviour follows Miles where the engine depends on it:
// - Loop counts: 1 plays once, 0 loops forever, n plays n times. Stream loop counts count down.
// - End-of-sample callbacks run on a separate thread, like the Miles timer thread. They never run
//   on the audio thread and never with the internal lock held, because the engine calls back into
//   the API from them.
// - AIL_set_sample_file resets the playback rate to the file's rate, as Miles does.
// - 3D samples: full volume inside min distance, min/distance between min and max, silent beyond
//   max. Pan comes from the listener's facing in the engine's right handed, z up world.
// - IMA ADPCM files are decoded to a 16 bit PCM WAV image, which is what Miles returns.
//
// Not implemented, because the engine works without them: the Miles delay filter, occlusion low
// pass filtering, speaker type selection, MIDI and DirectSound access.

#include "MilesLoader.h"
#include "AoTVideoAudio.h"
#include "mss/mss.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <algorithm>

#define MA_NO_MIDI
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_FLAC
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace
{

enum ObjectKind
{
	KIND_SAMPLE_2D = 1,
	KIND_SAMPLE_3D,
	KIND_STREAM,
	KIND_LISTENER,
	KIND_QUICK
};

enum { USER_DATA_SLOTS = 8 };
enum { DEFAULT_PLAYBACK_RATE = 22050 };

struct Voice;

// A data source in front of a decoder. It plays the decoder loopCount times (0 = forever) and
// counts the loops down, the way Miles does.
struct LoopSource
{
	ma_data_source_base base;
	ma_decoder *decoder;
	Voice *voice;
};

struct Voice
{
	h3DPOBJECT header; // first member, so that H3DSAMPLE and HSAMPLE point at a Voice
	ObjectKind kind;

	// Source
	ma_decoder decoder;
	bool decoderInit;
	LoopSource loopSource;
	bool loopSourceInit;
	void *fileHandle;					// streams: handle from the file callbacks
	ma_uint32 nativeRate;

	// Playback
	ma_sound sound;
	bool soundInit;
	bool playing;						// started and not paused
	bool paused;
	unsigned generation;				// changes on every start, so that stale end events are dropped

	// Settings
	float volume;
	float pan;							// Miles range: 0 left, 0.5 centre, 1 right
	S32 rate;
	S32 loopCount;						// 0 = forever
	volatile S32 loopsRemaining;

	// 3D
	float pos[3];
	float minDist;
	float maxDist;
	float occlusion;

	// Callbacks and user data
	AIL_sample_callback sampleCallback;
	AIL_3dsample_callback sample3DCallback;
	AIL_stream_callback streamCallback;
	S32 userData[USER_DATA_SLOTS];
};

struct Listener
{
	h3DPOBJECT header;
	ObjectKind kind;
	float pos[3];
	float face[3];
};

struct EndEvent
{
	Voice *voice;
	unsigned generation;
};

// Global state
std::recursive_mutex g_lock;
ma_engine g_engine;
bool g_engineInit = false;
int g_startupCount = 0;
DIG_DRIVER g_digitalDriver;
Listener g_listener;
bool g_listenerOpen = false;
std::vector<Voice *> g_voices;

AIL_file_open_callback g_fileOpen = nullptr;
AIL_file_close_callback g_fileClose = nullptr;
AIL_file_seek_callback g_fileSeek = nullptr;
AIL_file_read_callback g_fileRead = nullptr;

std::mutex g_eventLock;
std::condition_variable g_eventSignal;
std::deque<EndEvent> g_events;
std::thread g_eventThread;
bool g_eventThreadQuit = false;

void videoCloseLocked(); // video sound track, defined at the end

unsigned g_generationCounter = 0;		// unique per start, across all voices

char g_lastError[256] = "";
char g_providerName[] = "Miles Fast 2D Positional Audio";
const HPROVIDER PROVIDER_ID = (HPROVIDER)(uintptr_t)1;

// ------------------------------------------------------------------------------------------------
// Little helpers

inline Voice *toVoice(const void *p) { return (Voice *)p; }

void setError(const char *text)
{
	strncpy(g_lastError, text, sizeof(g_lastError) - 1);
	g_lastError[sizeof(g_lastError) - 1] = '\0';
}

ma_uint32 readLE32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ma_uint32)p[3] << 24); }
ma_uint16 readLE16(const unsigned char *p) { return (ma_uint16)(p[0] | (p[1] << 8)); }
void writeLE32(unsigned char *p, ma_uint32 v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
void writeLE16(unsigned char *p, ma_uint16 v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

// Size of a RIFF WAVE image from its header, or 0 when it is not one.
size_t wavImageSize(const void *image)
{
	const unsigned char *p = (const unsigned char *)image;
	if (p == nullptr || memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0)
		return 0;
	return (size_t)readLE32(p + 4) + 8;
}

// ------------------------------------------------------------------------------------------------
// End events. Miles calls end-of-sample callbacks on its timer thread; so do we.

void postEndEvent(Voice *voice)
{
	{
		std::lock_guard<std::mutex> lock(g_eventLock);
		EndEvent e = { voice, voice->generation };
		g_events.push_back(e);
	}
	g_eventSignal.notify_one();
}

void eventThreadMain()
{
	for (;;)
	{
		EndEvent e;
		{
			std::unique_lock<std::mutex> lock(g_eventLock);
			g_eventSignal.wait(lock, [] { return g_eventThreadQuit || !g_events.empty(); });
			if (g_eventThreadQuit && g_events.empty())
				return;
			e = g_events.front();
			g_events.pop_front();
		}

		AIL_sample_callback sampleCallback = nullptr;
		AIL_3dsample_callback sample3DCallback = nullptr;
		AIL_stream_callback streamCallback = nullptr;
		{
			std::lock_guard<std::recursive_mutex> lock(g_lock);
			if (std::find(g_voices.begin(), g_voices.end(), e.voice) == g_voices.end())
				continue; // released meanwhile
			Voice *v = e.voice;
			if (v->generation != e.generation || !v->playing)
				continue; // restarted or stopped meanwhile
			v->playing = false;
			sampleCallback = v->sampleCallback;
			sample3DCallback = v->sample3DCallback;
			streamCallback = v->streamCallback;
		}

		// Call without the lock: the engine calls back into the API from here.
		if (e.voice->kind == KIND_SAMPLE_2D && sampleCallback)
			sampleCallback((HSAMPLE)e.voice);
		else if (e.voice->kind == KIND_SAMPLE_3D && sample3DCallback)
			sample3DCallback((H3DSAMPLE)e.voice);
		else if (e.voice->kind == KIND_STREAM && streamCallback)
			streamCallback((HSTREAM)e.voice);
	}
}

void onSoundEnd(void *userData, ma_sound *)
{
	// Audio thread. Only queue the event.
	postEndEvent((Voice *)userData);
}

// ------------------------------------------------------------------------------------------------
// LoopSource

ma_result loopRead(ma_data_source *ds, void *out, ma_uint64 frameCount, ma_uint64 *framesRead)
{
	LoopSource *src = (LoopSource *)ds;
	ma_uint64 total = 0;
	ma_format format; ma_uint32 channels;
	ma_decoder_get_data_format(src->decoder, &format, &channels, nullptr, nullptr, 0);
	const ma_uint32 frameBytes = ma_get_bytes_per_frame(format, channels);

	int emptyReads = 0;
	while (total < frameCount)
	{
		ma_uint64 got = 0;
		void *dst = out ? (unsigned char *)out + total * frameBytes : nullptr;
		ma_result r = ma_decoder_read_pcm_frames(src->decoder, dst, frameCount - total, &got);
		total += got;
		if (total >= frameCount)
			break;
		if (got == 0 || r == MA_AT_END)
		{
			// End of the file: loop again or end.
			S32 remaining = src->voice->loopsRemaining;
			if (remaining == 1)
				break;
			if (remaining > 1)
				src->voice->loopsRemaining = remaining - 1;
			if (ma_decoder_seek_to_pcm_frame(src->decoder, 0) != MA_SUCCESS)
				break;
			if (got == 0 && ++emptyReads > 2)
				break; // empty file, do not spin
		}
	}

	if (framesRead)
		*framesRead = total;
	return (total == 0 && frameCount > 0) ? MA_AT_END : MA_SUCCESS;
}

ma_result loopSeek(ma_data_source *ds, ma_uint64 frame)
{
	return ma_decoder_seek_to_pcm_frame(((LoopSource *)ds)->decoder, frame);
}

ma_result loopFormat(ma_data_source *ds, ma_format *format, ma_uint32 *channels, ma_uint32 *rate, ma_channel *map, size_t mapCap)
{
	return ma_decoder_get_data_format(((LoopSource *)ds)->decoder, format, channels, rate, map, mapCap);
}

ma_result loopCursor(ma_data_source *ds, ma_uint64 *cursor)
{
	return ma_decoder_get_cursor_in_pcm_frames(((LoopSource *)ds)->decoder, cursor);
}

ma_result loopLength(ma_data_source *ds, ma_uint64 *length)
{
	return ma_decoder_get_length_in_pcm_frames(((LoopSource *)ds)->decoder, length);
}

const ma_data_source_vtable g_loopVTable = { loopRead, loopSeek, loopFormat, loopCursor, loopLength, nullptr, 0 };

// ------------------------------------------------------------------------------------------------
// Stream file access through the engine's file callbacks

ma_result streamRead(ma_decoder *decoder, void *out, size_t bytes, size_t *bytesRead)
{
	Voice *v = (Voice *)decoder->pUserData;
	U32 got = (g_fileRead && v->fileHandle) ? g_fileRead(v->fileHandle, out, (U32)bytes) : 0;
	if (bytesRead)
		*bytesRead = got;
	return (got == 0 && bytes > 0) ? MA_AT_END : MA_SUCCESS;
}

ma_result streamSeek(ma_decoder *decoder, ma_int64 offset, ma_seek_origin origin)
{
	Voice *v = (Voice *)decoder->pUserData;
	if (!g_fileSeek || !v->fileHandle)
		return MA_ERROR;
	U32 mode = AIL_FILE_SEEK_BEGIN;
	if (origin == ma_seek_origin_current) mode = AIL_FILE_SEEK_CURRENT;
	else if (origin == ma_seek_origin_end) mode = AIL_FILE_SEEK_END;
	return g_fileSeek(v->fileHandle, (S32)offset, mode) < 0 ? MA_ERROR : MA_SUCCESS;
}

// ------------------------------------------------------------------------------------------------
// Voices

void resetSettings(Voice *v)
{
	v->volume = 1.0f;
	v->pan = 0.5f;
	v->rate = v->nativeRate ? (S32)v->nativeRate : DEFAULT_PLAYBACK_RATE;
	v->loopCount = 1;
	v->loopsRemaining = 1;
	v->occlusion = 0.0f;
}

Voice *newVoice(ObjectKind kind)
{
	Voice *v = new Voice;
	memset(v, 0, sizeof(*v));
	v->kind = kind;
	v->minDist = 1.0f;
	v->maxDist = 1000000.0f;
	resetSettings(v);
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	g_voices.push_back(v);
	return v;
}

void releaseSound(Voice *v)
{
	if (v->soundInit)
	{
		ma_sound_uninit(&v->sound);
		v->soundInit = false;
	}
	v->playing = false;
	v->paused = false;
}

void releaseSource(Voice *v)
{
	releaseSound(v);
	if (v->loopSourceInit)
	{
		ma_data_source_uninit(&v->loopSource);
		v->loopSourceInit = false;
	}
	if (v->decoderInit)
	{
		ma_decoder_uninit(&v->decoder);
		v->decoderInit = false;
	}
	if (v->fileHandle && g_fileClose)
		g_fileClose(v->fileHandle);
	v->fileHandle = nullptr;
	v->nativeRate = 0;
}

void deleteVoice(Voice *v)
{
	if (!v)
		return;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	releaseSource(v);
	g_voices.erase(std::remove(g_voices.begin(), g_voices.end(), v), g_voices.end());
	delete v;
}

bool initLoopSource(Voice *v)
{
	ma_data_source_config config = ma_data_source_config_init();
	config.vtable = &g_loopVTable;
	if (ma_data_source_init(&config, &v->loopSource) != MA_SUCCESS)
		return false;
	v->loopSource.decoder = &v->decoder;
	v->loopSource.voice = v;
	v->loopSourceInit = true;

	ma_uint32 rate = 0;
	ma_decoder_get_data_format(&v->decoder, nullptr, nullptr, &rate, nullptr, 0);
	v->nativeRate = rate;
	return true;
}

ma_decoder_config decoderConfig()
{
	// Decode to float at the file's own channel count and rate; the engine resamples and mixes.
	return ma_decoder_config_init(ma_format_f32, 0, 0);
}

// Gain and pan for a 3D voice from the listener.
void compute3D(const Voice *v, float *gain, float *pan)
{
	*gain = 1.0f;
	*pan = 0.0f;
	if (!g_listenerOpen)
		return;

	const float dx = v->pos[0] - g_listener.pos[0];
	const float dy = v->pos[1] - g_listener.pos[1];
	const float dz = v->pos[2] - g_listener.pos[2];
	const float dist = sqrtf(dx * dx + dy * dy + dz * dz);

	if (dist > v->maxDist)
		*gain = 0.0f;
	else if (dist > v->minDist && dist > 0.0f)
		*gain = v->minDist / dist;

	// right = face x up(0,0,1), in the engine's right handed z up world
	float rx = g_listener.face[1];
	float ry = -g_listener.face[0];
	const float rlen = sqrtf(rx * rx + ry * ry);
	const float hlen = sqrtf(dx * dx + dy * dy);
	if (rlen > 0.0001f && hlen > 0.0001f)
	{
		rx /= rlen; ry /= rlen;
		float p = (dx * rx + dy * ry) / hlen;
		if (p > 1.0f) p = 1.0f;
		if (p < -1.0f) p = -1.0f;
		*pan = p;
	}
}

// Push the voice's settings into its sound.
void applySettings(Voice *v)
{
	if (!v->soundInit)
		return;

	float volume = v->volume;
	float pan = v->pan * 2.0f - 1.0f;
	if (v->kind == KIND_SAMPLE_3D)
	{
		float gain, pan3D;
		compute3D(v, &gain, &pan3D);
		volume *= gain;
		pan = pan3D;
	}
	ma_sound_set_volume(&v->sound, volume);
	ma_sound_set_pan(&v->sound, pan);
	if (v->nativeRate > 0 && v->rate > 0)
		ma_sound_set_pitch(&v->sound, (float)v->rate / (float)v->nativeRate);
}

// Create the sound for the voice and start it at the decoder's current frame.
bool createAndStartSound(Voice *v)
{
	const ma_uint32 flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
	if (ma_sound_init_from_data_source(&g_engine, &v->loopSource, flags, nullptr, &v->sound) != MA_SUCCESS)
	{
		setError("ma_sound_init_from_data_source failed");
		return false;
	}
	v->soundInit = true;
	ma_sound_set_end_callback(&v->sound, onSoundEnd, v);
	applySettings(v);
	ma_sound_start(&v->sound);
	return true;
}

// Start the voice from the beginning of its source.
void startVoice(Voice *v)
{
	if (!g_engineInit || !v->loopSourceInit)
		return;

	releaseSound(v);
	ma_decoder_seek_to_pcm_frame(&v->decoder, 0);
	v->loopsRemaining = v->loopCount;
	v->generation = ++g_generationCounter;
	v->playing = createAndStartSound(v);
	v->paused = false;
}

// Miles' stop is a pause. The sound is destroyed rather than stopped, because destroying waits for
// the audio thread: after this returns, the source is never read again until it is resumed, so the
// engine may free the sample's memory.
void pauseVoice(Voice *v)
{
	if (v->soundInit && v->playing && !v->paused)
	{
		ma_sound_uninit(&v->sound);
		v->soundInit = false;
		v->paused = true; // the decoder keeps its position for the resume
	}
}

void resumeVoice(Voice *v)
{
	if (v->playing && v->paused && g_engineInit && v->loopSourceInit)
	{
		v->paused = false;
		v->playing = createAndStartSound(v);
	}
}

void endVoice(Voice *v)
{
	releaseSound(v);
}

S32 setSampleImage(Voice *v, const void *image)
{
	releaseSource(v);
	const size_t size = wavImageSize(image);
	if (size == 0)
	{
		setError("Sample file is not a RIFF WAVE image");
		return 0;
	}
	ma_decoder_config config = decoderConfig();
	if (ma_decoder_init_memory(image, size, &config, &v->decoder) != MA_SUCCESS)
	{
		setError("Could not decode sample file");
		return 0;
	}
	v->decoderInit = true;
	if (!initLoopSource(v))
	{
		releaseSource(v);
		return 0;
	}
	// Miles takes the playback rate from the file.
	v->rate = (S32)v->nativeRate;
	return 1;
}

void totalAndCurrentMs(Voice *v, S32 *total, S32 *current)
{
	ma_uint64 length = 0, cursor = 0;
	if (v->decoderInit && v->nativeRate > 0)
	{
		ma_decoder_get_length_in_pcm_frames(&v->decoder, &length);
		if (v->soundInit)
			ma_sound_get_cursor_in_pcm_frames(&v->sound, &cursor);
		else
			ma_decoder_get_cursor_in_pcm_frames(&v->decoder, &cursor);
	}
	if (total) *total = v->nativeRate ? (S32)(length * 1000 / v->nativeRate) : 0;
	if (current) *current = v->nativeRate ? (S32)(cursor * 1000 / v->nativeRate) : 0;
}

void seekMs(Voice *v, S32 ms)
{
	if (!v->decoderInit || v->nativeRate == 0)
		return;
	const ma_uint64 frame = (ma_uint64)(ms < 0 ? 0 : ms) * v->nativeRate / 1000;
	if (v->soundInit)
		ma_sound_seek_to_pcm_frame(&v->sound, frame);
	else
		ma_decoder_seek_to_pcm_frame(&v->decoder, frame);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// MilesLoader: nothing to load.

bool MilesLoader::isLoaded() { return true; }
bool MilesLoader::isFailed() { return false; }
unsigned long MilesLoader::getLastError() { return 0; }
bool MilesLoader::load() { return true; }
void MilesLoader::unload() {}

// ------------------------------------------------------------------------------------------------
// Startup and shutdown

S32 __stdcall AIL_startup(void)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (g_startupCount++ == 0)
	{
		g_eventThreadQuit = false;
		g_eventThread = std::thread(eventThreadMain);
	}
	return 1;
}

S32 __stdcall AIL_quick_startup(S32 use_digital, S32, U32 output_rate, S32, S32 output_channels)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!use_digital)
		return 0;
	if (g_engineInit)
		return 1;

	ma_engine_config config = ma_engine_config_init();
	config.channels = output_channels > 0 ? (ma_uint32)output_channels : 2;
	config.sampleRate = output_rate > 0 ? (ma_uint32)output_rate : 0;
	config.listenerCount = 1;
	if (ma_engine_init(&config, &g_engine) != MA_SUCCESS)
	{
		// The settings may not suit the device. Fall back to the device's own format.
		config.channels = 0;
		config.sampleRate = 0;
		if (ma_engine_init(&config, &g_engine) != MA_SUCCESS)
		{
			setError("Could not open the audio device");
			return 0;
		}
	}
	g_engineInit = true;
	memset(&g_digitalDriver, 0, sizeof(g_digitalDriver));
	return 1;
}

void __stdcall AIL_quick_handles(HDIGDRIVER *pdig, HMDIDRIVER *pmdi, HDLSDEVICE *pdls)
{
	if (pdig) *pdig = g_engineInit ? &g_digitalDriver : nullptr;
	if (pmdi) *pmdi = nullptr;
	if (pdls) *pdls = nullptr;
}

void __stdcall AIL_shutdown(void)
{
	std::vector<Voice *> voices;
	{
		std::lock_guard<std::recursive_mutex> lock(g_lock);
		if (g_startupCount == 0)
			return;
		if (--g_startupCount > 0)
			return;
		voices = g_voices;
	}

	{
		std::lock_guard<std::mutex> lock(g_eventLock);
		g_eventThreadQuit = true;
		g_events.clear();
	}
	g_eventSignal.notify_one();
	if (g_eventThread.joinable())
		g_eventThread.join();

	std::lock_guard<std::recursive_mutex> lock(g_lock);
	for (Voice *v : voices)
		deleteVoice(v);
	g_voices.clear();
	videoCloseLocked();
	if (g_engineInit)
	{
		ma_engine_uninit(&g_engine);
		g_engineInit = false;
	}
	g_listenerOpen = false;
}

char *__stdcall AIL_set_redist_directory(const char *) { return const_cast<char *>(""); }
char *__stdcall AIL_last_error(void) { return g_lastError; }
S32 __stdcall AIL_set_preference(U32, S32 value) { return value; }
U32 __stdcall AIL_get_timer_highest_delay(void) { return 0; }
void __stdcall AIL_stop_timer(HTIMER) {}
void __stdcall AIL_release_timer_handle(HTIMER) {}
void __stdcall AIL_lock(void) { g_lock.lock(); }
void __stdcall AIL_unlock(void) { g_lock.unlock(); }
void __stdcall AIL_lock_mutex(void) { g_lock.lock(); }
void __stdcall AIL_unlock_mutex(void) { g_lock.unlock(); }
S32 __stdcall AIL_waveOutOpen(HDIGDRIVER *driver, LPHWAVEOUT *waveout, S32, LPWAVEFORMAT)
{
	if (driver) *driver = nullptr;
	if (waveout) *waveout = nullptr;
	return -1;
}
void __stdcall AIL_waveOutClose(HDIGDRIVER) {}

void __stdcall AIL_get_DirectSound_info(HSAMPLE, AILLPDIRECTSOUND *lplpDS, AILLPDIRECTSOUNDBUFFER *lplpDSB)
{
	if (lplpDS) *lplpDS = nullptr;
	if (lplpDSB) *lplpDSB = nullptr;
}

void __stdcall AIL_set_file_callbacks(AIL_file_open_callback opencb, AIL_file_close_callback closecb,
	AIL_file_seek_callback seekcb, AIL_file_read_callback readcb)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	g_fileOpen = opencb;
	g_fileClose = closecb;
	g_fileSeek = seekcb;
	g_fileRead = readcb;
}

// ------------------------------------------------------------------------------------------------
// Providers, filters, listener

S32 __stdcall AIL_enumerate_3D_providers(HPROENUM *next, HPROVIDER *dest, char **name)
{
	if (!next || *next != HPROENUM_FIRST)
		return 0;
	*next = 1;
	if (dest) *dest = PROVIDER_ID;
	if (name) *name = g_providerName;
	return 1;
}

M3DRESULT __stdcall AIL_open_3D_provider(HPROVIDER lib) { return lib == PROVIDER_ID ? M3D_NOERR : 1; }
void __stdcall AIL_close_3D_provider(HPROVIDER) {}
void __stdcall AIL_set_3D_speaker_type(HPROVIDER, S32) {}
S32 __stdcall AIL_enumerate_filters(HPROENUM *, HPROVIDER *dest, char **name)
{
	if (dest) *dest = nullptr;
	if (name) *name = nullptr;
	return 0;
}
HPROVIDER __stdcall AIL_set_sample_processor(HSAMPLE, SAMPLESTAGE, HPROVIDER) { return nullptr; }
void __stdcall AIL_set_filter_sample_preference(HSAMPLE, const char *, const void *) {}

H3DPOBJECT __stdcall AIL_open_3D_listener(HPROVIDER)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	memset(&g_listener, 0, sizeof(g_listener));
	g_listener.kind = KIND_LISTENER;
	g_listener.face[1] = 1.0f;
	g_listenerOpen = true;
	return (H3DPOBJECT)&g_listener;
}

void __stdcall AIL_close_3D_listener(H3DPOBJECT)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	g_listenerOpen = false;
}

static void refreshAll3D()
{
	for (Voice *v : g_voices)
		if (v->kind == KIND_SAMPLE_3D)
			applySettings(v);
}

void __stdcall AIL_set_3D_position(H3DPOBJECT obj, F32 X, F32 Y, F32 Z)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!obj) return;
	if ((void *)obj == (void *)&g_listener)
	{
		g_listener.pos[0] = X; g_listener.pos[1] = Y; g_listener.pos[2] = Z;
		refreshAll3D();
		return;
	}
	Voice *v = toVoice(obj);
	v->pos[0] = X; v->pos[1] = Y; v->pos[2] = Z;
	applySettings(v);
}

void __stdcall AIL_set_3D_orientation(H3DPOBJECT obj, F32 X_face, F32 Y_face, F32 Z_face, F32, F32, F32)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if ((void *)obj == (void *)&g_listener)
	{
		g_listener.face[0] = X_face; g_listener.face[1] = Y_face; g_listener.face[2] = Z_face;
		refreshAll3D();
	}
}

void __stdcall AIL_set_3D_velocity_vector(H3DPOBJECT, F32, F32, F32) {}

void __stdcall AIL_set_3D_user_data(H3DPOBJECT obj, U32 index, S32 value)
{
	if (obj && (void *)obj != (void *)&g_listener && index < USER_DATA_SLOTS)
		toVoice(obj)->userData[index] = value;
}

S32 __stdcall AIL_3D_user_data(H3DPOBJECT obj, U32 index)
{
	if (obj && (void *)obj != (void *)&g_listener && index < USER_DATA_SLOTS)
		return toVoice(obj)->userData[index];
	return 0;
}

// ------------------------------------------------------------------------------------------------
// 2D samples

HSAMPLE __stdcall AIL_allocate_sample_handle(HDIGDRIVER) { return (HSAMPLE)newVoice(KIND_SAMPLE_2D); }
void __stdcall AIL_release_sample_handle(HSAMPLE sample) { deleteVoice(toVoice(sample)); }

void __stdcall AIL_init_sample(HSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	Voice *v = toVoice(sample);
	releaseSource(v);
	resetSettings(v);
}

S32 __stdcall AIL_set_sample_file(HSAMPLE sample, const void *file_image, S32)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? setSampleImage(toVoice(sample), file_image) : 0;
}

S32 __stdcall AIL_set_named_sample_file(HSAMPLE sample, const char *, const void *file_image, S32, S32 block)
{
	return AIL_set_sample_file(sample, file_image, block);
}

void __stdcall AIL_start_sample(HSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) startVoice(toVoice(sample)); }
void __stdcall AIL_stop_sample(HSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) pauseVoice(toVoice(sample)); }
void __stdcall AIL_resume_sample(HSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) resumeVoice(toVoice(sample)); }
void __stdcall AIL_end_sample(HSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) endVoice(toVoice(sample)); }

AIL_sample_callback __stdcall AIL_register_EOS_callback(HSAMPLE sample, AIL_sample_callback EOS)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return nullptr;
	AIL_sample_callback old = toVoice(sample)->sampleCallback;
	toVoice(sample)->sampleCallback = EOS;
	return old;
}

void __stdcall AIL_set_sample_volume_pan(HSAMPLE sample, F32 volume, F32 pan)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->volume = volume;
	toVoice(sample)->pan = pan;
	applySettings(toVoice(sample));
}

void __stdcall AIL_sample_volume_pan(HSAMPLE sample, F32 *volume, F32 *pan)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (volume) *volume = sample ? toVoice(sample)->volume : 0.0f;
	if (pan) *pan = sample ? toVoice(sample)->pan : 0.5f;
}

void __stdcall AIL_set_sample_playback_rate(HSAMPLE sample, S32 playback_rate)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->rate = playback_rate;
	applySettings(toVoice(sample));
}

S32 __stdcall AIL_sample_playback_rate(HSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? toVoice(sample)->rate : 0;
}

void __stdcall AIL_set_sample_loop_count(HSAMPLE sample, S32 count)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->loopCount = count;
	toVoice(sample)->loopsRemaining = count;
}

S32 __stdcall AIL_sample_loop_count(HSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? toVoice(sample)->loopsRemaining : 0;
}

void __stdcall AIL_sample_ms_position(HSAMPLE sample, S32 *total_ms, S32 *current_ms)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (sample) totalAndCurrentMs(toVoice(sample), total_ms, current_ms);
	else { if (total_ms) *total_ms = 0; if (current_ms) *current_ms = 0; }
}

void __stdcall AIL_set_sample_ms_position(HSAMPLE sample, S32 pos)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (sample) seekMs(toVoice(sample), pos);
}

void __stdcall AIL_set_sample_user_data(HSAMPLE sample, U32 index, S32 value)
{
	if (sample && index < USER_DATA_SLOTS) toVoice(sample)->userData[index] = value;
}

S32 __stdcall AIL_sample_user_data(HSAMPLE sample, U32 index)
{
	return (sample && index < USER_DATA_SLOTS) ? toVoice(sample)->userData[index] : 0;
}

// ------------------------------------------------------------------------------------------------
// 3D samples

H3DSAMPLE __stdcall AIL_allocate_3D_sample_handle(HPROVIDER) { return (H3DSAMPLE)newVoice(KIND_SAMPLE_3D); }
void __stdcall AIL_release_3D_sample_handle(H3DSAMPLE sample) { deleteVoice(toVoice(sample)); }

S32 __stdcall AIL_set_3D_sample_file(H3DSAMPLE sample, const void *file_image)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return 0;
	Voice *v = toVoice(sample);
	releaseSource(v);
	resetSettings(v);
	return setSampleImage(v, file_image);
}

void __stdcall AIL_start_3D_sample(H3DSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) startVoice(toVoice(sample)); }
void __stdcall AIL_stop_3D_sample(H3DSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) pauseVoice(toVoice(sample)); }
void __stdcall AIL_resume_3D_sample(H3DSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) resumeVoice(toVoice(sample)); }
void __stdcall AIL_end_3D_sample(H3DSAMPLE sample) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (sample) endVoice(toVoice(sample)); }

AIL_3dsample_callback __stdcall AIL_register_3D_EOS_callback(H3DSAMPLE sample, AIL_3dsample_callback EOS)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return nullptr;
	AIL_3dsample_callback old = toVoice(sample)->sample3DCallback;
	toVoice(sample)->sample3DCallback = EOS;
	return old;
}

void __stdcall AIL_set_3D_sample_volume(H3DSAMPLE sample, F32 volume)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->volume = volume;
	applySettings(toVoice(sample));
}

F32 __stdcall AIL_3D_sample_volume(H3DSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? toVoice(sample)->volume : 0.0f;
}

void __stdcall AIL_set_3D_sample_distances(H3DSAMPLE sample, F32 max_dist, F32 min_dist)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->maxDist = max_dist;
	toVoice(sample)->minDist = min_dist;
	applySettings(toVoice(sample));
}

void __stdcall AIL_set_3D_sample_playback_rate(H3DSAMPLE sample, S32 playback_rate)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->rate = playback_rate;
	applySettings(toVoice(sample));
}

S32 __stdcall AIL_3D_sample_playback_rate(H3DSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? toVoice(sample)->rate : 0;
}

void __stdcall AIL_set_3D_sample_occlusion(H3DSAMPLE sample, F32 occlusion)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (sample) toVoice(sample)->occlusion = occlusion;
}

void __stdcall AIL_set_3D_sample_effects_level(H3DSAMPLE, F32) {}

void __stdcall AIL_set_3D_sample_loop_count(H3DSAMPLE sample, U32 count)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample) return;
	toVoice(sample)->loopCount = (S32)count;
	toVoice(sample)->loopsRemaining = (S32)count;
}

U32 __stdcall AIL_3D_sample_loop_count(H3DSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return sample ? (U32)toVoice(sample)->loopsRemaining : 0;
}

U32 __stdcall AIL_3D_sample_length(H3DSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample || !toVoice(sample)->decoderInit) return 0;
	ma_uint64 frames = 0;
	ma_decoder_get_length_in_pcm_frames(&toVoice(sample)->decoder, &frames);
	return (U32)(frames * 2); // bytes of 16 bit mono, which is all a 3D sample can be
}

U32 __stdcall AIL_3D_sample_offset(H3DSAMPLE sample)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample || !toVoice(sample)->decoderInit) return 0;
	ma_uint64 cursor = 0;
	if (toVoice(sample)->soundInit)
		ma_sound_get_cursor_in_pcm_frames(&toVoice(sample)->sound, &cursor);
	return (U32)(cursor * 2);
}

void __stdcall AIL_set_3D_sample_offset(H3DSAMPLE sample, U32 offset)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!sample || !toVoice(sample)->soundInit) return;
	ma_sound_seek_to_pcm_frame(&toVoice(sample)->sound, offset / 2);
}

// ------------------------------------------------------------------------------------------------
// Streams

HSTREAM __stdcall AIL_open_stream(HDIGDRIVER, const char *filename, S32)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!filename || !g_fileOpen)
		return nullptr;

	void *handle = nullptr;
	if (!g_fileOpen(filename, &handle) || !handle)
	{
		setError("Could not open stream file");
		return nullptr;
	}

	Voice *v = newVoice(KIND_STREAM);
	v->fileHandle = handle;
	ma_decoder_config config = decoderConfig();
	if (ma_decoder_init(streamRead, streamSeek, v, &config, &v->decoder) != MA_SUCCESS)
	{
		setError("Could not decode stream file");
		deleteVoice(v);
		return nullptr;
	}
	v->decoderInit = true;
	if (!initLoopSource(v))
	{
		deleteVoice(v);
		return nullptr;
	}
	v->rate = (S32)v->nativeRate;
	return (HSTREAM)v;
}

void __stdcall AIL_close_stream(HSTREAM stream) { deleteVoice(toVoice(stream)); }
void __stdcall AIL_start_stream(HSTREAM stream) { std::lock_guard<std::recursive_mutex> lock(g_lock); if (stream) startVoice(toVoice(stream)); }

void __stdcall AIL_pause_stream(HSTREAM stream, S32 onoff)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!stream) return;
	if (onoff) pauseVoice(toVoice(stream));
	else resumeVoice(toVoice(stream));
}

AIL_stream_callback __stdcall AIL_register_stream_callback(HSTREAM stream, AIL_stream_callback callback)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!stream) return nullptr;
	AIL_stream_callback old = toVoice(stream)->streamCallback;
	toVoice(stream)->streamCallback = callback;
	return old;
}

void __stdcall AIL_set_stream_volume_pan(HSTREAM stream, F32 volume, F32 pan)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!stream) return;
	toVoice(stream)->volume = volume;
	toVoice(stream)->pan = pan;
	applySettings(toVoice(stream));
}

void __stdcall AIL_stream_volume_pan(HSTREAM stream, F32 *volume, F32 *pan)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (volume) *volume = stream ? toVoice(stream)->volume : 0.0f;
	if (pan) *pan = stream ? toVoice(stream)->pan : 0.5f;
}

void __stdcall AIL_set_stream_playback_rate(HSTREAM stream, S32 rate)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!stream) return;
	toVoice(stream)->rate = rate;
	applySettings(toVoice(stream));
}

S32 __stdcall AIL_stream_playback_rate(HSTREAM stream)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return stream ? toVoice(stream)->rate : 0;
}

void __stdcall AIL_set_stream_loop_count(HSTREAM stream, S32 count)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!stream) return;
	toVoice(stream)->loopCount = count;
	toVoice(stream)->loopsRemaining = count;
}

S32 __stdcall AIL_stream_loop_count(HSTREAM stream)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	return stream ? toVoice(stream)->loopsRemaining : 0;
}

void __stdcall AIL_set_stream_loop_block(HSTREAM, S32, S32) {}

void __stdcall AIL_stream_ms_position(HSTREAM stream, S32 *total_milliseconds, S32 *current_milliseconds)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (stream) totalAndCurrentMs(toVoice(stream), total_milliseconds, current_milliseconds);
	else { if (total_milliseconds) *total_milliseconds = 0; if (current_milliseconds) *current_milliseconds = 0; }
}

void __stdcall AIL_set_stream_ms_position(HSTREAM stream, S32 pos)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (stream) seekMs(toVoice(stream), pos);
}

// ------------------------------------------------------------------------------------------------
// Quick API: one shot sounds by file name

HAUDIO __stdcall AIL_quick_load_and_play(const char *filename, U32 loop_count, S32)
{
	HSTREAM stream = AIL_open_stream(&g_digitalDriver, filename, 0);
	if (!stream)
		return nullptr;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	Voice *v = toVoice(stream);
	v->kind = KIND_QUICK;
	v->loopCount = (S32)loop_count;
	startVoice(v);
	return (HAUDIO)v;
}

void __stdcall AIL_quick_unload(HAUDIO audio) { deleteVoice(toVoice(audio)); }

void __stdcall AIL_quick_set_volume(HAUDIO audio, F32 volume, F32)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!audio) return;
	toVoice(audio)->volume = volume;
	applySettings(toVoice(audio));
}

// ------------------------------------------------------------------------------------------------
// WAV parsing and IMA ADPCM decompression

S32 __stdcall AIL_WAV_info(const void *data, AILSOUNDINFO *info)
{
	if (!info)
		return 0;
	memset(info, 0, sizeof(*info));
	const size_t size = wavImageSize(data);
	if (size == 0)
		return 0;

	const unsigned char *p = (const unsigned char *)data;
	const unsigned char *end = p + size;
	const unsigned char *chunk = p + 12;
	U32 factSamples = 0;
	bool haveFmt = false, haveData = false;

	while (chunk + 8 <= end)
	{
		const ma_uint32 chunkSize = readLE32(chunk + 4);
		const unsigned char *body = chunk + 8;
		if (memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16)
		{
			info->format = readLE16(body);
			info->channels = readLE16(body + 2);
			info->rate = readLE32(body + 4);
			info->block_size = readLE16(body + 12);
			info->bits = readLE16(body + 14);
			haveFmt = true;
		}
		else if (memcmp(chunk, "fact", 4) == 0 && chunkSize >= 4)
		{
			factSamples = readLE32(body);
		}
		else if (memcmp(chunk, "data", 4) == 0)
		{
			info->data_ptr = body;
			const size_t available = (size_t)(end - body);
			info->data_len = (U32)(chunkSize < available ? chunkSize : available);
			haveData = true;
		}
		chunk = body + chunkSize + (chunkSize & 1);
	}

	if (!haveFmt || !haveData)
		return 0;

	info->initial_ptr = data;
	if (info->format == WAVE_FORMAT_PCM && info->bits > 0 && info->channels > 0)
		info->samples = info->data_len / (U32)(info->bits / 8 * info->channels);
	else if (factSamples)
		info->samples = factSamples;
	return 1;
}

S32 __stdcall AIL_decompress_ADPCM(const AILSOUNDINFO *info, void **outdata, U32 *outsize)
{
	if (outdata) *outdata = nullptr;
	if (outsize) *outsize = 0;
	if (!info || !info->initial_ptr || !outdata || !outsize)
		return 0;

	const size_t size = wavImageSize(info->initial_ptr);
	ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
	ma_decoder decoder;
	if (size == 0 || ma_decoder_init_memory(info->initial_ptr, size, &config, &decoder) != MA_SUCCESS)
		return 0;

	ma_uint32 channels = 0, rate = 0;
	ma_decoder_get_data_format(&decoder, nullptr, &channels, &rate, nullptr, 0);

	std::vector<ma_int16> pcm;
	ma_int16 buffer[4096];
	const ma_uint64 framesPerRead = sizeof(buffer) / sizeof(buffer[0]) / (channels ? channels : 1);
	for (;;)
	{
		ma_uint64 got = 0;
		ma_decoder_read_pcm_frames(&decoder, buffer, framesPerRead, &got);
		if (got == 0)
			break;
		pcm.insert(pcm.end(), buffer, buffer + got * channels);
	}
	ma_decoder_uninit(&decoder);

	const ma_uint32 dataBytes = (ma_uint32)(pcm.size() * sizeof(ma_int16));
	const ma_uint32 total = 44 + dataBytes;
	unsigned char *image = (unsigned char *)malloc(total);
	if (!image)
		return 0;

	memcpy(image, "RIFF", 4); writeLE32(image + 4, total - 8); memcpy(image + 8, "WAVE", 4);
	memcpy(image + 12, "fmt ", 4); writeLE32(image + 16, 16);
	writeLE16(image + 20, WAVE_FORMAT_PCM); writeLE16(image + 22, (ma_uint16)channels);
	writeLE32(image + 24, rate); writeLE32(image + 28, rate * channels * 2);
	writeLE16(image + 32, (ma_uint16)(channels * 2)); writeLE16(image + 34, 16);
	memcpy(image + 36, "data", 4); writeLE32(image + 40, dataBytes);
	if (dataBytes)
		memcpy(image + 44, pcm.data(), dataBytes);

	*outdata = image;
	*outsize = total;
	return 1;
}

void __stdcall AIL_mem_free_lock(void *ptr) { free(ptr); }

//-------------------------------------------------------------------------------------------------
// Video sound track: a push stream that plays silence while it runs dry, so it never ends early.
namespace
{
struct VideoAudio
{
	ma_data_source_base base;
	std::mutex lock;
	std::deque<float> samples;
	ma_uint32 channels = 0;
	ma_uint32 rate = 0;
	ma_sound sound;
	bool soundInit = false;
	bool started = false;
	float volume = 1.0f;
};
VideoAudio g_video;

ma_result videoRead(ma_data_source *, void *out, ma_uint64 frameCount, ma_uint64 *framesRead)
{
	float *dst = (float *)out;
	const ma_uint64 wanted = frameCount * g_video.channels;
	ma_uint64 copied = 0;
	{
		std::lock_guard<std::mutex> lock(g_video.lock);
		const ma_uint64 have = g_video.samples.size();
		copied = have < wanted ? have : wanted;
		std::copy(g_video.samples.begin(), g_video.samples.begin() + (size_t)copied, dst);
		g_video.samples.erase(g_video.samples.begin(), g_video.samples.begin() + (size_t)copied);
	}
	for (ma_uint64 i = copied; i < wanted; ++i)
		dst[i] = 0.0f;
	if (framesRead) *framesRead = frameCount;
	return MA_SUCCESS;
}
ma_result videoSeek(ma_data_source *, ma_uint64) { return MA_NOT_IMPLEMENTED; }
ma_result videoFormat(ma_data_source *, ma_format *format, ma_uint32 *channels, ma_uint32 *rate, ma_channel *map, size_t mapCap)
{
	if (format) *format = ma_format_f32;
	if (channels) *channels = g_video.channels;
	if (rate) *rate = g_video.rate;
	if (map) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, g_video.channels);
	return MA_SUCCESS;
}
ma_data_source_vtable g_videoVtable = { videoRead, videoSeek, videoFormat, nullptr, nullptr, nullptr, 0 };

// Caller holds g_lock.
void videoCloseLocked()
{
	if (g_video.soundInit)
	{
		ma_sound_uninit(&g_video.sound); // waits for the audio thread
		ma_data_source_uninit(&g_video.base);
		g_video.soundInit = false;
	}
	g_video.started = false;
	std::lock_guard<std::mutex> lock(g_video.lock);
	g_video.samples.clear();
	g_video.channels = 0;
	g_video.rate = 0;
}
}

void AoT_VideoAudio_Reset()
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	videoCloseLocked();
}

void AoT_VideoAudio_Close()
{
	AoT_VideoAudio_Reset();
}

void AoT_VideoAudio_Push(const float *interleaved, unsigned frames, unsigned channels, unsigned sampleRate)
{
	if (!interleaved || frames == 0 || channels == 0 || sampleRate == 0)
		return;
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (!g_engineInit)
		return;
	if (g_video.soundInit && (g_video.channels != channels || g_video.rate != sampleRate))
		videoCloseLocked();
	if (!g_video.soundInit)
	{
		{
			std::lock_guard<std::mutex> vlock(g_video.lock);
			g_video.channels = channels;
			g_video.rate = sampleRate;
		}
		ma_data_source_config config = ma_data_source_config_init();
		config.vtable = &g_videoVtable;
		if (ma_data_source_init(&config, &g_video.base) != MA_SUCCESS)
			return;
		if (ma_sound_init_from_data_source(&g_engine, &g_video.base, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, &g_video.sound) != MA_SUCCESS)
		{
			ma_data_source_uninit(&g_video.base);
			return;
		}
		ma_sound_set_volume(&g_video.sound, g_video.volume);
		g_video.soundInit = true;
	}
	std::lock_guard<std::mutex> vlock(g_video.lock);
	g_video.samples.insert(g_video.samples.end(), interleaved, interleaved + (size_t)frames * channels);
}

void AoT_VideoAudio_Play()
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	if (g_video.soundInit && !g_video.started)
	{
		ma_sound_start(&g_video.sound);
		g_video.started = true;
	}
}

void AoT_VideoAudio_SetVolume(float volume)
{
	std::lock_guard<std::recursive_mutex> lock(g_lock);
	g_video.volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume);
	if (g_video.soundInit)
		ma_sound_set_volume(&g_video.sound, g_video.volume);
}
