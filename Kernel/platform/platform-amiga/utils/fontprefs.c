/*
 *	fontprefs - choose the console font
 *
 *	fontprefs		list the fonts and ask which one to use
 *	fontprefs NAME		use font NAME from now on
 *	fontprefs -l		load the chosen font (run at boot by /etc/rc)
 *
 *	The fonts are Fuzix's own ("fuzix"), the font in the machine's ROM
 *	when it has one ("rom": Topaz 8 on an Amiga), and 8x8 font files of
 *	2048 bytes (256 characters, 8 bytes each, top row first) in
 *	/usr/lib/fonts. The choice is kept in /etc/fontprefs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "../amigaioctl.h"

#define FONTDIR		"/usr/lib/fonts"
#define PREFS		"/etc/fontprefs"
#define FONTSIZE	2048
#define MAXFONTS	32

static unsigned char font[FONTSIZE];
/* The console: standard input, or tty1 when run at boot */
static int con;

static char names[MAXFONTS][32];
static char *descs[MAXFONTS];
static int nfonts;

static void add(const char *name, char *desc)
{
	if (nfonts == MAXFONTS)
		return;
	strlcpy(names[nfonts], name, sizeof(names[0]));
	descs[nfonts] = desc;
	nfonts++;
}

static int std_font(int which)
{
	font[0] = which;
	return ioctl(con, VTGETSTDFONT, font);
}

static void find_fonts(void)
{
	DIR *d;
	struct dirent *e;

	add("fuzix", "Fuzix's own font");
	if (std_font(VTSTDFONT_ROM) == 0)
		add("rom", "the font in ROM (Topaz 8 on an Amiga)");
	d = opendir(FONTDIR);
	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		add(e->d_name, "font file");
	}
	closedir(d);
}

/* Fill font[] with the named font */
static int get_font(const char *name)
{
	char path[64];
	int fd, n;

	if (strcmp(name, "fuzix") == 0)
		return std_font(VTSTDFONT_FUZIX);
	if (strcmp(name, "rom") == 0) {
		if (std_font(VTSTDFONT_ROM) == 0)
			return 0;
		fprintf(stderr, "fontprefs: this machine has no font in ROM.\n");
		return -1;
	}
	if (strchr(name, '/'))
		strlcpy(path, name, sizeof(path));
	else
		snprintf(path, sizeof(path), "%s/%s", FONTDIR, name);
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		perror(path);
		return -1;
	}
	n = read(fd, font, FONTSIZE);
	close(fd);
	if (n != FONTSIZE) {
		fprintf(stderr, "fontprefs: %s is not an 8x8 font (%d bytes, not %d).\n",
			path, n, FONTSIZE);
		return -1;
	}
	return 0;
}

static int set_font(const char *name)
{
	if (get_font(name))
		return -1;
	if (ioctl(con, VTSETFONT, font) == -1) {
		perror("fontprefs: setting the font");
		return -1;
	}
	return 0;
}

static void current(char *buf, int len)
{
	FILE *f = fopen(PREFS, "r");

	strlcpy(buf, "fuzix", len);
	if (f == NULL)
		return;
	if (fgets(buf, len, f))
		buf[strcspn(buf, "\n")] = 0;
	fclose(f);
}

static int save(const char *name)
{
	FILE *f = fopen(PREFS, "w");

	if (f == NULL) {
		perror(PREFS);
		return -1;
	}
	fprintf(f, "%s\n", name);
	fclose(f);
	return 0;
}

static int use(const char *name)
{
	if (set_font(name))
		return 1;
	save(name);
	printf("Using %s. A sample: Ab 0O1l {} [] | \xe5\xe4\xf6 \xc5\xc4\xd6 \xe9 \xfc \xa3 \xa9\n",
		name);
	return 0;
}

int main(int argc, char *argv[])
{
	char cur[32];
	char line[16];
	int i, n;

	if (argc == 2 && strcmp(argv[1], "-l") == 0) {
		/* O_NOCTTY: at boot we must not become the console's controlling
		   process, or the login shell later gets none */
		con = open("/dev/tty1", O_RDWR | O_NOCTTY);
		if (con == -1)
			return 1;
		current(cur, sizeof(cur));
		if (strcmp(cur, "fuzix") == 0)
			return 0;
		return set_font(cur) ? 1 : 0;
	}
	if (argc == 2 && argv[1][0] != '-')
		return use(argv[1]);
	if (argc != 1) {
		fprintf(stderr, "usage: fontprefs [-l | NAME]\n");
		return 1;
	}

	find_fonts();
	current(cur, sizeof(cur));
	printf("Console fonts:\n\n");
	for (i = 0; i < nfonts; i++)
		printf("  %d  %-12s %s%s\n", i + 1, names[i], descs[i],
			strcmp(names[i], cur) ? "" : "  (in use)");
	if (!isatty(0))
		return 0;
	printf("\nWhich font (1-%d, Return keeps %s)? ", nfonts, cur);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL)
		return 0;
	n = atoi(line);
	if (n < 1 || n > nfonts)
		return 0;
	return use(names[n - 1]);
}
