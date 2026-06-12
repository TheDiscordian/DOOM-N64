// N64 sound backend using libdragon mixer (RSP microcode accelerated).

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdragon.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "i_system.h"
#include "n64_debug.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#define N64_AUDIO_FREQUENCY     22050
#define N64_AUDIO_NUM_BUFFERS   4
#define N64_DEFAULT_SFX_RATE    11025
#define N64_SFX_MAX_FREQUENCY   48000.0f
#define N64_MUSIC_CHANNEL_BUDGET 16
#define N64_MUSIC_PATH_MAX      96

// From s_sound.c
extern int numChannels;
extern int snd_MusicVolume;

char* sndserver_filename = "sndserver";

typedef struct
{
    waveform_t wave;
    int8_t* pcm;
    int length;
    int frequency;
    boolean loaded;
    boolean owns_pcm;
} n64_sfx_t;

typedef struct
{
    int handle;
    int sfx_id;
    int start_tic;
    boolean active;
} n64_voice_t;

typedef enum
{
    N64_MUSIC_FMT_NONE = 0,
    N64_MUSIC_FMT_XM64,
    N64_MUSIC_FMT_YM64,
    N64_MUSIC_FMT_MUS
} n64_music_format_t;

typedef struct
{
    int handle;
    int lumpnum;
    int channels;
    int mus_data_len;
    n64_music_format_t format;
    const void *mus_data;
    char lump_name[9];
    char asset_path[N64_MUSIC_PATH_MAX];
    boolean registered;
    boolean opened;
    boolean playing;
    boolean paused;
    boolean looping;
    xm64player_t xm;
    ym64player_t ym;
} n64_music_track_t;

static n64_sfx_t sfx_cache[NUMSFX];
static n64_voice_t voices[MIXER_MAX_CHANNELS];
static n64_music_track_t music_track;

static int voice_count;
static int mixer_channel_count;
static int music_first_channel;
static int music_channel_count;
static int next_handle = 1;
static int next_music_handle = 1;
static boolean sound_initialized;
static boolean music_initialized;
static boolean missing_music_warning_printed;

static uint16_t N64_ReadLE16(const uint8_t* p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t N64_ReadLE32(const uint8_t* p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int N64_NormalizeVolume(int volume)
{
    if (volume < 0)
        return 0;

    // Doom defaults are often 0..15, while mixer gain math expects 0..127.
    if (volume <= 15)
        return (volume * 127 + 7) / 15;

    if (volume > 127)
        return 127;

    return volume;
}

static void N64_SfxWaveRead(void* ctx, samplebuffer_t* sbuf, int wpos, int wlen, bool seeking)
{
    n64_sfx_t* sfx = (n64_sfx_t*)ctx;
    int available;
    int8_t* dst;

    (void)seeking;

    if (!sfx || !sfx->pcm || wlen <= 0)
        return;

    if (wpos < 0)
        wpos = 0;

    if (wpos >= sfx->length)
        return;

    available = sfx->length - wpos;
    if (available > wlen)
        available = wlen;

    dst = (int8_t*)samplebuffer_append(sbuf, available);
    memcpy(dst, sfx->pcm + wpos, (size_t)available);
}

static void N64_SetupWaveform(n64_sfx_t* slot, const char* name)
{
    memset(&slot->wave, 0, sizeof(slot->wave));
    slot->wave.name = name;
    slot->wave.bits = 8;
    slot->wave.channels = 1;
    slot->wave.frequency = (float)slot->frequency;
    slot->wave.len = slot->length;
    slot->wave.loop_len = 0;
    slot->wave.start = NULL;
    slot->wave.read = N64_SfxWaveRead;
    slot->wave.ctx = slot;
    slot->wave.state_size = 0;
}

static int N64_GetSfxLumpByName(const char* sfx_name)
{
    char lump_name[20];

    sprintf(lump_name, "ds%s", sfx_name);

    if (W_CheckNumForName(lump_name) == -1)
        return W_GetNumForName("dspistol");

    return W_GetNumForName(lump_name);
}

static int N64_ResolveRootSfx(int sfx_id)
{
    int root;
    int safety;

    root = sfx_id;
    safety = 0;

    while (S_sfx[root].link && safety < NUMSFX)
    {
        int next = (int)(S_sfx[root].link - S_sfx);
        if (next <= 0 || next >= NUMSFX || next == root)
            break;

        root = next;
        safety++;
    }

    return root;
}

static void N64_LoadRootSfx(int sfx_id)
{
    n64_sfx_t* slot;
    int lump;
    int raw_len;
    uint8_t* raw;
    int rate;
    int count;
    int i;
    int8_t* pcm;

    slot = &sfx_cache[sfx_id];
    if (slot->loaded)
        return;

    lump = N64_GetSfxLumpByName(S_sfx[sfx_id].name);
    raw_len = W_LumpLength(lump);
    raw = (uint8_t*)W_CacheLumpNum(lump, PU_STATIC);

    rate = N64_DEFAULT_SFX_RATE;
    count = 0;

    if (raw_len > 8)
    {
        rate = (int)N64_ReadLE16(raw + 2);
        count = (int)N64_ReadLE32(raw + 4);

        if (count <= 0 || count > raw_len - 8)
            count = raw_len - 8;
    }
    else if (raw_len > 0)
    {
        count = raw_len;
    }

    pcm = NULL;
    if (count > 0)
    {
        pcm = (int8_t*)malloc((size_t)count);
        if (!pcm)
            I_Error("I_InitSound: out of memory for SFX %s (%d bytes)",
                    S_sfx[sfx_id].name, count);

        if (raw_len > 8)
        {
            for (i = 0; i < count; i++)
                pcm[i] = (int8_t)((int)raw[8 + i] - 128);
        }
        else
        {
            for (i = 0; i < count; i++)
                pcm[i] = (int8_t)((int)raw[i] - 128);
        }
    }

    Z_Free(raw);

    slot->pcm = pcm;
    slot->length = count;
    slot->frequency = (rate > 0) ? rate : N64_DEFAULT_SFX_RATE;
    slot->loaded = true;
    slot->owns_pcm = true;

    S_sfx[sfx_id].lumpnum = lump;
    S_sfx[sfx_id].data = slot->pcm;

    N64_SetupWaveform(slot, S_sfx[sfx_id].name);
}

static void N64_AliasSfx(int sfx_id, int root_sfx_id)
{
    n64_sfx_t* dst;
    n64_sfx_t* src;

    dst = &sfx_cache[sfx_id];
    src = &sfx_cache[root_sfx_id];

    dst->pcm = src->pcm;
    dst->length = src->length;
    dst->frequency = src->frequency;
    dst->loaded = true;
    dst->owns_pcm = false;

    S_sfx[sfx_id].lumpnum = S_sfx[root_sfx_id].lumpnum;
    S_sfx[sfx_id].data = dst->pcm;

    N64_SetupWaveform(dst, S_sfx[sfx_id].name);
}

static boolean N64_EnsureSfxLoaded(int sfx_id)
{
    int root;

    if (sfx_id <= 0 || sfx_id >= NUMSFX)
        return false;

    if (sfx_cache[sfx_id].loaded)
        return sfx_cache[sfx_id].pcm != NULL && sfx_cache[sfx_id].length > 0;

    root = N64_ResolveRootSfx(sfx_id);
    if (root <= 0 || root >= NUMSFX)
        return false;

    if (!sfx_cache[root].loaded)
        N64_LoadRootSfx(root);

    if (!sfx_cache[root].loaded || !sfx_cache[root].pcm || sfx_cache[root].length <= 0)
        return false;

    if (sfx_id != root)
        N64_AliasSfx(sfx_id, root);

    return sfx_cache[sfx_id].loaded
        && sfx_cache[sfx_id].pcm != NULL
        && sfx_cache[sfx_id].length > 0;
}

static void N64_ClearVoice(int voice)
{
    voices[voice].active = false;
    voices[voice].handle = 0;
    voices[voice].sfx_id = 0;
    voices[voice].start_tic = 0;
}

static int N64_FindVoiceByHandle(int handle)
{
    int i;

    for (i = 0; i < voice_count; i++)
    {
        if (voices[i].active && voices[i].handle == handle)
            return i;
    }

    return -1;
}

static int N64_AllocVoice(void)
{
    int i;
    int oldest_tic;
    int oldest_voice;

    oldest_tic = gametic;
    oldest_voice = 0;

    for (i = 0; i < voice_count; i++)
    {
        if (!voices[i].active)
            return i;

        if (!mixer_ch_playing(i))
        {
            N64_ClearVoice(i);
            return i;
        }

        if (voices[i].start_tic < oldest_tic)
        {
            oldest_tic = voices[i].start_tic;
            oldest_voice = i;
        }
    }

    return oldest_voice;
}

static float N64_PitchScale(int pitch)
{
    if (pitch < 0)
        pitch = 0;
    else if (pitch > 255)
        pitch = 255;

    return powf(2.0f, ((float)pitch - 128.0f) / 64.0f);
}

static void N64_ApplyChannelParams(int voice, int vol, int sep, int pitch)
{
    int separation;
    int left;
    int right;
    float lvol;
    float rvol;
    float freq;
    int sfx_id;

    vol = N64_NormalizeVolume(vol);

    if (sep < 0)
        sep = 0;
    else if (sep > 255)
        sep = 255;

    separation = sep + 1;
    left = vol - ((vol * separation * separation) >> 16);

    separation -= 257;
    right = vol - ((vol * separation * separation) >> 16);

    if (left < 0)
        left = 0;
    else if (left > 127)
        left = 127;

    if (right < 0)
        right = 0;
    else if (right > 127)
        right = 127;

    lvol = (float)left / 127.0f;
    rvol = (float)right / 127.0f;
    mixer_ch_set_vol(voice, lvol, rvol);

    sfx_id = voices[voice].sfx_id;
    if (sfx_id > 0 && sfx_id < NUMSFX)
    {
        freq = sfx_cache[sfx_id].wave.frequency * N64_PitchScale(pitch);

        // Some DOOM2 SFX lumps are 22050 Hz, so pitch variance can exceed 22050.
        // Keep playback within configured SFX channel limits to avoid mixer assert.
        if (freq > N64_SFX_MAX_FREQUENCY)
            freq = N64_SFX_MAX_FREQUENCY;

        mixer_ch_set_freq(voice, freq);
    }
}

static void N64_ResetMusicTrack(boolean close_player)
{
    if (close_player && music_track.opened)
    {
        if (music_track.playing || music_track.paused)
        {
            if (music_track.format == N64_MUSIC_FMT_XM64)
                xm64player_stop(&music_track.xm);
            else if (music_track.format == N64_MUSIC_FMT_YM64)
                ym64player_stop(&music_track.ym);
            else if (music_track.format == N64_MUSIC_FMT_MUS)
                mixer_ch_stop(music_first_channel);
        }

        if (music_track.format == N64_MUSIC_FMT_XM64)
            xm64player_close(&music_track.xm);
        else if (music_track.format == N64_MUSIC_FMT_YM64)
            ym64player_close(&music_track.ym);
    }

    memset(&music_track, 0, sizeof(music_track));
}

static int N64_FindLumpNumByData(void* data)
{
    int i;

    if (!data)
        return -1;

    for (i = 0; i < numlumps; i++)
    {
        if (lumpcache[i] == data)
            return i;
    }

    return -1;
}

static void N64_GetLumpNameVariants(int lumpnum, char* lower, char* upper)
{
    char raw[9];
    int i;

    memset(raw, 0, sizeof(raw));
    memcpy(raw, lumpinfo[lumpnum].name, 8);
    raw[8] = '\0';

    for (i = 0; i < 8; i++)
    {
        if (raw[i] == ' ')
        {
            raw[i] = '\0';
            break;
        }
    }

    for (i = 0; i < 9; i++)
    {
        unsigned char c;

        if (!raw[i])
            break;

        c = (unsigned char)raw[i];
        lower[i] = (char)tolower(c);
        upper[i] = (char)toupper(c);
    }

    lower[i] = '\0';
    upper[i] = '\0';
}

static boolean N64_FileExists(const char* path)
{
    FILE* fp;

    fp = fopen(path, "rb");
    if (!fp)
        return false;

    fclose(fp);
    return true;
}

static boolean N64_ResolveMusicTrackAsset(int lumpnum)
{
    static const char* dirs[] = { "rom:/music/", "rom:/" };
    static const char* exts[] = { ".xm64", ".XM64", ".ym64", ".YM64" };
    static const n64_music_format_t fmts[] = {
        N64_MUSIC_FMT_XM64,
        N64_MUSIC_FMT_XM64,
        N64_MUSIC_FMT_YM64,
        N64_MUSIC_FMT_YM64
    };
    char lower[9];
    char upper[9];
    const char* names[2];
    int d;
    int n;
    int e;

    if (lumpnum < 0 || lumpnum >= numlumps)
        return false;

    N64_GetLumpNameVariants(lumpnum, lower, upper);
    names[0] = lower;
    names[1] = upper;

    snprintf(music_track.lump_name,
             sizeof(music_track.lump_name),
             "%s",
             lower);

    for (d = 0; d < (int)(sizeof(dirs) / sizeof(dirs[0])); d++)
    {
        for (n = 0; n < 2; n++)
        {
            for (e = 0; e < (int)(sizeof(exts) / sizeof(exts[0])); e++)
            {
                snprintf(music_track.asset_path,
                         sizeof(music_track.asset_path),
                         "%s%s%s",
                         dirs[d],
                         names[n],
                         exts[e]);

                if (N64_FileExists(music_track.asset_path))
                {
                    music_track.format = fmts[e];
                    return true;
                }
            }
        }
    }

    music_track.asset_path[0] = '\0';
    music_track.format = N64_MUSIC_FMT_NONE;
    return false;
}

// ============================================================
// MUS sampled synthesizer (example-port style interpretation)
// ============================================================

#define MUS_MAX_CHANNELS 16
#define MUS_MAX_VOICES 16
#define MUS_PERC_CHANNEL 15
#define MUS_PERC_INSTRUMENT_BASE 100
#define MUS_NUM_MIDI_INSTRUMENTS 182
#define MUS_DEFAULT_CHANNEL_PAN 64
#define MUS_DEFAULT_CHANNEL_VOLUME 100
#define MUS_BANK_PATH_MAX N64_MUSIC_PATH_MAX
#define MUS_TICK_RATE 140
#define MUS_RELEASE_NUM 7
#define MUS_RELEASE_SHIFT 3
#define MUS_PERC_DECAY 1764
#define MUS_DEBUG_EVENT_LOG_BUDGET 24
#define MUS_DEBUG_SILENCE_SAMPLES (N64_AUDIO_FREQUENCY * 2)
#define MUS_BANK_V2_MAGIC 0x3253554Du
#define MUS_BANK_FLAG_ADPCM_IMA4 0x0001u

#define MUS_FALLBACK_NONE 0
#define MUS_FALLBACK_TRIANGLE 1
#define MUS_FALLBACK_NOISE 2

typedef struct
{
    uint32_t rate;
    uint32_t loop;
    uint32_t length;
    uint16_t base;
    int8_t sample[8];
} n64_mus_bank_header_t;

typedef struct
{
    uint32_t rate;
    uint32_t loop;
    uint32_t length;
    uint16_t base;
    uint16_t flags;
    uint32_t payload_len;
    int16_t adpcm_init_predictor;
    uint8_t adpcm_init_step_index;
    int16_t adpcm_loop_predictor;
    uint8_t adpcm_loop_step_index;
} n64_mus_bank_header_v2_t;

typedef enum
{
    N64_MUS_SAMPLE_PCM8 = 0,
    N64_MUS_SAMPLE_ADPCM_IMA4 = 1
} n64_mus_sample_encoding_t;

typedef struct
{
    int8_t *wave;
    uint32_t loop;
    uint32_t length;
    uint32_t rate;
    uint16_t base;
    uint32_t data_len;
    uint8_t encoding;
    int16_t adpcm_init_predictor;
    uint8_t adpcm_init_step_index;
    int16_t adpcm_loop_predictor;
    uint8_t adpcm_loop_step_index;
    boolean loaded;
} n64_mus_instrument_t;

typedef struct
{
    int32_t pitch;
    int32_t lvol;
    int32_t rvol;
    int16_t note_voice[128];
    uint8_t instrument;
    uint8_t volume;
    uint8_t pan;
    uint8_t _pad;
} n64_mus_channel_t;

typedef struct
{
    const int8_t *wave;
    uint32_t index;
    uint32_t step;
    uint32_t loop;
    uint32_t length;
    uint32_t phase;
    uint32_t phase_inc;
    uint32_t start_seq;
    int      perc_left;
    int32_t  lvol;
    int32_t  rvol;
    int16_t  channel;
    uint8_t  instrument;
    uint8_t  note;
    uint8_t  active;
    uint8_t  releasing;
    uint8_t  fallback_mode;
    uint8_t  percussion;
    uint8_t  adpcm_active;
    uint8_t  adpcm_step_index;
    uint16_t _pad2;
    uint32_t adpcm_sample_index;
    uint32_t adpcm_nibble_index;
    int16_t  adpcm_predictor;
} mus_voice_t;

typedef struct
{
    waveform_t    wave;
    const uint8_t *score;
    int            score_len;
    int            score_pos;
    int32_t        tick_delay;
    int            tick_frac;
    uint32_t       noise_lfsr;
    uint32_t       voice_seq;
    mus_voice_t    voices[MUS_MAX_VOICES];
    n64_mus_channel_t channels[MUS_MAX_CHANNELS];
    uint16_t       active_mask;     /* bit v set iff voices[v].active */
    uint8_t        playing;
    uint8_t        looping;
    uint8_t        eos;
#if DOOM_N64_DEBUG
    uint8_t        dbg_event_budget;
    uint32_t       dbg_batches;
    uint32_t       dbg_events;
    uint32_t       dbg_note_on;
    uint32_t       dbg_note_off;
    uint32_t       dbg_read_calls;
    uint32_t       dbg_generated_samples;
    uint32_t       dbg_nonzero_samples;
    int32_t        dbg_peak_abs;
    uint8_t        dbg_reported_nonzero;
    uint8_t        dbg_reported_silence;
#endif
} n64_mus_player_t;

static n64_mus_player_t mus_player;
static n64_mus_instrument_t mus_instruments[MUS_NUM_MIDI_INSTRUMENTS];
static uint32_t mus_instrument_ptrs[MUS_NUM_MIDI_INSTRUMENTS];
static char mus_bank_path[MUS_BANK_PATH_MAX];
static boolean mus_bank_loaded;

// Cached scratch the per-sample mix is rendered into before being block-copied
// into the uncached mixer buffer; processed in MUS_SCRATCH_SAMPLES chunks so any
// poll length is covered. 8-byte aligned for 64-bit copies into the dest.
#define MUS_SCRATCH_SAMPLES 1024
static int16_t mus_scratch[MUS_SCRATCH_SAMPLES * 2] __attribute__((aligned(8)));

static uint16_t N64_BSwap16(uint16_t value)
{
    return (uint16_t)((value >> 8) | (value << 8));
}

static uint32_t N64_BSwap32(uint32_t value)
{
    return ((value & 0x000000FFu) << 24)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0xFF000000u) >> 24);
}

static const int8_t mus_adpcm_index_table[16] = {
    -1, -1, -1, -1,
     2,  4,  6,  8,
    -1, -1, -1, -1,
     2,  4,  6,  8
};

static const int16_t mus_adpcm_step_table[89] = {
       7,    8,    9,   10,   11,   12,   13,   14,
      16,   17,   19,   21,   23,   25,   28,   31,
      34,   37,   41,   45,   50,   55,   60,   66,
      73,   80,   88,   97,  107,  118,  130,  143,
     157,  173,  190,  209,  230,  253,  279,  307,
     337,  371,  408,  449,  494,  544,  598,  658,
     724,  796,  876,  963, 1060, 1166, 1282, 1411,
    1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493,10442,11487,12635,13899,
   15289,16818,18500,20350,22385,24623,27086,29794,
   32767
};

static uint16_t N64_ReadBE16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t N64_ReadBE32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)
         | (uint32_t)p[3];
}

static int N64_MusSampleCountFromLength(uint32_t length)
{
    return (int)(length >> 16);
}

static boolean N64_MusHeaderCandidateValid(uint32_t rate,
                                           uint32_t loop,
                                           uint32_t length,
                                           uint16_t base)
{
    int sample_len;

    sample_len = N64_MusSampleCountFromLength(length);
    if (sample_len <= 0 || sample_len > (1 << 20))
        return false;

    if (rate == 0 || rate > 96000)
        return false;

    if (loop > length)
        return false;

    if (base > 127)
        return false;

    return true;
}

static boolean N64_MusParseBankHeaderV2(const uint8_t *raw,
                                        n64_mus_bank_header_v2_t *out)
{
    uint32_t magic;
    uint32_t rate;
    uint32_t loop;
    uint32_t length;
    uint16_t base;
    uint16_t flags;
    uint32_t payload_len;
    int16_t init_predictor;
    uint8_t init_step;
    int16_t loop_predictor;
    uint8_t loop_step;
    int sample_len;
    uint32_t min_payload;

    if (!raw || !out)
        return false;

    magic = N64_ReadLE32(raw + 0);
    if (magic == MUS_BANK_V2_MAGIC)
    {
        rate = N64_ReadLE32(raw + 4);
        loop = N64_ReadLE32(raw + 8);
        length = N64_ReadLE32(raw + 12);
        base = N64_ReadLE16(raw + 16);
        flags = N64_ReadLE16(raw + 18);
        payload_len = N64_ReadLE32(raw + 20);
        init_predictor = (int16_t)N64_ReadLE16(raw + 24);
        init_step = raw[26];
        loop_predictor = (int16_t)N64_ReadLE16(raw + 28);
        loop_step = raw[30];
    }
    else if (N64_ReadBE32(raw + 0) == MUS_BANK_V2_MAGIC)
    {
        rate = N64_ReadBE32(raw + 4);
        loop = N64_ReadBE32(raw + 8);
        length = N64_ReadBE32(raw + 12);
        base = N64_ReadBE16(raw + 16);
        flags = N64_ReadBE16(raw + 18);
        payload_len = N64_ReadBE32(raw + 20);
        init_predictor = (int16_t)N64_ReadBE16(raw + 24);
        init_step = raw[27];
        loop_predictor = (int16_t)N64_ReadBE16(raw + 28);
        loop_step = raw[31];
    }
    else
    {
        return false;
    }

    if (!N64_MusHeaderCandidateValid(rate, loop, length, base))
        return false;

    sample_len = N64_MusSampleCountFromLength(length);
    min_payload = (sample_len > 1) ? (uint32_t)((sample_len - 1 + 1) / 2) : 0;

    if ((flags & MUS_BANK_FLAG_ADPCM_IMA4) && payload_len < min_payload)
        return false;

    if (payload_len > (1u << 20))
        return false;

    out->rate = rate;
    out->loop = loop;
    out->length = length;
    out->base = base;
    out->flags = flags;
    out->payload_len = payload_len;
    out->adpcm_init_predictor = init_predictor;
    out->adpcm_init_step_index = init_step;
    out->adpcm_loop_predictor = loop_predictor;
    out->adpcm_loop_step_index = loop_step;
    return true;
}

static uint8_t N64_MusClampAdpcmStepIndex(int index)
{
    if (index < 0)
        return 0;
    if (index > 88)
        return 88;
    return (uint8_t)index;
}

static int16_t N64_MusDecodeAdpcmNibble(int16_t predictor,
                                        uint8_t *step_index,
                                        uint8_t nibble)
{
    int step;
    int diffq;
    int next_index;
    int value;

    if (!step_index)
        return predictor;

    step = mus_adpcm_step_table[*step_index];
    diffq = step >> 3;

    if (nibble & 1)
        diffq += step >> 2;
    if (nibble & 2)
        diffq += step >> 1;
    if (nibble & 4)
        diffq += step;

    value = predictor;
    if (nibble & 8)
        value -= diffq;
    else
        value += diffq;

    if (value < -32768)
        value = -32768;
    else if (value > 32767)
        value = 32767;

    next_index = (int)*step_index + mus_adpcm_index_table[nibble & 0x0F];
    *step_index = N64_MusClampAdpcmStepIndex(next_index);

    return (int16_t)value;
}

static uint8_t N64_MusReadAdpcmNibble(const n64_mus_instrument_t *inst,
                                      uint32_t nibble_index)
{
    uint32_t byte_index;
    uint8_t packed;

    if (!inst || !inst->wave)
        return 0;

    byte_index = nibble_index >> 1;
    if (byte_index >= inst->data_len)
        return 0;

    packed = (uint8_t)inst->wave[byte_index];
    if (nibble_index & 1u)
        return (uint8_t)((packed >> 4) & 0x0F);

    return (uint8_t)(packed & 0x0F);
}

static void N64_MusVoiceResetAdpcmStart(mus_voice_t *mv,
                                        const n64_mus_instrument_t *inst)
{
    if (!mv || !inst)
        return;

    mv->adpcm_active = 1;
    mv->adpcm_predictor = inst->adpcm_init_predictor;
    mv->adpcm_step_index = N64_MusClampAdpcmStepIndex(inst->adpcm_init_step_index);
    mv->adpcm_sample_index = 0;
    mv->adpcm_nibble_index = 0;
}

static void N64_MusVoiceResetAdpcmLoop(mus_voice_t *mv,
                                       const n64_mus_instrument_t *inst)
{
    uint32_t loop_sample;
    uint32_t sample_count;

    if (!mv || !inst)
        return;

    sample_count = (uint32_t)N64_MusSampleCountFromLength(inst->length);
    loop_sample = (uint32_t)(inst->loop >> 16);

    if (loop_sample == 0 || loop_sample >= sample_count)
    {
        N64_MusVoiceResetAdpcmStart(mv, inst);
        return;
    }

    mv->adpcm_active = 1;
    mv->adpcm_predictor = inst->adpcm_loop_predictor;
    mv->adpcm_step_index = N64_MusClampAdpcmStepIndex(inst->adpcm_loop_step_index);
    mv->adpcm_sample_index = loop_sample;
    mv->adpcm_nibble_index = loop_sample;
}

static int16_t N64_MusVoiceDecodeAdpcmSample(mus_voice_t *mv,
                                             const n64_mus_instrument_t *inst,
                                             uint32_t target_sample,
                                             boolean wrapped)
{
    uint32_t sample_count;
    uint32_t total_nibbles;

    if (!mv || !inst)
        return 0;

    sample_count = (uint32_t)N64_MusSampleCountFromLength(inst->length);
    if (sample_count == 0)
        return 0;

    if (target_sample >= sample_count)
        target_sample = sample_count - 1;

    if (!mv->adpcm_active)
        N64_MusVoiceResetAdpcmStart(mv, inst);

    if (wrapped)
        N64_MusVoiceResetAdpcmLoop(mv, inst);

    if (target_sample < mv->adpcm_sample_index)
    {
        if (inst->loop && target_sample >= (uint32_t)(inst->loop >> 16))
            N64_MusVoiceResetAdpcmLoop(mv, inst);
        else
            N64_MusVoiceResetAdpcmStart(mv, inst);
    }

    total_nibbles = (sample_count > 1) ? sample_count - 1 : 0;

    while (mv->adpcm_sample_index < target_sample
        && mv->adpcm_nibble_index < total_nibbles)
    {
        uint8_t nibble;

        nibble = N64_MusReadAdpcmNibble(inst, mv->adpcm_nibble_index);
        mv->adpcm_predictor = N64_MusDecodeAdpcmNibble(mv->adpcm_predictor,
                                                       &mv->adpcm_step_index,
                                                       nibble);
        mv->adpcm_nibble_index++;
        mv->adpcm_sample_index++;
    }

    return mv->adpcm_predictor;
}

static int N64_MusClamp7(int value)
{
    if (value < 0)
        return 0;
    if (value > 127)
        return 127;
    return value;
}

static void N64_MusRebuildChannelGains(int ch)
{
    int pan;
    int vol;

    pan = mus_player.channels[ch].pan;
    vol = mus_player.channels[ch].volume;

    mus_player.channels[ch].lvol = (127 - pan) * vol;
    mus_player.channels[ch].rvol = pan * vol;
}

static void N64_MusResetChannels(void)
{
    int ch;

    for (ch = 0; ch < MUS_MAX_CHANNELS; ch++)
    {
        memset(mus_player.channels[ch].note_voice,
               0,
               sizeof(mus_player.channels[ch].note_voice));
        mus_player.channels[ch].pitch = 0x10000;
        mus_player.channels[ch].instrument = 0;
        mus_player.channels[ch].volume = MUS_DEFAULT_CHANNEL_VOLUME;
        mus_player.channels[ch].pan = MUS_DEFAULT_CHANNEL_PAN;
        N64_MusRebuildChannelGains(ch);
    }
}

static void N64_MusStopVoice(int voice)
{
    mus_voice_t *mv;
    int ch;
    int note;

    if (voice < 0 || voice >= MUS_MAX_VOICES)
        return;

    mv = &mus_player.voices[voice];
    ch = mv->channel;
    note = mv->note;

    if (ch >= 0 && ch < MUS_MAX_CHANNELS && note >= 0 && note < 128)
    {
        if (mus_player.channels[ch].note_voice[note] == (int16_t)(voice + 1))
            mus_player.channels[ch].note_voice[note] = 0;
    }

    memset(mv, 0, sizeof(*mv));
    mus_player.active_mask &= (uint16_t)~(1u << voice);
}

static void N64_MusStopAllVoices(void)
{
    int i;

    for (i = 0; i < MUS_MAX_VOICES; i++)
        N64_MusStopVoice(i);
}

static void N64_MusUnloadInstruments(void)
{
    int i;

    for (i = 0; i < MUS_NUM_MIDI_INSTRUMENTS; i++)
    {
        if (mus_instruments[i].loaded && mus_instruments[i].wave)
            free(mus_instruments[i].wave);
        memset(&mus_instruments[i], 0, sizeof(mus_instruments[i]));
    }
}

static boolean N64_MusLoadPointerTable(void)
{
    static const char* bank_paths[] = {
        "rom:/MUS/MIDI_Instruments",
        "rom:/MUS/MIDI_Instruments.bin",
        "rom:/music/MIDI_Instruments.bin",
        "rom:/MIDI_Instruments.bin"
    };
    size_t count;
    int i;
    FILE *fp;

    memset(mus_instrument_ptrs, 0, sizeof(mus_instrument_ptrs));
    mus_bank_loaded = false;
    mus_bank_path[0] = '\0';

    for (i = 0; i < (int)(sizeof(bank_paths) / sizeof(bank_paths[0])); i++)
    {
        fp = fopen(bank_paths[i], "rb");
        if (!fp)
            continue;

        count = fread(mus_instrument_ptrs,
                      sizeof(uint32_t),
                      MUS_NUM_MIDI_INSTRUMENTS,
                      fp);
        fclose(fp);

        if (count == MUS_NUM_MIDI_INSTRUMENTS)
        {
            snprintf(mus_bank_path, sizeof(mus_bank_path), "%s", bank_paths[i]);
            mus_bank_loaded = true;
            return true;
        }
    }

    return false;
}

static void N64_MusNormalizeBankHeader(n64_mus_bank_header_t *header)
{
    uint32_t rate;
    uint32_t loop;
    uint32_t length;
    uint16_t base;
    uint32_t swapped_length;
    int sample_len;
    int swapped_sample_len;

    rate = header->rate;
    loop = header->loop;
    length = header->length;
    base = header->base;

    sample_len = (int)(length >> 16);
    swapped_length = N64_BSwap32(length);
    swapped_sample_len = (int)(swapped_length >> 16);

    if ((sample_len <= 0 || sample_len > (1 << 20))
        && swapped_sample_len > 0
        && swapped_sample_len <= (1 << 20))
    {
        length = swapped_length;
        loop = N64_BSwap32(loop);
        rate = N64_BSwap32(rate);
        base = N64_BSwap16(base);
        sample_len = swapped_sample_len;
    }

    if (rate == 0 || rate > 96000)
        rate = N64_DEFAULT_SFX_RATE;

    if (base > 127)
    {
        uint16_t swapped_base = N64_BSwap16(base);
        base = (swapped_base <= 127) ? swapped_base : 60;
    }

    if (loop > length)
        loop = 0;

    if (sample_len <= 0)
    {
        length = 0;
        loop = 0;
    }

    header->rate = rate;
    header->loop = loop;
    header->length = length;
    header->base = base;
}

static boolean N64_MusLoadInstrument(int instrument)
{
    n64_mus_bank_header_t header;
    n64_mus_bank_header_v2_t header_v2;
    uint8_t header_v2_raw[32];
    uint32_t offsets[2];
    n64_mus_instrument_t *inst;
    uint32_t data_len;
    size_t alloc_len;
    int sample_len;
    int attempt;
    FILE *fp;

    if (instrument < 0 || instrument >= MUS_NUM_MIDI_INSTRUMENTS)
        return false;

    inst = &mus_instruments[instrument];
    if (inst->loaded && inst->wave)
        return true;

    if (!mus_bank_loaded || !mus_bank_path[0])
        return false;

    offsets[0] = mus_instrument_ptrs[instrument];
    offsets[1] = N64_BSwap32(mus_instrument_ptrs[instrument]);

    if (offsets[0] == 0 && offsets[1] == 0)
        return false;

    fp = fopen(mus_bank_path, "rb");
    if (!fp)
        return false;

    for (attempt = 0; attempt < 2; attempt++)
    {
        long offset;

        if (offsets[attempt] == 0)
            continue;

        if (attempt == 1 && offsets[1] == offsets[0])
            continue;

        offset = (long)offsets[attempt];

        if (fseek(fp, offset, SEEK_SET) != 0)
            continue;

        if (fread(header_v2_raw, 1, sizeof(header_v2_raw), fp) == sizeof(header_v2_raw)
            && N64_MusParseBankHeaderV2(header_v2_raw, &header_v2))
        {
            data_len = header_v2.payload_len;
            alloc_len = data_len ? (size_t)data_len : 1u;

            inst->wave = (int8_t*)malloc(alloc_len);
            if (!inst->wave)
                break;

            if (data_len > 0)
            {
                if (fseek(fp, offset + (long)sizeof(header_v2_raw), SEEK_SET) != 0
                    || fread(inst->wave, 1, (size_t)data_len, fp) != (size_t)data_len)
                {
                    free(inst->wave);
                    inst->wave = NULL;
                    continue;
                }
            }
            else
            {
                inst->wave[0] = 0;
            }

            inst->loop = header_v2.loop;
            inst->length = header_v2.length;
            inst->rate = header_v2.rate;
            inst->base = header_v2.base;
            inst->data_len = data_len;
            inst->encoding = (header_v2.flags & MUS_BANK_FLAG_ADPCM_IMA4)
                ? N64_MUS_SAMPLE_ADPCM_IMA4
                : N64_MUS_SAMPLE_PCM8;
            inst->adpcm_init_predictor = header_v2.adpcm_init_predictor;
            inst->adpcm_init_step_index = N64_MusClampAdpcmStepIndex(header_v2.adpcm_init_step_index);
            inst->adpcm_loop_predictor = header_v2.adpcm_loop_predictor;
            inst->adpcm_loop_step_index = N64_MusClampAdpcmStepIndex(header_v2.adpcm_loop_step_index);
            inst->loaded = true;
            fclose(fp);
            return true;
        }

        clearerr(fp);

        if (fseek(fp, offset, SEEK_SET) != 0)
            continue;

        if (fread(&header, 1, sizeof(header), fp) != sizeof(header))
            continue;

        N64_MusNormalizeBankHeader(&header);

        sample_len = (int)(header.length >> 16);
        if (sample_len <= 0 || sample_len > (1 << 20))
            continue;

        inst->wave = (int8_t*)malloc((size_t)sample_len);
        if (!inst->wave)
            break;

        if (fseek(fp, offset + (long)offsetof(n64_mus_bank_header_t, sample), SEEK_SET) != 0
            || fread(inst->wave, 1, (size_t)sample_len, fp) != (size_t)sample_len)
        {
            free(inst->wave);
            inst->wave = NULL;
            continue;
        }

        inst->loop = header.loop;
        inst->length = header.length;
        inst->rate = header.rate;
        inst->base = header.base;
        inst->data_len = (uint32_t)sample_len;
        inst->encoding = N64_MUS_SAMPLE_PCM8;
        inst->adpcm_init_predictor = (int16_t)((int16_t)inst->wave[0] << 8);
        inst->adpcm_init_step_index = 0;
        inst->adpcm_loop_predictor = inst->adpcm_init_predictor;
        inst->adpcm_loop_step_index = 0;
        inst->loaded = true;
        fclose(fp);
        return true;
    }

    fclose(fp);
    return false;
}

static void N64_MusPrepareInstruments(const uint8_t *mus_data, int total_len, int inst_count)
{
    int i;

    N64_MusUnloadInstruments();

    if (!mus_bank_loaded || !mus_data)
        return;

    if (inst_count < 0)
        inst_count = 0;

    if (16 + inst_count * 2 > total_len)
        inst_count = (total_len > 16) ? (total_len - 16) / 2 : 0;

    for (i = 0; i < inst_count; i++)
    {
        int instrument;

        instrument = (int)N64_ReadLE16(mus_data + 16 + i * 2);
        if (instrument < 0 || instrument >= MUS_NUM_MIDI_INSTRUMENTS)
            continue;

        N64_MusLoadInstrument(instrument);
    }
}

static uint32_t N64_MusComputeFallbackPhaseInc(int note, int32_t pitch)
{
    double freq;
    double pitch_scale;
    double inc;

    freq = 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
    pitch_scale = (double)pitch / 65536.0;
    if (pitch_scale <= 0.0)
        pitch_scale = 0.01;
    freq *= pitch_scale;

    inc = freq * (4294967296.0 / (double)N64_AUDIO_FREQUENCY);
    if (inc < 1.0)
        inc = 1.0;
    if (inc > 4294967295.0)
        inc = 4294967295.0;

    return (uint32_t)inc;
}

static uint32_t N64_MusComputeSampleStep(const n64_mus_instrument_t *inst,
                                         int note,
                                         int32_t pitch,
                                         boolean percussion)
{
    double sample_rate;
    double ratio;
    double step;

    if (!inst)
        return 1;

    sample_rate = inst->rate ? (double)inst->rate : (double)N64_DEFAULT_SFX_RATE;
    ratio = sample_rate / (double)N64_AUDIO_FREQUENCY;

    if (!percussion)
        ratio *= pow(2.0, ((double)note - (double)inst->base) / 12.0);

    ratio *= (double)pitch / 65536.0;
    if (ratio <= 0.0)
        ratio = 1.0 / 65536.0;

    step = ratio * 65536.0;
    if (step < 1.0)
        step = 1.0;
    if (step > 4294967295.0)
        step = 4294967295.0;

    return (uint32_t)step;
}

static void N64_MusRefreshChannelPitch(int ch)
{
    int i;

    if (ch < 0 || ch >= MUS_MAX_CHANNELS)
        return;

    for (i = 0; i < MUS_MAX_VOICES; i++)
    {
        mus_voice_t *mv;

        mv = &mus_player.voices[i];
        if (!mv->active || mv->channel != ch)
            continue;

        if (mv->wave
            && mv->instrument < MUS_NUM_MIDI_INSTRUMENTS
            && mus_instruments[mv->instrument].loaded)
        {
            mv->step = N64_MusComputeSampleStep(&mus_instruments[mv->instrument],
                                                mv->note,
                                                mus_player.channels[ch].pitch,
                                                mv->percussion ? true : false);
        }
        else if (mv->fallback_mode == MUS_FALLBACK_TRIANGLE)
        {
            mv->phase_inc = N64_MusComputeFallbackPhaseInc(mv->note,
                                                           mus_player.channels[ch].pitch);
        }
    }
}

static void N64_MusApplyChannelGains(int ch)
{
    int i;

    if (ch < 0 || ch >= MUS_MAX_CHANNELS)
        return;

    for (i = 0; i < MUS_MAX_VOICES; i++)
    {
        mus_voice_t *mv;

        mv = &mus_player.voices[i];
        if (!mv->active || mv->channel != ch || mv->releasing)
            continue;

        mv->lvol = mus_player.channels[ch].lvol;
        mv->rvol = mus_player.channels[ch].rvol;
    }
}

static void N64_MusReleaseVoice(int voice)
{
    mus_voice_t *mv;
    int ch;
    int note;

    if (voice < 0 || voice >= MUS_MAX_VOICES)
        return;

    mv = &mus_player.voices[voice];
    if (!mv->active)
        return;

    ch = mv->channel;
    note = mv->note;

    if (ch >= 0 && ch < MUS_MAX_CHANNELS && note >= 0 && note < 128)
    {
        if (mus_player.channels[ch].note_voice[note] == (int16_t)(voice + 1))
            mus_player.channels[ch].note_voice[note] = 0;
    }

    if (mv->fallback_mode == MUS_FALLBACK_NOISE)
    {
        N64_MusStopVoice(voice);
        return;
    }

    mv->releasing = 1;
    mv->loop = 0;
}

static int N64_MusAllocVoice(void)
{
    int i;
    int releasing = -1;
    int oldest = -1;
    uint32_t oldest_seq = UINT32_MAX;

    for (i = 0; i < MUS_MAX_VOICES; i++)
    {
        if (!mus_player.voices[i].active)
            return i;

        if (mus_player.voices[i].releasing && releasing < 0)
            releasing = i;

        if (mus_player.voices[i].start_seq < oldest_seq)
        {
            oldest_seq = mus_player.voices[i].start_seq;
            oldest = i;
        }
    }

    if (releasing >= 0)
    {
        N64_MusStopVoice(releasing);
        return releasing;
    }

    if (oldest >= 0)
    {
        N64_MusStopVoice(oldest);
        return oldest;
    }

    return -1;
}

static void N64_MusPlayNote(int ch, int note, int note_volume)
{
    n64_mus_channel_t *channel;
    n64_mus_instrument_t *inst;
    mus_voice_t *mv;
    int voice;
    int instrument;

    if (ch < 0 || ch >= MUS_MAX_CHANNELS)
        return;

    note &= 0x7F;

    channel = &mus_player.channels[ch];

    if (note_volume >= 0)
    {
        channel->volume = (uint8_t)N64_MusClamp7(note_volume);
        N64_MusRebuildChannelGains(ch);
        N64_MusApplyChannelGains(ch);
    }

    if (channel->note_voice[note] > 0)
        N64_MusReleaseVoice(channel->note_voice[note] - 1);

    voice = N64_MusAllocVoice();
    if (voice < 0)
        return;

    N64_MusStopVoice(voice);
    mv = &mus_player.voices[voice];

    memset(mv, 0, sizeof(*mv));
    mv->active = 1;
    mus_player.active_mask |= (uint16_t)(1u << voice);
    mv->channel = (int16_t)ch;
    mv->note = (uint8_t)note;
    mv->lvol = channel->lvol;
    mv->rvol = channel->rvol;
    mv->start_seq = ++mus_player.voice_seq;

    if (ch == MUS_PERC_CHANNEL)
    {
        instrument = note + MUS_PERC_INSTRUMENT_BASE;
        mv->percussion = 1;
    }
    else
    {
        instrument = channel->instrument;
        mv->percussion = 0;
    }

    inst = NULL;
    if (instrument >= 0
        && instrument < MUS_NUM_MIDI_INSTRUMENTS
        && (mus_instruments[instrument].loaded || N64_MusLoadInstrument(instrument)))
    {
        inst = &mus_instruments[instrument];
    }

    if (inst && inst->wave)
    {
        mv->instrument = (uint8_t)instrument;
        mv->wave = inst->wave;
        mv->loop = inst->loop;
        mv->length = inst->length;
        mv->step = N64_MusComputeSampleStep(inst,
                                            note,
                                            channel->pitch,
                                            mv->percussion ? true : false);

        if (inst->encoding == N64_MUS_SAMPLE_ADPCM_IMA4)
            N64_MusVoiceResetAdpcmStart(mv, inst);
        else
            mv->adpcm_active = 0;
    }
    else
    {
        mv->adpcm_active = 0;
        if (mv->percussion)
        {
            mv->fallback_mode = MUS_FALLBACK_NOISE;
            mv->perc_left = MUS_PERC_DECAY;
        }
        else
        {
            mv->fallback_mode = MUS_FALLBACK_TRIANGLE;
            mv->phase_inc = N64_MusComputeFallbackPhaseInc(note, channel->pitch);
        }
    }

    channel->note_voice[note] = (int16_t)(voice + 1);

    #if DOOM_N64_DEBUG
    mus_player.dbg_note_on++;
    #endif
}

static void N64_MusReleaseNote(int ch, int note)
{
    int voice;

    if (ch < 0 || ch >= MUS_MAX_CHANNELS)
        return;

    note &= 0x7F;

    voice = mus_player.channels[ch].note_voice[note] - 1;
    if (voice >= 0 && voice < MUS_MAX_VOICES)
    {
        N64_MusReleaseVoice(voice);
        #if DOOM_N64_DEBUG
        mus_player.dbg_note_off++;
        #endif
    }
}

static boolean N64_IsMus(const void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    return data != NULL && len >= 16
        && d[0] == 'M' && d[1] == 'U' && d[2] == 'S' && d[3] == '\x1a';
}

static boolean N64_MusReadByte(uint8_t *out)
{
    if (mus_player.score_pos >= mus_player.score_len)
    {
        mus_player.eos = 1;
        return false;
    }

    *out = mus_player.score[mus_player.score_pos++];
    return true;
}

static void N64_MusProcessBatch(void)
{
    uint8_t ev, b, ctrl, val, note_byte, note;
    int last, ch, type;
    int note_volume;
    uint16_t delay;

    #if DOOM_N64_DEBUG
    mus_player.dbg_batches++;
    #endif

    for (;;)
    {
        if (!N64_MusReadByte(&ev))
            return;

        last = (ev >> 7) & 1;
        ch   = ev & 0x0F;
        type = (ev >> 4) & 0x07;
        #if DOOM_N64_DEBUG
        mus_player.dbg_events++;

        if (mus_player.dbg_event_budget > 0)
        {
            N64_DEBUGF("MUS event[%u]: type=%d ch=%d last=%d pos=%d\n",
                   (unsigned)mus_player.dbg_events,
                   type,
                   ch,
                   last,
                   mus_player.score_pos - 1);
            mus_player.dbg_event_budget--;
        }
        #endif

        switch (type)
        {
        case 0: /* release note: 1 byte (note) */
            if (!N64_MusReadByte(&note))
                return;
            N64_MusReleaseNote(ch, note);
            break;

        case 1: /* play note: 1-2 bytes */
            if (!N64_MusReadByte(&note_byte))
                return;
            note = note_byte & 0x7F;
            note_volume = -1;

            if (note_byte & 0x80)
            {
                if (!N64_MusReadByte(&b))
                    return;
                note_volume = (int)b;
            }

            N64_MusPlayNote(ch, note, note_volume);

            #if DOOM_N64_DEBUG
            if (mus_player.dbg_event_budget > 0)
            {
                N64_DEBUGF("MUS note on: ch=%d note=%d vol=%d inst=%d\n",
                       ch,
                       note,
                       note_volume,
                       mus_player.channels[ch].instrument);
                mus_player.dbg_event_budget--;
            }
            #endif
            break;

        case 2: /* pitch wheel: 1 byte */
            if (!N64_MusReadByte(&b))
                return;
            mus_player.channels[ch].pitch = ((int32_t)b << 6) - ((int32_t)b << 2) + 58180;
            N64_MusRefreshChannelPitch(ch);
            break;

        case 3: /* system event: 1 byte (ignored) */
            if (!N64_MusReadByte(&b))
                return;
            break;

        case 4: /* change controller: 2 bytes */
            if (!N64_MusReadByte(&ctrl) || !N64_MusReadByte(&val))
                return;

            if (ctrl == 0)
            {
                mus_player.channels[ch].instrument = (uint8_t)N64_MusClamp7((int)val);
                mus_player.channels[ch].pitch = 0x10000;
                N64_MusRefreshChannelPitch(ch);
            }
            else if (ctrl == 3)
            {
                mus_player.channels[ch].volume = (uint8_t)N64_MusClamp7((int)val);
                N64_MusRebuildChannelGains(ch);
                N64_MusApplyChannelGains(ch);
            }
            else if (ctrl == 4)
            {
                mus_player.channels[ch].pan = (uint8_t)N64_MusClamp7((int)val);
                N64_MusRebuildChannelGains(ch);
                N64_MusApplyChannelGains(ch);
            }

            #if DOOM_N64_DEBUG
            if (mus_player.dbg_event_budget > 0)
            {
                N64_DEBUGF("MUS ctrl: ch=%d ctrl=%d val=%d\n",
                       ch,
                       ctrl,
                       val);
                mus_player.dbg_event_budget--;
            }
            #endif
            break;

        case 6: /* score end */
            N64_DEBUGF("MUS score end: events=%u notes_on=%u notes_off=%u pos=%d/%d\n",
                   (unsigned)mus_player.dbg_events,
                   (unsigned)mus_player.dbg_note_on,
                   (unsigned)mus_player.dbg_note_off,
                   mus_player.score_pos,
                   mus_player.score_len);
            mus_player.eos = 1;
            return;

        default: /* case 5 (end of measure) and 7 (unused): no extra bytes */
            if (type == 7)
                N64_DEBUGF("MUS warning: reserved event type 7 at pos=%d\n", mus_player.score_pos - 1);
            break;
        }

        if (!last)
            continue;

        /* last event in batch: read variable-length delay */
        delay = 0;
        do
        {
            if (!N64_MusReadByte(&b))
                return;
            delay = (uint16_t)(delay * 128 + (b & 0x7F));
        } while (b & 0x80);

        mus_player.tick_delay = (int32_t)delay;

        #if DOOM_N64_DEBUG
        if (mus_player.dbg_event_budget > 0)
        {
            N64_DEBUGF("MUS delay: %u ticks\n", (unsigned)delay);
            mus_player.dbg_event_budget--;
        }
        #endif

        return;
    }
}

static void N64_MusResetPlayback(void)
{
    mus_player.score_pos  = 0;
    mus_player.tick_delay = 0;
    mus_player.tick_frac  = 0;
    mus_player.noise_lfsr = 0xDEADBEEFu;
    mus_player.eos        = 0;
    mus_player.voice_seq  = 0;

    N64_MusResetChannels();
    N64_MusStopAllVoices();
}

static void N64_MusRenderVoice(int voice, int32_t *mix_l, int32_t *mix_r)
{
    mus_voice_t *mv;
    int16_t sample;

    mv = &mus_player.voices[voice];
    if (!mv->active)
        return;

    sample = 0;

    if (mv->wave)
    {
        boolean wrapped;
        uint32_t sample_index;
        const n64_mus_instrument_t *inst;

        wrapped = false;
        inst = NULL;

        for (;;)
        {
            if (mv->index < mv->length)
                break;

            if (mv->loop && !mv->releasing && mv->loop < mv->length)
            {
                mv->index = mv->loop + (mv->index - mv->length);
                wrapped = true;
            }
            else
            {
                N64_MusStopVoice(voice);
                return;
            }
        }

        sample_index = mv->index >> 16;
        if (mv->instrument < MUS_NUM_MIDI_INSTRUMENTS
            && mus_instruments[mv->instrument].loaded)
        {
            inst = &mus_instruments[mv->instrument];
        }

        if (mv->adpcm_active
            && inst
            && inst->encoding == N64_MUS_SAMPLE_ADPCM_IMA4)
        {
            sample = N64_MusVoiceDecodeAdpcmSample(mv,
                                                   inst,
                                                   sample_index,
                                                   wrapped ? true : false);
        }
        else
        {
            sample = (int16_t)((int16_t)mv->wave[sample_index] << 8);
        }

        mv->index += mv->step ? mv->step : 1;
    }
    else if (mv->fallback_mode == MUS_FALLBACK_NOISE)
    {
        if (mv->perc_left <= 0)
        {
            N64_MusStopVoice(voice);
            return;
        }

        mus_player.noise_lfsr ^= mus_player.noise_lfsr >> 13;
        mus_player.noise_lfsr ^= mus_player.noise_lfsr << 17;
        mus_player.noise_lfsr ^= mus_player.noise_lfsr >> 5;

        sample = (int16_t)(mus_player.noise_lfsr >> 16);
        sample = (int16_t)((int32_t)sample * mv->perc_left / MUS_PERC_DECAY);
        mv->perc_left--;
    }
    else
    {
        uint32_t half;

        mv->phase += mv->phase_inc;
        half = (mv->phase < 0x80000000u)
             ? (mv->phase * 2u)
             : ((0xFFFFFFFFu - mv->phase) * 2u);
        sample = (int16_t)((int32_t)(half >> 16) - 32768);
    }

    *mix_l += ((int32_t)sample * mv->lvol) >> 15;
    *mix_r += ((int32_t)sample * mv->rvol) >> 15;

    if (mv->releasing)
    {
        mv->lvol = (mv->lvol * MUS_RELEASE_NUM) >> MUS_RELEASE_SHIFT;
        mv->rvol = (mv->rvol * MUS_RELEASE_NUM) >> MUS_RELEASE_SHIFT;

        if (mv->lvol <= 1 && mv->rvol <= 1)
            N64_MusStopVoice(voice);
    }
}

static void N64_MusWaveRead(void *ctx, samplebuffer_t *sbuf,
                            int wpos, int wlen, bool seeking)
{
    int16_t *buf;
    int i, v;
#if DOOM_N64_DEBUG
    int32_t sum;
    int32_t abs_sum;
#endif
    int32_t mix_l;
    int32_t mix_r;

    (void)ctx;

    #if DOOM_N64_DEBUG
    mus_player.dbg_read_calls++;

    if (mus_player.dbg_read_calls == 1)
    {
        N64_DEBUGF("MUS read begin: wpos=%d wlen=%d seeking=%d\n",
               wpos,
               wlen,
               seeking ? 1 : 0);
    }
    #endif

    if (seeking)
        N64_MusResetPlayback();

    buf = (int16_t *)samplebuffer_append(sbuf, wlen);
    if (!buf)
        return;

    {
        int remaining = wlen;
        int16_t *dst = buf;

        while (remaining > 0)
        {
            int chunk = (remaining < MUS_SCRATCH_SAMPLES)
                      ? remaining : MUS_SCRATCH_SAMPLES;
            int16_t *scratch = mus_scratch;

            for (i = 0; i < chunk; i++)
            {
                uint32_t mask;

                /* advance MUS tick counter */
                mus_player.tick_frac += MUS_TICK_RATE;
                while (mus_player.tick_frac >= N64_AUDIO_FREQUENCY)
                {
                    mus_player.tick_frac -= N64_AUDIO_FREQUENCY;
                    if (!mus_player.eos)
                    {
                        if (mus_player.tick_delay > 0)
                            mus_player.tick_delay--;
                        if (mus_player.tick_delay == 0)
                        {
                            do { N64_MusProcessBatch(); }
                            while (mus_player.tick_delay == 0 && !mus_player.eos);
                        }
                        if (mus_player.eos)
                        {
                            if (mus_player.looping)
                            {
                                N64_MusResetPlayback();
                                N64_MusProcessBatch();
                            }
                            else
                            {
                                mus_player.playing = 0;
                            }
                        }
                    }
                }

                mix_l = 0;
                mix_r = 0;

                /* render only active voices, ascending index order */
                mask = mus_player.active_mask;
                while (mask)
                {
                    v = __builtin_ctz(mask);
                    mask &= mask - 1;
                    N64_MusRenderVoice(v, &mix_l, &mix_r);
                }

                if (mix_l > 32767)
                    mix_l = 32767;
                else if (mix_l < -32768)
                    mix_l = -32768;

                if (mix_r > 32767)
                    mix_r = 32767;
                else if (mix_r < -32768)
                    mix_r = -32768;

                scratch[i * 2]     = (int16_t)mix_l;
                scratch[i * 2 + 1] = (int16_t)mix_r;

#if DOOM_N64_DEBUG
                sum = (mix_l + mix_r) / 2;
                abs_sum = (sum < 0) ? -sum : sum;
                if (abs_sum > mus_player.dbg_peak_abs)
                    mus_player.dbg_peak_abs = abs_sum;

                if (sum != 0)
                {
                    mus_player.dbg_nonzero_samples++;
                    if (!mus_player.dbg_reported_nonzero)
                    {
                        N64_DEBUGF("MUS first nonzero sample at %u (amp=%d, notes_on=%u, events=%u)\n",
                               (unsigned)mus_player.dbg_generated_samples,
                               (int)sum,
                               (unsigned)mus_player.dbg_note_on,
                               (unsigned)mus_player.dbg_events);
                        mus_player.dbg_reported_nonzero = 1;
                    }
                }

                mus_player.dbg_generated_samples++;
#endif
            }

            /* copy cached scratch into the uncached mixer buffer; both are
               8-byte aligned so move 4 int16s (two stereo samples) at a time. */
            {
                int words = chunk * 2;          /* int16 lanes in this chunk */
                int j = 0;
                const uint64_t *src64 = (const uint64_t *)scratch;
                uint64_t *dst64 = (uint64_t *)dst;

                for (; j + 4 <= words; j += 4)
                    *dst64++ = *src64++;

                for (; j < words; j++)
                    dst[j] = scratch[j];
            }

            dst += chunk * 2;
            remaining -= chunk;
        }
    }

    #if DOOM_N64_DEBUG
    if (!mus_player.dbg_reported_silence
        && mus_player.dbg_generated_samples >= MUS_DEBUG_SILENCE_SAMPLES
        && mus_player.dbg_nonzero_samples == 0)
    {
        N64_DEBUGF("MUS silence warning: generated=%u events=%u notes_on=%u score_pos=%d/%d\n",
               (unsigned)mus_player.dbg_generated_samples,
               (unsigned)mus_player.dbg_events,
               (unsigned)mus_player.dbg_note_on,
               mus_player.score_pos,
               mus_player.score_len);
        mus_player.dbg_reported_silence = 1;
    }
    #endif
}

static boolean N64_MusSetup(const void *data, int len)
{
    const uint8_t *d = (const uint8_t *)data;
    int score_start, score_len;
    int prim_channels, sec_channels, inst_count;

    if (!N64_IsMus(data, len))
        return false;

    score_len   = (int)N64_ReadLE16(d + 4);
    score_start = (int)N64_ReadLE16(d + 6);
    prim_channels = (int)N64_ReadLE16(d + 8);
    sec_channels  = (int)N64_ReadLE16(d + 10);
    inst_count    = (int)N64_ReadLE16(d + 12);

    if (score_start < 16 || score_start > len
        || score_len <= 0 || score_start + score_len > len)
    {
        N64_DEBUGF("I_RegisterSong: malformed MUS header (start=%d len=%d total=%d)\n",
               score_start, score_len, len);
        return false;
    }

    memset(&mus_player, 0, sizeof(mus_player));
    mus_player.score      = d + score_start;
    mus_player.score_len  = score_len;
    #if DOOM_N64_DEBUG
    mus_player.dbg_event_budget = MUS_DEBUG_EVENT_LOG_BUDGET;
    #endif

    N64_MusPrepareInstruments(d, len, inst_count);
    N64_MusResetPlayback();

    N64_DEBUGF("MUS setup: score_start=%d score_len=%d channels=%d secondary=%d instruments=%d\n",
               score_start,
               score_len,
               prim_channels,
               sec_channels,
               inst_count);

    if (mus_bank_loaded)
        N64_DEBUGF("MUS setup: instrument bank %s\n", mus_bank_path);
    else
        N64_DEBUGF("MUS setup: no instrument bank (using waveform fallback)\n");

    #if !DOOM_N64_DEBUG
    (void)prim_channels;
    (void)sec_channels;
    (void)inst_count;
    #endif

    mus_player.wave.name      = "MUS";
    mus_player.wave.bits      = 16;
    mus_player.wave.channels  = 2;
    mus_player.wave.frequency = (float)N64_AUDIO_FREQUENCY;
    mus_player.wave.len       = WAVEFORM_UNKNOWN_LEN;
    mus_player.wave.loop_len  = 0;
    mus_player.wave.read      = N64_MusWaveRead;
    mus_player.wave.ctx       = &mus_player;

    N64_DEBUGF("MUS setup: waveform ready (rate=%dHz)\n", N64_AUDIO_FREQUENCY);

    return true;
}

static void N64_ApplyMusicVolume(void)
{
    int volume;
    float gain;

    if (!music_track.opened)
        return;

    volume = N64_NormalizeVolume(snd_MusicVolume);

    gain = (float)volume / 127.0f;

    if (music_track.format == N64_MUSIC_FMT_XM64)
    {
        xm64player_set_vol(&music_track.xm, gain);
    }
    else if (music_track.format == N64_MUSIC_FMT_YM64)
    {
        int i;

        for (i = 0; i < music_track.channels; i++)
        {
            int ch = music_first_channel + i;
            if (ch >= 0 && ch < mixer_channel_count)
                mixer_ch_set_vol(ch, gain, gain);
        }
    }
    else if (music_track.format == N64_MUSIC_FMT_MUS)
    {
        mixer_ch_set_vol(music_first_channel, gain, gain);
    }
}

static boolean N64_OpenMusicTrack(void)
{
    if (!music_track.registered)
        return false;
    if (!music_track.asset_path[0] && music_track.format != N64_MUSIC_FMT_MUS)
        return false;

    if (music_track.opened)
        return true;

    if (music_channel_count <= 0)
    {
        N64_DEBUGF("I_PlaySong: no mixer channels available for music\n");
        return false;
    }

    if (music_track.format == N64_MUSIC_FMT_XM64)
    {
        xm64player_open(&music_track.xm, music_track.asset_path);
        music_track.channels = xm64player_num_channels(&music_track.xm);
        if (music_track.channels < 1)
            music_track.channels = 1;

        if (music_track.channels > music_channel_count)
        {
            N64_DEBUGF("I_PlaySong: %s needs %d channels, only %d reserved\n",
                   music_track.asset_path,
                   music_track.channels,
                   music_channel_count);
            xm64player_close(&music_track.xm);
            return false;
        }
    }
    else if (music_track.format == N64_MUSIC_FMT_YM64)
    {
        ym64player_open(&music_track.ym, music_track.asset_path, NULL);
        music_track.channels = ym64player_num_channels(&music_track.ym);
        if (music_track.channels < 1)
            music_track.channels = 1;

        if (music_track.channels > music_channel_count)
        {
            N64_DEBUGF("I_PlaySong: %s needs %d channels, only %d reserved\n",
                   music_track.asset_path,
                   music_track.channels,
                   music_channel_count);
            ym64player_close(&music_track.ym);
            return false;
        }
    }
    else if (music_track.format == N64_MUSIC_FMT_MUS)
    {
        if (!N64_MusSetup(music_track.mus_data, music_track.mus_data_len))
            return false;
        music_track.channels = 2;

        if (music_track.channels > music_channel_count)
        {
            N64_DEBUGF("I_PlaySong: MUS needs %d channels, only %d reserved\n",
                   music_track.channels,
                   music_channel_count);
            return false;
        }
    }
    else
    {
        return false;
    }

    music_track.opened = true;
    N64_ApplyMusicVolume();
    return true;
}

static void N64_StartMusicTrack(boolean looping)
{
    if (!N64_OpenMusicTrack())
        return;

    if (music_track.format == N64_MUSIC_FMT_XM64)
    {
        xm64player_set_loop(&music_track.xm, looping ? true : false);
        xm64player_play(&music_track.xm, music_first_channel);
    }
    else if (music_track.format == N64_MUSIC_FMT_YM64)
    {
        ym64player_play(&music_track.ym, music_first_channel);
    }
    else if (music_track.format == N64_MUSIC_FMT_MUS)
    {
        mus_player.looping = looping ? 1 : 0;
        mus_player.playing = 1;
        mus_player.eos     = 0;
        N64_DEBUGF("I_PlaySong: MUS start lump=%s loop=%d channel=%d\n",
               music_track.lump_name,
               looping ? 1 : 0,
               music_first_channel);
        mixer_ch_play(music_first_channel, &mus_player.wave);
    }

    music_track.looping = looping;
    music_track.playing = true;
    music_track.paused = false;
    N64_ApplyMusicVolume();
}

static void N64_StopMusicTrack(void)
{
    if (!music_track.opened)
        return;

    if (music_track.format == N64_MUSIC_FMT_XM64)
        xm64player_stop(&music_track.xm);
    else if (music_track.format == N64_MUSIC_FMT_YM64)
        ym64player_stop(&music_track.ym);
    else if (music_track.format == N64_MUSIC_FMT_MUS)
    {
        mixer_ch_stop(music_first_channel);
        mus_player.playing = 0;
        N64_DEBUGF("I_StopSong: MUS stop samples=%u nonzero=%u peak=%d events=%u notes_on=%u\n",
               (unsigned)mus_player.dbg_generated_samples,
               (unsigned)mus_player.dbg_nonzero_samples,
               (int)mus_player.dbg_peak_abs,
               (unsigned)mus_player.dbg_events,
               (unsigned)mus_player.dbg_note_on);
    }

    music_track.playing = false;
    music_track.paused = false;
}

static void N64_MaybeLoopMusic(void)
{
    if (!music_track.opened || !music_track.playing)
        return;

    if (music_track.format == N64_MUSIC_FMT_MUS
        && !mus_player.playing)
    {
        N64_DEBUGF("MUS playback finished: eos=%d pos=%d/%d events=%u notes_on=%u nonzero=%u\n",
               mus_player.eos ? 1 : 0,
               mus_player.score_pos,
               mus_player.score_len,
               (unsigned)mus_player.dbg_events,
               (unsigned)mus_player.dbg_note_on,
               (unsigned)mus_player.dbg_nonzero_samples);
        mixer_ch_stop(music_first_channel);
        music_track.playing = false;
        return;
    }

    if (music_track.format == N64_MUSIC_FMT_YM64
        && !mixer_ch_playing(music_first_channel))
    {
        if (!music_track.looping)
        {
            music_track.playing = false;
            return;
        }

        if (!ym64player_seek(&music_track.ym, 0))
        {
            ym64player_close(&music_track.ym);
            ym64player_open(&music_track.ym, music_track.asset_path, NULL);
        }

        ym64player_play(&music_track.ym, music_first_channel);
        N64_ApplyMusicVolume();
    }
}

static void N64_PumpAudio(void)
{
    int samples;

    if (!sound_initialized)
        return;

    N64_MaybeLoopMusic();

    samples = audio_get_buffer_length();

    while (audio_can_write())
    {
        int16_t* out = audio_write_begin();
        mixer_poll(out, samples);
        audio_write_end();
    }
}

void I_InitSound(void)
{
    int i;

    if (sound_initialized)
        return;

    if (numChannels < 1)
        numChannels = 1;
    if (numChannels > MIXER_MAX_CHANNELS - 1)
        numChannels = MIXER_MAX_CHANNELS - 1;

    voice_count = numChannels;
    mixer_channel_count = voice_count + N64_MUSIC_CHANNEL_BUDGET;
    if (mixer_channel_count > MIXER_MAX_CHANNELS)
        mixer_channel_count = MIXER_MAX_CHANNELS;

    music_first_channel = voice_count;
    music_channel_count = mixer_channel_count - music_first_channel;

    audio_init(N64_AUDIO_FREQUENCY, N64_AUDIO_NUM_BUFFERS);
    mixer_init(mixer_channel_count);

    for (i = 0; i < voice_count; i++)
    {
        mixer_ch_set_limits(i, 8, N64_SFX_MAX_FREQUENCY, 0);
        N64_ClearVoice(i);
    }

    for (i = music_first_channel; i < mixer_channel_count; i++)
        mixer_ch_set_limits(i, 16, 48000.0f, 0);

    memset(sfx_cache, 0, sizeof(sfx_cache));

    if (!music_initialized)
        I_InitMusic();

    sound_initialized = true;
    N64_PumpAudio();

    N64_DEBUGF("I_InitSound: %d SFX channels, %d music channels, %d Hz output\n",
           voice_count,
           music_channel_count,
           audio_get_frequency());
}

void I_UpdateSound(void)
{
}

void I_SubmitSound(void)
{
    N64_PumpAudio();
}

void I_ShutdownSound(void)
{
    int i;

    if (!sound_initialized)
        return;

    N64_ResetMusicTrack(true);

    for (i = 0; i < voice_count; i++)
    {
        mixer_ch_stop(i);
        N64_ClearVoice(i);
    }

    mixer_close();
    audio_close();

    for (i = 1; i < NUMSFX; i++)
    {
        if (sfx_cache[i].owns_pcm && sfx_cache[i].pcm)
            free(sfx_cache[i].pcm);
        S_sfx[i].data = NULL;
    }

    memset(sfx_cache, 0, sizeof(sfx_cache));
    voice_count = 0;
    mixer_channel_count = 0;
    music_first_channel = 0;
    music_channel_count = 0;
    sound_initialized = false;
}

void I_SetChannels(void)
{
    int i;

    for (i = 0; i < voice_count; i++)
        N64_ClearVoice(i);
}

int I_GetSfxLumpNum(sfxinfo_t* sfxinfo)
{
    return N64_GetSfxLumpByName(sfxinfo->name);
}

int
I_StartSound
( int id,
  int vol,
  int sep,
  int pitch,
  int priority )
{
    int voice;
    int handle;

    (void)priority;

    if (!sound_initialized)
        return 0;

    if (id <= 0 || id >= NUMSFX)
        return 0;

    if (!N64_EnsureSfxLoaded(id))
        return 0;

    voice = N64_AllocVoice();
    if (voices[voice].active)
        mixer_ch_stop(voice);

    if (++next_handle <= 0)
        next_handle = 1;

    handle = next_handle;
    voices[voice].active = true;
    voices[voice].handle = handle;
    voices[voice].sfx_id = id;
    voices[voice].start_tic = gametic;

    mixer_ch_play(voice, &sfx_cache[id].wave);
    N64_ApplyChannelParams(voice, vol, sep, pitch);

    return handle;
}

void I_StopSound(int handle)
{
    int voice;

    voice = N64_FindVoiceByHandle(handle);
    if (voice < 0)
        return;

    mixer_ch_stop(voice);
    N64_ClearVoice(voice);
}

int I_SoundIsPlaying(int handle)
{
    int voice;

    voice = N64_FindVoiceByHandle(handle);
    if (voice < 0)
        return 0;

    if (!mixer_ch_playing(voice))
    {
        N64_ClearVoice(voice);
        return 0;
    }

    return 1;
}

void
I_UpdateSoundParams
( int handle,
  int vol,
  int sep,
  int pitch )
{
    int voice;

    voice = N64_FindVoiceByHandle(handle);
    if (voice < 0)
        return;

    if (!mixer_ch_playing(voice))
    {
        N64_ClearVoice(voice);
        return;
    }

    N64_ApplyChannelParams(voice, vol, sep, pitch);
}

void I_InitMusic(void)
{
    if (music_initialized)
        return;

    N64_ResetMusicTrack(false);
    N64_MusStopAllVoices();
    N64_MusUnloadInstruments();
    xm64_set_extsampledir("rom:/music/samples");

    if (N64_MusLoadPointerTable())
    {
        N64_DEBUGF("I_InitMusic: MUS instrument bank loaded from %s\n",
               mus_bank_path);
    }
    else
    {
        N64_DEBUGF("I_InitMusic: MUS instrument bank missing; using waveform fallback\n");
    }

    missing_music_warning_printed = false;
    music_initialized = true;
}

void I_ShutdownMusic(void)
{
    if (!music_initialized)
        return;

    N64_ResetMusicTrack(true);
    N64_MusStopAllVoices();
    N64_MusUnloadInstruments();
    memset(mus_instrument_ptrs, 0, sizeof(mus_instrument_ptrs));
    mus_bank_path[0] = '\0';
    mus_bank_loaded = false;
    music_initialized = false;
}

void I_SetMusicVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    else if (volume > 127)
        volume = 127;

    snd_MusicVolume = volume;
    N64_ApplyMusicVolume();
}

void I_PauseSong(int handle)
{
    if (handle != music_track.handle)
        return;

    if (!music_track.opened || !music_track.playing)
        return;

    N64_StopMusicTrack();
    music_track.paused = true;
}

void I_ResumeSong(int handle)
{
    if (handle != music_track.handle)
        return;

    if (!sound_initialized || !music_track.opened || !music_track.paused)
        return;

    if (music_track.format == N64_MUSIC_FMT_XM64)
        xm64player_play(&music_track.xm, music_first_channel);
    else if (music_track.format == N64_MUSIC_FMT_YM64)
        ym64player_play(&music_track.ym, music_first_channel);
    else if (music_track.format == N64_MUSIC_FMT_MUS)
    {
        mus_player.playing = 1;
        mixer_ch_play(music_first_channel, &mus_player.wave);
    }

    music_track.playing = true;
    music_track.paused = false;
    N64_ApplyMusicVolume();
}

int I_RegisterSong(void* data)
{
    int llen;

    if (!music_initialized)
        I_InitMusic();

    N64_ResetMusicTrack(true);

    if (++next_music_handle <= 0)
        next_music_handle = 1;

    music_track.handle = next_music_handle;
    music_track.registered = true;
    music_track.lumpnum = N64_FindLumpNumByData(data);
    llen = (music_track.lumpnum >= 0)
         ? W_LumpLength(music_track.lumpnum) : 0;

    N64_DEBUGF("I_RegisterSong: handle=%d lumpnum=%d len=%d data=%p\n",
               music_track.handle,
               music_track.lumpnum,
               llen,
               data);

    if (N64_ResolveMusicTrackAsset(music_track.lumpnum))
    {
        N64_DEBUGF("I_RegisterSong: %s -> %s\n",
               music_track.lump_name,
               music_track.asset_path);
    }

    if (music_track.format == N64_MUSIC_FMT_NONE && data)
    {
        if (N64_IsMus(data, llen))
        {
            music_track.mus_data     = data;
            music_track.mus_data_len = llen;
            music_track.format       = N64_MUSIC_FMT_MUS;
            N64_DEBUGF("I_RegisterSong: MUS sampled fallback -> %s\n",
                   music_track.lump_name[0] ? music_track.lump_name : "?");
        }
        else if (!missing_music_warning_printed)
        {
            N64_DEBUGF("I_RegisterSong: no .xm64/.ym64 and not MUS format\n");
            missing_music_warning_printed = true;
        }
    }

    return next_music_handle;
}

void
I_PlaySong
( int handle,
  int looping )
{
    N64_DEBUGF("I_PlaySong: request handle=%d active=%d format=%d looping=%d opened=%d\n",
           handle,
           music_track.handle,
           music_track.format,
           looping,
           music_track.opened ? 1 : 0);

    if (handle != music_track.handle)
        return;

    if (!sound_initialized || !music_track.registered)
        return;

    if (!music_track.asset_path[0] && music_track.format != N64_MUSIC_FMT_MUS)
        return;

    if (music_track.playing || music_track.paused)
        N64_StopMusicTrack();

    N64_StartMusicTrack(looping ? true : false);

    if (!music_track.playing)
    {
        N64_DEBUGF("I_PlaySong: failed to start music asset %s\n",
               music_track.asset_path);
    }
}

void I_StopSong(int handle)
{
    if (handle != music_track.handle)
        return;

    N64_StopMusicTrack();
}

void I_UnRegisterSong(int handle)
{
    if (handle != music_track.handle)
        return;

    N64_ResetMusicTrack(true);
}
