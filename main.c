#include <forge/logger.h>
#include <forge/err.h>
#include <forge/io.h>

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
        char *data;
        size_t len;
        size_t cap;
} line;

typedef struct {
        line **lines;
        size_t len;
        size_t cap;
} text;

typedef struct {
        text text;
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
        line *l = (line *)malloc(sizeof(line));
        l->data = NULL;
        l->len = 0;
        l->cap = 0;
        return l;
}

text
text_init(void)
{
        line **lines = (line **)malloc(sizeof(line *) * g_scrn_height);
        for (size_t i = 0; i < g_scrn_height; ++i) {
                lines[i] = line_alloc();
        }

        return (text) {
                .lines = lines,
                .len = g_scrn_height,
                .cap = g_scrn_height,
        };
}

sten_context
init_context(void)
{
        sten_context ctx = {0};

        ctx.text = text_init();

        ctx.win.cur = newwin(g_scrn_height - 1, g_scrn_width, 0, 0);
        ctx.win.w = g_scrn_width;
        ctx.win.h = g_scrn_height;
        ctx.win.x_offset = 0;
        ctx.win.y_offset = 0;

        ctx.cursor.r = 0;
        ctx.cursor.c = 0;

        return ctx;
}

void
insert_at_cursor(sten_context *ctx, char ch)
{
        line *line = ctx->text.lines[ctx->cursor.r];
        assert(line != NULL);

        if (line->len >= line->cap) {
                line->cap = !line->cap ? 256 : line->cap * 2;
                line->data = (char *)realloc(line->data, line->cap);
                assert(line->data != NULL);
        }

        // Shift existing characters to the right
        if (line->len > ctx->cursor.c) {
                memmove(line->data + ctx->cursor.c + 1,
                        line->data + ctx->cursor.c,
                        line->len - ctx->cursor.c);
        }

        // Insert the new character
        line->data[ctx->cursor.c] = ch;
        line->len++;

        // Null-terminate the string
        if (line->len < line->cap) {
                line->data[line->len] = '\0';
        }

        // Update cursor position
        ctx->cursor.c++;
}

void
render(const sten_context *ctx)
{
        werase(ctx->win.cur);

        for (size_t i = 0; i < ctx->text.len; ++i) {
                if (i >= ctx->win.h) break; // Don't render beyond window height
                if (ctx->text.lines[i]->data != NULL) {
                        mvwprintw(ctx->win.cur, i, 0, "%s", ctx->text.lines[i]->data);
                }
        }

        // Move cursor to its position
        wmove(ctx->win.cur, ctx->cursor.r - ctx->win.y_offset, ctx->cursor.c - ctx->win.x_offset);
        wrefresh(ctx->win.cur);
}

void
left(sten_context *ctx)
{
        if (ctx->cursor.c > 0) {
                ctx->cursor.c--;
        }
}

void
right(sten_context *ctx)
{
        line *current_line = ctx->text.lines[ctx->cursor.r];
        if (ctx->cursor.c < current_line->len) {
                ctx->cursor.c++;
        }
}

void
up(sten_context *ctx)
{
        forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "down(): ctx->cursor.r = %zu", ctx->cursor.r);
        if (ctx->cursor.r > 0) {
                ctx->cursor.r--; // Move to previous line
                line *target_line = ctx->text.lines[ctx->cursor.r];
                // Adjust column to not exceed target line's length
                if (ctx->cursor.c > target_line->len) {
                        ctx->cursor.c = target_line->len;
                }
        }
}

void
down(sten_context *ctx)
{
        forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "down(): len = %zu", ctx->text.len);
        if (ctx->cursor.r < ctx->text.len - 1) {
                ctx->cursor.r++; // Move to next line
                line *target_line = ctx->text.lines[ctx->cursor.r];
                // Adjust column to not exceed target line's length
                if (ctx->cursor.c > target_line->len) {
                        ctx->cursor.c = target_line->len;
                }
        }
}

void
enter(sten_context *ctx)
{
        line *current_line = ctx->text.lines[ctx->cursor.r];
        assert(current_line != NULL);

        // If text buffer is full, reallocate
        if (ctx->text.len >= ctx->text.cap) {
                ctx->text.cap *= 2;
                ctx->text.lines = (line **)realloc(ctx->text.lines, ctx->text.cap * sizeof(line *));
                assert(ctx->text.lines != NULL);
                // Initialize new lines
                for (size_t i = ctx->text.len; i < ctx->text.cap; ++i) {
                        ctx->text.lines[i] = line_alloc();
                }
        }

        // Create a new line
        line *new_line = line_alloc();

        // Case 1: Cursor at the beginning
        if (ctx->cursor.c == 0) {
                // Shift all lines down from cursor.r
                memmove(&ctx->text.lines[ctx->cursor.r + 1],
                        &ctx->text.lines[ctx->cursor.r],
                        (ctx->text.len - ctx->cursor.r) * sizeof(line *));
                ctx->text.lines[ctx->cursor.r] = new_line;
                ctx->text.len++;
                ctx->cursor.r++;
                ctx->cursor.c = 0;
        }
        // Case 2: Cursor at the end
        else if (ctx->cursor.c == current_line->len) {
                // Insert new line after current line
                memmove(&ctx->text.lines[ctx->cursor.r + 2],
                        &ctx->text.lines[ctx->cursor.r + 1],
                        (ctx->text.len - ctx->cursor.r - 1) * sizeof(line *));
                ctx->text.lines[ctx->cursor.r + 1] = new_line;
                ctx->text.len++;
                ctx->cursor.r++;
                ctx->cursor.c = 0;
        }
        // Case 3: Cursor in the middle
        else {
                // Split current line at cursor
                size_t split_pos = ctx->cursor.c;
                size_t remaining_len = current_line->len - split_pos;

                // New line's data
                new_line->data = (char *)malloc(current_line->cap);
                assert(new_line->data != NULL);
                new_line->cap = current_line->cap;
                new_line->len = remaining_len;

                // Copy the second part of the current line to the new line
                memcpy(new_line->data, current_line->data + split_pos, remaining_len);
                new_line->data[remaining_len] = '\0';

                // Truncate current line at cursor
                current_line->len = split_pos;
                current_line->data[split_pos] = '\0';

                // Insert new line after current line
                memmove(&ctx->text.lines[ctx->cursor.r + 2],
                        &ctx->text.lines[ctx->cursor.r + 1],
                        (ctx->text.len - ctx->cursor.r - 1) * sizeof(line *));
                ctx->text.lines[ctx->cursor.r + 1] = new_line;
                ctx->text.len++;
                ctx->cursor.r++;
                ctx->cursor.c = 0;
        }
}

void
backspace(sten_context *ctx)
{
        line *current_line = ctx->text.lines[ctx->cursor.r];
        assert(current_line != NULL);

        // Case 1: Cursor is not at the start of the line
        if (ctx->cursor.c > 0) {
                // Shift characters left to overwrite the character before the cursor
                memmove(current_line->data + ctx->cursor.c - 1,
                        current_line->data + ctx->cursor.c,
                        current_line->len - ctx->cursor.c + 1); // +1 to include null terminator
                current_line->len--;
                ctx->cursor.c--;

                // Shrink allocation if necessary
                if (current_line->len < current_line->cap / 2 && current_line->cap > 256) {
                        current_line->cap /= 2;
                        current_line->data = (char *)realloc(current_line->data, current_line->cap);
                        assert(current_line->data != NULL);
                }
        }
        // Case 2: Cursor is at the start of the line, and it's not the first line
        else if (ctx->cursor.r > 0) {
                line *prev_line = ctx->text.lines[ctx->cursor.r - 1];
                assert(prev_line != NULL);

                // Move cursor to the end of the previous line
                ctx->cursor.c = prev_line->len;
                ctx->cursor.r--;

                // If current line is not empty, append its contents to the previous line
                if (current_line->len > 0) {
                        // Ensure previous line has enough capacity
                        if (prev_line->len + current_line->len >= prev_line->cap) {
                                prev_line->cap = (prev_line->len + current_line->len + 256) & ~255; // Round up to next 256
                                prev_line->data = (char *)realloc(prev_line->data, prev_line->cap);
                                assert(prev_line->data != NULL);
                        }

                        // Append current line's data to previous line
                        memcpy(prev_line->data + prev_line->len, current_line->data, current_line->len);
                        prev_line->len += current_line->len;
                        prev_line->data[prev_line->len] = '\0';
                }

                // Free the current line
                free(current_line->data);
                free(current_line);

                // Shift remaining lines up
                memmove(&ctx->text.lines[ctx->cursor.r + 1],
                        &ctx->text.lines[ctx->cursor.r + 2],
                        (ctx->text.len - ctx->cursor.r - 2) * sizeof(line *));
                ctx->text.len--;

                // Shrink text buffer if necessary
                if (ctx->text.len < ctx->text.cap / 2 && ctx->text.cap > g_scrn_height) {
                        ctx->text.cap /= 2;
                        ctx->text.lines = (line **)realloc(ctx->text.lines, ctx->text.cap * sizeof(line *));
                        assert(ctx->text.lines != NULL);
                }
        }
        // Case 3: Cursor is at the start of the first line, do nothing
}

void
save_file(const sten_context *ctx)
{
        const char *fp = "1.txt";
        char **lines = (char **)malloc(sizeof(char *) * ctx->text.len);

        for (size_t i = 0; i < ctx->text.len; ++i) {
                lines[i] = ctx->text.lines[i]->data;
        }

        if (!forge_io_write_lines(fp, (const char **)lines, ctx->text.len)) {
                forge_logger_log(&logger, FORGE_LOG_LEVEL_ERR, "could not save file");
        }

        free(lines);
}

void
input_loop(void)
{
        forge_logger_log(&logger, FORGE_LOG_LEVEL_DEBUG, "starting input loop");

        sten_context ctx = init_context();

        int ch;
        while ((ch = getch()) != CTRL('q')) {
                switch (ch) {
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
                case CTRL('s'): {
                        save_file(&ctx);
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
