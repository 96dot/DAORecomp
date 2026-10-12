// Host audio output: an SDL3 stream that plays the 5.1 frames the game's XAudio2
// submits (dao::apu::PopFrame). SDL converts to the device's layout (e.g. downmixes
// to stereo) and rate.
#include "stdafx.h"
#include "apu/audio.h"
#include "apu/audio_output.h"

#include <SDL3/SDL.h>

namespace dao::apu
{
    namespace
    {
        SDL_AudioStream* s_stream = nullptr;
        uint64_t s_underruns = 0;
        bool s_gameAudioStarted = false; // (the silence before the game's first frame isn't an underrun)
        FILE* s_dump = nullptr; // DAO_AUDIO_DUMP=<path.wav>: what's played, as 6-channel float WAV
        uint32_t s_dumpBytes = 0;

        void UpdateDumpHeader()
        {
            struct
            {
                char riff[4]; uint32_t riffSize; char wave[4];
                char fmt[4]; uint32_t fmtSize; uint16_t format, channels; uint32_t rate, byteRate;
                uint16_t blockAlign, bits;
                char data[4]; uint32_t dataSize;
            } header = { { 'R', 'I', 'F', 'F' }, 36 + s_dumpBytes, { 'W', 'A', 'V', 'E' }, { 'f', 'm', 't', ' ' }, 16,
                3, uint16_t(kFrameChannels), 48000, 48000 * kFrameChannels * 4, uint16_t(kFrameChannels * 4), 32,
                { 'd', 'a', 't', 'a' }, s_dumpBytes };
            fseek(s_dump, 0, SEEK_SET);
            fwrite(&header, sizeof(header), 1, s_dump);
            fseek(s_dump, 0, SEEK_END);
        }

        // Called by SDL's audio thread whenever the device needs more data.
        void SDLCALL FeedDevice(void*, SDL_AudioStream* stream, int additionalAmount, int)
        {
            constexpr int kFrameBytes = int(kFrameSampleCount * sizeof(float));
            float planar[kFrameSampleCount];
            float interleaved[kFrameSampleCount];
            while (additionalAmount > 0)
            {
                if (PopFrame(planar))
                {
                    s_gameAudioStarted = true;
                    for (uint32_t s = 0; s < kFrameChannelSamples; s++)
                    {
                        for (uint32_t c = 0; c < kFrameChannels; c++)
                            interleaved[s * kFrameChannels + c] = planar[c * kFrameChannelSamples + s];
                    }
                }
                else
                {
                    // The game hasn't produced audio in time (or isn't running audio yet).
                    memset(interleaved, 0, sizeof(interleaved));
                    if (s_gameAudioStarted)
                    {
                        const uint64_t underruns = ++s_underruns;
                        if (underruns == 100 || underruns == 1000 || underruns % 10000 == 0)
                            LOGW("Audio output: game audio is not keeping up ({} underruns so far)", underruns);
                    }
                }
                SDL_PutAudioStreamData(stream, interleaved, kFrameBytes);
                if (s_dump)
                {
                    fwrite(interleaved, kFrameBytes, 1, s_dump);
                    s_dumpBytes += kFrameBytes;
                    if ((s_dumpBytes / kFrameBytes) % 188 == 0) // ~1 s
                        UpdateDumpHeader();
                }
                additionalAmount -= kFrameBytes;
            }
        }
    }

    bool InitializeAudioOutput()
    {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            LOGW("Audio output: SDL audio init failed: {}", SDL_GetError());
            return false;
        }
        const SDL_AudioSpec spec{ SDL_AUDIO_F32, int(kFrameChannels), 48000 };
        s_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, FeedDevice, nullptr);
        if (!s_stream)
        {
            LOGW("Audio output: can't open the playback device: {}", SDL_GetError());
            return false;
        }
        if (const char* dump = getenv("DAO_AUDIO_DUMP"))
        {
            s_dump = fopen(dump, "wb");
            if (s_dump)
                UpdateDumpHeader();
        }
        SDL_ResumeAudioStreamDevice(s_stream);
        LOGI("Audio output: {} via SDL {}", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?",
            SDL_GetVersion());
        return true;
    }
}
