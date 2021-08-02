/*
 * (C) Radim Kolar 1997-2004
 * (C) winshining https://github.com/winshining 2017
 * This is free software, see GNU Public License version 2 for
 * details.
 *
 * Simple forking WWW Server benchmark:
 *
 * Usage:
 *   webbench --help
 *
 * Return codes:
 *    0 - sucess
 *    1 - benchmark failed (server is not on-line)
 *    2 - bad param
 *    3 - internal error, fork failed
 * 
 */ 
#include "socket.c"
#include "uuid.c"
#include <unistd.h>
#include <sys/param.h>
#include <rpc/types.h>
#include <getopt.h>
#include <strings.h>
#include <time.h>
#include <signal.h>

/* Allow: GET, POST, HEAD, OPTIONS, TRACE */
#define METHOD_GET 0
#define METHOD_HEAD 1
#define METHOD_OPTIONS 2
#define METHOD_TRACE 3
#define METHOD_POST 4
#define PROGRAM_VERSION "1.6"

#define POST_SIZE     1024
#define REQUEST_SIZE  2048
#define MAX_BUF_SIZE  2048
#define BOUNDARY_SIZE 57

#define POST_MIME_URLENCODED                    "application/x-www-form-urlencoded"
#define POST_MIME_MULTIFORM                     "multipart/form-data; boundary="
#define POST_CONTENT_DISPOSITION                "Content-Disposition: form-data; name=\"webbench\"; "
#define POST_CONTENT_DISPOSITION_FILENAME_START "filename=\""
#define POST_CONTENT_DISPOSITION_FILENAME_END   "\""
#define POST_CONTENT_DISPOSITION_CONTENT_TYPE   "Content-Type: application/octet-stream" 

/* values */
volatile int timerexpired = 0;

typedef struct {
    int succeeded;
    int failed;
    long bytes;
} statistics_t;

typedef struct {
    int post;
    int in_file;
    FILE *file;
    long offset;
    char *boundary;
    char *content;
} post_t;

typedef struct {
    int proxyport;
    char *proxyhost;
} proxy_t;

typedef struct {
    int count;
    char **key;
    char **value;
} header_t;

/* globals */
typedef struct {
    int http_version; /* 0 - http/0.9, 1 - http/1.0, 2 - http/1.1 */
    int method;
    int clients;
    int force;
    int force_reload;
    int benchtime;

    proxy_t proxy;
    post_t post;
    header_t header;
} bench_params_t;

statistics_t statistics = {
    0, 0, 0
};

bench_params_t bench_params = {
    1,
    METHOD_GET,
    1,
    0,
    0,
    30,
    { 80, NULL },
    { 0, 0, NULL, 0, NULL, NULL },
    { 0, NULL, NULL }
};

/* internal */
int mypipe[2];
char host[MAXHOSTNAMELEN];
char request[REQUEST_SIZE];

static const struct option long_options[] =
{
    {"force",    no_argument,        &bench_params.force,          1},
    {"reload",   no_argument,        &bench_params.force_reload,   1},
    {"time",     required_argument,  NULL,                        't'},
    {"help",     no_argument,        NULL,                        '?'},
    {"http09",   no_argument,        NULL,                        '9'},
    {"http10",   no_argument,        NULL,                        '1'},
    {"http11",   no_argument,        NULL,                        '2'},
    {"get",      no_argument,        &bench_params.method,        METHOD_GET},
    {"head",     no_argument,        &bench_params.method,        METHOD_HEAD},
    {"options",  no_argument,        &bench_params.method,        METHOD_OPTIONS},
    {"trace",    no_argument,        &bench_params.method,        METHOD_TRACE},
    {"post",     required_argument,  NULL,                        'o'},
    {"file",     no_argument,        NULL,                        'i'},
    {"header",   required_argument,  NULL,                        'd'},
    {"version",  no_argument,        NULL,                        'V'},
    {"proxy",    required_argument,  NULL,                        'p'},
    {"clients",  required_argument,  NULL,                        'c'},
    {NULL,       0,                  NULL,                         0}
};

/* prototypes */
static void benchcore(const char* host, const int port, char *request);
static int bench(void);
static void build_request(const char *url);

static void alarm_handler(int signal)
{
    switch (signal) {
    case SIGALRM:
    default:
        timerexpired = 1;
    }
}

static void usage(void)
{
   fprintf(stderr,
    "webbench [option]... URL\n"
    "  -f|--force               Don't wait for reply from server.\n"
    "  -r|--reload              Send reload request - Pragma: no-cache.\n"
    "  -t|--time <sec>          Run benchmark for <sec> seconds. Default 30.\n"
    "  -p|--proxy <server:port> Use proxy server for request.\n"
    "  -c|--clients <n>         Run <n> HTTP clients at once. Default one.\n"
    "  -9|--http09              Use HTTP/0.9 style requests.\n"
    "  -1|--http10              Use HTTP/1.0 protocol.\n"
    "  -2|--http11              Use HTTP/1.1 protocol.\n"
    "  --get                    Use GET request method.\n"
    "  --head                   Use HEAD request method.\n"
    "  --options                Use OPTIONS request method.\n"
    "  --trace                  Use TRACE request method.\n"
    "  -o|--post                Use POST request method.\n"
    "  -i|--file                Use multipart/form-data for POST request method.\n"
    "  -d|--header <header:xxx> Specify custom header.\n"
    "  -?|-h|--help             This information.\n"
    "  -V|--version             Display program version.\n"
    );
};

static int init_header(int count)
{
    int new_add = 0;
    char **new_ptr = NULL;
    header_t *header = &bench_params.header;

    if (count <= 0)
        count = 1;

    if (header->count) {
        if (count <= header->count) {
            fprintf(stderr, "Warning in init_header: count not larger than bench_params.header.count.");
            return header->count;
        }

        new_add = 1;
    }

    header->count = count;

    if (!new_add)
        header->key = (char **)malloc(header->count * sizeof(char *));
    else {
        new_ptr = (char **)realloc(header->key, header->count * sizeof(char *));
        if (new_ptr == NULL) {
            if (header->key) {
                free(header->key);
            }

            header->key = NULL;
        } else {
            header->key = new_ptr;
        }
    }

    if (header->key == NULL)
        return 0;

    if (!new_add)
        header->value = (char **)malloc(header->count * sizeof(char *));
    else {
        new_ptr = (char **)realloc(header->value, header->count * sizeof(char *));
        if (new_ptr == NULL) {
            if (header->value) {
                free(header->value);
            }

            header->value = NULL;
        } else {
            header->value = new_ptr;
        }
    }

    if (header->value == NULL) {
        if (!new_add)
            free(header->key);

        return 0;
    }

    return 1;
}

static void free_header(void)
{
    if (bench_params.header.key)
        free(bench_params.header.key);

    if (bench_params.header.value)
        free(bench_params.header.value);
}

static void free_boundary(void)
{
    if (bench_params.post.boundary)
        free(bench_params.post.boundary);
}

int main(int argc, char *argv[])
{
    int opt = 0;
    int i, header_count = 0;
    int options_index = 0;
    char uuid[UUID_SIZE + 1];
    char *tmp = NULL;

    if(argc == 1) {
        usage();
        goto failed;
    }

    while((opt = getopt_long(argc, argv, "912Vfrt:p:c:d:o:i?h", long_options, &options_index)) != EOF) {
        switch(opt) {
        case 0:
            break;
        case 'f':
            bench_params.force = 1;
            break;
        case 'r':
            bench_params.force_reload = 1;
            break;
        case '9':
            bench_params.http_version = 0;
            break;
        case '1':
            bench_params.http_version = 1;
            break;
        case '2':
            bench_params.http_version = 2;
            break;
        case 'V':
            printf(PROGRAM_VERSION "\n");
            exit(0);
        case 't':
            bench_params.benchtime = atoi(optarg);
            if (bench_params.benchtime <= 0)
                fprintf(stderr, "Warning in option --time %s: Invalid value, defaults to 30.\n", optarg);

            break;
        case 'p':
            /* proxy server parsing server:port */
            tmp = strrchr(optarg, ':');
            bench_params.proxy.proxyhost = optarg;
            if(tmp == NULL)
                break;

            if(tmp == optarg) {
                fprintf(stderr, "Error in option --proxy %s: Missing hostname.\n", optarg);
                goto failed;
            }

            if(tmp == optarg + strlen(optarg) - 1) {
                fprintf(stderr, "Error in option --proxy %s: Port number is missing.\n", optarg);
                goto failed;
            }

            *tmp = '\0';
            bench_params.proxy.proxyport = atoi(tmp + 1);
            if (bench_params.proxy.proxyport <= 0)
                fprintf(stderr, "Warning in option --proxy %s: Invalid proxy port, defaults to 80.\n", optarg);

            break;
        case ':':
        case 'h':
        case '?':
            usage();
            goto failed;
        case 'c':
            bench_params.clients = atoi(optarg);
            if (bench_params.clients <= 0)
               fprintf(stderr, "Warning in option --clients %s: Invalid clients, defaults to 1.\n", optarg);

            break;
        case 'd':
            tmp = strrchr(optarg, ':');
            if (tmp == NULL) {
                fprintf(stderr, "Error in option --header %s: Bad format.\n", optarg);
                goto failed;
            }

            if (tmp == optarg) {
                fprintf(stderr, "Error in option --header %s: Missing custom header.\n", optarg);
                goto failed;
            }

            if (tmp == optarg + strlen(optarg) - 1) {
                fprintf(stderr, "Error in option --header %s: Header value is missing.\n", optarg);
                goto failed;
            }

            header_count++;
            if (!init_header(header_count)) {
                free_header();
                fprintf(stderr, "Error in option --header %s: Alloc for header failed.\n", optarg);
                goto failed;
            }

            bench_params.header.key[header_count - 1] = optarg;
            *tmp = '\0';
            while (*(++tmp) == ' ') { /* void */ }
            bench_params.header.value[header_count - 1] = tmp;
            break;
        case 'o':
            if (strlen(optarg) > POST_SIZE) {
                fprintf(stderr, "Error in option --post %s: Content or file name too large.\n", optarg);
                goto failed;
            }

            bench_params.method = METHOD_POST;
            bench_params.post.post = 1;
            bench_params.post.content = optarg;
            break;
        case 'i':
            bench_params.post.in_file = 1;
            break;
        default:
            break;
        }
    }

    if(optind == argc) {
        fprintf(stderr, "webbench: Missing URL!\n");
        usage();
        goto failed;
    }

    if (bench_params.post.in_file) {
        if (!bench_params.post.post) {
            fprintf(stderr, "Error in option -i|--file: --post not specified.\n");
            goto failed;
        }

        bench_params.post.boundary = (char *)malloc(BOUNDARY_SIZE + 1);
        if (bench_params.post.boundary == NULL) {
            fprintf(stderr, "Error in alloc for boundary.\n");
            goto failed;
        }

        strcat(bench_params.post.boundary, "-------------------------");
        random_uuid(uuid);
        snprintf(bench_params.post.boundary + strlen(bench_params.post.boundary), 9, "%s", uuid);
        snprintf(bench_params.post.boundary + strlen(bench_params.post.boundary), 5, "%s", uuid + 9);
        snprintf(bench_params.post.boundary + strlen(bench_params.post.boundary), 5, "%s", uuid + 14);
        snprintf(bench_params.post.boundary + strlen(bench_params.post.boundary), 5, "%s", uuid + 19);
        snprintf(bench_params.post.boundary + strlen(bench_params.post.boundary), 13, "%s", uuid + 24);
    } else {
        if (bench_params.post.post) {
            for (i = 0; i < bench_params.header.count; i++) {
                if (strcasecmp(bench_params.header.value[i], POST_MIME_URLENCODED) == 0)
                    break;
            }

            if (i == bench_params.header.count) {
                header_count++;

                if (!init_header(header_count)) {
                    fprintf(stderr, "Error in option --header %s: Alloc for header failed.\n", POST_MIME_URLENCODED);
                    goto failed;
                }

                bench_params.header.key[header_count - 1] = "Content-Type";
                bench_params.header.value[header_count - 1] = (char *)POST_MIME_URLENCODED;
            }
        }
    }

    /* Copyright */
    fprintf(stderr,
        "Webbench - Simple Web Benchmark "PROGRAM_VERSION"\n"
        "Copyright (c) Radim Kolar 1997-2004, https://github.com/winshining 2017, GPL Open Source Software.\n"
    );

    /* print bench info */
    printf("\nBenchmarking: ");

    switch(bench_params.method) {
    case METHOD_GET:
    default:
        printf("GET");
        break;
    case METHOD_OPTIONS:
        printf("OPTIONS");
        break;
    case METHOD_HEAD:
        printf("HEAD");
        break;
    case METHOD_TRACE:
        printf("TRACE");
        break;
    case METHOD_POST:
        printf("POST");
    }

    build_request(argv[optind]);
    printf(" %s", argv[optind]);

    if (bench_params.post.post) {
        if (!bench_params.post.in_file)
            printf(" Content-Type: %s", POST_MIME_URLENCODED);
        else
            printf(" Content-Type: %s%s", POST_MIME_MULTIFORM, bench_params.post.boundary);
    }

    switch(bench_params.http_version) {
    case 0:
        printf(" (using HTTP/0.9)");
        break;
    case 2:
        printf(" (using HTTP/1.1)");
        break;
    }

    if (bench_params.http_version == 0 && bench_params.post.post) {
        fprintf(stderr, "Error in HTTP support: HTTP/0.9 does not support POST method.\n");
        goto failed;
    }

    printf("\n");
    if (bench_params.clients == 1)
        printf("1 client");
    else
        printf("%d clients", bench_params.clients);

    printf(", running %d sec", bench_params.benchtime);

    if (bench_params.force)
        printf(", early socket close");

    if (bench_params.proxy.proxyhost != NULL)
        printf(", via proxy server %s:%d", bench_params.proxy.proxyhost, bench_params.proxy.proxyport);

    if (bench_params.header.key != NULL) {
        for (i = 0; i < bench_params.header.count; i++)
            printf(", custom header: \"%s: %s\"", bench_params.header.key[i], bench_params.header.value[i]);
    }

    if (bench_params.force_reload)
        printf(", forcing reload");

    printf(".\n");

    return bench();

failed:
    free_header();
    free_boundary();

    return 2;
}

static int build_special_request(void)
{
    int i;
    long cl;
    char str[64];
    header_t *header = &bench_params.header;

    if (header->key) {
        for (i = 0; i < header->count; i++)
            sprintf(request + strlen(request), "%s: %s\r\n", header->key[i], header->value[i]);
    }

    if (bench_params.post.post) {
        if (!bench_params.post.in_file)
            sprintf(request + strlen(request), "Content-Length: %ld\r\n", strlen(bench_params.post.content));
        else {
            sprintf(request + strlen(request), "Content-Type: %s%s\r\n", POST_MIME_MULTIFORM, bench_params.post.boundary);

            fseek(bench_params.post.file, 0L, SEEK_END);

            cl = 2 + BOUNDARY_SIZE + strlen("\r\n") /* first boundary */
                + strlen(POST_CONTENT_DISPOSITION)
                + strlen(POST_CONTENT_DISPOSITION_FILENAME_START)
                + strlen(bench_params.post.content)
                + strlen(POST_CONTENT_DISPOSITION_FILENAME_END)
                + strlen("\r\n") /* Content-Disposition */
                + strlen(POST_CONTENT_DISPOSITION_CONTENT_TYPE)
                + strlen("\r\n\r\n") /* Content-Type in Content-Disposition */
                + ftell(bench_params.post.file) /* file length */
                + strlen("\r\n")
                + 2 + BOUNDARY_SIZE
                + strlen("--\r\n"); /* last boundary */

            bzero(str, 64);
            sprintf(str, "%ld", cl);

            if (strlen(request) + strlen("Content-Length: \r\n") + strlen(str) >= REQUEST_SIZE) {
                fprintf(stderr, "Error in request size: %ld, overflowed.\n",
                    strlen(request) + strlen("Content-Lenght: \r\n") + strlen(str));

                free_header();
                free_boundary();
                return 0;
            }

            sprintf(request + strlen(request), "Content-Length: %ld\r\n", cl);
            fseek(bench_params.post.file, 0L, SEEK_SET);
        }

        strcat(request, "\r\n");
        if (!bench_params.post.in_file)
            memcpy(request + strlen(request), bench_params.post.content, strlen(bench_params.post.content));
        else {
            strcat(request, "--");
            strcat(request, bench_params.post.boundary);
            strcat(request, "\r\n");
            strcat(request, POST_CONTENT_DISPOSITION);
            strcat(request, POST_CONTENT_DISPOSITION_FILENAME_START);
            strcat(request, bench_params.post.content);
            strcat(request, POST_CONTENT_DISPOSITION_FILENAME_END);
            strcat(request, "\r\n");
            strcat(request, POST_CONTENT_DISPOSITION_CONTENT_TYPE);
            strcat(request, "\r\n\r\n");
            /* content\r\n--boundary--\r\n */
        }
    }

    free_header();
    return 1;
}

void build_request(const char *url)
{
    char tmp[10];
    int i;

    bzero(host, MAXHOSTNAMELEN);
    bzero(request, REQUEST_SIZE);
 
    if (bench_params.force_reload && bench_params.proxy.proxyhost != NULL && bench_params.http_version < 1)
        bench_params.http_version = 1;

    if (bench_params.method == METHOD_HEAD && bench_params.http_version < 1)
        bench_params.http_version = 1;

    if (bench_params.method == METHOD_OPTIONS && bench_params.http_version < 2)
        bench_params.http_version = 2;

    if (bench_params.method == METHOD_TRACE && bench_params.http_version < 2)
        bench_params.http_version = 2;

    if (bench_params.method == METHOD_POST && bench_params.http_version < 2) {
        /* rfc1867 was published in 1995, http 1.0 was published in 1982. */
        if (bench_params.post.in_file)
            bench_params.http_version = 2;
        else
            bench_params.http_version = 1;
    }

    switch (bench_params.method) {
    default:
    case METHOD_GET:
        strcpy(request, "GET");
        break;
    case METHOD_HEAD:
        strcpy(request, "HEAD");
        break;
    case METHOD_OPTIONS:
        strcpy(request, "OPTIONS");
        break;
    case METHOD_TRACE:
        strcpy(request, "TRACE");
        break;
    case METHOD_POST:
        strcpy(request, "POST");
        break;
    }

    strcat(request, " ");

    if (NULL == strstr(url, "://")) {
        fprintf(stderr, "\n%s: is not a valid URL.\n", url);
        exit(2);
    }

    if (strlen(url) > MAX_BUF_SIZE) {
        fprintf(stderr, "URL is too long.\n");
        exit(2);
    }

    if (bench_params.proxy.proxyhost == NULL) {
        if (0 != strncasecmp("http://", url, 7)) {
            fprintf(stderr, "\nOnly HTTP protocol is directly supported, set --proxy for others.\n");
            exit(2);
        }
    }

    /* protocol/host delimiter */
    i = strstr(url, "://") - url + 3;
    /* printf("%d\n", i); */

    if (strchr(url + i, '/') == NULL) {
        fprintf(stderr, "\nInvalid URL syntax - hostname don't ends with '/'.\n");
        exit(2);
    }

    if (bench_params.proxy.proxyhost == NULL) {
        /* get port from hostname */
        if (index(url + i, ':') != NULL
            && index(url + i, ':') < index(url + i, '/')
        )
        {
            strncpy(host, url + i, strchr(url + i, ':') - url - i);
            bzero(tmp, 10);
            strncpy(tmp, index(url + i, ':') + 1, strchr(url + i, '/') - index(url + i, ':') - 1);
            /* printf("tmp = %s\n", tmp); */
            bench_params.proxy.proxyport = atoi(tmp);
            if (bench_params.proxy.proxyport == 0)
                bench_params.proxy.proxyport = 80;
        } else
            strncpy(host, url + i, strcspn(url + i, "/"));

        // printf("Host = %s\n", host);
        strcat(request + strlen(request), url + i + strcspn(url + i, "/"));
    } else {
        // printf("ProxyHost = %s\nProxyPort = %d\n",
        // bench_params.proxy.proxyhost, bench_params.proxy.proxyport);
        strcat(request, url);
    }

    if (bench_params.http_version == 1)
        strcat(request, " HTTP/1.0");
    else if (bench_params.http_version == 2)
        strcat(request, " HTTP/1.1");

    strcat(request, "\r\n");
    if (bench_params.http_version > 0)
        strcat(request, "User-Agent: WebBench "PROGRAM_VERSION"\r\n");

    if (bench_params.proxy.proxyhost == NULL && bench_params.http_version > 0) {
        strcat(request, "Host: ");
        strcat(request, host);
        strcat(request, "\r\n");
    }

    if (bench_params.force_reload && bench_params.proxy.proxyhost != NULL)
        strcat(request, "Pragma: no-cache\r\n");

    if (bench_params.http_version > 1)
        strcat(request, "Connection: close\r\n");

    /* add empty line at end */
    if (bench_params.http_version > 0 && !bench_params.post.post)
        strcat(request, "\r\n");
    // printf("Req = %s\n", request);
}

/* vraci system rc error kod */
static int bench(void)
{
    int i, j, k;
    pid_t pid = 0;
    FILE *f;

    /* check avaibility of target server */
    i = Socket(
        bench_params.proxy.proxyhost == NULL ? host : bench_params.proxy.proxyhost,
        bench_params.proxy.proxyport
    );

    if (i < 0) {
        fprintf(stderr, "\nConnect to server failed. Aborting benchmark.\n");
        return 1;
    }

    close(i);

    /* create pipe */
    if (pipe(mypipe)) {
        perror("pipe failed.");
        return 3;
    }

    /* not needed, since we have alarm() in childrens */
    /* wait 4 next system clock tick */
    /*
    cas = time(NULL);
    while (time(NULL) == cas)
        sched_yield();
    */

    /* fork childs */
    for (i = 0; i < bench_params.clients; i++) {
        pid = fork();

        if (pid <= (pid_t) 0) {
            /* child process or error*/
            sleep(1); /* make childs faster */
            break;
        }
    }

    if (pid < (pid_t) 0) {
        fprintf(stderr, "problems forking worker no. %d\n", i);
        perror("fork failed.");
        return 3;
    }

    if (pid == (pid_t) 0) {
        /* I am a child */
        do {
            if (bench_params.post.post && bench_params.post.in_file) {
                bench_params.post.file = fopen(bench_params.post.content, "r");
                if (bench_params.post.file == NULL) {
                    fprintf(stderr, "Error in file open: %s.\n", bench_params.post.content);
                    break;
                }
            }

            if (build_special_request()) {
                if (bench_params.proxy.proxyhost == NULL)
                    benchcore(host, bench_params.proxy.proxyport, request);
                else
                    benchcore(bench_params.proxy.proxyhost, bench_params.proxy.proxyport, request);
            }
        } while (0);

        /* write results to pipe */
        f = fdopen(mypipe[1], "w");
        if (f == NULL) {
            perror("open pipe for writing failed.");
            return 3;
        }

        /* fprintf(stderr, "Child - %d %d\n", succeeded, failed); */
        fprintf(f, "%d %d %ld\n", statistics.succeeded, statistics.failed, statistics.bytes);
        fclose(f);
        return 0;
    } else {
        if (i == bench_params.clients) {
            free_header();
            free_boundary();
        }

        /* parent */
        f = fdopen(mypipe[0], "r");
        if (f == NULL) {
            perror("open pipe for reading failed.");
            return 3;
        }

        setvbuf(f, NULL, _IONBF, 0);
        statistics.succeeded = 0;
        statistics.failed = 0;
        statistics.bytes = 0;

        for ( ;; ) {
            pid = fscanf(f, "%d %d %d", &i, &j, &k);
            if (pid < 2) {
                fprintf(stderr, "Some of our childrens died.\n");
                break;
            }

            statistics.succeeded += i;
            statistics.failed += j;
            statistics.bytes += k;
            /* fprintf(stderr, "*Knock* %d %d read = %d\n", succeeded, failed, pid); */
            if (--bench_params.clients == 0)
                break;
        }

        fclose(f);

        printf("\nsucceeded = %d pages/min, %ld bytes/sec.\nRequests: %d successful, %d failed.\n",
            (int) ((statistics.succeeded + statistics.failed) / (bench_params.benchtime / 60.0f)),
            (long) (statistics.bytes / (float) bench_params.benchtime),
            statistics.succeeded,
            statistics.failed);
    }

    return i;
}

static void close_post_file(void)
{
    if (bench_params.post.file) {
        fclose(bench_params.post.file);
        bench_params.post.file = NULL;
    }
}

void benchcore(const char *host, const int port, char *req)
{
    int rlen;
    char buf[MAX_BUF_SIZE];
    char multipart_initial[REQUEST_SIZE];
    int s = 0, i;
    struct sigaction sa;
    size_t r;
    int multipart_first = 0, eof = 0, reread = 0;

    /* setup alarm signal handler */
    sa.sa_handler = alarm_handler;
    sa.sa_flags = 0;

    if (sigaction(SIGALRM, &sa, NULL))
        exit(3);

    alarm(bench_params.benchtime);

    rlen = strlen(req);

    if (bench_params.post.in_file) {
        /* for retry */
        bzero(multipart_initial, REQUEST_SIZE);
        memcpy(multipart_initial, req, rlen);
    }

nexttry:
    for ( ;; ) {
        if (timerexpired) {
            if (statistics.failed > 0) {
                /* fprintf(stderr, "Correcting failed by signal\n"); */
                statistics.failed--;
            }

            close_post_file();
            return;
        }

        if (!multipart_first) {
            s = Socket(host, port);
            if (s < 0) {
                statistics.failed++;
                continue;
            }

            if (bench_params.post.in_file)
                multipart_first = 1;
        }

        if (rlen != write(s, req, rlen)) {
            statistics.failed++;
            close(s);

            if (bench_params.post.file) {
                rlen = strlen(multipart_initial);
                memcpy(req, multipart_initial, rlen);
                multipart_first = 0;

                fseek(bench_params.post.file, 0L, SEEK_SET);
            }

            continue;
        }

        if (bench_params.post.post) {
            statistics.bytes += rlen;
        }

        if (bench_params.post.in_file && !feof(bench_params.post.file)) {
retry:
            r = fread(req, sizeof (char), REQUEST_SIZE, bench_params.post.file);
            if (r < REQUEST_SIZE) {
                if (timerexpired) {
                    close_post_file();
                    continue;
                }

                if (ferror(bench_params.post.file)) {
                    fprintf(stderr, "Error in fread, child: %d.\n", getpid());
                    clearerr(bench_params.post.file);
                    close_post_file();

                    if (reread)
                        break;

                    if ((bench_params.post.file = fopen(bench_params.post.content, "r")) == NULL) {
                        fprintf(stderr, "Error in fopen %s, child: %d.\n", bench_params.post.content, getpid());
                        break;
                    }

                    /* socket is ok */
                    fseek(bench_params.post.file, bench_params.post.offset, SEEK_CUR);
                    reread = 1;
                    goto retry;
                } else {
                    reread = 0;

                    /* eof */
                    if (feof(bench_params.post.file))
                        eof = 1;
                }
            }

            if (r > 0) {
                bench_params.post.offset += r;
                rlen = r;
                continue;
            }
        }

        if (eof) {
            /* \r\n--boundary--\r\n */
            bench_params.post.offset = 0;
            bzero(request, REQUEST_SIZE);
            sprintf(request, "\r\n--%s--\r\n", bench_params.post.boundary);
            rlen = strlen(request);
            eof = 0;
            continue;
        }

        if (bench_params.http_version == 0) {
            if (shutdown(s, 1)) {
                statistics.failed++;
                close(s);
                continue;
            }
        }

        if (bench_params.force == 0) {
            /* read all available data from socket */
            for ( ;; ) {
                if (timerexpired)
                    break;

                i = read(s, buf, MAX_BUF_SIZE);
                /* fprintf(stderr, "%d\n", i); */
                if (i < 0) {
                    statistics.failed++;
                    close(s);

                    if (bench_params.post.in_file) {
                        rlen = strlen(multipart_initial);
                        memcpy(req, multipart_initial, rlen);
                        multipart_first = 0;

                        clearerr(bench_params.post.file);
                        fseek(bench_params.post.file, 0L, SEEK_SET);
                    }

                    goto nexttry;
                } else {
                    if (i == 0)
                        break;
                    else {
                        if (!bench_params.post.post)
                            statistics.bytes += i;
                    }
                }
            }
        }

        if (bench_params.post.in_file) {
            rlen = strlen(multipart_initial);
            memcpy(req, multipart_initial, rlen);
            multipart_first = 0;

            clearerr(bench_params.post.file);
            fseek(bench_params.post.file, 0L, SEEK_SET);
        }

        if (close(s)) {
            statistics.failed++;
            continue;
        }

        statistics.succeeded++;
    }
}

