#include "../user.h"

typedef struct block_header {
    size_t size;
    int free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_head;
static block_header_t *heap_tail;

static size_t align_up(size_t v, size_t a) {
    return (v + a - 1U) & ~(a - 1U);
}

static void maybe_split_block(block_header_t *b, size_t need) {
    size_t min_remain = sizeof(block_header_t) + 16U;
    if (!b || b->size < need + min_remain) {
        return;
    }
    block_header_t *n = (block_header_t *)((char *)(b + 1) + need);
    n->size = b->size - need - sizeof(block_header_t);
    n->free = 1;
    n->next = b->next;
    b->size = need;
    b->next = n;
    if (heap_tail == b) {
        heap_tail = n;
    }
}

static void coalesce_all(void) {
    block_header_t *b = heap_head;
    while (b && b->next) {
        block_header_t *n = b->next;
        if (b->free && n->free &&
            (char *)(b + 1) + b->size == (char *)n) {
            b->size += sizeof(block_header_t) + n->size;
            b->next = n->next;
            if (heap_tail == n) {
                heap_tail = b;
            }
            continue;
        }
        b = b->next;
    }
}

void *malloc(size_t size) {
    if (size == 0U) {
        return 0;
    }
    size = align_up(size, 16U);

    for (block_header_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            maybe_split_block(b, size);
            b->free = 0;
            return (void *)(b + 1);
        }
    }

    size_t total = sizeof(block_header_t) + size;
    block_header_t *b = (block_header_t *)sys_sbrk((long)total);
    if (b == (void *)-1) {
        return 0;
    }
    b->size = size;
    b->free = 0;
    b->next = 0;

    if (!heap_head) {
        heap_head = b;
        heap_tail = b;
    } else {
        heap_tail->next = b;
        heap_tail = b;
    }
    return (void *)(b + 1);
}

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    block_header_t *b = ((block_header_t *)ptr) - 1;
    b->free = 1;
    coalesce_all();
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0U) {
        free(ptr);
        return 0;
    }

    size = align_up(size, 16U);
    block_header_t *b = ((block_header_t *)ptr) - 1;
    if (b->size >= size) {
        maybe_split_block(b, size);
        return ptr;
    }

    if (b->next && b->next->free &&
        (char *)(b + 1) + b->size == (char *)b->next &&
        (b->size + sizeof(block_header_t) + b->next->size) >= size) {
        b->size += sizeof(block_header_t) + b->next->size;
        b->next = b->next->next;
        if (!b->next) {
            heap_tail = b;
        }
        maybe_split_block(b, size);
        return ptr;
    }

    void *n = malloc(size);
    if (!n) {
        return 0;
    }
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

void *calloc(size_t n, size_t size) {
    if (n == 0U || size == 0U) {
        return malloc(0U);
    }
    if (n > ((size_t)-1) / size) {
        return 0;
    }
    size_t total = n * size;
    void *p = malloc(total);
    if (!p) {
        return 0;
    }
    memset(p, 0, total);
    return p;
}

void abort(void) {
    puts("abort\n");
    sys_exit(127);
    for (;;) {
    }
}
