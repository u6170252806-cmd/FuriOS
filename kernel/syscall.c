#include "syscall.h"
#include "task.h"
#include "fs.h"
#include "pipe.h"
#include "user_copy.h"
#include "string.h"
#include "timer.h"
#include "uart.h"

static int normalize_abs_path(const char *in, char *out, size_t outsz) {
    char comps[16][INODE_NAME_MAX + 1];
    int comp_count = 0;
    const char *p = in;

    if (!in || in[0] != '/' || outsz < 2) {
        return -1;
    }

    while (*p == '/') {
        p++;
    }

    while (*p) {
        char comp[INODE_NAME_MAX + 1];
        int n = 0;
        while (*p && *p != '/') {
            if (n < INODE_NAME_MAX) {
                comp[n++] = *p;
            }
            p++;
        }
        comp[n] = '\0';

        while (*p == '/') {
            p++;
        }

        if (n == 0 || (n == 1 && comp[0] == '.')) {
            continue;
        }
        if (n == 2 && comp[0] == '.' && comp[1] == '.') {
            if (comp_count > 0) {
                comp_count--;
            }
            continue;
        }
        if (comp_count >= (int)(sizeof(comps) / sizeof(comps[0]))) {
            return -1;
        }
        strcpy(comps[comp_count++], comp);
    }

    size_t pos = 0;
    out[pos++] = '/';
    for (int i = 0; i < comp_count; i++) {
        size_t n = strlen(comps[i]);
        if (pos + n + 1 >= outsz) {
            return -1;
        }
        if (pos > 1) {
            out[pos++] = '/';
        }
        memcpy(out + pos, comps[i], n);
        pos += n;
    }
    out[pos] = '\0';
    return 0;
}

static int resolve_path(task_t *t, const char *path_in, char *path_out, size_t outsz) {
    if (!t || !path_in || !path_out || outsz < 2) {
        return -1;
    }
    if (path_in[0] == '/') {
        return normalize_abs_path(path_in, path_out, outsz);
    }

    char joined[MAX_PATH];
    if (strcmp(t->cwd, "/") == 0) {
        joined[0] = '/';
        strncpy(joined + 1, path_in, sizeof(joined) - 2);
        joined[sizeof(joined) - 1] = '\0';
    } else {
        strncpy(joined, t->cwd, sizeof(joined) - 1);
        joined[sizeof(joined) - 1] = '\0';
        size_t len = strlen(joined);
        if (len + 1 >= sizeof(joined)) {
            return -1;
        }
        joined[len++] = '/';
        while (*path_in && len < sizeof(joined) - 1) {
            joined[len++] = *path_in++;
        }
        joined[len] = '\0';
    }
    return normalize_abs_path(joined, path_out, outsz);
}

static int fd_mode(int flags) {
    return flags & O_ACCMODE;
}

static bool open_flags_valid(int flags) {
    const int allowed = O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND | O_NONBLOCK;
    return (flags & ~allowed) == 0;
}

#define MAX_POLL_FDS 16

static long sys_write_impl(trapframe_t *tf, uint64_t fd, uint64_t buf, uint64_t len,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;
    task_t *t = task_current();
    if (!t || len == 0) {
        return 0;
    }

    if (fd >= MAX_FDS || !t->fds[fd].used) {
        return -1;
    }

    fd_t *f = &t->fds[fd];
    const char *user_buf = (const char *)buf;

    if (f->kind == FD_NONE) {
        if (fd > 2) {
            return -1;
        }
        for (uint64_t i = 0; i < len; i++) {
            char c;
            if (copy_from_user(&c, user_buf + i, 1) != 0) {
                return -1;
            }
            uart_putc(c);
        }
        return (long)len;
    }

    if (f->kind == FD_INODE) {
        if (!f->inode) {
            return -1;
        }
        int mode = fd_mode(f->flags);
        if (mode == O_RDONLY) {
            return -1;
        }
        uint8_t tmp[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            if (copy_from_user(tmp, user_buf + done, chunk) != 0) {
                return done ? (long)done : -1;
            }
            if (f->flags & O_APPEND) {
                f->offset = f->inode->size;
            }
            int n = fs_write(f->inode, &f->offset, tmp, (size_t)chunk);
            if (n <= 0) {
                return done ? (long)done : -1;
            }
            done += (uint64_t)n;
        }
        return (long)done;
    }

    if (f->kind == FD_PIPE_W) {
        if (!f->pipe) {
            return -1;
        }
        uint8_t tmp[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            if (copy_from_user(tmp, user_buf + done, chunk) != 0) {
                return done ? (long)done : -1;
            }

            if (!pipe_has_readers(f->pipe)) {
                return done ? (long)done : -1;
            }

            size_t n = pipe_write(f->pipe, tmp, (size_t)chunk);
            if (n == 0) {
                if (done > 0) {
                    task_wake_pipe(f->pipe);
                    return (long)done;
                }
                if (f->flags & O_NONBLOCK) {
                    return -11;
                }
                task_block_on_pipe_write(f->pipe);
                return -2;
            }
            done += (uint64_t)n;
            task_wake_pipe(f->pipe);
        }
        return (long)done;
    }

    if (f->kind == FD_PIPE_R) {
        return -1;
    }
    return -1;
}

static long sys_read_impl(trapframe_t *tf, uint64_t fd, uint64_t buf, uint64_t len,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;
    task_t *t = task_current();
    if (!t || len == 0) {
        return 0;
    }

    if (fd >= MAX_FDS || !t->fds[fd].used) {
        return -1;
    }

    fd_t *f = &t->fds[fd];
    char *user_buf = (char *)buf;

    if (f->kind == FD_NONE) {
        if (fd != 0) {
            return -1;
        }
        if ((f->flags & O_NONBLOCK) && !uart_rx_ready()) {
            return -11;
        }
        for (uint64_t i = 0; i < len; i++) {
            int c = uart_getc();
            char ch = (char)c;
            if (copy_to_user(user_buf + i, &ch, 1) != 0) {
                return i ? (long)i : -1;
            }
            if (ch == '\n' || ch == '\r') {
                return (long)(i + 1);
            }
        }
        return (long)len;
    }

    if (f->kind == FD_INODE) {
        if (!f->inode) {
            return -1;
        }
        int mode = fd_mode(f->flags);
        if (mode == O_WRONLY) {
            return -1;
        }
        if ((f->flags & O_NONBLOCK) && fs_is_tty(f->inode) && !uart_rx_ready()) {
            return -11;
        }
        uint8_t tmp[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            int n = fs_read(f->inode, &f->offset, tmp, (size_t)chunk);
            if (n <= 0) {
                return done ? (long)done : n;
            }
            if (copy_to_user(user_buf + done, tmp, (size_t)n) != 0) {
                return done ? (long)done : -1;
            }
            done += (uint64_t)n;
            if ((uint64_t)n < chunk) {
                break;
            }
        }
        return (long)done;
    }

    if (f->kind == FD_PIPE_R) {
        if (!f->pipe) {
            return -1;
        }
        uint8_t tmp[128];
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > sizeof(tmp)) {
                chunk = sizeof(tmp);
            }
            size_t n = pipe_read(f->pipe, tmp, (size_t)chunk);
            if (n == 0) {
                if (done > 0) {
                    task_wake_pipe(f->pipe);
                    return (long)done;
                }
                if (!pipe_has_writers(f->pipe)) {
                    return 0;
                }
                if (f->flags & O_NONBLOCK) {
                    return -11;
                }
                task_block_on_pipe_read(f->pipe);
                return -2;
            }
            if (copy_to_user(user_buf + done, tmp, n) != 0) {
                return done ? (long)done : -1;
            }
            done += (uint64_t)n;
            task_wake_pipe(f->pipe);
        }
        return (long)done;
    }

    if (f->kind == FD_PIPE_W) {
        return -1;
    }
    return -1;
}

static long sys_open_impl(trapframe_t *tf, uint64_t path, uint64_t flags, uint64_t mode,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)mode;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }

    inode_t *ino = fs_lookup(kpath);
    if (!ino && (flags & O_CREAT)) {
        ino = fs_create_file(kpath);
    }
    if (!ino) {
        return -1;
    }

    int iflags = (int)flags;
    if (!open_flags_valid(iflags)) {
        return -1;
    }

    int mode_bits = fd_mode(iflags);
    if (mode_bits > O_RDWR) {
        return -1;
    }
    if ((iflags & O_APPEND) && mode_bits == O_RDONLY) {
        return -1;
    }
    if ((iflags & O_TRUNC) && mode_bits == O_RDONLY) {
        return -1;
    }
    if (ino->type == INODE_DIR && mode_bits != O_RDONLY) {
        return -1;
    }
    if (ino->type == INODE_FILE &&
        (mode_bits == O_WRONLY || mode_bits == O_RDWR) &&
        !ino->writable) {
        return -1;
    }
    if (ino->type == INODE_DEV &&
        (mode_bits == O_WRONLY || mode_bits == O_RDWR) &&
        !ino->writable) {
        return -1;
    }

    int fd = task_fd_alloc(t);
    if (fd < 0) {
        return -1;
    }

    t->fds[fd].inode = ino;
    t->fds[fd].pipe = 0;
    t->fds[fd].kind = FD_INODE;
    t->fds[fd].flags = iflags;
    t->fds[fd].offset = (iflags & O_APPEND) ? ino->size : 0;
    if ((iflags & O_TRUNC) && ino->type == INODE_FILE) {
        if (fs_truncate(ino, 0) != 0) {
            task_fd_close(t, fd);
            return -1;
        }
        t->fds[fd].offset = 0;
    }
    return fd;
}

static long sys_close_impl(trapframe_t *tf, uint64_t fd, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || fd >= MAX_FDS) {
        return -1;
    }
    task_fd_close(t, (int)fd);
    return 0;
}

static long sys_dup2_impl(trapframe_t *tf, uint64_t oldfd, uint64_t newfd, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    return task_fd_dup2(t, (int)oldfd, (int)newfd);
}

static long sys_pipe_impl(trapframe_t *tf, uint64_t user_fds, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || user_fds == 0) {
        return -1;
    }

    pipe_t *p = pipe_alloc();
    if (!p) {
        return -1;
    }

    int rfd = task_fd_alloc(t);
    if (rfd < 0) {
        pipe_close_read(p);
        pipe_close_write(p);
        return -1;
    }

    int wfd = task_fd_alloc(t);
    if (wfd < 0) {
        task_fd_close(t, rfd);
        pipe_close_read(p);
        pipe_close_write(p);
        return -1;
    }

    t->fds[rfd].kind = FD_PIPE_R;
    t->fds[rfd].pipe = p;
    t->fds[rfd].inode = 0;
    t->fds[rfd].offset = 0;
    t->fds[rfd].flags = O_RDONLY;

    t->fds[wfd].kind = FD_PIPE_W;
    t->fds[wfd].pipe = p;
    t->fds[wfd].inode = 0;
    t->fds[wfd].offset = 0;
    t->fds[wfd].flags = O_WRONLY;

    int out[2] = {rfd, wfd};
    if (copy_to_user((void *)user_fds, out, sizeof(out)) != 0) {
        task_fd_close(t, rfd);
        task_fd_close(t, wfd);
        return -1;
    }
    return 0;
}

static long sys_sleep_impl(trapframe_t *tf, uint64_t ticks, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    if (ticks == 0) {
        return 0;
    }

    uint64_t now = timer_ticks();
    uint64_t wake = now + ticks;
    if (wake < now) {
        wake = ~0ULL;
    }
    task_block_on_sleep(wake);
    return -2;
}

static long sys_brk_impl(trapframe_t *tf, uint64_t addr, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_brk(addr);
}

static long sys_exit_impl(trapframe_t *tf, uint64_t code, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    task_mark_exit((int)code);
    return 0;
}

static long sys_yield_impl(trapframe_t *tf, uint64_t a0, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    task_yield();
    return 0;
}

static long sys_fork_impl(trapframe_t *tf, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_fork(tf);
}

static long sys_exec_impl(trapframe_t *tf, uint64_t path, uint64_t argv_ptr, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }

    inode_t *ino = fs_lookup(kpath);
    if (!ino || ino->type != INODE_FILE || !ino->executable) {
        return -1;
    }

    const char *kargv_store[MAX_ARGV + 1];
    char arg_buf[MAX_ARGV][MAX_ARG_LEN];
    memset(kargv_store, 0, sizeof(kargv_store));

    if (argv_ptr) {
        for (int i = 0; i < MAX_ARGV; i++) {
            uint64_t uptr = 0;
            if (copy_from_user(&uptr, (const void *)(argv_ptr + (uint64_t)i * sizeof(uint64_t)),
                               sizeof(uint64_t)) != 0) {
                return -1;
            }
            if (uptr == 0) {
                break;
            }
            if (copy_user_str(arg_buf[i], (const char *)uptr, MAX_ARG_LEN) != 0) {
                return -1;
            }
            kargv_store[i] = arg_buf[i];
        }
    }

    int rc = task_exec(t, ino->data, ino->size, kargv_store);
    if (rc != 0) {
        return -1;
    }

    if (!t->tf) {
        return -1;
    }
    *tf = *t->tf;
    return 0;
}

static long sys_wait_impl(trapframe_t *tf, uint64_t pid, uint64_t status_ptr, uint64_t options,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;

    int status = 0;
    int rc = task_wait((int)pid, (int)options, &status);
    if (rc > 0 && status_ptr) {
        if (copy_to_user((void *)status_ptr, &status, sizeof(status)) != 0) {
            return -1;
        }
    }
    return rc;
}

static long sys_getpid_impl(trapframe_t *tf, uint64_t a0, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    return t ? t->pid : -1;
}

static long sys_kill_impl(trapframe_t *tf, uint64_t pid, uint64_t sig, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_kill((int)pid, (int)sig);
}

static long sys_fcntl_impl(trapframe_t *tf, uint64_t fd, uint64_t cmd, uint64_t arg,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || fd >= MAX_FDS || !t->fds[fd].used) {
        return -1;
    }
    fd_t *f = &t->fds[fd];

    switch ((int)cmd) {
        case F_GETFL:
            return f->flags;
        case F_SETFL: {
            int keep = f->flags & O_ACCMODE;
            int newf = (int)arg;
            if (newf & O_APPEND) {
                keep |= O_APPEND;
            }
            if (newf & O_NONBLOCK) {
                keep |= O_NONBLOCK;
            }
            f->flags = keep;
            return 0;
        }
        default:
            return -1;
    }
}

static int16_t poll_fd_revents(task_t *t, int fd, int16_t events) {
    if (!t || fd < 0 || fd >= MAX_FDS || !t->fds[fd].used) {
        return POLLNVAL;
    }

    fd_t *f = &t->fds[fd];
    int mode = fd_mode(f->flags);
    int16_t revents = 0;

    if (f->kind == FD_NONE) {
        if (fd == 0) {
            if ((events & POLLIN) && uart_rx_ready()) {
                revents |= POLLIN;
            }
        } else if (fd == 1 || fd == 2) {
            if (events & POLLOUT) {
                revents |= POLLOUT;
            }
        } else {
            revents |= POLLNVAL;
        }
        return revents;
    }

    if (f->kind == FD_INODE) {
        if (!f->inode) {
            return POLLERR;
        }
        if (f->inode->type == INODE_DEV) {
            if (f->inode->dev_kind == DEV_TTY) {
                if ((events & POLLIN) && mode != O_WRONLY && uart_rx_ready()) {
                    revents |= POLLIN;
                }
                if ((events & POLLOUT) && mode != O_RDONLY) {
                    revents |= POLLOUT;
                }
                return revents;
            }
            if ((events & POLLIN) && mode != O_WRONLY) {
                revents |= POLLIN;
            }
            if ((events & POLLOUT) && mode != O_RDONLY) {
                revents |= POLLOUT;
            }
            return revents;
        }
        if ((events & POLLIN) && mode != O_WRONLY) {
            if (f->inode->type == INODE_FILE && f->offset < f->inode->size) {
                revents |= POLLIN;
            }
            if (f->inode->type == INODE_DIR &&
                f->offset < (size_t)f->inode->child_count * sizeof(dirent_t)) {
                revents |= POLLIN;
            }
        }
        if ((events & POLLOUT) && mode != O_RDONLY) {
            if (f->inode->type == INODE_FILE && f->inode->writable) {
                revents |= POLLOUT;
            }
        }
        return revents;
    }

    if (f->kind == FD_PIPE_R) {
        if (!f->pipe) {
            return POLLERR;
        }
        if (!pipe_has_writers(f->pipe)) {
            revents |= POLLHUP;
        }
        if ((events & POLLIN) &&
            (pipe_available_read(f->pipe) > 0 || !pipe_has_writers(f->pipe))) {
            revents |= POLLIN;
        }
        return revents;
    }

    if (f->kind == FD_PIPE_W) {
        if (!f->pipe) {
            return POLLERR;
        }
        if (!pipe_has_readers(f->pipe)) {
            revents |= POLLERR | POLLHUP;
            return revents;
        }
        if ((events & POLLOUT) && pipe_available_write(f->pipe) > 0) {
            revents |= POLLOUT;
        }
        return revents;
    }

    return POLLNVAL;
}

static long sys_poll_impl(trapframe_t *tf, uint64_t fds_ptr, uint64_t nfds, uint64_t timeout_ticks,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }
    if ((long)nfds < 0 || nfds > MAX_POLL_FDS) {
        return -1;
    }
    if (nfds == 0) {
        if (timeout_ticks == 0) {
            return 0;
        }
        task_block_on_sleep(timer_ticks() + 1);
        return -2;
    }

    fu_pollfd_t kfds[MAX_POLL_FDS];
    size_t bytes = (size_t)nfds * sizeof(kfds[0]);
    if (copy_from_user(kfds, (const void *)fds_ptr, bytes) != 0) {
        return -1;
    }

    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        kfds[i].revents = poll_fd_revents(t, kfds[i].fd, kfds[i].events);
        if (kfds[i].revents & (kfds[i].events | POLLERR | POLLHUP | POLLNVAL)) {
            ready++;
        }
    }

    if (copy_to_user((void *)fds_ptr, kfds, bytes) != 0) {
        return -1;
    }
    if (ready > 0) {
        return ready;
    }
    if (timeout_ticks == 0) {
        return 0;
    }

    task_block_on_sleep(timer_ticks() + 1);
    return -2;
}

static long sys_setpgid_impl(trapframe_t *tf, uint64_t pid, uint64_t pgid, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_setpgid((int)pid, (int)pgid);
}

static long sys_getpgid_impl(trapframe_t *tf, uint64_t pid, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_getpgid((int)pid);
}

static long sys_mmap_impl(trapframe_t *tf, uint64_t addr, uint64_t len, uint64_t prot,
                          uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)tf;
    return task_mmap(addr, len, (int)prot, (int)flags, (int)fd, offset);
}

static long sys_munmap_impl(trapframe_t *tf, uint64_t addr, uint64_t len, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_munmap(addr, len);
}

static long sys_mprotect_impl(trapframe_t *tf, uint64_t addr, uint64_t len, uint64_t prot,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;
    return task_mprotect(addr, len, (int)prot);
}

static long sys_chdir_impl(trapframe_t *tf, uint64_t path, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }

    inode_t *ino = fs_lookup(kpath);
    if (!ino || ino->type != INODE_DIR) {
        return -1;
    }

    strcpy(t->cwd, kpath);
    return 0;
}

static long sys_mkdir_impl(trapframe_t *tf, uint64_t path, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }
    return fs_create_dir(kpath) ? 0 : -1;
}

static long sys_rmdir_impl(trapframe_t *tf, uint64_t path, uint64_t a1, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }
    return fs_rmdir(kpath);
}

static long sys_unlink_impl(trapframe_t *tf, uint64_t path, uint64_t a1, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(input_path, (const char *)path, sizeof(input_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }
    return fs_unlink(kpath);
}

static long sys_getcwd_impl(trapframe_t *tf, uint64_t buf, uint64_t len, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || buf == 0 || len == 0) {
        return -1;
    }

    size_t need = strlen(t->cwd) + 1;
    if (need > len) {
        return -1;
    }
    if (copy_to_user((void *)buf, t->cwd, need) != 0) {
        return -1;
    }
    return 0;
}

static long sys_rename_impl(trapframe_t *tf, uint64_t old_path, uint64_t new_path, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t) {
        return -1;
    }

    char input_old[MAX_PATH];
    char input_new[MAX_PATH];
    char kold[MAX_PATH];
    char knew[MAX_PATH];

    if (copy_user_str(input_old, (const char *)old_path, sizeof(input_old)) != 0) {
        return -1;
    }
    if (copy_user_str(input_new, (const char *)new_path, sizeof(input_new)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_old, kold, sizeof(kold)) != 0) {
        return -1;
    }
    if (resolve_path(t, input_new, knew, sizeof(knew)) != 0) {
        return -1;
    }
    return fs_rename(kold, knew);
}

static long sys_mount_impl(trapframe_t *tf, uint64_t source, uint64_t target, uint64_t fstype,
                           uint64_t flags, uint64_t data, uint64_t a5) {
    (void)tf;
    (void)data;
    (void)a5;

    task_t *t = task_current();
    if (!t || source == 0 || target == 0 || fstype == 0) {
        return -1;
    }

    char in_src[MAX_PATH];
    char in_tgt[MAX_PATH];
    char ksrc[MAX_PATH];
    char ktgt[MAX_PATH];
    char kfstype[16];

    if (copy_user_str(in_src, (const char *)source, sizeof(in_src)) != 0) {
        return -1;
    }
    if (copy_user_str(in_tgt, (const char *)target, sizeof(in_tgt)) != 0) {
        return -1;
    }
    if (copy_user_str(kfstype, (const char *)fstype, sizeof(kfstype)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_src, ksrc, sizeof(ksrc)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_tgt, ktgt, sizeof(ktgt)) != 0) {
        return -1;
    }
    return fs_mount(ksrc, ktgt, kfstype, flags);
}

static long sys_umount_impl(trapframe_t *tf, uint64_t target, uint64_t flags, uint64_t a2,
                            uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || target == 0) {
        return -1;
    }

    char in_tgt[MAX_PATH];
    char ktgt[MAX_PATH];
    if (copy_user_str(in_tgt, (const char *)target, sizeof(in_tgt)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_tgt, ktgt, sizeof(ktgt)) != 0) {
        return -1;
    }
    return fs_umount(ktgt, flags);
}

static long sys_mkfsext4_impl(trapframe_t *tf, uint64_t target, uint64_t flags, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || target == 0) {
        return -1;
    }

    char in_tgt[MAX_PATH];
    char ktgt[MAX_PATH];
    fu_mkfs_ext4_opts_t opts_local;
    const fu_mkfs_ext4_opts_t *opts_ptr = 0;
    if (copy_user_str(in_tgt, (const char *)target, sizeof(in_tgt)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_tgt, ktgt, sizeof(ktgt)) != 0) {
        return -1;
    }
    if (a2 != 0U) {
        uint32_t user_size = 0U;
        if (copy_from_user(&user_size, (const void *)a2, sizeof(user_size)) != 0) {
            return -1;
        }
        if (user_size < 48U || user_size > sizeof(opts_local)) {
            return -1;
        }
        memset(&opts_local, 0, sizeof(opts_local));
        if (copy_from_user(&opts_local, (const void *)a2, user_size) != 0) {
            return -1;
        }
        opts_local.size = user_size;
        opts_ptr = &opts_local;
    }
    return fs_mkfs_ext4(ktgt, flags, opts_ptr);
}

static long sys_fsckext4_impl(trapframe_t *tf, uint64_t target, uint64_t flags, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || target == 0) {
        return -1;
    }

    char in_tgt[MAX_PATH];
    char ktgt[MAX_PATH];
    if (copy_user_str(in_tgt, (const char *)target, sizeof(in_tgt)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_tgt, ktgt, sizeof(ktgt)) != 0) {
        return -1;
    }
    return fs_fsck_ext4(ktgt, flags);
}

static long sys_link_impl(trapframe_t *tf, uint64_t old_path, uint64_t new_path, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || old_path == 0 || new_path == 0) {
        return -1;
    }

    char in_old[MAX_PATH];
    char in_new[MAX_PATH];
    char kold[MAX_PATH];
    char knew[MAX_PATH];
    if (copy_user_str(in_old, (const char *)old_path, sizeof(in_old)) != 0) {
        return -1;
    }
    if (copy_user_str(in_new, (const char *)new_path, sizeof(in_new)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_old, kold, sizeof(kold)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_new, knew, sizeof(knew)) != 0) {
        return -1;
    }
    return fs_link(kold, knew);
}

static long sys_symlink_impl(trapframe_t *tf, uint64_t target, uint64_t link_path, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || target == 0 || link_path == 0) {
        return -1;
    }

    char target_str[MAX_PATH];
    char in_link[MAX_PATH];
    char klink[MAX_PATH];
    if (copy_user_str(target_str, (const char *)target, sizeof(target_str)) != 0) {
        return -1;
    }
    if (copy_user_str(in_link, (const char *)link_path, sizeof(in_link)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_link, klink, sizeof(klink)) != 0) {
        return -1;
    }
    return fs_symlink(target_str, klink);
}

static long sys_readlink_impl(trapframe_t *tf, uint64_t path, uint64_t buf, uint64_t buflen,
                              uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || path == 0 || buf == 0) {
        return -1;
    }

    char in_path[MAX_PATH];
    char kpath[MAX_PATH];
    if (copy_user_str(in_path, (const char *)path, sizeof(in_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }

    char kbuf[MAX_PATH];
    size_t cap = buflen;
    if (cap > sizeof(kbuf)) {
        cap = sizeof(kbuf);
    }
    int n = fs_readlink(kpath, kbuf, cap);
    if (n < 0) {
        return -1;
    }
    if (n > 0 && copy_to_user((void *)buf, kbuf, (size_t)n) != 0) {
        return -1;
    }
    return n;
}

static long sys_lstat_impl(trapframe_t *tf, uint64_t path, uint64_t statbuf, uint64_t a2,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    task_t *t = task_current();
    if (!t || path == 0 || statbuf == 0) {
        return -1;
    }

    char in_path[MAX_PATH];
    char kpath[MAX_PATH];
    fu_stat_t st;
    if (copy_user_str(in_path, (const char *)path, sizeof(in_path)) != 0) {
        return -1;
    }
    if (resolve_path(t, in_path, kpath, sizeof(kpath)) != 0) {
        return -1;
    }
    if (fs_lstat(kpath, &st) != 0) {
        return -1;
    }
    if (copy_to_user((void *)statbuf, &st, sizeof(st)) != 0) {
        return -1;
    }
    return 0;
}

typedef long (*sys_fn_t)(trapframe_t *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

typedef struct {
    uint16_t nr;
    const char *name;
    uint8_t argc;
} syscall_info_t;

#define SYSCALL_HANDLER_FOR_SYS_WRITE  sys_write_impl
#define SYSCALL_HANDLER_FOR_SYS_READ   sys_read_impl
#define SYSCALL_HANDLER_FOR_SYS_OPEN   sys_open_impl
#define SYSCALL_HANDLER_FOR_SYS_CLOSE  sys_close_impl
#define SYSCALL_HANDLER_FOR_SYS_EXIT   sys_exit_impl
#define SYSCALL_HANDLER_FOR_SYS_YIELD  sys_yield_impl
#define SYSCALL_HANDLER_FOR_SYS_FORK   sys_fork_impl
#define SYSCALL_HANDLER_FOR_SYS_EXEC   sys_exec_impl
#define SYSCALL_HANDLER_FOR_SYS_WAIT   sys_wait_impl
#define SYSCALL_HANDLER_FOR_SYS_GETPID sys_getpid_impl
#define SYSCALL_HANDLER_FOR_SYS_KILL   sys_kill_impl
#define SYSCALL_HANDLER_FOR_SYS_FCNTL  sys_fcntl_impl
#define SYSCALL_HANDLER_FOR_SYS_POLL   sys_poll_impl
#define SYSCALL_HANDLER_FOR_SYS_SETPGID sys_setpgid_impl
#define SYSCALL_HANDLER_FOR_SYS_GETPGID sys_getpgid_impl
#define SYSCALL_HANDLER_FOR_SYS_MMAP   sys_mmap_impl
#define SYSCALL_HANDLER_FOR_SYS_MUNMAP sys_munmap_impl
#define SYSCALL_HANDLER_FOR_SYS_MPROTECT sys_mprotect_impl
#define SYSCALL_HANDLER_FOR_SYS_CHDIR  sys_chdir_impl
#define SYSCALL_HANDLER_FOR_SYS_MKDIR  sys_mkdir_impl
#define SYSCALL_HANDLER_FOR_SYS_RMDIR  sys_rmdir_impl
#define SYSCALL_HANDLER_FOR_SYS_UNLINK sys_unlink_impl
#define SYSCALL_HANDLER_FOR_SYS_GETCWD sys_getcwd_impl
#define SYSCALL_HANDLER_FOR_SYS_RENAME sys_rename_impl
#define SYSCALL_HANDLER_FOR_SYS_DUP2   sys_dup2_impl
#define SYSCALL_HANDLER_FOR_SYS_PIPE   sys_pipe_impl
#define SYSCALL_HANDLER_FOR_SYS_SLEEP  sys_sleep_impl
#define SYSCALL_HANDLER_FOR_SYS_BRK    sys_brk_impl
#define SYSCALL_HANDLER_FOR_SYS_MOUNT  sys_mount_impl
#define SYSCALL_HANDLER_FOR_SYS_UMOUNT sys_umount_impl
#define SYSCALL_HANDLER_FOR_SYS_MKFSEXT4 sys_mkfsext4_impl
#define SYSCALL_HANDLER_FOR_SYS_FSCKEXT4 sys_fsckext4_impl
#define SYSCALL_HANDLER_FOR_SYS_LINK   sys_link_impl
#define SYSCALL_HANDLER_FOR_SYS_SYMLINK sys_symlink_impl
#define SYSCALL_HANDLER_FOR_SYS_READLINK sys_readlink_impl
#define SYSCALL_HANDLER_FOR_SYS_LSTAT  sys_lstat_impl
#define SYSCALL_HANDLER_FOR(sym)       SYSCALL_HANDLER_FOR_##sym

enum {
    FUROS_SYSCALL_INFO_COUNT =
#define X(sym, nr, sc_name, sc_argc) 1 +
        FUROS_SYSCALL_LIST(X)
#undef X
        0
};

_Static_assert((int)FUROS_SYSCALL_INFO_COUNT == (int)SYS_MAX, "syscall enum/table mismatch");

static const syscall_info_t syscall_info_table[SYS_MAX] = {
#define X(sym, sc_nr, sc_name, sc_argc) [sym] = { .nr = sc_nr, .name = sc_name, .argc = sc_argc },
    FUROS_SYSCALL_LIST(X)
#undef X
};

static const sys_fn_t syscall_handler_table[SYS_MAX] = {
#define X(sym, sc_nr, sc_name, sc_argc) [sym] = SYSCALL_HANDLER_FOR(sym),
    FUROS_SYSCALL_LIST(X)
#undef X
};

static long sys_unknown_impl(trapframe_t *tf, uint64_t a0, uint64_t a1, uint64_t a2,
                             uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)tf;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;
    return -1;
}

static const syscall_info_t *syscall_info_lookup(uint64_t nr) {
    if (nr >= SYS_MAX) {
        return 0;
    }
    const syscall_info_t *info = &syscall_info_table[nr];
    if (info->nr != nr) {
        return 0;
    }
    return info;
}

long syscall_dispatch(trapframe_t *tf, bool *force_resched) {
    uint64_t nr = tf->x[8];
    uint64_t a0 = tf->x[0];
    uint64_t a1 = tf->x[1];
    uint64_t a2 = tf->x[2];
    uint64_t a3 = tf->x[3];
    uint64_t a4 = tf->x[4];
    uint64_t a5 = tf->x[5];

    const syscall_info_t *info = syscall_info_lookup(nr);
    sys_fn_t handler = sys_unknown_impl;
    if (info && syscall_handler_table[nr]) {
        handler = syscall_handler_table[nr];
    }
    long ret = handler(tf, a0, a1, a2, a3, a4, a5);

    task_t *cur = task_current();

    if (nr == SYS_EXIT) {
        *force_resched = true;
    } else if (nr == SYS_WAIT && ret == -2) {
        *force_resched = true;
    } else if ((nr == SYS_READ || nr == SYS_WRITE) && ret == -2) {
        *force_resched = true;
    } else if (nr == SYS_SLEEP && ret == -2) {
        *force_resched = true;
    } else if (nr == SYS_POLL && ret == -2) {
        *force_resched = true;
    } else if (nr == SYS_KILL) {
        *force_resched = true;
    } else if (nr == SYS_YIELD) {
        *force_resched = true;
    } else {
        *force_resched = false;
    }

    if (cur && cur->state == TASK_ZOMBIE) {
        *force_resched = true;
    }

    if (!(nr == SYS_EXEC && ret == 0)) {
        tf->x[0] = (uint64_t)ret;
    }
    return ret;
}
