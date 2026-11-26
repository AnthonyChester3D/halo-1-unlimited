/* main.c: the Halo 1 Unlimited installer, an XBE of its own (nxdk; port/oxbox/installer/README.md). It makes OpenCE
from the player's own retail Halo XBE, with the package beside it (opence.oxpk: tools/mkpackage.py) and the pictures
from the game's own ui.map (pictures.c), keeping the retail XBE as default_retail.xbe. Nothing of Microsoft's code or
of Halo's art is in the installer or the package.

A player sees menus climbed with A and B: Install (choose your Halo, then it runs to the end with no questions) or,
installed, Play, Check for updates / Update, Map Packs and More (the original Halo back, About), each item explained
in a panel under the menu; an operation shows a few plain lines, its details going to installer_log.txt. With
developer.ini beside the installer ("developer = 1"; "update_url = <address>" and "allow_unsigned = 1" for a test
update location), a Developer item opens the list below, as the installer was.

The Developer list shows every Halo it finds: the ones it can install to first, in green; the others in grey.
	A      install (or update) OpenCE in the folder chosen
	X      put the retail Halo back (from default_retail.xbe), and its stock maps (stockmaps.c)
	Y      look online for a newer package (download.c: the project's latest release)
	START  play what's in the folder (green ones only)
	WHITE  import a map pack's maps into Halo 1 Unlimited (another Halo's folder: packs.c), or take them out
	B      back to the dashboard
	LT/RT  a page up or down
A package signed by a key it trusts that carries a newer installer (tools/mkpackage.py --installer) offers to update
the installer itself.

A test update location (update_url.txt: download.h) points Y, and the game's updater, at another address, a PC's or a
NAS's; with an allow_unsigned line in it, the test packages there (not signed) are used as signed ones are. */
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <SDL.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "download.h"
#include "library.h"
#include "logs.h"
#include "package.h"
#include "packs.h"
#include "pictures.h"
#include "scancache.h"
#include "screen.h"
#include "sha256.h"
#include "stockmaps.h"
#include "consolidate.h"
#include "text.h"

#include "keys.h"
#include "notices.h"
/* (v1.171, installer 64: the network card and USB stopped before it starts anything: a quick reboot left them writing
into the next program's memory) */
#define OXBOX_QUIESCE_DEFINE
#include "../net/oxbox_quiesce.h"

#define TITLE "Halo 1 Unlimited installer"
/* the installer's version: a package carrying a higher one can update it (tools/mkpackage.py reads it from the mark) */
#ifndef INSTALLER_VERSION
#define INSTALLER_VERSION 76
#endif
__attribute__((used)) static struct
{
	char mark[8];
	unsigned long version;
} const installer_mark = {{'O', 'X', 'I', 'N', 'S', 'T', 'V', '1'}, INSTALLER_VERSION};
/* its own folder's files: D:'s, or H:'s when D: isn't its folder (own_folder: the game's updater) */
static char own_drive = 'D';
static char const *own_path(char const *name);
#define PACKAGE_PATH own_path("opence.oxpk")
#define OLD_PACKAGE_PATH own_path("opence_old.oxpk") /* (the package a download replaced) */
#define DOWNLOAD_PATH own_path("opence_download.oxpk")
#define SCAN_CACHE_PATH own_path("scan_cache.bin") /* (what was found in the Halo folders and packs: scancache.h) */
#define URL_PATH own_path("update_url.txt") /* (a test update location: download.h) */
#define OLD_URL_PATH own_path("installer_url.txt") /* (its earlier name) */
#define OLD_INSTALLER_PATH own_path("installer_old.xbe") /* (the installer an update replaced) */
#define NOTICES_PATH own_path("THIRD_PARTY.txt")
#define LOG_PATH own_path("installer_log.txt")
/* what the installer fills in a build (package.c): its pictures and each header. All of them are left out of its hash
when it's checked (installer 42 has 5: before, 4 fitted, and the fifth, SELECT CAMPAIGN, failed every check) */
#define GENERATED_REGIONS (1 + HEADER_COUNT)
#define LAST_PATH own_path("installer_last.txt") /* the folder of the last install that worked, first in the list next time */
/* the updater: this installer's copy in each Halo folder it installs to (<Halo>\updater\default.xbe), and the note the
game leaves beside it (the game's offer of a newer release: port/oxbox/oxbox_sdk.c) to have it update that folder and
start the game again, with no buttons pressed */
#define UPDATER_FOLDER "updater"
#define AUTO_PATH own_path("auto_update.txt")
#define SETTINGS_PATH own_path("installer_settings.txt") /* "update = always": updates installed without asking */
#define CONTINUE_PATH own_path("update_continue.txt") /* (the installer an update carried goes on with the update) */
static int auto_mode;
static int developer;              /* (developer.ini beside the installer says developer = 1: the Developer screen) */
static char developer_url[512];    /* (its update_url: a test update location, as update_url.txt's) */
static int developer_unsigned;     /* (its allow_unsigned = 1) */
static char developer_library[512]; /* (its library_url: a test map library's catalog; allow_unsigned counts for it too) */
static char developer_logs[512];    /* (its logs_url: where Send logs sends them, for testing) */
static int quiet;                  /* (a player's screen runs an operation: its details only in the log: say_in) */
static char task_title[80];        /* (what that screen says it's doing: heading) */
static int stepping;               /* (step: a line for the player, shown under the title) */
static int task_problem;           /* (a warning or a problem in this task: its result waits for a button) */
#define HALO_TITLE_ID 0x4D530004ul
/* Halo 1 Unlimited's own title ID ('LJ' 'HU', 0x4C4A4855: tools/mkxbe.py), so its own save folder (E:\UDATA\4c4a4855)
and storage (E:\TDATA\4c4a4855): its profiles and gametypes are its own, starting empty, and retail Halo's are never
read or written. (v1.53 alone had 'OC' 0002, 4f430002) */
#define OWN_TITLE_FOLDER "4c4a4855"
#define V153_TITLE_FOLDER "4f430002"
#define HALO_TITLE_FOLDER "4d530004"
#define MAX_XBE (16 * 1024 * 1024)
static struct package package;
static int package_ok, package_missing, signature;
static int test_unsigned;          /* (update_url.txt's allow_unsigned: a test location's unsigned packages are used) */
static char updater_halo[260];     /* (the game's updater: the Halo folder it's in, whose update_url.txt counts too) */

/* a package it uses: signed by a key it trusts, or, with a test location that allows it, not signed at all */
static int trusted(int sig)
{
	return sig == PACKAGE_SIGNED || (sig == PACKAGE_UNSIGNED && test_unsigned);
}
static char const *signer;
static char package_error[256];

static unsigned long u32(unsigned char const *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- the log: the screen below the heading while working, and installer_log.txt ---- */

#define LOG_TOP 5
#define LOG_ROWS (SCREEN_ROWS - LOG_TOP)
#define LOG_COLUMNS (SCREEN_COLUMNS - SCREEN_SPINNER_COLUMNS - 1) /* (beside the spinner's corner) */
static char log_lines[LOG_ROWS][SCREEN_COLUMNS + 1];
static unsigned long log_colours[LOG_ROWS];
static int log_count;

static void log_file(char const *text)
{
	FILE *file = fopen(LOG_PATH, "a");
	if (file)
	{
		fprintf(file, "%s\n", text);
		fclose(file);
	}
}

static void log_clear(void)
{
	memset(log_lines, 0, sizeof(log_lines));
	log_count = 0;
}

static void say_line(unsigned long colour, char const *text, int length)
{
	if (log_count == LOG_ROWS)
	{
		memmove(log_lines[0], log_lines[1], sizeof(log_lines[0]) * (LOG_ROWS - 1));
		memmove(&log_colours[0], &log_colours[1], sizeof(log_colours[0]) * (LOG_ROWS - 1));
		log_count--;
	}
	text_print(log_lines[log_count], sizeof(log_lines[0]), "%.*s", length, text);
	log_colours[log_count++] = colour;
}

/* ---- a task's receipt: what a player's screen shows under its title. Its steps (done as the next starts), the maps
by name as they're made, grouped (Multiplayer maps, Campaign), a later pass over them as a count, and its result ---- */

enum { RECEIPT_ACTION, RECEIPT_RESULT, RECEIPT_GROUP, RECEIPT_PROBLEM, RECEIPT_STATUS, RECEIPT_BLANK };
#define RECEIPT_MOST 24
static struct receipt_entry
{
	int kind, done, n, m;
	unsigned long colour;               /* (a problem's) */
	char text[200];
	char names[480];                    /* (a group's maps, ", " between) */
	char right[24];                     /* (beside it, in place of "n of m": "12 of 40 MB") */
} receipt[RECEIPT_MOST];
static int receipt_count, receipt_plan;  /* (receipt_plan: entry 0 is the task's plan, until a step takes its place) */
static int receipt_last_n;              /* (the map a pass is on: a lower one starts the next pass) */
static int receipt_pass;
static int receipt_later_total;         /* (a later pass's maps, when not all of them: Halo's 13 without the campaign) */
static int receipt_batch;               /* (Consolidate all: one line a pack, its maps counted on it; its steps in the log) */

/* Halo's maps, as the game calls them (the first 13 multiplayer, then the campaign) */
static struct { char const *file, *name; } const receipt_stock[] = {
	{"beavercreek", "Battle Creek"}, {"bloodgulch", "Blood Gulch"}, {"boardingaction", "Boarding Action"},
	{"carousel", "Derelict"}, {"chillout", "Chill Out"}, {"damnation", "Damnation"}, {"hangemhigh", "Hang 'Em High"},
	{"longest", "Longest"}, {"prisoner", "Prisoner"}, {"putput", "Chiron TL-34"}, {"ratrace", "Rat Race"},
	{"sidewinder", "Sidewinder"}, {"wizard", "Wizard"},
	{"a10", "The Pillar of Autumn"}, {"a30", "Halo"}, {"a50", "The Truth and Reconciliation"},
	{"b30", "The Silent Cartographer"}, {"b40", "Assault on the Control Room"}, {"c10", "343 Guilty Spark"},
	{"c20", "The Library"}, {"c40", "Two Betrayals"}, {"d20", "Keyes"}, {"d40", "The Maw"}};
#define RECEIPT_STOCK_MP 13
#define RECEIPT_STOCK (int)(sizeof(receipt_stock) / sizeof(receipt_stock[0]))

static void receipt_clear(void)
{
	receipt_count = receipt_plan = receipt_last_n = receipt_pass = receipt_later_total = 0;
}

/* a new entry: everything before it is done; a group: the groups before it (a status line goes) */
static struct receipt_entry *receipt_add(int kind)
{
	struct receipt_entry *e;
	int i;
	if (receipt_count && receipt[receipt_count - 1].kind == RECEIPT_STATUS)
		receipt_count--;
	if (kind != RECEIPT_STATUS && kind != RECEIPT_PROBLEM && kind != RECEIPT_BLANK)
		for (i = 0; i < receipt_count; i++)
			if (kind != RECEIPT_GROUP || receipt[i].kind == RECEIPT_GROUP)
				receipt[i].done = 1;
	if (receipt_count == RECEIPT_MOST)
	{
		memmove(&receipt[0], &receipt[1], sizeof(receipt[0]) * (RECEIPT_MOST - 1));
		receipt_count--;
		receipt_plan = 0;
	}
	e = &receipt[receipt_count++];
	memset(e, 0, sizeof(*e));
	e->kind = kind;
	return e;
}

/* the entry running now (a group under its step, else the step), or NULL */
static struct receipt_entry *receipt_running(void)
{
	int i;
	for (i = receipt_count - 1; i >= 0; i--)
		if ((receipt[i].kind == RECEIPT_ACTION || receipt[i].kind == RECEIPT_GROUP) && !receipt[i].done)
			return &receipt[i];
		else if (receipt[i].kind == RECEIPT_ACTION || receipt[i].kind == RECEIPT_RESULT)
			return NULL;
	return NULL;
}

/* a step for the player: "...." at its end: an action (done when the next comes), else a result */
static void receipt_step(char const *text)
{
	int length = (int)strlen(text);
	int action = length > 3 && !strcmp(text + length - 3, "...");
	struct receipt_entry *e;
	if (action && receipt_plan && receipt_count == 1)
		receipt_count = 0;               /* (the plan said it: the steps say it better) */
	receipt_plan = 0;
	if (!action && receipt_count)
		receipt_add(RECEIPT_BLANK);      /* (a line between the steps and the result) */
	e = receipt_add(action ? RECEIPT_ACTION : RECEIPT_RESULT);
	text_print(e->text, sizeof(e->text), "%s", text);
	receipt_last_n = receipt_pass = 0;
}

/* a map made ("name (3 of 23): making it small...", "Converting 3 of 14: x.map"): its name in the receipt, under its
group; a later pass over them, a count. 0: no such line */
static int receipt_progress(char const *text)
{
	char const *of = strstr(text, " of "), *open, *close;
	char name[64];
	int n, m, k, length;
	struct receipt_entry *e = NULL;
	if (!of || of == text || of[-1] < '0' || of[-1] > '9' || of[4] < '0' || of[4] > '9')
		return 0;
	if (receipt_batch)
	{
		/* (all the packs: beside the pack, which pass over its maps and how far: "shrinking 5 of 14") */
		static char const *const words[][2] = {{"Measuring", "measuring"}, {"Reading", "measuring"},
			{"Converting", "shrinking"}, {"Checking", "checking"}, {"Shrinking", "shrinking"}};
		int w;
		if ((e = receipt_running()) == NULL)
			return 1;
		for (w = 0; w < 5 && strncmp(text, words[w][0], strlen(words[w][0])); w++)
			;
		for (open = of; open > text && open[-1] >= '0' && open[-1] <= '9'; open--)
			;
		n = atoi(open);
		text_print(e->right, sizeof(e->right), "%s%s%d of %d", w < 5 ? words[w][1] : "", w < 5 ? " " : "", n,
			atoi(of + 4));
		return 1;
	}
	open = strstr(text, " (");
	close = strstr(of, "):");
	if (open && open < of && close)
	{
		/* "name (n of m): what" */
		n = atoi(open + 2);
		m = atoi(of + 4);
		text_print(name, sizeof(name), "%.*s", (int)(open - text), text);
	}
	else if (!strncmp(text, "Converting ", 11) && (close = strstr(of, ": ")) != NULL)
	{
		n = atoi(text + 11);
		m = atoi(of + 4);
		text_print(name, sizeof(name), "%s", close + 2);
	}
	else
	{
		/* (another "how far along" line: under the step, in place of the last) */
		if (!receipt_count || receipt[receipt_count - 1].kind != RECEIPT_STATUS)
			e = receipt_add(RECEIPT_STATUS);
		else
			e = &receipt[receipt_count - 1];
		text_print(e->text, sizeof(e->text), "%s", text);
		return 1;
	}
	length = (int)strlen(name);
	if (length > 4 && !_stricmp(name + length - 4, ".map"))
		name[length - 4] = 0;
	if (n == receipt_last_n)
		return 1;                        /* (the same map, its next part) */
	if (n < receipt_last_n)
	{
		/* (a later pass over them: one line under the step, counted) */
		receipt_pass++;
		e = receipt_add(RECEIPT_GROUP);
		text_print(e->text, sizeof(e->text), strstr(text, "putting the small map") ? "Putting them in place" :
			"Checking them");
	}
	receipt_last_n = n;
	if (receipt_pass)
	{
		e = receipt_running();
		if (e)
		{
			e->n++;
			e->m = receipt_later_total ? receipt_later_total : m;
		}
		return 1;
	}
	/* (the first pass: its name, under its group) */
	for (k = 0; k < RECEIPT_STOCK && _stricmp(receipt_stock[k].file, name); k++)
		;
	{
		char const *title = k == RECEIPT_STOCK ? "" : k < RECEIPT_STOCK_MP ? "Multiplayer maps" :
			receipt_later_total && receipt_later_total <= RECEIPT_STOCK_MP ? "Campaign (its textures and sounds only)" :
			"Campaign";
		int total = k == RECEIPT_STOCK ? m : k < RECEIPT_STOCK_MP ? RECEIPT_STOCK_MP : RECEIPT_STOCK - RECEIPT_STOCK_MP;
		e = receipt_running();
		if (!e || e->kind != RECEIPT_GROUP || strcmp(e->text, title))
		{
			e = receipt_add(RECEIPT_GROUP);
			text_print(e->text, sizeof(e->text), "%s", title);
			e->m = total;
		}
		e->n++;
		length = (int)strlen(e->names);
		text_print(e->names + length, sizeof(e->names) - length, "%s%s", length ? ", " : "",
			k == RECEIPT_STOCK ? name : receipt_stock[k].name);
	}
	return 1;
}

/* a problem or warning: in the receipt as it's said */
static void receipt_problem(unsigned long colour, char const *text)
{
	struct receipt_entry *e = receipt_add(RECEIPT_PROBLEM);
	e->colour = colour;
	text_print(e->text, sizeof(e->text), "%s", text);
}

/* (lines of the receipt, as drawn) */
#define RECEIPT_TOP (LOG_TOP - 1)
#define RECEIPT_ROWS (SCREEN_ROWS - 1 - RECEIPT_TOP)  /* (the last row: task_wait's buttons) */
static struct receipt_line
{
	unsigned long colour, right_colour;
	int column;
	char text[SCREEN_COLUMNS + 1];
	char right[24];
} receipt_lines[RECEIPT_ROWS * 4];
static int receipt_line_count;

static struct receipt_line *receipt_line(int column, unsigned long colour)
{
	struct receipt_line *l;
	if (receipt_line_count == (int)(sizeof(receipt_lines) / sizeof(receipt_lines[0])))
	{
		memmove(&receipt_lines[0], &receipt_lines[1], sizeof(receipt_lines[0]) * (receipt_line_count - 1));
		receipt_line_count--;
	}
	l = &receipt_lines[receipt_line_count++];
	memset(l, 0, sizeof(*l));
	l->column = column;
	l->colour = colour;
	return l;
}

/* text wrapped at its spaces from column to LOG_COLUMNS (a list: between its names, after their ", ") */
static void receipt_wrap(int column, unsigned long colour, char const *text, int list)
{
	int width = LOG_COLUMNS - column;
	for (;;)
	{
		int length = (int)strlen(text), cut = length;
		if (length > width)
		{
			for (cut = width; cut > width / 3 && (text[cut] != ' ' || (list && text[cut - 1] != ',')); cut--)
				;
			if (cut == width / 3)
				for (cut = width; cut > width / 2 && text[cut] != ' '; cut--)
					;
			if (text[cut] != ' ')
				cut = width;
		}
		text_print(receipt_line(column, colour)->text, SCREEN_COLUMNS + 1, "%.*s", cut, text);
		text += cut;
		while (*text == ' ')
			text++;
		if (!*text)
			return;
	}
}

static void receipt_show(void)
{
	int i, first, r;
	receipt_line_count = 0;
	for (i = 0; i < receipt_count; i++)
	{
		struct receipt_entry const *e = &receipt[i];
		struct receipt_line *l;
		char label[200];
		int length;
		switch (e->kind)
		{
		case RECEIPT_ACTION:
		case RECEIPT_GROUP:
			text_print(label, sizeof(label), "%s", e->text);
			if (e->done)
			{
				/* (done: without its "..." and "(about 5 minutes)") */
				char *note;
				length = (int)strlen(label);
				if (length > 3 && !strcmp(label + length - 3, "..."))
					label[length -= 3] = 0;
				if (length && label[length - 1] == ')' && (note = strrchr(label, '(')) != NULL && note > label &&
					note[-1] == ' ')
					note[-1] = 0;
			}
			if (e->kind == RECEIPT_GROUP && !label[0] && !e->names[0])
				break;
			l = receipt_line(e->kind == RECEIPT_GROUP ? 4 : 2, e->done ? COLOUR_GREY : COLOUR_WHITE);
			text_print(l->text, sizeof(l->text), "%s", label);
			if (e->done)
			{
				text_print(l->right, sizeof(l->right), "%s", e->right[0] ? e->right : "done");
				l->right_colour = COLOUR_GREEN;
			}
			else if (e->right[0])
				text_print(l->right, sizeof(l->right), "%s", e->right);
			else if (e->m)
				text_print(l->right, sizeof(l->right), "%d of %d", e->n, e->m);
			if (!l->right_colour)
				l->right_colour = COLOUR_GREY;
			if (e->kind == RECEIPT_GROUP && e->names[0])
			{
				if (!label[0])
				{
					/* (no group name: the names in its place, the count on the step's line) */
					receipt_line_count--;
					if (receipt_line_count && !e->done)
					{
						struct receipt_line *step_line = &receipt_lines[receipt_line_count - 1];
						text_print(step_line->right, sizeof(step_line->right), "%d of %d", e->n, e->m);
						step_line->right_colour = COLOUR_GREY;
					}
				}
				receipt_wrap(6, COLOUR_DIM, e->names, 1);
			}
			break;
		case RECEIPT_RESULT:
			receipt_wrap(2, COLOUR_WHITE, e->text, 0);
			break;
		case RECEIPT_PROBLEM:
			receipt_wrap(2, e->colour, e->text, 0);
			break;
		case RECEIPT_STATUS:
			receipt_wrap(4, COLOUR_DIM, e->text, 0);
			break;
		case RECEIPT_BLANK:
			receipt_line(0, COLOUR_DIM);
			break;
		}
	}
	/* (too many lines: its last ones) */
	first = receipt_line_count > RECEIPT_ROWS ? receipt_line_count - RECEIPT_ROWS : 0;
	for (r = 0; r < RECEIPT_ROWS; r++)
	{
		struct receipt_line const *l = first + r < receipt_line_count ? &receipt_lines[first + r] : NULL;
		screen_row(RECEIPT_TOP + r, COLOUR_BLACK);
		if (!l)
			continue;
		if (l->right[0])
		{
			int at = LOG_COLUMNS - (int)strlen(l->right);
			screen_text(RECEIPT_TOP + r, l->column, l->colour, "%.*s", at - 1 - l->column, l->text);
			screen_text(RECEIPT_TOP + r, at, l->right_colour, "%s", l->right);
		}
		else
			screen_text(RECEIPT_TOP + r, l->column, l->colour, "%.*s", LOG_COLUMNS - l->column, l->text);
	}
	screen_show();
}

/* (the old name: a task's screen drawn again) */
static void task_line_show(void)
{
	receipt_show();
}

static void say_in(unsigned long colour, char const *format, ...)
{
	char text[256], *at;
	va_list list;
	int r, indent = 0;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	log_file(text);
	/* (a player's screen: the details only in the log, the latest on one dim line under the title; a warning or a
	problem shows below as it does) */
	if (quiet && colour != COLOUR_RED && colour != COLOUR_YELLOW)
	{
		for (at = text; *at == ' ' || *at == '('; at++)
			;
		/* (only what's for the player: a step, or how far along it is; the rest is in the log) */
		if (stepping)
		{
			if (receipt_batch)
				return;
			receipt_step(at);
		}
		else if (!receipt_progress(at))
			return;
		receipt_show();
		return;
	}
	if (quiet)
	{
		task_problem = 1;
		for (at = text; *at == ' '; at++)
			;
		receipt_problem(colour, at);
		receipt_show();
		return;
	}
	/* (a long line wrapped at a space, the rest indented as it was) */
	while (text[indent] == ' ')
		indent++;
	for (at = text; (int)strlen(at) > LOG_COLUMNS;)
	{
		int cut = LOG_COLUMNS;
		while (cut > LOG_COLUMNS / 2 && at[cut] != ' ')
			cut--;
		if (at[cut] != ' ')
			cut = LOG_COLUMNS;
		say_line(colour, at, cut);
		at += cut;
		while (*at == ' ')
			at++;
		if (indent + 2 < LOG_COLUMNS / 2)
		{
			at -= indent + 2;
			memset(at, ' ', (size_t)(indent + 2));
		}
	}
	say_line(colour, at, (int)strlen(at));
	for (r = 0; r < LOG_ROWS; r++)
	{
		screen_row(LOG_TOP + r, COLOUR_BLACK);
		screen_text(LOG_TOP + r, 0, log_colours[r], "%s", log_lines[r]);
	}
	screen_show();
}

#define say(...) say_in(COLOUR_WHITE, __VA_ARGS__)

/* a line for the player (on a task's screen, under its title; else as say) */
static void step(char const *format, ...)
{
	char text[256];
	va_list list;
	va_start(list, format);
	text_vprint(text, sizeof(text), format, list);
	va_end(list);
	stepping = 1;
	say_in(COLOUR_WHITE, "%s", text);
	stepping = 0;
}

static void pictures_step(char const *step)
{
	say("  %s", step);
}

/* the package's signature, as the heading says it */
static char const *signature_text(unsigned long *colour)
{
	static char text[48];
	*colour = signature == PACKAGE_SIGNED ? COLOUR_GREEN : COLOUR_YELLOW;
	if (signature == PACKAGE_SIGNED)
		text_print(text, sizeof(text), "signed (%s)", signer);
	else
		text_print(text, sizeof(text), "%s", signature == PACKAGE_UNKNOWN_KEY ? "signed by a key it doesn't know" :
			"not signed");
	return text;
}

static void player_heading(char const *title);

static void task_line_show(void);

static void heading(char const *what)
{
	if (quiet)
	{
		/* (an operation's own heading on a player's screen: the screen's title stays, and its line) */
		player_heading(task_title);
		task_line_show();
		return;
	}
	screen_clear();
	screen_text(0, 0, COLOUR_YELLOW, "%s, version %d", TITLE, INSTALLER_VERSION);
	if (package_ok)
	{
		unsigned long colour;
		char const *said = signature_text(&colour);
		screen_text(0, SCREEN_COLUMNS - (int)strlen(said), colour, "%s", said);
	}
	screen_text(1, 0, COLOUR_GREY, "Package: ");
	if (package_ok && package.release)
		screen_text(1, 9, COLOUR_WHITE, "%s (release %lu)", package.name, package.release);
	else
		screen_text(1, 9, COLOUR_WHITE, "%s", package_ok ? package.name : "none");
	screen_text(2, 0, COLOUR_DIM, "------------------------------------------------------------------------");
	if (what)
		screen_text(3, 0, COLOUR_WHITE, "%s", what);
	screen_show();
}

/* ---- the controllers: every pad, d-pad or left stick (held: repeats), triggers a page ---- */

enum { NONE, UP, DOWN, PAGE_UP, PAGE_DOWN, A, B, X, Y, START, WHITE, REFRESH };

/* (the background update check's state: a menu watching it is drawn again when it changes: wait_input's REFRESH) */
enum { CHECK_FAILED, CHECK_RUNNING, CHECK_NEWER, CHECK_NEWEST };
static volatile int check_state = CHECK_FAILED;
static int menu_watches_check, check_seen = -1;

static unsigned held_buttons = ~0u; /* (none counts as pressed until it's been seen up) */
static int held_direction;
static DWORD held_since, last_repeat;

/* the pads as last read (pads_main): buttons held (1 << A...), and the direction held */
static volatile unsigned pad_buttons;
static volatile int pad_direction;

/* the pads read every 16 ms on a thread of their own, from the start to the end, whatever else the installer is doing
(a menu, looking round the drives, installing): the dashboard's own back-to-the-dashboard buttons work only while
something reads the pads. Every SDL call is made here */
static DWORD WINAPI pads_main(LPVOID unused)
{
	SDL_GameController *pads[4] = {NULL, NULL, NULL, NULL};
	(void)unused;
	SDL_Init(SDL_INIT_GAMECONTROLLER);
	for (;;)
	{
		SDL_Event e;
		unsigned buttons = 0;
		int direction = NONE, i;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_CONTROLLERDEVICEADDED)
			{
				for (i = 0; i < 4 && pads[i]; i++)
					;
				if (i < 4)
					pads[i] = SDL_GameControllerOpen(e.cdevice.which);
			}
			else if (e.type == SDL_CONTROLLERDEVICEREMOVED)
			{
				for (i = 0; i < 4; i++)
				{
					if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which)
					{
						SDL_GameControllerClose(pads[i]);
						pads[i] = NULL;
					}
				}
			}
		}
		for (i = 0; i < 4; i++)
		{
			SDL_GameController *p = pads[i];
			int y;
			if (!p)
				continue;
			buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_A) << A;
			buttons |= (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_B) |
				SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_BACK)) << B;
			buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_X) << X;
			buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_Y) << Y;
			buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_START) << START;
			buttons |= SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) << WHITE; /* (nxdk's White) */
			y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
			if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -16000)
				direction = UP;
			else if (SDL_GameControllerGetButton(p, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 16000)
				direction = DOWN;
			else if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
				direction = PAGE_UP;
			else if (SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
				direction = PAGE_DOWN;
		}
		pad_buttons = buttons;
		pad_direction = direction;
		Sleep(16);
	}
	return 0;
}

static void pads_start(void)
{
	HANDLE thread = CreateThread(NULL, 0, pads_main, NULL, 0, NULL);
	if (thread)
	{
		SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL); /* (it's never kept waiting by the work) */
		CloseHandle(thread);
	}
}

/* a button pressed since the last look (or a direction, repeating while held); NONE: none */
static int poll_input(void)
{
	unsigned buttons = pad_buttons, pressed;
	int direction = pad_direction, i;
	DWORD now;
	pressed = buttons & ~held_buttons;
	held_buttons = buttons;
	for (i = A; i <= WHITE; i++)
	{
		if (pressed & (1u << i))
			return i;
	}
	now = GetTickCount();
	if (direction != held_direction)
	{
		held_direction = direction;
		held_since = last_repeat = now;
		return direction;
	}
	if (direction != NONE && now - held_since > 400 && now - last_repeat > 90)
	{
		last_repeat = now;
		return direction;
	}
	return NONE;
}

/* (the spinner stops while it waits for a button: nothing is being done; and moves again after it) */
static int wait_input(void)
{
	int b;
	screen_spinner(0);
	while ((b = poll_input()) == NONE)
	{
		if (menu_watches_check && check_state != check_seen)
		{
			check_seen = check_state;
			b = REFRESH;
			break;
		}
		Sleep(16);
	}
	screen_spinner(1);
	return b;
}

/* ---- files ---- */

static int exists(char const *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

/* a whole file (up to MAX_XBE bytes), or NULL */
static unsigned char *read_whole(char const *path, unsigned long *size)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	unsigned char *data;
	DWORD got;
	if (file == INVALID_HANDLE_VALUE)
		return NULL;
	*size = GetFileSize(file, NULL);
	if (*size == INVALID_FILE_SIZE || *size > MAX_XBE || !(data = malloc(*size ? *size : 1)))
	{
		CloseHandle(file);
		return NULL;
	}
	if (!ReadFile(file, data, *size, &got, NULL) || got != *size)
	{
		free(data);
		data = NULL;
	}
	CloseHandle(file);
	return data;
}

/* the file's SHA-256 with some of it read as zeros (a build's pictures), as written on the disk; 0 when read */
static int file_sha256(char const *path, unsigned long const *zero_at, unsigned long const *zero_size, int zeros,
	unsigned char *digest, unsigned long *size)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	static unsigned char buffer[65536];
	struct sha256 s;
	unsigned long at = 0;
	DWORD got;
	int z;
	if (file == INVALID_HANDLE_VALUE)
		return 1;
	sha256_start(&s);
	while (ReadFile(file, buffer, sizeof(buffer), &got, NULL) && got)
	{
		for (z = 0; z < zeros; z++)
		{
			unsigned long from = zero_at[z] > at ? zero_at[z] : at;
			unsigned long to = zero_at[z] + zero_size[z] < at + got ? zero_at[z] + zero_size[z] : at + got;
			if (from < to)
				memset(buffer + (from - at), 0, to - from);
		}
		sha256_add(&s, buffer, got);
		at += got;
	}
	CloseHandle(file);
	sha256_end(&s, digest);
	*size = at;
	return 0;
}

struct writer
{
	HANDLE file;
};

static int write_part(void *context, unsigned char const *data, unsigned long size)
{
	DWORD put;
	return !WriteFile(((struct writer *)context)->file, data, size, &put, NULL) || put != size;
}

/* (package_make's parts, about 25,000 of them, most a few bytes to a few KB: gathered and written a megabyte at a time.
Each WriteFile is a trip through the kernel and FATX: one a part took ~14 s of an install in xemu. Installer 65) */
#define GATHER_SIZE (1024 * 1024)

struct gatherer
{
	struct writer w;
	unsigned char *buffer;
	unsigned long held;
};

static int gather_flush(struct gatherer *g)
{
	unsigned long held = g->held;
	g->held = 0;
	return held && write_part(&g->w, g->buffer, held);
}

static int gather_part(void *context, unsigned char const *data, unsigned long size)
{
	struct gatherer *g = context;
	while (size)
	{
		unsigned long n = GATHER_SIZE - g->held < size ? GATHER_SIZE - g->held : size;
		memcpy(g->buffer + g->held, data, n);
		g->held += n, data += n, size -= n;
		if (g->held == GATHER_SIZE && gather_flush(g))
			return 1;
	}
	return 0;
}

static HANDLE create(char const *path)
{
	DeleteFileA(path);
	return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

/* (written to the disk before it's closed) */
static int finish(HANDLE file)
{
	IO_STATUS_BLOCK status;
	int ok = NT_SUCCESS(NtFlushBuffersFile(file, &status));
	return CloseHandle(file) && ok;
}

/* ---- XBEs ---- */

/* what an XBE's headers say */
struct xbe_head
{
	int present, halo, opence;
	unsigned long size;
};

static void read_head(char const *path, struct xbe_head *head)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	static unsigned char h[16384];
	unsigned long base, cert, count, sections, i;
	DWORD got = 0;
	memset(head, 0, sizeof(*head));
	if (file == INVALID_HANDLE_VALUE)
		return;
	head->present = 1;
	head->size = GetFileSize(file, NULL);
	ReadFile(file, h, sizeof(h), &got, NULL);
	CloseHandle(file);
	if (got < 0x180 || memcmp(h, "XBEH", 4))
		return;
	base = u32(h + 0x104);
	cert = u32(h + 0x118) - base;
	head->halo = cert + 12 <= got && u32(h + cert + 8) == HALO_TITLE_ID;
	/* (an OpenCE build: the perf log's section, OXPERF, which mkxbe.py adds) */
	count = u32(h + 0x11c);
	sections = u32(h + 0x120) - base;
	for (i = 0; i < count && i < 64 && sections + (i + 1) * 0x38 <= got; i++)
	{
		unsigned long name = u32(h + sections + i * 0x38 + 0x14) - base;
		if (name + 7 <= got && !memcmp(h + name, "OXPERF", 7))
			head->opence = 1;
	}
}

static int retail_size(unsigned long size)
{
	int i;
	for (i = 0; package_ok && i < package.count; i++)
	{
		if (package.xbes[i].retail_size == size)
			return 1;
	}
	return 0;
}

/* a retail XBE read whole, and the package's build for it (package_match: its data is the disc's while it's
matched as a patched copy; retail_original() puts the copy's own bytes back) */
struct retail
{
	unsigned char *data;
	unsigned long size;
	struct package_patch patch;
	struct package_xbe const *xbe;
};

static void retail_original(struct retail *r)
{
	package_unpatch(r->data, &r->patch);
}

/* reads path as a retail XBE; r->xbe NULL if the package doesn't know it */
static int retail_read(char const *path, struct retail *r)
{
	memset(r, 0, sizeof(*r));
	r->data = read_whole(path, &r->size);
	if (!r->data)
		return 1;
	if (package_ok)
		r->xbe = package_match(&package, r->data, r->size, &r->patch);
	return 0;
}

static void retail_free(struct retail *r)
{
	free(r->data);
	r->data = NULL;
}

/* ---- the Halo folders ---- */

enum
{
	RETAIL,    /* default.xbe is a retail Halo the package knows */
	INSTALLED, /* default.xbe is OpenCE, and default_retail.xbe a retail Halo the package knows */
	OTHER,     /* default.xbe is another XBE, and default_retail.xbe a retail Halo */
	MISSING,   /* no default.xbe, and default_retail.xbe a retail Halo */
	UNKNOWN    /* no retail Halo the package knows (or default_retail.xbe is in the way) */
};

struct halo
{
	char path[160];   /* "E:\Games\Halo" */
	char device[200]; /* "\Device\Harddisk0\Partition1\Games\Halo" */
	int state;
	struct package_xbe const *retail; /* (the package's build for its retail XBE) */
	int patched;                      /* that retail XBE is a disc's patched for media or regions */
	int backup_in_the_way;            /* default_retail.xbe is there and isn't a retail Halo it knows */
	int opence, up_to_date;
	struct stockmaps_look stock;      /* its 13 multiplayer maps: retail, or small ones drawing on stock.shared */
	int unfinished;                   /* (Halo 1 Unlimited's own folder being built: BUILDING_MARK in it) */
	char note[40];
	char detail[2][SCREEN_COLUMNS + 1];
};

#define MAX_HALOS 128
static struct halo halos[MAX_HALOS];
static int halo_count;
static char last_install[160]; /* (LAST_PATH's folder: "E:\Games\Halo"; empty when none) */

#define BUILDING_MARK "h1u_building.txt" /* (in Halo 1 Unlimited's own folder while it's built: own_install) */

static int ready(struct halo const *h)
{
	return h->state != UNKNOWN;
}

static char const *const partitions[] = {"E:\\Partition1", "F:\\Partition6", "G:\\Partition7"};

static void to_device(char const *path, char *device, size_t size)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		if (path[0] == partitions[i][0])
		{
			text_print(device, size, "\\Device\\Harddisk0\\%s%s", partitions[i] + 3, path + 2);
			return;
		}
	}
	text_print(device, size, "%s", path);
}

/* "E:\..." from a device path on a partition this has mounted, or 0 */
/* this XBE's own path, as a device's ("\\Device\\Harddisk0\\Partition6\\games\\Halo\\updater\\default.xbe"). A title
started with XLaunchNewImage("D:\\updater\\default.xbe") (the game starting its updater) is given its launcher's D: and
the rest after a ';' ("...\\games\\Halo;\\updater\\default.xbe"): joined here as one path */
static void kernel_image(char *own, int size)
{
	char *semicolon;
	text_print(own, size, "%.*s", XeImageFileName->Length, XeImageFileName->Buffer);
	if ((semicolon = strchr(own, ';')) != NULL)
	{
		char rest[260];
		text_print(rest, sizeof(rest), "%s", semicolon + 1);
		*semicolon = 0;
		if (semicolon > own && semicolon[-1] == '\\')
			semicolon[-1] = 0;
		text_print(own + strlen(own), size - (int)strlen(own), "%s%s", rest[0] == '\\' ? "" : "\\", rest);
	}
}

/* where a drive letter points ("\\Device\\Harddisk0\\Partition6\\games\\Halo\\"), or "" */
static void drive_target(char letter, char *out, int size)
{
	ANSI_STRING name, target;
	OBJECT_ATTRIBUTES attributes;
	HANDLE handle;
	char buffer[260], link[8];
	ULONG length = 0;
	out[0] = 0;
	text_print(link, sizeof(link), "\\??\\%c:", letter);
	RtlInitAnsiString(&name, link);
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
	if (!NT_SUCCESS(NtOpenSymbolicLinkObject(&handle, &attributes)))
		return;
	target.Buffer = buffer;
	target.Length = 0;
	target.MaximumLength = sizeof(buffer) - 1;
	if (NT_SUCCESS(NtQuerySymbolicLinkObject(handle, &target, &length)))
		text_print(out, size, "%.*s", target.Length, target.Buffer);
	NtClose(handle);
}

/* a file of its own folder (own_folder): up to 4 of these at once */
static char const *own_path(char const *name)
{
	static char paths[4][64];
	static int next;
	char *path = paths[next++ & 3];
	text_print(path, sizeof(paths[0]), "%c:\\%s", own_drive, name);
	return path;
}

static int same_folder(char const *a, char const *b)
{
	size_t n = strlen(a), m = strlen(b);
	if (n && a[n - 1] == '\\') n--;
	if (m && b[m - 1] == '\\') m--;
	return n == m && !_strnicmp(a, b, n);
}

/* its own folder as H: (its files are then H:'s: own_path) */
static void own_drive_mount(char const *folder)
{
	char path[300];
	text_print(path, sizeof(path), "%s%s", folder, folder[0] && folder[strlen(folder) - 1] == '\\' ? "" : "\\");
	if (nxIsDriveMounted('H'))
		nxUnmountDrive('H');
	if (nxMountDrive('H', path))
		own_drive = 'H';
}

/* how it was started, for the screen when it has no package (and the log) */
static char started_as[2][200];

/* its own folder. D: is, as nxdk or the kernel made it, when it's started from a dashboard or as one; but the game's
updater was given the game's D:, the Halo folder (XLaunchNewImage("D:\\updater\\default.xbe"), up to v1.74), and its
own path, joined at the launch path's ';', can name another folder than D:'s. Then its own folder is mounted as H:
and its files are read there: D: itself is never changed (a remount of D: hung the installer started from C:). D:
having an updater folder with the game's note in it, and none of its own, says it's the updater given the Halo
folder */
static void own_folder(void)
{
	char own[260], d[260], line[400];
	kernel_image(own, sizeof(own));
	drive_target('D', d, sizeof(d));
	text_print(started_as[0], sizeof(started_as[0]), "Started as %.*s", XeImageFileName->Length,
		XeImageFileName->Buffer);
	text_print(started_as[1], sizeof(started_as[1]), "with D: as %s", d[0] ? d : "(none)");
	if (!_strnicmp(own, "\\Device\\", 8))
	{
		char *slash = strrchr(own, '\\');
		slash[1] = 0;
		if (!same_folder(own, d))
			own_drive_mount(own);
	}
	if (own_drive == 'D' && d[0] && !exists("D:\\auto_update.txt") &&
		exists("D:\\" UPDATER_FOLDER "\\auto_update.txt") && exists("D:\\" UPDATER_FOLDER "\\default.xbe"))
	{
		char updater[300];
		text_print(updater, sizeof(updater), "%s%s" UPDATER_FOLDER "\\", d, d[strlen(d) - 1] == '\\' ? "" : "\\");
		own_drive_mount(updater);
	}
	drive_target(own_drive, d, sizeof(d));
	text_print(line, sizeof(line), "(%s, %s; its folder, %c:, is %s)", started_as[0], started_as[1], own_drive, d);
	log_file(line);
}

/* this XBE's own path as a device's: its own folder (own_folder) and its file's name */
static void own_image(char *own, int size)
{
	char image[260], *name;
	kernel_image(image, sizeof(image));
	name = strrchr(image, '\\');
	drive_target(own_drive, own, size);
	if (!own[0])
	{
		text_print(own, size, "%s", image);
		return;
	}
	text_print(own + strlen(own), size - (int)strlen(own), "%s%s", own[strlen(own) - 1] == '\\' ? "" : "\\",
		name ? name + 1 : "default.xbe");
}

static int from_device(char const *device, char *path, size_t size)
{
	int i;
	/* (C:, the dashboard's partition, which isn't mounted: an installer started from there, as a dashboard, is read
	through D:, its own folder, so it still copies itself into the game as its updater. Not a drive Halo folders are
	looked for on) */
	static char const system_prefix[] = "\\Device\\Harddisk0\\Partition2\\";
	if (!_strnicmp(device, system_prefix, sizeof(system_prefix) - 1))
	{
		char const *name = strrchr(device, '\\');
		text_print(path, size, "%c:\\%s", own_drive, name + 1);
		return 1;
	}
	for (i = 0; i < 3; i++)
	{
		char prefix[64];
		size_t n;
		text_print(prefix, sizeof(prefix), "\\Device\\Harddisk0\\%s\\", partitions[i] + 3);
		n = strlen(prefix);
		if (!_strnicmp(device, prefix, n))
		{
			text_print(path, size, "%c:\\%s", partitions[i][0], device + n);
			return 1;
		}
	}
	return 0;
}

/* the folder's stock maps are small ones this can put back */
static int stock_maps_back(struct halo const *h)
{
	return h->stock.small && h->stock.restorable;
}

/* X does something here: the retail XBE, the retail stock maps, or both */
static int can_restore(struct halo const *h)
{
	return h->state == INSTALLED || h->state == OTHER || h->state == MISSING || stock_maps_back(h);
}

/* what examine looks at, and finds, of a Halo folder (examine_measure: its XBEs read, its stock maps' headers) */
static void examine_measure(struct halo *h)
{
	{
		char mark[220];
		text_print(mark, sizeof(mark), "%s\\" BUILDING_MARK, h->path);
		h->unfinished = GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES;
	}
	char main[200], backup[200];
	struct xbe_head main_head, backup_head;
	struct retail r;
	struct package_xbe const *main_retail = NULL, *backup_retail = NULL;
	int main_patched = 0, backup_patched = 0;

	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	read_head(main, &main_head);
	read_head(backup, &backup_head);
	if (main_head.halo && retail_size(main_head.size) && !retail_read(main, &r))
	{
		main_retail = r.xbe, main_patched = r.patch.patched;
		retail_free(&r);
	}
	if (backup_head.present && retail_size(backup_head.size) && !retail_read(backup, &r))
	{
		backup_retail = r.xbe, backup_patched = r.patch.patched;
		retail_free(&r);
	}
	stockmaps_examine(h->path, &h->stock);
	h->opence = main_head.opence;
	h->up_to_date = 0;
	h->backup_in_the_way = backup_head.present && !backup_retail;
	if (main_retail && !h->backup_in_the_way)
	{
		h->state = RETAIL;
		h->retail = main_retail, h->patched = main_patched;
	}
	else if (backup_retail)
	{
		h->state = !main_head.present ? MISSING : h->opence ? INSTALLED : OTHER;
		h->retail = backup_retail, h->patched = backup_patched;
		if (h->state == INSTALLED)
		{
			unsigned long at[GENERATED_REGIONS], size[GENERATED_REGIONS], made;
			unsigned char digest[32];
			int zeros = package_pictures(&package, h->retail, at, size, GENERATED_REGIONS);
			h->up_to_date = !file_sha256(main, at, size, zeros, digest, &made) && made == h->retail->build_size &&
				!memcmp(digest, h->retail->build_sha256, 32);
		}
	}
	else
	{
		h->state = UNKNOWN;
		h->retail = NULL;
	}
}

/* its note and lines, as the lists show them */
static void examine_describe(struct halo *h)
{
	switch (h->state)
	{
	case RETAIL:
		text_print(h->note, sizeof(h->note), "retail Halo: ready to install");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: retail %s%s.", h->retail->name,
			h->patched ? ", region/media patched" : "");
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE (keeping it as default_retail.xbe).");
		if (stock_maps_back(h))
		{
			text_print(h->note, sizeof(h->note), "retail Halo, small stock maps");
			text_print(h->detail[1], sizeof(h->detail[1]), "X puts the retail stock maps back: retail Halo needs them.");
		}
		break;
	case INSTALLED:
		text_print(h->note, sizeof(h->note), h->up_to_date ? "OpenCE (this package's)" : "OpenCE (another build)");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: OpenCE (%s). Kept: %s.",
			h->up_to_date ? "this package's" : "another build", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "%s", h->up_to_date ?
			"A installs it again; X puts the retail Halo back." : "A updates it to this package's; X: retail Halo.");
		break;
	case OTHER:
		text_print(h->note, sizeof(h->note), "another XBE (retail kept)");
		text_print(h->detail[0], sizeof(h->detail[0]), "default.xbe: another XBE. Kept: retail %s.", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE (keeping that XBE as default_old.xbe).");
		break;
	case MISSING:
		text_print(h->note, sizeof(h->note), "no default.xbe (retail kept)");
		text_print(h->detail[0], sizeof(h->detail[0]), "No default.xbe. Kept: retail %s.", h->retail->name);
		text_print(h->detail[1], sizeof(h->detail[1]), "A installs OpenCE; X puts the retail Halo back.");
		break;
	default:
		if (h->backup_in_the_way)
		{
			text_print(h->note, sizeof(h->note), "default_retail.xbe in the way");
			text_print(h->detail[0], sizeof(h->detail[0]),
				"default_retail.xbe is here, but isn't a retail Halo XBE this package knows.");
			text_print(h->detail[1], sizeof(h->detail[1]), "Move it out of the folder, then start the installer again.");
		}
		else
		{
			text_print(h->note, sizeof(h->note), h->opence ? "OpenCE, but no retail kept" : "modified or unknown XBE");
			text_print(h->detail[0], sizeof(h->detail[0]), "%s",
				!package_ok ? "There's no package to install." :
				"default.xbe isn't a retail Halo XBE this package knows (from a disc).");
			text_print(h->detail[1], sizeof(h->detail[1]),
				"To install here, copy your disc's default.xbe in as default_retail.xbe.");
			if (stock_maps_back(h))
				text_print(h->detail[1], sizeof(h->detail[1]), "X puts the retail stock maps back.");
		}
	}
}

/* v1.142: what examine_measure found, kept between runs (scancache.h) by the folder's key: its two XBEs' sizes and
write times, its maps folder's listing (names, sizes, write times: an unfinished swap's temporary files change it, so
the power-cut recovery runs) and the package's build. Nothing of an unchanged folder is read: before, every Halo
folder's XBEs were read whole and hashed, and its 23 stock maps' headers read, at every start. A folder the installer
has just changed is looked at afresh (examine_fresh) */
struct halo_facts
{
	long state, patched, backup_in_the_way, opence, up_to_date, has_retail;
	unsigned char retail_sha256[32];
	struct stockmaps_look stock;
};
static int examine_fresh;

static unsigned long examine_key(char const *path)
{
	static char const *const xbes[] = {"default.xbe", "default_retail.xbe"};
	WIN32_FIND_DATAA found;
	char pattern[240];
	HANDLE find;
	unsigned long key = 0;
	int i;
	for (i = 0; i < 2; i++)
	{
		text_print(pattern, sizeof(pattern), "%s\\%s", path, xbes[i]);
		find = FindFirstFileA(pattern, &found);
		if (find == INVALID_HANDLE_VALUE)
			key = scan_cache_hash(key, "-", 1);
		else
		{
			key = scan_cache_hash(key, &found.nFileSizeLow, 4);
			key = scan_cache_hash(key, &found.ftLastWriteTime, sizeof(found.ftLastWriteTime));
			FindClose(find);
		}
	}
	text_print(pattern, sizeof(pattern), "%s\\maps\\*", path);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			char name[MAX_PATH];
			int n;
			for (n = 0; found.cFileName[n] && n < MAX_PATH - 1; n++)
				name[n] = (char)(found.cFileName[n] >= 'A' && found.cFileName[n] <= 'Z' ? found.cFileName[n] + 32 :
					found.cFileName[n]);
			key = scan_cache_hash(key, name, (unsigned long)n);
			key = scan_cache_hash(key, &found.nFileSizeLow, 4);
			key = scan_cache_hash(key, &found.ftLastWriteTime, sizeof(found.ftLastWriteTime));
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	key = scan_cache_hash(key, &package.build, sizeof(package.build));
	key = scan_cache_hash(key, &package_ok, sizeof(package_ok));
	return key;
}

static void examine(struct halo *h)
{
	struct halo_facts facts;
	unsigned long key = examine_key(h->path);
	char mark[220];
	if (!examine_fresh && scan_cache_get(h->path, key, &facts, sizeof(facts)) &&
		(!facts.has_retail || (package_ok && package_find(&package, facts.retail_sha256))))
	{
		h->state = (int)facts.state;
		h->patched = (int)facts.patched;
		h->backup_in_the_way = (int)facts.backup_in_the_way;
		h->opence = (int)facts.opence;
		h->up_to_date = (int)facts.up_to_date;
		h->retail = facts.has_retail ? package_find(&package, facts.retail_sha256) : NULL;
		h->stock = facts.stock;
		text_print(mark, sizeof(mark), "%s\\" BUILDING_MARK, h->path);
		h->unfinished = GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES;
	}
	else
	{
		examine_measure(h);
		memset(&facts, 0, sizeof(facts));
		facts.state = h->state;
		facts.patched = h->patched;
		facts.backup_in_the_way = h->backup_in_the_way;
		facts.opence = h->opence;
		facts.up_to_date = h->up_to_date;
		facts.has_retail = h->retail != NULL;
		if (h->retail)
			memcpy(facts.retail_sha256, h->retail->retail_sha256, 32);
		facts.stock = h->stock;
		scan_cache_put(h->path, examine_key(h->path), &facts, sizeof(facts));   /* (after the recovery's changes) */
	}
	examine_describe(h);
}

static int skipped_folder(char const *name)
{
	static char const *const skip[] = {"TDATA", "UDATA", "CACHE", "$RECYCLE.BIN", "System Volume Information"};
	size_t i;
	if (name[0] == '.')
		return 1;
	for (i = 0; i < sizeof(skip) / sizeof(*skip); i++)
	{
		if (!_stricmp(name, skip[i]))
			return 1;
	}
	return 0;
}

static int looking;                          /* (find_halos is looking round the drives: shown on screen) */

static void scan_note(char const *folder)
{
	if (!looking)
		return;
	screen_row(5, COLOUR_BLACK);
	screen_text(4, 0, COLOUR_WHITE, "Looking for Halo: %d found", halo_count);
	screen_text(5, 0, COLOUR_DIM, "%.72s", folder);
	screen_show();
}

/* 1 when folder has a Halo XBE in it (as default.xbe, or default_retail.xbe); *has_main: 1 if it has a default.xbe */
static int halo_in(char const *folder, int *has_main)
{
	char path[200];
	struct xbe_head head;
	text_print(path, sizeof(path), "%s\\default.xbe", folder);
	read_head(path, &head);
	*has_main = head.present;
	if (head.halo)
		return 1;
	text_print(path, sizeof(path), "%s\\default_retail.xbe", folder);
	read_head(path, &head);
	return head.halo;
}

/* folder, if it has a Halo XBE in it (as default.xbe, or default_retail.xbe alone); 1 if it has a default.xbe */
static int add_halo(char const *folder)
{
	int i, has_main;
	if (!halo_in(folder, &has_main))
		return has_main;
	for (i = 0; i < halo_count; i++)
	{
		if (!_stricmp(halos[i].path, folder))
			return has_main;
	}
	if (halo_count == MAX_HALOS)
		return has_main;
	scan_note(folder);
	text_print(halos[halo_count].path, sizeof(halos[halo_count].path), "%s", folder);
	to_device(folder, halos[halo_count].device, sizeof(halos[halo_count].device));
	examine(&halos[halo_count]);
	halo_count++;
	return 1;
}

/* the folders in this one: each with a Halo is added; one without any XBE is looked in too, `depth` more levels
(E:\Games, E:\Games\Halo mods) */
static void scan(char const *folder, int depth)
{
	WIN32_FIND_DATAA found;
	char pattern[200], path[200];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*", folder);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !skipped_folder(found.cFileName))
		{
			text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
			if (!add_halo(path) && depth > 0)
				scan(path, depth - 1);
		}
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

static void flush_drive(char const *path);

static int is_last_install(struct halo const *h)
{
	return last_install[0] && !_stricmp(h->path, last_install);
}

/* the folder of the last install that worked, from LAST_PATH (none: empty) */
static void last_install_read(void)
{
	FILE *file = fopen(LAST_PATH, "r");
	size_t n;
	last_install[0] = 0;
	if (!file)
		return;
	if (!fgets(last_install, sizeof(last_install), file))
		last_install[0] = 0;
	fclose(file);
	n = strlen(last_install);
	while (n && (last_install[n - 1] == '\n' || last_install[n - 1] == '\r' || last_install[n - 1] == ' '))
		last_install[--n] = 0;
	/* (only a folder on a drive it looks on) */
	if (n < 3 || last_install[1] != ':' || last_install[2] != '\\')
		last_install[0] = 0;
}

/* the drive the installer is on, written out as an install's is */
static void flush_own_drive(void)
{
	char own[260], *slash, drive[260];
	own_image(own, sizeof(own));
	if ((slash = strrchr(own, '\\')) != NULL)
		*slash = 0;
	if (from_device(own, drive, sizeof(drive)))
		flush_drive(drive);
}

/* an install worked: its folder goes first in the list from now on (LAST_PATH, beside the installer, written out) */
static void last_install_write(char const *path)
{
	FILE *file = fopen(LAST_PATH, "w");
	text_print(last_install, sizeof(last_install), "%s", path);
	if (!file)
		return;
	fprintf(file, "%s\n", path);
	fclose(file);
	flush_own_drive();
}

/* the last install's folder at the very top, then the ones it can install to, then the rest (each group as found) */
static void sort_halos(void)
{
	static struct halo sorted[MAX_HALOS];
	int i, n = 0;
	for (i = 0; i < halo_count; i++)
	{
		if (is_last_install(&halos[i]))
			sorted[n++] = halos[i];
	}
	for (i = 0; i < halo_count; i++)
	{
		if (ready(&halos[i]) && !is_last_install(&halos[i]))
			sorted[n++] = halos[i];
	}
	for (i = 0; i < halo_count; i++)
	{
		if (!ready(&halos[i]) && !is_last_install(&halos[i]))
			sorted[n++] = halos[i];
	}
	memcpy(halos, sorted, sizeof(halos[0]) * halo_count);
}

static int packs_listed;                     /* (Map Packs' list is up to date: halo_changed clears it) */

/* the drives looked round for Halo folders, ONCE a run: nothing but the installer changes them while it runs, so from
then on the list is kept, and what the installer changes is looked at again by halo_changed */
static struct halo *player_install(void);

static void find_halos(void)
{
	static int looked;
	char own[260], *slash, path[260];
	unsigned long started = GetTickCount();
	int i;
	if (looked)
		return;
	looked = looking = 1;
	halo_count = 0;
	heading(NULL);
	/* the folder the installer is in, if it's in Halo's (E:\Games\Halo\OpenCE installer: E:\Games\Halo) */
	own_image(own, sizeof(own));
	for (i = 0; i < 2 && (slash = strrchr(own, '\\')); i++)
		*slash = 0;
	if (from_device(own, path, sizeof(path)))
		add_halo(path);
	/* (the last install's folder, even one deeper than the look round goes) */
	if (last_install[0])
		add_halo(last_install);
	for (i = 0; i < 3; i++)
	{
		char root[4] = {partitions[i][0], ':', 0};
		scan(root, 2);
	}
	looking = 0;
	sort_halos();
	scan_cache_save(SCAN_CACHE_PATH);
	{
		unsigned long hits, misses;
		char line[160];
		scan_cache_counts(&hits, &misses);
		text_print(line, sizeof(line), "time: looked round the drives: %d Halo folders in %lu ms (kept facts so far: %lu used, "
			"%lu looked at)", halo_count, GetTickCount() - started, hits, misses);
		log_file(line);
	}
}

/* v1.142: only what the main menu and an update need, at the start: the installer's own folder's Halo and the last
install's (each checked: still there, still Halo 1 Unlimited). The drives are looked round (find_halos) only when a
screen needs every Halo folder (Install, Map Packs, More, Developer), or when there's no install where the last one
was */
static void find_install(void)
{
	char own[260], *slash, path[260];
	unsigned long started = GetTickCount();
	int i;
	if (halo_count)
		return;
	own_image(own, sizeof(own));
	for (i = 0; i < 2 && (slash = strrchr(own, '\\')); i++)
		*slash = 0;
	if (from_device(own, path, sizeof(path)))
		add_halo(path);
	if (last_install[0])
		add_halo(last_install);
	sort_halos();
	{
		char line[160];
		text_print(line, sizeof(line), "time: the install looked at: %d Halo folders in %lu ms", halo_count,
			GetTickCount() - started);
		log_file(line);
	}
	if (!player_install())
		find_halos();
	scan_cache_save(SCAN_CACHE_PATH);
}

/* the installer has changed this Halo folder (NULL: any of them): it's looked at again, added when it's new, dropped
when it's gone; packs: Map Packs' list too (else only what changed in it is looked at again there). The drives aren't
looked round again */
static void halo_changed(char const *changed, int packs)
{
	char folder[160];
	int i, n = 0, has_main, added = 0;
	if (changed)
	{
		text_print(folder, sizeof(folder), "%s", changed);   /* (it may be a path in the list, which moves) */
		for (i = 0; i < halo_count && _stricmp(halos[i].path, folder); i++)
			;
		if (i == halo_count)
		{
			add_halo(folder);
			added = 1;
		}
	}
	for (i = 0; i < halo_count; i++)
	{
		if (!changed || !_stricmp(halos[i].path, folder))
		{
			if (!halo_in(halos[i].path, &has_main))
				continue;                  /* (gone) */
			if (!added)
			{
				examine_fresh = 1;
				examine(&halos[i]);
				examine_fresh = 0;
			}
		}
		if (n != i)
			halos[n] = halos[i];
		n++;
	}
	halo_count = n;
	sort_halos();
	if (packs)
		packs_listed = 0;
	packs_forget();
	scan_cache_save(SCAN_CACHE_PATH);
}

/* ---- installing ---- */

/* everything the drive's driver is holding back written now: the folder's changes (a file deleted, renamed) as well
as the files' contents. (FATX keeps them for a while, so a console switched off just after could lose them) */
static void flush_drive(char const *path)
{
	char device[64];
	ANSI_STRING name;
	OBJECT_ATTRIBUTES attributes;
	IO_STATUS_BLOCK status;
	HANDLE volume;
	NTSTATUS result;
	int i;
	for (i = 0; i < 3 && path[0] != partitions[i][0]; i++)
		;
	if (i == 3)
		return;
	text_print(device, sizeof(device), "\\Device\\Harddisk0\\%s", partitions[i] + 3);
	RtlInitAnsiString(&name, device);
	InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, NULL, NULL);
	result = NtOpenFile(&volume, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &status,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
	if (NT_SUCCESS(result))
	{
		result = NtFlushBuffersFile(volume, &status);
		NtClose(volume);
	}
	if (!NT_SUCCESS(result))
	{
		char text[96];
		text_print(text, sizeof(text), "(couldn't flush %s: %08lx)", device, (unsigned long)result);
		log_file(text);
	}
}

static unsigned char pictures[PICTURES_SIZE], header[HEADER_COUNT * HEADER_SIZE];

static int free_space(char const *path, unsigned long long need)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {path[0], ':', '\\', 0};
	if (!GetDiskFreeSpaceExA(root, &available, &total, &free_total))
		return 1;
	return available.QuadPart >= need;
}

/* writes data to path (through path.new, checked as written), replacing what's there */
static int write_checked(char const *path, unsigned char const *data, unsigned long size)
{
	char temp[220];
	unsigned char want[32], got[32];
	unsigned long got_size;
	struct writer w;
	text_print(temp, sizeof(temp), "%s.new", path);
	w.file = create(temp);
	if (w.file == INVALID_HANDLE_VALUE)
		return 1;
	if (write_part(&w, data, size))
	{
		CloseHandle(w.file);
		return 1;
	}
	if (!finish(w.file))
		return 1;
	sha256(data, size, want);
	if (file_sha256(temp, NULL, NULL, 0, got, &got_size) || got_size != size || memcmp(want, got, 32))
		return 1;
	DeleteFileA(path);
	return !MoveFileA(temp, path);
}

/* the old default.xbe out of the way: an XBE this didn't make is kept (default_old.xbe, default_old2.xbe...) */
static int set_aside(struct halo *h, char const *main)
{
	char old[220];
	int n;
	if (!exists(main))
		return 0;
	if (h->state != OTHER)
		return !DeleteFileA(main);
	for (n = 1; n < 100; n++)
	{
		if (n == 1)
			text_print(old, sizeof(old), "%s\\default_old.xbe", h->path);
		else
			text_print(old, sizeof(old), "%s\\default_old%d.xbe", h->path, n);
		if (!exists(old))
		{
			say("Kept the XBE that was there as %s", strrchr(old, '\\') + 1);
			return !MoveFileA(main, old);
		}
	}
	return 1;
}

/* the stock maps' showcase files the package carries, as maps\showcase\<name>.txt, the player's choices kept. The
installer remembers what it wrote (<Halo>\opence_showcase.txt: each file's name and the SHA-256 of what it wrote):
- a file it wrote that's still exactly as written: replaced by the package's when that differs (an update);
- a file it wrote that the player changed: never touched again;
- a file it wrote that the player deleted: never written again;
- a file it never wrote: written if it isn't there; one that's there (the player's own, or written before the
  installer remembered) is left be, unless it's already the package's, which it then remembers.
Not being able to write one doesn't stop anything */
#define SHOWCASE_RECORD_MAX 64
struct showcase_record
{
	char name[32];
	unsigned char sha[32];
};

static int hex_value(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int showcase_records_read(struct halo *h, struct showcase_record *records)
{
	char path[220], line[160];
	FILE *file;
	int count = 0;
	text_print(path, sizeof(path), "%s\\opence_showcase.txt", h->path);
	file = fopen(path, "r");
	if (!file)
		return 0;
	while (count < SHOWCASE_RECORD_MAX && fgets(line, sizeof(line), file))
	{
		char *space = strchr(line, ' ');
		int k, ok = 1;
		if (!space || space - line >= 32 || strlen(space + 1) < 64)
			continue;
		*space = 0;
		for (k = 0; k < 32 && ok; k++)
		{
			int high = hex_value(space[1 + 2 * k]), low = hex_value(space[2 + 2 * k]);
			ok = high >= 0 && low >= 0;
			records[count].sha[k] = (unsigned char)(high << 4 | low);
		}
		if (!ok)
			continue;
		text_print(records[count].name, sizeof(records[count].name), "%s", line);
		count++;
	}
	fclose(file);
	return count;
}

static void showcase_records_write(struct halo *h, struct showcase_record const *records, int count)
{
	char path[220];
	FILE *file;
	int i, k;
	text_print(path, sizeof(path), "%s\\opence_showcase.txt", h->path);
	file = fopen(path, "w");
	if (!file)
	{
		say_in(COLOUR_YELLOW, "  couldn't write opence_showcase.txt: the showcase files won't be updated");
		return;
	}
	for (i = 0; i < count; i++)
	{
		fprintf(file, "%s ", records[i].name);
		for (k = 0; k < 32; k++)
			fprintf(file, "%02x", records[i].sha[k]);
		fprintf(file, "\n");
	}
	fclose(file);
}

static void install_showcase(struct halo *h)
{
	static struct showcase_record records[SHOWCASE_RECORD_MAX];
	char path[220], name[32];
	unsigned char const *data;
	unsigned char want[32], got[32];
	unsigned long n, size, got_size, wrote = 0, updated = 0;
	int count, i, changed = 0;
	if (!package.showcase_count)
		return;
	count = showcase_records_read(h, records);
	text_print(path, sizeof(path), "%s\\maps\\showcase", h->path);
	CreateDirectoryA(path, NULL);
	for (n = 0; n < package.showcase_count; n++)
	{
		struct showcase_record *record = NULL;
		int there, same_as_package;
		package_showcase_file(&package, n, name, &data, &size);
		sha256(data, size, want);
		for (i = 0; i < count; i++)
			if (!_stricmp(records[i].name, name))
				record = &records[i];
		text_print(path, sizeof(path), "%s\\maps\\showcase\\%s", h->path, name);
		there = exists(path) && !file_sha256(path, NULL, NULL, 0, got, &got_size);
		same_as_package = there && got_size == size && !memcmp(got, want, 32);
		if (record)
		{
			/* (deleted, changed by the player, or already this package's: left be) */
			if (!there || memcmp(got, record->sha, 32) || same_as_package)
				continue;
			if (write_checked(path, data, size))
				say_in(COLOUR_YELLOW, "  couldn't update maps\\showcase\\%s", name);
			else
			{
				memcpy(record->sha, want, 32);
				updated++;
				changed = 1;
			}
			continue;
		}
		if (count == SHOWCASE_RECORD_MAX)
			continue;
		if (there && !same_as_package)
			continue;                  /* (the player's own) */
		if (!there && write_checked(path, data, size))
		{
			say_in(COLOUR_YELLOW, "  couldn't write maps\\showcase\\%s", name);
			continue;
		}
		if (!there)
			wrote++;
		text_print(records[count].name, sizeof(records[count].name), "%s", name);
		memcpy(records[count].sha, want, 32);
		count++;
		changed = 1;
	}
	if (changed)
		showcase_records_write(h, records, count);
	if (wrote || updated)
		say("Stock maps' showcase files (maps\\showcase): %lu added, %lu updated (ones you changed or deleted are kept as "
			"you left them)", wrote, updated);
}

/* what the game reads of what's installed (<Halo>\opence_release.txt: the package's release and network version, for
its look for a newer release), and this installer's copy as the folder's updater (<Halo>\updater\default.xbe), which
the game starts to update it. Neither is needed to play: a failure is only said */
static void install_updater(struct halo *h)
{
	char path[220], own[260], own_path[260];
	FILE *file;
	unsigned char *data;
	unsigned long size;

	text_print(path, sizeof(path), "%s\\opence_release.txt", h->path);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "release %lu\nnetwork %lu\n%s\n", package.release, package.network, package.name);
		fclose(file);
	}
	else
		say_in(COLOUR_YELLOW, "  (couldn't write opence_release.txt: the game won't look for updates)");
	own_image(own, sizeof(own));
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER "\\default.xbe", h->path);
	if (!from_device(own, own_path, sizeof(own_path)))
	{
		say_in(COLOUR_YELLOW, "  (this installer isn't on the hard drive: no updater copied)");
		return;
	}
	if (!_stricmp(own_path, path))
		return;                        /* (this is that folder's updater) */
	data = read_whole(own_path, &size);
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER, h->path);
	CreateDirectoryA(path, NULL);
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER "\\default.xbe", h->path);
	if (!data || write_checked(path, data, size))
		say_in(COLOUR_YELLOW, "  (couldn't copy the installer to updater\\default.xbe: the game can't update itself)");
	else
		say("Copied this installer to updater\\default.xbe (the game's updater)");
	free(data);
}

/* the game's own files and folders in a title's storage (E:\TDATA\<title>) */
static char const *const own_files[] = {"hmp_stats.txt", "hmp_good_maps.txt", "hmp_bad_maps.txt", "mapshare.txt",
	"map_check.txt", "opence.ini", "opence_settings.txt", "maps", "perf"};

/* the game's own storage (E:\TDATA\4c4a4855): its own files (map stats and favourites, map lists, T:\maps, its
settings) moved there from where earlier builds kept them, Halo's storage (4d530004: release 1 and the test builds
before v1.53) and v1.53's (4f430002), when they aren't there yet. Moved, never copied: a rename on the same drive,
which takes no room. Nothing of the player's saves is copied or touched: the game's save folder starts empty, its own.
('OC' 0001, which the first test builds used, is Open Crossing's: never touched here.) */
static void own_title_folders(void)
{
	static char const *const earlier[] = {V153_TITLE_FOLDER, HALO_TITLE_FOLDER};
	char from[200], to[200];
	int moved = 0, failed = 0;
	unsigned i, k;

	for (k = 0; k < 2; k++)
	{
		for (i = 0; i < sizeof(own_files) / sizeof(own_files[0]); i++)
		{
			text_print(from, sizeof(from), "E:\\TDATA\\%s\\%s", earlier[k], own_files[i]);
			text_print(to, sizeof(to), "E:\\TDATA\\" OWN_TITLE_FOLDER "\\%s", own_files[i]);
			if (!exists(from) || exists(to))
				continue;
			CreateDirectoryA("E:\\TDATA", NULL);
			CreateDirectoryA("E:\\TDATA\\" OWN_TITLE_FOLDER, NULL);
			if (MoveFileA(from, to))
				moved++;
			else
				failed++;
		}
	}
	/* (v1.53's storage, if that emptied it) */
	RemoveDirectoryA("E:\\TDATA\\" V153_TITLE_FOLDER);
	if (moved || failed)
	{
		say("Moved %d of its own files (map stats, map lists, settings) to its own storage, E:\\TDATA\\" OWN_TITLE_FOLDER,
			moved);
		if (failed)
			say_in(COLOUR_YELLOW, "  (%d couldn't be moved)", failed);
		flush_drive("E:\\");
	}
}

/* map sharing's KEEP DOWNLOADS UNDER (the game's OPTIONS), set once, when the game is installed on E:, where room is
precious: the biggest of 2 GB, 1 GB, 600 MB and 300 MB under half the room free there (else 300 MB), as downloads=<MB>
in the game's settings. Never changed once the settings have one (the player's choice, or an earlier install's). On
another drive: no limit, which the game takes when there's none */
static void downloads_default(struct halo const *h)
{
	static unsigned long const sizes[4] = {2048, 1024, 600, 300};
	char const *path = "E:\\TDATA\\" OWN_TITLE_FOLDER "\\opence_settings.txt";
	char text[600], line[40];
	ULARGE_INTEGER available, total, free_total;
	unsigned long free_mb, got = 0, put = 0;
	HANDLE file;
	int k;

	if ((h->path[0] | 0x20) != 'e' || !GetDiskFreeSpaceExA("E:\\", &available, &total, &free_total))
		return;
	file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE)
	{
		if (!ReadFile(file, text, sizeof(text) - 1, &got, NULL))
			got = 0;
		CloseHandle(file);
	}
	text[got] = 0;
	if (strstr(text, "downloads="))
		return;
	free_mb = (unsigned long)(available.QuadPart >> 20);
	for (k = 0; k < 3 && sizes[k] > free_mb / 2; k++)
		;
	CreateDirectoryA("E:\\TDATA", NULL);
	CreateDirectoryA("E:\\TDATA\\" OWN_TITLE_FOLDER, NULL);
	file = CreateFileA(path, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return;
	SetFilePointer(file, 0, NULL, FILE_END);
	text_print(line, sizeof(line), "%sdownloads=%lu\r\n", got && text[got - 1] != '\n' ? "\r\n" : "", sizes[k]);
	WriteFile(file, line, (unsigned long)strlen(line), &put, NULL);
	CloseHandle(file);
	say("Downloaded maps kept under %lu MB (%lu MB free on E:; OPTIONS in the game changes it).", sizes[k], free_mb);
	flush_drive("E:\\");
}

/* ---- the pictures kept (installer 65): what pictures_make made from an install's ui.map, kept in its updater folder
(updater\pictures.bin), so an update doesn't read ui.map and make them again. Used only when made by the same picture
code (pictures_code) from the same ui.map (its size, write time and first 2048 bytes), and its own SHA-256 reads
right; else they're made again, and kept. (The player's own art, as the XBE beside it is: it never leaves the Xbox) ---- */

#define KEPT_PICTURES_MAGIC 0x4350584Ful   /* "OXPC" */
#define KEPT_PICTURES_SIZE (PICTURES_SIZE + HEADER_COUNT * HEADER_SIZE)

struct kept_pictures
{
	unsigned long magic, code, pictures_size, header_size, ui_size, ui_time_low, ui_time_high;
	unsigned char ui_head[32];
	unsigned char sha[32];            /* (of what follows: the pictures, then the headers) */
};

/* what the kept pictures must have been made from; 0 when it reads (1: none can be used) */
static int kept_pictures_key(char const *ui_map, struct kept_pictures *key)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	unsigned char head[2048];
	FILE *file;
	int got;
	memset(key, 0, sizeof(*key));
	if (!pictures_code || !GetFileAttributesExA(ui_map, GetFileExInfoStandard, &data) || data.nFileSizeHigh ||
		!(file = fopen(ui_map, "rb")))
		return 1;
	got = fread(head, 1, sizeof(head), file) == sizeof(head);
	fclose(file);
	if (!got)
		return 1;
	key->magic = KEPT_PICTURES_MAGIC;
	key->code = pictures_code;
	key->pictures_size = PICTURES_SIZE;
	key->header_size = HEADER_COUNT * HEADER_SIZE;
	key->ui_size = data.nFileSizeLow;
	key->ui_time_low = data.ftLastWriteTime.dwLowDateTime;
	key->ui_time_high = data.ftLastWriteTime.dwHighDateTime;
	sha256(head, sizeof(head), key->ui_head);
	return 0;
}

/* the kept pictures into pictures[] and header[]; 0 when they were there and right */
static int kept_pictures_read(struct halo *h, struct kept_pictures const *key)
{
	char path[220];
	unsigned char *data, sha[32];
	unsigned long size;
	int right;
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER "\\pictures.bin", h->path);
	if (!(data = read_whole(path, &size)))
		return 1;
	right = size == sizeof(*key) + KEPT_PICTURES_SIZE && !memcmp(data, key, offsetof(struct kept_pictures, sha));
	if (right)
	{
		sha256(data + sizeof(*key), KEPT_PICTURES_SIZE, sha);
		right = !memcmp(sha, ((struct kept_pictures const *)data)->sha, 32);
	}
	if (right)
	{
		memcpy(pictures, data + sizeof(*key), PICTURES_SIZE);
		memcpy(header, data + sizeof(*key) + PICTURES_SIZE, HEADER_COUNT * HEADER_SIZE);
	}
	free(data);
	return !right;
}

/* pictures[] and header[] kept for the next time (nothing said if it can't: they're made again then) */
static void kept_pictures_write(struct halo *h, struct kept_pictures const *key)
{
	char path[220];
	unsigned char *data = malloc(sizeof(*key) + KEPT_PICTURES_SIZE);
	struct kept_pictures *kept = (struct kept_pictures *)data;
	if (!data)
		return;
	*kept = *key;
	memcpy(data + sizeof(*key), pictures, PICTURES_SIZE);
	memcpy(data + sizeof(*key) + PICTURES_SIZE, header, HEADER_COUNT * HEADER_SIZE);
	sha256(data + sizeof(*key), KEPT_PICTURES_SIZE, kept->sha);
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER, h->path);
	CreateDirectoryA(path, NULL);
	text_print(path, sizeof(path), "%s\\" UPDATER_FOLDER "\\pictures.bin", h->path);
	log_file(write_checked(path, data, sizeof(*key) + KEPT_PICTURES_SIZE) ? "pictures: couldn't keep them in updater\\pictures.bin" :
		"pictures: kept in updater\\pictures.bin");
	free(data);
}

static int install(struct halo *h)
{
	char main[200], backup[200], temp[220], ui_map[200], error[256], line[SCREEN_COLUMNS + 8];
	unsigned long zero_at[GENERATED_REGIONS], zero_size[GENERATED_REGIONS], made_size;
	unsigned char digest[32];
	int zeros, have_pictures, kept, failed;
	struct retail r;
	struct gatherer g;
	struct kept_pictures key;
	unsigned long started = GetTickCount(), retail_ms, pictures_ms, written_ms, checked_ms;

	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	text_print(temp, sizeof(temp), "%s\\default.xbe.new", h->path);
	text_print(ui_map, sizeof(ui_map), "%s\\maps\\ui.map", h->path);
	text_print(line, sizeof(line), "Installing OpenCE in %s", h->path);
	heading(line);
	log_clear();
	{
		unsigned long colour;
		char const *said = signature_text(&colour);
		say("Installing %s", package.name);
		if (!quiet)
			say_in(colour, "  (release %lu, %s)", package.release, said);
		else
			log_file(said);
		say("in %s", h->path);
	}
	/* the retail XBE, checked again */
	if (retail_read(h->state == RETAIL ? main : backup, &r) || r.xbe != h->retail)
	{
		say_in(COLOUR_RED, "The retail XBE couldn't be read, or changed: look again.");
		retail_free(&r);
		return 1;
	}
	if (!free_space(h->path, (unsigned long long)h->retail->build_size + r.size + 1024 * 1024))
	{
		say_in(COLOUR_RED, "Not enough room on the drive (%lu KB needed).", (h->retail->build_size + r.size) / 1024 + 1024);
		retail_free(&r);
		return 1;
	}
	retail_ms = GetTickCount() - started;
	/* the pictures, from the game's own ui.map: the ones made last time when they're right (installer 65) */
	kept = !kept_pictures_key(ui_map, &key) && !kept_pictures_read(h, &key);
	if (kept)
	{
		say("Menu pictures: the ones made last time (updater\\pictures.bin)");
		have_pictures = 1;
	}
	else
	{
		say("Making the pictures from maps\\ui.map:");
		pictures_progress = pictures_step;
		have_pictures = !pictures_make(ui_map, pictures, header, error);
		if (!have_pictures)
			say_in(COLOUR_YELLOW, "  no pictures (%s): they'll be blank", error);
		else if (key.magic)
			kept_pictures_write(h, &key);
	}
	pictures_ms = GetTickCount() - started;
	/* the build, beside the old XBE (from the disc's bytes) */
	say("Making OpenCE from %s%s...", h->retail->name, r.patch.patched ? " (region/media patched)" : "");
	g.held = 0;
	g.buffer = malloc(GATHER_SIZE);
	g.w.file = g.buffer ? create(temp) : INVALID_HANDLE_VALUE;
	if (g.w.file == INVALID_HANDLE_VALUE)
	{
		free(g.buffer);
		say_in(COLOUR_RED, "Couldn't create default.xbe.new.");
		retail_free(&r);
		return 1;
	}
	failed = package_make(&package, h->retail, r.data, have_pictures ? pictures : NULL, have_pictures ? header : NULL,
		gather_part, &g, error);
	if (!failed && gather_flush(&g))
	{
		failed = 1;
		text_print(error, sizeof(error), "couldn't write the new XBE");
	}
	free(g.buffer);
	if (failed)
	{
		CloseHandle(g.w.file);
		DeleteFileA(temp);
		say_in(COLOUR_RED, "%s", error);
		retail_free(&r);
		return 1;
	}
	if (!finish(g.w.file))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "Couldn't finish writing default.xbe.new.");
		retail_free(&r);
		return 1;
	}
	written_ms = GetTickCount() - started;
	zeros = package_pictures(&package, h->retail, zero_at, zero_size, GENERATED_REGIONS);
	if (file_sha256(temp, zero_at, zero_size, zeros, digest, &made_size) || made_size != h->retail->build_size ||
		memcmp(digest, h->retail->build_sha256, 32))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "default.xbe.new didn't read back right: nothing changed.");
		retail_free(&r);
		return 1;
	}
	say("  written and checked (%lu KB)", made_size / 1024);
	checked_ms = GetTickCount() - started;
	/* the retail XBE kept, as it was */
	if (h->state == RETAIL && !exists(backup))
	{
		retail_original(&r);
		if (write_checked(backup, r.data, r.size))
		{
			DeleteFileA(temp);
			say_in(COLOUR_RED, "Couldn't keep the retail XBE: nothing changed.");
			retail_free(&r);
			return 1;
		}
		say("Kept the retail XBE as default_retail.xbe");
	}
	retail_free(&r);
	/* and the new one in its place */
	if (set_aside(h, main) || !MoveFileA(temp, main))
	{
		say_in(COLOUR_RED, "Couldn't put default.xbe.new in place of default.xbe.");
		say("(The retail XBE is safe in default_retail.xbe.)");
		return 1;
	}
	install_showcase(h);
	install_updater(h);
	own_title_folders();
	downloads_default(h);
	{
		char times[160];
		unsigned long all_ms = GetTickCount() - started;
		text_print(times, sizeof(times), "time: installed in %lu ms: retail XBE read %lu, pictures %lu (%s), XBE written %lu, "
			"checked %lu, the rest %lu", all_ms, retail_ms, pictures_ms - retail_ms, kept ? "kept" : "made",
			written_ms - pictures_ms, checked_ms - written_ms, all_ms - checked_ms);
		log_file(times);
	}
	say_in(COLOUR_GREEN, "Done: OpenCE is this folder's default.xbe.");
	return 0;
}

/* ---- the stock maps (stockmaps.c): the package is let go while it works, for its 17 MB of a map's tags and more ---- */

static void package_read(void);

static void stock_maps_say(int warning, char const *text)
{
	say_in(warning ? COLOUR_YELLOW : COLOUR_WHITE, "%s", text);
}

/* (for the log: what it has to work with) */
static void log_memory(void)
{
	MM_STATISTICS statistics;
	char line[64];
	statistics.Length = sizeof(statistics);
	if (NT_SUCCESS(MmQueryStatistics(&statistics)))
	{
		text_print(line, sizeof(line), "(memory free: %lu KB)", statistics.AvailablePages * 4);
		log_file(line);
	}
}

/* ---- the cache drives X, Y and Z (partitions 3-5): the stock maps swap makes its small maps there (stockmaps.h). They're
the installer's to use while it runs (nothing else can run then): a game that finds its cache gone makes it again ---- */

static char const *const cache_letters[STOCKMAPS_CACHES] = {"X:", "Y:", "Z:"};

static unsigned long long cache_free(int i)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {cache_letters[i][0], ':', '\\', 0};
	return GetDiskFreeSpaceExA(root, &available, &total, &free_total) ? available.QuadPart : 0;
}

/* mounted, and those whose room reads (formatted) given to the swap; what a swap left there goes */
static void cache_drives(void)
{
	char line[160];
	int i, n = 0;
	n += text_print(line, sizeof(line), "cache drives:");
	for (i = 0; i < STOCKMAPS_CACHES; i++)
	{
		char device[40];
		ULARGE_INTEGER available, total, free_total;
		char root[4] = {cache_letters[i][0], ':', '\\', 0};
		text_print(device, sizeof(device), "\\Device\\Harddisk0\\Partition%d\\", 3 + i);
		if (nxMountDrive(cache_letters[i][0], device) && GetDiskFreeSpaceExA(root, &available, &total, &free_total))
		{
			stockmaps_cache[i] = cache_letters[i];
			n += text_print(line + n, sizeof(line) - (size_t)n, " %s %lu of %lu MB free", cache_letters[i],
				(unsigned long)(available.QuadPart >> 20), (unsigned long)(total.QuadPart >> 20));
		}
		else
			n += text_print(line + n, sizeof(line) - (size_t)n, " %s not there", cache_letters[i]);
	}
	log_file(line);
	stockmaps_cache_clear();
}

/* everything in a folder deleted, folders and all (to a depth of 16) */
static void delete_tree(char const *folder, int depth)
{
	WIN32_FIND_DATAA found;
	char pattern[240], path[240];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*", folder);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, ".."))
			continue;
		text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
		SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			if (depth < 16)
			{
				delete_tree(path, depth + 1);
				RemoveDirectoryA(path);
			}
		}
		else
			DeleteFileA(path);
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

/* room on the cache drives for the swap's small maps: if they haven't enough between them, they're emptied, the fullest
first, until they have (without it the swap reads each retail map twice: it takes longer, but works the same) */
static void cache_make_room(char const *halo)
{
	unsigned long long need = stockmaps_cache_need(halo), have = 0;
	int i, emptied[STOCKMAPS_CACHES] = {0};
	char line[160];
	if (!need)
	{
		log_file("cache drives: not needed (the small maps are made in the maps folder: its drive has the room)");
		return;
	}
	for (;;)
	{
		unsigned long long least = ~0ull;
		int fullest = -1;
		have = 0;
		for (i = 0; i < STOCKMAPS_CACHES; i++)
		{
			unsigned long long room;
			if (!stockmaps_cache[i])
				continue;
			room = cache_free(i);
			have += room;
			if (!emptied[i] && room < least)
				least = room, fullest = i;
		}
		if (have >= need || fullest < 0)
			break;
		text_print(line, sizeof(line), "cache drives: %lu MB free, the swap's small maps need %lu: emptying %s",
			(unsigned long)(have >> 20), (unsigned long)(need >> 20), cache_letters[fullest]);
		log_file(line);
		delete_tree(cache_letters[fullest], 0);
		emptied[fullest] = 1;
	}
	text_print(line, sizeof(line), "cache drives: %lu MB free for the swap's small maps (they need %lu)", (unsigned long)(have >> 20),
		(unsigned long)(need >> 20));
	log_file(line);
}

/* how long the spinner spent drawing since `since` (screen_spinner_busy_ms), for the log after a swap or restore's time lines */
static void log_spinner(unsigned long since)
{
	char line[96];
	unsigned long ms = screen_spinner_busy_ms() - since;
	text_print(line, sizeof(line), "time: the spinner drew for %lu.%lu s of it", ms / 1000u, ms / 100u % 10u);
	log_file(line);
}

static int stock_maps_restore(struct halo *h)
{
	char error[256];
	int failed, b;
	unsigned long spun;
	say("This puts the retail maps back too: it takes about half an hour, and needs room for them (about 1.2 GB free).");
	say_in(COLOUR_GREY, "(A map pack that draws on stock.shared needs it: installing here again remakes it.)");
	say_in(COLOUR_YELLOW, "A: go on   B: leave everything as it is");
	while ((b = wait_input()) != A && b != B)
		;
	if (b == B)
	{
		say("Nothing changed.");
		return 1;
	}
	stockmaps_flush_drive = flush_drive;
	stockmaps_log = log_file;
	package_free(&package);
	log_memory();
	spun = screen_spinner_busy_ms();
	failed = stockmaps_restore(h->path, stock_maps_say, error);
	log_spinner(spun);
	package_read();
	if (failed)
	{
		say_in(COLOUR_RED, "%s", error);
		say("The retail XBE wasn't put back: this folder still plays Halo 1 Unlimited.");
		return 1;
	}
	say("  maps back, each checked against the original's size and CRC");
	return 0;
}

/* after an install, by hand (the game's updater never asks): the retail stock maps swapped for small ones, if the
player wants. A failure changes nothing: the retail maps play as they did */
static void offer_stock_maps(struct halo *h)
{
	char error[256];
	int b, failed;
	unsigned long spun;
	stockmaps_examine(h->path, &h->stock);
	/* (all 23 there, each retail or small: some small is a swap that stopped, to go on with) */
	if (h->stock.small == STOCK_MAPS || h->stock.small + h->stock.retail_sized != STOCK_MAPS)
		return;
	if (h->stock.small)
		say("The maps here were being made small: %d of %d are done. Finish it? It saves more room as it goes.", h->stock.small,
			STOCK_MAPS);
	else
	{
		say("Save about 800 MB? The 23 multiplayer and campaign maps can share one file, stock.shared, for their textures and "
			"sounds.");
		say("It takes about five minutes, and needs about 350 MB free while it works.");
	}
	say("Retail Halo can't play them that way: X in this installer puts the exact originals back.");
	if (quiet)
		b = A;                         /* (a player's install: always, with no question) */
	else
	{
		say_in(COLOUR_YELLOW, "A: do it   B: not now");
		while ((b = wait_input()) != A && b != B)
			;
	}
	if (b == B)
		return;
	stockmaps_flush_drive = flush_drive;
	stockmaps_log = log_file;
	package_free(&package);
	log_memory();
	cache_make_room(h->path);
	spun = screen_spinner_busy_ms();
	failed = stockmaps_convert(h->path, stock_maps_say, error);
	log_spinner(spun);
	if (failed)
		say_in(COLOUR_YELLOW, "%s", error);
	else
		say_in(COLOUR_GREEN, "Done: the maps are small now (stock.shared holds what they share).");
	package_read();
}

static int restore(struct halo *h)
{
	char main[200], backup[200], line[SCREEN_COLUMNS + 8];
	struct retail r;
	text_print(main, sizeof(main), "%s\\default.xbe", h->path);
	text_print(backup, sizeof(backup), "%s\\default_retail.xbe", h->path);
	text_print(line, sizeof(line), "Putting the retail Halo back in %s", h->path);
	heading(line);
	log_clear();
	say("Putting the retail Halo back in %s", h->path);
	/* the stock maps first: if they can't come back, the game that plays them stays */
	if (stock_maps_back(h) && stock_maps_restore(h))
		return 1;
	if (h->stock.small && !h->stock.restorable)
		say_in(COLOUR_YELLOW, "(The stock maps draw on stock.shared, with no restore table: they stay as they are.)");
	if (h->state != INSTALLED && h->state != OTHER && h->state != MISSING)
	{
		say_in(COLOUR_GREEN, "Done: the retail stock maps are back.");
		return 0;
	}
	if (retail_read(backup, &r) || r.xbe != h->retail)
	{
		say_in(COLOUR_RED, "default_retail.xbe couldn't be read, or changed: look again.");
		retail_free(&r);
		return 1;
	}
	retail_original(&r);
	if (h->state == OTHER && set_aside(h, main))
	{
		say_in(COLOUR_RED, "Couldn't move the XBE that's there out of the way.");
		retail_free(&r);
		return 1;
	}
	if (write_checked(main, r.data, r.size))
	{
		say_in(COLOUR_RED, "Couldn't write default.xbe: try again.");
		retail_free(&r);
		return 1;
	}
	retail_free(&r);
	say_in(COLOUR_GREEN, "Done: the retail Halo is back (default_retail.xbe is kept).");
	return 0;
}

static void launch(char const *device)
{
	screen_spinner(0);
	screen_hand_over();
	oxbox_quiesce_devices();
	XLaunchXBE(device);
	/* (only if the path didn't convert) */
	screen_init();
	heading("Couldn't start it.");
}

static void launch_halo(struct halo *h)
{
	char device[240], own[260], note[260];
	text_print(device, sizeof(device), "%s\\default.xbe", h->device);
	/* (installer 73: the game's OPTIONS > INSTALLER starts the installer that last started it, this one, which has its
	package beside it; never the install's own updater copy, which hasn't, and can't tell the install from another) */
	own_image(own, sizeof(own));
	text_print(note, sizeof(note), "%s\\" UPDATER_FOLDER "\\", h->device);
	if (own[0] && _strnicmp(own, note, strlen(note)))
	{
		text_print(note, sizeof(note), "%s\\" UPDATER_FOLDER "\\installer.txt", h->path);
		log_file(write_checked(note, (unsigned char const *)own, (unsigned long)strlen(own)) ?
			"couldn't write updater\\installer.txt (the game's way back to this installer)" :
			"updater\\installer.txt: this installer (the game's OPTIONS > INSTALLER starts it)");
	}
	log_file("starting");
	log_file(device);
	launch(device);
}

/* ---- the package, read (at the start, and after a download) ---- */

static void package_read(void)
{
	unsigned long colour;
	package_free(&package);
	package_ok = !package_load(PACKAGE_PATH, &package, package_error);
	package_missing = !package_ok && GetFileAttributesA(PACKAGE_PATH) == INVALID_FILE_ATTRIBUTES;
	signature = PACKAGE_UNSIGNED;
	signer = NULL;
	log_file(package_ok ? package.name : package_error);
	if (!package_ok)
		return;
	signature = package_verify(&package, trusted_keys, trusted_key_count, &signer);
	if (signature == PACKAGE_BAD)
	{
		package_ok = 0;
		text_print(package_error, sizeof(package_error),
			"opence.oxpk was changed after it was signed: it can't be installed.");
	}
	log_file(package_ok ? signature_text(&colour) : package_error);
	if (package_ok && package.installer_version)
	{
		char line[96];
		text_print(line, sizeof(line), "(it carries installer version %lu)", package.installer_version);
		log_file(line);
	}
}

/* THIRD_PARTY.txt beside the installer: the notices of the code in it (notices.h, from the THIRD_PARTY.txt it was
built with), written when what's there differs, as after the installer updated itself */
static void notices_write(void)
{
	unsigned long size;
	unsigned char *there = read_whole(NOTICES_PATH, &size);
	int same = there && size == sizeof(notices) && !memcmp(there, notices, size);
	free(there);
	if (!same)
	{
		HANDLE file = create(NOTICES_PATH);
		DWORD put;
		if (file != INVALID_HANDLE_VALUE)
		{
			int ok = WriteFile(file, notices, sizeof(notices), &put, NULL) && put == sizeof(notices);
			log_file(finish(file) && ok ? "wrote THIRD_PARTY.txt" : "(couldn't write THIRD_PARTY.txt)");
		}
	}
}

/* ---- updates: the installer itself, from a package that carries a newer one ---- */

static void update_installer(void)
{
	char own[260], name[64], path[80], temp[96], *slash;
	unsigned char want[32], got[32];
	unsigned long got_size;
	struct writer w;

	own_image(own, sizeof(own));
	slash = strrchr(own, '\\');
	text_print(name, sizeof(name), "%s", slash ? slash + 1 : own);
	text_print(path, sizeof(path), "%c:\\%s", own_drive, name);
	text_print(temp, sizeof(temp), "%c:\\%s.new", own_drive, name);
	heading("Updating the installer");
	log_clear();
	say("Writing installer version %lu beside this one (%s)...", package.installer_version, name);
	w.file = create(temp);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		say_in(COLOUR_RED, "Couldn't write beside the installer: nothing changed.");
		return;
	}
	if (write_part(&w, package.body + package.installer_offset, package.installer_size))
	{
		CloseHandle(w.file);
		DeleteFileA(temp);
		say_in(COLOUR_RED, "Couldn't write the new installer (is the drive full?): nothing changed.");
		return;
	}
	sha256(package.body + package.installer_offset, package.installer_size, want);
	if (!finish(w.file) || file_sha256(temp, NULL, NULL, 0, got, &got_size) || got_size != package.installer_size ||
		memcmp(want, got, 32))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "The new installer didn't read back right: nothing changed.");
		return;
	}
	say("  written and checked (%lu KB)", got_size / 1024);
	/* this one kept as installer_old.xbe, the new one in its place */
	DeleteFileA(OLD_INSTALLER_PATH);
	if (!MoveFileA(path, OLD_INSTALLER_PATH))
	{
		DeleteFileA(temp);
		say_in(COLOUR_RED, "Couldn't move this installer aside: nothing changed.");
		return;
	}
	if (!MoveFileA(temp, path))
	{
		MoveFileA(OLD_INSTALLER_PATH, path);
		DeleteFileA(temp);
		say_in(COLOUR_RED, "Couldn't put the new installer in place: this one is kept.");
		return;
	}
	flush_own_drive();
	say_in(COLOUR_GREEN, "Done: the installer is version %lu (this one is kept as installer_old.xbe).",
		package.installer_version);
	say("Starting it...");
	Sleep(1500);
	log_file("starting the new installer");
	launch(own);
}

/* when the package (signed by a key it trusts) carries a newer installer: asks, then updates */
static void offer_installer_update(void)
{
	int b;
	if (!package_ok || package.installer_version <= INSTALLER_VERSION)
		return;
	if (!trusted(signature))
	{
		log_file("(the package's installer isn't used: the package isn't signed by a key this installer trusts)");
		return;
	}
	heading("A newer installer");
	log_clear();
	say("opence.oxpk carries installer version %lu. This installer is version %d.", package.installer_version,
		INSTALLER_VERSION);
	say("Updating puts it in place of this one (kept as installer_old.xbe), then starts it.");
	if (auto_mode)
		b = A;
	else
	{
		say_in(COLOUR_YELLOW, "A: update the installer now   B: not now");
		while ((b = wait_input()) != A && b != B)
			;
	}
	if (b == A)
		update_installer();
	else
		log_file("installer update: not now");
}

/* ---- updates: a newer package, downloaded (download.c) ---- */

static void update_step(void *context, char const *text)
{
	(void)context;
	say("%s", text);
}

static void update_progress(void *context, unsigned long done, unsigned long total)
{
	(void)context;
	if (quiet)
	{
		struct receipt_entry *e = receipt_running();
		if (!e)
		{
			e = receipt_add(RECEIPT_ACTION);
			text_print(e->text, sizeof(e->text), "Downloading...");
		}
		if (total)
		{
			/* (a bar on the row's left, the MB on its right) */
			int bar = (int)((unsigned long long)done * 30 / total), i;
			for (i = 0; i < 30; i++)
				e->text[i] = i < bar ? '#' : '.';
			e->text[30] = 0;
			/* (tenths of a MB for a small one: a 3 MB update isn't "0 of 3" for a while) */
			if (total < (100ul << 20))
				text_print(e->right, sizeof(e->right), "%lu.%lu of %lu.%lu MB", done >> 20, (done % (1ul << 20)) * 10 >> 20,
					total >> 20, (total % (1ul << 20)) * 10 >> 20);
			else
				text_print(e->right, sizeof(e->right), "%lu of %lu MB", (done + 524288) >> 20, (total + 524288) >> 20);
		}
		else
			text_print(e->right, sizeof(e->right), "%lu MB", (done + 524288) >> 20);
		receipt_show();
		return;
	}
	screen_row(4, COLOUR_BLACK);
	if (total)
	{
		int bar = (int)((unsigned long long)done * 40 / total), i;
		char line[41];
		for (i = 0; i < 40; i++)
			line[i] = i < bar ? '#' : '.';
		line[40] = 0;
		screen_text(4, 0, COLOUR_WHITE, "[%s] %lu of %lu KB", line, done / 1024, total / 1024);
	}
	else
		screen_text(4, 0, COLOUR_WHITE, "%lu KB", done / 1024);
	screen_show();
}

/* one place's update_url.txt: 1 with its address in url (its first http:// or https:// line), allow_unsigned noted */
static int url_file(char const *path, char *url, int size)
{
	FILE *file = fopen(path, "r");
	char line[512];
	int found = 0;
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file))
	{
		char *at = line;
		size_t n = strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t'))
			line[--n] = 0;
		while (*at == ' ' || *at == '\t')
			at++;
		if (!found && (!_strnicmp(at, "https://", 8) || !_strnicmp(at, "http://", 7)))
		{
			text_print(url, size, "%s", at);
			found = 1;
		}
		else if (!_stricmp(at, "allow_unsigned"))
			test_unsigned = 1;
	}
	fclose(file);
	return found;
}

/* where updates come from: update_url.txt beside the installer (or installer_url.txt, its earlier name), or, for the
game's updater, the Halo folder's; else the project's latest release */
static void update_url(char *url, int size)
{
	char path[300];
	test_unsigned = 0;
	if (developer && developer_url[0])
	{
		/* (developer.ini's test location) */
		text_print(url, size, "%s", developer_url);
		test_unsigned = developer_unsigned;
		return;
	}
	if (url_file(URL_PATH, url, size) || url_file(OLD_URL_PATH, url, size))
		return;
	test_unsigned = 0;
	if (updater_halo[0])
	{
		text_print(path, sizeof(path), "%s\\update_url.txt", updater_halo);
		if (url_file(path, url, size))
			return;
	}
	test_unsigned = 0;
	text_print(url, size, "%s", DOWNLOAD_DEFAULT_URL);
}

/* ---- the update check at start: in the background, the newest package's first 128 bytes, its header (its release at
116, its build at 124), as the game's own check takes them (net/oxbox_update.c); downloaded whole only when Update is
chosen (check_updates). The network isn't used by two at once: check_wait first ---- */

static volatile unsigned long check_release, check_build;
static unsigned char check_head[128];

/* a package newer than the one beside the installer (none: any is). A test build (release 0) is replaced only by a
newer test build, from a test location: a release is older code, as the game's own check holds (it doesn't look at
all in a test build) */
static int newer_package(unsigned long release, unsigned long build)
{
	if (!package_ok)
		return 1;
	if (!package.release && release)
		return 0;
	return release > package.release || (release == package.release && build > package.build);
}

/* the version the check found, as said to a player: "v1.126", or a release's name ("Halo 1 Unlimited, release 2") */
static char const *check_version(void)
{
	static char text[72];
	if (check_build)
		text_print(text, sizeof(text), "v1.%lu", check_build);
	else
		text_print(text, sizeof(text), "%.64s", (char const *)check_head + 48);
	return text;
}
static unsigned long check_got;
static HANDLE check_thread;

static int check_put(void *context, unsigned char const *data, unsigned long size)
{
	unsigned long n = size < sizeof(check_head) - check_got ? size : sizeof(check_head) - check_got;
	(void)context;
	memcpy(check_head + check_got, data, n);
	check_got += n;
	return check_got >= sizeof(check_head);    /* (enough: the rest isn't fetched) */
}

static DWORD WINAPI check_main(LPVOID unused)
{
	char url[512], address[32], error[256];
	unsigned long size;
	(void)unused;
	update_url(url, sizeof(url));
	check_got = 0;
	if (download_network(NULL, address, error))
	{
		check_state = CHECK_FAILED;
		return 0;
	}
	/* (a limit above any package's size: download refuses a longer one before reading; check_put stops it at 128) */
	download(url, 256ul << 20, check_put, NULL, NULL, &size, error);
	if (check_got < sizeof(check_head) || memcmp(check_head, "OXPK", 4))
	{
		check_state = CHECK_FAILED;
		return 0;
	}
	check_release = u32(check_head + 116);
	check_build = u32(check_head + 124);
	check_state = newer_package(check_release, check_build) ? CHECK_NEWER : CHECK_NEWEST;
	return 0;
}

static void check_wait(void)
{
	if (!check_thread)
		return;
	WaitForSingleObject(check_thread, INFINITE);
	CloseHandle(check_thread);
	check_thread = NULL;
}

static void check_start(void)
{
	check_wait();
	check_state = CHECK_RUNNING;
	check_thread = CreateThread(NULL, 0, check_main, NULL, 0, NULL);
	if (!check_thread)
		check_state = CHECK_FAILED;
}

/* ---- the update downloaded while it's asked about (update_prompt): on a thread of its own, into DOWNLOAD_PATH, as
check_updates would; Yes (or Always) takes what it got (check_updates waits for the rest, its progress shown), No stops
it and deletes it. Nothing is checked or put in place until the answer: that's check_updates', as before ---- */

static HANDLE fetch_thread;
static volatile int fetch_stop, fetch_result;           /* (0 still going, 1 all in, 2 failed or stopped) */
static volatile unsigned long fetch_done, fetch_total;
static unsigned long fetch_size;
static char fetch_error[256];

static int fetch_put(void *context, unsigned char const *data, unsigned long size)
{
	return fetch_stop || write_part(context, data, size);
}

static void fetch_step(void *context, char const *text)
{
	(void)context;
	(void)text;
}

static void fetch_progress(void *context, unsigned long done, unsigned long total)
{
	(void)context;
	fetch_done = done;
	fetch_total = total;
}

static DWORD WINAPI fetch_main(LPVOID unused)
{
	static struct writer w;
	struct download_report report = {fetch_step, fetch_progress, NULL};
	char url[512], address[32];
	(void)unused;
	update_url(url, sizeof(url));
	if (download_network(NULL, address, fetch_error))
	{
		fetch_result = 2;
		return 0;
	}
	w.file = create(DOWNLOAD_PATH);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		text_print(fetch_error, sizeof(fetch_error), "couldn't write beside the installer");
		fetch_result = 2;
		return 0;
	}
	if (download(url, 64ul * 1024 * 1024, fetch_put, &w, &report, &fetch_size, fetch_error) || fetch_stop)
	{
		CloseHandle(w.file);
		DeleteFileA(DOWNLOAD_PATH);
		if (fetch_stop)
			text_print(fetch_error, sizeof(fetch_error), "stopped");
		fetch_result = 2;
		return 0;
	}
	if (!finish(w.file))
	{
		DeleteFileA(DOWNLOAD_PATH);
		text_print(fetch_error, sizeof(fetch_error), "couldn't finish writing it");
		fetch_result = 2;
		return 0;
	}
	fetch_result = 1;
	return 0;
}

static void fetch_start(void)
{
	check_wait();
	fetch_stop = 0;
	fetch_result = 0;
	fetch_done = fetch_total = 0;
	fetch_error[0] = 0;
	fetch_thread = CreateThread(NULL, 0, fetch_main, NULL, 0, NULL);
	if (!fetch_thread)
		fetch_result = 2;
	log_file("updates: downloading while asked");
}

/* (No: stopped, and what it got deleted) */
static void fetch_cancel(void)
{
	if (!fetch_thread)
		return;
	fetch_stop = 1;
	WaitForSingleObject(fetch_thread, INFINITE);
	CloseHandle(fetch_thread);
	fetch_thread = NULL;
	DeleteFileA(DOWNLOAD_PATH);
	log_file("updates: the download stopped (No)");
}

/* (Yes: the rest waited for, its progress shown; 1 when it's all in DOWNLOAD_PATH, its size in *size) */
static int fetch_finish(unsigned long *size)
{
	if (!fetch_thread)
		return 0;
	while (!fetch_result)
	{
		update_progress(NULL, fetch_done, fetch_total);
		Sleep(100);
	}
	WaitForSingleObject(fetch_thread, INFINITE);
	CloseHandle(fetch_thread);
	fetch_thread = NULL;
	if (fetch_result != 1)
	{
		say_in(COLOUR_YELLOW, "The download didn't finish (%s): trying again.", fetch_error);
		return 0;
	}
	update_progress(NULL, fetch_size, fetch_size);
	*size = fetch_size;
	return 1;
}

/* looks for a newer package online; 1 when opence.oxpk was replaced */
static int check_updates(void)
{
	static struct package fresh;
	char url[512], address[32], error[256], value[200];
	struct download_report report = {update_step, update_progress, NULL};
	struct writer w;
	unsigned long size;
	char const *fresh_signer;
	int b, changed = 0;

	check_wait();
	heading("Looking online for a newer package");
	log_clear();
	update_url(url, sizeof(url));
	say("From %s", url);
	/* (downloaded while the update was asked about: the rest waited for) */
	if (fetch_finish(&size))
		goto downloaded;
	if (download_network(&report, address, error))
	{
		say_in(COLOUR_RED, "No network: %s.", error);
		goto back;
	}
	say("The Xbox's address: %s", address);
	w.file = create(DOWNLOAD_PATH);
	if (w.file == INVALID_HANDLE_VALUE)
	{
		say_in(COLOUR_RED, "Couldn't write beside the installer.");
		goto back;
	}
	if (download(url, 64ul * 1024 * 1024, write_part, &w, &report, &size, error))
	{
		CloseHandle(w.file);
		DeleteFileA(DOWNLOAD_PATH);
		say_in(COLOUR_RED, "Couldn't download it: %s.", error);
		goto back;
	}
	if (!finish(w.file))
	{
		DeleteFileA(DOWNLOAD_PATH);
		say_in(COLOUR_RED, "Couldn't finish writing what was downloaded.");
		goto back;
	}
downloaded:
	say("  %lu KB downloaded", size / 1024);
	if (package_load(DOWNLOAD_PATH, &fresh, error))
	{
		DeleteFileA(DOWNLOAD_PATH);
		say_in(COLOUR_RED, "What came isn't a package that reads: %s", error);
		goto back;
	}
	say("Found: %s (release %lu)", fresh.name, fresh.release);
	if (fresh.build)
		say("  build v1.%02lu", fresh.build);
	package_manifest_value(&fresh, "date", value, sizeof(value));
	if (value[0])
		say("  made %s", value);
	package_manifest_value(&fresh, "notes", value, sizeof(value));
	if (value[0])
		say("  %s", value);
	/* only a package signed by a key it trusts, and not older than the one it has */
	b = package_verify(&fresh, trusted_keys, trusted_key_count, &fresh_signer);
	if (!trusted(b))
	{
		say_in(COLOUR_RED, "It isn't signed by a key this installer trusts, so it isn't used.");
		goto discard;
	}
	if (b == PACKAGE_SIGNED)
		say_in(COLOUR_GREEN, "  signed (%s)", fresh_signer);
	else
		say_in(COLOUR_YELLOW, "  not signed: used because update_url.txt says allow_unsigned (a test location)");
	if (package_ok && package_same(&fresh, &package))
	{
		say_in(COLOUR_GREEN, "That's the package you have: it's up to date.");
		goto discard;
	}
	if (package_ok && trusted(signature) && !newer_package(fresh.release, fresh.build))
	{
		say_in(COLOUR_YELLOW, !package.release && fresh.release ? "That's a release, older than your test build (%s), "
			"so yours is kept." : "That's older than yours (%s), so yours is kept.", package.name);
		goto discard;
	}
	if (fresh.installer_version > INSTALLER_VERSION)
		say("  It carries installer version %lu (this one is %d).", fresh.installer_version, INSTALLER_VERSION);
	if (auto_mode)
		b = A;
	else
	{
		say_in(COLOUR_YELLOW, package_ok ? "A: use it in place of yours   B: keep yours" : "A: use it   B: back");
		while ((b = wait_input()) != A && b != B)
			;
	}
	if (b == B)
		goto discard;
	package_free(&fresh);
	/* the old package kept as opence_old.oxpk, the new one in its place */
	DeleteFileA(OLD_PACKAGE_PATH);
	if (exists(PACKAGE_PATH) && !MoveFileA(PACKAGE_PATH, OLD_PACKAGE_PATH))
	{
		DeleteFileA(DOWNLOAD_PATH);
		say_in(COLOUR_RED, "Couldn't move the old package aside: nothing changed.");
		goto back;
	}
	if (!MoveFileA(DOWNLOAD_PATH, PACKAGE_PATH))
	{
		MoveFileA(OLD_PACKAGE_PATH, PACKAGE_PATH);
		DeleteFileA(DOWNLOAD_PATH);
		say_in(COLOUR_RED, "Couldn't put the new package in place: yours is kept.");
		goto back;
	}
	flush_own_drive();
	package_read();
	say_in(COLOUR_GREEN, "Done: it's opence.oxpk now (the old one is kept as opence_old.oxpk).");
	changed = 1;
	goto back;
discard:
	package_free(&fresh);
	DeleteFileA(DOWNLOAD_PATH);
back:
	if (auto_mode)
	{
		Sleep(1000);
		return changed;
	}
	say_in(COLOUR_YELLOW, "B: back");
	while (wait_input() != B)
		;
	return changed;
}

/* ---- the list ---- */

/* ---- map packs (packs.c): another Halo's maps imported into Halo 1 Unlimited ---- */

/* a folder whose maps can be imported: not an install of the game (its maps are the game's own) */
static int can_import(struct halo const *h)
{
	return h->state != INSTALLED && !is_last_install(h);
}

static void packs_say(int warning, char const *text)
{
	say_in(warning ? COLOUR_YELLOW : COLOUR_WHITE, "%s", text);
}

/* the maps folder of an install of the game (the last, else any): Halo's own maps, to tell a pack's from */
static void stock_maps_folder(char *out, int size)
{
	int i, found = -1;
	for (i = 0; i < halo_count && found < 0; i++)
		found = is_last_install(&halos[i]) ? i : -1;
	for (i = 0; i < halo_count && found < 0; i++)
		found = halos[i].state == INSTALLED ? i : -1;
	if (found < 0)
		out[0] = 0;
	else
		text_print(out, size, "%s\\maps", halos[found].path);
}

/* ---- Consolidate packs: what it would save (consolidate.c: it only reads) ---- */

#define CONSOLIDATE_MAPS 256
static char consolidate_paths[CONSOLIDATE_MAPS][220];
static char const *consolidate_list[CONSOLIDATE_MAPS];

/* the first .map in a folder, as the game takes it: its path in out[220]; 0 when it has one */
static int folder_map(char const *folder, char *out)
{
	WIN32_FIND_DATAA found;
	char pattern[220];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\*.map", folder);
	find = FindFirstFileA(pattern, &found);
	if (find == INVALID_HANDLE_VALUE)
		return 1;
	text_print(out, 220, "%s\\%s", folder, found.cFileName);
	FindClose(find);
	return 0;
}

/* where the measuring is, on the bottom row (a big pack takes minutes) */
static void consolidate_progress(int warning, char const *text)
{
	/* (a player's screen: how far along, in its receipt; a map's own warning is summed up at the end) */
	if (quiet)
	{
		if (!warning && receipt_progress(text))
			receipt_show();
		return;
	}
	screen_row(SCREEN_ROWS - 1, COLOUR_BLACK);
	screen_text(SCREEN_ROWS - 1, 0, COLOUR_GREY, "%.70s", text);
	screen_show();
}

/* one group of maps measured and said */
static void consolidate_say_group(char const *stock_index, char const *name, int count, unsigned long long *before,
	unsigned long long *after)
{
	struct consolidate_measure m;
	char error[256];
	int k;
	for (k = 0; k < count; k++)
		consolidate_list[k] = consolidate_paths[k];
	consolidate_progress(0, name);
	if (consolidate_measure(stock_index, consolidate_list, count, consolidate_progress, &m, error))
	{
		say_in(COLOUR_RED, "%s: %s", name, error);
		return;
	}
	if (m.maps)
		say_in(COLOUR_GREEN, "%s: %lu MB, about %lu MB consolidated", name, (unsigned long)(m.before >> 20), (unsigned long)(m.after >> 20));
	else
		say("%s: nothing to measure", name);
	if (m.already)
		say_in(COLOUR_GREY, "  %d already on shared files: left as they are", m.already);
	if (m.unreadable)
		say_in(COLOUR_YELLOW, "  %d can't be read: left as they are", m.unreadable);
	*before += m.before;
	*after += m.after;
}

/* every pack in this Halo's maps folder: a folder without a .map, whose folders each hold one; then the maps loose in it (a
modded Halo's) and its own folder maps, Halo's own left out. It writes and changes nothing */
static void consolidate_show(struct halo *h, struct pack_scan const *scan, char const *stock_folder)
{
	WIN32_FIND_DATAA found;
	char stock_index[220], pattern[220], folder[220];
	unsigned long long before = 0, after = 0;
	HANDLE find;
	int count = 0, k;

	text_print(stock_index, sizeof(stock_index), "%s\\stock.index", stock_folder);
	package_free(&package);
	log_memory();
	/* the packs */
	text_print(pattern, sizeof(pattern), "%s\\maps\\*", h->path);
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			WIN32_FIND_DATAA inner;
			char inner_pattern[220];
			HANDLE find_inner;
			char pack_name[64];
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.' ||
				strstr(found.cFileName, ".hmp_") || !_stricmp(found.cFileName, "showcase"))
				continue;
			text_print(folder, sizeof(folder), "%s\\maps\\%s", h->path, found.cFileName);
			/* (a folder map on its own: with the loose ones, below) */
			if (!folder_map(folder, consolidate_paths[0]))
				continue;
			count = 0;
			text_print(pack_name, sizeof(pack_name), "%.60s", found.cFileName);
			text_print(inner_pattern, sizeof(inner_pattern), "%s\\*", folder);
			find_inner = FindFirstFileA(inner_pattern, &inner);
			if (find_inner == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				char map_folder[220];
				if (!(inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || inner.cFileName[0] == '.' ||
					strstr(inner.cFileName, ".hmp_") || count == CONSOLIDATE_MAPS)
					continue;
				text_print(map_folder, sizeof(map_folder), "%s\\%s", folder, inner.cFileName);
				if (!folder_map(map_folder, consolidate_paths[count]))
					count++;
			} while (FindNextFileA(find_inner, &inner));
			FindClose(find_inner);
			if (count)
				consolidate_say_group(stock_index, pack_name, count, &before, &after);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	/* the rest: loose maps that aren't Halo's own, and folder maps */
	count = 0;
	for (k = 0; k < scan->count && count < CONSOLIDATE_MAPS; k++)
	{
		if (!scan->maps[k].halos && !scan->maps[k].small)
			text_print(consolidate_paths[count++], 220, "%s\\maps\\%s.map", h->path, scan->maps[k].name);
	}
	find = FindFirstFileA(pattern, &found);
	if (find != INVALID_HANDLE_VALUE)
	{
		do
		{
			if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.' ||
				strstr(found.cFileName, ".hmp_") || !_stricmp(found.cFileName, "showcase") || count == CONSOLIDATE_MAPS)
				continue;
			text_print(folder, sizeof(folder), "%s\\maps\\%s", h->path, found.cFileName);
			if (!folder_map(folder, consolidate_paths[count]))
				count++;
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
	if (count)
		consolidate_say_group(stock_index, "Other maps", count, &before, &after);
	if (before)
		say_in(COLOUR_GREEN, "In all: %lu MB now, about %lu MB consolidated. Nothing was changed.", (unsigned long)(before >> 20),
			(unsigned long)(after >> 20));
	else
		say("Nothing here to consolidate. Nothing was changed.");
	screen_row(SCREEN_ROWS - 1, COLOUR_BLACK);
	package_read();
}

/* a pack's multiplayer maps and modded campaign imported, then made small against the install's stock.shared
(consolidate.c): their small maps beside their links in the starred install's maps folder (a level's too: a folder map
the campaign picker plays), the blocks two or more use in the pack's shared file. The pack's own folder is left as it is */
/* the maps made small (-1: it failed), and their MB before and after */
static int consolidate_pack(struct halo *h, struct pack_scan *scan, char const *stock_folder, unsigned long *before_mb,
	unsigned long *after_mb)
{
	static char folders[CONSOLIDATE_MAPS][220];
	static char const *folder_list[CONSOLIDATE_MAPS];
	struct consolidate_result r;
	char error[256], pack_folder[220], link[240];
	int k, count = 0;
	DWORD started;
	int made = -1;

	*before_mb = *after_mb = 0;
	step(scan->campaign ? "Importing its maps and campaign..." : "Importing its maps...");
	if (packs_import(scan, 1, scan->campaign != 0, packs_say, error))
	{
		say_in(COLOUR_RED, "%s", error);
		return -1;
	}
	text_print(pack_folder, sizeof(pack_folder), "%s\\maps\\%s", packs_storage, scan->key);
	for (k = 0; k < scan->count && count < CONSOLIDATE_MAPS; k++)
	{
		struct pack_map const *m = &scan->maps[k];
		if ((m->type != 1 && (m->type != 0 || !scan->campaign)) || m->halos || m->small)
			continue;
		text_print(folders[count], 220, "%s\\%s", pack_folder, m->name);
		text_print(link, sizeof(link), "%s\\map.link", folders[count]);
		/* (one the import skipped isn't in the list) */
		if (GetFileAttributesA(link) == INVALID_FILE_ATTRIBUTES)
			continue;
		text_print(consolidate_paths[count], 220, "%s\\maps\\%s.map", h->path, m->name);
		consolidate_list[count] = consolidate_paths[count];
		folder_list[count] = folders[count];
		count++;
	}
	step("Making %d maps small (some minutes)...", count);
	package_free(&package);
	log_memory();
	started = GetTickCount();
	if (consolidate_convert(stock_folder, pack_folder, scan->key, consolidate_list, folder_list, count, consolidate_progress,
		&r, error))
		say_in(COLOUR_RED, "%s", error);
	else
	{
		made = r.converted;
		*before_mb = (unsigned long)(r.before >> 20);
		*after_mb = (unsigned long)((r.after + (1 << 20) - 1) >> 20);
		if (r.converted)
			say_in(COLOUR_GREEN, "Done: %d maps, %lu MB as they were, %lu MB small%s%s%s.", r.converted,
				(unsigned long)(r.before >> 20), (unsigned long)((r.after + (1 << 20) - 1) >> 20),
				r.shared[0] ? " (with " : "", r.shared, r.shared[0] ? ".shared)" : "");
		if (r.converted)
			step("Done: %d maps went from %lu MB to %lu MB.", r.converted, (unsigned long)(r.before >> 20),
				(unsigned long)((r.after + (1 << 20) - 1) >> 20));
		else
			step("No map was made small.");
		if (r.already)
			say_in(COLOUR_GREY, "  %d already draw on shared files: left as they are.", r.already);
		if (r.unreadable)
			say_in(COLOUR_YELLOW, "  %d can't be read: left as they are.", r.unreadable);
		if (r.too_big)
			say_in(COLOUR_YELLOW, r.too_big == 1 ? "  1 map is too big to shrink: it stays full size, and plays as it is." :
				"  %d maps are too big to shrink: they stay full size, and play as they are.", r.too_big);
		if (r.failed)
			say_in(COLOUR_YELLOW, "  %d didn't read back right: left as they are.", r.failed);
		say_in(COLOUR_GREY, "The game plays the small ones. %s\\maps is left as it is.", h->path);
		/* (developer.ini: what it saved and took, a line for packs_known.txt, in pack_results.txt beside the installer) */
		if (developer && r.converted)
		{
			FILE *file = fopen(own_path("pack_results.txt"), "a");
			if (file)
			{
				fprintf(file, "%08lx  %4d  %5lu  %5lu  %5lu  %s (installer %d)\n", scan->fingerprint, scan->count,
					(unsigned long)(scan->size >> 20), (unsigned long)((r.after + (1 << 20) - 1) >> 20),
					(unsigned long)((GetTickCount() - started) / 1000), scan->path, INSTALLER_VERSION);
				fclose(file);
				flush_own_drive();
			}
		}
	}
	screen_row(SCREEN_ROWS - 1, COLOUR_BLACK);
	package_read();
	return made;
}

/* ---- Shrink maps: full-size maps that a pack's file here was made from (its list names them: consolidate_shrink) ---- */

#define SHRINK_PACKS 32
static struct shrink_pack
{
	char path[220];                 /* the pack's file */
	char name[32];
	int first, count;               /* its maps, in consolidate_paths */
} shrink_packs[SHRINK_PACKS];
static int shrink_pack_count, shrink_count;
#define MAP_HEADER 2048
static unsigned long long shrink_before, shrink_after;
static struct consolidate_member shrink_members[CONSOLIDATE_MAPS];

/* a file's first size bytes: 1 when read */
static int read_head_bytes(char const *path, unsigned char *to, unsigned long size)
{
	HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;
	int ok;
	if (file == INVALID_HANDLE_VALUE)
		return 0;
	ok = ReadFile(file, to, size, &got, NULL) && got == size;
	CloseHandle(file);
	return ok;
}

/* a shrink cut short put right: a <name>.map.big with no <name>.map goes back; one beside it, and a .map.new, go */
static void shrink_tidy(char const *folder)
{
	static char const *const kinds[] = {"*.map.big", "*.map.new"};
	WIN32_FIND_DATAA found;
	char pattern[240], path[240], map[240];
	HANDLE find;
	int k;
	for (k = 0; k < 2; k++)
	{
		text_print(pattern, sizeof(pattern), "%s\\%s", folder, kinds[k]);
		find = FindFirstFileA(pattern, &found);
		if (find == INVALID_HANDLE_VALUE)
			continue;
		do
		{
			text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
			text_print(map, sizeof(map), "%s", path);
			map[strlen(map) - 4] = 0;
			if (!k && GetFileAttributesA(map) == INVALID_FILE_ATTRIBUTES)
				MoveFileA(path, map);
			else
				DeleteFileA(path);
		} while (FindNextFileA(find, &found));
		FindClose(find);
	}
}

/* a folder's map, if it's full size and one of the members (count of them): added to the pack's */
static void shrink_consider(char const *folder, struct shrink_pack *p, int members)
{
	unsigned char head[MAP_HEADER];
	char path[220];
	uint32_t size, crc_head, crc_tail;
	unsigned long length;
	int k;
	shrink_tidy(folder);
	if (folder_map(folder, path) || shrink_count >= CONSOLIDATE_MAPS)
		return;
	for (k = 0; k < shrink_count; k++)
	{
		if (!_stricmp(consolidate_paths[k], path))
			return;
	}
	if (!read_head_bytes(path, head, sizeof(head)) || u32(head + 0x700) == 0x73687264ul || !u32(head + 8))
		return;
	if (consolidate_identity(path, &size, &crc_head, &crc_tail))
		return;
	for (k = 0; k < members; k++)
	{
		struct consolidate_member const *m = &shrink_members[k];
		if (m->size != size || m->head != crc_head || m->tail != crc_tail)
			continue;
		length = u32(head + 8);
		text_print(consolidate_paths[shrink_count++], 220, "%s", path);
		p->count++;
		shrink_before += size;
		/* (its small map packed about as well as it does) */
		shrink_after += (unsigned long long)m->small_length * size / length;
		return;
	}
}

/* the packs' files in a maps folder (in it and in its folders), each with the full-size maps it was made from where the game
would find it for them: in its folder, and in that folder's folders */
static void shrink_scan(char const *root)
{
	WIN32_FIND_DATAA found, file;
	char folder[220], pattern[240], inner[240];
	HANDLE find, files;
	int level;
	for (level = 0; level < 2; level++)
	{
		text_print(pattern, sizeof(pattern), "%s\\*", root);
		find = level ? FindFirstFileA(pattern, &found) : INVALID_HANDLE_VALUE;
		if (level && find == INVALID_HANDLE_VALUE)
			return;
		do
		{
			if (level)
			{
				if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.' ||
					strstr(found.cFileName, ".hmp_"))
					continue;
				text_print(folder, sizeof(folder), "%s\\%s", root, found.cFileName);
			}
			else
				text_print(folder, sizeof(folder), "%s", root);
			text_print(pattern, sizeof(pattern), "%s\\*.shared", folder);
			files = FindFirstFileA(pattern, &file);
			if (files == INVALID_HANDLE_VALUE)
				continue;
			do
			{
				struct shrink_pack *p;
				int members;
				if (!_stricmp(file.cFileName, "stock.shared") || shrink_pack_count == SHRINK_PACKS)
					continue;
				p = &shrink_packs[shrink_pack_count];
				text_print(p->path, sizeof(p->path), "%s\\%s", folder, file.cFileName);
				if (consolidate_members(p->path, shrink_members, CONSOLIDATE_MAPS, &members, p->name))
					continue;
				p->first = shrink_count;
				p->count = 0;
				shrink_consider(folder, p, members);
				text_print(inner, sizeof(inner), "%s\\*", folder);
				{
					WIN32_FIND_DATAA sub;
					HANDLE subs = FindFirstFileA(inner, &sub);
					if (subs != INVALID_HANDLE_VALUE)
					{
						do
						{
							char map_folder[220];
							if (!(sub.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || sub.cFileName[0] == '.' ||
								strstr(sub.cFileName, ".hmp_"))
								continue;
							text_print(map_folder, sizeof(map_folder), "%s\\%s", folder, sub.cFileName);
							shrink_consider(map_folder, p, members);
						} while (FindNextFileA(subs, &sub));
						FindClose(subs);
					}
				}
				if (p->count)
					shrink_pack_count++;
			} while (FindNextFileA(files, &file));
			FindClose(files);
		} while (level && FindNextFileA(find, &found));
		if (level)
			FindClose(find);
	}
}

/* this install's maps folder, the starred install's and E:'s old storage looked at for maps to shrink */
static void shrink_look(struct halo const *h)
{
	char root[200];
	shrink_pack_count = shrink_count = 0;
	shrink_before = shrink_after = 0;
	text_print(root, sizeof(root), "%s\\maps", h->path);
	shrink_scan(root);
	/* (the packs imported into the starred install, and into E:'s storage by installers before 41) */
	if (packs_storage[0] && _stricmp(packs_storage, h->path))
	{
		text_print(root, sizeof(root), "%s\\maps", packs_storage);
		shrink_scan(root);
	}
	shrink_scan("E:\\TDATA\\" OWN_TITLE_FOLDER "\\maps");
}

/* shrinking: where it is on the bottom row, and why a map was left in the log */
static void shrink_say(int warning, char const *text)
{
	if (warning)
		say_in(COLOUR_YELLOW, "  %s", text);
	else
		consolidate_progress(0, text);
}

/* each pack's file's full-size maps made small against it */
static void shrink_maps(char const *stock_folder)
{
	struct consolidate_result r;
	char error[256];
	unsigned long long before = 0, after = 0;
	int k, i, done = 0, left = 0;
	package_free(&package);
	log_memory();
	for (k = 0; k < shrink_pack_count; k++)
	{
		struct shrink_pack const *p = &shrink_packs[k];
		for (i = 0; i < p->count; i++)
			consolidate_list[i] = consolidate_paths[p->first + i];
		say("%s: %d map%s...", p->name, p->count, p->count == 1 ? "" : "s");
		if (consolidate_shrink(stock_folder, p->path, consolidate_list, p->count, shrink_say, &r, error))
		{
			say_in(COLOUR_RED, "  %s", error);
			left += p->count;
			continue;
		}
		done += r.converted;
		left += p->count - r.converted;
		before += r.before;
		after += r.after;
	}
	if (done)
		say_in(COLOUR_GREEN, "Done: %d map%s shrunk, %lu MB as they were, %lu MB now.", done, done == 1 ? "" : "s",
			(unsigned long)(before >> 20), (unsigned long)((after + (1 << 20) - 1) >> 20));
	if (left)
		say_in(COLOUR_YELLOW, "%d didn't come out as their pack's file says: left as they were.", left);
	screen_row(SCREEN_ROWS - 1, COLOUR_BLACK);
	package_read();
}

static void map_pack(struct halo *h)
{
	enum { BOTH, MULTIPLAYER, CAMPAIGN, REMOVE, MEASURE, CONSOLIDATE, SHRINK, BACK };
	static struct pack_scan scan;
	char stock[200], error[256], line[SCREEN_COLUMNS + 8], stock_index[220], shrink_label[64];
	int choices[8], count = 0, chosen = 0, b, k;

	text_print(line, sizeof(line), "Maps: %s", h->path);
	heading(line);
	log_clear();
	say("Looking at the maps in %s\\maps...", h->path);
	stock_maps_folder(stock, sizeof(stock));
	/* (what's imported goes in that install's own folder, the starred one: never E:'s storage) */
	text_print(packs_storage, sizeof(packs_storage), "%.*s", stock[0] ? (int)strlen(stock) - 5 : 0, stock);
	packs_scan(h->path, stock, &scan);
	if (can_import(h) && !packs_storage[0])
	{
		say_in(COLOUR_YELLOW, "Install Halo 1 Unlimited first: a pack's maps are imported into its folder.");
		scan.multiplayer = scan.campaign = scan.multiplayer_imported = scan.campaign_imported = 0;
	}
	else if (can_import(h))
	{
		say("Multiplayer: %d maps Halo 1 Unlimited doesn't have%s.", scan.multiplayer,
			scan.multiplayer_imported ? " (imported)" : "");
		if (scan.campaign)
			say("Campaign: %d level%s, modded%s.", scan.campaign, scan.campaign == 1 ? "" : "s",
				scan.campaign_imported ? " (imported)" : "");
		else
			say("Campaign: none that isn't Halo's own.");
		say_in(COLOUR_GREY, "Nothing is copied: the game plays them from this folder, which is left as it is. They go in "
			"the map list as the pack \"%s\", and the campaign under the folder's name (in %s).", scan.key, packs_storage);
	}
	else
	{
		/* (an install of the game: its maps are the game's own, nothing to import) */
		say("This is an install of the game: the packs in its maps folder can be measured.");
		scan.multiplayer = scan.campaign = scan.multiplayer_imported = scan.campaign_imported = 0;
	}
	text_print(stock_index, sizeof(stock_index), "%s\\stock.index", stock);
	/* (an install's full-size maps, downloaded or put there, that a pack's file beside them was made from) */
	shrink_pack_count = shrink_count = 0;
	if (!can_import(h) && stock[0] && GetFileAttributesA(stock_index) != INVALID_FILE_ATTRIBUTES)
	{
		shrink_look(h);
		if (shrink_count)
			say_in(COLOUR_GREEN, "You have %d map%s that can be shrunk to save space.", shrink_count,
				shrink_count == 1 ? "" : "s");
	}
	text_print(shrink_label, sizeof(shrink_label), "Shrink maps (saves about %lu MB)",
		(unsigned long)((shrink_before - (shrink_after < shrink_before ? shrink_after : shrink_before)) >> 20));
	if (scan.multiplayer && scan.campaign)
		choices[count++] = BOTH;
	if (scan.multiplayer)
		choices[count++] = MULTIPLAYER;
	if (scan.campaign)
		choices[count++] = CAMPAIGN;
	if (scan.multiplayer_imported || scan.campaign_imported)
		choices[count++] = REMOVE;
	if (shrink_count)
		choices[count++] = SHRINK;
	/* (with the install's stock.shared to measure against) */
	if (stock[0])
		choices[count++] = MEASURE;
	if (stock[0] && (scan.multiplayer || scan.campaign) && GetFileAttributesA(stock_index) != INVALID_FILE_ATTRIBUTES)
		choices[count++] = CONSOLIDATE;
	choices[count++] = BACK;
	for (;;)
	{
		char const *const labels[] = {"Import multiplayer and campaign", "Import multiplayer maps only",
			"Import the campaign only", "Take out what's imported", "See what consolidating would save",
			"Make its maps small (consolidate)", shrink_label, "Back"};
		for (k = 0; k < count; k++)
		{
			int r = SCREEN_ROWS - 2 - count + k, at;
			int again = (choices[k] == BOTH && scan.multiplayer_imported && scan.campaign_imported) ||
				(choices[k] == MULTIPLAYER && scan.multiplayer_imported) ||
				(choices[k] == CAMPAIGN && scan.campaign_imported);
			screen_row(r, k == chosen ? COLOUR_BAR : COLOUR_BLACK);
			at = screen_text(r, 2, choices[k] == BACK ? COLOUR_GREY : COLOUR_GREEN, "%s", labels[choices[k]]);
			if (again)
				screen_text(r, at, COLOUR_GREY, " (again: replaces what's imported)");
		}
		screen_row(SCREEN_ROWS - 1, COLOUR_BLACK);
		screen_text(SCREEN_ROWS - 1, 0, COLOUR_YELLOW, "A: choose   B: back");
		screen_show();
		b = wait_input();
		if (b == UP && chosen > 0)
			chosen--;
		else if (b == DOWN && chosen + 1 < count)
			chosen++;
		else if (b == B || (b == A && choices[chosen] == BACK))
			return;
		else if (b == A)
			break;
	}
	heading(line);
	log_clear();
	if (choices[chosen] == MEASURE)
		consolidate_show(h, &scan, stock);
	else if (choices[chosen] == CONSOLIDATE)
	{
		unsigned long before, after;
		consolidate_pack(h, &scan, stock, &before, &after);
	}
	else if (choices[chosen] == SHRINK)
		shrink_maps(stock);
	else if (choices[chosen] == REMOVE)
	{
		packs_remove(&scan, 1, 1, packs_say);
		say_in(COLOUR_GREEN, "Done. The pack's own folder wasn't touched.");
	}
	else if (packs_import(&scan, choices[chosen] != CAMPAIGN, choices[chosen] != MULTIPLAYER, packs_say, error))
		say_in(COLOUR_RED, "%s", error);
	else
		say_in(COLOUR_GREEN, "Done: they're in the game's folder, %s.", packs_storage);
	flush_drive("E:\\");
	if (packs_storage[0] && packs_storage[0] != 'E')
		flush_drive(packs_storage);
	say_in(COLOUR_YELLOW, "B: back");
	while (wait_input() != B)
		;
}

#define LIST_TOP 3
#define LIST_ROWS 16
static int selected, first_shown;

static void show_list(void)
{
	int i, ready_count = 0, row;
	heading(NULL);
	for (i = 0; i < halo_count; i++)
		ready_count += ready(&halos[i]);
	if (!package_ok)
	{
		screen_text(LIST_TOP, 0, COLOUR_RED, "%s", package_error);
		if (package_missing)
			screen_text(LIST_TOP + 1, 0, COLOUR_WHITE, "Put opence.oxpk beside this installer, or press Y to download it.");
		if (package_missing)
		{
			screen_text(LIST_TOP + 3, 0, COLOUR_GREY, "%.70s", started_as[0]);
			screen_text(LIST_TOP + 4, 0, COLOUR_GREY, "%.70s", started_as[1]);
		}
	}
	else if (!halo_count)
	{
		screen_text(LIST_TOP, 0, COLOUR_WHITE, "No Halo found in E:, F: or G: (in their folders, or Games folders).");
	}
	if (selected < first_shown)
		first_shown = selected;
	if (selected >= first_shown + LIST_ROWS)
		first_shown = selected - LIST_ROWS + 1;
	for (row = 0; package_ok && row < LIST_ROWS && first_shown + row < halo_count; row++)
	{
		struct halo *h = &halos[first_shown + row];
		int r = LIST_TOP + row, is_selected = first_shown + row == selected;
		unsigned long colour = ready(h) ? COLOUR_GREEN : COLOUR_GREY;
		size_t n = strlen(h->path);
		screen_row(r, is_selected ? COLOUR_BAR : COLOUR_BLACK);
		if (is_last_install(h))
			screen_text(r, 0, COLOUR_YELLOW, "*");
		if (n > 40)
			screen_text(r, 1, colour, "...%s", h->path + n - 37);
		else
			screen_text(r, 1, colour, "%s", h->path);
		screen_text(r, 42, colour, "%.30s", h->note);
	}
	if (halo_count > LIST_ROWS)
		screen_text(LIST_TOP + LIST_ROWS, 1, COLOUR_DIM, "%d Halo found, %d ready: %d-%d shown (LT/RT: a page)%s",
			halo_count, ready_count, first_shown + 1, first_shown + row, is_last_install(&halos[0]) ? "  * last" : "");
	else if (halo_count)
		screen_text(LIST_TOP + LIST_ROWS, 1, COLOUR_DIM, "%d Halo found, %d ready%s", halo_count, ready_count,
			is_last_install(&halos[0]) ? "   * your last install" : "");
	if (halo_count && package_ok)
	{
		struct halo *h = &halos[selected];
		char actions[SCREEN_COLUMNS + 1];
		int at = 0;
		screen_text(21, 0, COLOUR_WHITE, "%s", h->detail[0]);
		screen_text(22, 0, COLOUR_WHITE, "%s", h->detail[1]);
		actions[0] = 0;
		if (ready(h))
			at += text_print(actions + at, sizeof(actions) - at, "A: %s   ", h->state == INSTALLED ?
				(h->up_to_date ? "install again" : "update") : "install");
		if (can_restore(h))
			at += text_print(actions + at, sizeof(actions) - at, h->state == INSTALLED || h->state == OTHER ||
				h->state == MISSING ? "X: retail Halo back   " : "X: maps back   ");
		if (ready(h) && h->state != MISSING)
			at += text_print(actions + at, sizeof(actions) - at, "START: play   ");
		at += text_print(actions + at, sizeof(actions) - at, "WHITE: maps   ");
		text_print(actions + at, sizeof(actions) - at, "B: dashboard");
		screen_text(24, 0, COLOUR_YELLOW, "%s", actions);
	}
	else
		screen_text(24, 0, COLOUR_YELLOW, "B: dashboard");
	screen_text(23, 0, COLOUR_DIM, "Y: look online for a newer package");
	screen_show();
}

/* after an install or a restore: the folder looked at again, the list kept on it */
static void look_again(struct halo *h)
{
	char path[160];
	int i;
	flush_drive(h->path);
	examine_fresh = 1;
	examine(h);
	examine_fresh = 0;
	packs_forget();
	scan_cache_save(SCAN_CACHE_PATH);
	text_print(path, sizeof(path), "%s", h->path);
	sort_halos();
	for (i = 0; i < halo_count; i++)
	{
		if (!strcmp(halos[i].path, path))
			selected = i;
	}
}

/* the game's updater (AUTO_PATH beside it): this folder's Halo (the one above <Halo>\updater) updated to the latest
signed package, then started, with no buttons pressed unless something goes wrong */
static void auto_update(void)
{
	char own[260], folder[260], *slash;
	struct halo *h = NULL;
	int failed = 1;

	auto_mode = 1;
	log_file("auto update (the game's updater)");
	/* (its folder: own_folder's, as a file in it) */
	drive_target(own_drive, own, sizeof(own));
	if (own[0] && own[strlen(own) - 1] != '\\')
		text_print(own + strlen(own), (int)(sizeof(own) - strlen(own)), "\\");
	text_print(own + strlen(own), (int)(sizeof(own) - strlen(own)), "default.xbe");
	if (from_device(own, folder, sizeof(folder)) && (slash = strrchr(folder, '\\')) != NULL)
	{
		*slash = 0;
		slash = strrchr(folder, '\\');
		if (slash && !_stricmp(slash + 1, UPDATER_FOLDER))
		{
			*slash = 0;
			halo_count = 0;
			add_halo(folder);
			h = halo_count ? &halos[0] : NULL;
			text_print(updater_halo, sizeof(updater_halo), "%s", folder);
		}
	}
	if (!h)
	{
		heading("Updating Halo 1 Unlimited");
		say_in(COLOUR_RED, "This updater isn't in a Halo folder's updater folder: nothing to update.");
		goto done;
	}
	/* (a newer package; and the installer it carries, which starts again and goes on from here) */
	if (check_updates())
		offer_installer_update();
	if (!package_ok || !trusted(signature))
	{
		heading("Updating Halo 1 Unlimited");
		say_in(COLOUR_RED, "There's no package signed by a key this updater trusts: nothing changed.");
		goto done;
	}
	examine(h);
	if (h->state == INSTALLED && h->up_to_date)
	{
		heading("Updating Halo 1 Unlimited");
		say_in(COLOUR_GREEN, "%s is up to date (%s).", h->path, package.name);
		failed = 0;
		goto done;
	}
	if (!ready(h) || h->state == UNKNOWN)
	{
		heading("Updating Halo 1 Unlimited");
		say_in(COLOUR_RED, "%s can't be updated here: start the installer itself to see why.", h->path);
		goto done;
	}
	failed = install(h);
done:
	DeleteFileA(AUTO_PATH);
	flush_own_drive();
	if (failed)
	{
		say_in(COLOUR_YELLOW, "A or B: back to the game (as it was)");
		{
			int b;
			while ((b = wait_input()) != A && b != B)
				;
		}
	}
	else
		Sleep(1500);
	auto_mode = 0;
	if (h)
		launch_halo(h);
	launch(NULL);
}

/* ---- the Developer screen (developer.ini: developer = 1): every Halo folder in a list, and what can be done to each, as
the installer was before its player's screens ---- */

static void developer_screen(void)
{
	find_halos();
	selected = first_shown = 0;
	for (;;)
	{
		struct halo *h = halo_count ? &halos[selected] : NULL;
		int b;
		show_list();
		b = wait_input();
		if (b == UP && selected > 0)
			selected--;
		else if (b == DOWN && selected + 1 < halo_count)
			selected++;
		else if (b == PAGE_UP)
			selected = selected > LIST_ROWS ? selected - LIST_ROWS : 0;
		else if (b == PAGE_DOWN && halo_count)
			selected = selected + LIST_ROWS < halo_count ? selected + LIST_ROWS : halo_count - 1;
		else if (b == B)
			return;
		else if (h && package_ok && b == A && ready(h))
		{
			int failed = install(h);
			if (!failed)
			{
				last_install_write(h->path);
				offer_stock_maps(h);
			}
			look_again(h);
			say_in(COLOUR_YELLOW, failed ? "B: back" : "START: play it   B: back");
			while ((b = wait_input()) != B && !(b == START && !failed))
				;
			if (b == START)
				launch_halo(&halos[selected]);
		}
		else if (h && b == X && can_restore(h))
		{
			restore(h);
			look_again(h);
			say_in(COLOUR_YELLOW, "B: back");
			while (wait_input() != B)
				;
		}
		else if (h && b == START && ready(h) && h->state != MISSING)
			launch_halo(h);
		else if (h && b == WHITE)
			map_pack(h);
		else if (b == Y && check_updates())
		{
			offer_installer_update();
			halo_changed(NULL, 1);
			selected = first_shown = 0;
		}
	}
}

/* ---- the player's screens: menus climbed with A (choose) and B (back), what the highlighted item does in a panel under
them, an operation's progress in a few plain lines (its details in installer_log.txt) ---- */

#define MENU_TOP 5
#define MENU_MOST 12
#define PANEL_TOP 19
#define PANEL_ROWS 5

struct menu_item
{
	char text[64];
	char const *about;
	int disabled;
};

/* text wrapped at spaces into width columns from (row, column), at most rows lines: how many it took */
static int wrapped(int row, int column, int width, int rows, unsigned long colour, char const *text)
{
	int used = 0;
	while (*text && used < rows)
	{
		int n = (int)strlen(text), cut = n;
		if (n > width)
		{
			for (cut = width; cut > width / 2 && text[cut] != ' '; cut--)
				;
			if (text[cut] != ' ')
				cut = width;
		}
		for (n = 0; n < cut && text[n] != '\n'; n++)
			;
		screen_text(row + used++, column, colour, "%.*s", n, text);
		text += n;
		while (*text == ' ' || *text == '\n')
			text++;
	}
	return used;
}

static void player_heading(char const *title)
{
	screen_clear();
	screen_text(0, 1, COLOUR_YELLOW, "HALO 1 UNLIMITED");
	if (package_ok && package.build)
		screen_text(0, SCREEN_COLUMNS - 8, COLOUR_DIM, "v1.%lu", package.build);
	screen_text(1, 0, COLOUR_DIM, "------------------------------------------------------------------------");
	if (title)
		screen_text(3, 1, COLOUR_WHITE, "%s", title);
	screen_show();
}

/* (menu_steps: lines between a menu's intro and its items, the answers so far and the question they answer; the items
indented under them) */
static char const *const *menu_lines;
static unsigned long const *menu_line_colours;
static int menu_line_count, menu_indent;

/* a menu: the item chosen (A), or -1 (B). intro: a line or two above it (NULL: none) */
static int menu(char const *title, char const *intro, struct menu_item const *items, int count, int chosen)
{
	int top = MENU_TOP, k, b, l;
	if (chosen < 0 || chosen >= count)
		chosen = 0;
	for (;;)
	{
		player_heading(title);
		top = MENU_TOP;
		if (intro)
			top += wrapped(MENU_TOP, 1, SCREEN_COLUMNS - 2, 4, COLOUR_GREY, intro) + 1;
		for (l = 0; l < menu_line_count; l++)
			screen_text(top++, 3, menu_line_colours[l], "%s", menu_lines[l]);
		{
			/* (a long list scrolls: the chosen item shown, with what's above and below it marked) */
			int rows = PANEL_TOP - 1 - top, first = 0, r;
			if (rows > MENU_MOST)
				rows = MENU_MOST;
			if (chosen >= rows)
				first = chosen - rows + 1;
			for (r = 0; r < rows && first + r < count; r++)
			{
				k = first + r;
				screen_row(top + r, k == chosen ? COLOUR_BAR : COLOUR_BLACK);
				screen_text(top + r, 3 + menu_indent, items[k].disabled ? COLOUR_DIM : k == chosen ? COLOUR_WHITE :
					COLOUR_GREEN, "%s", items[k].text);
			}
			if (first)
				screen_text(top, 1, COLOUR_DIM, "^");
			if (first + rows < count)
				screen_text(top + rows - 1, 1, COLOUR_DIM, "v");
		}
		if (items[chosen].about)
			wrapped(PANEL_TOP, 3, SCREEN_COLUMNS - 6, PANEL_ROWS, COLOUR_GREY, items[chosen].about);
		screen_text(SCREEN_ROWS - 1, SCREEN_COLUMNS - 22, COLOUR_DIM, "B: back    A: select");
		screen_show();
		b = wait_input();
		if (b == REFRESH)
			return -2;                 /* (the update check answered: the caller makes its items again) */
		if (b == UP && chosen > 0)
			chosen--;
		else if (b == DOWN && chosen + 1 < count)
			chosen++;
		else if (b == B)
			return -1;
		else if (b == A && !items[chosen].disabled)
			return chosen;
	}
}

/* a menu under a question: lines, the answers chosen so far (the first locked of them, in grey) and then the question
(in white), its items indented under it: as menu() */
static int menu_steps(char const *title, char const *intro, char const *const *lines, int line_count, int locked,
	struct menu_item const *items, int count, int chosen)
{
	unsigned long colours[8];
	int k;
	if (line_count > 8)
		line_count = 8;
	for (k = 0; k < line_count; k++)
		colours[k] = k < locked ? COLOUR_GREY : COLOUR_WHITE;
	menu_lines = lines;
	menu_line_colours = colours;
	menu_line_count = line_count;
	menu_indent = 2;
	k = menu(title, intro, items, count, chosen);
	menu_lines = NULL;
	menu_line_colours = NULL;
	menu_line_count = menu_indent = 0;
	return k;
}

/* a page of text, read until A or B */
static void page(char const *title, char const *text)
{
	int b;
	player_heading(title);
	wrapped(MENU_TOP, 1, SCREEN_COLUMNS - 2, SCREEN_ROWS - MENU_TOP - 2, COLOUR_WHITE, text);
	screen_text(SCREEN_ROWS - 1, SCREEN_COLUMNS - 22, COLOUR_DIM, "B: back    A: continue");
	screen_show();
	while ((b = wait_input()) != A && b != B)
		;
}

/* an operation on a player's screen: its title, a plain line of what it does; its details go to the log */
static void task_begin(char const *title, char const *plan)
{
	text_print(task_title, sizeof(task_title), "%s", title);
	quiet = 1;
	task_problem = 0;
	log_clear();
	player_heading(title);
	if (plan)
		log_file(plan);
	receipt_clear();
	if (plan)
	{
		/* (the plan: the first step, until the steps say it) */
		text_print(receipt_add(RECEIPT_ACTION)->text, sizeof(receipt[0].text), "%s", plan);
		receipt_plan = 1;
	}
	receipt_show();
}

static void task_end(void)
{
	quiet = 0;
}

/* after an operation's result: it went well, its last line shown a moment; a warning or a problem, B (or A) */
static void task_wait(void)
{
	int b;
	if (!task_problem)
	{
		Sleep(1500);
		return;
	}
	screen_text(SCREEN_ROWS - 1, SCREEN_COLUMNS - 22, COLOUR_DIM, "B: back    A: continue");
	screen_show();
	while ((b = wait_input()) != A && b != B)
		;
}

/* ---- the Map Library (library.c): maps to download, from the library's catalog (LIBRARY_DEFAULT_URL, or developer.ini's
library_url), into the install's maps folder */

static unsigned long library_started;

static void library_progress(void *context, unsigned long done, unsigned long total)
{
	char const *what = context;
	int bar = total ? (int)((unsigned long long)done * 40 / total) : 0, i;
	char line[41];
	for (i = 0; i < 40; i++)
		line[i] = i < bar ? '#' : '.';
	line[40] = 0;
	screen_row(MENU_TOP + 2, COLOUR_BLACK);
	screen_text(MENU_TOP + 2, 1, COLOUR_WHITE, "%-12s [%s] %lu.%lu of %lu.%lu MB", what, line, done >> 20,
		(done % (1ul << 20)) * 10 >> 20, total >> 20, (total % (1ul << 20)) * 10 >> 20);
	screen_show();
}

static void library_step(void *context, char const *text)
{
	(void)context;
	log_file(text);
}

static struct library library;          /* (the library's catalog: fetched once a run, again when its address changes) */
static char library_loaded_from[512];

/* the library's catalog here: 0 when it is (else the player was told why not) */
static int library_load(void)
{
	char url[512], error[256], address[32];
	int i;
	text_print(url, sizeof(url), "%s", developer_library[0] ? developer_library : LIBRARY_DEFAULT_URL);
	check_wait();                      /* (one download at a time: the update check's is done) */
	if (library.items && !strcmp(library_loaded_from, url))
		return 0;
	library_free(&library);
	player_heading("Download New Maps");
	screen_text(MENU_TOP, 1, COLOUR_GREY, "Looking at the map library...");
	screen_show();
	library_started = GetTickCount();
	if ((i = download_network(NULL, address, error) ? 1 :
		library_fetch(url, developer_library[0] && developer_unsigned, &library, error)) != 0)
	{
		char text[400];
		log_file(error);
		text_print(text, sizeof(text), "Couldn't get the map library's list: %s.%s", error, i == 1 ?
			" Is the Xbox connected to the internet?" : "");
		page("Download New Maps", text);
		library_free(&library);
		return 1;
	}
	text_print(library_loaded_from, sizeof(library_loaded_from), "%s", url);
	{
		char line[200];
		text_print(line, sizeof(line), "library: %d items (serial %lu, %s) from %s in %lu ms", library.count,
			library.serial, library.signer[0] ? library.signer : "not signed", url, GetTickCount() - library_started);
		log_file(line);
	}
	return 0;
}

/* the library's single maps (packs 0) or its whole map packs (packs 1): each one's state, A to get or remove it */
static void player_library(struct halo *h, int packs)
{
	char const *list_title = packs ? "Full Map Packs" : "Individual Maps";
	struct menu_item *items;
	int *shown;
	char maps[200], error[256];
	int k = 0, i, count;
	text_print(maps, sizeof(maps), "%s\\maps", h->path);
	items = malloc(library.count * sizeof(*items));
	shown = malloc(library.count * sizeof(*shown));
	if (!items || !shown)
	{
		free(items);
		free(shown);
		return;
	}
	library_settle(&library, maps);
	for (;;)
	{
		struct library_item *item;
		static char const *const states[] = {"", "Installed", "Update", "Replaces yours", "Other version"};
		library_states(&library, maps);
		for (i = 0, count = 0; i < library.count; i++)
		{
			item = &library.items[i];
			if (!item->pack != !packs)
				continue;
			text_print(items[count].text, sizeof(items[count].text), "%-26.26s %-9.9s %4lu MB  %s", item->name,
				item->group, (item->size + 524288) >> 20, states[item->state]);
			items[count].about = item->about[0] ? item->about : packs ? "A map pack from the library." :
				"A map from the library.";
			items[count].disabled = 0;
			shown[count++] = i;
		}
		if (!count)
		{
			page(list_title, packs ? "The map library has no map packs in it yet." :
				"The map library has no maps in it yet.");
			break;
		}
		if ((k = menu(list_title, library.signer[0] ? packs ? "Whole map packs, ready to play. A: get one." :
			"Maps made for Halo 1 Unlimited. A: get one." : "From a TEST library, not signed. A: get one.", items, count,
			k)) < 0)
			break;
		item = &library.items[shown[k]];
		{
			struct menu_item choices[3];
			char title[80], line[400], where[300];
			int n = 0, get = -1, remove = -1, c;
			memset(choices, 0, sizeof(choices));
			if (item->state != LIBRARY_INSTALLED)
			{
				text_print(choices[n].text, sizeof(choices[n].text), item->state == LIBRARY_UPDATE ? "Update it" :
					"Download it");
				get = n++;
			}
			else
			{
				text_print(choices[n].text, sizeof(choices[n].text), "Remove it from this Xbox");
				remove = n++;
			}
			text_print(choices[n++].text, sizeof(choices[0].text), "Back");
			text_print(title, sizeof(title), "%s", item->name);
			library_folder(item, maps, where, sizeof(where));
			text_print(line, sizeof(line), "%s%s%s%s%lu MB to download, %lu MB on %c:. Goes in %s.%s%s%s", item->group,
				item->version[0] ? ", version " : "", item->version, item->group[0] || item->version[0] ? ". " : "",
				(item->size + 524288) >> 20, (item->unpacked + 524288) >> 20, maps[0], where,
				item->state == LIBRARY_OTHER ? " Your copy in maps\\" : "", item->state == LIBRARY_OTHER ? item->item : "",
				item->state == LIBRARY_OTHER ? " is replaced." : item->state == LIBRARY_SWAP ?
				" It replaces the other version you have." : "");
			c = menu(title, line, choices, n, 0);
			if (c == get)
			{
				struct download_report report = {library_step, library_progress, "Downloading"};
				unsigned long started = GetTickCount();
				char text[300];
				player_heading(title);
				screen_text(MENU_TOP, 1, COLOUR_GREY, "Getting it from the map library...");
				screen_show();
				if (library_get(&library, item, maps, &report, error))
				{
					text_print(text, sizeof(text), "library: %s: %s", item->item, error);
					log_file(text);
					text_print(text, sizeof(text), "Couldn't get %s: %s.", item->name, error);
					page(title, text);
				}
				else
				{
					text_print(text, sizeof(text), "library: %s: in (%lu MB, %lu ms)", item->item, item->size >> 20,
						GetTickCount() - started);
					log_file(text);
					screen_text(MENU_TOP + 4, 1, COLOUR_GREEN, "Done: it's in Halo 1 Unlimited's map list.");
					screen_show();
					Sleep(1500);
				}
			}
			else if (c == remove)
			{
				struct menu_item sure[2] = {{"Remove it", NULL, 0}, {"Back", NULL, 0}};
				library_folder(item, maps, where, sizeof(where));
				text_print(line, sizeof(line), "%s comes off this Xbox (its folder %s is deleted). You can download it "
					"again any time.", item->name, where);
				if (menu(title, line, sure, 2, 1) == 0 && library_remove(item, maps, error))
					page(title, error);
			}
		}
	}
	free(items);
	free(shown);
}

/* Download New Maps: the library's single maps, or its whole map packs */
static void player_download(struct halo *h)
{
	struct menu_item items[3];
	int k = 0, i, maps_count = 0, packs_count = 0;
	if (!h || library_load())
		return;
	if (!library.count)
	{
		page("Download New Maps", "The map library has nothing in it yet.");
		return;
	}
	for (i = 0; i < library.count; i++)
		*(library.items[i].pack ? &packs_count : &maps_count) += 1;
	memset(items, 0, sizeof(items));
	text_print(items[0].text, sizeof(items[0].text), maps_count ? "Download Individual Maps (%d)" :
		"Download Individual Maps (none yet)", maps_count);
	items[0].about = "Maps made for Halo 1 Unlimited (Halo Map Porter's), one at a time: they go together in the Library "
		"pack of its map list.";
	items[0].disabled = !maps_count;
	text_print(items[1].text, sizeof(items[1].text), packs_count ? "Download Full Map Packs (%d)" :
		"Download Full Map Packs (none yet)", packs_count);
	items[1].about = "Whole map packs, already made small: each one is a pack of its own in the map list.";
	items[1].disabled = !packs_count;
	text_print(items[2].text, sizeof(items[2].text), "Back");
	if (!maps_count)
		k = 1;
	while ((k = menu("Download New Maps", NULL, items, 3, k)) >= 0 && k != 2)
		player_library(h, k);
}

/* Halo 1 Unlimited as installed: the last install that worked (installer_last.txt), else the first found; NULL: none */
static struct halo *player_install(void)
{
	int i;
	for (i = 0; i < halo_count; i++)
		if (is_last_install(&halos[i]) && halos[i].state == INSTALLED && !halos[i].unfinished)
			return &halos[i];
	for (i = 0; i < halo_count; i++)
		if (halos[i].state == INSTALLED && !halos[i].unfinished)
			return &halos[i];
	return NULL;
}

/* "F:\Games\Halo" as a player reads it: "Halo on F: (Games\Halo)" */
static void place_name(char const *path, char *out, int size)
{
	char const *last = strrchr(path, '\\');
	text_print(out, size, "%s on %c: (%s)", last ? last + 1 : path, path[0], path[1] && path[2] ? path + 3 : path);
}

static void what_is_it(void)
{
	page("What is Halo 1 Unlimited?",
		"Halo 1 Unlimited is Halo: Combat Evolved for your Xbox, made from your own copy of Halo, with more to play:\n\n"
		"- 16-player games over system link and online, with bots\n"
		"- thousands of community maps, and map packs you already have\n"
		"- the campaign in split-screen co-op, and custom campaigns\n\n"
		"It's built from the Halo on your hard drive, which keeps working as it was. Nothing of Halo's is "
		"downloaded: only Halo 1 Unlimited's own parts.");
}

/* ---- the player's install: Halo 1 Unlimited in a folder of its own (own_install), built from their Halo, which isn't
changed: its XBE, ui.map and intro movie copied, the maps built (stockmaps_build: the small maps, stock.shared and its
tables; the levels' small maps only with the campaign), then the game's XBE made there as an install makes it
(install). BUILDING_MARK is in the folder until it's done: a folder with it is unfinished, never played or built from,
and is emptied when it's installed again ---- */

static int copy_file(char const *from, char const *to)
{
	HANDLE in = CreateFileA(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL), out;
	unsigned char *buffer;
	DWORD got, put;
	int failed = 0;
	if (in == INVALID_HANDLE_VALUE)
		return 1;
	out = create(to);
	buffer = malloc(1024 * 1024);
	if (out == INVALID_HANDLE_VALUE || !buffer)
		failed = 1;
	while (!failed && ReadFile(in, buffer, 1024 * 1024, &got, NULL) && got)
		failed = !WriteFile(out, buffer, got, &put, NULL) || put != got;
	CloseHandle(in);
	free(buffer);
	if (out != INVALID_HANDLE_VALUE && !finish(out))
		failed = 1;
	if (failed)
		DeleteFileA(to);
	return failed;
}

static int write_file(char const *path, unsigned char const *data, unsigned long size)
{
	HANDLE file = create(path);
	DWORD put;
	int failed = file == INVALID_HANDLE_VALUE || !WriteFile(file, data, size, &put, NULL) || put != size;
	if (file != INVALID_HANDLE_VALUE && !finish(file))
		failed = 1;
	return failed;
}

/* the files in folder matching pattern ("*.map") deleted */
static void delete_matching_files(char const *folder, char const *pattern)
{
	WIN32_FIND_DATAA found;
	char path[260];
	HANDLE find;
	text_print(path, sizeof(path), "%s\\%s", folder, pattern);
	if ((find = FindFirstFileA(path, &found)) == INVALID_HANDLE_VALUE)
		return;
	do
	{
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		text_print(path, sizeof(path), "%s\\%s", folder, found.cFileName);
		SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
		DeleteFileA(path);
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

static unsigned long long size_of(char const *path)
{
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
		return 0;
	return (unsigned long long)data.nFileSizeHigh << 32 | data.nFileSizeLow;
}

static unsigned long long drive_free(char drive)
{
	ULARGE_INTEGER available, total, free_total;
	char root[4] = {drive, ':', '\\', 0};
	return GetDiskFreeSpaceExA(root, &available, &total, &free_total) ? available.QuadPart : 0;
}

/* the movies it takes (intro, and the credits with the campaign: the attract ones are the Showcase's now; Halo plays on
without any): what pattern, in bink\ */
static int movie_wanted(char const *name, int campaign)
{
	return !_strnicmp(name, "intro", 5) || (campaign && !_strnicmp(name, "credits", 7));
}

static unsigned long long movies_size(char const *halo, int campaign)
{
	WIN32_FIND_DATAA found;
	char pattern[240];
	HANDLE find;
	unsigned long long n = 0;
	text_print(pattern, sizeof(pattern), "%s\\bink\\*.bik", halo);
	if ((find = FindFirstFileA(pattern, &found)) == INVALID_HANDLE_VALUE)
		return 0;
	do
		if (movie_wanted(found.cFileName, campaign))
			n += found.nFileSizeLow;
	while (FindNextFileA(find, &found));
	FindClose(find);
	return n;
}

static void movies_copy(char const *halo, char const *folder, int campaign)
{
	WIN32_FIND_DATAA found;
	char pattern[240], from[240], to[240];
	HANDLE find;
	text_print(pattern, sizeof(pattern), "%s\\bink\\*.bik", halo);
	if ((find = FindFirstFileA(pattern, &found)) == INVALID_HANDLE_VALUE)
		return;
	text_print(to, sizeof(to), "%s\\bink", folder);
	CreateDirectoryA(to, NULL);
	do
	{
		if (!movie_wanted(found.cFileName, campaign))
			continue;
		text_print(from, sizeof(from), "%s\\bink\\%s", halo, found.cFileName);
		text_print(to, sizeof(to), "%s\\bink\\%s", folder, found.cFileName);
		if (copy_file(from, to))
			say_in(COLOUR_YELLOW, "The movie %s couldn't be copied: Halo plays on without it.", found.cFileName);
	} while (FindNextFileA(find, &found));
	FindClose(find);
}

/* what the install takes on the drive */
static unsigned long long own_need(struct halo const *source, int campaign)
{
	char path[240];
	text_print(path, sizeof(path), "%s\\maps\\ui.map", source->path);
	return stockmaps_build_need(campaign) + size_of(path) + movies_size(source->path, campaign) +
		(source->retail ? 2ull * source->retail->build_size : 16ull << 20) + (16ull << 20);
}

/* where on drive it goes: <drive>:\Games\Halo 1 Unlimited when the drive has a Games folder; else beside the Halo it's
built from, in the same folders on this drive (Llama, 2026-11-26: a Halo in F:\HaloCE\Halo1Retail -> F:\HaloCE\Halo 1
Unlimited, on G: G:\HaloCE\Halo 1 Unlimited; a Halo in a root folder, F:\Halo -> F:\Halo 1 Unlimited); a folder of
that name that isn't Halo 1 Unlimited's: "Halo 1 Unlimited 2", and so on */
static void own_folder_on(char drive, char const *source, char *out, int size)
{
	char base[200], mark[260], xbe[260];
	char const *slash;
	int n;
	text_print(base, sizeof(base), "%c:\\Games", drive);
	if (GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES)
	{
		/* (the folders the Halo is in, below its drive, its own name left off: "F:\HaloCE\Halo1Retail" -> "\HaloCE") */
		slash = source && source[0] && source[1] == ':' ? strrchr(source, '\\') : NULL;
		if (slash && slash > source + 2)
			text_print(base, sizeof(base), "%c:%.*s", drive, (int)(slash - (source + 2)), source + 2);
		else
			text_print(base, sizeof(base), "%c:", drive);
	}
	for (n = 1; n < 10; n++)
	{
		if (n == 1)
			text_print(out, size, "%s\\Halo 1 Unlimited", base);
		else
			text_print(out, size, "%s\\Halo 1 Unlimited %d", base, n);
		text_print(mark, sizeof(mark), "%s\\" BUILDING_MARK, out);
		text_print(xbe, sizeof(xbe), "%s\\default.xbe", out);
		if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES ||
			GetFileAttributesA(xbe) == INVALID_FILE_ATTRIBUTES)
			return;
	}
}

/* the build: 0 when Halo 1 Unlimited is in folder */
static int own_install(struct halo *source, char const *folder, int campaign)
{
	char path[260], to[260], error[256];
	unsigned long spun;
	int failed, i;
	struct halo *h = NULL;
	/* (an unfinished one: emptied) */
	text_print(path, sizeof(path), "%s\\" BUILDING_MARK, folder);
	if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
		delete_tree(folder, 0);
	/* (the folders above it too, when they aren't there: own_folder_on's G:\HaloCE for a Halo in F:\HaloCE\...) */
	for (i = 3; folder[i]; i++)
	{
		if (folder[i] == '\\')
		{
			text_print(to, sizeof(to), "%.*s", i, folder);
			CreateDirectoryA(to, NULL);
		}
	}
	CreateDirectoryA(folder, NULL);
	text_print(to, sizeof(to), "%s\\maps", folder);
	CreateDirectoryA(to, NULL);
	if (write_file(path, (unsigned char const *)"Halo 1 Unlimited is being built here.\r\n", 39))
	{
		say_in(COLOUR_RED, "Couldn't write in %s. Is the drive full, or failing?", folder);
		return 1;
	}
	step("Copying Halo's game and menus...");
	text_print(path, sizeof(path), "%s\\default.xbe", source->path);
	text_print(to, sizeof(to), "%s\\default.xbe", folder);
	failed = copy_file(path, to);
	text_print(path, sizeof(path), "%s\\maps\\ui.map", source->path);
	text_print(to, sizeof(to), "%s\\maps\\ui.map", folder);
	if (!failed)
		failed = copy_file(path, to);
	if (failed)
	{
		say_in(COLOUR_RED, "Couldn't copy Halo's files from %s. Is the drive full?", source->path);
		goto stop;
	}
	movies_copy(source->path, folder, campaign);
	/* the maps */
	step("Building the maps (about 5 minutes)...");
	receipt_later_total = campaign ? 0 : RECEIPT_STOCK_MP;
	stockmaps_flush_drive = flush_drive;
	stockmaps_log = log_file;
	package_free(&package);
	log_memory();
	spun = screen_spinner_busy_ms();
	failed = stockmaps_build(source->path, folder, campaign, stock_maps_say, error);
	log_spinner(spun);
	package_read();
	if (failed)
	{
		say_in(COLOUR_RED, "%s", error);
		goto stop;
	}
	/* the game */
	add_halo(folder);
	for (i = 0; i < halo_count; i++)
		if (!_stricmp(halos[i].path, folder))
			h = &halos[i];
	if (h)
		step("Making the game...");
	if (!h || install(h))
	{
		if (!h)
			say_in(COLOUR_RED, "The copied Halo in %s doesn't read as one.", folder);
		goto stop;
	}
	text_print(path, sizeof(path), "%s\\" BUILDING_MARK, folder);
	DeleteFileA(path);
	flush_drive(folder);
	last_install_write(folder);
	return 0;
stop:
	/* (what was built goes: the room's given back, and their Halo is as it was) */
	delete_tree(folder, 0);
	RemoveDirectoryA(folder);
	flush_drive(folder);
	return 1;
}

/* the room their Halo's game, maps and movies take (what remove_original deletes) */
static unsigned long long original_size(struct halo const *source)
{
	static char const *const patterns[] = {"maps\\*.map", "bink\\*.bik", "default.xbe"};
	WIN32_FIND_DATAA found;
	char path[260];
	HANDLE find;
	unsigned long long n = 0;
	int k;
	for (k = 0; k < 3; k++)
	{
		text_print(path, sizeof(path), "%s\\%s", source->path, patterns[k]);
		if ((find = FindFirstFileA(path, &found)) == INVALID_HANDLE_VALUE)
			continue;
		do
			n += found.nFileSizeLow;
		while (FindNextFileA(find, &found));
		FindClose(find);
	}
	return n;
}

/* their Halo, as Halo 1 Unlimited was built from it: deleted (its XBE, maps and movies; anything else in it stays) */
static void remove_original(struct halo *source)
{
	static char const *const parts[] = {"maps", "bink"};
	char path[260];
	int k;
	for (k = 0; k < 2; k++)
	{
		text_print(path, sizeof(path), "%s\\%s", source->path, parts[k]);
		delete_matching_files(path, k ? "*.bik" : "*.map");
		RemoveDirectoryA(path);
	}
	text_print(path, sizeof(path), "%s\\default.xbe", source->path);
	DeleteFileA(path);
	RemoveDirectoryA(source->path);
	flush_drive(source->path);
}

/* a Halo it can be built from: a retail Halo with all 23 of Halo's maps as they came */
static int buildable(struct halo *h)
{
	if (!ready(h) || h->state != RETAIL || h->unfinished)
		return 0;
	stockmaps_examine(h->path, &h->stock);
	return h->stock.retail_sized == STOCK_MAPS && !h->stock.small;
}

static void player_map_packs(void);
static int packs_look(char *stock, int size);
static unsigned long packs_saving(void);

/* room on a drive as people read it: "686 GB", "795 MB" (two at a time) */
static char const *space_text(unsigned long long bytes)
{
	static char text[2][24];
	static int next;
	char *t = text[next++ & 1];
	unsigned long mb = (unsigned long)(bytes >> 20);
	if (mb >= 10240)
		text_print(t, sizeof(text[0]), "%lu GB", mb / 1024);
	else
		text_print(t, sizeof(text[0]), "%lu MB", mb);
	return t;
}

static int player_install_flow(void)
{
	struct menu_item items[MENU_MOST];
	struct halo *choice[MENU_MOST];
	char line[160], folder[200], source_path[160];
	int n = 0, i, k, failed, campaign = 1, d, drive = -1, stage = 0;
	static char const drives[3] = {'F', 'G', 'E'};   /* (E: last: it's the console's smallest) */
	for (i = 0; i < halo_count && n < MENU_MOST; i++)
	{
		if (!ready(&halos[i]) || halos[i].state != RETAIL || halos[i].unfinished)
			continue;
		place_name(halos[i].path, items[n].text, sizeof(items[n].text));
		items[n].disabled = !buildable(&halos[i]);
		items[n].about = items[n].disabled ? "Some of this Halo's maps are missing or changed: Halo 1 Unlimited needs all "
			"of Halo's maps to build from. Copy Halo from its disc again." :
			"Halo 1 Unlimited is built from this Halo, into a folder of its own. This Halo isn't changed: it plays as it "
			"always has.";
		choice[n++] = &halos[i];
	}
	if (!n)
	{
		page("Install Halo 1 Unlimited",
			"No copy of Halo was found on your Xbox's hard drive.\n\n"
			"Copy Halo from its disc to E:, F: or G: (in a folder of its own, or in a Games folder), then start this "
			"installer again.");
		return 1;
	}
	/* one question at a time, each answer locked in (in grey) as the next is asked; B goes back a step: the Halo, the
	drive, the campaign, then Start */
	for (k = 0; k < n && items[k].disabled; k++)
		;
	if (k >= n)
		k = 0;
	for (;;)
	{
		unsigned long long with, without;
		char intro[240], drive_line[80], campaign_line[80];
		char const *lines[3];
		struct menu_item go[3];
		int r, count = 0, map[3];
		if (stage == 0)
		{
			r = menu("Choose your Halo", "Halo 1 Unlimited is built from a copy of Halo on your hard drive.", items, n, k);
			if (r == -2)
				continue;
			if (r < 0)
				return 1;
			k = r;
			text_print(source_path, sizeof(source_path), "%s", choice[k]->path);
			drive = -1;
			stage = 1;
			continue;
		}
		with = own_need(choice[k], 1);
		without = own_need(choice[k], 0);
		place_name(source_path, line, sizeof(line));
		text_print(intro, sizeof(intro), "Halo 1 Unlimited will be built from %s.", line);
		if (drive < 0)
		{
			/* (the one picked first: the first with room for it all, F: and G: before E:) */
			for (d = 0; d < 3 && drive < 0; d++)
				if (drive_free(drives[d]) >= with)
					drive = d;
			for (d = 0; d < 3 && drive < 0; d++)
				if (drive_free(drives[d]) >= without)
					drive = d;
			campaign = drive >= 0 && drive_free(drives[drive]) >= with;
		}
		if (drive >= 0)
			text_print(drive_line, sizeof(drive_line), "Install to: %c: (%s free)", drives[drive],
				space_text(drive_free(drives[drive])));
		text_print(campaign_line, sizeof(campaign_line), campaign ? "Campaign: Included (+%s)" :
			"Campaign: Not included (saves %s)", space_text(with - without));
		if (stage == 1)
		{
			/* the drive: E:, F: and G: as they're there */
			static char const order[3] = {'E', 'F', 'G'};
			int chosen = 0, dd;
			for (dd = 0; dd < 3; dd++)
			{
				int at;
				ULARGE_INTEGER available, total, free_total;
				char root[4] = {order[dd], ':', '\\', 0};
				if (!GetDiskFreeSpaceExA(root, &available, &total, &free_total))
					continue;
				for (at = 0; at < 3 && drives[at] != order[dd]; at++)
					;
				text_print(go[count].text, sizeof(go[count].text), "%c: (%s free)", order[dd],
					space_text(available.QuadPart));
				go[count].disabled = available.QuadPart < without;
				go[count].about = go[count].disabled ? "There isn't room for Halo 1 Unlimited on this drive." :
					"Halo 1 Unlimited goes in a folder of its own on this drive.";
				if (at == drive)
					chosen = count;
				map[count++] = at;
			}
			if (!count)
			{
				page("Install Halo 1 Unlimited", "No drive was found to install to.");
				return 1;
			}
			lines[0] = "Install to:";
			r = menu_steps("Install Halo 1 Unlimited", intro, lines, 1, 0, go, count, chosen);
			if (r == -2)
				continue;
			if (r < 0)
			{
				stage = 0;
				continue;
			}
			drive = map[r];
			if (drive_free(drives[drive]) < with)
				campaign = 0;
			stage = 2;
			continue;
		}
		if (stage == 2)
		{
			char question[80];
			int room = drive_free(drives[drive]) >= with;
			text_print(question, sizeof(question), "Campaign: Included? (+%s)", space_text(with - without));
			text_print(go[0].text, sizeof(go[0].text), "Yes");
			go[0].about = room ? "Halo's campaign is installed too." :
				"There isn't room on this drive for the campaign too: leave it out, or go back and pick another drive.";
			go[0].disabled = !room;
			text_print(go[1].text, sizeof(go[1].text), "No");
			go[1].about = "Only multiplayer: Halo's campaign can't be played in Halo 1 Unlimited.";
			go[1].disabled = 0;
			lines[0] = drive_line;
			lines[1] = question;
			r = menu_steps("Install Halo 1 Unlimited", intro, lines, 2, 1, go, 2, campaign && room ? 0 : 1);
			if (r == -2)
				continue;
			if (r < 0)
			{
				stage = 1;
				continue;
			}
			campaign = r == 0;
			stage = 3;
			continue;
		}
		/* Start */
		{
			static char summary[300];
			unsigned long long need = campaign ? with : without;
			own_folder_on(drives[drive], choice[k]->path, folder, sizeof(folder));
			text_print(go[0].text, sizeof(go[0].text), "Start install (about 6 minutes, needs %s)", space_text(need));
			text_print(summary, sizeof(summary), "Installs Halo 1 Unlimited to %s. It will use %s.", folder,
				space_text(need));
			go[0].about = summary;
			go[0].disabled = drive_free(drives[drive]) < need;
			lines[0] = drive_line;
			lines[1] = campaign_line;
			r = menu_steps("Install Halo 1 Unlimited", intro, lines, 2, 2, go, 1, 0);
			if (r == -2)
				continue;
			if (r < 0)
			{
				stage = 2;
				continue;
			}
			break;
		}
	}
	own_folder_on(drives[drive], choice[k]->path, folder, sizeof(folder));
	task_begin("Installing Halo 1 Unlimited", "Building Halo 1 Unlimited from your Halo...");
	failed = own_install(choice[k], folder, campaign);
	task_end();
	halo_changed(folder, 1);
	if (failed)
	{
		task_wait();
		return 1;
	}
	for (;;)
	{
		struct menu_item done[4];
		enum { DONE_PLAY, DONE_REMOVE, DONE_PACKS, DONE_BACK };
		int actions[4], count = 0, packs = 0;
		struct halo *h1u = player_install(), *original = NULL;
		static char about[300];
		for (i = 0; i < halo_count; i++)
		{
			if (!_stricmp(halos[i].path, source_path))
				original = &halos[i];
			else if (can_import(&halos[i]) && !halos[i].unfinished)
				packs++;
		}
		text_print(done[count].text, sizeof(done[count].text), "Play now");
		done[count].about = NULL;
		done[count].disabled = 0;
		actions[count++] = DONE_PLAY;
		if (original)
		{
			text_print(done[count].text, sizeof(done[count].text), "Remove your original Halo");
			text_print(about, sizeof(about), "Frees about %lu MB. Halo 1 Unlimited can rebuild it exactly any time "
				"(More..., Rebuild original Halo).%s", (unsigned long)(original_size(original) >> 20), campaign ? "" :
				" Its campaign can't be: it wasn't installed.");
			done[count].about = about;
			done[count].disabled = 0;
			actions[count++] = DONE_REMOVE;
		}
		if (packs)
		{
			text_print(done[count].text, sizeof(done[count].text), "We found %d map pack%s on your Xbox: set %s up",
				packs, packs == 1 ? "" : "s", packs == 1 ? "it" : "them");
			static char packs_about[400];
			char stock[200], saving[32];
			unsigned long saved = packs_look(stock, sizeof(stock)) ? packs_saving() : 0;
			if (saved >= 1000)
				text_print(saving, sizeof(saving), "%lu.%lu GB", saved / 1024, saved % 1024 * 10 / 1024);
			else
				text_print(saving, sizeof(saving), "%lu MB", saved);
			text_print(packs_about, sizeof(packs_about), "Consolidate them: they're shrunk to fit in Halo 1 Unlimited and "
				"use a lot less space%s%s%s. Or link them to Halo 1 Unlimited and leave your map packs as they are. "
				"Either way, you can play them online in Halo 1 Unlimited.", saved ? " (around " : "",
				saved ? saving : "", saved ? " less, once you delete the originals" : "");
			done[count].about = packs_about;
			done[count].disabled = 0;
			actions[count++] = DONE_PACKS;
		}
		text_print(done[count].text, sizeof(done[count].text), "Back to the menu");
		done[count].about = NULL;
		done[count].disabled = 0;
		actions[count++] = DONE_BACK;
		k = menu("Halo 1 Unlimited is installed", "Everything's ready.", done, count, 0);
		if (k < 0 || actions[k] == DONE_BACK)
			return 0;
		if (actions[k] == DONE_PLAY && h1u)
			launch_halo(h1u);
		if (actions[k] == DONE_PACKS)
		{
			player_map_packs();
			continue;
		}
		if (actions[k] != DONE_REMOVE)
			return 0;
		{
			struct menu_item sure[2] = {{"Remove it", NULL, 0}, {"Keep it", NULL, 0}};
			if (menu("Remove your original Halo?", campaign ? "Its game, maps and movies are deleted. Halo 1 Unlimited "
				"can rebuild it exactly any time." : "Its game, maps and movies are deleted. Halo 1 Unlimited can rebuild "
				"its multiplayer maps, but NOT its campaign: the campaign wasn't installed.", sure, 2, 1) != 0)
				continue;
			char gone[160];
			text_print(gone, sizeof(gone), "%s", original->path);
			task_begin("Removing your original Halo", "Deleting its game, maps and movies...");
			remove_original(original);
			task_end();
			halo_changed(gone, 1);
			return 0;
		}
	}
}

/* Update: a newer version online, installed (the game's updater's way: no questions) */
static void player_update(struct halo *h)
{
	int changed;
	if (h->up_to_date)
	{
		task_begin("Checking for updates", "Looking online for a newer Halo 1 Unlimited...");
		auto_mode = 1;
		changed = check_updates();
		if (changed)
			offer_installer_update();      /* (a newer installer: it starts, and Update is offered there) */
		auto_mode = 0;
		if (!changed)
			step("Halo 1 Unlimited is up to date.");
		task_end();
		halo_changed(h->path, 0);        /* (the package beside the installer is newer: is it the one installed?) */
		if (!changed)
		{
			task_wait();
			return;
		}
		h = player_install();
		if (!h)
			return;
		if (h->up_to_date)
		{
			/* (the new package's game is the one installed) */
			check_state = CHECK_NEWEST;
			page("Halo 1 Unlimited is up to date", "Halo 1 Unlimited is the newest version.");
			return;
		}
	}
	/* (the package beside the installer is installed: the online check's answer stands against it) */
	if (check_state == CHECK_NEWER && package_ok && !newer_package(check_release, check_build))
		check_state = CHECK_NEWEST;
	text_print(task_title, sizeof(task_title), "Updating Halo 1 Unlimited");
	task_begin("Updating Halo 1 Unlimited", "Updating Halo 1 Unlimited (about a minute)...");
	if (!install(h))
	{
		step("Done: Halo 1 Unlimited v1.%lu is installed.", package.build);
		offer_stock_maps(h);
	}
	task_end();
	look_again(h);
	task_wait();
}

/* ---- the update asked about first thing: "There's a new update available. Update now?" Yes / No / Always
(installer_settings.txt "update = always": from then on no question) ---- */

static int update_asked;

static int update_always(void)
{
	char line[80];
	FILE *file = fopen(SETTINGS_PATH, "r");
	int always = 0;
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file))
	{
		char *at = line;
		while (*at == ' ' || *at == '\t')
			at++;
		if (!_strnicmp(at, "update", 6) && strstr(at, "=") && strstr(at, "always"))
			always = 1;
	}
	fclose(file);
	return always;
}

static void update_always_set(void)
{
	static char const text[] = "update = always\r\n";
	write_file(SETTINGS_PATH, (unsigned char const *)text, sizeof(text) - 1);
	flush_own_drive();
	log_file("updates: always");
}

/* Halo 1 Unlimited's game brought up to the package beside the installer (after a download) */
static void update_installed(void)
{
	struct halo *h;
	find_install();                      /* (v1.142: the last install, not the drives looked round) */
	h = player_install();
	check_state = CHECK_NEWEST;
	if (!h || h->up_to_date)
		return;                            /* (nothing installed: Install uses the new package) */
	task_begin("Updating Halo 1 Unlimited", "Updating Halo 1 Unlimited (about a minute)...");
	if (!install(h))
	{
		step("Done: Halo 1 Unlimited v1.%lu is installed.", package.build);
		offer_stock_maps(h);
	}
	task_end();
	look_again(h);
	task_wait();
}

/* the newer package downloaded; the newer installer it carries put in place and started (it goes on with the update:
CONTINUE_PATH); then Halo 1 Unlimited updated */
static void update_go(void)
{
	int changed;
	task_begin("Updating Halo 1 Unlimited", "Updating...");
	auto_mode = 1;
	changed = check_updates();
	if (changed && package_ok && package.installer_version > INSTALLER_VERSION && trusted(signature))
	{
		static char const text[] = "update\r\n";
		write_file(CONTINUE_PATH, (unsigned char const *)text, sizeof(text) - 1);
		flush_own_drive();
		offer_installer_update();          /* (the new installer starts: it doesn't come back here) */
		DeleteFileA(CONTINUE_PATH);        /* (it didn't) */
	}
	auto_mode = 0;
	task_end();
	if (!changed)
	{
		task_wait();
		return;
	}
	update_installed();
}

/* a newer version online: asked about (once a run), or with "always" just installed */
static void update_prompt(void)
{
	struct menu_item items[3] = {
		{"Yes", "Updates Halo 1 Unlimited now (a minute or two).", 0},
		{"No", "Not now: the main menu offers it.", 0},
		{"Always", "Yes, and from now on updates are installed without asking.", 0}};
	char intro[160];
	int k;
	update_asked = 1;
	if (!update_always())
	{
		text_print(intro, sizeof(intro), "There's a new update available (%s). Update now?", check_version());
		fetch_start();                   /* (downloading while it's asked about) */
		k = menu("A new update is available", intro, items, 3, 0);
		if (k == 2)
			update_always_set();
		else if (k != 0)
		{
			fetch_cancel();
			return;
		}
	}
	update_go();
}

/* first thing at the start: online for a newer version (up to 15 s waited for, B skips: a slower answer is asked
about at the main menu), before Halo folders are looked for */
static void startup_update_check(void)
{
	DWORD start = GetTickCount();
	check_start();
	player_heading("Checking for updates");
	screen_text(MENU_TOP, 1, COLOUR_GREY, "Looking online for a newer Halo 1 Unlimited...");
	screen_text(MENU_TOP + 2, 1, COLOUR_DIM, "B: skip");
	screen_show();
	while (check_state == CHECK_RUNNING && GetTickCount() - start < 15000)
	{
		if (poll_input() == B)
			break;
		Sleep(16);
	}
	if (check_state == CHECK_NEWER)
		update_prompt();
}

/* ---- Map Packs: the modded Halos' folders on the drives (the other Halo folders), each one's status and what can be done:
Consolidate (recommended: its maps made small in Halo 1 Unlimited's folder, drawing on stock.shared; the ones map sharing
sends), Link (its maps played where they are), take it out again, and, consolidated, its own folder deleted. Known
packs (packs_known.h) show what consolidating saved and took on a console; others an estimate ---- */

#define MOST_PACKS 64
static struct pack_scan *pack_scans;        /* (MOST_PACKS: the packs found, kept while packs_listed) */
static int pack_count;

static int unshrunk_small(struct pack_scan const *scan, char *missing, int *small);

/* 1 when it's consolidated: its maps' small copies there (a pack's shared file in its folder, or a small copy beside a
map's link: a pack of one map, or whose maps share nothing, has no shared file) */
static int consolidated(struct pack_scan const *scan)
{
	char pattern[240], missing[40];
	WIN32_FIND_DATAA found;
	HANDLE find;
	int small;
	text_print(pattern, sizeof(pattern), "%s\\maps\\%s\\*.shared", packs_storage, scan->key);
	if ((find = FindFirstFileA(pattern, &found)) != INVALID_HANDLE_VALUE)
	{
		FindClose(find);
		return 1;
	}
	unshrunk_small(scan, missing, &small);
	return small > 0;
}

/* an imported map with no small copy beside its link (its name in missing[40]): 0 when every one has one */
static int unshrunk(struct pack_scan const *scan, char *missing)
{
	int small;
	return unshrunk_small(scan, missing, &small);
}

/* (and how many have one: small) */
static int unshrunk_small(struct pack_scan const *scan, char *missing, int *small)
{
	WIN32_FIND_DATAA found, inner;
	char pattern[240], path[260];
	HANDLE find, maps;
	int n = 0;
	missing[0] = 0;
	*small = 0;
	text_print(pattern, sizeof(pattern), "%s\\maps\\%s\\*", packs_storage, scan->key);
	if ((find = FindFirstFileA(pattern, &found)) == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == '.')
			continue;
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\map.link", packs_storage, scan->key, found.cFileName);
		if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
			continue;
		text_print(path, sizeof(path), "%s\\maps\\%s\\%s\\*.map", packs_storage, scan->key, found.cFileName);
		if ((maps = FindFirstFileA(path, &inner)) != INVALID_HANDLE_VALUE)
		{
			FindClose(maps);
			(*small)++;
			continue;
		}
		if (!n++)
			text_print(missing, 40, "%s", found.cFileName);
	} while (FindNextFileA(find, &found));
	FindClose(find);
	return n;
}

/* what consolidating it saves and takes: a known pack's numbers, or an estimate ("about"): a quarter of its size (a
third with a campaign: levels share less), 0.7 s a MB on a console */
static void pack_numbers(struct pack_scan const *scan, unsigned long *after_mb, unsigned long *minutes, int *known)
{
	struct pack_known const *k = packs_known(scan->fingerprint);
	unsigned long mb = (unsigned long)(scan->size >> 20);
	*known = k != NULL;
	*after_mb = k ? k->after_mb : scan->campaign ? mb / 3 : mb / 4;
	*minutes = k && k->seconds ? (k->seconds + 59) / 60 : (mb * 7 / 10 + 59) / 60;
	if (!*minutes)
		*minutes = 1;
}

static void player_pack(struct halo *h, struct pack_scan *scan, char const *stock)
{
	for (;;)
	{
		struct menu_item items[6];
		int actions[6], n = 0, k, imported = scan->multiplayer_imported || scan->campaign_imported, known, is_small;
		unsigned long after, minutes, mb = (unsigned long)(scan->size >> 20);
		char intro[300], stock_index[220], consolidate_about[300], name[64], missing[40], kinds[64];
		enum { PACK_CONSOLIDATE, PACK_LINK, PACK_REMOVE, PACK_DELETE, PACK_BACK };
		text_print(name, sizeof(name), "%s", strrchr(scan->path, '\\') ? strrchr(scan->path, '\\') + 1 : scan->path);
		pack_numbers(scan, &after, &minutes, &known);
		is_small = consolidated(scan);
		text_print(stock_index, sizeof(stock_index), "%s\\stock.index", stock);
		if (scan->multiplayer)
			text_print(kinds, sizeof(kinds), "%d multiplayer map%s%s", scan->multiplayer, scan->multiplayer == 1 ? "" :
				"s", scan->campaign ? " and a campaign" : "");
		else
			text_print(kinds, sizeof(kinds), scan->campaign ? "A campaign" : "No maps");
		text_print(intro, sizeof(intro), "%s, %lu MB. %s", kinds, mb, is_small ?
			"It's consolidated: its maps are small in Halo 1 Unlimited." : imported ?
			"It's linked: Halo 1 Unlimited plays its maps where they are." : "It isn't in Halo 1 Unlimited yet.");
		if (!is_small && (scan->multiplayer || scan->campaign))
		{
			text_print(items[n].text, sizeof(items[n].text), "Consolidate (recommended)");
			text_print(consolidate_about, sizeof(consolidate_about), "Shrinks %s from %s%lu MB to %s%lu MB (%s%lu "
				"minute%s). Small maps are quicker to send to other players online. The pack's own folder isn't "
				"changed.", name, "", mb, known ? "" : "about ", after, known ? "" : "about ", minutes,
				minutes == 1 ? "" : "s");
			items[n].about = consolidate_about;
			items[n].disabled = GetFileAttributesA(stock_index) == INVALID_FILE_ATTRIBUTES;
			if (items[n].disabled)
				items[n].about = "Halo 1 Unlimited's own maps need to be made small first: install it again.";
			actions[n++] = PACK_CONSOLIDATE;
		}
		if (!imported && (scan->multiplayer || scan->campaign))
		{
			text_print(items[n].text, sizeof(items[n].text), "Link");
			items[n].about = "Keeps your pack as it is: Halo 1 Unlimited plays its maps from its own folder. Adds less "
				"than 1 MB.";
			items[n].disabled = 0;
			actions[n++] = PACK_LINK;
		}
		if (imported)
		{
			text_print(items[n].text, sizeof(items[n].text), "Take it out of Halo 1 Unlimited");
			items[n].about = is_small ? "Its small maps are deleted from Halo 1 Unlimited. The pack's own folder isn't "
				"changed." : "Its maps leave Halo 1 Unlimited's map list. The pack's own folder isn't changed.";
			items[n].disabled = 0;
			actions[n++] = PACK_REMOVE;
		}
		if (is_small)
		{
			int left = unshrunk(scan, missing);
			static char delete_about[300];
			text_print(items[n].text, sizeof(items[n].text), "Delete the pack's own folder");
			if (left)
				text_print(delete_about, sizeof(delete_about), "%d of its maps (%s%s) have no small copy in Halo 1 "
					"Unlimited: they'd go with the folder. Consolidate it again first.", left, missing,
					left > 1 ? " and more" : "");
			else
				text_print(delete_about, sizeof(delete_about), "Frees %lu MB. Halo 1 Unlimited keeps its small maps. The "
					"pack's own folder can't be rebuilt: its game and menus aren't kept.", mb);
			items[n].about = delete_about;
			items[n].disabled = left != 0;
			actions[n++] = PACK_DELETE;
		}
		text_print(items[n].text, sizeof(items[n].text), "Back");
		items[n].about = NULL;
		items[n].disabled = 0;
		actions[n++] = PACK_BACK;
		k = menu(name, intro, items, n, 0);
		if (k < 0 || actions[k] == PACK_BACK)
			return;
		if (actions[k] == PACK_CONSOLIDATE)
		{
			char title[80];
			text_print(title, sizeof(title), "Consolidating %s", name);
			unsigned long before, after;
			task_begin(title, "Making its maps small...");
			consolidate_pack(h, scan, stock, &before, &after);
			task_end();
			task_wait();
		}
		else if (actions[k] == PACK_LINK)
		{
			char error[256];
			task_begin("Linking the pack", "Adding its maps to Halo 1 Unlimited's map list...");
			if (packs_import(scan, 1, scan->campaign != 0, packs_say, error))
				say_in(COLOUR_RED, "%s", error);
			else
				step("Done: its maps are in Halo 1 Unlimited's map list.");
			task_end();
			task_wait();
		}
		else if (actions[k] == PACK_REMOVE)
		{
			task_begin("Taking the pack out", NULL);
			packs_remove(scan, 1, 1, packs_say);
			task_end();
		}
		else if (actions[k] == PACK_DELETE)
		{
			struct menu_item sure[2] = {{"Delete it", NULL, 0}, {"Keep it", NULL, 0}};
			char warn[300];
			text_print(warn, sizeof(warn), "%s and everything in it is deleted. Halo 1 Unlimited keeps the pack's small "
				"maps, but the folder itself (its own game and menus) can't be rebuilt.", scan->path);
			if (menu("Delete the pack's own folder?", warn, sure, 2, 1) != 0)
				continue;
			task_begin("Deleting the pack's folder", NULL);
			delete_tree(scan->path, 0);
			RemoveDirectoryA(scan->path);
			flush_drive(scan->path);
			task_end();
			halo_changed(scan->path, 0);      /* (off the Halo folders' list; Map Packs drops it) */
			return;
		}
		packs_scan(h->path, stock, scan);
	}
}

/* the Halo folder in the list at path; NULL: none (its folder was deleted) */
static struct halo *halo_at(char const *path)
{
	int i;
	for (i = 0; i < halo_count; i++)
		if (!_stricmp(halos[i].path, path))
			return &halos[i];
	return NULL;
}

/* the packs, looked at (once: then only the one chosen, after what was done to it); stock: stock.index's path. 0: no
room for the list */
static int packs_look(char *stock, int size)
{
	int i;
	if (!pack_scans)
		pack_scans = malloc(MOST_PACKS * sizeof(*pack_scans));
	if (!pack_scans)
		return 0;
	stock_maps_folder(stock, size);
	text_print(packs_storage, sizeof(packs_storage), "%.*s", stock[0] ? (int)strlen(stock) - 5 : 0, stock);
	if (packs_listed)
		return 1;
	{
		unsigned long started = GetTickCount(), hits, misses;
		char line[160];
		for (i = 0, pack_count = 0; i < halo_count && pack_count < MOST_PACKS; i++)
		{
			struct pack_scan *scan = &pack_scans[pack_count];
			if (!can_import(&halos[i]) || halos[i].unfinished)
				continue;
			packs_scan(halos[i].path, stock, scan);
			if (scan->multiplayer || scan->campaign || scan->multiplayer_imported || scan->campaign_imported)
				pack_count++;
		}
		scan_cache_save(SCAN_CACHE_PATH);
		scan_cache_counts(&hits, &misses);
		text_print(line, sizeof(line), "time: Map Packs looked at: %d packs in %lu ms (kept facts so far: %lu used, %lu "
			"looked at)", pack_count, GetTickCount() - started, hits, misses);
		log_file(line);
	}
	packs_listed = 1;
	return 1;
}

/* what consolidating every pack that isn't yet would save, in MB (known packs' numbers, else estimates) */
static unsigned long packs_saving(void)
{
	unsigned long saved = 0, after, minutes;
	int n, known;
	for (n = 0; n < pack_count; n++)
	{
		unsigned long mb = (unsigned long)(pack_scans[n].size >> 20);
		if (consolidated(&pack_scans[n]))
			continue;
		pack_numbers(&pack_scans[n], &after, &minutes, &known);
		if (mb > after)
			saved += mb - after;
	}
	return saved;
}

/* "1 h 20 min", "45 min" */
static void say_minutes(char *out, int size, unsigned long minutes)
{
	if (minutes >= 60)
		text_print(out, size, "%lu h %lu min", minutes / 60, minutes % 60);
	else
		text_print(out, size, "%lu min", minutes ? minutes : 1);
}

/* the packs Consolidate all does (all = 1: not consolidated yet) or Link all does (all = 0: not in it at all) */
static int pack_wanted(struct pack_scan const *scan, int consolidating)
{
	int linked = scan->multiplayer_imported || scan->campaign_imported;
	if (!scan->multiplayer && !scan->campaign)
		return 0;
	return consolidating ? !consolidated(scan) : !linked && !consolidated(scan);
}

/* Consolidate all / Link all: every such pack in turn, one line each on the screen (its maps counted, then what it went
from and to), with nothing to answer: started, it can be left to run. A pack that fails is said and passed. The end waits
for a button, so what was done is there to read */
static void packs_all(int consolidating, char const *stock)
{
	unsigned long total_before = 0, total_after = 0;
	int n, done = 0, failed = 0, wanted = 0;
	DWORD started = GetTickCount();
	char took[32];
	for (n = 0; n < pack_count; n++)
		wanted += pack_wanted(&pack_scans[n], consolidating);
	task_begin(consolidating ? "Consolidating all your map packs" : "Linking all your map packs", NULL);
	log_file(consolidating ? "Consolidate all:" : "Link all:");
	for (n = 0; n < pack_count; n++)
	{
		struct pack_scan *scan = &pack_scans[n];
		struct halo *h = halo_at(scan->path);
		struct receipt_entry *e;
		char const *name = strrchr(scan->path, '\\') ? strrchr(scan->path, '\\') + 1 : scan->path;
		unsigned long before = 0, after = 0;
		int made;
		if (!h || !pack_wanted(scan, consolidating))
			continue;
		receipt_batch = 0;
		e = receipt_add(RECEIPT_ACTION);
		text_print(e->text, sizeof(e->text), "%s", name);
		/* (the title says which pack of them) */
		text_print(task_title, sizeof(task_title), "%s (%d of %d)", consolidating ? "Consolidating all your map packs" :
			"Linking all your map packs", done + failed + 1, wanted);
		player_heading(task_title);
		receipt_show();
		receipt_batch = 1;
		if (consolidating)
		{
			char title[300];
			text_print(title, sizeof(title), "Consolidating %s", scan->path);
			log_file(title);
			made = consolidate_pack(h, scan, stock, &before, &after);
		}
		else
		{
			char error[256];
			made = packs_import(scan, 1, scan->campaign != 0, packs_say, error) ? -1 : 1;
			if (made < 0)
				say_in(COLOUR_RED, "%s", error);
		}
		receipt_batch = 0;
		/* (its line: what it went from and to; it's marked done by the next) */
		for (e = &receipt[receipt_count - 1]; e > receipt && e->kind != RECEIPT_ACTION; e--)
			;
		if (made < 0)
		{
			failed++;
			text_print(e->right, sizeof(e->right), "failed");
		}
		else
		{
			done++;
			total_before += before;
			total_after += after;
			if (consolidating)
				text_print(e->right, sizeof(e->right), made ? "%lu -> %lu MB" : "nothing to shrink", before, after);
			else
				text_print(e->right, sizeof(e->right), "linked");
		}
		e->done = 1;
		packs_scan(h->path, stock, scan);
		receipt_show();
	}
	say_minutes(took, sizeof(took), (GetTickCount() - started + 30000) / 60000);
	if (consolidating)
		step("Done: %d pack%s consolidated, %lu MB -> %lu MB, in %s.%s", done, done == 1 ? "" : "s", total_before,
			total_after, took, failed ? "" : " Your map packs' own folders are left as they are.");
	else
		step("Done: %d pack%s linked.", done, done == 1 ? "" : "s");
	if (failed)
		say_in(COLOUR_YELLOW, "%d didn't work: installer_log.txt says why.", failed);
	task_end();
	task_problem = 1;                    /* (it was left to run: the end waits to be read) */
	task_wait();
}

static void player_map_packs(void)
{
	struct menu_item *items;
	char stock[200];
	char (*abouts)[240];
	static char consolidate_all_about[300], link_all_about[200];
	int n, k = 0, first, to_consolidate, to_link, all_consolidate, all_link, pack_k = -1;
	items = malloc((MOST_PACKS + 2) * sizeof(*items));
	abouts = malloc(MOST_PACKS * sizeof(*abouts));
	if (!items || !abouts)
	{
		free(items);
		free(abouts);
		return;
	}
	for (;;)
	{
		if (!packs_listed)
		{
			player_heading("Classic Map Packs");
			screen_text(MENU_TOP, 1, COLOUR_GREY, "Looking at your map packs...");
			screen_show();
		}
		if (!packs_look(stock, sizeof(stock)))
			break;
		/* (first: Consolidate all and Link all, when there's a pack for them) */
		{
			unsigned long minutes = 0, saved = 0, after, each;
			char stock_index[220], took[32];
			int known;
			to_consolidate = to_link = 0;
			for (n = 0; n < pack_count; n++)
			{
				if (pack_wanted(&pack_scans[n], 1))
				{
					to_consolidate++;
					pack_numbers(&pack_scans[n], &after, &each, &known);
					minutes += each;
					if ((unsigned long)(pack_scans[n].size >> 20) > after)
						saved += (unsigned long)(pack_scans[n].size >> 20) - after;
				}
				to_link += pack_wanted(&pack_scans[n], 0);
			}
			first = 0;
			all_consolidate = all_link = -1;
			if (to_consolidate > 1)
			{
				text_print(stock_index, sizeof(stock_index), "%s\\stock.index", stock);
				say_minutes(took, sizeof(took), minutes);
				text_print(items[first].text, sizeof(items[first].text), "Consolidate all (%d packs, about %s)",
					to_consolidate, took);
				text_print(consolidate_all_about, sizeof(consolidate_all_about), "Every pack that isn't consolidated yet, "
					"one after another: about %s, nothing to answer, so you can leave it running. Saves around %lu MB once "
					"you delete the originals. The packs' own folders aren't changed.", took, saved);
				items[first].about = consolidate_all_about;
				items[first].disabled = GetFileAttributesA(stock_index) == INVALID_FILE_ATTRIBUTES;
				all_consolidate = first++;
			}
			if (to_link > 1)
			{
				text_print(items[first].text, sizeof(items[first].text), "Link all (%d packs)", to_link);
				text_print(link_all_about, sizeof(link_all_about), "Every pack that isn't in Halo 1 Unlimited yet goes "
					"in its map list as it is. Quick: a minute or so.");
				items[first].about = link_all_about;
				items[first].disabled = 0;
				all_link = first++;
			}
		}
		for (n = 0; n < pack_count; n++)
		{
			struct pack_scan *scan = &pack_scans[n];
			unsigned long after, minutes;
			int known;
			char const *last;
			int small = consolidated(scan), linked = scan->multiplayer_imported || scan->campaign_imported;
			unsigned long mb = (unsigned long)(scan->size >> 20);
			last = strrchr(scan->path, '\\') ? strrchr(scan->path, '\\') + 1 : scan->path;
			pack_numbers(scan, &after, &minutes, &known);
			text_print(items[first + n].text, sizeof(items[0].text), "%-36.36s %5lu MB  %s", last, mb,
				small ? "Consolidated" : linked ? "Linked" : "Not added");
			/* (what it is now, and what choosing it can do) */
			if (small)
				text_print(abouts[n], sizeof(abouts[n]), "It's in Halo 1 Unlimited, consolidated: its maps are small "
					"and ready to play.");
			else if (linked)
				text_print(abouts[n], sizeof(abouts[n]), "It's in Halo 1 Unlimited, linked: its maps play from where "
					"they are. Consolidate it to shrink it from %lu MB to %s%lu MB.", mb, known ? "" : "about ", after);
			else
				text_print(abouts[n], sizeof(abouts[n]), "It isn't in Halo 1 Unlimited yet. Consolidate it to shrink "
					"it from %lu MB to %s%lu MB, or link it as it is.", mb, known ? "" : "about ", after);
			items[first + n].about = abouts[n];
			items[first + n].disabled = 0;
		}
		if (!n)
		{
			page("Classic Map Packs", "No map packs were found on your hard drive.\n\nA map pack is a modded Halo's folder (its "
				"default.xbe and maps) on E:, F: or G:.");
			break;
		}
		if (pack_k >= 0)
			k = first + pack_k;         /* (the pack chosen last, wherever the rows above it went) */
		pack_k = -1;
		if ((k = menu("Classic Map Packs", "The map packs on your Xbox:", items, first + n, k)) < 0)
			break;
		if (k == all_consolidate || k == all_link)
		{
			struct menu_item sure[2] = {{"Start", NULL, 0}, {"Back", NULL, 0}};
			char line[300];
			if (k == all_consolidate)
				text_print(line, sizeof(line), "%d packs are made small, one after another. It takes a while: you can leave "
					"it running. Your packs' own folders aren't changed.", to_consolidate);
			else
				text_print(line, sizeof(line), "%d packs go in Halo 1 Unlimited's map list as they are.", to_link);
			if (menu(k == all_consolidate ? "Consolidate all your map packs?" : "Link all your map packs?", line, sure, 2,
				0) == 0)
				packs_all(k == all_consolidate, stock);
			continue;
		}
		k -= first;
		if (!halo_at(pack_scans[k].path))
			packs_listed = 0;
		else
			player_pack(halo_at(pack_scans[k].path), &pack_scans[k], stock);
		if (!halo_at(pack_scans[k].path) && packs_listed)
		{
			/* (its folder was deleted: off the list) */
			memmove(&pack_scans[k], &pack_scans[k + 1], (pack_count - k - 1) * sizeof(*pack_scans));
			pack_count--;
			k = k < pack_count ? k : pack_count - 1;
		}
		pack_k = k;
	}
	free(items);
	free(abouts);
}

/* Rebuild original Halo: their Halo made again from Halo 1 Unlimited (stockmaps_rebuild: each map exact), in a folder
of its own beside it: <drive>:\Games\Halo (or "Halo 2"... when that's taken) */
static void rebuild_original(struct halo *h)
{
	char folder[200], base[64], path[260], to[260], error[256], mark[240];
	int n, failed;
	unsigned long spun;
	text_print(base, sizeof(base), "%c:\\Games", h->path[0]);
	if (GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES)
		text_print(base, sizeof(base), "%c:", h->path[0]);
	/* (a free name, or one a rebuild was stopped in: BUILDING_MARK still there) */
	for (n = 1; n < 10; n++)
	{
		if (n == 1)
			text_print(folder, sizeof(folder), "%s\\Halo", base);
		else
			text_print(folder, sizeof(folder), "%s\\Halo %d", base, n);
		text_print(mark, sizeof(mark), "%s\\" BUILDING_MARK, folder);
		if (GetFileAttributesA(folder) == INVALID_FILE_ATTRIBUTES || GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES)
			break;
	}
	task_begin("Rebuilding your original Halo", "Rebuilding Halo's maps exactly as they came (about half an hour)...");
	/* (BUILDING_MARK in it until it's done: stopped halfway (the dashboard's buttons, a power cut), it's no Halo to
	play, and the next rebuild empties it and starts again) */
	if (GetFileAttributesA(mark) != INVALID_FILE_ATTRIBUTES)
		delete_tree(folder, 0);
	CreateDirectoryA(folder, NULL);
	text_print(to, sizeof(to), "%s\\maps", folder);
	CreateDirectoryA(to, NULL);
	if (write_file(mark, (unsigned char const *)"Halo is being rebuilt here.\r\n", 29))
	{
		say_in(COLOUR_RED, "Couldn't write in %s. Is the drive full, or failing?", folder);
		task_end();
		task_wait();
		return;
	}
	flush_drive(folder);
	text_print(path, sizeof(path), "%s\\maps\\ui.map", h->path);
	text_print(to, sizeof(to), "%s\\maps\\ui.map", folder);
	failed = copy_file(path, to);
	stockmaps_flush_drive = flush_drive;
	stockmaps_log = log_file;
	package_free(&package);
	spun = screen_spinner_busy_ms();
	if (!failed)
		failed = stockmaps_rebuild(h->path, folder, stock_maps_say, error);
	else
		text_print(error, sizeof(error), "Couldn't copy Halo's menus (ui.map). Is the drive full?");
	log_spinner(spun);
	package_read();
	if (!failed)
		movies_copy(h->path, folder, 1);
	text_print(path, sizeof(path), "%s\\default_retail.xbe", h->path);
	text_print(to, sizeof(to), "%s\\default.xbe", folder);
	if (!failed && copy_file(path, to))
	{
		failed = 1;
		text_print(error, sizeof(error), "Couldn't copy Halo's game (default_retail.xbe). Is the drive full?");
	}
	if (failed)
	{
		/* (what was rebuilt goes: the room's given back) */
		say_in(COLOUR_RED, "%s", error);
		delete_tree(folder, 0);
		RemoveDirectoryA(folder);
	}
	else
	{
		flush_drive(folder);       /* (everything in it written before the mark goes) */
		DeleteFileA(mark);
		step("Done: your original Halo is in %s.", folder);
	}
	flush_drive(folder);
	task_end();
	halo_changed(folder, 1);
	if (!failed)
		page("Your original Halo is back", "Halo is rebuilt exactly as it came, in its own folder. Halo 1 Unlimited "
			"is as it was.");
	else
		task_wait();
}

/* ---- Send logs (logs.c; docs/log_uploads.md): the player's choice, each time ---- */

static char const *logs_url(void)
{
	return developer_logs[0] ? developer_logs : LOGS_DEFAULT_URL;
}

static void send_logs(struct halo *h)
{
	struct menu_item sure[2] = {{"Send them", NULL, 0}, {"Back", NULL, 0}};
	struct download_report report = {library_step, library_progress, "Sending"};
	char code[9], error[256], text[400];
	unsigned long kilobytes;
	int files;
	if (menu("Send your logs?", "Sends Halo 1 Unlimited's logs to its developers, so they can see what went wrong: what "
		"the game and this installer did, the maps played, your profile names and your Xbox's network addresses. "
		"Nothing else on your Xbox is sent, and they're deleted after 30 days.", sure, 2, 0) != 0)
		return;
	check_wait();                      /* (one connection at a time: the update check's is done) */
	player_heading("Sending your logs");
	screen_text(MENU_TOP, 1, COLOUR_GREY, "Packing the logs and sending them...");
	screen_show();
	if (logs_send(logs_url(), h ? h->path : NULL, LOG_PATH, package_ok ? package.build : 0, INSTALLER_VERSION, code,
		&files, &kilobytes, &report, error))
	{
		text_print(text, sizeof(text), "logs: not sent: %s", error);
		log_file(text);
		text_print(text, sizeof(text), "They couldn't be sent: %s. Is the Xbox connected to the internet?", error);
		page("Your logs weren't sent", text);
		return;
	}
	text_print(text, sizeof(text), "logs: sent, code %s (%d files, %lu KB)", code, files, kilobytes);
	log_file(text);
	/* (installer 74: no file count or size on the screen, Llama: they only raise questions; the log keeps them) */
	text_print(text, sizeof(text), "Your code is %.4s-%.4s\n\nTell the Halo 1 Unlimited developers this code, and they'll "
		"find your logs with it.", code, code + 4);
	page("Your logs were sent", text);
}

static void player_more(struct halo *h)
{
	/* (Send logs first: a player pressing A over and over lands on it, which asks first, not on Rebuild) */
	static struct menu_item const all[4] = {
		{"Send logs", "Something went wrong? Send Halo 1 Unlimited's logs to its developers (you choose each time: "
			"nothing is ever sent otherwise).", 0},
		{"Rebuild original Halo", "Your original Halo, made again exactly as it came from Halo 1 Unlimited's maps, in a "
			"folder of its own (about half an hour). Halo 1 Unlimited isn't changed.", 0},
		{"About", NULL, 0},
		{"Back", NULL, 0}};
	enum { LOGS, REBUILD, ABOUT, BACK };
	struct menu_item items[4];
	int actions[4], k, i, original = 0, count = 0;
	char about[400];
	for (i = 0; i < 4; i++)
	{
		if (i == LOGS && !logs_url()[0])
			continue;              /* (no address for them yet: no item) */
		items[count] = all[i];
		items[count].disabled = i == REBUILD && !h;
		actions[count++] = i;
	}
	while ((k = menu("More", NULL, items, count, 0)) >= 0 && actions[k] != BACK)
	{
		if (actions[k] == LOGS)
			send_logs(h);
		else if (actions[k] == REBUILD && h)
		{
			struct menu_item sure[2] = {{"Rebuild it", NULL, 0}, {"Back", NULL, 0}};
			/* (only now are the drives looked round: is the original Halo still there? The list is made again, so h with
			it) */
			find_halos();
			h = player_install();
			for (i = 0, original = 0; i < halo_count; i++)
				if (halos[i].state == RETAIL && !halos[i].unfinished)
					original = 1;
			if (!h)
				return;
			if (original)
			{
				page("Rebuild original Halo", "Your original Halo is still on your Xbox: there's nothing to rebuild.");
				continue;
			}
			if (menu("Rebuild your original Halo?", "Halo is made again exactly as it came, in a folder of its own. It "
				"takes about half an hour, and about 1.7 GB.", sure, 2, 1) == 0)
			{
				rebuild_original(h);
				return;
			}
		}
		else if (actions[k] == ABOUT)
		{
			text_print(about, sizeof(about), "%s\n\nInstaller version %d.\n%s", package_ok ? package.name :
				"No Halo 1 Unlimited package is beside this installer.", INSTALLER_VERSION, h ? h->path : "");
			page("About", about);
		}
	}
}

/* the main screen */
static void player_main(void)
{
	int chosen = 0, pointed_at_update = 0;
	for (;;)
	{
		struct menu_item items[6];
		int n = 0, k, actions[6];
		struct halo *h;
		if (check_state == CHECK_NEWER && !update_asked)
			update_prompt();               /* (the answer came after the start's wait) */
		h = player_install();
		enum { INSTALL, WHAT, PLAY, UPDATE, PACKS, LIBRARY, MORE, DEVELOPER };
		if (!h)
		{
			text_print(items[n].text, sizeof(items[n].text), "Install Halo 1 Unlimited");
			items[n].about = "Builds Halo 1 Unlimited from the Halo on your hard drive (about 6 minutes).";
			items[n].disabled = !package_ok;
			actions[n++] = INSTALL;
			text_print(items[n].text, sizeof(items[n].text), "What is Halo 1 Unlimited?");
			items[n].about = "What it is, and what it does to your Xbox.";
			items[n].disabled = 0;
			actions[n++] = WHAT;
		}
		else
		{
			text_print(items[n].text, sizeof(items[n].text), "Play Halo 1 Unlimited");
			items[n].about = NULL;
			items[n].disabled = 0;
			actions[n++] = PLAY;
			items[n].disabled = 0;
			if (!h->up_to_date && package_ok)
			{
				text_print(items[n].text, sizeof(items[n].text), "Update Halo 1 Unlimited (v1.%lu)", package.build);
				items[n].about = "A newer version is ready to install (about a minute).";
			}
			else if (check_state == CHECK_RUNNING)
			{
				text_print(items[n].text, sizeof(items[n].text), "Checking for updates...");
				items[n].about = "Looking online for a newer Halo 1 Unlimited.";
				items[n].disabled = 1;
			}
			else if (check_state == CHECK_NEWER)
			{
				text_print(items[n].text, sizeof(items[n].text), "Update Halo 1 Unlimited (%s)", check_version());
				items[n].about = "A newer Halo 1 Unlimited is online: it's downloaded and installed (a minute or two).";
			}
			else if (check_state == CHECK_NEWEST)
			{
				text_print(items[n].text, sizeof(items[n].text), "Already up to date!");
				items[n].about = "This is the newest Halo 1 Unlimited.";
				items[n].disabled = 1;
			}
			else
			{
				text_print(items[n].text, sizeof(items[n].text), "Check for updates");
				items[n].about = "Couldn't look online (is the Xbox connected?). Press A to try again.";
			}
			actions[n++] = UPDATE;
			if (library_ready() || developer_library[0])
			{
				text_print(items[n].text, sizeof(items[n].text), "Download New Maps");
				items[n].about = "Download new maps for Halo 1 Unlimited.";
				items[n].disabled = 0;
				actions[n++] = LIBRARY;
			}
			text_print(items[n].text, sizeof(items[n].text), "Manage Classic Map Packs");
			items[n].about = "Add the map packs on your Xbox to Halo 1 Unlimited's map list.";
			items[n].disabled = 0;
			actions[n++] = PACKS;
			text_print(items[n].text, sizeof(items[n].text), "More...");
			items[n].about = logs_url()[0] ? "Rebuild your original Halo, send your logs, and about this installer." :
				"Rebuild your original Halo, and about this installer.";
			items[n].disabled = 0;
			actions[n++] = MORE;
		}
		if (developer)
		{
			text_print(items[n].text, sizeof(items[n].text), "Developer");
			items[n].about = "Every Halo folder, and everything the installer can do to each (developer.ini).";
			items[n].disabled = 0;
			actions[n++] = DEVELOPER;
		}
		/* (an update ready: the menu starts on it, once) */
		if (h && !pointed_at_update && (check_state == CHECK_NEWER || (!h->up_to_date && package_ok)))
		{
			for (k = 0; k < n; k++)
				if (actions[k] == UPDATE && !items[k].disabled)
					chosen = k;
			pointed_at_update = 1;
		}
		menu_watches_check = 1;            /* (the check's answer shown, and asked about, as it comes) */
		k = menu(h ? "Main menu" : "Welcome", package_ok ? NULL :
			"This installer needs its opence.oxpk beside it to install. Put it there and start the installer again.",
			items, n, chosen);
		menu_watches_check = 0;
		if (k == -2)
			continue;
		if (k < 0)
		{
			log_file("to the dashboard");
			launch(NULL);
		}
		chosen = k;
		/* (v1.142: these need every Halo folder: the drives looked round now, once a run; the list is made again. More
		looks round only when Rebuild original Halo is picked: player_more) */
		if (actions[k] == INSTALL || actions[k] == PACKS)
		{
			find_halos();
			h = player_install();
		}
		switch (actions[k])
		{
		case INSTALL:
			player_install_flow();
			break;
		case WHAT:
			what_is_it();
			break;
		case PLAY:
			launch_halo(h);
			break;
		case UPDATE:
			if (h->up_to_date && check_state == CHECK_FAILED)
				check_start();             /* (try again: the item shows it checking) */
			else
				player_update(h);
			break;
		case PACKS:
			player_map_packs();
			break;
		case LIBRARY:
			player_download(h);
			break;
		case MORE:
			player_more(h);
			break;
		case DEVELOPER:
			developer_screen();
			halo_changed(NULL, 1);           /* (it can do anything to any of them) */
			break;
		}
	}
}

/* developer.ini beside the installer: "developer = 1" shows the Developer screen; "update_url = <address>" and
"allow_unsigned = 1" are a test update location's (as update_url.txt's) */
static void developer_read(void)
{
	FILE *file = fopen(own_path("developer.ini"), "r");
	char line[600];
	developer = 0;
	if (!file)
		return;
	while (fgets(line, sizeof(line), file))
	{
		char *key = line, *value, *end;
		while (*key == ' ' || *key == '\t')
			key++;
		if (!(value = strchr(key, '=')))
			continue;
		for (end = value; end > key && (end[-1] == ' ' || end[-1] == '\t'); end--)
			;
		*end = 0;
		for (value++; *value == ' ' || *value == '\t'; value++)
			;
		for (end = value + strlen(value); end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '); end--)
			;
		*end = 0;
		if (!_stricmp(key, "developer"))
			developer = atoi(value) != 0;
		else if (!_stricmp(key, "update_url"))
			text_print(developer_url, sizeof(developer_url), "%s", value);
		else if (!_stricmp(key, "allow_unsigned"))
			developer_unsigned = atoi(value) != 0;
		else if (!_stricmp(key, "library_url"))
			text_print(developer_library, sizeof(developer_library), "%s", value);
		else if (!_stricmp(key, "logs_url"))
			text_print(developer_logs, sizeof(developer_logs), "%s", value);
	}
	fclose(file);
	log_file(developer ? "developer.ini: developer = 1" : "developer.ini: no developer = 1");
}

int main(void)
{
	own_folder();
	nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
	nxMountDrive('F', "\\Device\\Harddisk0\\Partition6\\");
	nxMountDrive('G', "\\Device\\Harddisk0\\Partition7\\");
	screen_init();
	screen_spinner(1);                 /* (something moves at once: the reading and the rest take a while) */
	pads_start();
	heading("Reading the package...");
	{
		char line[96];
		text_print(line, sizeof(line), "---- %s, version %d", TITLE, INSTALLER_VERSION);
		log_file(line);
	}
	consolidate_log = log_file; /* (the map packs' "time:" lines) */
	developer_read();
	cache_drives();
	notices_write();
	package_read();
	if (exists(AUTO_PATH))
		auto_update();
	{
		char url[512];
		update_url(url, sizeof(url)); /* (update_url.txt's allow_unsigned, for the installer the package carries) */
	}
	offer_installer_update();
	last_install_read();
	if (last_install[0])
	{
		char line[200];
		text_print(line, sizeof(line), "last install: %s", last_install);
		log_file(line);
	}
	scan_cache_load(SCAN_CACHE_PATH);
	if (exists(CONTINUE_PATH))
	{
		/* (started by the update it came with: Halo 1 Unlimited updated from the new package) */
		DeleteFileA(CONTINUE_PATH);
		flush_own_drive();
		log_file("going on with the update");
		update_asked = 1;
		update_installed();
	}
	else
		startup_update_check();
	find_install();                      /* (v1.142: the drives looked round only when a screen needs them) */
	player_main();
	return 0;
}
