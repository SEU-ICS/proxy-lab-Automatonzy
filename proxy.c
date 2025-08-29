#include <stdio.h>
#include "csapp.h"
/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
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
    parse_uri(uri, host, port, path);

    client_headers[0] = '\0';
    while (Rio_readlineb(&rio_client, buf, MAXLINE) > 0) {
        if (!strcmp(buf, "\r\n")) break;
        strcat(client_headers, buf);
    }

    build_http_header(server_request, host, path, port, client_headers);
    int serverfd = Open_clientfd(host, port);
    Rio_readinitb(&rio_server, serverfd);

    Rio_writen(serverfd, server_request, strlen(server_request));

    ssize_t n;
    while ((n = Rio_readnb(&rio_server, buf, MAXLINE)) > 0) {
        Rio_writen(connfd, buf, n);
    }

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
int main(int argc,char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int listenfd = Open_listenfd(argv[1]);
    while (1) {
        struct sockaddr_storage clientaddr;
        socklen_t clientlen = sizeof(clientaddr);
        int connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        doit(connfd);
        Close(connfd);
    }   
    //printf("%s", user_agent_hdr);
    return 0;
}
