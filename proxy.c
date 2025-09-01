#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_LINES 32

static void parse_host_header_value(const char *host_hdr_value,
                                    char *host, char *port);

typedef struct{
    char url[MAXLINE];
    char *obj;
    size_t size;
    unsigned long lru_tick;
} cache_line_t;

typedef struct{
    cache_line_t lines[CACHE_LINES];
    size_t total_size;
    unsigned long tick;
    pthread_rwlock_t rwlock;
}cache_t;

static cache_t g_cache;

static void cache_init(void);
static int cache_lookup(const char *url,char **out_buf,size_t *out_len);
static void cache_insert(const char *url,const char *buf,size_t len);
static void cache_evict_until(size_t need);
static void build_cache_key(const char *host,const char *port,const char *path,char *key,size_t cap);
static void parse_host_header_value(const char *host_hdr_value,
                                    char *host, char *port) {
    char tmp[MAXLINE];
    size_t len = strnlen(host_hdr_value, sizeof(tmp) - 1);
    strncpy(tmp, host_hdr_value, len);
    tmp[len] = '\0';
    char *s = tmp;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t')) --e;
    *e = '\0';

    char *colon = strchr(s, ':');
    if (colon) {
        *colon = '\0';
        strncpy(host, s, MAXLINE - 1);
        host[MAXLINE - 1] = '\0';
        strncpy(port, colon + 1, 15);
        port[15] = '\0';
    } else {
        strncpy(host, s, MAXLINE - 1);
        host[MAXLINE - 1] = '\0';
        strcpy(port, "80");
    }
}
static void cache_init(void) {
    memset(&g_cache, 0, sizeof(g_cache));
    pthread_rwlock_init(&g_cache.rwlock, NULL);
}

static void build_cache_key(const char *host, const char *port, const char *path,
                            char *key, size_t cap) {
    snprintf(key, cap, "http://%s:%s%s", host, port, path);
}
static int cache_lookup(const char *url, char **out_buf,size_t *out_len){
    int hit=0;
    pthread_rwlock_rdlock(&g_cache.rwlock);

    for(int i=0;i<CACHE_LINES;i++){
        cache_line_t *L=&g_cache.lines[i];
        if(L->size>0&&!strcmp(L->url,url)){
            *out_len=L->size;
            *out_buf=Malloc(L->size);
            memcpy(*out_buf,L->obj,L->size);

            hit=1;
            break;
        }
    }
    pthread_rwlock_unlock(&g_cache.rwlock);

    if(hit) {
        pthread_rwlock_wrlock(&g_cache.rwlock);
        g_cache.tick++;
        for(int i=0;i<CACHE_LINES;i++){
            cache_line_t *L=&g_cache.lines[i];
            if (L->size > 0 && !strcmp(L->url, url)) {
                L->lru_tick=g_cache.tick;
                break;
            }
        }
        pthread_rwlock_unlock(&g_cache.rwlock);
    }
    return hit;
}

static void cache_evict_until(size_t need){
    while(g_cache.total_size+need>MAX_CACHE_SIZE){
        int evict_idx=-1;
        unsigned long best=~0UL;
        for(int i=0;i<CACHE_LINES;i++){
            cache_line_t *L=&g_cache.lines[i];
            if(L->size>0 && L->lru_tick<best){
                best=L->lru_tick;
                evict_idx=i;
            }
        }
        if(evict_idx<0)break;
        cache_line_t *E=&g_cache.lines[evict_idx];
        g_cache.total_size-=E->size;
        Free(E->obj);
        memset(E,0,sizeof(*E));
    }
}
static void cache_insert(const char *url,const char *buf,size_t len){
    if(len>MAX_OBJECT_SIZE)return;
    if(len==0)return;

    pthread_rwlock_wrlock(&g_cache.rwlock);

    if(g_cache.total_size+len>MAX_CACHE_SIZE){
        cache_evict_until(len);
    }
    int idx=-1;
    unsigned long best=~0UL;
    for(int i=0;i<CACHE_LINES;i++){
        if(g_cache.lines[i].size==0){
            idx=i;break;
        }
        if(g_cache.lines[i].lru_tick<best){
            best=g_cache.lines[i].lru_tick;idx=i;
        }
    }
    cache_line_t *L=&g_cache.lines[idx];
    if(L->size>0){
        g_cache.total_size-=L->size;
        Free(L->obj);
        memset(L,0,sizeof(*L));
    }
    strncpy(L->url,url,sizeof(L->url)-1);
    L->obj=Malloc(len);
    memcpy(L->obj,buf,len);
    L->size=len;

    g_cache.tick++;
    L->lru_tick=g_cache.tick;
    g_cache.total_size+=len;
    pthread_rwlock_unlock(&g_cache.rwlock);
}

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void *thread(void *vargp);
void doit(int connfd);
int parse_uri(const char *uri, char *host, char *port, char *path);
void build_http_header(char *header, const char *host, const char *path,
                       const char *port, const char *client_headers);
void doit(int connfd)
{
    rio_t rio_client, rio_server;
    char buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], port[16], path[MAXLINE];
    char client_headers[MAXLINE*10];
    char server_request[MAXLINE*16];
    Rio_readinitb(&rio_client, connfd);
    if (Rio_readlineb(&rio_client, buf, MAXLINE) <= 0) return;
    fprintf(stderr, "[request-line] %s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET")) {
        const char *msg = "HTTP/1.0 501 Not Implemented\r\n\r\n";
        Rio_writen(connfd, (void *)msg, strlen(msg));
        return;
    }
    client_headers[0] = '\0';
    char host_hdr_value[MAXLINE]; host_hdr_value[0] = '\0';

    while (Rio_readlineb(&rio_client, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break;  
        if (!strncasecmp(buf, "Host:", 5)) {
            const char *v = buf + 5;
            strncpy(host_hdr_value, v, sizeof(host_hdr_value) - 1);
            host_hdr_value[sizeof(host_hdr_value) - 1] = '\0';
        }
        strncat(client_headers, buf, sizeof(client_headers) - 1 - strlen(client_headers));
    }


    host[0] = port[0] = path[0] = '\0';

    if (!strncasecmp(uri, "http://", 7)) {
        parse_uri(uri, host, port, path);
    } else if (uri[0] == '/') {
        if (host_hdr_value[0] == '\0') {
            const char *msg = "HTTP/1.0 400 Bad Request\r\n\r\n";
            Rio_writen(connfd, (void *)msg, strlen(msg));
            return;
        }
        parse_host_header_value(host_hdr_value, host, port);
        strncpy(path, uri, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        const char *msg = "HTTP/1.0 400 Bad Request\r\n\r\n";
        Rio_writen(connfd, (void *)msg, strlen(msg));
        return;
    }

    char cache_key[MAXLINE];
    build_cache_key(host, port, path, cache_key, sizeof(cache_key));

    char *hit_buf = NULL;
    size_t hit_len = 0;
    if (cache_lookup(cache_key, &hit_buf, &hit_len)) {
        fprintf(stderr, "[cache] HIT %s (len=%zu)\n", cache_key, hit_len);
        Rio_writen(connfd, (void *)hit_buf, hit_len);
        Free(hit_buf);
        return;
    }

    build_http_header(server_request, host, path, port, client_headers);

    int serverfd = Open_clientfd(host, port);
    Rio_readinitb(&rio_server, serverfd);

    size_t cap = MAX_OBJECT_SIZE + 1;
    char *obj_buf = Malloc(cap);
    size_t obj_len = 0;

    Rio_writen(serverfd, server_request, strlen(server_request));

    ssize_t n;
    while ((n = Rio_readnb(&rio_server, buf, MAXLINE)) > 0) {
        Rio_writen(connfd, buf, n);
        if (obj_len + (size_t)n <= MAX_OBJECT_SIZE) {
            memcpy(obj_buf + obj_len, buf, n);
            obj_len += (size_t)n;
        }
    }
    Close(serverfd);

    if (obj_len > 0 && obj_len <= MAX_OBJECT_SIZE) {
        cache_insert(cache_key, obj_buf, obj_len);
        fprintf(stderr, "[cache] INSERT %s (len=%zu)\n", cache_key, obj_len);
    }
    Free(obj_buf);
    Close(serverfd);
}
int parse_uri(const char *uri, char *host, char *port, char *path) {
    const char *pos;
    if (strncasecmp(uri, "http://", 7) == 0) {
        uri += 7;
    }

    pos = strchr(uri, '/');
    if (pos) {
        strcpy(path, pos);
        strncpy(host, uri, pos - uri);
        host[pos - uri] = '\0';
    } else {
        strcpy(path, "/");
        strcpy(host, uri);
    }

    char *colon = strchr(host, ':');
    if (colon) {
        *colon = '\0';
        strcpy(port, colon + 1);
    } else {
        strcpy(port, "80");
    }
    return 1;
}
void build_http_header(char *header, const char *host, const char *path,
                       const char *port, const char *client_headers) {
    char buf[MAXLINE];

    sprintf(header, "GET %s HTTP/1.0\r\n", path);

    sprintf(buf, "Host: %s:%s\r\n", host, port);
    strcat(header, buf);
    strcat(header, user_agent_hdr);
    strcat(header, "Connection: close\r\n");
    strcat(header, "Proxy-Connection: close\r\n");

    const char *p = client_headers;
    while (*p) {
        if (!strncasecmp(p, "Host:", 5) ||
            !strncasecmp(p, "Connection:", 11) ||
            !strncasecmp(p, "Proxy-Connection:", 17) ||
            !strncasecmp(p, "Keep-Alive:", 11) ||
            !strncasecmp(p, "Transfer-Encoding:", 18) ||
            !strncasecmp(p, "Upgrade:", 8)) {
        } else {
            strcat(header, p);
        }
        p = strchr(p, '\n');
        if (!p) break;
        p++;
    }

    strcat(header, "\r\n");
}
void *thread(void *vargp) {
    int connfd = *((int *)vargp); 
    Free(vargp);                  

    Pthread_detach(pthread_self()); 

    doit(connfd); 
    Close(connfd);
    return NULL;
}
int main(int argc,char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    Signal(SIGPIPE, SIG_IGN);

    cache_init();

    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);

        int *connfdp=Malloc(sizeof(int));
        *connfdp=connfd;

        pthread_t tid;
        pthread_create(&tid,NULL,thread,connfdp);
    }   
    //printf("%s", user_agent_hdr);
    return 0;
}

