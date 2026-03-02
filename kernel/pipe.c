#include "pipe.h"
#include "string.h"

static pipe_t pipes[MAX_PIPES];

static void pipe_maybe_release(pipe_t *p) {
    if (!p) {
        return;
    }
    if (p->readers == 0 && p->writers == 0) {
        memset(p, 0, sizeof(*p));
    }
}

void pipe_init(void) {
    memset(pipes, 0, sizeof(pipes));
}

pipe_t *pipe_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].used) {
            memset(&pipes[i], 0, sizeof(pipes[i]));
            pipes[i].used = true;
            pipes[i].readers = 1;
            pipes[i].writers = 1;
            return &pipes[i];
        }
    }
    return 0;
}

void pipe_ref_read(pipe_t *p) {
    if (!p || !p->used) {
        return;
    }
    p->readers++;
}

void pipe_ref_write(pipe_t *p) {
    if (!p || !p->used) {
        return;
    }
    p->writers++;
}

void pipe_close_read(pipe_t *p) {
    if (!p || !p->used || p->readers <= 0) {
        return;
    }
    p->readers--;
    pipe_maybe_release(p);
}

void pipe_close_write(pipe_t *p) {
    if (!p || !p->used || p->writers <= 0) {
        return;
    }
    p->writers--;
    pipe_maybe_release(p);
}

bool pipe_has_readers(const pipe_t *p) {
    return p && p->used && p->readers > 0;
}

bool pipe_has_writers(const pipe_t *p) {
    return p && p->used && p->writers > 0;
}

size_t pipe_available_read(const pipe_t *p) {
    if (!p || !p->used) {
        return 0;
    }
    return p->count;
}

size_t pipe_available_write(const pipe_t *p) {
    if (!p || !p->used || p->count >= PIPE_CAPACITY) {
        return 0;
    }
    return PIPE_CAPACITY - p->count;
}

size_t pipe_read(pipe_t *p, void *dst, size_t len) {
    if (!p || !p->used || !dst || len == 0) {
        return 0;
    }
    if (p->count == 0) {
        return 0;
    }

    size_t n = len;
    if (n > p->count) {
        n = p->count;
    }

    uint8_t *d = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = p->data[p->rpos];
        p->rpos = (p->rpos + 1) % PIPE_CAPACITY;
    }
    p->count -= n;
    return n;
}

size_t pipe_write(pipe_t *p, const void *src, size_t len) {
    if (!p || !p->used || !src || len == 0) {
        return 0;
    }
    if (p->count >= PIPE_CAPACITY) {
        return 0;
    }

    size_t space = PIPE_CAPACITY - p->count;
    size_t n = len;
    if (n > space) {
        n = space;
    }

    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) {
        p->data[p->wpos] = s[i];
        p->wpos = (p->wpos + 1) % PIPE_CAPACITY;
    }
    p->count += n;
    return n;
}
