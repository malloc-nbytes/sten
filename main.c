#include <forge/logger.h>
#include <forge/err.h>
#include <forge/io.h>
#include <forge/utils.h>

#include <ncurses.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CTRL(x) ((x) & 0x1F)
#define BACKSPACE 263
#define ESCAPE 27
#define ENTER 10
#define SPACE 32

typedef struct line {
        char *chars;
        size_t len;
        size_t cap;
} line;

typedef struct {
        struct {
                line **data;
                size_t len;
                size_t cap;
        } lines;
        struct {
                size_t r;
                size_t c;
        } cursor;
        struct {
                WINDOW *cur;
                size_t w;
                size_t h;
                size_t x_offset;
                size_t y_offset;
        } win;
} sten_context;

static size_t g_scrn_width = 0;
static size_t g_scrn_height = 0;

static forge_logger logger;

void
init_ncurses(void)
{
        initscr();
        raw();
        keypad(stdscr, TRUE);
        noecho();
        timeout(100);
        getmaxyx(stdscr, g_scrn_height, g_scrn_width);
}

line *
line_alloc(void)
{
        line *ln = (line *)malloc(sizeof(line));
        ln->chars = NULL;
        ln->len = 0;
        ln->cap = 0;
        return ln;
}

sten_context
init_context(void)
{
        return (sten_context) {
                .lines = {
                        .data = NULL,
                        .len = 0,
                        .cap = 0,
                },
                .cursor = {
                        .r = 0,
                        .c = 0,
                },
                .win = {
                        .cur = stdscr,
                        .w = g_scrn_width,
                        .h = g_scrn_height,
                        .x_offset = 0,
                        .y_offset = 0,
                },
        };
}

static void
adjust_view(sten_context *ctx)
{
        /* Vertical scrolling */
        if (ctx->cursor.r < ctx->win.y_offset) {
                ctx->win.y_offset = ctx->cursor.r;
        }
        if (ctx->cursor.r >= ctx->win.y_offset + ctx->win.h) {
                ctx->win.y_offset = ctx->cursor.r - ctx->win.h + 1;
        }

        /* Horizontal scrolling */
        size_t line_len = 0;
        if (ctx->cursor.r < ctx->lines.len) {
                line_len = ctx->lines.data[ctx->cursor.r]->len;
        }
        if (ctx->cursor.c > line_len) {
                ctx->cursor.c = line_len; /* Clamp (should not be needed) */
        }
        if (ctx->cursor.c < ctx->win.x_offset) {
                ctx->win.x_offset = ctx->cursor.c;
        }
        if (ctx->cursor.c >= ctx->win.x_offset + ctx->win.w) {
                ctx->win.x_offset = ctx->cursor.c - ctx->win.w + 1;
        }
}

void
insert_at_cursor(sten_context *ctx, char ch)
{
        /* Ensure there are enough lines up to the cursor row */
        while (ctx->cursor.r >= ctx->lines.len) {
                if (ctx->lines.len == ctx->lines.cap) {
                        ctx->lines.cap = ctx->lines.cap ? ctx->lines.cap * 2 : 8;
                        ctx->lines.data = realloc(ctx->lines.data, sizeof(line *) * ctx->lines.cap);
                }
                line *ln = line_alloc();
                ctx->lines.data[ctx->lines.len++] = ln;
        }

        line *ln = ctx->lines.data[ctx->cursor.r];

        /* Ensure capacity in the line */
        if (ln->len == ln->cap) {
                ln->cap = ln->cap ? ln->cap * 2 : 8;
                ln->chars = realloc(ln->chars, ln->cap);
        }

        /* Shift characters right and insert */
        memmove(ln->chars + ctx->cursor.c + 1, ln->chars + ctx->cursor.c, ln->len - ctx->cursor.c);
        ln->chars[ctx->cursor.c] = ch;
        ln->len++;
        ctx->cursor.c++;

        adjust_view(ctx);
}

void
render(const sten_context *ctx)
{
        erase(); /* Clear the screen */

        /* Render visible lines */
        for (size_t sy = 0; sy < ctx->win.h; sy++) {
                size_t buf_r = ctx->win.y_offset + sy;
                if (buf_r >= ctx->lines.len) {
                        /* Non-existent lines */
                        //mvaddch(sy, 0, '~');
                        continue;
                }
                line *ln = ctx->lines.data[buf_r];
                size_t sx = 0;
                for (size_t bx = ctx->win.x_offset; bx < ln->len && sx < ctx->win.w; bx++, sx++) {
                        mvaddch(sy, sx, ln->chars[bx]);
                }
        }

        /* Set cursor position on screen */
        size_t screen_y = ctx->cursor.r - ctx->win.y_offset;
        size_t screen_x = ctx->cursor.c - ctx->win.x_offset;
        move(screen_y, screen_x);
        refresh();
}

void
left(sten_context *ctx)
{
        if (ctx->cursor.c > 0) {
                ctx->cursor.c--;
        }
        adjust_view(ctx); /* Added for consistency with other movements */
}

void
right(sten_context *ctx)
{
        if (ctx->cursor.r >= ctx->lines.len) return;
        line *ln = ctx->lines.data[ctx->cursor.r];
        if (ctx->cursor.c < ln->len) {
                ctx->cursor.c++;
        }
        adjust_view(ctx);
}

void
up(sten_context *ctx)
{
        if (ctx->cursor.r > 0) {
                ctx->cursor.r--;
                line *ln = ctx->lines.data[ctx->cursor.r];
                if (ctx->cursor.c > ln->len) {
                        ctx->cursor.c = ln->len;
                }
        }
        adjust_view(ctx);
}

void
down(sten_context *ctx)
{
        if (ctx->cursor.r + 1 < ctx->lines.len) {
                ctx->cursor.r++;
                line *ln = ctx->lines.data[ctx->cursor.r];
                if (ctx->cursor.c > ln->len) {
                        ctx->cursor.c = ln->len;
                }
        }
        adjust_view(ctx);
}

void
enter(sten_context *ctx)
{
        line *cur = NULL;
        if (ctx->cursor.r < ctx->lines.len) {
                cur = ctx->lines.data[ctx->cursor.r];
        } else {
                /* Add new line at end if cursor is beyond */
                if (ctx->lines.len == ctx->lines.cap) {
                        ctx->lines.cap = ctx->lines.cap ? ctx->lines.cap * 2 : 8;
                        ctx->lines.data = realloc(ctx->lines.data, sizeof(line *) * ctx->lines.cap);
                }
                line *ln = line_alloc();
                ctx->lines.data[ctx->lines.len++] = ln;
                ctx->cursor.c = 0;
                adjust_view(ctx);
                return;
        }

        /* Create new line for split */
        line *newln = line_alloc();
        size_t split_len = cur->len - ctx->cursor.c;
        if (split_len > 0) {
                newln->cap = split_len;
                newln->chars = malloc(newln->cap);
                memcpy(newln->chars, cur->chars + ctx->cursor.c, split_len);
                newln->len = split_len;
                cur->len = ctx->cursor.c;
        }

        /* Insert new line after current */
        if (ctx->lines.len == ctx->lines.cap) {
                ctx->lines.cap = ctx->lines.cap ? ctx->lines.cap * 2 : 8;
                ctx->lines.data = realloc(ctx->lines.data, sizeof(line *) * ctx->lines.cap);
        }
        memmove(ctx->lines.data + ctx->cursor.r + 2, ctx->lines.data + ctx->cursor.r + 1, (ctx->lines.len - ctx->cursor.r - 1) * sizeof(line *));
        ctx->lines.data[ctx->cursor.r + 1] = newln;
        ctx->lines.len++;

        /* Move cursor to new line */
        ctx->cursor.r++;
        ctx->cursor.c = 0;

        adjust_view(ctx);
}

void
backspace(sten_context *ctx)
{
        if (ctx->lines.len == 0 || ctx->cursor.r >= ctx->lines.len) return;

        line *ln = ctx->lines.data[ctx->cursor.r];
        if (ctx->cursor.c > 0) {
                /* Delete previous char in line */
                memmove(ln->chars + ctx->cursor.c - 1, ln->chars + ctx->cursor.c, ln->len - ctx->cursor.c);
                ln->len--;
                ctx->cursor.c--;
        } else if (ctx->cursor.r > 0) {
                /* Merge with previous line */
                line *prev = ctx->lines.data[ctx->cursor.r - 1];
                size_t old_prev_len = prev->len;

                /* Ensure capacity in prev */
                size_t new_cap = prev->cap;
                while (prev->len + ln->len > new_cap) {
                        new_cap = new_cap ? new_cap * 2 : 8;
                }
                if (new_cap != prev->cap) {
                        prev->cap = new_cap;
                        prev->chars = realloc(prev->chars, prev->cap);
                }

                /* Append current line to prev */
                if (ln->len > 0) {
                        memcpy(prev->chars + prev->len, ln->chars, ln->len);
                        prev->len += ln->len;
                }

                /* Free current line */
                free(ln->chars);
                free(ln);

                /* Shift lines up */
                memmove(ctx->lines.data + ctx->cursor.r, ctx->lines.data + ctx->cursor.r + 1, (ctx->lines.len - ctx->cursor.r - 1) * sizeof(line *));
                ctx->lines.len--;

                /* Move cursor to end of prev line */
                ctx->cursor.r--;
                ctx->cursor.c = old_prev_len;
        }

        adjust_view(ctx);
}

void
eol(sten_context *ctx)
{
        if (ctx->cursor.r < ctx->lines.len) {
                line *ln = ctx->lines.data[ctx->cursor.r];
                ctx->cursor.c = ln->len;
        } else {
                ctx->cursor.c = 0;
        }
        adjust_view(ctx);
}

void
bol(sten_context *ctx)
{
        ctx->cursor.c = 0;
        adjust_view(ctx);
}

void
del_char_under_cursor(sten_context *ctx)
{
        if (ctx->lines.len == 0 || ctx->cursor.r >= ctx->lines.len) return;

        line *ln = ctx->lines.data[ctx->cursor.r];
        if (ctx->cursor.c < ln->len) {
                /* Delete character under cursor */
                memmove(ln->chars + ctx->cursor.c, ln->chars + ctx->cursor.c + 1, ln->len - ctx->cursor.c - 1);
                ln->len--;
        } else if (ctx->cursor.r + 1 < ctx->lines.len) {
                /* Merge with next line if at end of current line */
                line *next = ctx->lines.data[ctx->cursor.r + 1];
                size_t old_len = ln->len;

                /* Ensure capacity in current line */
                size_t new_cap = ln->cap;
                while (ln->len + next->len > new_cap) {
                        new_cap = new_cap ? new_cap * 2 : 8;
                }
                if (new_cap != ln->cap) {
                        ln->cap = new_cap;
                        ln->chars = realloc(ln->chars, ln->cap);
                }

                /* Append next line to current */
                if (next->len > 0) {
                        memcpy(ln->chars + ln->len, next->chars, next->len);
                        ln->len += next->len;
                }

                /* Free next line */
                free(next->chars);
                free(next);

                /* Shift lines up */
                memmove(ctx->lines.data + ctx->cursor.r + 1, ctx->lines.data + ctx->cursor.r + 2, (ctx->lines.len - ctx->cursor.r - 2) * sizeof(line *));
                ctx->lines.len--;
        }

        adjust_view(ctx);
}

void
input_loop(void)
{
        forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "starting input loop");

        sten_context ctx = init_context();

        int ch;
        while ((ch = getch()) != CTRL('q')) {
                switch (ch) {
                case CTRL('d'): {
                        del_char_under_cursor(&ctx);
                } break;
                case CTRL('e'): {
                        eol(&ctx);
                } break;
                case CTRL('a'): {
                        bol(&ctx);
                } break;
                case CTRL('b'):
                case KEY_LEFT: {
                        left(&ctx);
                } break;
                case CTRL('f'):
                case KEY_RIGHT: {
                        right(&ctx);
                } break;
                case CTRL('n'):
                case KEY_DOWN: {
                        down(&ctx);
                } break;
                case CTRL('p'):
                case KEY_UP: {
                        up(&ctx);
                } break;
                case ENTER: {
                        enter(&ctx);
                } break;
                case CTRL('h'):
                case BACKSPACE: {
                        backspace(&ctx);
                } break;
                default: {
                        if (isprint(ch)) {
                                insert_at_cursor(&ctx, ch);
                        }
                } break;
                }
                render(&ctx);
        }
}

int
main(void)
{
        (void)forge_io_truncate_file("log");
        if (!forge_logger_init(&logger, "log", FORGE_LOG_LEVEL_DEBUG)) {
                forge_err("failed to init logger");
        }

        init_ncurses();
        input_loop();
        endwin();
        return 0;
}
