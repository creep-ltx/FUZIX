/*
 *	screenmode - choose the Amiga's screen mode
 *
 *	screenmode		list the modes and ask which one to use
 *	screenmode NAME		use mode NAME from now on
 *	screenmode -l		set the chosen mode (run at boot by /etc/rc)
 *
 *	The modes are pal (640x256, 80x32), ntsc (640x200, 80x25) and vga
 *	(640x480, 80x60, 31kHz: needs a VGA or multisync monitor, and an
 *	AGA Amiga). The choice is kept in /etc/screenmode. A mode
 *	that needs more memory than the screens have now starts with the
 *	next boot. Holding the left mouse button while Fuzix starts keeps
 *	the Amiga's own mode, for a monitor that shows nothing in the
 *	chosen one.
 *
 *	GNU GPL version 2, like FUZIX.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "../amigaioctl.h"

#define PREFS		"/etc/screenmode"
#define NMODES		3

static const char *names[NMODES] = { "pal", "ntsc", "vga" };
static const char *descs[NMODES] = {
	"640x256, 80x32 characters, 50Hz (any monitor)",
	"640x200, 80x25 characters, 60Hz (any monitor)",
	"640x480, 80x60 characters, 31kHz (VGA or multisync monitor)"
};

/* The console: standard input, or tty1 when run at boot */
static int con;

static int find(const char *name)
{
	int i;

	if (strcmp(name, "640x480") == 0 || strcmp(name, "productivity") == 0)
		return VTMODE_VGA;
	for (i = 0; i < NMODES; i++)
		if (strcmp(name, names[i]) == 0)
			return i;
	return -1;
}

/* The saved mode, or -1 */
static int saved(void)
{
	FILE *f = fopen(PREFS, "r");
	char line[32];

	if (f == NULL)
		return -1;
	if (fgets(line, sizeof(line), f) == NULL)
		line[0] = 0;
	fclose(f);
	line[strcspn(line, "\n")] = 0;
	return find(line);
}

static int set(int m)
{
	unsigned char b = m;

	return ioctl(con, VTSETMODE, &b);
}

static int use(const char *name, int info)
{
	int m = find(name);
	int own = (info >> 4) & 15;
	FILE *f;

	if (m < 0) {
		fprintf(stderr, "screenmode: no mode called %s\n", name);
		return 1;
	}
	if (!(info & (0x100 << m))) {
		fprintf(stderr, "screenmode: this Amiga cannot show %s%s\n", name,
			m == VTMODE_VGA ? " (it needs an A1200 or A4000)" : "");
		return 1;
	}
	f = fopen(PREFS, "w");
	if (f == NULL || fprintf(f, "%s\n", names[m]) < 0 || fclose(f)) {
		perror(PREFS);
		return 1;
	}
	if (m == VTMODE_VGA)
		printf("The vga mode needs a VGA or multisync monitor. If the "
		       "screen stays\nblack, hold the left mouse button while "
		       "Fuzix starts: it then starts\nin %s, and screenmode %s "
		       "brings that back for good.\n", names[own], names[own]);
	if (!(info & (0x1000 << m))) {
		printf("%s is used from the next start.\n", names[m]);
		return 0;
	}
	if ((info & 15) == m)
		return 0;
	fflush(stdout);
	if (set(m) == -1) {
		perror("screenmode");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	char line[16];
	int info, cur, own, m, i;

	if (argc == 2 && strcmp(argv[1], "-l") == 0) {
		/* O_NOCTTY: at boot we must not become the console's controlling
		   process, or the login shell later gets none */
		con = open("/dev/tty1", O_RDWR | O_NOCTTY);
		if (con == -1)
			return 1;
		info = ioctl(con, VTGETMODE, 0);
		if (info == -1)
			return 1;
		own = (info >> 4) & 15;
		m = saved();
		if (m < 0)
			m = own;
		/* Always called: it also gives back the memory a larger
		   mode would have needed */
		if (set(m | VTMODE_BOOT) == -1) {
			if (errno == EBUSY)
				printf("Mouse button held: the screen stays %s.\n",
				       names[own]);
			else
				printf("screenmode: this Amiga cannot show %s.\n",
				       names[m]);
			return 1;
		}
		return 0;
	}

	/* Whichever of stdin, stdout, stderr is the screen */
	for (con = 0; con < 3; con++)
		if ((info = ioctl(con, VTGETMODE, 0)) != -1)
			break;
	if (info == -1) {
		fprintf(stderr, "screenmode: run it on the Amiga's screen\n");
		return 1;
	}
	if (argc == 2 && argv[1][0] != '-')
		return use(argv[1], info);
	if (argc != 1) {
		fprintf(stderr, "usage: screenmode [-l | NAME]\n");
		return 1;
	}

	cur = info & 15;
	m = saved();
	printf("Screen modes:\n\n");
	for (i = 0; i < NMODES; i++) {
		if (!(info & (0x100 << i)))
			continue;
		printf("  %d  %-5s %s%s%s\n", i + 1, names[i], descs[i],
		       i == cur ? "  (in use)" : "",
		       i == m && i != cur ? "  (from the next start)" : "");
	}
	if (!isatty(0))
		return 0;
	printf("\nWhich mode (Return keeps %s)? ", names[cur]);
	fflush(stdout);
	if (fgets(line, sizeof(line), stdin) == NULL)
		return 0;
	i = atoi(line);
	if (i < 1 || i > NMODES || !(info & (0x100 << (i - 1))))
		return 0;
	return use(names[i - 1], info);
}
