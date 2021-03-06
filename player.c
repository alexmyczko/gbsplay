/*
 * gbsplay is a Gameboy sound player
 * This file contains the player code common to both CLI and X11 frontends.
 *
 * 2003-2005,2008,2018,2020 (C) by Tobias Diedrich <ranma+gbsplay@tdiedrich.de>
 *                                 Christian Garbs <mitch@cgarbs.de>
 * Licensed under GNU GPL v1 or, at your option, any later version.
 */

#include "common.h"

#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "util.h"
#include "cfgparser.h"

#include "player.h"

/* global variables */
char *myname;
char *filename;

#define DEFAULT_REFRESH_DELAY 33

long refresh_delay = DEFAULT_REFRESH_DELAY; /* msec */

static long *subsong_playlist;
static long subsong_playlist_idx = 0;

static unsigned long random_seed;

/* default values */
long verbosity = 3;

plugout_open_fn  sound_open;
plugout_skip_fn  sound_skip;
plugout_pause_fn sound_pause;
plugout_io_fn    sound_io;
plugout_step_fn  sound_step;
plugout_write_fn sound_write;
plugout_close_fn sound_close;

static long playmode = PLAYMODE_LINEAR;
static long loopmode = 0;
static enum plugout_endian endian = PLUGOUT_ENDIAN_NATIVE;
static long rate = 44100;
static long silence_timeout = 2;
static long fadeout = 3;
static long subsong_gap = 2;
static long subsong_start = -1;
static long subsong_stop = -1;
static long subsong_timeout = 2*60;

static const char cfgfile[] = ".gbsplayrc";

static char *sound_name = PLUGOUT_DEFAULT;
static char *filter_type = GBHW_CFG_FILTER_DMG;
static char *sound_description;

static struct gbhw_buffer buf = {
	.data = NULL,
	.pos  = 0,
	.bytes = 8192,
};

/* configuration directives */
const struct cfg_option options[] = {
	{ "endian", &endian, cfg_endian },
	{ "fadeout", &fadeout, cfg_long },
	{ "filter_type", &filter_type, cfg_string },
	{ "loop", &loopmode, cfg_long },
	{ "output_plugin", &sound_name, cfg_string },
	{ "rate", &rate, cfg_long },
	{ "refresh_delay", &refresh_delay, cfg_long },
	{ "silence_timeout", &silence_timeout, cfg_long },
	{ "subsong_gap", &subsong_gap, cfg_long },
	{ "subsong_timeout", &subsong_timeout, cfg_long },
	{ "verbosity", &verbosity, cfg_long },
	/* playmode not implemented yet */
	{ NULL, NULL, NULL }
};

static void swap_endian(struct gbhw_buffer *buf)
{
	long i;

	for (i=0; i<buf->bytes/sizeof(short); i++) {
		short x = buf->data[i];
		buf->data[i] = ((x & 0xff) << 8) | (x >> 8);
	}
}

static void iocallback(long cycles, uint32_t addr, uint8_t val, void *priv)
{
	sound_io(cycles, addr, val);
}

static void callback(struct gbhw_buffer *buf, void *priv)
{
	if ((is_le_machine() && endian == PLUGOUT_ENDIAN_BIG) ||
	    (is_be_machine() && endian == PLUGOUT_ENDIAN_LITTLE)) {
		swap_endian(buf);
	}
	sound_write(buf->data, buf->pos*2*sizeof(int16_t));
	buf->pos = 0;
}

long *setup_playlist(long songs)
/* setup a playlist in shuffle mode */
{
	long i;
	long *playlist;
	
	playlist = (long*) calloc( songs, sizeof(long) );
	for (i=0; i<songs; i++) {
		playlist[i] = i;
	}

	/* reinit RNG with current seed - playlists shall be reproducible! */
	srand(random_seed);
	shuffle_long(playlist, songs);

	return playlist;
}

long get_next_subsong(struct gbs *gbs)
/* returns the number of the subsong that is to be played next */
{
	long next = -1;
	switch (playmode) {

	case PLAYMODE_RANDOM:
		next = rand_long(gbs->songs);
		break;

	case PLAYMODE_SHUFFLE:
		subsong_playlist_idx++;
		if (subsong_playlist_idx == gbs->songs) {
			free(subsong_playlist);
			random_seed++;
			subsong_playlist = setup_playlist(gbs->songs);
			subsong_playlist_idx = 0;
		}
		next = subsong_playlist[subsong_playlist_idx];
		break;

	case PLAYMODE_LINEAR:
		next = gbs->subsong + 1;
		break;
	}
	return next;
}

int get_prev_subsong(struct gbs *gbs)
/* returns the number of the subsong that has been played previously */
{
	int prev = -1;
	switch (playmode) {

	case PLAYMODE_RANDOM:
		prev = rand_long(gbs->songs);
		break;

	case PLAYMODE_SHUFFLE:
		subsong_playlist_idx--;
		if (subsong_playlist_idx == -1) {
			free(subsong_playlist);
			random_seed--;
			subsong_playlist = setup_playlist(gbs->songs);
			subsong_playlist_idx = gbs->songs-1;
		}
		prev = subsong_playlist[subsong_playlist_idx];
		break;

	case PLAYMODE_LINEAR:
		prev = gbs->subsong - 1;
		break;
	}
	return prev;
}

static void setup_playmode(struct gbs *gbs)
/* initializes the chosen playmode (set start subsong etc.) */
{
	switch (playmode) {

	case PLAYMODE_RANDOM:
		if (gbs->subsong == -1) {
			gbs->subsong = get_next_subsong(gbs);
		}
		break;

	case PLAYMODE_SHUFFLE:
		subsong_playlist = setup_playlist(gbs->songs);
		subsong_playlist_idx = 0;
		if (gbs->subsong == -1) {
			gbs->subsong = subsong_playlist[0];
		} else {
			/* randomize playlist until desired start song is first */
			/* (rotation does not work because this must be reproducible */
			/* by setting random_seed to the old value */
			while (subsong_playlist[0] != gbs->subsong) {
				random_seed++;
				subsong_playlist = setup_playlist(gbs->songs);
			}
		}
		break;

	case PLAYMODE_LINEAR:
		if (gbs->subsong == -1) {
			gbs->subsong = gbs->defaultsong - 1;
		}
		break;
	}
}

long nextsubsong_cb(struct gbs *gbs, void *priv)
{
	long subsong = get_next_subsong(gbs);

	if (gbs->subsong == subsong_stop ||
	    subsong >= gbs->songs) {
		if (loopmode) {
			subsong = subsong_start;
			setup_playmode(gbs);
		} else {
			return false;
		}
	}

	gbs_init(gbs, subsong);
	if (sound_skip)
		sound_skip(subsong);
	return true;
}

void update_displaytime(struct displaytime *time, struct gbs *gbs)
{
	long played = gbs->ticks / GBHW_CLOCK;
	long total = gbs->subsong_info[gbs->subsong].len / 1024;

	time->played_min = played / 60;
	time->played_sec = played % 60;

	if (total == 0) {
		total = subsong_timeout;
	}
	time->total_min = total / 60;
	time->total_sec = total % 60;
}

static char *endian_str(long endian)
{
	switch (endian) {
	case PLUGOUT_ENDIAN_BIG: return "big";
	case PLUGOUT_ENDIAN_LITTLE: return "little";
	case PLUGOUT_ENDIAN_NATIVE: return "native";
	default: return "invalid";
	}
}

static char *filename_only(char *with_pathname)
/* this creates an additional reference the original string, so don't free it */
{
	char *ret = with_pathname;
	char *p   = with_pathname;

	/* we could strlen() and then iterate backwards,
           but we'd have to iterate over the string twice */
	while (*p != 0) {
		if (*p++ == '/')
			ret = p;
	}
	return ret;
}

static void version(void)
{
	printf("%s %s\n", myname, GBS_VERSION);
	exit(0);
}

static void usage(long exitcode)
{
	FILE *out = exitcode ? stderr : stdout;
	fprintf(out,
		_("Usage: %s [option(s)] <gbs-file> [start_at_subsong [stop_at_subsong] ]\n"
		  "\n"
		  "Available options are:\n"
		  "  -E        endian, b == big, l == little, n == native (%s)\n"
		  "  -f        set fadeout (%ld seconds)\n"
		  "  -g        set subsong gap (%ld seconds)\n"
		  "  -h        display this help and exit\n"
		  "  -H        set output high-pass type (%s)\n"
		  "  -l        loop mode\n"
		  "  -o        select output plugin (%s)\n"
		  "            'list' shows available plugins\n"
		  "  -q        reduce verbosity\n"
		  "  -r        set samplerate (%ldHz)\n"
		  "  -R        set refresh delay (%ld milliseconds)\n"
		  "  -t        set subsong timeout (%ld seconds)\n"
		  "  -T        set silence timeout (%ld seconds)\n"
		  "  -v        increase verbosity\n"
		  "  -V        print version and exit\n"
		  "  -z        play subsongs in shuffle mode\n"
		  "  -Z        play subsongs in random mode (repetitions possible)\n"
		  "  -1 to -4  mute a channel on startup\n"),
		myname,
		endian_str(endian),
		fadeout,
		subsong_gap,
		_(filter_type),
		sound_name,
		rate,
		refresh_delay,
		subsong_timeout,
		silence_timeout);
	exit(exitcode);
}

static void parseopts(int *argc, char ***argv)
{
	long res;
	myname = filename_only(*argv[0]);
	while ((res = getopt(*argc, *argv, "1234c:E:f:g:hH:lo:qr:R:t:T:vVzZ")) != -1) {
		switch (res) {
		default:
			usage(1);
			break;
		case '1':
		case '2':
		case '3':
		case '4':
			gbhw_ch[res-'1'].mute ^= 1;
			break;
		case 'c':
			cfg_parse(optarg, options);
			break;
		case 'E':
			if (strcasecmp(optarg, "b") == 0) {
				endian = PLUGOUT_ENDIAN_BIG;
			} else if (strcasecmp(optarg, "l") == 0) {
				endian = PLUGOUT_ENDIAN_LITTLE;
			} else if (strcasecmp(optarg, "n") == 0) {
				endian = PLUGOUT_ENDIAN_NATIVE;
			} else {
				printf(_("\"%s\" is not a valid endian.\n\n"), optarg);
				usage(1);
			}
			break;
		case 'f':
			sscanf(optarg, "%ld", &fadeout);
			break;
		case 'g':
			sscanf(optarg, "%ld", &subsong_gap);
			break;
		case 'h':
			usage(0);
			break;
		case 'H':
			filter_type = optarg;
			break;
		case 'l':
			loopmode = 1;
			break;
		case 'o':
			sound_name = optarg;
			break;
		case 'q':
			verbosity -= 1;
			break;
		case 'r':
			sscanf(optarg, "%ld", &rate);
			break;
		case 'R':
			sscanf(optarg, "%ld", &refresh_delay);
			break;
		case 't':
			sscanf(optarg, "%ld", &subsong_timeout);
			break;
		case 'T':
			sscanf(optarg, "%ld", &silence_timeout);
			break;
		case 'v':
			verbosity += 1;
			break;
		case 'V':
			version();
			break;
		case 'z':
			playmode = PLAYMODE_SHUFFLE;
			break;
		case 'Z':
			playmode = PLAYMODE_RANDOM;
			break;
		}
	}
	*argc -= optind;
	*argv += optind;
}

static void select_plugin(void)
{
	const struct output_plugin *plugout;

	if (strcmp(sound_name, "list") == 0) {
		plugout_list_plugins();
		exit(0);
	}

	plugout = plugout_select_by_name(sound_name);
	if (plugout == NULL) {
		fprintf(stderr, _("\"%s\" is not a known output plugin.\n\n"),
		        sound_name);
		exit(1);
	}

	sound_open = plugout->open;
	sound_skip = plugout->skip;
	sound_io = plugout->io;
	sound_step = plugout->step;
	sound_write = plugout->write;
	sound_close = plugout->close;
	sound_pause = plugout->pause;
	sound_description = plugout->description;

	if (plugout->flags & PLUGOUT_USES_STDOUT) {
		verbosity = 0;
	}
}

struct gbs *common_init(int argc, char **argv)
{
	char *usercfg;
	struct gbs *gbs;

	i18n_init();

	/* initialize RNG */
	random_seed = time(0)+getpid();
	srand(random_seed);

	usercfg = get_userconfig(cfgfile);
	cfg_parse(SYSCONF_PREFIX "/gbsplayrc", options);
	cfg_parse((const char*)usercfg, options);
	free(usercfg);
	parseopts(&argc, &argv);
	select_plugin();

	if (argc < 1) {
		usage(1);
	}

	if (sound_open(endian, rate, &buf.bytes) != 0) {
		fprintf(stderr, _("Could not open output plugin \"%s\"\n"),
		        sound_name);
		exit(1);
	}
	buf.data = malloc(buf.bytes);

	if (sound_io)
		gbhw_setiocallback(iocallback, NULL);
	if (sound_write)
		gbhw_setcallback(callback, NULL);
	gbhw_setrate(rate);
	if (!gbhw_setfilter(filter_type)) {
		fprintf(stderr, _("Invalid filter type \"%s\"\n"), filter_type);
		exit(1);
	}

	/* options have been parsed, argv[0] is our filename */
	filename = filename_only(argv[0]);

	if (argc >= 2) {
		sscanf(argv[1], "%ld", &subsong_start);
		subsong_start--;
	}

	if (argc >= 3) {
		sscanf(argv[2], "%ld", &subsong_stop);
		subsong_stop--;
	}

	gbs = gbs_open(argv[0]);
	if (gbs == NULL) {
		exit(1);
	}

	/* sanitize commandline values */
	if (subsong_start < -1) {
		subsong_start = 0;
	} else if (subsong_start >= gbs->songs) {
		subsong_start = gbs->songs-1;
	}
	if (subsong_stop <  0) {
		subsong_stop = -1;
	} else if (subsong_stop >= gbs->songs) {
		subsong_stop = -1;
	}

	gbs->subsong = subsong_start;
	gbs->subsong_timeout = subsong_timeout;
	gbs->silence_timeout = silence_timeout;
	gbs->gap = subsong_gap;
	gbs->fadeout = fadeout;
	setup_playmode(gbs);
	gbhw_setbuffer(&buf);
	gbs_set_nextsubsong_cb(gbs, nextsubsong_cb, NULL);
	gbs_init(gbs, gbs->subsong);
	if (sound_skip)
		sound_skip(gbs->subsong);
	if (verbosity>0) {
		gbs_printinfo(gbs, 0);
	}

	return gbs;
}

void common_cleanup(struct gbs *gbs)
{
	sound_close();
	free(buf.data);
	gbs_close(gbs);
}
