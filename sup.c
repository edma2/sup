/*
 * sup.c
 * Multi-threaded chat server
 * Author: Eugene Ma 
 * http://github.com/edma2
 */
#include <stdio.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <netdb.h>
#include <pthread.h>
#include <string.h>

#define NUM_THREADS 4
#define QUEUE_MAX 16

/* Waiting client sockets */
struct {
        pthread_cond_t empty;
        pthread_mutex_t mutex;
        int sockets[QUEUE_MAX];
        int read, write;
} queue;

/* Active client sockets */
typedef struct Node Node;
struct {
        Node *head;
        pthread_mutex_t mutex;
} list;

struct Node {
        Node *next;
        int sock;
};

void queue_init(void);
int queue_add(int sock);
int queue_get(void);

static int queue_size(void);

void list_init(void);
void list_delete(int sock);
Node *list_append(int sock);
int list_broadcast(char *buf, int len);

void *run(void *arg);
void do_work(int client);

void logger(const char *format, ...) {
        va_list ap;

        va_start(ap, format);
        fprintf(stderr, "sup: ");
        vfprintf(stderr, format, ap);
        va_end(ap);
        fprintf(stderr, "\n");
}

int main(int argc, char *argv[]) {
        struct addrinfo *res, *ap, hints;
        struct sockaddr sa;
        socklen_t len = sizeof(struct sockaddr);
        char hostname[256];
        int family;
        int listener, client, i;
        pthread_t worker_th;
        void *src;

        if (argc != 3) {
                printf("Usage: %s <ip> <port>\n", argv[0]);
                return -1;
        }

        memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(argv[1], argv[2], &hints, &res)) {
                perror("getaddrinfo");
                return -1;
        }
        /* Bind socket to a valid socket address */
        for (ap = res; ap != NULL; ap = ap->ai_next) {
                family = ap->ai_family;
                listener = socket(family, ap->ai_socktype, ap->ai_protocol);
                if (listener >= 0 
                    && bind(listener, ap->ai_addr, ap->ai_addrlen) >= 0
                    && listen(listener, 5) >= 0)
                        break;
                close(listener);
        }
        freeaddrinfo(res);
        if (ap == NULL) {
                logger("unable to bind address");
                return -1;
        }
        if (family == AF_INET6)
                logger("IPv6 detected!");
        else
                logger("IPv4 only...");
        logger("listening on %s %s", argv[1], argv[2]);

        /* Start thread pool */
        queue_init();
        for (i = 0; i < NUM_THREADS; i++) {
                logger("starting thread %d", i);
                if (pthread_create(&worker_th, NULL, run, NULL) < 0)
                        perror("pthread_create");
                else if (pthread_detach(worker_th) < 0)
                        perror("pthread_detach");
                else
                        continue;
                close(listener);
                return -1;
        }
        /* Accept connections and pass sockets to queue */
        list_init();
        while (1) {
                client = accept(listener, &sa, &len);
                if (client < 0) {
                        perror("accept");
                        break;
                }
                src = (family == AF_INET6) ? 
                        (void *)&((struct sockaddr_in6 *)&sa)->sin6_addr :
                        (void *)&((struct sockaddr_in *)&sa)->sin_addr;
                if (inet_ntop(family, src, hostname, sizeof(hostname)) != NULL)
                        logger("new connection from %s!", hostname);
                if (queue_add(client) < 0)
                        close(client);
        }
        close(listener);
        return 0;
}

/* Initialize synchronization variables and reset queue pointers */
void queue_init(void) {
        pthread_mutex_init(&queue.mutex, NULL);
        pthread_cond_init(&queue.empty, NULL);
        queue.read = 0;
        queue.write = 0;
}

/* Get socket from queue */
int queue_get(void) {
        int sock;

        pthread_mutex_lock(&queue.mutex);

        /* Wait until we see an item in the queue */
        while (!queue_size())
                pthread_cond_wait(&queue.empty, &queue.mutex);

        sock = queue.sockets[queue.read];
        queue.read = (queue.read+1)%QUEUE_MAX;
        pthread_mutex_unlock(&queue.mutex);
        return sock;
}

/* Add socket to queue */
int queue_add(int sock) {
        pthread_mutex_lock(&queue.mutex);

        /* Return immediately if queue full */
        if (queue_size() == QUEUE_MAX-1) {
                pthread_mutex_unlock(&queue.mutex);
                return -1;
        }

        /* Add socket to queue and wake up waiters */
        queue.sockets[queue.write] = sock;
        queue.write = (queue.write+1)%QUEUE_MAX;
        pthread_cond_signal(&queue.empty);
        pthread_mutex_unlock(&queue.mutex);
        return 0;
}

/* Return the number of elements in the queue */
static int queue_size(void) {
        return (QUEUE_MAX - queue.read + queue.write)%QUEUE_MAX;
}

void list_init(void) {
        list.head = NULL;
        pthread_mutex_init(&list.mutex, NULL);
}

Node *list_append(int sock) {
        Node *p;

        p = malloc(sizeof(Node));
        if (p == NULL)
                return NULL;
        p->sock = sock;
        pthread_mutex_lock(&list.mutex);
        p->next = list.head;
        list.head = p;
        pthread_mutex_unlock(&list.mutex);
        return p;
}

void list_delete(int sock) {
        Node *p, *prev;

        pthread_mutex_lock(&list.mutex);
        for (p = list.head; p != NULL; p = p->next) {
                if (p->sock == sock)
                        break;
                prev = p;
        }
        /* Not found! */
        if (p == NULL)
                return;
        if (p == list.head)
                list.head = p->next;
        else 
                prev->next = p->next;
        free(p);
        pthread_mutex_unlock(&list.mutex);
}

int list_broadcast(char *buf, int len) {
        Node *p;

        pthread_mutex_lock(&list.mutex);
        for (p = list.head; p != NULL; p = p->next) {
                if (write(p->sock, buf, len) != len)
                        return -1;
        }
        pthread_mutex_unlock(&list.mutex);
        return 0;
}

/* Fetch sockets from queue and process client requests */
void *run(void *arg) {
        int client;

        while (1) {
                client = queue_get();
                list_append(client);
                do_work(client);
                list_delete(client);
                close(client);
        }
        return NULL;
}

void do_work(int client) {
        char buf[1024];
        int seen;

        while (1) {
                seen = read(client, buf, sizeof(buf)-1);
                if (seen < 0) {
                        perror("read");
                        break;
                } 
                if (!seen) {
                        logger("Client closed connection!");
                        break;
                }
                buf[seen] = '\0';
                /* Broadcast message to all clients except mine*/
                if (list_broadcast(buf, seen+1) < 0) {
                        perror("write");
                        break;
                }
        }
}
