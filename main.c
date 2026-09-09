/*
 * logd — системный логгер CactOS (упрощённый аналог syslogd/klogd).
 *
 * Читает кольцевой буфер ядра /dev/kmsg (offset-based: read() сам двигает
 * файловый курсор) и дописывает строки в текстовый лог-файл. Буфер ядра
 * конечен, поэтому logd опрашивает /dev/kmsg часто, чтобы ничего не терять.
 *
 * Запускается супервизором cgoct как /sbin/logd (см. Cgoct-x86_32).
 *
 * /etc/logd.conf (все ключи необязательны):
 *   file=/var/log/kmsg.log   — куда писать лог ядра
 *   interval=2               — пауза (сек) между опросами, когда новых строк нет
 *   console=1                — дублировать строки на /dev/console
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#define CONFIG_PATH       "/etc/logd.conf"
#define LOG_PATH_DEFAULT  "/var/log/kmsg.log"
#define KMSG_PATH         "/dev/kmsg"
#define MAX_LINE          256
#define READ_CHUNK        256

static char log_path[128] = LOG_PATH_DEFAULT;
static int  interval_sec  = 2;
static int  console_on    = 0;
static int  out_fd        = -1;

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        char *eq = p;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;
        int vlen = (int)strlen(val);
        while (vlen > 0 && (val[vlen - 1] == '\n' || val[vlen - 1] == '\r' ||
                            val[vlen - 1] == ' ' || val[vlen - 1] == '\t'))
            val[--vlen] = '\0';
        if (strcmp(key, "file") == 0) {
            strncpy(log_path, val, sizeof(log_path) - 1);
            log_path[sizeof(log_path) - 1] = '\0';
        } else if (strcmp(key, "interval") == 0) {
            int v = atoi(val);
            if (v >= 1 && v <= 3600) interval_sec = v;
        } else if (strcmp(key, "console") == 0) {
            console_on = (val[0] == '1' || val[0] == 'y' || val[0] == 'Y');
        }
    }
    fclose(f);
}

/* Написать одну готовую строку (с '\n') в лог и, по желанию, на консоль. */
static void emit_line(char *line, int len) {
    if (out_fd < 0) return;
    write(out_fd, line, (size_t)len);
    if (console_on) {
        int cfd = open("/dev/console", O_WRONLY);
        if (cfd >= 0) {
            write(cfd, line, (size_t)len);
            close(cfd);
        }
    }
}

/* Сборка строк из потока байт. */
static char carry[MAX_LINE + 2];
static int  carry_len = 0;

static void handle_chunk(const char *data, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (data[i] == '\n') {
            if (carry_len > 0) {
                carry[carry_len++] = '\n';
                emit_line(carry, carry_len);
                carry_len = 0;
            }
        } else if (carry_len < MAX_LINE) {
            carry[carry_len++] = data[i];
        }
        /* строка длиннее MAX_LINE — байт отбрасывается */
    }
}

/* Накопленный хвост без '\n' (ядро отдаёт и незакрытую строку). */
static void flush_carry(void) {
    if (carry_len > 0) {
        carry[carry_len++] = '\n';
        emit_line(carry, carry_len);
        carry_len = 0;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("logd: starting\n");
    config_load();

    out_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (out_fd < 0) {
        printf("logd: cannot open %s, logging disabled\n", log_path);
    } else {
        printf("logd: appending to %s\n", log_path);
    }

    int kmsg = open(KMSG_PATH, O_RDONLY);
    if (kmsg < 0) {
        printf("logd: cannot open %s, retrying\n", KMSG_PATH);
        /* Ждём появления узла — не выходим, чтобы не дёргать супервизор. */
        while (kmsg < 0) {
            sleep((unsigned int)interval_sec);
            kmsg = open(KMSG_PATH, O_RDONLY);
        }
        printf("logd: %s opened\n", KMSG_PATH);
    }

    char buf[READ_CHUNK];
    for (;;) {
        int n;
        int got = 0;
        while ((n = (int)read(kmsg, buf, sizeof(buf))) > 0) {
            handle_chunk(buf, n);
            got = 1;
        }
        if (got) {
            flush_carry();
        } else {
            sleep((unsigned int)interval_sec);
        }
    }
    return 0;
}
