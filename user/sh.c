#include "user.h"

#define SH_MAX_TOKENS 64
#define SH_MAX_CMDS 8
#define SH_MAX_ARGS 16
#define SH_MAX_WORDS 2048
#define SH_MAX_LINE  512

typedef struct {
    char *argv[SH_MAX_ARGS];
    int argc;
    char *in_path;
    char *out_path;
    int out_append;
} shell_cmd_t;

static int is_space(char c) {
    return c == ' ' || c == '\t';
}

static int is_op_char(char c) {
    return c == '|' || c == '<' || c == '>';
}

static int has_path_sep(const char *cmd) {
    for (const char *p = cmd; *p; p++) {
        if (*p == '/') {
            return 1;
        }
    }
    return 0;
}

static void build_exec_path(const char *cmd, const char *prefix, char *out, int out_len) {
    if (has_path_sep(cmd)) {
        int i = 0;
        while (cmd[i] && i < out_len - 1) {
            out[i] = cmd[i];
            i++;
        }
        out[i] = '\0';
        return;
    }

    int i = 0;
    while (prefix[i] && i < out_len - 1) {
        out[i] = prefix[i];
        i++;
    }
    int j = 0;
    while (cmd[j] && i < out_len - 1) {
        out[i++] = cmd[j++];
    }
    out[i] = '\0';
}

static void reset_cmd(shell_cmd_t *cmd) {
    memset(cmd, 0, sizeof(*cmd));
}

static int lex_line(const char *line, char *words, int words_len, char **tokens, int max_tokens) {
    const char *p = line;
    char *w = words;
    char *w_end = words + words_len;
    int ntok = 0;

    while (*p) {
        while (is_space(*p)) {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        if (ntok >= max_tokens - 1) {
            return -1;
        }

        if (is_op_char(*p)) {
            tokens[ntok++] = w;
            if (*p == '>' && p[1] == '>') {
                if (w + 3 > w_end) {
                    return -1;
                }
                *w++ = '>';
                *w++ = '>';
                *w++ = '\0';
                p += 2;
            } else {
                if (w + 2 > w_end) {
                    return -1;
                }
                *w++ = *p++;
                *w++ = '\0';
            }
            continue;
        }

        tokens[ntok++] = w;
        while (*p && !is_space(*p) && !is_op_char(*p)) {
            if (w + 2 > w_end) {
                return -1;
            }

            if (*p == '\\' && p[1] != '\0') {
                p++;
                *w++ = *p++;
                continue;
            }

            if (*p == '\'') {
                p++;
                while (*p && *p != '\'') {
                    if (w + 2 > w_end) {
                        return -1;
                    }
                    *w++ = *p++;
                }
                if (*p != '\'') {
                    return -1;
                }
                p++;
                continue;
            }

            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] != '\0') {
                        p++;
                    }
                    if (w + 2 > w_end) {
                        return -1;
                    }
                    *w++ = *p++;
                }
                if (*p != '"') {
                    return -1;
                }
                p++;
                continue;
            }

            *w++ = *p++;
        }
        *w++ = '\0';
    }

    tokens[ntok] = 0;
    return ntok;
}

static int parse_line(char *line, char *words, int words_len, shell_cmd_t *cmds, int *cmd_count) {
    char *tokens[SH_MAX_TOKENS];
    int ntok = lex_line(line, words, words_len, tokens, SH_MAX_TOKENS);
    if (ntok < 0) {
        return -1;
    }
    if (ntok == 0) {
        *cmd_count = 0;
        return 0;
    }

    int ci = 0;
    reset_cmd(&cmds[ci]);

    for (int i = 0; i < ntok; i++) {
        char *t = tokens[i];

        if (strcmp(t, "|") == 0) {
            if (cmds[ci].argc == 0 || ci + 1 >= SH_MAX_CMDS) {
                return -1;
            }
            cmds[ci].argv[cmds[ci].argc] = 0;
            ci++;
            reset_cmd(&cmds[ci]);
            continue;
        }

        if (strcmp(t, "<") == 0 || strcmp(t, ">") == 0 || strcmp(t, ">>") == 0) {
            if (i + 1 >= ntok) {
                return -1;
            }
            char *path = tokens[++i];
            if (strcmp(path, "|") == 0 ||
                strcmp(path, "<") == 0 ||
                strcmp(path, ">") == 0 ||
                strcmp(path, ">>") == 0) {
                return -1;
            }
            if (t[0] == '<') {
                cmds[ci].in_path = path;
            } else {
                cmds[ci].out_path = path;
                cmds[ci].out_append = (t[1] == '>');
            }
            continue;
        }

        if (cmds[ci].argc >= SH_MAX_ARGS - 1) {
            return -1;
        }
        cmds[ci].argv[cmds[ci].argc++] = t;
    }

    if (cmds[ci].argc == 0) {
        return -1;
    }
    cmds[ci].argv[cmds[ci].argc] = 0;
    *cmd_count = ci + 1;
    return 0;
}

static int run_builtin(shell_cmd_t *cmd) {
    if (strcmp(cmd->argv[0], "exit") == 0) {
        return 1;
    }
    if (strcmp(cmd->argv[0], "cd") == 0) {
        const char *dst = (cmd->argc > 1) ? cmd->argv[1] : "/";
        if (sys_chdir(dst) < 0) {
            puts("cd: no such directory\n");
        }
        return 2;
    }
    if (strcmp(cmd->argv[0], "init") == 0) {
        puts("init is managed by pid 1\n");
        return 2;
    }
    return 0;
}

static int spawn_cmd(shell_cmd_t *cmd, int in_fd, int out_fd, int close_fd) {
    char exec_path[64];
    build_exec_path(cmd->argv[0], "/bin/", exec_path, sizeof(exec_path));

    int pid = sys_fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (in_fd >= 0 && sys_dup2(in_fd, 0) < 0) {
            puts("dup2 input failed\n");
            sys_exit(1);
        }
        if (out_fd >= 0 && sys_dup2(out_fd, 1) < 0) {
            puts("dup2 output failed\n");
            sys_exit(1);
        }

        if (close_fd >= 0) {
            sys_close(close_fd);
        }
        if (in_fd >= 0) {
            sys_close(in_fd);
        }
        if (out_fd >= 0) {
            sys_close(out_fd);
        }

        if (cmd->in_path) {
            int fd = sys_open(cmd->in_path, O_RDONLY);
            if (fd < 0 || sys_dup2(fd, 0) < 0) {
                puts("redirect input failed\n");
                sys_exit(1);
            }
            sys_close(fd);
        }
        if (cmd->out_path) {
            int flags = O_CREAT | O_WRONLY;
            if (cmd->out_append) {
                flags |= O_APPEND;
            } else {
                flags |= O_TRUNC;
            }
            int fd = sys_open(cmd->out_path, flags);
            if (fd < 0 || sys_dup2(fd, 1) < 0) {
                puts("redirect output failed\n");
                sys_exit(1);
            }
            sys_close(fd);
        }

        const char *exec_argv[SH_MAX_ARGS];
        int exec_argc = cmd->argc;
        for (int i = 0; i < cmd->argc; i++) {
            exec_argv[i] = cmd->argv[i];
        }
        if (cmd->argc == 1 &&
            strcmp(cmd->argv[0], "cat") == 0 &&
            (in_fd >= 0 || cmd->in_path != 0) &&
            exec_argc < SH_MAX_ARGS - 1) {
            exec_argv[exec_argc++] = "-";
        }
        exec_argv[exec_argc] = 0;
        if (sys_exec(exec_path, exec_argv) < 0) {
            if (!has_path_sep(cmd->argv[0])) {
                build_exec_path(cmd->argv[0], "/sbin/", exec_path, sizeof(exec_path));
                if (sys_exec(exec_path, exec_argv) >= 0) {
                    sys_exit(0);
                }
            }
            puts("exec failed: ");
            puts(cmd->argv[0]);
            puts("\n");
            sys_exit(127);
        }
        sys_exit(0);
    }
    return pid;
}

static void wait_children(const int *pids, int count) {
    for (int i = 0; i < count; i++) {
        int status = 0;
        (void)sys_waitpid(pids[i], &status, 0);
        (void)status;
    }
}

static int run_pipeline(shell_cmd_t *cmds, int ncmd) {
    int pids[SH_MAX_CMDS];
    int spawned = 0;
    int prev_read = -1;

    for (int i = 0; i < ncmd; i++) {
        int next_pipe[2] = {-1, -1};
        if (i + 1 < ncmd) {
            if (sys_pipe(next_pipe) < 0) {
                puts("pipe failed\n");
                if (prev_read >= 0) {
                    sys_close(prev_read);
                }
                wait_children(pids, spawned);
                return -1;
            }
        }

        int in_fd = prev_read;
        int out_fd = (i + 1 < ncmd) ? next_pipe[1] : -1;
        int close_fd = (i + 1 < ncmd) ? next_pipe[0] : -1;
        int pid = spawn_cmd(&cmds[i], in_fd, out_fd, close_fd);
        if (pid < 0) {
            puts("fork failed\n");
            if (prev_read >= 0) {
                sys_close(prev_read);
            }
            if (next_pipe[0] >= 0) {
                sys_close(next_pipe[0]);
            }
            if (next_pipe[1] >= 0) {
                sys_close(next_pipe[1]);
            }
            wait_children(pids, spawned);
            return -1;
        }
        pids[spawned++] = pid;

        if (prev_read >= 0) {
            sys_close(prev_read);
        }
        if (next_pipe[1] >= 0) {
            sys_close(next_pipe[1]);
        }
        prev_read = next_pipe[0];
    }

    if (prev_read >= 0) {
        sys_close(prev_read);
    }
    wait_children(pids, spawned);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char line[SH_MAX_LINE];
    char words[SH_MAX_WORDS];
    shell_cmd_t cmds[SH_MAX_CMDS];

    for (;;) {
        puts("sh$ ");
        int n = readline(line, sizeof(line));
        if (n <= 0) {
            sys_yield();
            continue;
        }

        int ncmd = 0;
        if (parse_line(line, words, sizeof(words), cmds, &ncmd) != 0) {
            puts("parse error\n");
            continue;
        }
        if (ncmd == 0) {
            continue;
        }

        if (ncmd == 1) {
            int brc = run_builtin(&cmds[0]);
            if (brc == 1) {
                return 0;
            }
            if (brc != 0) {
                continue;
            }
        }

        (void)run_pipeline(cmds, ncmd);
    }

    return 0;
}
