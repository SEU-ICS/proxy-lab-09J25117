#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_N 32

static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct {
    char uri[MAXLINE];
    char *data;
    int len;
    int stamp;
    int valid;
} blk;

static blk cache[CACHE_N];
static sem_t wlock, mlock;
static int readers = 0;
static int tick = 0;

static int has(char *line, char *pre)
{
    return strncasecmp(line, pre, strlen(pre)) == 0;
}

static void err(int fd, char *cause, char *code, char *msg)
{
    char buf[MAXLINE], body[MAXBUF];
    sprintf(body, "<html><body>%s %s (%s)</body></html>", code, msg, cause);
    sprintf(buf, "HTTP/1.0 %s %s\r\n", code, msg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}

static void parse_uri(char *uri, char *host, char *port, char *path)
{
    char *p = strstr(uri, "//");
    char *s, *c;
    if (p == NULL)
        p = uri;
    else
        p += 2;
    s = strchr(p, '/');
    if (s == NULL) {
        strcpy(path, "/");
    } else {
        strcpy(path, s);
        *s = '\0';
    }
    c = strchr(p, ':');
    if (c != NULL) {
        strcpy(port, c + 1);
        *c = '\0';
    } else {
        strcpy(port, "80");
    }
    strcpy(host, p);
}

static blk *find(char *uri)
{
    int i;
    for (i = 0; i < CACHE_N; i++)
        if (cache[i].valid && !strcmp(cache[i].uri, uri))
            break;
    if (i == CACHE_N)
        return NULL;
    P(&mlock);
    readers++;
    if (readers == 1)
        P(&wlock);
    cache[i].stamp = ++tick;
    V(&mlock);
    return &cache[i];
}

static void leave(void)
{
    P(&mlock);
    readers--;
    if (readers == 0)
        V(&wlock);
    V(&mlock);
}

static void put(char *uri, char *data, int len)
{
    int i, j;
    P(&wlock);
    for (i = 0; i < CACHE_N; i++)
        if (!cache[i].valid)
            break;
    if (i == CACHE_N) {
        i = 0;
        for (j = 1; j < CACHE_N; j++)
            if (cache[j].stamp < cache[i].stamp)
                i = j;
        free(cache[i].data);
    }
    cache[i].data = malloc(len);
    memcpy(cache[i].data, data, len);
    cache[i].len = len;
    cache[i].stamp = ++tick;
    cache[i].valid = 1;
    strcpy(cache[i].uri, uri);
    V(&wlock);
}

static void doit(int fd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], key[MAXLINE];
    char host[MAXLINE], port[16], path[MAXLINE];
    char req[MAXLINE], hdr[MAXBUF];
    rio_t rio, srio;
    blk *c;
    char *body;
    int sfd, n, hn, bn, ok;

    Rio_readinitb(&rio, fd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        Close(fd);
        return;
    }
    sscanf(buf, "%s %s", method, uri);
    strcpy(key, uri);

    if (strcasecmp(method, "GET") != 0) {
        err(fd, method, "501", "Not Implemented");
        Close(fd);
        return;
    }

    c = find(key);
    if (c != NULL) {
        Rio_writen(fd, c->data, c->len);
        leave();
        Close(fd);
        return;
    }

    parse_uri(uri, host, port, path);

    sprintf(req, "GET %s HTTP/1.0\r\n", path);

    while (Rio_readlineb(&rio, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n"))
            break;
        if (has(buf, "Host:") || has(buf, "User-Agent:") ||
            has(buf, "Connection:") || has(buf, "Proxy-Connection:"))
            continue;
        if (strlen(req) + strlen(buf) < MAXLINE)
            strcat(req, buf);
    }

    sprintf(buf, "Host: %s\r\n", host);
    strcat(req, buf);
    strcat(req, user_agent_hdr);
    strcat(req, "Connection: close\r\n");
    strcat(req, "Proxy-Connection: close\r\n\r\n");

    sfd = Open_clientfd(host, port);
    Rio_writen(sfd, req, strlen(req));

    Rio_readinitb(&srio, sfd);

    hn = 0;
    while ((n = Rio_readlineb(&srio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
        if (hn + n < MAXBUF) {
            memcpy(hdr + hn, buf, n);
            hn += n;
        }
        if (!strcmp(buf, "\r\n"))
            break;
    }

    body = malloc(MAX_OBJECT_SIZE);
    bn = 0;
    ok = 1;
    while ((n = Rio_readnb(&srio, buf, MAXLINE)) > 0) {
        Rio_writen(fd, buf, n);
        if (ok && bn + n <= MAX_OBJECT_SIZE) {
            memcpy(body + bn, buf, n);
            bn += n;
        } else {
            ok = 0;
        }
    }

    if (ok && bn > 0 && hn + bn <= MAX_OBJECT_SIZE) {
        char *full = malloc(hn + bn);
        memcpy(full, hdr, hn);
        memcpy(full + hn, body, bn);
        put(key, full, hn + bn);
        free(full);
    }

    free(body);
    Close(sfd);
    Close(fd);
}

static void *work(void *v)
{
    int fd = (int)(long)v;
    Pthread_detach(pthread_self());
    doit(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    int lfd, cfd;
    socklen_t clen;
    struct sockaddr_storage caddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);
    Sem_init(&wlock, 0, 1);
    Sem_init(&mlock, 0, 1);

    lfd = Open_listenfd(argv[1]);

    while (1) {
        clen = sizeof(caddr);
        cfd = Accept(lfd, (SA *)&caddr, &clen);
        Pthread_create(&tid, NULL, work, (void *)(long)cfd);
    }
}
