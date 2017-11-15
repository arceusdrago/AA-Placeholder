/*
Copyright 2004-2011 Tom Rothamel <pytom@bishoujo.us>

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation files
(the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge,
publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "pss.h"
#include <Python.h>
#include <SDL/SDL.h>
#include <SDL/SDL_thread.h>
#include <stdio.h>

/* Declarations of ffdecode functions. */
struct VideoState;

struct VideoState *ffpy_stream_open(SDL_RWops *, const char *);
void ffpy_stream_close(struct VideoState *is);
void ffpy_alloc_event(struct VideoState *vs, PyObject *surface);
int ffpy_refresh_event(struct VideoState *vs);
void ffpy_init(int rate, int status);
int ffpy_audio_decode(struct VideoState *is, Uint8 *stream, int len);

    
/* The current Python. */
PyInterpreterState* interp;
PyThreadState* thread = NULL;

static void incref(PyObject *ref) {
    PyThreadState *oldstate;

    PyEval_AcquireLock();
    oldstate = PyThreadState_Swap(thread);
    Py_INCREF(ref);    
    PyThreadState_Swap(oldstate);
    PyEval_ReleaseLock();
}

static void decref(PyObject *ref) {
    PyThreadState *oldstate;

    PyEval_AcquireLock();
    oldstate = PyThreadState_Swap(thread);
    Py_DECREF(ref);    
    PyThreadState_Swap(oldstate);
    PyEval_ReleaseLock();
}

/* A mutex that protects the shared data structures. */
SDL_mutex *name_mutex;

#define LOCK_NAME() { SDL_LockMutex(name_mutex); }
#define UNLOCK_NAME() { SDL_UnlockMutex(name_mutex); }

/* Locking on entry from python... */
// #define BEGIN() PyThreadState *_save;
// #define ENTER() { printf("Locking by %s.\n", __FUNCTION__); _save = PyEval_SaveThread(); SDL_LockAudio(); printf("Lock by %s\n", __FUNCTION__);  }
// #define EXIT() { SDL_UnlockAudio(); PyEval_RestoreThread(_save); printf("Release by %s\n", __FUNCTION__); }

#define BEGIN() PyThreadState *_save;
#define ENTER() { _save = PyEval_SaveThread(); SDL_LockAudio(); }
#define EXIT() { SDL_UnlockAudio(); PyEval_RestoreThread(_save); }
#define ALTENTER() { _save = PyEval_SaveThread(); }
#define ALTEXIT() { PyEval_RestoreThread(_save); }

/* Min and Max */
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

/* Various error codes. */
#define SUCCESS 0
#define SDL_ERROR -1
#define SOUND_ERROR -2
#define PSS_ERROR -3

/* This is called with the appropriate error code at the end of a
 * function. */
#define error(err) PSS_error = err
int PSS_error = SUCCESS;
static const char *error_msg = NULL;

/* Have we been initialized? */
static int initialized = 0;

/*
 * This structure represents a channel the system knows about
 * and can play from.
 */
struct Channel {

    /* The currently playing sample, NULL if this sample isn't playing
       anything. */
    struct VideoState *playing;

    /* The name of the playing music. */
    PyObject *playing_name;

    /* The number of ms to take to fade in the playing sample. */
    int playing_fadein;

    /* Is the playing sample tight? */
    int playing_tight;
    
    /* The queued up sample. */
    struct VideoState *queued;    

    /* The name of the queued up sample. */
    PyObject *queued_name;

    /* The number of ms to take to fade in the queued sample. */
    int queued_fadein;

    /* Is the queued sample tight? */
    int queued_tight;
    
    /* Is this channel paused? */
    int paused;
    
    /* The volume of the channel. */
    int volume;

    /* The position (in bytes) that this channel has queued to. */
    int pos;

    /* 
     * The number of bytes for each step of fade.
     * 0 when no fade is in progress.
     */
    int fade_step_len;

    /* How many bytes we are into the current fade step. */
    int fade_off;

    /* The current fade volume. */
    int fade_vol;

    /* The change in fade_vol for each step. */
    int fade_delta;
    
    /* The number of bytes in which we'll stop. */
    int stop_bytes;

    /* The event posted to the queue when we finish a track. */
    int event;

    /* The pan being applied to the current channel. */
    float pan_start;
    float pan_end;

    /* The length of the current pan, in samples. */
    unsigned int pan_length;

    /* The number of samples we've finished in the current pan. */
    unsigned int pan_done;


    /* These are used like in pan, above. Unlike the volume parameter,
       the voulme set here is persisted between sessions. */
    float vol2_start;
    float vol2_end;
    unsigned int vol2_length;
    unsigned int vol2_done;
};

struct Dying {
    struct VideoState *stream;
    struct Dying *next;
};

static struct Dying *dying = NULL;

/*
 * The number of channels the system knows about.
 */
int num_channels = 0;

/*
 * All of the channels that the system knows about.
 */
struct Channel *channels = NULL;

/*
 * The spec of the audio that is playing.
 */
SDL_AudioSpec audio_spec;


static float interpolate_pan(struct Channel *c) {
    float done;

    if (c->pan_done > c->pan_length) {
        c->pan_length = 0;
    }

    if (c->pan_length == 0) {
        return c->pan_end;
    }

    done = 1.0 * c->pan_done / c->pan_length;

    return c->pan_start + done * (c->pan_end - c->pan_start);
    
}

static float interpolate_vol2(struct Channel *c) {
    float done;

    if (c->vol2_done > c->vol2_length) {
        c->vol2_length = 0;
    }

    if (c->vol2_length == 0) {
        return c->vol2_end;
    }

    done = 1.0 * c->vol2_done / c->vol2_length;

    return c->vol2_start + done * (c->vol2_end - c->vol2_start);
}

static int ms_to_bytes(int ms) {
    return ((long long) ms) * audio_spec.freq * audio_spec.channels * 2 / 1000;
}

static int bytes_to_ms(int bytes) {
    return ((long long) bytes) * 1000 / (audio_spec.freq * audio_spec.channels * 2);
}

static void start_sample(struct Channel* c, int reset_fade) {
    int fade_steps;

    if (!c) return;

    c->pos = 0;

    if (reset_fade) {
        
        if (c->playing_fadein == 0) {
            c->fade_step_len = 0;
        } else {
            fade_steps = c->volume;
            c->fade_delta = 1;
            c->fade_off = 0;
            c->fade_vol = 0;

            if (fade_steps) {
                c->fade_step_len = ms_to_bytes(c->playing_fadein) / fade_steps;
                c->fade_step_len &= ~0x7; // Even sample.
            } else {
                c->fade_step_len = 0;
            }
        }

        c->stop_bytes = -1;
    } 
}

static void free_sample(struct VideoState *ss) {
    ffpy_stream_close(ss);
}

#define MAX_SHORT (32767)
#define MIN_SHORT (-32768)

// Actually mixes the audio.
static void mixaudio(Uint8 *dst, Uint8 *src, int length, int volume) {
    int i;
    short *sdst = (short *) dst;
    short *ssrc = (short *) src;

    for (i = 0; i < length / 2; i++) {
        int sound = *sdst + (volume * *ssrc) / SDL_MIX_MAXVOLUME;
        if (sound > MAX_SHORT) {
            sound = MAX_SHORT;
        }
        if (sound < MIN_SHORT) {
            sound = MIN_SHORT;
        }

        *sdst++ = (short) sound;
        ssrc++;        
    }
}


// Mixes the audio, while performing fading.
static void fade_mixaudio(struct Channel *c,
                          Uint8 *dst, Uint8 *src, int length) {

    while (length) {
        
        // No fade case.
        if (c->fade_step_len == 0) {
            mixaudio(dst, src, length, c->volume);
            return;
        }

        // Fading, but we have some space left in the current step.
        if (c->fade_off < c->fade_step_len) {
            int l = min(c->fade_step_len - c->fade_off, length);

            mixaudio(dst, src, l, c->fade_vol);
            
            length -= l;
            dst += l;
            src += l;
            c->fade_off += l;
            continue;
        }
        
        // Otherwise, we have no space left in the current fade step.
        // Go to the next step.
        c->fade_off = 0;
        c->fade_vol += c->fade_delta;

        // Don't stop on a fadeout.
        if (c->fade_vol <= 0) {
            c->fade_vol = 0;
        }
        
        // Stop on a fadein.
        if (c->fade_vol >= c->volume) {
            c->fade_vol = c->volume;
            c->fade_step_len = 0;
        }
    }
    
    return;
}

static void post_event(struct Channel *c) {
    if (! c->event) {
        return;
    }

    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = c->event;
    SDL_PushEvent(&e);
}

/* This handels panning and vol2 manipulations. */
static void pan_audio(struct Channel *c, Uint8 *stream, int length) {
    int i;
    short *sample = (short *) stream;
    length /= 4;

    float pan;
    float vol2;
    int left = 256;
    int right = 256;
    
    
    for (i = 0; i < length; i++) {

        if ((i & 0x1f) == 0) {
            pan = interpolate_pan(c);
            vol2 = interpolate_vol2(c);
            
            // If nothing to do, skip 32 samples.
            if (pan == 0.0 && vol2 == 1.0) {
                i += 31;
                c->pan_done += 32;
                c->vol2_done += 32;
                sample += 32 * 2;
                continue;
            }

            vol2 *= 256.0;

            if (pan < 0) {
                left = (int) vol2;
                right = (int) (vol2 * (1.0 + pan));
            } else {
                left = (int) (vol2 * (1.0 - pan));
                right = (int) vol2;
            }
        }


        *sample = (short) ((*sample * left) >> 8);
        sample++;
        *sample = (short) ((*sample * right) >> 8);
        sample++;

        c->pan_done += 1;
        c->vol2_done += 1;
    }
    
}

static void callback(void *userdata, Uint8 *stream, int length) {
    int channel = 0;

    for (channel = 0; channel < num_channels; channel++) {

        
        int mixed = 0;
        struct Channel *c = &channels[channel];

        if (! c->playing) {
            continue;
        }

        if (c->paused) {
            continue;
        }

        while (mixed < length && c->playing) {
            int mixleft = length - mixed;
            Uint8 buffer[mixleft];
            int bytes;

            // Decode some amount of data.

            bytes = ffpy_audio_decode(c->playing, buffer, mixleft);
            
            // We have some data in the buffer.
            if (c->stop_bytes && bytes) {
                
                if (c->stop_bytes != -1)
                    bytes = min(c->stop_bytes, bytes);

                pan_audio(c, buffer, bytes);
                fade_mixaudio(c, &stream[mixed], buffer, bytes);

                mixed += bytes;
                
                if (c->stop_bytes != -1)
                    c->stop_bytes -= bytes;

                c->pos += bytes;
                
                continue;
            }

            // Otherwise, no data is left in the buffer. Check why,
            // and act accordingly.

            // Skip to the next sample.
            if (c->stop_bytes == 0 || bytes == 0) {

                int old_tight = c->playing_tight;
                struct Dying *d;
                
                post_event(c);

                d = malloc(sizeof(struct Dying));
                d->next = dying;
                d->stream = c->playing;
                dying = d;
                
                LOCK_NAME();

                decref(c->playing_name);

                c->playing = c->queued;
                c->playing_name = c->queued_name;
                c->playing_fadein = c->queued_fadein;
                c->playing_tight = c->queued_tight;
                
                c->queued = NULL;
                c->queued_name = NULL;
                c->queued_fadein = 0;
                c->queued_tight = 0;

                UNLOCK_NAME();
                
                start_sample(c, ! old_tight);
                
                continue;                
            }
        }

    }

}

/*
 * Checks that the given channel is in range. Returns 0 if it is,
 * sets an error and returns -1 if it is not. Allocates channels
 * that don't already exist.
 */
static int check_channel(int c) {
    int i;

    if (c < 0) {
        error(PSS_ERROR);
        error_msg = "Channel number out of range.";
        return -1;
    }

    if (c >= num_channels) {
        channels = realloc(channels, sizeof(struct Channel) * (c + 1));
    
        for (i = num_channels; i <= c; i++) {
         
            channels[i].playing = NULL;
            channels[i].queued = NULL;
            channels[i].playing_name = NULL;
            channels[i].queued_name = NULL;
            channels[i].volume = SDL_MIX_MAXVOLUME;        
            channels[i].paused = 1;
            channels[i].event = 0;
            channels[i].pan_start = 0.0;
            channels[i].pan_end = 0.0;
            channels[i].pan_length = 0;
            channels[i].pan_done = 0;
            channels[i].vol2_start = 1.0;
            channels[i].vol2_end = 1.0;
            channels[i].vol2_length = 0;
            channels[i].vol2_done = 0;
        }

        num_channels = c + 1;
    }
    
    return 0;
}


/*
 * Loads the provided sample. Returns the sample on success, NULL on
 * failure.
 */
struct VideoState *load_sample(SDL_RWops *rw, const char *ext) {
    struct VideoState *rv;    
    rv = ffpy_stream_open(rw, ext);
    return rv;
}

    
void PSS_play(int channel, SDL_RWops *rw, const char *ext, PyObject *name, int fadein, int tight, int paused) {

    BEGIN();
    
    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];
    ENTER();
    
    LOCK_NAME();

    /* Free playing and queued samples. */
    if (c->playing) {
        free_sample(c->playing);
        c->playing = NULL;
        decref(c->playing_name);
        c->playing_name = NULL;
        c->playing_tight = 0;
    }
        
    if (c->queued) {
        free_sample(c->queued);
        c->queued = NULL;
        decref(c->queued_name);
        c->queued_name = NULL;
        c->queued_tight = 0;
    }

    /* Allocate playing sample. */
    
    c->playing = load_sample(rw, ext);

    if (! c->playing) {
    	UNLOCK_NAME();
    	EXIT();
        error(SOUND_ERROR);
        return;
    }

    incref(name);
    c->playing_name = name;

    c->playing_fadein = fadein;
    c->playing_tight = tight;
    
    c->paused = paused;
    start_sample(c, 1);    
/*     update_pause(); */
        
    UNLOCK_NAME();

    EXIT();
    error(SUCCESS);
}

void PSS_queue(int channel, SDL_RWops *rw, const char *ext, PyObject *name, int fadein, int tight) {

    BEGIN();
    
    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];
    
    ENTER();

    /* If we're not playing, then we should play instead of queue. */
    if (!c->playing) {
        EXIT();
        PSS_play(channel, rw, ext, name, fadein, tight, 0);
        return;            
    }
    
    /* Free queued sample. */
        
    if (c->queued) {
        free_sample(c->queued);
        c->queued = NULL;
        decref(c->queued_name);
        c->queued_name = NULL;
        c->queued_tight = 0;
    }
        
    /* Allocate queued sample. */
    c->queued = load_sample(rw, ext);

    if (! c->queued) {
        EXIT();
        error(SOUND_ERROR);
        return;
    }

    incref(name);
    c->queued_name = name;
    c->queued_fadein = fadein;
    c->queued_tight = tight;
    
    EXIT();
    error(SUCCESS);
}
              

/*
 * Stops all music from playing, freeing the data used by the
 * music.
 */
void PSS_stop(int channel) {
    BEGIN();

    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();
    LOCK_NAME();

    if (c->playing) {
        post_event(c);
    }
    
    /* Free playing and queued samples. */
    if (c->playing) {
        free_sample(c->playing);
        c->playing = NULL;
        decref(c->playing_name);
        c->playing_name = NULL;
    }
        
    if (c->queued) {
        free_sample(c->queued);
        c->queued = NULL;
        decref(c->queued_name);
        c->queued_name = NULL;
    }
    
/*     update_pause(); */

    UNLOCK_NAME();
    EXIT();    

    error(SUCCESS);
}

/*
 * This dequeues the queued sound from the supplied channel, if
 * such a sound is queued. This does nothing to the playing
 * sound.
 *
 * This does nothing if the playing sound is tight, ever_tight is
 * false.
 */
void PSS_dequeue(int channel, int even_tight) {
    BEGIN();

    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();

    if (c->queued && (! c->playing_tight || even_tight)) {
        free_sample(c->queued);
        c->queued = NULL;
        decref(c->queued_name);
        c->queued_name = NULL;
    } else {
        c->queued_tight = 0;
    }


    
    EXIT();    
    error(SUCCESS);
}

/*
 * Returns the queue depth of the current channel. This is 0 if we're
 * stopped, 1 if there's something playing but nothing queued, and 2
 * if there's both something playing and something queued.
 */
int PSS_queue_depth(int channel) {
    int rv = 0;
    BEGIN();

    struct Channel *c;

    if (check_channel(channel)) {
        return 0;
    }

    c = &channels[channel];

    ENTER();

    if (c->playing) rv++;
    if (c->queued) rv++;
    
    EXIT();    
    error(SUCCESS);

    return rv;
}

PyObject *PSS_playing_name(int channel) {
	BEGIN();
	PyObject *rv;
    
    struct Channel *c;

    if (check_channel(channel)) {
        Py_INCREF(Py_None);
        return Py_None;
    }


    c = &channels[channel];

    ALTENTER();
    LOCK_NAME();
    ALTEXIT();

    if (c->playing_name) {
        rv = c->playing_name;
    } else {
        rv = Py_None;
    }

    Py_INCREF(rv);

    ALTENTER();
    UNLOCK_NAME();
    ALTEXIT();

    error(SUCCESS);
        
    return rv;
}

/*
 * Causes the given channel to fadeout playing after a specified
 * number of milliseconds. The playing sound stops once the
 * fadeout finishes (a queued sound may then start at full volume).
 */
void PSS_fadeout(int channel, int ms) {
    BEGIN();
    int fade_steps;
    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();

    if (ms == 0) {
        c->stop_bytes = 0;
        EXIT();

        error(SUCCESS);
        return;
    }


        
    fade_steps = c->volume;
    c->fade_delta = -1;
    c->fade_off = 0;
    c->fade_vol = c->volume;

    if (fade_steps) {
        c->fade_step_len = ms_to_bytes(ms) / fade_steps;
        c->fade_step_len &= ~0x7; // Even sample.
    } else {
        c->fade_step_len = 0;
    }
        
    c->stop_bytes = ms_to_bytes(ms);
    c->queued_tight = 0;

    if (!c->queued) {
        c->playing_tight = 0;
    }
    
    EXIT();    

    error(SUCCESS);
}

/*
 * Sets the pause flag on the given channel 0 = unpaused, 1 = paused.
 */
void PSS_pause(int channel, int pause) {
    BEGIN();
    
    struct Channel *c;

    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();

    c->paused = pause;

    EXIT();

    error(SUCCESS);
    
}

void PSS_unpause_all(void) {

    int i;
    
    BEGIN();

    ENTER();

    for (i = 0; i < num_channels; i++) {
        channels[i].paused = 0;
    }

    EXIT();

    error(SUCCESS);
    
}

/*
 * Returns the position of the given channel, in ms.
 */
int PSS_get_pos(int channel) {
    int rv;
    struct Channel *c;

    BEGIN();
    
    if (check_channel(channel)) {
        return -1;
    }

    c = &channels[channel];

    ENTER();
    
    if (c->playing) {
        rv = bytes_to_ms(c->pos);
    } else {
        rv = -1;
    }

    EXIT();
    
    error(SUCCESS);
    return rv;
}

/*
 * Sets an event that is queued up when the track on the given channel
 * ends due to natural termination or a forced stop.
 */
void PSS_set_endevent(int channel, int event) {
    struct Channel *c;
    BEGIN();
    
    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();
    
    c->event = event;
    
    EXIT();
    
    error(SUCCESS);    
}

/*
 * This sets the natural volume of the channel. (This may not take
 * effect immediately if a fade is going on.)
 */
void PSS_set_volume(int channel, float volume) {
    struct Channel *c;
    BEGIN();
    
    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();
    
    c->volume = (int) (volume * SDL_MIX_MAXVOLUME);
    
    EXIT();
    
    error(SUCCESS);    
}



float PSS_get_volume(int channel) {

    float rv;
    
    struct Channel *c;
    BEGIN();
    
    if (check_channel(channel)) {
        return 0.0;
    }

    c = &channels[channel];

    ENTER();
    
    rv = 1.0 * c->volume / SDL_MIX_MAXVOLUME;
    
    EXIT();
    
    error(SUCCESS);    
    return rv;
}

/*
 * This sets the pan of the channel... independent volumes for the
 * left and right channels.
 */
void PSS_set_pan(int channel, float pan, float delay) {
    struct Channel *c;
    BEGIN();
    
    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();
    
    c->pan_start = interpolate_pan(c);
    c->pan_end = pan;
    c->pan_length = (int) (audio_spec.freq * delay);
    c->pan_done = 0;
    
    EXIT();
    
    error(SUCCESS);    
}

/*
 * This sets the secondary volume of the channel.
 */
void PSS_set_secondary_volume(int channel, float vol2, float delay) {
    struct Channel *c;
    BEGIN();
    
    if (check_channel(channel)) {
        return;
    }

    c = &channels[channel];

    ENTER();

    c->vol2_start = interpolate_vol2(c);
    c->vol2_end = vol2;
    c->vol2_length = (int) (audio_spec.freq * delay);
    c->vol2_done = 0;
    
    EXIT();
    
    error(SUCCESS);    
}


/*
 * Initializes the sound to the given frequencies, channels, and
 * sample buffer size.
 */
void PSS_init(int freq, int stereo, int samples, int status) {

    if (initialized) {
        return;
    }

    name_mutex = SDL_CreateMutex();

    PyEval_InitThreads();

    if (!thread) {
        thread = PyThreadState_Get();
        interp = thread->interp;
        thread = PyThreadState_New(interp);
    }
        
    if (!thread) {
        error(SDL_ERROR);
        return;
    }
    
    if (SDL_Init(SDL_INIT_AUDIO)) {
        error(SDL_ERROR);
        return;
    }

    audio_spec.freq = freq;
    audio_spec.format = AUDIO_S16SYS;
    audio_spec.channels = stereo;
    audio_spec.samples = samples;
    audio_spec.callback = callback;
    audio_spec.userdata = NULL;

    if (SDL_OpenAudio(&audio_spec, NULL)) {
        error(SDL_ERROR);
        return;
    }

    ffpy_init(audio_spec.freq, status);

    SDL_PauseAudio(0);
    
    initialized = 1;

    error(SUCCESS);
}

void PSS_quit() {
    BEGIN();

    if (! initialized) {
        return;
    }
    
    int i;

    ENTER();
    SDL_PauseAudio(1);
    EXIT();

    for (i = 0; i < num_channels; i++) {
        PSS_stop(i);
    }

    SDL_CloseAudio();

    num_channels = 0;
    initialized = 0;
    error(SUCCESS);
}

/* This must be called frequently, to take care of deallocating dead
 * streams. */
void PSS_periodic() {
    BEGIN();

    if (!dying) {
        return;
    }

    ENTER();
    
    while (dying) {
        struct Dying *d = dying;
        ffpy_stream_close(d->stream);
        dying = d->next;
        free(d);
    }
    
    EXIT();
}

/* This should be called in response to an FF_ALLOC_EVENT, with a pygame
 * surface to display the movie on. */
void PSS_alloc_event(PyObject *surface) {
    int i;

    for (i = 0; i < num_channels; i++) {
        if (channels[i].playing) {
            ffpy_alloc_event(channels[i].playing, surface);
        }
    }
}

/* This should be called in response to a FF_REFRESH_EVENT */
int PSS_refresh_event(void) {
    int i;
    int rv = 0;
    for (i = 0; i < num_channels; i++) {
        if (channels[i].playing) {
        	rv = rv | ffpy_refresh_event(channels[i].playing);
        }
    }

    return rv;
}

/*
 * Returns the error message string if an error has occured, or
 * NULL if no error has happened.
 */
char *PSS_get_error() {
    switch(PSS_error) {
    case 0:
        return (char *) "";
    case SDL_ERROR:
        return (char *) SDL_GetError();
    case SOUND_ERROR:
        return (char *) "Some sort of codec error.";
    case PSS_ERROR:
        return (char *) error_msg;
    default:
        return (char *) "Error getting error.";
    }
}

