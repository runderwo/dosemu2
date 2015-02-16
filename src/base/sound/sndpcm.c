/*
 *  Copyright (C) 2006 Stas Sergeev <stsp@users.sourceforge.net>
 *
 * The below copyright strings have to be distributed unchanged together
 * with this file. This prefix can not be modified or separated.
 */

/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Purpose: Sound midlayer. Resampling, converting and all the other hard work.
 *
 * Author: Stas Sergeev.
 *
 * TODO: Add the ADPCM processing.
 * TODO: Add the PDM processing for PC-Speaker.
 */

#include "emu.h"
#include "utilities.h"
#include "ringbuf.h"
#include "timers.h"
#include "sound/sound.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include <pthread.h>
#include <assert.h>


#define SND_BUFFER_SIZE 100000	/* enough to hold 1.1s of 44100/stereo */
#define BUFFER_DELAY 40000.0

#define MIN_BUFFER_DELAY (BUFFER_DELAY)
#define NORM_BUFFER_DELAY (BUFFER_DELAY * 2)
#define INIT_BUFFER_DELAY (BUFFER_DELAY * 4)
#define MAX_BUFFER_DELAY (BUFFER_DELAY * 10)
#define MAX_BUFFER_PERIOD (MAX_BUFFER_DELAY - MIN_BUFFER_DELAY)
#define MIN_GUARD_SIZE 1024
#define MIN_READ_GUARD_PERIOD (1000000 * MIN_GUARD_SIZE / (2 * 44100))
#define WR_BUFFER_LW (BUFFER_DELAY / 2)
#define MIN_READ_DELAY (MIN_BUFFER_DELAY + MIN_READ_GUARD_PERIOD)
#define WRITE_AREA_SIZE MIN_BUFFER_DELAY
#define WRITE_INIT_POS (WRITE_AREA_SIZE / 2)

/*    Layout of our buffer is as follows:
 *
 *                  |              |
 * WRITE_INIT_POS ->+  write area  +<---------------------- <-WR_BUFFER_LW (GC)
 *                  |              |                       \
 * WRITE_AREA_SIZE->+--------------+->MIN_BUFFER_DELAY     |
 *                  |              |                       |
 *                  +  read area   +->INIT_BUFFER_DELAY    |
 *                  |              |                       |
 *                  +--------------+->MAX_BUFFER_DELAY     |
 *                  |              |                       |
 *                  |   GC area    +---------------------->
 *                  \_____________/
 */

enum {
    SNDBUF_STATE_INACTIVE,
    SNDBUF_STATE_PLAYING,
    SNDBUF_STATE_FLUSHING,
    SNDBUF_STATE_STALLED,
};

struct sample {
    int format;
    double tstamp;
    unsigned char data[2];
};

static const struct sample mute_samp = { PCM_FORMAT_NONE, 0, {0, 0} };

struct stream {
    int channels;
    struct rng_s buffer;
    int buf_cnt;
    int state;
    int flags;
    int stretch:1;
    int prepared:1;
    double start_time;
    double stop_time;
    double stretch_per;
    double stretch_tot;
    /* for the raw channels heuristic */
    double raw_speed_adj;
    double last_adj_time;
    double last_fillup;
    /* --- */
    char *name;
    int id;
};

#define MAX_STREAMS 10
#define MAX_PLAYERS 10
struct pcm_player_wr {
    struct pcm_player player;
    double time;
    int opened:1;
    int last_cnt[MAX_STREAMS];
    int last_idx[MAX_STREAMS];
    double last_tstamp[MAX_STREAMS];
};

struct pcm_struct {
    struct stream stream[MAX_STREAMS];
    int num_streams;
    pthread_mutex_t strm_mtx;
    pthread_mutex_t time_mtx;
    struct pcm_player_wr players[MAX_PLAYERS];
    int num_players;
    int playing;
    double time;
} pcm;

int pcm_init(void)
{
    int i;
    S_printf("PCM: init\n");
    pthread_mutex_init(&pcm.strm_mtx, NULL);
    pthread_mutex_init(&pcm.time_mtx, NULL);
#ifdef USE_DL_PLUGINS
#if 0
    /* SDL is loaded in config.h, not here */
#ifdef SDL_SUPPORT
    load_plugin("sdl");
#endif
#endif
#ifdef USE_ALSA
    load_plugin("alsa");
#endif
#ifdef USE_SNDFILE
    load_plugin("sndfile");
#endif
#ifdef USE_FLUIDSYNTH
    load_plugin("fluidsynth");
#endif
#endif
    for (i = 0; i < pcm.num_players; i++) {
	struct pcm_player_wr *p = &pcm.players[i];
	if (p->player.open)
	    p->opened = p->player.open(p->player.arg);
	else
	    p->opened = 1;
	if (!p->opened) {
	    S_printf("PCM: \"%s\" failed to open\n",
		  p->player.name);
	}
    }
    return 1;
}

static void pcm_clear_stream(int strm_idx)
{
    pcm.stream[strm_idx].buf_cnt += rng_count(&pcm.stream[strm_idx].buffer);
    rng_clear(&pcm.stream[strm_idx].buffer);
}

static void pcm_reset_stream(int strm_idx)
{
    pcm_clear_stream(strm_idx);
    if (pcm.stream[strm_idx].state != SNDBUF_STATE_FLUSHING)
	pcm.stream[strm_idx].prepared = 0;
    pcm.stream[strm_idx].state = SNDBUF_STATE_INACTIVE;
    pcm.stream[strm_idx].stretch = 0;
    pcm.stream[strm_idx].stretch_per = 0;
    pcm.stream[strm_idx].stretch_tot = 0;
}

int pcm_allocate_stream(int channels, char *name, int id)
{
    int index;
    if (pcm.num_streams >= MAX_STREAMS) {
	error("PCM: stream pool exhausted, max=%i\n", MAX_STREAMS);
	return -1;
    }
    index = pcm.num_streams++;
    rng_init(&pcm.stream[index].buffer, SND_BUFFER_SIZE,
	     sizeof(struct sample));
    pcm.stream[index].buf_cnt = 0;
    pcm.stream[index].channels = channels;
    pcm.stream[index].name = name;
    pcm.stream[index].id = id;
    pcm_reset_stream(index);
    S_printf("PCM: Stream %i allocated for \"%s\"\n", index, name);
    return index;
}

void pcm_set_flag(int strm_idx, int flag)
{
    if (pcm.stream[strm_idx].flags & flag)
	return;
    S_printf("PCM: setting flag %x for stream %i (%s)\n",
	     flag, strm_idx, pcm.stream[strm_idx].name);
    pcm.stream[strm_idx].flags |= flag;
    if (pcm.stream[strm_idx].flags & PCM_FLAG_RAW)
	pcm.stream[strm_idx].raw_speed_adj = 1.0;
}

void pcm_clear_flag(int strm_idx, int flag)
{
    if (!(pcm.stream[strm_idx].flags & flag))
	return;
    S_printf("PCM: clearing flag %x for stream %i (%s)\n",
	     flag, strm_idx, pcm.stream[strm_idx].name);
    pcm.stream[strm_idx].flags &= ~flag;
}

int pcm_format_size(int format)
{
    switch (format) {
    case PCM_FORMAT_U8:
    case PCM_FORMAT_S8:
	return 1;
    case PCM_FORMAT_U16_LE:
    case PCM_FORMAT_S16_LE:
	return 2;
    default:
	error("PCM: format %i is not supported\n", format);
	return 0;
    }
}

static int pcm_get_format8(int is_signed)
{
    return is_signed ? PCM_FORMAT_S8 : PCM_FORMAT_U8;
}

static int pcm_get_format16(int is_signed)
{
    return is_signed ? PCM_FORMAT_S16_LE : PCM_FORMAT_U16_LE;
}

int pcm_get_format(int is_16, int is_signed)
{
    return is_16 ? pcm_get_format16(is_signed) :
	pcm_get_format8(is_signed);
}

static int cutoff(int val, int min, int max)
{
    if (val < min)
	return min;
    if (val > max)
	return max;
    return val;
}

int pcm_samp_cutoff(int val, int format)
{
    switch (format) {
    case PCM_FORMAT_U8:
	return cutoff(val, 0, UCHAR_MAX);
    case PCM_FORMAT_S8:
	return cutoff(val, SCHAR_MIN, SCHAR_MAX);
    case PCM_FORMAT_U16_LE:
	return cutoff(val, 0, USHRT_MAX);
    case PCM_FORMAT_S16_LE:
	return cutoff(val, SHRT_MIN, SHRT_MAX);
    default:
	error("PCM: format %i is not supported\n", format);
	return 0;
    }
}

#define UC2SS(v) ((*(unsigned char *)(v) - 128) * 256)
#define SC2SS(v) (*(signed char *)(v) * 256)
#define US2SS(v) (*(unsigned short *)(v) - 32768)
#define SS2UC(v) ((unsigned char)(((v) + 32768) / 256))
#define SS2SC(v) ((signed char)((v) / 256))
#define SS2US(v) ((unsigned short)((v) + 32768))

static short sample_to_S16(void *data, int format)
{
    switch (format) {
    case PCM_FORMAT_U8:
	return UC2SS(data);
    case PCM_FORMAT_S8:
	return SC2SS(data);
    case PCM_FORMAT_U16_LE:
	return US2SS(data);
    case PCM_FORMAT_S16_LE:
	return *(short *) data;
    default:
	error("PCM: format %i is not supported\n", format);
	return 0;
    }
}

static void S16_to_sample(short sample, sndbuf_t *buf, int format)
{
    switch (format) {
    case PCM_FORMAT_U8:
	*buf = SS2UC(sample);
	break;
    case PCM_FORMAT_S8:
	*buf = SS2SC(sample);
	break;
    case PCM_FORMAT_U16_LE:
	*buf = SS2US(sample);
	break;
    case PCM_FORMAT_S16_LE:
	*buf = sample;
	break;
    default:
	error("PCM: format1 %i is not supported\n", format);
    }
}

static int count_active_streams(int id)
{
    int i, ret = 0;
    for (i = 0; i < pcm.num_streams; i++) {
	if (id != PCM_ID_MAX && pcm.stream[i].id != id)
	    continue;
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE)
	    continue;
	ret++;
    }
    return ret;
}

double pcm_frame_period_us(int rate)
{
    return (1000000.0 / rate);
}

double pcm_frag_period(int size, struct player_params *params)
{
    int samp_sz, nsamps;
    samp_sz = pcm_format_size(params->format);
    nsamps = size / samp_sz;
    return nsamps * pcm_frame_period_us(params->rate) / params->channels;
}

int pcm_frag_size(double period, struct player_params *params)
{
    int nframes = period / pcm_frame_period_us(params->rate);
    int nsamps = nframes * params->channels;
    return nsamps * pcm_format_size(params->format);
}

static int peek_last_sample(int strm_idx, struct sample *samp)
{
    int idx = rng_count(&pcm.stream[strm_idx].buffer);
    if (!idx)
	return 0;
    return rng_peek(&pcm.stream[strm_idx].buffer, idx - 1, samp);
}

void pcm_prepare_stream(int strm_idx)
{
    long long now = GETusTIME(0);
    struct stream *s = &pcm.stream[strm_idx];
    switch (s->state) {

    case SNDBUF_STATE_PLAYING:
	error("PCM: prepare playing stream %s\n", s->name);
	return;

    case SNDBUF_STATE_STALLED:
	error("PCM: prepare stalled stream %s\n", s->name);
	/* should never happen, but if we are here we reset stretches */
	pthread_mutex_lock(&pcm.strm_mtx);
	pcm_reset_stream(strm_idx);
	pthread_mutex_unlock(&pcm.strm_mtx);
	break;

    case SNDBUF_STATE_FLUSHING:
	/* very careful: because of poor syncing the stupid things happen.
	 * Like, for instance, samples written in the future... */
	if (now < s->stop_time) {
	    S_printf("PCM: ERROR: sample in the future, %f now=%llu, %s\n",
		    s->stop_time, now, s->name);
	    now = s->stop_time;
	}
	break;
    }

    s->start_time = now;
    s->prepared = 1;
}

static void pcm_stream_stretch(int strm_idx)
{
    long long now = GETusTIME(0);
    struct stream *s = &pcm.stream[strm_idx];
    assert(s->state != SNDBUF_STATE_PLAYING &&
	    s->state != SNDBUF_STATE_INACTIVE);
    s->start_time = now;
    s->stretch = 1;
}

static double calc_buffer_fillup(int strm_idx, double time)
{
    struct stream *s = &pcm.stream[strm_idx];
    if (s->state != SNDBUF_STATE_PLAYING && s->state != SNDBUF_STATE_FLUSHING)
	return 0;
    return s->stop_time > time ? s->stop_time - time : 0;
}

static void pcm_start_output(int id)
{
    int i;
    long long now = GETusTIME(0);
    for (i = 0; i < pcm.num_players; i++) {
	struct pcm_player_wr *p = &pcm.players[i];
	if (p->player.id != id)
	    continue;
	if (p->opened) {
	    pcm_reset_player(i);
	    pthread_mutex_unlock(&pcm.strm_mtx);
	    p->player.start(p->player.arg);
	    pthread_mutex_lock(&pcm.strm_mtx);
	}
    }
    pcm.time = now - MAX_BUFFER_DELAY;
    pcm.playing |= (1 << id);
    S_printf("PCM: output started\n");
}

static void pcm_stop_output(int id)
{
    int i;
    for (i = 0; i < pcm.num_players; i++) {
	struct pcm_player_wr *p = &pcm.players[i];
	if (id != PCM_ID_MAX && p->player.id != id)
	    continue;
	if (p->opened) {
	    pthread_mutex_unlock(&pcm.strm_mtx);
	    p->player.stop(p->player.arg);
	    pthread_mutex_lock(&pcm.strm_mtx);
	}
    }
    pcm.playing &= ~(1 << id);
    S_printf("PCM: output stopped\n");
}

static void pcm_handle_get(int strm_idx, double time)
{
    double fillup = calc_buffer_fillup(strm_idx, time);
    if (debug_level('S') >= 9)
	S_printf("PCM: Buffer %i fillup=%f\n", strm_idx, fillup);
    switch (pcm.stream[strm_idx].state) {

    case SNDBUF_STATE_INACTIVE:
	error("PCM: getting data from inactive buffer (strm=%i)\n",
	      strm_idx);
	break;

    case SNDBUF_STATE_PLAYING:
#if 1
	if (pcm.stream[strm_idx].flags & PCM_FLAG_RAW) {
#define ADJ_PERIOD 2000000
	    double raw_delay = NORM_BUFFER_DELAY;
	    double delta = (fillup - raw_delay) / (raw_delay * 320);
	    if (time - pcm.stream[strm_idx].last_adj_time > ADJ_PERIOD) {
		/* of course this heuristic doesnt work, but we have to try... */
		if ((fillup > raw_delay * 2 &&
		     fillup > pcm.stream[strm_idx].last_fillup) ||
		    (fillup < raw_delay / 1.5 &&
		     fillup < pcm.stream[strm_idx].last_fillup)) {
		    pcm.stream[strm_idx].raw_speed_adj -= delta;
		    if (pcm.stream[strm_idx].raw_speed_adj > 5)
			pcm.stream[strm_idx].raw_speed_adj = 5;
		    if (pcm.stream[strm_idx].raw_speed_adj < 0.2)
			pcm.stream[strm_idx].raw_speed_adj = 0.2;
//          error("speed %f\n", pcm.stream[strm_idx].raw_speed_adj);
		}
		pcm.stream[strm_idx].last_fillup = fillup;
		pcm.stream[strm_idx].last_adj_time = time;
	    }
	}
#endif
	if (rng_count(&pcm.stream[strm_idx].buffer) <
	    pcm.stream[strm_idx].channels * 2 && fillup == 0) {
	    S_printf("PCM: ERROR: buffer on stream %i exhausted (%s)\n",
		      strm_idx, pcm.stream[strm_idx].name);
	    /* ditch the last sample here, if it is the only remaining */
	    pcm_clear_stream(strm_idx);
	}
	if (fillup == 0) {
	    if (!(pcm.stream[strm_idx].flags & PCM_FLAG_RAW))
		S_printf("PCM: ERROR: buffer on stream %i stalled (%s)\n",
		      strm_idx, pcm.stream[strm_idx].name);
	    pcm.stream[strm_idx].state = SNDBUF_STATE_STALLED;
	}
	if (pcm.stream[strm_idx].state == SNDBUF_STATE_PLAYING &&
		!(pcm.stream[strm_idx].flags & PCM_FLAG_POST) &&
		fillup < WR_BUFFER_LW) {
	    S_printf("PCM: buffer fillup %f is too low, %s %i %f\n",
		    fillup, pcm.stream[strm_idx].name,
		    rng_count(&pcm.stream[strm_idx].buffer), time);
	}
	break;

    case SNDBUF_STATE_FLUSHING:
	if (rng_count(&pcm.stream[strm_idx].buffer) <
		pcm.stream[strm_idx].channels * 2 && fillup == 0) {
	    pcm_reset_stream(strm_idx);
	    S_printf("PCM: stream %s stopped\n", pcm.stream[strm_idx].name);
	} else if (fillup == 0 && !pcm.stream[strm_idx].stretch) {
	    pcm_stream_stretch(strm_idx);
	    S_printf("PCM: stream %s passed wr margin\n",
		    pcm.stream[strm_idx].name);
	}
	break;

    case SNDBUF_STATE_STALLED:
	break;
    }
}

static void pcm_handle_write(int strm_idx, double time)
{
    if (pcm.playing && time < pcm.time + MAX_BUFFER_PERIOD) {
	if (pcm.stream[strm_idx].state == SNDBUF_STATE_PLAYING)
	    error("PCM: timing screwed up\n");
	S_printf("PCM: timing screwed up, s=%s cur=%f pl=%f delta=%f\n",
		pcm.stream[strm_idx].name, time, pcm.time,
		pcm.time - time);
    }

    switch (pcm.stream[strm_idx].state) {
    case SNDBUF_STATE_STALLED:
	pcm.stream[strm_idx].stretch_tot += pcm.stream[strm_idx].stretch_per;
	S_printf("PCM: restarting stalled stream %s, str=%f strt=%f\n",
		pcm.stream[strm_idx].name, pcm.stream[strm_idx].stretch_per,
		pcm.stream[strm_idx].stretch_tot);
	pcm.stream[strm_idx].stretch_per = 0;
	break;
    case SNDBUF_STATE_FLUSHING:
	if (pcm.stream[strm_idx].stretch)
	    S_printf("PCM: restarting stream %s\n", pcm.stream[strm_idx].name);
	else
	    S_printf("PCM: resuming stream %s\n", pcm.stream[strm_idx].name);
	break;
    }

    if (pcm.stream[strm_idx].state != SNDBUF_STATE_PLAYING) {
	pcm.stream[strm_idx].state = SNDBUF_STATE_PLAYING;
	pcm.stream[strm_idx].stretch = 0;
	pcm.stream[strm_idx].prepared = 0;
    }
}

static void pcm_handle_flush(int strm_idx)
{
    switch (pcm.stream[strm_idx].state) {

    case SNDBUF_STATE_INACTIVE:
	if (!(pcm.stream[strm_idx].flags & PCM_FLAG_RAW)) {
	    error("PCM: attempt to flush inactive buffer (%s)\n",
		  pcm.stream[strm_idx].name);
	}
	break;

    case SNDBUF_STATE_PLAYING:
	pcm.stream[strm_idx].state = SNDBUF_STATE_FLUSHING;
	break;

    case SNDBUF_STATE_STALLED:
	pthread_mutex_lock(&pcm.strm_mtx);
	pcm_reset_stream(strm_idx);
	pthread_mutex_unlock(&pcm.strm_mtx);
	break;
    }
}

int pcm_flush(int strm_idx)
{
    S_printf("PCM: flushing stream %i (%s)\n", strm_idx,
	     pcm.stream[strm_idx].name);
    pcm_handle_flush(strm_idx);
    return 1;
}

static double get_stream_time(int strm_idx)
{
    long long now = GETusTIME(0);
    double time = pcm.stream[strm_idx].start_time;
    double delta = now - time;
    switch (pcm.stream[strm_idx].state) {

    case SNDBUF_STATE_INACTIVE:
	if (pcm.stream[strm_idx].prepared) {
user_tstamp:
	    if (delta > WRITE_AREA_SIZE) {
		error("PCM: too large delta on stream %s\n",
			pcm.stream[strm_idx].name);
		pcm.stream[strm_idx].start_time = time = now - WRITE_INIT_POS;
	    }
	    return time;
	}
	S_printf("PCM: ERROR: not prepared %s\n", pcm.stream[strm_idx].name);
	return now - WRITE_INIT_POS;

    case SNDBUF_STATE_STALLED:
	pcm.stream[strm_idx].stretch_per = now - WRITE_AREA_SIZE -
		pcm.stream[strm_idx].stop_time;
	return now - WRITE_AREA_SIZE;

    case SNDBUF_STATE_FLUSHING:
	if (pcm.stream[strm_idx].stretch)
	    return now - fmod(delta, MIN_BUFFER_DELAY);
	if (pcm.stream[strm_idx].prepared &&
		!(pcm.stream[strm_idx].flags & PCM_FLAG_SLTS))
	    goto user_tstamp;
	/* in SLoppy TimeStamp mode ignore user's timestamp */
	/* no break */
    case SNDBUF_STATE_PLAYING:
	return pcm.stream[strm_idx].stop_time;

    }
    return 0;
}

double pcm_get_stream_time(int strm_idx)
{
    /* we allow user to write samples to the future to prevent
     * subsequent underflows */
    return get_stream_time(strm_idx) - pcm.stream[strm_idx].stretch_tot;
}

double pcm_time_lock(int strm_idx)
{
    /* well, yes, the lock needs to be per-stream... Go get it. :) */
    pthread_mutex_lock(&pcm.time_mtx);
    return pcm_get_stream_time(strm_idx);
}

void pcm_time_unlock(int strm_idx)
{
    pthread_mutex_unlock(&pcm.time_mtx);
}

static double pcm_calc_tstamp(int strm_idx)
{
    double tstamp = get_stream_time(strm_idx);
    if (pcm.stream[strm_idx].flags & PCM_FLAG_RAW) {
	long long now = GETusTIME(0);
	if (tstamp < now)
	    tstamp = now;
    }
    return tstamp;
}

void pcm_write_interleaved(sndbuf_t ptr[][SNDBUF_CHANS], int frames,
	int rate, int format, int nchans, int strm_idx)
{
    int i, j;
    struct sample samp;
    double frame_per;
    assert(nchans <= pcm.stream[strm_idx].channels);
    if (pcm.stream[strm_idx].flags & PCM_FLAG_RAW)
	rate /= pcm.stream[strm_idx].raw_speed_adj;

    samp.format = format;
    samp.tstamp = 0;
    frame_per = pcm_frame_period_us(rate);
    pthread_mutex_lock(&pcm.strm_mtx);
    for (i = 0; i < frames; i++) {
	int l;
	struct sample s2;
retry:
	samp.tstamp = pcm_calc_tstamp(strm_idx);
	l = peek_last_sample(strm_idx, &s2);
	assert(!(l && samp.tstamp < s2.tstamp));
	for (j = 0; j < pcm.stream[strm_idx].channels; j++) {
	    int ch = j % nchans;
	    memcpy(samp.data, &ptr[i][ch], pcm_format_size(format));
	    l = rng_put(&pcm.stream[strm_idx].buffer, &samp);
	    if (!l) {
		error("Sound buffer %i overflowed (%s)\n", strm_idx,
		    pcm.stream[strm_idx].name);
		pcm_reset_stream(strm_idx);
		goto retry;
	    }
	}
	pcm_handle_write(strm_idx, samp.tstamp);
	pcm.stream[strm_idx].stop_time = samp.tstamp + frame_per;
    }

    if (!(pcm.playing & (1 << pcm.stream[strm_idx].id)))
	pcm_start_output(pcm.stream[strm_idx].id);
    pthread_mutex_unlock(&pcm.strm_mtx);
}

static void pcm_remove_samples(double time)
{
    #define GUARD_SAMPS 1
    int i;
    struct sample s;
    for (i = 0; i < pcm.num_streams; i++) {
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE)
	    continue;
	while (rng_count(&pcm.stream[i].buffer) >= pcm.stream[i].channels *
		(GUARD_SAMPS + 1)) {
	    /* we leave GUARD_SAMPS samples below the timestamp untouched */
	    rng_peek(&pcm.stream[i].buffer, pcm.stream[i].channels *
		    GUARD_SAMPS, &s);
	    if (s.tstamp > time)
		break;
	    pcm.stream[i].buf_cnt += pcm.stream[i].channels;
	    rng_remove(&pcm.stream[i].buffer, pcm.stream[i].channels, NULL);
	}
    }
}

static void pcm_get_samples(double time,
		struct sample samp[MAX_STREAMS][SNDBUF_CHANS], int *idxs,
		int out_channels, int id)
{
    int i, j;
    struct sample s[SNDBUF_CHANS], prev_s[SNDBUF_CHANS];

    for (i = 0; i < pcm.num_streams; i++) {
	for (j = 0; j < out_channels; j++)
	    samp[i][j] = mute_samp;
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE ||
		pcm.stream[i].id != id)
	    continue;

//    S_printf("PCM: stream %i fillup: %i\n", i, rng_count(&pcm.stream[i].buffer));
	for (j = 0; j < pcm.stream[i].channels; j++) {
	    if (idxs[i] >= pcm.stream[i].channels)
		rng_peek(&pcm.stream[i].buffer,
			 idxs[i] - pcm.stream[i].channels + j, &prev_s[j]);
	    else
		prev_s[j] = mute_samp;
	}
	if (out_channels == 2 && pcm.stream[i].channels == 1)
	    prev_s[1] = prev_s[0];
	while (rng_count(&pcm.stream[i].buffer) - idxs[i] >=
	       pcm.stream[i].channels) {
	    for (j = 0; j < pcm.stream[i].channels; j++)
		rng_peek(&pcm.stream[i].buffer, idxs[i] + j, &s[j]);
	    if (s[0].tstamp > time) {
//        S_printf("PCM: stream %i time=%lli, req_time=%lli\n", i, s.tstamp, time);
		memcpy(samp[i], prev_s, sizeof(struct sample) * out_channels);
		break;
	    }
	    memcpy(prev_s, s, sizeof(struct sample) * pcm.stream[i].channels);
	    if (out_channels == 2 && pcm.stream[i].channels == 1)
		prev_s[1] = prev_s[0];
	    idxs[i] += pcm.stream[i].channels;
	}
    }
}

static void pcm_mix_samples(struct sample in[][SNDBUF_CHANS],
	sndbuf_t out[SNDBUF_CHANS], int channels, int format)
{
    int i, j;
    int value[2] = { 0, 0 };

    for (j = 0; j < channels; j++) {
	for (i = 0; i < pcm.num_streams; i++) {
	    if (in[i][j].format == PCM_FORMAT_NONE)
		continue;
	    value[j] += sample_to_S16(in[i][j].data, in[i][j].format);
	}
	S16_to_sample(pcm_samp_cutoff(value[j], PCM_FORMAT_S16_LE),
		&out[j], format);
    }
}

static void calc_idxs(struct pcm_player_wr *pl, int idxs[MAX_STREAMS])
{
    int i;
    for (i = 0; i < pcm.num_streams; i++) {
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE)
	    continue;
	assert(pcm.stream[i].buf_cnt >= pl->last_cnt[i]);
	if (pl->last_idx[i] > pcm.stream[i].buf_cnt - pl->last_cnt[i]) {
	    struct sample s;
	    idxs[i] = pl->last_idx[i] - (pcm.stream[i].buf_cnt -
		    pl->last_cnt[i]);
	    assert(idxs[i] <= rng_count(&pcm.stream[i].buffer));
	    rng_peek(&pcm.stream[i].buffer, idxs[i] - 1, &s);
	    assert(pl->last_tstamp[i] == s.tstamp);
	} else {
	    idxs[i] = 0;
	}
    }
}

static void save_idxs(struct pcm_player_wr *pl, int idxs[MAX_STREAMS])
{
    int i;
    for (i = 0; i < pcm.num_streams; i++) {
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE)
	    continue;
	assert(idxs[i] <= rng_count(&pcm.stream[i].buffer));
	if (idxs[i] > 0) {
	    struct sample s;
	    rng_peek(&pcm.stream[i].buffer, idxs[i] - 1, &s);
	    pl->last_tstamp[i] = s.tstamp;
	}
	pl->last_cnt[i] = pcm.stream[i].buf_cnt;
	pl->last_idx[i] = idxs[i];
    }
}

int pcm_data_get_interleaved(sndbuf_t buf[][SNDBUF_CHANS], int nframes,
			   struct player_params *params)
{
    int idxs[MAX_STREAMS], out_idx, handle, id;
    long long now;
    double start_time, stop_time, frame_period, frag_period, time;
    struct sample samp[MAX_STREAMS][SNDBUF_CHANS];

    now = GETusTIME(0);
    handle = params->handle;
    id = pcm.players[handle].player.id;
    start_time = pcm.players[handle].time;
    frag_period = nframes * pcm_frame_period_us(params->rate);
    stop_time = start_time + frag_period;
    if (start_time < now - MAX_BUFFER_DELAY) {
	error("PCM: \"%s\" too large delay, start=%f min=%f d=%f\n",
		  pcm.players[handle].player.name, start_time,
		  now - MAX_BUFFER_DELAY, now - MAX_BUFFER_DELAY - start_time);
	start_time = now - INIT_BUFFER_DELAY;
	stop_time = start_time + frag_period;
    }
    if (start_time > now - MIN_READ_DELAY) {
	error("PCM: \"%s\" too small delay, stop=%f max=%f d=%f\n",
		  pcm.players[handle].player.name, stop_time,
		  now - MIN_BUFFER_DELAY, stop_time -
		  (now - MIN_BUFFER_DELAY));
	start_time = now - INIT_BUFFER_DELAY;
	stop_time = start_time + frag_period;
    }
    if (stop_time > now - MIN_BUFFER_DELAY) {
	size_t new_nf;
	S_printf("PCM: \"%s\" too small delay, stop=%f max=%f d=%f\n",
		  pcm.players[handle].player.name, stop_time,
		  now - MIN_BUFFER_DELAY, stop_time -
		  (now - MIN_BUFFER_DELAY));
	stop_time = now - MIN_BUFFER_DELAY;
	frag_period = stop_time - start_time;
	new_nf = frag_period / pcm_frame_period_us(params->rate);
	assert(new_nf <= nframes);
	nframes = new_nf;
    }
    S_printf("PCM: going to process %i samps for %s (st=%f stp=%f d=%f)\n",
	 nframes, pcm.players[handle].player.name, start_time,
	 stop_time, now - start_time);

    pthread_mutex_lock(&pcm.strm_mtx);
    if (!pcm.players[handle].opened) {
	S_printf("PCM: player %s already closed\n",
		pcm.players[handle].player.name);
	pthread_mutex_unlock(&pcm.strm_mtx);
	return 0;
    }
    frame_period = pcm_frame_period_us(params->rate);
    time = start_time;
    calc_idxs(&pcm.players[handle], idxs);

    for (out_idx = 0; out_idx < nframes; out_idx++) {
	pcm_get_samples(time, samp, idxs, params->channels, id);
	pcm_mix_samples(samp, buf[out_idx], params->channels, params->format);
	time += frame_period;
    }
    if (fabs(time - stop_time) > frame_period)
	error("PCM: time=%f stop_time=%f p=%f\n",
		    time, stop_time, frame_period);
    pcm.players[handle].time = stop_time;
    save_idxs(&pcm.players[handle], idxs);
    pthread_mutex_unlock(&pcm.strm_mtx);

    if (out_idx != nframes)
	error("PCM: requested=%i prepared=%i\n", nframes, out_idx);
    return out_idx;
}

size_t pcm_data_get(void *data, size_t size,
			   struct player_params *params)
{
    int i, j;
    sndbuf_t buf[size][SNDBUF_CHANS];
    int ss = pcm_format_size(params->format);
    int fsz = params->channels * ss;
    int nframes = size / fsz;
    nframes = pcm_data_get_interleaved(buf, nframes, params);
    for (i = 0; i < nframes; i++) {
	for (j = 0; j < params->channels; j++)
	    memcpy(data + (i * fsz + j * ss), &buf[i][j], ss);
    }
    return nframes * fsz;
}

static void pcm_advance_time(double stop_time)
{
    int i;
    pthread_mutex_lock(&pcm.strm_mtx);
    pcm.time = stop_time;
    /* remove processed samples from input buffers (last sample stays) */
    pcm_remove_samples(stop_time);
    for (i = 0; i < pcm.num_streams; i++) {
	if (pcm.stream[i].state == SNDBUF_STATE_INACTIVE)
	    continue;
	if (debug_level('S') >= 9)
	    S_printf("PCM: stream %i fillup2: %i\n", i,
		 rng_count(&pcm.stream[i].buffer));
	pcm_handle_get(i, stop_time + MAX_BUFFER_PERIOD);
    }

    for (i = 0; i < PCM_ID_MAX; i++) {
	if (!count_active_streams(i) && (pcm.playing & (1 << i)))
	    pcm_stop_output(i);
    }
    pthread_mutex_unlock(&pcm.strm_mtx);
}

int pcm_register_player(struct pcm_player player)
{
    S_printf("PCM: registering clocked player: %s\n", player.name);
    if (pcm.num_players >= MAX_PLAYERS) {
	error("PCM: attempt to register more than %i clocked player\n",
	      MAX_PLAYERS);
	return 0;
    }
    pcm.players[pcm.num_players].player = player;
    return pcm.num_players++;
}

void pcm_reset_player(int handle)
{
    long long now = GETusTIME(0);
    pcm.players[handle].time = now - INIT_BUFFER_DELAY;
    memset(pcm.players[handle].last_idx, 0,
	    sizeof(pcm.players[handle].last_idx));
    memset(pcm.players[handle].last_cnt, 0,
	    sizeof(pcm.players[handle].last_cnt));
}

void pcm_timer(void)
{
    int i;
    long long now = GETusTIME(0);
    for (i = 0; i < pcm.num_players; i++) {
	struct pcm_player_wr *p = &pcm.players[i];
	if (p->opened && p->player.timer) {
	    double delta = now - NORM_BUFFER_DELAY - pcm.players[i].time;
	    p->player.timer(delta, p->player.arg);
	}
    }
    pthread_mutex_lock(&pcm.time_mtx);
    pcm_advance_time(now - MAX_BUFFER_DELAY);
    pthread_mutex_unlock(&pcm.time_mtx);
}

void pcm_reset(void)
{
    int i;
    S_printf("PCM: reset\n");
    pthread_mutex_lock(&pcm.strm_mtx);
    if (pcm.playing)
	pcm_stop_output(PCM_ID_MAX);
    for (i = 0; i < pcm.num_streams; i++)
	pcm_reset_stream(i);
    pthread_mutex_unlock(&pcm.strm_mtx);
}

void pcm_done(void)
{
    int i;
    for (i = 0; i < pcm.num_streams; i++) {
	if (pcm.stream[i].state == SNDBUF_STATE_PLAYING ||
		pcm.stream[i].state == SNDBUF_STATE_STALLED)
	    pcm_flush(i);
    }
    pthread_mutex_lock(&pcm.strm_mtx);
    if (pcm.playing)
	pcm_stop_output(PCM_ID_MAX);
    for (i = 0; i < pcm.num_players; i++) {
	struct pcm_player_wr *p = &pcm.players[i];
	if (p->opened) {
	    if (p->player.close) {
		pthread_mutex_unlock(&pcm.strm_mtx);
		p->player.close(p->player.arg);
		pthread_mutex_lock(&pcm.strm_mtx);
	    }
	    p->opened = 0;
	}
    }
    for (i = 0; i < pcm.num_streams; i++)
	rng_destroy(&pcm.stream[i].buffer);
    pthread_mutex_unlock(&pcm.strm_mtx);
    pthread_mutex_destroy(&pcm.strm_mtx);
    pthread_mutex_destroy(&pcm.time_mtx);
}
