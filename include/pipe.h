#ifndef FUROS_PIPE_H
#define FUROS_PIPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PIPE_CAPACITY 1024
#define MAX_PIPES 32

typedef struct pipe {
    bool used;
    uint8_t data[PIPE_CAPACITY];
    size_t rpos;
    size_t wpos;
    size_t count;
    int readers;
    int writers;
} pipe_t;

void pipe_init(void);
pipe_t *pipe_alloc(void);
void pipe_ref_read(pipe_t *p);
void pipe_ref_write(pipe_t *p);
void pipe_close_read(pipe_t *p);
void pipe_close_write(pipe_t *p);
bool pipe_has_readers(const pipe_t *p);
bool pipe_has_writers(const pipe_t *p);
size_t pipe_available_read(const pipe_t *p);
size_t pipe_available_write(const pipe_t *p);
size_t pipe_read(pipe_t *p, void *dst, size_t len);
size_t pipe_write(pipe_t *p, const void *src, size_t len);

#endif
