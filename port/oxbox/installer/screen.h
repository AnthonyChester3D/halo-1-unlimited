/* screen.h: the installer's text screen: 72 x 27 characters (8 x 16, nxdk's unscii font) in colour, drawn straight
into the framebuffer, only the characters that changed */
#ifndef OXBOX_SCREEN_H
#define OXBOX_SCREEN_H

#define SCREEN_COLUMNS 72
#define SCREEN_ROWS 27

#define COLOUR_BLACK 0x000000
#define COLOUR_WHITE 0xE8E8E8
#define COLOUR_GREY 0x8C8C8C
#define COLOUR_DIM 0x5A5A5A
#define COLOUR_GREEN 0x50E060
#define COLOUR_YELLOW 0xF0D050
#define COLOUR_RED 0xFF6060
#define COLOUR_BAR 0x1C3A6E

void screen_init(void);
void screen_clear(void);
/* text at (row, column), cut at the screen's edge; returns the column after it */
int screen_text(int row, int column, unsigned long colour, char const *format, ...);
/* a row's background (the selection bar), and its text cleared */
void screen_row(int row, unsigned long background);
/* draws what changed */
void screen_show(void);
/* a black picture that stays up while the console starts the next XBE, instead of the next XBE's bytes showing
through the old one */
void screen_hand_over(void);

#endif
