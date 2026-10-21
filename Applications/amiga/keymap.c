/*
 *	keymap - choose the keyboard layout
 *
 *	keymap		list the keymaps and ask which one to use
 *	keymap NAME	use keymap NAME from now on (usa = the built-in one)
 *	keymap -l	load the chosen keymap (run at boot by /etc/rc)
 *
 *	The keymaps are AmigaOS keymaps (DEVS:Keymaps) converted by
 *	mkkeymap.py into /usr/lib/keymaps. The choice is kept in /etc/keymap.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>

#define KEYDIR		"/usr/lib/keymaps"
#define PREFS		"/etc/keymap"
#define AKM_SIZE	4096
#define MAXMAPS		40

static unsigned char map[AKM_SIZE];
static int con;

/* AmigaOS keymap names are short: say what they are */
static const char *names[][2] = {
	{ "usa", "USA (built in)" },
	{ "usa2", "USA Dvorak" },
	{ "gb", "British" },
	{ "d", "German" },
	{ "f", "French" },
	{ "i", "Italian" },
	{ "e", "Spanish" },
	{ "s", "Swedish and Finnish" },
	{ "dk", "Danish" },
	{ "n", "Norwegian" },
	{ "cdn", "Canadian French" },
	{ "ch1", "Swiss (German)" },
	{ "ch2", "Swiss (French)" },
	{ "po", "Portuguese" },
	{ "la", "Latin American" },
	{ "polska", "Polish" },
	{ "tr", "Turkish" },
	{ "greek", "Greek (needs a Greek font)" },
	{ "rusd", "Russian, German keyboard (needs a Cyrillic font)" },
	{ "rusgb", "Russian, British keyboard (needs a Cyrillic font)" },
	{ "rusus", "Russian, US keyboard (needs a Cyrillic font)" },
	{ "rusuae", "Russian, rusuae variant (needs a Cyrillic font)" },
	{ "ruswin", "Russian, Windows layout (needs a Cyrillic font)" },
	{ NULL, NULL }
};

static const char *describe(const char *n)
{
	int i;

	for (i = 0; names[i][0]; i++)
		if (strcmp(names[i][0], n) == 0)
			return names[i][1];
	return "";
}

static char list[MAXMAPS][32];
static int nmaps;

static void find_maps(void)
{
	DIR *d;
	struct dirent *e;
	int i, j;

	strcpy(list[nmaps++], "usa");
	d = opendir(KEYDIR);
	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL && nmaps < MAXMAPS) {
		if (e->d_name[0] == '.')
			continue;
		strlcpy(list[nmaps++], e->d_name, sizeof(list[0]));
	}
	closedir(d);
	/* alphabetical after usa */
	for (i = 1; i < nmaps; i++)
		for (j = i + 1; j < nmaps; j++)
			if (strcmp(list[i], list[j]) > 0) {
				char t[32];
				strcpy(t, list[i]);
				strcpy(list[i], list[j]);
				strcpy(list[j], t);
			}
}

static int set_map(const char *name)
{
	char path[64];
	int fd;

	memset(map, 0, sizeof(map));
	if (strcmp(name, "usa") == 0)
		memcpy(map, "AKM0", 4);
	else {
		if (strchr(name, '/'))
			strlcpy(path, name, sizeof(path));
		else
			snprintf(path, sizeof(path), "%s/%s", KEYDIR, name);
		fd = open(path, O_RDONLY);
		if (fd == -1) {
			perror(path);
			return -1;
		}
		if (read(fd, map, sizeof(map)) < 4 || memcmp(map, "AKM1", 4)) {
			fprintf(stderr, "keymap: %s is not a keymap.\n", path);
			close(fd);
			return -1;
		}
		close(fd);
	}
	if (ioctl(con, KBSETTRANS, map) == -1) {
		perror("keymap: setting the keymap");
		return -1;
	}
	return 0;
}

static void current(char *buf, int len)
{
	FILE *f = fopen(PREFS, "r");

	strlcpy(buf, "usa", len);
	if (f == NULL)
		return;
	if (fgets(buf, len, f))
		buf[strcspn(buf, "\n")] = 0;
	fclose(f);
}

static int use(const char *name)
{
	FILE *f;

	if (set_map(name))
		return 1;
	f = fopen(PREFS, "w");
	if (f) {
		fprintf(f, "%s\n", name);
		fclose(f);
	}
	printf("Using the %s keymap", name);
	if (*describe(name))
		printf(" (%s)", describe(name));
	printf(".\n");
	return 0;
}

int main(int argc, char *argv[])
{
	char cur[32], line[16];
	int i, n;

	if (argc == 2 && strcmp(argv[1], "-l") == 0) {
		/* At boot: not as the console's controlling process */
		con = open("/dev/tty1", O_RDWR | O_NOCTTY);
		if (con == -1)
			return 1;
		current(cur, sizeof(cur));
		if (strcmp(cur, "usa") == 0)
			return 0;
		return set_map(cur) ? 1 : 0;
	}
	if (argc == 2 && argv[1][0] != '-')
		return use(argv[1]);
	if (argc != 1) {
		fprintf(stderr, "usage: keymap [-l | NAME]\n");
		return 1;
	}

	find_maps();
	current(cur, sizeof(cur));
	printf("Keymaps:\n\n");
	for (i = 0; i < nmaps; i++)
		printf("  %2d  %-8s %s%s\n", i + 1, list[i], describe(list[i]),
		       strcmp(list[i], cur) ? "" : "  (in use)");
	if (!isatty(0))
		return 0;
	printf("\nWhich keymap (1-%d, Return keeps %s)? ", nmaps, cur);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL)
		return 0;
	n = atoi(line);
	if (n < 1 || n > nmaps)
		return 0;
	return use(list[n - 1]);
}
