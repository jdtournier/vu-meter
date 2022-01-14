#define  _POSIX_C_SOURCE  200809L
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <limits.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static volatile int     done = 0;

static pa_simple       *audio = NULL;
static size_t           audio_channels = 0;
static size_t           audio_samples = 0;
static int32_t         *audio_buffer = NULL;    /* audio_buffer[audio_samples][audio_channels] */
static int32_t         *audio_min = NULL;       /* audio_min[audio_channels] */
static int32_t         *audio_max = NULL;       /* audio_max[audio_channels] */
static pthread_t        audio_thread;

static pthread_mutex_t  peak_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   peak_update = PTHREAD_COND_INITIALIZER;
static float           *peak_amplitude = NULL;
static volatile int     peak_available = 0;

int vu_peak_available(void)
{
    return peak_available;
}

static void *worker(void *unused)
{
    (void)unused;  /* Silence warning about unused parameter. */

    while (!done) {
        int  err = 0;

        if (pa_simple_read(audio, audio_buffer, audio_channels * audio_samples * sizeof audio_buffer[0], &err) < 0) {
            done = -EIO;
            break;
        }

        for (size_t c = 0; c < audio_channels; c++) {
            audio_min[c] = (int32_t)( 2147483647);
            audio_max[c] = (int32_t)(-2147483648);
        }

        int32_t *const  end = audio_buffer + audio_channels * audio_samples;
        int32_t        *ptr = audio_buffer;

        /* Min-max peak detect. */
        while (ptr < end) {
            for (size_t c = 0; c < audio_channels; c++) {
                const int32_t  s = *(ptr++);
                audio_min[c] = (audio_min[c] < s) ? audio_min[c] : s;
                audio_max[c] = (audio_max[c] > s) ? audio_max[c] : s;
            }
        }

        /* absolute values. */
        for (size_t c = 0; c < audio_channels; c++) {
            if (audio_min[c] == (int32_t)(-2147483648))
                audio_min[c] =  (int32_t)( 2147483647);
            else
            if (audio_min[c] < 0)
                audio_min[c] = -audio_min[c];
            else
                audio_min[c] = 0;

            if (audio_max[c] < 0)
                audio_max[c] = 0;
        }

        /* Update peak amplitudes. */
        pthread_mutex_lock(&peak_lock);
        if (peak_available++) {
            for (size_t c = 0; c < audio_channels; c++) {
                const float  amplitude = (audio_max[c] > audio_min[c]) ? audio_max[c] / 2147483647.0f : audio_min[c] / 2147483647.0f;
                peak_amplitude[c] = (peak_amplitude[c] > amplitude) ? peak_amplitude[c] : amplitude;
            }
        } else {
            for (size_t c = 0; c < audio_channels; c++) {
                const float  amplitude = (audio_max[c] > audio_min[c]) ? audio_max[c] / 2147483647.0f : audio_min[c] / 2147483647.0f;
                peak_amplitude[c] = amplitude;
            }
        }
        pthread_cond_broadcast(&peak_update);
        pthread_mutex_unlock(&peak_lock);
    }

    /* Wake up all waiters on the peak update, too. */
    pthread_cond_broadcast(&peak_update);
    return NULL;
}


const char *vu_error(int err)
{
    if (err < 0)
        return strerror(-err);
    else
    if (err > 0)
        return pa_strerror(err);
    else
        return "OK";
}

void vu_stop(void)
{
    if (audio) {
        if (!done)
            done = 1;
        pthread_join(audio_thread, NULL);
        pa_simple_free(audio);
        audio_thread = pthread_self();
        audio = NULL;
    }

    free(audio_buffer);  /* Note: free(NULL) is OK. */
    free(audio_min);
    free(audio_max);

    audio_buffer = NULL;
    audio_min    = NULL;
    audio_max    = NULL;
    audio_channels = 0;
    audio_samples  = 0;

    pthread_mutex_lock(&peak_lock);
    free(peak_amplitude);
    peak_amplitude = NULL;
    peak_available = 0;
    pthread_mutex_unlock(&peak_lock);
}


void vu_wait(void)
{
    pthread_mutex_lock(&peak_lock);
    if (audio && !done)
        pthread_cond_wait(&peak_update, &peak_lock);

    pthread_mutex_unlock(&peak_lock);
}


int vu_peak(float *to, int num)
{
    pthread_mutex_lock(&peak_lock);
    if (!peak_amplitude || !peak_available || audio_channels < 1) {
        pthread_mutex_unlock(&peak_lock);
        return 0;
    }

    const int  have = (int)audio_channels;

    if (num > 0) {
        const int  cmax = (num < have) ? num : have;
        for (int c = 0; c < cmax; c++)
            to[c] = peak_amplitude[c];
    }

    peak_available = 0;
    pthread_mutex_unlock(&peak_lock);
    return have;
}


int vu_start(const char *server,
             const char *appname,
             const char *devname,
             const char *stream,
             int         channels,
             int         rate,
             int         samples)
{
    pa_sample_spec  samplespec;
    pa_buffer_attr  bufferspec;
    pthread_attr_t  attrs;
    int             err;

    if (!appname || !*appname || !stream || !*stream ||
        channels < 1  || channels > 128 || rate < 1 || rate > 1000000 || samples < 1 || samples > 1000000) {
        return -EINVAL;
    }

    /* Empty or "default" server maps to NULL. */
    if (server && (!*server || !strcmp(server, "default")))
        server = NULL;

    /* Empty or "default" devname maps to NULL. */
    if (devname && (!*devname || !strcmp(devname, "default")))
        devname = NULL;

    /* If already running, stop. */
    if (audio) {
        vu_stop();
    }

    done = 0;

    pthread_mutex_lock(&peak_lock);

    samplespec.format   = PA_SAMPLE_S32NE;
    samplespec.rate     = rate;
    samplespec.channels = channels;

    bufferspec.maxlength = (uint32_t)(-1);
    bufferspec.tlength   = (uint32_t)(-1);
    bufferspec.prebuf    = (uint32_t)(-1);
    bufferspec.minreq    = (uint32_t)(-1);
    bufferspec.fragsize  = (uint32_t)channels * (uint32_t)samples * (uint32_t)sizeof audio_buffer[0];

    err = PA_OK;
    audio = pa_simple_new(server, appname, PA_STREAM_RECORD, devname, stream, &samplespec, NULL, &bufferspec, &err);
    if (!audio) {
        pthread_mutex_unlock(&peak_lock);
        return err;
    }

    /* Allocate memory for the various buffers. */
    audio_buffer = calloc((size_t)channels * sizeof audio_buffer[0], (size_t)samples);
    audio_min = malloc((size_t)channels * sizeof audio_min[0]);
    audio_max = malloc((size_t)channels * sizeof audio_max[0]);
    peak_amplitude = malloc((size_t)channels * sizeof peak_amplitude[0]);
    if (!audio_buffer || !audio_min || !audio_max || !peak_amplitude) {
        free(peak_amplitude);
        free(audio_max);
        free(audio_min);
        free(audio_buffer);
        pa_simple_free(audio);
        audio = NULL;
        audio_buffer = NULL;
        audio_min = NULL;
        audio_max = NULL;
        peak_amplitude = NULL;
        pthread_mutex_unlock(&peak_lock);
        return -ENOMEM;
    }

    for (int c = 0; c < channels; c++)
        peak_amplitude[c] = 0.0f;

    peak_available = 0;

    audio_channels = channels;
    audio_samples  = samples;

    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, 2 * PTHREAD_STACK_MIN);

    err = pthread_create(&audio_thread, &attrs, worker, NULL);
    if (err) {
        pa_simple_free(audio);
        free(peak_amplitude);
        free(audio_max);
        free(audio_min);
        free(audio_buffer);
        audio = NULL;
        audio_buffer = NULL;
        audio_min = NULL;
        audio_max = NULL;
        peak_amplitude = NULL;
        audio_channels = 0;
        audio_samples = 0;
        pthread_mutex_unlock(&peak_lock);
        return -err;
    }

    pthread_attr_destroy(&attrs);

    pthread_mutex_unlock(&peak_lock);

    return 0;
}
