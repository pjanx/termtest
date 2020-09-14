//
// termtest: terminal evaluation tool
//
// Copyright (c) 2020, Přemysl Eric Janouch <p@janouch.name>
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
// OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
// CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
//

// NOTE: We don't need to and will not free any memory. This is intentional.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <glob.h>

#include <curses.h>
#include <term.h>

#define CSI "\x1b["
#define OSC "\x1b]"
#define DCS "\x1bP"
#define ST  "\x1b\\"
#define ST8 "\x9c"
#define BEL "\x07"

#define SGR0 CSI "m"

extern char **environ;
static struct termios saved_termios;
struct winsize ws;

// tty_atexit restores the terminal into its original mode. Some of the tested
// extensions can't be reset by terminfo strings, so don't bother with that.
static void tty_atexit() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios); }

// tty_cbreak puts the terminal in the cbreak mode.
static bool tty_cbreak() {
	if (tcgetattr(STDIN_FILENO, &saved_termios) < 0)
		return false;

	struct termios buf = saved_termios;
	buf.c_lflag &= ~(ECHO | ICANON);
	buf.c_cc[VMIN] = 1;
	buf.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &buf) < 0)
		return false;

	if (tcgetattr(STDIN_FILENO, &buf) < 0 || (buf.c_lflag & (ECHO | ICANON)) ||
			buf.c_cc[VMIN] != 1 || buf.c_cc[VTIME] != 0) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
		return false;
	}
	return true;
}

// comm writes a string to the terminal and waits for a result. Returns NULL
// if it didn't manage to get a response, or when an error has happened.
static char *comm(const char *req, bool wait_first) {
	ssize_t len = write(STDOUT_FILENO, req, strlen(req));
	if (len < strlen(req)) return NULL;

	struct pollfd pfd = { .fd = STDIN_FILENO, .events = POLLIN };
	if (wait_first) poll(&pfd, 1, -1);

	int lag = getenv("SSH_CONNECTION") ? 250 : 50;
	char buf[1000] = ""; size_t buf_len = 0; int n = 0;
	while ((n = poll(&pfd, 1, lag /* unreliable, timing-dependent */))) {
		if (n < 0) return NULL;
		len = read(STDIN_FILENO, buf + buf_len, sizeof buf - buf_len - 1);
		if (len <= 0) return NULL;
		buf_len += len;
	}
	return strdup(buf);
}

enum { DEC_UNKNOWN, DEC_SET, DEC_RESET, DEC_PERMSET, DEC_PERMRESET };

// decrpmstr returns a textual description of a DECRPM response.
static const char *decrpmstr(int status) {
	if (status == DEC_UNKNOWN)   return "unknown";
	if (status == DEC_SET)       return "set";
	if (status == DEC_RESET)     return "reset";
	if (status == DEC_PERMSET)   return "permanently set";
	if (status == DEC_PERMRESET) return "permanently reset";
	return "?";
}

// parse_decrpm returns whether the mode response is valid (result >= 0),
// as well as the terminal's response if it is, see the DEC_* constants.
static int parse_decrpm(const char *resp) {
	// E.g., \x1b[?1000;2$y
	if (resp[0] != '\x1b' || resp[1] != '[' || resp[2] != '?') return -1;
	char *end = NULL; errno = 0; long mode = strtol(resp + 3, &end, 10);
	if (errno || mode < 0 || *end != ';') return -1;
	if (!isdigit(end[1]) || end[2] != '$' || end[3] != 'y' || end[4]) return -1;
	return end[1] - '0';
}

// deccheck checks whether a particular DEC mode is supported, whether it is
// enabled, and returns that information as a string.
static const char *deccheck(int number) {
	char buf[1000] = ""; snprintf(buf, sizeof buf, CSI "?%d$p", number);
	return decrpmstr(parse_decrpm(comm(buf, false)));
}

// test_mouse tests whether a particular mouse mode is supported.
static void test_mouse(int mode) {
	ioctl(STDIN_FILENO, TIOCGWINSZ, &ws);
	comm(CSI "?1002l" CSI "?1003l" CSI "?1005l"
		CSI "?1006l" CSI "?1015l" CSI "?1016l" CSI "?1000h", false);

	char buf[100] = "";
	snprintf(buf, sizeof buf, CSI "?%dh" "%d: ", mode, mode);
	char *resp = comm(buf, true);

	unsigned int b = -1, x = -1, y = -1;
	unsigned char bc = -1, xc = -1, yc = -1, m = -1;
	if (sscanf(resp, CSI "M%c%c%c", &bc, &xc, &yc) == 3
		&& bc >= 32 && xc >= 32 && yc >= 32) {
		// Beware that this isn't compatible with xterm run with the -lc switch.
		if (strlen(resp) > 6) {
			printf("1005? ");
			for (const char *p = resp; *p; p++)
				printf("%c", *p < 32 || *p > 127 ? '.' : *p);
			printf("\n");
		} else {
			printf("1000/1005 (%d @ %d,%d)\n", bc - 32, xc - 32, yc - 32);
		}
	} else if (sscanf(resp, CSI "<%u;%u;%u%c", &b, &x, &y, &m) == 4
		&& (m == 'm' || m == 'M')) {
		printf("%s (%u%c @ %u,%u)\n",
			(x > ws.ws_col || y > ws.ws_row) ? "1016" : "1006/1016",
			b, m, x, y);
	} else if (sscanf(resp, CSI "%u;%u;%u%c", &b, &x, &y, &m) == 4
		&& m == 'M') {
		printf("1015 (%u @ %u,%u)\n", b - 32, x, y);
	} else {
		printf("Failed to parse.\n");
	}

	comm("Waiting for button up events, press a key if hanging.\n", true);
}

// colour prints a cell with the given indexed colour as a background.
static void colour(int n) {
	n > 7 ? printf(CSI "48;5;%dm ", n) : printf(CSI "%dm ", 40 + n);
}

// direct prints a cell with the given direct colour as a background.
// sep can be set either to ':' for ISO-8613-6 or ';' for more compatibility.
static void direct(char sep, int r, int g, int b) {
	printf(CSI "48%c2%c%d%c%d%c%dm ", sep, sep, r, sep, g, sep, b);
}

int main(int argc, char *argv[]) {
	if (!tty_cbreak())
		abort();

	// Identify the terminal emulator, which is passed by arguments.
	for (int i = 1; i < argc; i++)
		printf("%s ", argv[i]);
	printf("\n");

	// Initialise terminfo, this should definitely succeed.
	int err; char *term = getenv("TERM");
	if (setupterm((char *)term, 1, &err) != OK)
		abort();

	// VTE wouldn't have sent a response to DECRQM otherwise!
	comm("-- Press any key to start\n", true);

	printf("-- Identification\nTERM=%s\n", term);
	char *upperterm = strdup(term);
	for (char *p = upperterm; *p; p++)
		*p = toupper(*p);
	printf("Version env var candidates: ");
	for (char **p = environ; *p; p++)
		if (strstr(*p, "VERSION") || strstr(*p, upperterm))
			printf("%s ", *p);
	printf("\n");

	printf("-- DECRQM: ");
	char *rpm = comm(CSI "?1000$p", false);
	bool decrqm_supported = rpm && parse_decrpm(rpm) >= 0;
	printf("%d\n", decrqm_supported);

	printf("-- Colours\n");
	start_color();  // Does this need initscr()?  ncurses doesn't initialise.
	printf("Terminfo: %d colours, has_colors=%d\n",
		tigetnum("colors"), has_colors());

	char *colorterm = getenv("COLORTERM");
	if (colorterm) {
		printf("COLORTERM=%s", colorterm);
		if (!strcmp(colorterm, "truecolor") || !strcmp(colorterm, "24bit"))
			printf(" - Claims to support 24-bit colours");
		printf("\n");
	}

	// For a comprehensive list of unusual terminfo entries, see tmux(1),
	// user_caps(5), and comments in misc/terminfo.src. Looking for them
	// here out of curiosity, all sequences are mostly standardised.
	const char *Tc = tigetstr("Tc");
	if (Tc && Tc != (char *)-1)
		printf("Terminfo: tmux extension claims direct color.\n");

	for (int n =   0; n <   8; n++) colour(n); printf(SGR0 "\n");
	for (int n =   8; n <  16; n++) colour(n); printf(SGR0 "\n");
	for (int n = 232; n < 256; n++) colour(n); printf(SGR0 "\n");

	// Ideally, both ramps should be visible, and smooth.
	for (int g = 255; g >= 192; g--) direct(';', 255, g, 0); printf(SGR0 "\n");
	for (int g = 255; g >= 192; g--) direct(':', 255, g, 0); printf(SGR0 "\n");

	printf("-- Colour change\n");
	printf("Terminfo: can_change %d, initialize_color %d\n",
		!!can_change, !!initialize_color);

	// The response from urxvt is wrongly missing the colour number.
	char *bright_red_save = comm(OSC "4;9;?" BEL, false);
	if (!strncmp(bright_red_save, OSC "4;", 4)) {
		char *copy = strdup(bright_red_save + 4);
		*strpbrk(copy, BEL ST8 "\x1b") = 0;
		printf("We have read colour contents from the terminal: %s\n", copy);
	} else *bright_red_save = 0;

	printf(CSI "0;38;5;9m" "Indexed" SGR0 " " CSI "1;31m" "Bold" SGR0 "\n");
	printf("Press a key to stop.\n");
	for (int r = 0; r < 255; r += 8) {
		char buf[1000] = "";
		snprintf(buf, sizeof buf, OSC "4;9;rgb:%02x/%02x/%02x" BEL, r, 0, 0);
		if (*comm(buf, false)) break;
		poll(NULL, 0, 50 /* delay */);
	}
	if (*bright_red_save)
		comm(bright_red_save, false);
	else
		comm(OSC "104;9" BEL, false);

	// Linux palette sequence, supported by e.g. pterm.
	// We must take care to suffix it with an OSC terminator at least.
	for (int r = 0; r < 255; r += 8) {
		char buf[1000] = "";
		snprintf(buf, sizeof buf, OSC "P9%02x%02x%02x", r, 0, 0);
		if (*comm(buf, false)) break;
		poll(NULL, 0, 50 /* delay */);
	}
	printf("\a\r");

	printf("-- Bold and blink attributes\n");
	bool bbc_supported = enter_bold_mode && enter_blink_mode
		&& set_a_foreground && set_a_background && exit_attribute_mode;
	printf("Terminfo: %d\n", bbc_supported);
	if (bbc_supported) {
		tputs(tiparm(set_a_foreground, COLOR_GREEN), 1, putchar);
		tputs(tiparm(set_a_background, COLOR_BLUE), 1, putchar);
		printf("Terminfo%s ", exit_attribute_mode);
		tputs(enter_bold_mode, 1, putchar);
		tputs(tiparm(set_a_foreground, COLOR_GREEN), 1, putchar);
		tputs(tiparm(set_a_background, COLOR_BLUE), 1, putchar);
		printf("Bold%s ", exit_attribute_mode);
		tputs(enter_blink_mode, 1, putchar);
		tputs(tiparm(set_a_foreground, COLOR_GREEN), 1, putchar);
		tputs(tiparm(set_a_background, COLOR_BLUE), 1, putchar);
		printf("Blink%s ", exit_attribute_mode);
		printf("\n");
	}

	printf(CSI "0;32;44m" "SGR" SGR0 " ");
	printf(CSI "1;32;44m" "Bold" SGR0 " ");
	printf(CSI "5;32;44m" "Blink" SGR0 " ");
	printf("\n");
	printf(CSI "0;5m" "Blink with default colours." SGR0);
	printf("\n");

	printf("-- Italic attribute\n");
	bool italic_supported = enter_italics_mode && exit_italics_mode;
	printf("Terminfo: %d\n", italic_supported);
	if (italic_supported)
		printf("%sTerminfo test.%s\n", enter_italics_mode, exit_italics_mode);
	printf(CSI "3m" "SGR test.\n" SGR0);

	printf("-- Overline attribute\n");
	const char *Smol = tigetstr("Smol");
	if (Smol && Smol != (char*)-1)
		printf("Terminfo: found tmux extension.\n");
	printf(CSI "53m" "SGR test.\n" SGR0);

	printf("-- Underline colour\n");
	const char *Setulc = tigetstr("Setulc");
	if (Setulc && Setulc != (char*)-1)
		printf("Terminfo: found tmux extension.\n");
	printf(CSI "4;58;2;0;255;0m" "SGR test." SGR0 "\n");
	printf(CSI "4;58:5:46m" "SGR test." SGR0 "\n");

	printf("-- Bar cursor\n");
	const char *Ss = tigetstr("Ss");
	const char *Se = tigetstr("Se");
	if (Ss && Ss != (char*)-1)
		printf("Terminfo: found tmux extension for setting.\n");
	if (Se && Se != (char*)-1)
		printf("Terminfo: found tmux extension for resetting.\n");

	comm(CSI "5 q" "Blinking (press a key): ", true);
	printf("\n");
	comm(CSI "6 q" "Steady (press a key): ", true);
	printf("\n");

	// There's no actual way of restoring this to what it was before.
	// Terminfo "cnorm" at most undoes blinking in xterm.
	comm(CSI "2 q", false);

	printf("-- w3mimgdisplay\n");
	const char *windowid = getenv("WINDOWID");
	if (windowid) {
		printf("WINDOWID=%s\n", windowid);
		printf("There should be a picture. Press a key.\n");
		poll(NULL, 0, 50 /* wait for a refresh */);

		char buf[1000] = "";
		snprintf(buf, sizeof buf, "/usr/lib/w3m:%s", getenv("PATH"));
		setenv("PATH", buf, true /* replace */);

		glob_t gb;
		glob("/usr/share/pixmaps/*.xpm", 0, NULL, &gb);
		glob("/usr/share/pixmaps/*.png", GLOB_APPEND, NULL, &gb);

		FILE *fp = popen("w3mimgdisplay >/dev/null", "w");
		fprintf(fp, "0;1;0;0;%d;%d;;;;;%s\n4;\n3;", 100, 100, gb.gl_pathv[0]);
		pclose(fp);
		globfree(&gb);

		comm("", true);
	}

	printf("-- Sixel graphics\n");
	comm(CSI "4c" DCS "0;0;0;q??~~??~~??iTiTiT" ST, false);

	printf("-- Mouse protocol\n");
	// TODO: Inspect terminfo kmous, XM, xm.
	//  - We can say what protocol kmous expects, whether 1000 or 1006.
	//     - Sadly urxvt still has the 1000/1005 sequence there.
	while (!ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) && ws.ws_col < 223) {
		if (!*comm("Your terminal needs to be at least 223 columns wide.\n"
			"Press a key once you've made it wide enough.\n", true))
			break;
	}

	comm(CSI "?1000h" CSI "?1004h", false);
	printf("Focus in and out of the window, press a key to abort.\n");
	while (true) {
		char *in = comm("", true);
		if (*in != '\x1b') break;
		else if (in[1] == '[' && in[2] == 'I') printf("Focused in.\n");
		else if (in[1] == '[' && in[2] == 'O') printf("Focused out.\n");
	}
	comm(CSI "?1000l" CSI "?1004l", false);

	printf("Click the rightmost column, if it's possible.\n");
	int mouses[] = { 1005, 1006, 1015, 1016 };
	for (size_t i = 0; i < sizeof mouses / sizeof *mouses; i++) {
		if (decrqm_supported)
			printf("DECRQM(%d): %s\n", mouses[i], deccheck(mouses[i]));
		test_mouse(mouses[i]);
	}
	comm(CSI "?1000l", false);

	printf("-- Selection\n");
	const char *Ms = tigetstr("Ms");
	if (Ms && Ms != (char *)-1)
		printf("Terminfo: found tmux extension for selections.\n");

	char *selection = comm(OSC "52;pc;?" BEL, false);
	if (!strncmp(selection, OSC "52;", 5)) {
		printf("We have received the selection from the terminal!" CSI "1m\n");
		char *semi = strrchr(selection, ';');
		*strpbrk(semi, BEL ST8 "\x1b") = 0;
		FILE *fp = popen("base64 -d", "w");
		fprintf(fp, "%s", semi + 1);
		pclose(fp);
		printf(CSI "m\n");
	}

	comm(OSC "52;pc;VGVzdA==" BEL /* ST didn't work, UTF-8 issues? */, false);
	comm("Check if the selection now contains 'Test' and press a key.\n", true);

	printf("-- Bracketed paste\n");
	if (decrqm_supported)
		printf("DECRQM: %s\n", deccheck(2004));

	// We might consider xdotool... though it can't operate the clipboard,
	// so we'd have to use Xlib, and that is too much effort.
	char *pasted = comm(CSI "?2004h" "Paste something: ", true);
	printf("%d\n", !strncmp(pasted, CSI "200~", 6));
	comm(CSI "?2004l", false);

	// Let the user see the results when run outside an interactive shell.
	comm("-- Finished\n", true);

	// atexit is broken in tcc -run, see https://savannah.nongnu.org/bugs/?56495
	tty_atexit();
	return 0;
}
