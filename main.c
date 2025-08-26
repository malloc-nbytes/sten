#include <forge/logger.h>
#include <forge/err.h>
#include <forge/io.h>
#include <forge/utils.h>
#include <forge/arg.h>
#include <forge/colors.h>

#include <ncurses.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

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
        FILE *fp;
} sten_context;

static size_t g_scrn_width = 0;
static size_t g_scrn_height = 0;

static forge_logger logger;

char **
read_file_to_lines(FILE *f)
{
        char *line = NULL;
        size_t len = 0;
        ssize_t read;

        struct {
                char **data;
                size_t len;
                size_t cap;
        } buf = {
                .data = NULL,
                .cap = 0,
                .len = 0,
        };

        while ((read = getline(&line, &len, f)) != -1) {
                if (line[read - 1] == '\n') {
                        line[read - 1] = '\0';
                }
                if (buf.len >= buf.cap) {
                        buf.cap = buf.cap == 0 ? 2 : buf.cap*2;
                        buf.data = realloc(buf.data, buf.cap * sizeof(char *));
                }
                buf.data[buf.len++] = strdup(line);
        }

        if (buf.len >= buf.cap) {
                buf.cap = buf.cap == 0 ? 2 : buf.cap*2;
                buf.data = realloc(buf.data, buf.cap * sizeof(char *));
        }
        buf.data[buf.len++] = NULL;

        free(line);

        return buf.data;
}

int
write_lines(FILE        *f,
            const line **lines,
            size_t       lines_n)
{
        if (fseek(f, 0, SEEK_SET) != 0) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to seek to start of file: %s", strerror(errno));
                return 0;
        }

        if (ftruncate(fileno(f), 0) != 0) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to truncate file: %s", strerror(errno));
                return 0;
        }

        for (size_t i = 0; i < lines_n; ++i) {
                if (lines[i] && lines[i]->chars && lines[i]->len > 0) {
                        if (fwrite(lines[i]->chars, 1, lines[i]->len, f) != lines[i]->len) {
                                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to write line %zu: %s", i, strerror(errno));
                                return 0;
                        }
                        if (fputc('\n', f) == EOF) {
                                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to write newline for line %zu: %s", i, strerror(errno));
                                return 0;
                        }
                } else {
                        // Write a newline for empty lines
                        if (fputc('\n', f) == EOF) {
                                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to write newline for empty line %zu: %s", i, strerror(errno));
                                return 0;
                        }
                }
        }

        if (fflush(f) != 0) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to flush file: %s", strerror(errno));
                return 0;
        }

        return 1;
}

void
init_ncurses(void)
{
        initscr();
        raw();
        keypad(stdscr, TRUE);
        noecho();
        timeout(100);
        getmaxyx(stdscr, g_scrn_height, g_scrn_width);
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_RED);
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
                .fp = NULL,
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
                // TODO: refactor this code out
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
                        continue;
                }
                line *ln = ctx->lines.data[buf_r];
                size_t sx = 0;

                /* Find start of trailing whitespace */
                size_t trail_start = ln->len;
                for (size_t i = ln->len; i > 0; i--) {
                        if (ln->chars[i - 1] != ' ' && ln->chars[i - 1] != '\t') {
                                trail_start = i;
                                break;
                        }
                }

                /* Render characters */
                for (size_t bx = ctx->win.x_offset; bx < ln->len && sx < ctx->win.w; bx++, sx++) {
                        if (bx >= trail_start && (ln->chars[bx] == ' ' || ln->chars[bx] == '\t')) {
                                attron(COLOR_PAIR(1));
                                mvaddch(sy, sx, ln->chars[bx]);
                                attroff(COLOR_PAIR(1));
                        } else {
                                mvaddch(sy, sx, ln->chars[bx]);
                        }
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
                //size_t old_len = ln->len;

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
save(const sten_context *ctx)
{
        if (!ctx->fp) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "No file opened for saving");
                return;
        }

        if (!write_lines(ctx->fp, (const line **)ctx->lines.data, ctx->lines.len)) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "Failed to save file");
        } else {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_INFO, "File saved successfully");
        }
}

void
load_txt_from_file(sten_context *ctx,
                   FILE         *fp)
{
        // NOTE: The filepath should *never* be NULL here!
        //       Make sure to validate `fp` before calling
        //       this function!
        assert(fp);

        char **lns = read_file_to_lines(fp);
        for (size_t i = 0; lns && lns[i]; ++i) {
                line *ln = line_alloc();
                size_t ln_n = strlen(lns[i]);

                ln->chars = strdup(lns[i]);
                ln->len = ln_n;
                ln->cap = ln_n+1;

                // TODO: refactor this code out
                if (ctx->lines.len == ctx->lines.cap) {
                        ctx->lines.cap = ctx->lines.cap ? ctx->lines.cap * 2 : 8;
                        ctx->lines.data = realloc(ctx->lines.data, sizeof(line *) * ctx->lines.cap);
                }
                ctx->lines.data[ctx->lines.len++] = ln;

                free(lns[i]);
        }

        if (lns) {
                free(lns);
        }
}

void
input_loop(int argc, char **argv)
{
        forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "starting input loop");

        sten_context ctx = init_context();

        forge_arg *arg = forge_arg_alloc(argc, argv, 1);
        forge_arg *it = arg;
        while (it) {
                if (it->h == 0) {
                        ctx.fp = fopen(arg->s, "r+");
                        if (!ctx.fp) {
                                forge_err_wargs("could not open file `%s`: %s", arg->s, strerror(errno));
                        }
                } else if (it->h == 1) {
                        forge_err_wargs("unknown option %s", it->s);
                } else {
                        forge_err_wargs("unknown option %s", it->s);
                }
                it = it->n;
        }
        forge_arg_free(arg);

        if (ctx.fp) {
                load_txt_from_file(&ctx, ctx.fp);
        }

        int ch;
        while ((ch = getch()) != CTRL('q')) {
                switch (ch) {
                case CTRL('s'): {
                        save(&ctx);
                } break;
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
                        if (ch == '\t') {
                                for (size_t i = 0; i < 4; ++i)
                                        insert_at_cursor(&ctx, ' ');
                        }
                        if (isprint(ch)) {
                                insert_at_cursor(&ctx, ch);
                        }
                } break;
                }
                render(&ctx);
        }

        if (ctx.fp) {
                fclose(ctx.fp);
        }
}

int
main(int argc, char **argv)
{
        (void)forge_io_truncate_file("log");
        if (!forge_logger_init(&logger, "log", FORGE_LOG_LEVEL_DEBUG)) {
                forge_err("failed to init logger");
        }

        init_ncurses();
        input_loop(argc, argv);
        endwin();
        return 0;
}
