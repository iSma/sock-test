#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>

char HOSTNAME[1024];
FILE *LOG_FILE = NULL;

#define DEBUG "DEBUG"
#define INFO  "INFO"
#define ERROR "ERROR"

#define DNS_RETRY 5

struct options {
    const char *port;
    const char *tracker_url;
    int n_peers;
    int client_loops;
    int sleep_start;
    int sleep_loop;
    int sock_mode;
    int ip_version;
    const char *log_dir;
    bool client;
    bool server;
} DEFAULT_OPTIONS = {
    .port         = "54321",
    .tracker_url  = "localhost",
    .n_peers      = 50,
    .client_loops = 0,
    .sleep_start  = 30,
    .sleep_loop   = 5,
    .sock_mode    = SOCK_DGRAM,
    .ip_version   = AF_UNSPEC,
    .log_dir      = "-",
    .client       = false,
    .server       = false
};

void *client(void *arg);
void client_loop(int sock_mode, const struct addrinfo *peer, int loop);

void *server(void *arg);
void server_loop(int sock_mode, int sockfd);


uint16_t addr_to_str(const struct sockaddr *ai_addr, char *str, size_t max_len);
void LOG(const char *level, const char *subject, const char *format, ...);
void LOG_ERRNO(const char *subject);

struct options parse_options(int argc, char **argv);
void usage(const char *argv0);

int main(int argc, char **argv)
{
    struct options opt = parse_options(argc, argv);
    gethostname(HOSTNAME, sizeof(HOSTNAME));

    if (strncmp(opt.log_dir, "-", 1) != 0) {
        char log_path[2048];
        if (opt.log_dir[strlen(opt.log_dir)-1] == '/')
            snprintf(log_path, sizeof(log_path), "%s%s.txt", opt.log_dir, HOSTNAME);
        else
            snprintf(log_path, sizeof(log_path), "%s/%s.txt", opt.log_dir, HOSTNAME);

        FILE *log_file = fopen(log_path, "w");
        if (!log_file)
            fprintf(stderr, "Can't open log file '%s', logging to stdout", log_path);
        else
            fprintf(stderr, "Logging to '%s'", log_path);

        LOG_FILE = log_file;
    }

    LOG(INFO, "program_start",
            "tracker=%s\tport=%s\tn_peers=%d\tmode=%s\tip_version=%s\t"
            "client_loops=%d\tsleep_start=%d\tsleep_loop=%d\tclient=%d\tserver=%d\tlog_dir=%s",
            opt.tracker_url, opt.port, opt.n_peers,
            opt.sock_mode == SOCK_DGRAM ? "UDP" : "TCP",
            opt.ip_version == AF_INET ? "4" :
            opt.ip_version == AF_INET6 ? "6" : "any",
            opt.client_loops, opt.sleep_start, opt.sleep_loop,
            opt.client, opt.server, opt.log_dir);

    pthread_t server_thread = 0;

    if (opt.server) {
        if (opt.client)
            pthread_create(&server_thread, NULL, server, &opt);
        else
            server(&opt);
    }

    if (opt.client) {
        sleep(2);
        client(&opt);

        if (opt.server)
            pthread_cancel(server_thread);
    }

    LOG(INFO, "program_end", "");
    return 0;
}


void *client(void *arg) {
    const struct options *opt = arg;

    LOG(INFO, "client_sleep", "sleep=%d", opt->sleep_start);
    sleep(opt->sleep_start);
    LOG(INFO, "client_start", "");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = opt->ip_version;
    hints.ai_socktype = opt->sock_mode;

    struct addrinfo *peers = NULL;

    for (int loop = 0; opt->client_loops <= 0 || loop < opt->client_loops; loop++) {
        LOG(INFO, "client_loop", "loop=%d", loop);

        LOG(INFO, "dns_lookup", "url=%s", opt->tracker_url);
        for (int i = 0; i < DNS_RETRY; i++) {
            int status = getaddrinfo(opt->tracker_url, opt->port, &hints, &peers);
            if (status == 0)
                break;

            LOG(ERROR, "getaddrinfo", "try=%d\tretries=%d\terrno=%d\tstrerror=%s",
                    i+1, DNS_RETRY,
                    status, gai_strerror(status));
            freeaddrinfo(peers);

            if (i+1 < DNS_RETRY)
                sleep(opt->sleep_loop);
            else
                return NULL;
        }

        int n_peers = 0;
        for(struct addrinfo *p = peers; p != NULL; p = p->ai_next) {
            client_loop(opt->sock_mode, p, loop);
            if (++n_peers >= opt->n_peers)
                break;
        }

        sleep(opt->sleep_loop);
    }

    freeaddrinfo(peers);
    LOG(INFO, "client_end", "");
    return NULL;
}

void client_loop(int sock_mode, const struct addrinfo *peer, int loop) {
    int status = 0;
    char addr_str[64];
    char loop_str[16] = {'\0'};

    addr_to_str(peer->ai_addr, addr_str, sizeof(addr_str));

    snprintf(loop_str, sizeof(loop_str), "%d", loop);

    LOG(INFO, "msg_send", "status=try\tloop=%d\tto=%s", loop, addr_str);

    LOG(DEBUG, "socket", "domain=%d\ttype=%d\tprotocol=%d",
            peer->ai_family, peer->ai_socktype, peer->ai_protocol);

    int sockfd = socket(peer->ai_family, peer->ai_socktype, peer->ai_protocol);
    if (sockfd < 0) {
        LOG_ERRNO("socket");
        return;
    }

    if (sock_mode == SOCK_DGRAM) {
        LOG(DEBUG, "sendto", "sockfd=%d\tbuf=%s\tlen=%d\tdest_addr=%s\taddrlen=%d",
                sockfd, loop_str, sizeof(loop_str), addr_str, peer->ai_addrlen);

        status = sendto(sockfd, loop_str, sizeof(loop_str), 0, peer->ai_addr, peer->ai_addrlen);
        if (status < 0) {
            LOG_ERRNO("sendto");
            close(sockfd);
            return;
        }
    }

    if (sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "connect", "sockfd=%d\taddr=%s\taddrlen=%d",
                sockfd, addr_str, peer->ai_addrlen);

        status = connect(sockfd, peer->ai_addr, peer->ai_addrlen);
        if (status < 0) {
            LOG_ERRNO("connect");
            close(sockfd);
            return;
        }


        LOG(DEBUG, "send", "sockfd=%d\tbuf=%s\tlen=%d",
                sockfd, loop_str, sizeof(loop_str));

        status = send(sockfd, loop_str, sizeof(loop_str), 0);
        if (status < 0) {
            LOG_ERRNO("send");
            close(sockfd);
            return;
        }
    }

    LOG(INFO, "msg_send", "status=ok\tloop=%d\tto=%s", loop, addr_str);
    close(sockfd);
}


void *server(void *arg) {
    const struct options *opt = arg;
    struct addrinfo hints, *host;
    char addr_str[64];
    int status = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = opt->ip_version;
    hints.ai_socktype = opt->sock_mode;
    hints.ai_flags = AI_PASSIVE;
    getaddrinfo(NULL, opt->port, &hints, &host);

    for(struct addrinfo *p = host; p != NULL; p = p->ai_next) {
        addr_to_str(host->ai_addr, addr_str, sizeof(addr_str));
        LOG(INFO, "server_addr", "ip_verion=%s addr=%s",
                p->ai_family == AF_INET6 ? "IPv6" : "IPv4", addr_str);
    }

    LOG(DEBUG, "server_socket", "domain=%d\ttype=%d\tprotocol=%d",
            host->ai_family, host->ai_socktype, host->ai_protocol);

    int sockfd = socket(host->ai_family, host->ai_socktype, host->ai_protocol);
    if (sockfd < 0) {
        LOG_ERRNO("server_socket");
        freeaddrinfo(host);
        return NULL;
    }

    addr_to_str(host->ai_addr, addr_str, sizeof(addr_str));
    LOG(DEBUG, "bind", "sockfd=%d\taddr=%s\taddrlen=%d",
            sockfd, addr_str, host->ai_addrlen);

    status = bind(sockfd, host->ai_addr, host->ai_addrlen);
    if (status < 0) {
        LOG_ERRNO("bind");
        freeaddrinfo(host);
        return NULL;
    }

    freeaddrinfo(host);

    if (opt->sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "listen", "sockfd=%d\tbacklog=%d", sockfd, opt->n_peers);
        status = listen(sockfd, opt->n_peers);
        if (status < 0) {
            LOG_ERRNO("listen");
            return NULL;
        }
    }

    LOG(INFO, "server_start", "mode=%s\tport=%s",
            opt->sock_mode == SOCK_DGRAM ? "UDP" : "TCP", opt->port);

    for (;;) {
        server_loop(opt->sock_mode, sockfd);
    }
}


void server_loop(int sock_mode, int sockfd) {
    char buf[16];
    char addr_str[64];
    struct sockaddr client;
    socklen_t client_size = sizeof(client);
    int status = 0;

    if (sock_mode == SOCK_DGRAM) {
        LOG(DEBUG, "recvfrom", "sockfd=%d\tlen=%d\tsrc_addr=%p",
                sockfd, sizeof(buf), &client);

        status = recvfrom(sockfd, buf, sizeof(buf), 0, &client, &client_size);
        if (status < 0) {
            LOG_ERRNO("recvfrom");
            return;
        }
    }

    if (sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "accept", "sockfd=%d\tsrc_addr=%p", sockfd, &client);

        int clientfd = accept(sockfd, &client, &client_size);
        if (clientfd < 0) {
            LOG_ERRNO("accept");
            return;
        }

        LOG(DEBUG, "recv", "clientfd=%d\tlen=%d", clientfd, sizeof(buf));
        status = recv(clientfd, buf, sizeof(buf), 0);
        if (status < 0) {
            LOG_ERRNO("recv");
            close(clientfd);
            return;
        }

        close(clientfd);
    }

    addr_to_str(&client, addr_str, sizeof(addr_str));
    LOG(INFO, "msg_recv", "from=%s\tloop=%s", addr_str, buf);
}


uint16_t addr_to_str(const struct sockaddr *addr, char *str, size_t max_len) {
    char addr_str[INET6_ADDRSTRLEN];

    if (addr->sa_family == AF_INET) {
        struct sockaddr_in *ipv4 = (struct sockaddr_in *)addr;
        uint16_t port = ntohs(ipv4->sin_port);

        inet_ntop(addr->sa_family, &(ipv4->sin_addr), addr_str, sizeof(addr_str));
        snprintf(str, max_len, "%s:%d", addr_str, port);
        return port;
    }

    if (addr->sa_family == AF_INET6) {
        struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)addr;
        uint16_t port = ntohs(ipv6->sin6_port);

        inet_ntop(addr->sa_family, &(ipv6->sin6_addr), addr_str, sizeof(addr_str));
        snprintf(str, max_len, "[%s]:%d", addr_str, port);
        return port;
    }

    snprintf(str, max_len, "<UNKNOWN>");
    return 0;
}


void LOG(const char *level, const char *subject, const char *format, ...) {
    FILE *file = LOG_FILE ? LOG_FILE : stdout;
    char time_str[20] = {'\0'};
    time_t now = time(NULL);
    struct tm time_r;
    localtime_r(&now, &time_r);
    strftime(time_str, sizeof(time_str), "%FT%H:%M:%S", &time_r);

    char msg[1024] = {'\0'};
    va_list vargs;
    va_start(vargs, format);
    vsnprintf(msg, sizeof(msg), format, vargs);
    va_end (vargs);

    fprintf(file, "%s\t%s\t%s\t%s\t%s\n", time_str, HOSTNAME, level, subject, msg);
    fflush(file);
}


void LOG_ERRNO(const char *subject) {
    LOG(ERROR, subject, "errno=%d\tstrerror=%s", errno, strerror(errno));
}


struct options parse_options(int argc, char **argv) {
    struct options opt = DEFAULT_OPTIONS;
    bool errors = false;
    char c;
    int port = 0;
    while ((c = getopt(argc, argv, "hp:u:n:l:x:z:m:i:f:cs")) != -1) {
        switch(c) {
            case 'h':
                usage(argv[0]);
                exit(0);
            case 'p':
                port = strtol(optarg, NULL, 10);
                if (!errno && port > 0 && port <= 65536)
                    break;

                fprintf(stderr, "Option -%c must be in 1..65536\n", c);
                errors = true;
                break;

            case 'u':
                opt.tracker_url = optarg;
                break;

            case 'n':
                opt.n_peers = strtol(optarg, NULL, 10);
                if (!errno && opt.n_peers > 0)
                    break;

                fprintf(stderr, "Option -%c must > 0\n", c);
                errors = true;
                break;

            case 'l':
                opt.client_loops = strtol(optarg, NULL, 10);
                if (!errno && opt.client_loops >= 0)
                    break;

                fprintf(stderr, "Option -%c must be >= 0\n", c);
                errors = true;
                break;

            case 'x':
                opt.sleep_start = strtol(optarg, NULL, 10);
                if (!errno && opt.sleep_start >= 0)
                    break;

                fprintf(stderr, "Option -%c must be >= 0\n", c);
                errors = true;
                break;

            case 'z':
                opt.sleep_loop = strtol(optarg, NULL, 10);
                if (!errno && opt.sleep_loop >= 0)
                    break;

                fprintf(stderr, "Option -%c must be >= 0\n", c);
                errors = true;
                break;

            case 'm':
                if (strcmp(optarg, "UDP") == 0) {
                    opt.sock_mode = SOCK_DGRAM;
                    break;
                }

                if (strcmp(optarg, "TCP") == 0) {
                    opt.sock_mode = SOCK_STREAM;
                    break;
                }

                fprintf(stderr, "Option -%c must be UDP or TCP\n", c);
                errors = true;
                break;
            case 'i':
                if (strcmp(optarg, "4") == 0) {
                    opt.ip_version = AF_INET;
                    break;
                }

                if (strcmp(optarg, "6") == 0) {
                    opt.ip_version = AF_INET6;
                    break;
                }

                if (strcmp(optarg, "any") == 0) {
                    opt.ip_version = AF_UNSPEC;
                    break;
                }

                fprintf(stderr, "Option -%c must be 4, 6 or any\n", c);
                errors = true;
                break;

            case 'f':
                opt.log_dir = optarg;
                break;

            case 'c':
                opt.client = true;
                break;
            case 's':
                opt.server = true;
                break;

            default:
                errors = true;
        }
    }

    if (errors) {
        usage(argv[0]);
        exit(1);
    }

    return opt;
}


void usage(const char *argv0) {
    struct options o = DEFAULT_OPTIONS;
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Options:\n"
            "  -h               Print this message and exit\n"
            "  -p PORT          (default: %s)\n"
            "  -u TRACKER_URL   Tracker url (usually tasks.<docker_service_name>) (default: '%s')\n"
            "  -n N_PEERS       Maximum number of peers to exchange with (default: %d)\n"
            "  -l CLIENT_LOOPS  Number of client iterations, 0 for non-stop (default: %d)\n"
            "  -x SLEEP_START   Seconds of sleep before client starts (default: %d)\n"
            "  -z SLEEP_LOOP    Seconds of sleep after each iteration (default: %d)\n"
            "  -m SOCK_MODE     Socket mode: UDP or TCP (default: %s)\n"
            "  -i IP_VERSION    IP version: 4, 6 or any (default: %s)\n"
            "  -f LOG_DIR       Log destination directory or '-' for stdout (default: '%s')\n"
            "  -c               Run client\n"
            "  -s               Run server\n",
            argv0, o.port, o.tracker_url,
            o.n_peers, o.client_loops,
            o.sleep_start, o.sleep_loop,
            o.sock_mode == SOCK_DGRAM ? "UDP" : "TCP",
            o.ip_version == AF_INET ? "4" : o.ip_version == AF_INET6 ? "6" : "any",
            o.log_dir);
}
