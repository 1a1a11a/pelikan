#include <slimcache/bb_core.h>
#include <slimcache/bb_setting.h>
#include <slimcache/bb_stats.h>

#include <cc_metric.h>

#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sysexits.h>

static struct setting setting = {
    SETTING(OPTION_INIT)
};

#define PRINT_DEFAULT(_name, _type, _default, _description) \
    log_stdout("  %-31s ( default: %s )", #_name,  _default);

static const unsigned int nopt = OPTION_CARDINALITY(struct setting);

struct stats Stats = {
   STATS(METRIC_INIT)
};

const unsigned int Nmetric = METRIC_CARDINALITY(struct stats);


static void
show_usage(void)
{
    log_stdout(
            "Usage:" CRLF
            "  broadbill_slimcache [option|config]" CRLF
            );
    log_stdout(
            "Description:" CRLF
            "  broadbill_slimcache is one of the unified cache backends. " CRLF
            "  It uses cuckoo hashing to efficiently store small key/val " CRLF
            "  pairs. It speaks the memcached protocol and supports all " CRLF
            "  ASCII memcached commands (except for prepend/append). " CRLF
            CRLF
            "  The storage in slimcache is preallocated as a hash table " CRLF
            "  The maximum key/val size allowed has to be specified when " CRLF
            "  starting the service, and cannot be updated after launch." CRLF
            );
    log_stdout(
            "Options:" CRLF
            "  -h, --help        show this message" CRLF
            "  -v, --version     show version number" CRLF
            );
    log_stdout(
            "Example:" CRLF
            "  ./broadbill_slimcache ../template/slimcache.config" CRLF
            );
    log_stdout("Setting & Default Values:");
    SETTING(PRINT_DEFAULT)
}

static void
show_version(void)
{
    log_stdout("Version: %s", BB_VERSION_STRING);
}

static int
getaddr(struct addrinfo **ai, char *hostname, char *servname)
{
    struct addrinfo hints = { .ai_flags = AI_PASSIVE, .ai_family = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM };
    int ret;

    ret = getaddrinfo(hostname, servname, &hints, ai);
    if (ret < 0) {
        log_error("cannot resolve address");
    }

    return ret;
}

static void
run(void)
{
    rstatus_t status;

    for (;;) {
        status = core_evwait();
        if (status != CC_OK) {
            log_crit("core event loop exits due to failure");
            break;
        }
    }

    core_teardown();
}

static rstatus_t
setup(void)
{
    struct addrinfo *ai;
    int ret;
    rstatus_t status;

    /* setup log first, so we log properly */
    ret = log_setup((int)setting.log_level.val.vuint,
            setting.log_name.val.vstr);
    if (ret < 0) {
        log_error("log setup failed");

        goto error;
    }
    /* stats in case other initialization updates certain metrics */
    metric_reset((struct metric *)&Stats, Nmetric);

    mbuf_setup((uint32_t)setting.mbuf_size.val.vuint);

    array_setup((uint32_t)setting.array_nelem_delta.val.vuint);

    status = cuckoo_setup((size_t)setting.cuckoo_item_size.val.vuint,
            (uint32_t)setting.cuckoo_nitem.val.vuint);
    if (status != CC_OK) {
        log_error("cuckoo module setup failed");

        goto error;
    }

    mbuf_pool_create((uint32_t)setting.mbuf_poolsize.val.vuint);
    conn_pool_create((uint32_t)setting.tcp_poolsize.val.vuint);
    buf_sock_pool_create((uint32_t)setting.buf_sock_poolsize.val.vuint);
    request_pool_create((uint32_t)setting.request_poolsize.val.vuint);

    /* set up core after static resources are ready */
    ret = getaddr(&ai, setting.server_host.val.vstr,
            setting.server_port.val.vstr);
    if (ret < 0) {
        log_error("address invalid");

        goto error;
    }
    status = core_setup(ai);
    freeaddrinfo(ai); /* freeing it before return/error to avoid memory leak */
    if (status != CC_OK) {
        log_crit("cannot start core event loop");

        goto error;
    }

    return CC_OK;

error:
    core_teardown();

    request_pool_destroy();
    buf_sock_pool_destroy();
    conn_pool_destroy();
    mbuf_pool_destroy();

    cuckoo_teardown();
    array_teardown();
    mbuf_teardown();
    log_teardown();

    return CC_ERROR;
}

int
main(int argc, char **argv)
{
    rstatus_t status;
    FILE *fp = NULL;

    if (argc > 2) {
        show_usage();
        exit(EX_USAGE);
    }

    if (argc == 1) {
        log_stderr("launching server with default values.");
    }

    if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            show_usage();

            exit(EX_OK);
        }
        if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
            show_version();

            exit(EX_OK);
        }
        fp = fopen(argv[1], "r");
        if (fp == NULL) {
            log_stderr("cannot open config: incorrect path or doesn't exist");

            exit(EX_DATAERR);
        }
    }

    status = option_load_default((struct option *)&setting, nopt);
    if (status != CC_OK) {
        log_stderr("fail to load default option values");

        exit(EX_CONFIG);
    }
    if (fp != NULL) {
        log_stderr("load config from %s", argv[1]);
        status = option_load_file(fp, (struct option *)&setting, nopt);
        fclose(fp);
    }
    if (status != CC_OK) {
        log_stderr("fail to load config");

        exit(EX_DATAERR);
    }
    option_printall((struct option *)&setting, nopt);

    status = setup();
    if (status != CC_OK) {
        log_crit("setup failed");

        exit(EX_CONFIG);
    }

    run();

    exit(EX_OK);
}
