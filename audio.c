/*
 * Sound: the ALSA mixer of card 0 through the raw control ioctls (no
 * alsa-lib in the initramfs), the headphone jack, and the output policy.
 *
 * The CS42L58's "Headphone Volume" is the master: it sets the jack and,
 * through the LM48821, the built-in speaker. The speaker amplifier is on
 * while nothing is plugged into the jack ("Speaker when unplugged"), off
 * while earphones are in. The jack is the sound card's input device
 * ("... Headphones", SW_HEADPHONE_INSERT, from the PMU's headphone detect).
 *
 * Included by main.c before ui_m3.c.
 */
#include <sound/asound.h>

#define SND_CTL_DEV	"/dev/snd/controlC0"
#define VOL_CTL		"Headphone Volume"	/* 0..72 = -60..+12 dB */
#define MUTE_CTL	"Headphone Switch"
#define AMP_CTL		"Speaker Amp Switch"
#define AMP_VOL_CTL	"Speaker Amp Volume"	/* 0..31 */
#define VOL_0DB		60			/* control value of 0 dB */
#define AMP_GAIN	22			/* LM48821 gain code for the speaker */

static int snd_fd = -1;
static int jack_fd = -1;
static bool jack_in;		/* earphones plugged */
static bool snd_present;

static bool snd_open(void)
{
	if (snd_fd >= 0)
		return true;
	snd_fd = open(SND_CTL_DEV, O_RDWR | O_CLOEXEC);
	return snd_fd >= 0;
}

/*
 * id of the named control; false if the card has none. Found once: the
 * card's list is read only on a name's first use (a volume drag sets two
 * controls per step).
 */
static bool snd_ctl_id(const char *name, struct snd_ctl_elem_id *out)
{
	static struct { const char *name; struct snd_ctl_elem_id id; } known[8];
	struct snd_ctl_elem_list list;
	struct snd_ctl_elem_id *ids;
	unsigned int i;
	bool ok = false;

	for (i = 0; i < 8 && known[i].name; i++)
		if (!strcmp(known[i].name, name)) {
			*out = known[i].id;
			return true;
		}
	if (!snd_open())
		return false;
	memset(&list, 0, sizeof(list));
	if (ioctl(snd_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) < 0 || !list.count)
		return false;
	ids = calloc(list.count, sizeof(*ids));
	if (!ids)
		return false;
	list.space = list.count;
	list.pids = ids;
	if (ioctl(snd_fd, SNDRV_CTL_IOCTL_ELEM_LIST, &list) == 0) {
		for (i = 0; i < list.used; i++) {
			if (!strcmp((const char *)ids[i].name, name)) {
				*out = ids[i];
				ok = true;
				break;
			}
		}
	}
	free(ids);
	for (i = 0; ok && i < 8; i++)
		if (!known[i].name) {
			known[i].name = name;	/* (string constants) */
			known[i].id = *out;
			break;
		}
	return ok;
}

/* write both channels of an integer or boolean control */
static bool snd_ctl_set(const char *name, long v)
{
	struct snd_ctl_elem_value val;
	struct snd_ctl_elem_info info;

	memset(&val, 0, sizeof(val));
	memset(&info, 0, sizeof(info));
	if (!snd_ctl_id(name, &info.id))
		return false;
	if (ioctl(snd_fd, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0)
		return false;
	val.id = info.id;
	for (unsigned int i = 0; i < info.count && i < 2; i++)
		val.value.integer.value[i] = v;
	return ioctl(snd_fd, SNDRV_CTL_IOCTL_ELEM_WRITE, &val) == 0;
}

static long snd_ctl_get(const char *name, long def)
{
	struct snd_ctl_elem_value val;

	memset(&val, 0, sizeof(val));
	if (!snd_ctl_id(name, &val.id))
		return def;
	if (ioctl(snd_fd, SNDRV_CTL_IOCTL_ELEM_READ, &val) < 0)
		return def;
	return val.value.integer.value[0];
}

/* 0..100 % -> mute at 0, else -60..0 dB in 1 dB steps */
static void snd_apply_volume(int pct)
{
	if (!snd_present)
		return;
	if (pct <= 0) {
		snd_ctl_set(MUTE_CTL, 0);
		return;
	}
	snd_ctl_set(VOL_CTL, (pct * VOL_0DB + 50) / 100);
	snd_ctl_set(MUTE_CTL, 1);
}

static void snd_apply_output(bool speaker_when_unplugged)
{
	if (!snd_present)
		return;
	snd_ctl_set(AMP_CTL, speaker_when_unplugged && !jack_in);
}

static bool jack_read_state(void)
{
	unsigned long sw[1] = { 0 };

	if (jack_fd >= 0 && ioctl(jack_fd, EVIOCGSW(sizeof(sw)), sw) >= 0)
		jack_in = !!(sw[0] & (1UL << SW_HEADPHONE_INSERT));
	return jack_in;
}

/* true when the plug state changed */
static bool jack_poll(void)
{
	struct input_event ev;
	bool was = jack_in;

	if (jack_fd < 0)
		return false;
	while (read(jack_fd, &ev, sizeof(ev)) == sizeof(ev))
		if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT)
			jack_in = ev.value;
	return jack_in != was;
}

static const char *find_input(const char *name);

static void snd_init(int vol_pct, bool speaker_when_unplugged)
{
	const char *jack = find_input("Headphones");

	snd_present = snd_ctl_id(VOL_CTL, &(struct snd_ctl_elem_id){ 0 });
	if (jack)
		jack_fd = open(jack, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	jack_read_state();
	if (!snd_present)
		return;
	snd_ctl_set(AMP_VOL_CTL, AMP_GAIN);
	snd_apply_volume(vol_pct);
	snd_apply_output(speaker_when_unplugged);
}
