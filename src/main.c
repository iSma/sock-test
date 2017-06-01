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
void LOG(const char *level, const char *format, ...);
void LOG_ERRNO(const char *prefix);

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
            LOG_ERRNO("Can't open log file '%s', logging to stdout");
        else
            LOG(INFO, "Logging to %s", log_path);

        LOG_FILE = log_file;
    }

    LOG(INFO, "Program started");
    LOG(INFO, "Options: tracker=%s port=%s n_peers=%d mode=%s ip_version=%s",
            opt.tracker_url, opt.port, opt.n_peers,
            opt.sock_mode == SOCK_DGRAM ? "UDP" : "TCP",
            opt.ip_version == AF_INET ? "4" :
            opt.ip_version == AF_INET6 ? "6" : "any");
    LOG(INFO, "Options: client_loops=%d sleep_start=%d sleep_loop=%d client=%d server=%d log_dir=%s",
            opt.client_loops, opt.sleep_start, opt.sleep_loop,
            opt.client, opt.server, opt.log_dir);

    pthread_t server_thread = 0;

    if (opt.server) {
        LOG(INFO, "Starting server...");

        if (opt.client)
            pthread_create(&server_thread, NULL, server, &opt);
        else
            server(&opt);
    }

    if (opt.client) {
        sleep(2);
        LOG(INFO, "Starting client...");

        client(&opt);

        if (opt.server)
            pthread_cancel(server_thread);
    }

    LOG(INFO, "Program stopped");
    return 0;
}


void *client(void *arg) {
    const struct options *opt = arg;

    LOG(INFO, "Sleeping for %ds", opt->sleep_start);
    sleep(opt->sleep_start);
    LOG(INFO, "Client started");

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = opt->ip_version;
    hints.ai_socktype = opt->sock_mode;

    struct addrinfo *peers = NULL;

    LOG(INFO, "getaddrinfo: retrieving peers in %s", opt->tracker_url);
    for (int i = 0; i < DNS_RETRY; i++) {
        int status = getaddrinfo(opt->tracker_url, opt->port, &hints, &peers);
        if (status == 0)
            break;

        LOG(ERROR, "getaddrinfo: Failed (%d/%d) [%d] %s", i+1, DNS_RETRY, status, gai_strerror(status));
        freeaddrinfo(peers);

        if (i+1 < DNS_RETRY) {
            LOG(INFO, "getaddrinfo: retrying in %ds", opt->sleep_loop);
            sleep(opt->sleep_loop);
        } else {
            LOG(ERROR, "getaddrinfo: failed %d times, exiting", DNS_RETRY);
            return NULL;
        }
    }

    LOG(INFO, "getaddrinfo: success", opt->sleep_start);

    for (int loop = 0; opt->client_loops <= 0 || loop < opt->client_loops; loop++) {
        LOG(INFO, "Loop %d", loop);

        int n_peers = 0;
        for(struct addrinfo *p = peers; p != NULL; p = p->ai_next) {
            client_loop(opt->sock_mode, p, loop);
            if (++n_peers >= opt->n_peers)
                break;
        }

        sleep(opt->sleep_loop);
    }

    freeaddrinfo(peers);
    return NULL;
}

void client_loop(int sock_mode, const struct addrinfo *peer, int loop) {
    int status = 0;
    char addr_str[64];
    char loop_str[16] = {'\0'};

    addr_to_str(peer->ai_addr, addr_str, sizeof(addr_str));

    snprintf(loop_str, sizeof(loop_str), "%d", loop);

    LOG(INFO, "Sending to %s", addr_str);

    LOG(DEBUG, "socket: domain=%d type=%d protocol=%d",
            peer->ai_family, peer->ai_socktype, peer->ai_protocol);

    int sockfd = socket(peer->ai_family, peer->ai_socktype, peer->ai_protocol);
    if (sockfd < 0) {
        LOG_ERRNO("socket");
        return;
    }

    if (sock_mode == SOCK_DGRAM) {
        LOG(DEBUG, "sendto: sockfd=%d buf='%s' len=%d dest_addr=%s addrlen=%d",
                sockfd, loop_str, sizeof(loop_str), addr_str, peer->ai_addrlen);

        status = sendto(sockfd, loop_str, sizeof(loop_str), 0, peer->ai_addr, peer->ai_addrlen);
        if (status < 0) {
            LOG_ERRNO("sendto");
            close(sockfd);
            return;
        }
    }

    if (sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "connect: sockfd=%d addr=%s addrlen=%d",
                sockfd, addr_str, peer->ai_addrlen);

        status = connect(sockfd, peer->ai_addr, peer->ai_addrlen);
        if (status < 0) {
            LOG_ERRNO("connect");
            close(sockfd);
            return;
        }


        LOG(DEBUG, "send: sockfd=%d buf='%s' len=%d",
                sockfd, loop_str, sizeof(loop_str));

        status = send(sockfd, loop_str, sizeof(loop_str), 0);
        if (status < 0) {
            LOG_ERRNO("send");
            close(sockfd);
            return;
        }
    }

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

    LOG(INFO, "Available addresses (selecting first):");
    for(struct addrinfo *p = host; p != NULL; p = p->ai_next) {
        addr_to_str(host->ai_addr, addr_str, sizeof(addr_str));
        LOG(INFO, "=> [%s]: %s", p->ai_family == AF_INET6 ? "IPv6" : "IPv4", addr_str);
    }

    LOG(DEBUG, "server socket: domain=%d type=%d protocol=%d",
            host->ai_family, host->ai_socktype, host->ai_protocol);

    int sockfd = socket(host->ai_family, host->ai_socktype, host->ai_protocol);
    if (sockfd < 0) {
        LOG_ERRNO("server socket");
        freeaddrinfo(host);
        return NULL;
    }

    addr_to_str(host->ai_addr, addr_str, sizeof(addr_str));
    LOG(DEBUG, "bind: sockfd=%d addr=%s addrlen=%d",
            sockfd, addr_str, host->ai_addrlen);

    status = bind(sockfd, host->ai_addr, host->ai_addrlen);
    if (status < 0) {
        LOG_ERRNO("bind");
        freeaddrinfo(host);
        return NULL;
    }

    freeaddrinfo(host);

    if (opt->sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "listen: sockfd=%d backlog=%d", sockfd, opt->n_peers);
        status = listen(sockfd, opt->n_peers);
        if (status < 0) {
            LOG_ERRNO("listen");
            return NULL;
        }
    }

    LOG(INFO, "%s Server listening on port %s",
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
        LOG(DEBUG, "recvfrom: sockfd=%d len=%d src_addr=%p",
                sockfd, sizeof(buf), &client);

        status = recvfrom(sockfd, buf, sizeof(buf), 0, &client, &client_size);
        if (status < 0) {
            LOG_ERRNO("recvfrom");
            return;
        }
    }

    if (sock_mode == SOCK_STREAM) {
        LOG(DEBUG, "accept: sockfd=%d src_addr=%p", sockfd, &client);

        int clientfd = accept(sockfd, &client, &client_size);
        if (clientfd < 0) {
            LOG_ERRNO("accept");
            return;
        }

        LOG(DEBUG, "recv: clientfd=%d len=%d", clientfd, sizeof(buf));
        status = recv(clientfd, buf, sizeof(buf), 0);
        if (status < 0) {
            LOG_ERRNO("recv");
            close(clientfd);
            return;
        }

        close(clientfd);
    }

    addr_to_str(&client, addr_str, sizeof(addr_str));
    LOG(INFO, "Message received from %s during loop %s", addr_str, buf);
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


void LOG(const char *level, const char *format, ...) {
    FILE *file = LOG_FILE ? LOG_FILE : stdout;
    char time_str[20] = {'\0'};
    time_t now = time(NULL);
    struct tm time_r;
    localtime_r(&now, &time_r);
    strftime(time_str, sizeof(time_str), "%FT%H:%M:%S", &time_r);

    fprintf(file, "[%s]\t[%s]\t%s:\t", time_str, HOSTNAME, level);

    va_list vargs;
    va_start(vargs, format);
    vfprintf(file, format, vargs);
    va_end (vargs);

    fprintf(file, "\n");
    fflush(file);
}


void LOG_ERRNO(const char *prefix) {
    LOG("ERROR", "%s: [%d] %s", prefix, errno, strerror(errno));
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
