/* FAudio - XAudio Reimplementation for FNA
 *
 * Copyright (c) 2011-2018 Ethan Lee, Luigi Auriemma, and the MonoGame Team
 *
 * This software is provided 'as-is', without any express or implied warranty.
 * In no event will the authors be held liable for any damages arising from
 * the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software in a
 * product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 *
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 *
 * 3. This notice may not be removed or altered from any source distribution.
 *
 * Ethan "flibitijibibo" Lee <flibitijibibo@flibitijibibo.com>
 *
 */

#include "FAudio_internal.h"

#include <SDL.h>

/* Internal Types */

typedef struct FAudioPlatformDevice
{
	uint32_t bufferSize;
	SDL_AudioDeviceID device;
	FAudioWaveFormatExtensible format;
} FAudioPlatformDevice;

/* WaveFormatExtensible Helpers */

static inline uint32_t GetMask(uint16_t channels)
{
	if (channels == 1) return SPEAKER_MONO;
	if (channels == 2) return SPEAKER_STEREO;
	if (channels == 3) return SPEAKER_2POINT1;
	if (channels == 4) return SPEAKER_QUAD;
	if (channels == 5) return SPEAKER_4POINT1;
	if (channels == 6) return SPEAKER_5POINT1;
	if (channels == 8) return SPEAKER_7POINT1;
	FAudio_assert(0 && "Unrecognized speaker layout!");
	return SPEAKER_STEREO;
}

static inline void WriteWaveFormatExtensible(
	FAudioWaveFormatExtensible *fmt,
	int channels,
	int samplerate
) {
	fmt->Format.wBitsPerSample = 32;
	fmt->Format.wFormatTag = FAUDIO_FORMAT_EXTENSIBLE;
	fmt->Format.nChannels = channels;
	fmt->Format.nSamplesPerSec = samplerate;
	fmt->Format.nBlockAlign = (
		fmt->Format.nChannels *
		(fmt->Format.wBitsPerSample / 8)
	);
	fmt->Format.nAvgBytesPerSec = (
		fmt->Format.nSamplesPerSec *
		fmt->Format.nBlockAlign
	);
	fmt->Format.cbSize = sizeof(FAudioWaveFormatExtensible) - sizeof(FAudioWaveFormatEx);
	fmt->Samples.wValidBitsPerSample = 32;
	fmt->dwChannelMask = GetMask(fmt->Format.nChannels);
	FAudio_memcpy(&fmt->SubFormat, &DATAFORMAT_SUBTYPE_IEEE_FLOAT, sizeof(FAudioGUID));
}

/* Mixer Thread */

void FAudio_INTERNAL_MixCallback(void *userdata, Uint8 *stream, int len)
{
	FAudio *audio = (FAudio*) userdata;

	FAudio_zero(stream, len);
	if (audio->active)
	{
		FAudio_INTERNAL_UpdateEngine(
			audio,
			(float*) stream
		);
	}
}

/* Platform Functions */

void FAudio_PlatformAddRef()
{
	/* SDL tracks ref counts for each subsystem */
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		SDL_Log("SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
	}
	FAudio_INTERNAL_InitSIMDFunctions(
		SDL_HasSSE2(),
		SDL_HasNEON()
	);
}

void FAudio_PlatformRelease()
{
	/* SDL tracks ref counts for each subsystem */
	SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void FAudio_PlatformInit(FAudio *audio, uint32_t deviceIndex)
{
	FAudioPlatformDevice *device;
	SDL_AudioSpec want, have;

	/* Allocate a new device container*/
	device = (FAudioPlatformDevice*) audio->pMalloc(
		sizeof(FAudioPlatformDevice)
	);

	/* Build the device format.
	 * The most unintuitive part of this is the use of outputChannels
	 * instead of master.inputChannels. Bizarrely, the effect chain can
	 * dictate the _actual_ output channel count, and when the channel count
	 * mismatches, we have to add a staging buffer for effects to process on
	 * before ultimately copying the final result to the device. ARGH.
	 */
	want.freq = audio->master->master.inputSampleRate;
	want.format = AUDIO_F32;
	want.channels = audio->master->outputChannels;
	want.silence = 0;
	want.samples = 1024;
	want.callback = FAudio_INTERNAL_MixCallback;
	want.userdata = audio;

	/* Open the device, finally. */
	device->device = SDL_OpenAudioDevice(
		deviceIndex > 0 ? SDL_GetAudioDeviceName(deviceIndex - 1, 0) : NULL,
		0,
		&want,
		&have,
#if SDL_VERSION_ATLEAST(2, 0, 9)
		SDL_AUDIO_ALLOW_SAMPLES_CHANGE
#else
#warning Please update to SDL 2.0.9 ASAP!
		0
#endif
	);
	if (device->device == 0)
	{
		audio->pFree(device);
		SDL_Log("OpenAudioDevice failed: %s\n", SDL_GetError());
		FAudio_assert(0 && "Failed to open audio device!");
		return;
	}

	/* Write up the format */
	WriteWaveFormatExtensible(&device->format, have.channels, have.freq);
	device->bufferSize = have.samples;

	/* Give the output format to the engine */
	audio->updateSize = device->bufferSize;
	audio->mixFormat = &device->format;

	/* Also give some info to the master voice */
	audio->master->outputChannels = have.channels;
	audio->master->master.inputSampleRate = have.freq;

	/* Start the thread! */
	SDL_PauseAudioDevice(device->device, 0);

	audio->platform = device;
}

void FAudio_PlatformQuit(FAudio *audio)
{
	FAudioPlatformDevice *device = audio->platform;
	SDL_CloseAudioDevice(
		device->device
	);
	audio->pFree(device);
	audio->platform = NULL;
}

uint32_t FAudio_PlatformGetDeviceCount()
{
	return SDL_GetNumAudioDevices(0) + 1;
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len);

void FAudio_PlatformGetDeviceDetails(
	uint32_t index,
	FAudioDeviceDetails *details
) {
	const char *name, *envvar;
	int channels, rate;

	FAudio_zero(details, sizeof(FAudioDeviceDetails));
	if (index > FAudio_PlatformGetDeviceCount())
	{
		return;
	}

	details->DeviceID[0] = L'0' + index;
	if (index == 0)
	{
		name = "Default Device";
		details->Role = FAudioGlobalDefaultDevice;
	}
	else
	{
		name = SDL_GetAudioDeviceName(index - 1, 0);
		details->Role = FAudioNotDefaultDevice;
	}
	FAudio_UTF8_To_UTF16(
		name,
		(uint16_t*) details->DisplayName,
		sizeof(details->DisplayName)
	);

	/* TODO: SDL_GetAudioDeviceSpec! */
	envvar = SDL_getenv("SDL_AUDIO_FREQUENCY");
	if (!envvar || ((rate = SDL_atoi(envvar)) == 0))
	{
		rate = 48000;
	}
	envvar = SDL_getenv("SDL_AUDIO_CHANNELS");
	if (!envvar || ((channels = SDL_atoi(envvar)) == 0))
	{
		channels = 2;
	}
	WriteWaveFormatExtensible(&details->OutputFormat, channels, rate);
}

FAudioPlatformFixedRateSRC FAudio_PlatformInitFixedRateSRC(
	uint32_t channels,
	uint32_t inputRate,
	uint32_t outputRate
) {
	return (FAudioPlatformFixedRateSRC) SDL_NewAudioStream(
		AUDIO_F32,
		channels,
		inputRate,
		AUDIO_F32,
		channels,
		outputRate
	);
}

void FAudio_PlatformCloseFixedRateSRC(FAudioPlatformFixedRateSRC resampler)
{
	SDL_FreeAudioStream((SDL_AudioStream*) resampler);
}

uint32_t FAudio_PlatformResample(
	FAudioPlatformFixedRateSRC resampler,
	float *input,
	uint32_t inLen,
	float *output,
	uint32_t outLen
) {
	SDL_AudioStream *stream = (SDL_AudioStream*) resampler;
	SDL_AudioStreamPut(stream, input, inLen * sizeof(float));
	return SDL_AudioStreamGet(
		stream,
		output,
		outLen * sizeof(float)
	) / sizeof(float);
}

/* Threading */

FAudioThread FAudio_PlatformCreateThread(
	FAudioThreadFunc func,
	const char *name,
	void* data
) {
	return (FAudioThread) SDL_CreateThread(
		(SDL_ThreadFunction) func,
		name,
		data
	);
}

void FAudio_PlatformWaitThread(FAudioThread thread, int32_t *retval)
{
	SDL_WaitThread((SDL_Thread*) thread, retval);
}

void FAudio_PlatformThreadPriority(FAudioThreadPriority priority)
{
	SDL_SetThreadPriority((SDL_ThreadPriority) priority);
}

uint64_t FAudio_PlatformGetThreadID()
{
	return (uint64_t) SDL_ThreadID();
}

FAudioMutex FAudio_PlatformCreateMutex()
{
	return (FAudioMutex) SDL_CreateMutex();
}

void FAudio_PlatformDestroyMutex(FAudioMutex mutex)
{
	SDL_DestroyMutex((SDL_mutex*) mutex);
}

void FAudio_PlatformLockMutex(FAudioMutex mutex)
{
	SDL_LockMutex((SDL_mutex*) mutex);
}

void FAudio_PlatformUnlockMutex(FAudioMutex mutex)
{
	SDL_UnlockMutex((SDL_mutex*) mutex);
}

void FAudio_sleep(uint32_t ms)
{
	SDL_Delay(ms);
}

/* Time */

uint32_t FAudio_timems()
{
	return SDL_GetTicks();
}

/* FAudio I/O */

FAudioIOStream* FAudio_fopen(const char *path)
{
	FAudioIOStream *io = (FAudioIOStream*) SDL_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_RWops *rwops = SDL_RWFromFile(path, "rb");
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

FAudioIOStream* FAudio_memopen(void *mem, int len)
{
	FAudioIOStream *io = (FAudioIOStream*) FAudio_malloc(
		sizeof(FAudioIOStream)
	);
	SDL_RWops *rwops = SDL_RWFromMem(mem, len);
	io->data = rwops;
	io->read = (FAudio_readfunc) rwops->read;
	io->seek = (FAudio_seekfunc) rwops->seek;
	io->close = (FAudio_closefunc) rwops->close;
	io->lock = FAudio_PlatformCreateMutex();
	return io;
}

uint8_t* FAudio_memptr(FAudioIOStream *io, size_t offset)
{
	SDL_RWops *rwops = (SDL_RWops*) io->data;
	FAudio_assert(rwops->type == SDL_RWOPS_MEMORY);
	return rwops->hidden.mem.base + offset;
}

void FAudio_close(FAudioIOStream *io)
{
	io->close(io->data);
	FAudio_PlatformDestroyMutex((FAudioMutex) io->lock);
	FAudio_free(io);
}

/* UTF8->UTF16 Conversion, taken from PhysicsFS */

#define UNICODE_BOGUS_CHAR_VALUE 0xFFFFFFFF
#define UNICODE_BOGUS_CHAR_CODEPOINT '?'

static uint32_t FAudio_UTF8_CodePoint(const char **_str)
{
    const char *str = *_str;
    uint32_t retval = 0;
    uint32_t octet = (uint32_t) ((uint8_t) *str);
    uint32_t octet2, octet3, octet4;

    if (octet == 0)  /* null terminator, end of string. */
        return 0;

    else if (octet < 128)  /* one octet char: 0 to 127 */
    {
        (*_str)++;  /* skip to next possible start of codepoint. */
        return octet;
    } /* else if */

    else if ((octet > 127) && (octet < 192))  /* bad (starts with 10xxxxxx). */
    {
        /*
         * Apparently each of these is supposed to be flagged as a bogus
         *  char, instead of just resyncing to the next valid codepoint.
         */
        (*_str)++;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else if (octet < 224)  /* two octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 1;  /* skip to next possible start of codepoint. */
        retval = ((octet << 6) | (octet2 - 128));
        if ((retval >= 0x80) && (retval <= 0x7FF))
            return retval;
    } /* else if */

    else if (octet < 240)  /* three octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 2;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 12)) | ((octet2-128) << 6) | ((octet3-128)) );

        /* There are seven "UTF-16 surrogates" that are illegal in UTF-8. */
        switch (retval)
        {
            case 0xD800:
            case 0xDB7F:
            case 0xDB80:
            case 0xDBFF:
            case 0xDC00:
            case 0xDF80:
            case 0xDFFF:
                return UNICODE_BOGUS_CHAR_VALUE;
        } /* switch */

        /* 0xFFFE and 0xFFFF are illegal, too, so we check them at the edge. */
        if ((retval >= 0x800) && (retval <= 0xFFFD))
            return retval;
    } /* else if */

    else if (octet < 248)  /* four octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet -= (128+64+32+16);
        octet2 = (uint32_t) ((uint8_t) *(++str));
        if ((octet2 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet3 = (uint32_t) ((uint8_t) *(++str));
        if ((octet3 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet4 = (uint32_t) ((uint8_t) *(++str));
        if ((octet4 & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 3;  /* skip to next possible start of codepoint. */
        retval = ( ((octet << 18)) | ((octet2 - 128) << 12) |
                   ((octet3 - 128) << 6) | ((octet4 - 128)) );
        if ((retval >= 0x10000) && (retval <= 0x10FFFF))
            return retval;
    } /* else if */

    /*
     * Five and six octet sequences became illegal in rfc3629.
     *  We throw the codepoint away, but parse them to make sure we move
     *  ahead the right number of bytes and don't overflow the buffer.
     */

    else if (octet < 252)  /* five octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 4;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    else  /* six octets */
    {
        (*_str)++;  /* advance at least one byte in case of an error */
        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        octet = (uint32_t) ((uint8_t) *(++str));
        if ((octet & (128+64)) != 128)  /* Format isn't 10xxxxxx? */
            return UNICODE_BOGUS_CHAR_VALUE;

        *_str += 6;  /* skip to next possible start of codepoint. */
        return UNICODE_BOGUS_CHAR_VALUE;
    } /* else if */

    return UNICODE_BOGUS_CHAR_VALUE;
}

void FAudio_UTF8_To_UTF16(const char *src, uint16_t *dst, size_t len)
{
    len -= sizeof (uint16_t);   /* save room for null char. */
    while (len >= sizeof (uint16_t))
    {
        uint32_t cp = FAudio_UTF8_CodePoint(&src);
        if (cp == 0)
            break;
        else if (cp == UNICODE_BOGUS_CHAR_VALUE)
            cp = UNICODE_BOGUS_CHAR_CODEPOINT;

        if (cp > 0xFFFF)  /* encode as surrogate pair */
        {
            if (len < (sizeof (uint16_t) * 2))
                break;  /* not enough room for the pair, stop now. */

            cp -= 0x10000;  /* Make this a 20-bit value */

            *(dst++) = 0xD800 + ((cp >> 10) & 0x3FF);
            len -= sizeof (uint16_t);

            cp = 0xDC00 + (cp & 0x3FF);
        } /* if */

        *(dst++) = cp;
        len -= sizeof (uint16_t);
    } /* while */

    *dst = 0;
}

/* vim: set noexpandtab shiftwidth=8 tabstop=8: */
