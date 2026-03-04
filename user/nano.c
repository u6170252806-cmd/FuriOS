#include "user.h"

#define CTRL_KEY(k) ((k) & 0x1f)

enum editor_key {
    KEY_ARROW_LEFT = 1000,
    KEY_ARROW_RIGHT,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_DEL,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_BRACKETED_PASTE,
    KEY_BRACKETED_PASTE_END,
};

#define NANO_MAX_LINE_CHARS (256 * 1024)
#define NANO_MAX_ROWS       (256 * 1024)

typedef struct {
    char *chars;
    int len;
    int cap;
} editor_row_t;

typedef struct {
    int cx;
    int cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    editor_row_t *rows;
    int numrows;
    int dirty;
    int quit_guard;
    int paste_mode;
    char filename[256];
    char status[128];
    int status_ttl;
} editor_state_t;

static editor_state_t E;

static void term_write(const char *s) {
    (void)sys_write(1, s, strlen(s));
}

static void term_write_n(const char *s, int n) {
    if (n > 0) {
        (void)sys_write(1, s, (unsigned long)n);
    }
}

static int is_printable(char c) {
    return c >= 32 && c <= 126;
}

static int read_byte(char *out) {
    if (!out) {
        return -1;
    }
    while (sys_read(0, out, 1) != 1) {
    }
    return 0;
}

static int read_byte_timeout(char *out, unsigned long timeout_ticks) {
    fu_pollfd_t pfd;
    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int ready = sys_poll(&pfd, 1, timeout_ticks);
    if (ready <= 0) {
        return -1;
    }
    if ((pfd.revents & (POLLIN | POLLHUP)) == 0) {
        return -1;
    }
    long n = sys_read(0, out, 1);
    if (n != 1) {
        return -1;
    }
    return 0;
}

static int write_all(int fd, const char *p, int want) {
    int done = 0;
    while (done < want) {
        long n = write(fd, p + done, (unsigned long)(want - done));
        if (n <= 0) {
            return -1;
        }
        done += (int)n;
    }
    return 0;
}

static const char *find_substr(const char *hay, const char *needle) {
    if (!*needle) {
        return hay;
    }
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && hay[i + j] && hay[i + j] == needle[j]) {
            j++;
        }
        if (!needle[j]) {
            return hay + i;
        }
    }
    return 0;
}

static void editor_set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(E.status, sizeof(E.status), fmt, ap);
    va_end(ap);
    E.status_ttl = 160;
}

static int editor_read_key(void) {
    char c = 0;
    if (read_byte(&c) != 0) {
        return '\x1b';
    }

    if (c != '\x1b') {
        return (int)c;
    }

    char first = 0;
    if (read_byte_timeout(&first, 1) != 0) {
        return '\x1b';
    }

    if (first == '[') {
        char ch = 0;
        if (read_byte_timeout(&ch, 1) != 0) {
            return '\x1b';
        }
        if (ch >= '0' && ch <= '9') {
            int param = 0;
            int digits = 0;
            while (ch >= '0' && ch <= '9') {
                if (digits < 6) {
                    param = (param * 10) + (ch - '0');
                }
                digits++;
                if (read_byte_timeout(&ch, 1) != 0) {
                    return '\x1b';
                }
            }
            if (ch == '~') {
                switch (param) {
                    case 1: return KEY_HOME;
                    case 3: return KEY_DEL;
                    case 4: return KEY_END;
                    case 5: return KEY_PAGE_UP;
                    case 6: return KEY_PAGE_DOWN;
                    case 7: return KEY_HOME;
                    case 8: return KEY_END;
                    case 200: return KEY_BRACKETED_PASTE;
                    case 201: return KEY_BRACKETED_PASTE_END;
                }
            } else {
                while ((ch >= '0' && ch <= '9') || ch == ';') {
                    if (read_byte_timeout(&ch, 1) != 0) {
                        return '\x1b';
                    }
                }
                switch (ch) {
                    case 'A': return KEY_ARROW_UP;
                    case 'B': return KEY_ARROW_DOWN;
                    case 'C': return KEY_ARROW_RIGHT;
                    case 'D': return KEY_ARROW_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                }
            }
        } else if (ch == '?') {
            /* Consume private CSI sequence (e.g. ?2004h) fully. */
            do {
                if (read_byte_timeout(&ch, 1) != 0) {
                    return '\x1b';
                }
            } while ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?');
            return '\x1b';
        } else {
            switch (ch) {
                case 'A': return KEY_ARROW_UP;
                case 'B': return KEY_ARROW_DOWN;
                case 'C': return KEY_ARROW_RIGHT;
                case 'D': return KEY_ARROW_LEFT;
                case 'H': return KEY_HOME;
                case 'F': return KEY_END;
            }
        }
    } else if (first == 'O') {
        char ch = 0;
        if (read_byte_timeout(&ch, 1) != 0) {
            return '\x1b';
        }
        switch (ch) {
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
        }
    }

    return '\x1b';
}

static int row_reserve(editor_row_t *row, int need) {
    if (!row || need <= 0) {
        return -1;
    }
    if (need > NANO_MAX_LINE_CHARS) {
        return -1;
    }
    if (need <= row->cap) {
        return 0;
    }
    int new_cap = row->cap ? row->cap : 16;
    while (new_cap < need) {
        if (new_cap > (NANO_MAX_LINE_CHARS / 2)) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }
    char *new_buf = (char *)realloc(row->chars, (size_t)new_cap);
    if (!new_buf) {
        return -1;
    }
    row->chars = new_buf;
    row->cap = new_cap;
    return 0;
}

static int row_insert_char(editor_row_t *row, int at, char c) {
    if (!row) {
        return -1;
    }
    if (at < 0 || at > row->len) {
        at = row->len;
    }
    if (row->len + 2 > NANO_MAX_LINE_CHARS) {
        return -1;
    }
    if (row_reserve(row, row->len + 2) != 0) {
        return -1;
    }
    if (!row->chars) {
        return -1;
    }
    memmove(row->chars + at + 1, row->chars + at, (size_t)(row->len - at + 1));
    row->chars[at] = c;
    row->len++;
    return 0;
}

static int row_append(editor_row_t *row, const char *s, int len) {
    if (!row || len < 0) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (row->len + len + 1 > NANO_MAX_LINE_CHARS) {
        return -1;
    }
    if (row_reserve(row, row->len + len + 1) != 0) {
        return -1;
    }
    if (!row->chars) {
        return -1;
    }
    memcpy(row->chars + row->len, s, (size_t)len);
    row->len += len;
    row->chars[row->len] = '\0';
    return 0;
}

static void row_del_char(editor_row_t *row, int at) {
    if (at < 0 || at >= row->len || !row->chars) {
        return;
    }
    memmove(row->chars + at, row->chars + at + 1, (size_t)(row->len - at));
    row->len--;
}

static void editor_insert_row(int at, const char *s, int len) {
    if (at < 0 || at > E.numrows) {
        return;
    }
    if (len < 0 || len + 1 > NANO_MAX_LINE_CHARS) {
        editor_set_status("insert failed: line too long");
        return;
    }
    if (E.numrows >= NANO_MAX_ROWS) {
        editor_set_status("insert failed: too many rows");
        return;
    }

    editor_row_t new_row;
    memset(&new_row, 0, sizeof(new_row));
    if (row_reserve(&new_row, len + 1) != 0 || !new_row.chars) {
        editor_set_status("insert failed: out of memory");
        free(new_row.chars);
        return;
    }
    if (len > 0 && s) {
        memcpy(new_row.chars, s, (size_t)len);
    }
    new_row.len = len;
    new_row.chars[len] = '\0';

    editor_row_t *new_rows = (editor_row_t *)realloc(E.rows, (size_t)(E.numrows + 1) * sizeof(editor_row_t));
    if (!new_rows) {
        free(new_row.chars);
        editor_set_status("insert failed: out of memory");
        return;
    }
    E.rows = new_rows;

    if (at < E.numrows) {
        memmove(&E.rows[at + 1], &E.rows[at], (size_t)(E.numrows - at) * sizeof(editor_row_t));
    }
    E.rows[at] = new_row;
    E.numrows++;
    E.dirty++;
}

static void editor_del_row(int at) {
    if (at < 0 || at >= E.numrows) {
        return;
    }
    free(E.rows[at].chars);
    if (at < E.numrows - 1) {
        memmove(&E.rows[at], &E.rows[at + 1], (size_t)(E.numrows - at - 1) * sizeof(editor_row_t));
    }
    E.numrows--;
    E.dirty++;
}

static void editor_insert_char(char c) {
    if (E.cy == E.numrows) {
        editor_insert_row(E.numrows, "", 0);
    }
    if (E.cy < 0 || E.cy >= E.numrows) {
        return;
    }
    if (row_insert_char(&E.rows[E.cy], E.cx, c) == 0) {
        E.cx++;
        E.dirty++;
    } else {
        editor_set_status("insert failed: line too long");
    }
}

static void editor_insert_newline(void) {
    if (E.cy < 0 || E.cy > E.numrows) {
        return;
    }

    if (E.cy == E.numrows) {
        int before = E.numrows;
        editor_insert_row(E.numrows, "", 0);
        if (E.numrows != before + 1) {
            return;
        }
        E.cy++;
        E.cx = 0;
        return;
    }

    editor_row_t *row = &E.rows[E.cy];
    if (E.cx <= 0) {
        int before = E.numrows;
        editor_insert_row(E.cy, "", 0);
        if (E.numrows != before + 1) {
            return;
        }
    } else if (E.cx >= row->len) {
        int before = E.numrows;
        editor_insert_row(E.cy + 1, "", 0);
        if (E.numrows != before + 1) {
            return;
        }
    } else {
        int before = E.numrows;
        editor_insert_row(E.cy + 1, row->chars + E.cx, row->len - E.cx);
        if (E.numrows != before + 1) {
            return;
        }
        row = &E.rows[E.cy];
        row->len = E.cx;
        row->chars[E.cx] = '\0';
        E.dirty++;
    }
    E.cy++;
    E.cx = 0;
}

static void editor_del_char(void) {
    if (E.cy == E.numrows) {
        return;
    }
    if (E.cy == 0 && E.cx == 0) {
        return;
    }
    if (E.cy < 0 || E.cy >= E.numrows) {
        return;
    }

    editor_row_t *row = &E.rows[E.cy];
    if (E.cx > 0) {
        row_del_char(row, E.cx - 1);
        E.cx--;
        E.dirty++;
        return;
    }

    int prev_len = E.rows[E.cy - 1].len;
    if (row_append(&E.rows[E.cy - 1], row->chars, row->len) == 0) {
        editor_del_row(E.cy);
        E.cy--;
        E.cx = prev_len;
    } else {
        editor_set_status("merge failed: line too long");
    }
}

static void editor_open(const char *path) {
    if (!path) {
        return;
    }
    (void)snprintf(E.filename, sizeof(E.filename), "%s", path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        editor_insert_row(0, "", 0);
        E.dirty = 0;
        editor_set_status("new file: %s", E.filename);
        return;
    }

    char chunk[256];
    char *line = 0;
    int line_len = 0;
    int line_cap = 0;

    for (;;) {
        long n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            break;
        }
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\r') {
                continue;
            }
            if (c == '\n') {
                editor_insert_row(E.numrows, line ? line : "", line_len);
                line_len = 0;
                continue;
            }
            if (line_len + 2 > line_cap) {
                int new_cap = line_cap ? line_cap * 2 : 64;
                while (new_cap < line_len + 2) {
                    if (new_cap > (NANO_MAX_LINE_CHARS / 2)) {
                        new_cap = line_len + 2;
                        break;
                    }
                    new_cap *= 2;
                }
                if (new_cap > NANO_MAX_LINE_CHARS) {
                    editor_set_status("open truncated long line");
                    continue;
                }
                char *tmp = (char *)realloc(line, (size_t)new_cap);
                if (!tmp) {
                    continue;
                }
                line = tmp;
                line_cap = new_cap;
            }
            line[line_len++] = c;
            line[line_len] = '\0';
        }
    }
    (void)close(fd);

    if (line_len > 0 || E.numrows == 0) {
        editor_insert_row(E.numrows, line ? line : "", line_len);
    }
    free(line);
    E.dirty = 0;
}

static void editor_save(void) {
    if (E.filename[0] == '\0') {
        editor_set_status("save failed: no filename");
        return;
    }

    char tmp_path[300];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", E.filename);
    if (n <= 0 || n >= (int)sizeof(tmp_path)) {
        editor_set_status("save failed: filename too long");
        return;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        editor_set_status("save failed: %s", tmp_path);
        return;
    }

    int ok = 1;

    for (int i = 0; i < E.numrows; i++) {
        editor_row_t *row = &E.rows[i];
        if (row->len > 0 && write_all(fd, row->chars, row->len) != 0) {
            ok = 0;
            break;
        }
        if (i != E.numrows - 1) {
            if (write_all(fd, "\n", 1) != 0) {
                ok = 0;
                break;
            }
        }
    }
    if (ok && fsync(fd) != 0) {
        ok = 0;
    }
    (void)close(fd);

    if (!ok) {
        (void)unlink(tmp_path);
        editor_set_status("save failed during write");
        return;
    }
    if (rename(tmp_path, E.filename) != 0) {
        (void)unlink(tmp_path);
        editor_set_status("save failed during rename");
        return;
    }
    E.dirty = 0;
    editor_set_status("wrote %d line(s): %s", E.numrows, E.filename);
}

static int editor_prompt(const char *prompt, char *buf, int buflen) {
    int len = 0;
    buf[0] = '\0';

    for (;;) {
        editor_set_status(prompt, buf);
        if (E.status_ttl < 80) {
            E.status_ttl = 80;
        }

        term_write("\033[?25l");
        term_write("\033[H");
        for (int y = 0; y < E.screenrows; y++) {
            term_write("~\033[K\r\n");
        }
        term_write("\033[7m");
        for (int i = 0; i < E.screencols; i++) {
            term_write(" ");
        }
        term_write("\033[m\r\n");
        term_write("\033[K");
        term_write(E.status);
        term_write("\033[?25h");
        int c = editor_read_key();

        if (c == '\r') {
            if (len != 0) {
                editor_set_status("");
                return len;
            }
        } else if (c == '\x1b') {
            editor_set_status("");
            buf[0] = '\0';
            return -1;
        } else if (c == KEY_DEL || c == 127 || c == CTRL_KEY('h')) {
            if (len > 0) {
                len--;
                buf[len] = '\0';
            }
        } else if (is_printable((char)c)) {
            if (len < buflen - 1) {
                buf[len++] = (char)c;
                buf[len] = '\0';
            }
        }
    }
}

static void editor_find(void) {
    char query[64];
    if (editor_prompt("search (ESC to cancel): %s", query, (int)sizeof(query)) < 0) {
        editor_set_status("search canceled");
        return;
    }
    if (query[0] == '\0') {
        editor_set_status("empty search");
        return;
    }

    int start = E.cy + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = start; i < E.numrows; i++) {
            const char *p = find_substr(E.rows[i].chars, query);
            if (p) {
                E.cy = i;
                E.cx = (int)(p - E.rows[i].chars);
                E.rowoff = E.cy;
                editor_set_status("found: %s", query);
                return;
            }
        }
        start = 0;
    }
    editor_set_status("not found: %s", query);
}

static void editor_scroll(void) {
    if (E.screenrows < 1) {
        E.screenrows = 1;
    }
    if (E.screencols < 1) {
        E.screencols = 1;
    }
    if (E.numrows < 0) {
        E.numrows = 0;
    }
    if (E.cy < 0) {
        E.cy = 0;
    }
    if (E.cy > E.numrows) {
        E.cy = E.numrows;
    }
    if (E.cx < 0) {
        E.cx = 0;
    }
    if (E.rowoff < 0) {
        E.rowoff = 0;
    }
    if (E.coloff < 0) {
        E.coloff = 0;
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.cx < E.coloff) {
        E.coloff = E.cx;
    }
    if (E.cx >= E.coloff + E.screencols) {
        E.coloff = E.cx - E.screencols + 1;
    }
}

static void editor_draw_rows(void) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            term_write("~");
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                (void)snprintf(welcome, sizeof(welcome), "FuriOS nano - Ctrl+O save | Ctrl+X quit | Ctrl+F find");
                int wlen = (int)strlen(welcome);
                if (wlen > E.screencols - 2) {
                    wlen = E.screencols - 2;
                }
                int pad = (E.screencols - wlen) / 2;
                for (int i = 0; i < pad - 1; i++) {
                    term_write(" ");
                }
                term_write_n(welcome, wlen);
            }
        } else {
            editor_row_t *row = &E.rows[filerow];
            int start = E.coloff;
            if (start > row->len) {
                start = row->len;
            }
            int len = row->len - start;
            if (len < 0) {
                len = 0;
            }
            if (len > E.screencols) {
                len = E.screencols;
            }
            if (len > 0 && row->chars) {
                term_write_n(row->chars + start, len);
            }
        }
        term_write("\033[K");
        term_write("\r\n");
    }
}

static void editor_draw_status(void) {
    char left[96];
    char right[40];

    const char *name = E.filename[0] ? E.filename : "[No Name]";
    (void)snprintf(left, sizeof(left), " nano %s | %d line(s) %s",
                   name, E.numrows, E.dirty ? "(modified)" : "");
    (void)snprintf(right, sizeof(right), "%d/%d", E.cy + 1, E.numrows ? E.numrows : 1);

    int llen = (int)strlen(left);
    int rlen = (int)strlen(right);
    if (llen > E.screencols) {
        llen = E.screencols;
    }

    term_write("\033[7m");
    term_write_n(left, llen);
    while (llen < E.screencols) {
        if (E.screencols - llen == rlen) {
            term_write_n(right, rlen);
            break;
        }
        term_write(" ");
        llen++;
    }
    term_write("\033[m");
    term_write("\r\n");

    term_write("\033[K");
    if (E.status_ttl > 0) {
        int slen = (int)strlen(E.status);
        if (slen > E.screencols) {
            slen = E.screencols;
        }
        term_write_n(E.status, slen);
        E.status_ttl--;
    }
}

static void editor_refresh(void) {
    editor_scroll();

    term_write("\033[?25l");
    term_write("\033[H");
    editor_draw_rows();
    editor_draw_status();

    char pos[32];
    int x = (E.cx - E.coloff) + 1;
    int y = (E.cy - E.rowoff) + 1;
    if (x < 1) {
        x = 1;
    }
    if (y < 1) {
        y = 1;
    }
    (void)snprintf(pos, sizeof(pos), "\033[%d;%dH", y, x);
    term_write(pos);
    term_write("\033[?25h");
}

static void editor_move_cursor(int key) {
    editor_row_t *row = (E.cy >= 0 && E.cy < E.numrows) ? &E.rows[E.cy] : 0;

    switch (key) {
        case KEY_ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.rows[E.cy].len;
            }
            break;
        case KEY_ARROW_RIGHT:
            if (row && E.cx < row->len) {
                E.cx++;
            } else if (row && E.cx == row->len && E.cy < E.numrows - 1) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case KEY_ARROW_UP:
            if (E.cy > 0) {
                E.cy--;
            }
            break;
        case KEY_ARROW_DOWN:
            if (E.cy < E.numrows - 1) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= 0 && E.cy < E.numrows) ? &E.rows[E.cy] : 0;
    int rowlen = row ? row->len : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

static int editor_process_key(void) {
    int c = editor_read_key();

    switch (c) {
        case '\r':
            editor_insert_newline();
            break;
        case CTRL_KEY('q'):
        case CTRL_KEY('x'):
            if (E.dirty && E.quit_guard > 0) {
                editor_set_status("unsaved changes: press Ctrl+X %d more time(s) to quit", E.quit_guard);
                E.quit_guard--;
                return 0;
            }
            term_write("\033[2J\033[H");
            return 1;
        case CTRL_KEY('s'):
        case CTRL_KEY('o'):
            editor_save();
            break;
        case CTRL_KEY('f'):
            editor_find();
            break;
        case CTRL_KEY('g'):
            editor_set_status("Ctrl+O save | Ctrl+X quit | Ctrl+F find | Arrows move");
            break;
        case KEY_HOME:
            E.cx = 0;
            break;
        case KEY_END:
            if (E.cy >= 0 && E.cy < E.numrows) {
                E.cx = E.rows[E.cy].len;
            }
            break;
        case KEY_PAGE_UP:
        case KEY_PAGE_DOWN: {
            if (c == KEY_PAGE_UP) {
                E.cy = E.rowoff;
            } else {
                E.cy = E.rowoff + E.screenrows - 1;
                if (E.cy > E.numrows - 1) {
                    E.cy = E.numrows - 1;
                }
            }
            int times = E.screenrows;
            while (times--) {
                editor_move_cursor(c == KEY_PAGE_UP ? KEY_ARROW_UP : KEY_ARROW_DOWN);
            }
            break;
        }
        case KEY_ARROW_LEFT:
        case KEY_ARROW_RIGHT:
        case KEY_ARROW_UP:
        case KEY_ARROW_DOWN:
            editor_move_cursor(c);
            break;
        case KEY_DEL:
            editor_move_cursor(KEY_ARROW_RIGHT);
            editor_del_char();
            break;
        case KEY_BRACKETED_PASTE:
            E.paste_mode = 1;
            editor_set_status("pasting...");
            break;
        case KEY_BRACKETED_PASTE_END:
            E.paste_mode = 0;
            editor_set_status("paste done");
            break;
        case 127:
        case CTRL_KEY('h'):
            editor_del_char();
            break;
        case CTRL_KEY('l'):
        case '\x1b':
            if (E.paste_mode) {
                E.paste_mode = 0;
                editor_set_status("paste canceled");
            }
            break;
        default:
            if (E.paste_mode) {
                if (c == '\r') {
                    break;
                }
                if (c == '\n') {
                    editor_insert_newline();
                    break;
                }
                if (c == '\t') {
                    editor_insert_char(' ');
                    editor_insert_char(' ');
                    editor_insert_char(' ');
                    editor_insert_char(' ');
                    break;
                }
                if (is_printable((char)c)) {
                    editor_insert_char((char)c);
                }
                break;
            }
            if (c == '\t') {
                editor_insert_char(' ');
                editor_insert_char(' ');
                editor_insert_char(' ');
                editor_insert_char(' ');
            } else if (is_printable((char)c)) {
                editor_insert_char((char)c);
            }
            break;
    }

    E.quit_guard = 2;
    return 0;
}

static void editor_init(void) {
    memset(&E, 0, sizeof(E));
    E.screenrows = 22;
    E.screencols = 80;
    E.quit_guard = 2;
}

int main(int argc, char **argv) {
    editor_init();
    if (argc > 1) {
        editor_open(argv[1]);
    } else {
        editor_insert_row(0, "", 0);
        E.dirty = 0;
    }
    editor_set_status("Ctrl+O save | Ctrl+X quit | Ctrl+F find");

    for (;;) {
        editor_refresh();
        if (editor_process_key()) {
            break;
        }
    }
    return 0;
}
