/* watch -- execute a program repeatedly, displaying output fullscreen
 *
 * Based on the original 1991 'watch' by Tony Rems <rembo@unisoft.com>
 * (with mods and corrections by Francois Pinard).
 *
 * Substantially reworked, new features (differences option, SIGWINCH
 * handling, unlimited command length, long line handling) added Apr 1999 by
 * Mike Coleman <mkc@acm.org>.
 *
 * Changes by Albert Cahalan, 2002-2003.
 */

#define VERSION "0.2.0"

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <locale.h>
#include "proc/procps.h"

#ifdef FORCE_8BIT
#undef isprint
#define isprint(x) ( (x>=' '&&x<='~') || (x>=0xa0) )
#endif

static struct option longopts[] = {
	{"differences", optional_argument, 0, 'd'},
	{"help", no_argument, 0, 'h'},
	{"interval", required_argument, 0, 'n'},
	{"no-title", no_argument, 0, 't'},
	{"version", no_argument, 0, 'v'},
	{"paging", no_argument, 0, 'p'},
	{0, 0, 0, 0}
};

static char usage[] =
    "Usage: %s [-dhntpv] [--differences[=cumulative]] [--help] [--interval=<n>] [--no-title] [--paging] [--version] <command>\n";

static char *progname;

static int curses_started = 0;
static int height = 24, width = 80;
static int old_height, old_width;
static int screen_size_changed = 0;
static int show_title = 2;  // number of lines used, 2 or 0

#define min(x,y) ((x) > (y) ? (y) : (x))

static void do_usage(void) NORETURN;
static void do_usage(void)
{
	fprintf(stderr, usage, progname);
	exit(1);
}

static void do_exit(int status) NORETURN;
static void do_exit(int status)
{
	if (curses_started)
		endwin();
	exit(status);
}

/* signal handler */
static void die(int notused) NORETURN;
static void die(int notused)
{
	(void) notused;
	do_exit(0);
}

static void
winch_handler(int notused)
{
	(void) notused;
	screen_size_changed = 1;
}

static char env_col_buf[24];
static char env_row_buf[24];
static int incoming_cols;
static int incoming_rows;

static void
get_terminal_size(void)
{
	old_height = height;
	old_width = width;
	struct winsize w;
	if(!incoming_cols){  // have we checked COLUMNS?
		const char *s = getenv("COLUMNS");
		incoming_cols = -1;
		if(s && *s){
			long t;
			char *endptr;
			t = strtol(s, &endptr, 0);
			if(!*endptr && (t>0) && (t<(long)666)) incoming_cols = (int)t;
			width = incoming_cols;
			snprintf(env_col_buf, sizeof env_col_buf, "COLUMNS=%d", width);
			putenv(env_col_buf);
		}
	}
	if(!incoming_rows){  // have we checked LINES?
		const char *s = getenv("LINES");
		incoming_rows = -1;
		if(s && *s){
			long t;
			char *endptr;
			t = strtol(s, &endptr, 0);
			if(!*endptr && (t>0) && (t<(long)666)) incoming_rows = (int)t;
			height = incoming_rows;
			snprintf(env_row_buf, sizeof env_row_buf, "LINES=%d", height);
			putenv(env_row_buf);
		}
	}
	if (incoming_cols<0 || incoming_rows<0){
		if (ioctl(2, TIOCGWINSZ, &w) == 0) {
			if (incoming_rows<0 && w.ws_row > 0){
				height = w.ws_row;
				snprintf(env_row_buf, sizeof env_row_buf, "LINES=%d", height);
				putenv(env_row_buf);
			}
			if (incoming_cols<0 && w.ws_col > 0){
				width = w.ws_col;
				snprintf(env_col_buf, sizeof env_col_buf, "COLUMNS=%d", width);
				putenv(env_col_buf);
			}
		}
	}
}

int
main(int argc, char *argv[])
{
	int optc;
	int option_differences = 0,
	    option_differences_cumulative = 0,
	    option_help = 0, option_version = 0,
	    option_paging = 0;
	double interval = 2;
	char *command;
	int command_length = 0;	/* not including final \0 */
	FILE *command_pipe = NULL;
	chtype *command_output = NULL;
	int command_output_length = 0;
	chtype *previous_command_output = NULL;
	int previous_command_output_length = 0;

	setlocale(LC_ALL, "");
	progname = argv[0];

	while ((optc = getopt_long(argc, argv, "+d::hn:pvt", longopts, (int *) 0))
	       != EOF) {
		switch (optc) {
		case 'd':
			option_differences = 1;
			if (optarg)
				option_differences_cumulative = 1;
			break;
		case 'h':
			option_help = 1;
			break;
		case 't':
			show_title = 0;
			break;
		case 'n':
			{
				char *str;
				interval = strtod(optarg, &str);
				if (!*optarg || *str)
					do_usage();
				if(interval < 0.1)
					interval = 0.1;
				if(interval > ~0u/1000000)
					interval = ~0u/1000000;
			}
			break;
		case 'v':
			option_version = 1;
			break;
		case 'p':
			option_paging = 1;
			break;
		default:
			do_usage();
			break;
		}
	}

	if (option_version) {
		fprintf(stderr, "%s\n", VERSION);
		if (!option_help)
			exit(0);
	}

	if (option_help) {
		fprintf(stderr, usage, progname);
		fputs("  -d, --differences[=cumulative]\thighlight changes between updates\n", stderr);
		fputs("\t\t(cumulative means highlighting is cumulative)\n", stderr);
		fputs("  -h, --help\t\t\t\tprint a summary of the options\n", stderr);
		fputs("  -n, --interval=<seconds>\t\tseconds to wait between updates\n", stderr);
		fputs("  -v, --version\t\t\t\tprint the version number\n", stderr);
		fputs("  -t, --no-title\t\t\tturns off showing the header\n", stderr);
		exit(0);
	}

	if (optind >= argc)
		do_usage();

	command = strdup(argv[optind++]);
	command_length = strlen(command);
	for (; optind < argc; optind++) {
		char *endp;
		int s = strlen(argv[optind]);
		command = realloc(command, command_length + s + 2);	/* space and \0 */
		endp = command + command_length;
		*endp = ' ';
		memcpy(endp + 1, argv[optind], s);
		command_length += 1 + s;	/* space then string length */
		command[command_length] = '\0';
	}

	get_terminal_size();

	/* Catch keyboard interrupts so we can put tty back in a sane state.  */
	signal(SIGINT, die);
	signal(SIGTERM, die);
	signal(SIGHUP, die);
	signal(SIGWINCH, winch_handler);

	/* Set up tty for curses use.  */
	curses_started = 1;
	initscr();
	nonl();
	keypad(stdscr, TRUE);
	noecho();
	cbreak();
	halfdelay(1);
	curs_set(0);

	struct timeval tv;
	if (gettimeofday(&tv, NULL) == -1) {
		puts("gettimeofday() returned -1");
		exit(1);
	}
	unsigned long time_then = (tv.tv_sec * 1000000ul) + tv.tv_usec;
	int rerun_command = 1;
	int max_y = -1;
	int origin_x = 0, origin_y = 0, view_changed = 0;
	int go_to_end = 0;

	for (;;) {
		time_t t = time(NULL);
		char *ts = ctime(&t);
		int tsl = strlen(ts);
		char *header;
		int x, y;
		int c;

		/* If the terminal size changes, we need to pass that on to ncurses. */
		if (screen_size_changed) {
			get_terminal_size();
			resizeterm(height, width);
			screen_size_changed = 0;
			view_changed = 1;
		}

		// Clamp viewport to visible size.
		if (max_y != -1) {
			origin_y = origin_y > max_y - height ? max_y - height : origin_y;
		}

		/* If either the timeout passed (and so we need to run the command
		   again), or if the user changed the view (by paging around), then we
		   need to redraw. */
		if (rerun_command || view_changed) {
			clear();

			if (rerun_command) {
				// Reopen the process pipe.
				if (command_pipe != NULL) {
					pclose(command_pipe);
				}
				if (!(command_pipe = popen(command, "r"))) {
					perror("popen");
					do_exit(2);
				}
				max_y = -1;

				// Keep previous command output so that we can do differences.
				if (previous_command_output != NULL) {
					free(previous_command_output);
				}
				previous_command_output = (chtype *)malloc(sizeof(chtype) * command_output_length);
				memcpy(previous_command_output, command_output, sizeof(chtype) * command_output_length);
				previous_command_output_length = command_output_length;

				// Reinitialize the command output buffer.
				command_output_length = 0;
			}

			if (show_title) {
				// left justify interval and command,
				// right justify time, clipping all to fit window width
				asprintf(&header, "Every %.1fs: %.*s",
					interval, min(width - 1, command_length), command);
				mvaddstr(0, 0, header);
				if (strlen(header) > (size_t) (width - tsl - 1))
					mvaddstr(0, width - tsl - 4, "...  ");
				mvaddstr(0, width - tsl + 1, ts);
				free(header);
			}

			restart_draw:
			x = 0;
			y = show_title;
			int offset = 0;
			while (true) {
				int done = 0;
				while ((offset < command_output_length) && !done) {
					// Print to the terminal.
					if (isprint(command_output[offset] & A_CHARTEXT)
						&& (y - origin_y >= show_title) && (y - origin_y < height)
						&& (x - origin_x < width) && (x - origin_x >= 0)
						&& !go_to_end) {
						move(y - origin_y, x - origin_x);
						addch(command_output[offset]);
					}

					// Advance 'x', 'y', 'offset'.
					switch (command_output[offset]) {
					case '\t':
						x += 8;
						break;
					case '\n':
						x = 0;
						y++;
						break;
					default:
						x++;
						break;
					}
					offset++;

					// Are we done drawing our window of the output?
					done = !go_to_end && (y > origin_y + height);
				}

				if (done) break;

				/* If we got here, it means that we need to read more process
				   output. */
				if (feof(command_pipe)) {
					max_y = y;
					/* Unless there's nothing else to read, in which case we're
					   done. */
					if (go_to_end) {
						origin_y = y - height;
						go_to_end = 0;
						goto restart_draw;
					}
					break;
				} else {
					char buffer[128];
					size_t bytes_read = fread(buffer, sizeof(char), sizeof(buffer) / sizeof(char), command_pipe);
					command_output = (chtype *)realloc(command_output, sizeof(chtype) * (command_output_length + bytes_read));
					if (command_output == NULL) {
						puts("couldn't realloc");
						exit(1);
					}
					int i;
					for (i = 0; i < bytes_read; i++) {
						command_output[command_output_length + i] = buffer[i];
					}
					command_output_length += bytes_read;

					// Compare the new bytes against previous invocation's output bytes.
					if (option_differences && (previous_command_output != NULL)) {
						for (i = command_output_length - bytes_read; (i < command_output_length) && (i < previous_command_output_length); i++) {
							// Highlight a difference!
							if ((command_output[i] & A_CHARTEXT) != (previous_command_output[i] & A_CHARTEXT)) {
								command_output[i] |= A_STANDOUT;
							}

							// If --differences=cumulative, also bring over attributes from old output.
							if (option_differences_cumulative) {
								command_output[i] |= previous_command_output[i] & A_ATTRIBUTES;
							}
						}
					}
				}
			}

			refresh();
		}

		// Get input for paging.
		view_changed = 0;
		if (option_paging && ((c = getch()) != ERR)) {
			switch (c) {
			case KEY_UP:
				origin_y -= 8;
				origin_y = origin_y >= 0 ? origin_y : 0;
				view_changed = 1;
				break;
			case KEY_RIGHT:
				origin_x += 8;
				view_changed = 1;
				break;
			case KEY_DOWN:
				origin_y += 8;
				view_changed = 1;
				break;
			case KEY_LEFT:
				origin_x -= 8;
				origin_x = origin_x >= 0 ? origin_x : 0;
				view_changed = 1;
				break;
			case KEY_NPAGE:
				origin_y += height - show_title;
				view_changed = 1;
				break;
			case KEY_PPAGE:
				origin_y -= height - show_title;
				origin_y = origin_y >= 0 ? origin_y : 0;
				view_changed = 1;
				break;
			case 'g':
				origin_x = 0;
				origin_y = 0;
				view_changed = 1;
				break;
			case 'G':
				origin_x = 0;
				go_to_end = 1;
				view_changed = 1;
				break;
			}
		}

		// Check if interval passed: do we need to run the command again?
		if (gettimeofday(&tv, NULL) == -1) {
			puts("gettimeofday() returned -1");
			exit(1);
		}
		unsigned long time_now = (tv.tv_sec * 1000000ul) + tv.tv_usec;
		if (time_now - time_then >= interval * 1000000) {
			time_then = time_now;
			rerun_command = 1;
		} else {
			rerun_command = 0;
		}
	}

	endwin();

	return 0;
}
