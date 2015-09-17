/*
 * ccommon cache common library.
 * Copyright (C) 2013 Twitter, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cc_log.h>

#include <cc_mm.h>
#include <cc_print.h>
#include <cc_rbuf.h>
#include <cc_util.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define LOG_MODULE_NAME "ccommon::log"

static log_metrics_st *log_metrics = NULL;
static bool log_init = false;

void
log_setup(log_metrics_st *metrics)
{
    log_stderr("set up the %s module", LOG_MODULE_NAME);

    log_metrics = metrics;
    if (metrics != NULL) {
        LOG_METRIC_INIT(log_metrics);
    }

    if (log_init) {
        log_stderr("%s has already been setup, overwrite", LOG_MODULE_NAME);
    }
    log_init = true;
}

void
log_teardown(void)
{
    log_stderr("tear down the %s module", LOG_MODULE_NAME);

    if (!log_init) {
        log_stderr("%s has never been setup", LOG_MODULE_NAME);
    }

    log_metrics = NULL;
    log_init = false;
}

struct logger *
log_create(int level, char *filename, uint32_t buf_cap)
{
    struct logger *logger;

    log_stderr("create logger with level %d filename %p cap %u", level, filename, buf_cap);

    logger = cc_alloc(sizeof(struct logger));

    if (logger == NULL) {
        log_stderr("Could not create logger due to OOM");
        INCR(log_metrics, log_create_ex);
        return NULL;
    }

    logger->name = filename;
    logger->level = level;

    if (buf_cap > 0) {
        logger->buf = rbuf_create(buf_cap);

        if (logger->buf == NULL) {
            cc_free(logger);
            log_stderr("Could not create logger - buffer not allocated due to OOM");
            INCR(log_metrics, log_create_ex);
            return NULL;
        }
    } else {
        logger->buf = NULL;
    }

    if (filename != NULL) {
        logger->fd = open(filename, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (logger->fd < 0) {
            cc_free(logger);
            log_stderr("Could not create logger - cannot open file");
            INCR(log_metrics, log_open_ex);
            INCR(log_metrics, log_create_ex);
            return NULL;
        } else {
            INCR(log_metrics, log_open);
        }
    }

    INCR(log_metrics, log_create);
    INCR(log_metrics, log_curr);

    return logger;
}

void
log_destroy(struct logger **l)
{
    struct logger *logger = *l;

    if (logger == NULL) {
        return;
    }

    /* flush first in case there's data left in the buffer */
    log_flush(logger);

    if (logger->fd >= 0 && logger->fd != STDERR_FILENO
        && logger->fd != STDOUT_FILENO) {
        close(logger->fd);
    }

    if (logger->buf != NULL) {
        rbuf_destroy(logger->buf);
    }

    cc_free(logger);
    *l = NULL;

    INCR(log_metrics, log_destroy);
    DECR(log_metrics, log_curr);
}

rstatus_t
log_reopen(struct logger *logger)
{
    if (logger->fd != STDERR_FILENO && logger->fd != STDOUT_FILENO) {
        close(logger->fd);
        logger->fd = open(logger->name, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if (logger->fd < 0) {
            log_stderr("reopening log file '%s' failed, ignored: %s", logger->name,
                       strerror(errno));
            INCR(log_metrics, log_open_ex);
            return CC_ERROR;
        }
    }

    INCR(log_metrics, log_open);

    return CC_OK;
}

void
log_level_set(struct logger *logger, int level)
{
    logger->level = level;
}

void
_log_write(struct logger *logger, char *buf, int len)
{
    int n;

    if (logger->buf != NULL) {
        n = rbuf_write(logger->buf, buf, len);
    } else {
        if (logger->fd < 0) {
            INCR(log_metrics, log_write_ex);
            return;
        }

        n = write(logger->fd, buf, len);

        if (n < 0) {
            INCR(log_metrics, log_write_ex);
            logger->nerror++;
            return;
        }
    }

    if (n < len) {
        INCR(log_metrics, log_skip);
        INCR_N(log_metrics, log_skip_byte, len - n);
        logger->nerror++;
    } else {
        INCR(log_metrics, log_write);
        INCR_N(log_metrics, log_write_byte, len);
    }
}

void
_log_fd(int fd, const char *fmt, ...)
{
    int len, size, errno_save;
    char buf[LOG_MAX_LEN];
    va_list args;
    ssize_t n;

    errno_save = errno;
    len = 0;            /* length of output */
    size = LOG_MAX_LEN; /* size of output buffer */

    va_start(args, fmt);
    len += cc_vscnprintf(buf, size, fmt, args);
    va_end(args);

    buf[len++] = '\n';

    n = write(fd, buf, len);

    if (n < 0) {
        INCR(log_metrics, log_write_ex);
    }

    if (n < len) {
        INCR(log_metrics, log_skip);
        INCR_N(log_metrics, log_skip_byte, len - n);
    } else {
        INCR(log_metrics, log_write);
        INCR_N(log_metrics, log_write_byte, len);
    }

    errno = errno_save;
}

/*
 * Hexadecimal dump in the canonical hex + ascii display
 * See -C option in man hexdump
 */
void
_log_hexdump(struct logger *logger, int level, char *data, int datalen)
{
    char buf[8 * LOG_MAX_LEN];
    int i, off, len, size, errno_save;

    if (!log_loggable(logger, level)) {
        return;
    }

    /* log hexdump */
    errno_save = errno;
    off = 0;                  /* data offset */
    len = 0;                  /* length of output buffer */
    size = 8 * LOG_MAX_LEN;   /* size of output buffer */

    while (datalen != 0 && (len < size - 1)) {
        char *save;
        unsigned char c;
        int savelen;

        len += cc_scnprintf(buf + len, size - len, "%08x  ", off);

        save = data;
        savelen = datalen;

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++) {
            c = (unsigned char)(*data);
            len += cc_scnprintf(buf + len, size - len, "%02x%s", c,
                    (i == 7) ? "  " : " ");
        }
        for ( ; i < 16; i++) {
            len += cc_scnprintf(buf + len, size - len, "  %s",
                    (i == 7) ? "  " : " ");
        }

        data = save;
        datalen = savelen;

        len += cc_scnprintf(buf + len, size - len, "  |");

        for (i = 0; datalen != 0 && i < 16; data++, datalen--, i++) {
            c = (unsigned char)(isprint(*data) ? *data : '.');
            len += cc_scnprintf(buf + len, size - len, "%c", c);
        }
        len += cc_scnprintf(buf + len, size - len, "|\n");

        off += 16;
    }

    _log_write(logger, buf, len);

    errno = errno_save;
}

void
log_flush(struct logger *logger)
{
    ssize_t n;
    size_t buf_len;

    if (logger->buf == NULL) {
        return;
    }

    if (logger->fd < 0) {
        log_stderr("Cannot flush logger %p; invalid file descriptor", logger);
        INCR(log_metrics, log_flush_ex);
        return;
    }

    buf_len = rbuf_rcap(logger->buf);
    n = rbuf_read_fd(logger->buf, logger->fd);

    if (n < buf_len) {
        INCR(log_metrics, log_flush_ex);
    } else {
        INCR(log_metrics, log_flush);
    }
}
