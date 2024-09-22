#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>

const char* MSG_501 = "HTTP/1.0 501 Not Implemented\r\n\r\n"
                      "<html><body><h1>501 Not Implemented</h1></body></html>\r\n";

const char* MSG_400 = "HTTP/1.0 400 Bad Request\r\n\r\n"
                      "<html><body><h1>400 Bad Request</h1></body><html>\r\n";

const char* MSG_404 = "HTTP/1.0 404 Not Found\r\n\r\n"
                      "<html><body><h1>404 Not Found</h1></body></html>\r\n";

const char* MSG_200 = "HTTP/1.0 200 OK\r\n\r\n";

static void die(const char *msg){
    perror(msg);
    exit(1);
}

int connect_to_mdb(char* mdb_lookup_host, int mdb_port){
    //get mdb server IP
    struct hostent *he;
    if ((he = gethostbyname(mdb_lookup_host)) == NULL) {
        die("gethostbyname failed");
    }
    char *mdbip = inet_ntoa(*(struct in_addr *)he->h_addr);

    //mdb sock
    int mdbsock;
    if ((mdbsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    struct sockaddr_in mdbaddr;
    memset(&mdbaddr, 0, sizeof(mdbaddr)); // must zero out the structure
    mdbaddr.sin_family      = AF_INET;
    mdbaddr.sin_addr.s_addr = inet_addr(mdbip);
    mdbaddr.sin_port        = htons(mdb_port); // must be in network byte order

    // Establish a TCP connection to the mdbserver

    if (connect(mdbsock, (struct sockaddr *) &mdbaddr, sizeof(mdbaddr)) < 0)
        die("connect failed");

    return mdbsock;

}

struct app_state{
    int server_sock;
    int mdb_sock;
    FILE* mdb_file;
    char* web_root;
};


struct app_state* initialize_app(int server_port, char* web_root, char* mdb_lookup_host, int mdb_port){
    struct app_state* s = malloc(sizeof(struct app_state));
    // to do : die if malloc fails 
    s->mdb_sock = connect_to_mdb(mdb_lookup_host, mdb_port);
    s->mdb_file = fdopen(s->mdb_sock, "r");
    s->web_root = web_root;
   // fprintf(stderr, "%s", s->web_root);
    int servsock;
    if ((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        die("socket failed");

    // Construct local address structure

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // any network interface
    servaddr.sin_port = htons(server_port);

    // Bind to the local address

    if (bind(servsock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0)
        die("bind failed");

    // Start listening for incoming connections

    if (listen(servsock, 5 /* queue size for connection requests */ ) < 0)
        die("listen failed");

    s->server_sock = servsock;

    return s;
}

void shutdown_app(struct app_state* s){
    close(s->mdb_sock);
    close(s->server_sock);
}

void process_dyn_request(struct app_state* s, int client_sock, char *logLine, char* requestURI){
    const char *form =
        "<h1>mdb-lookup</h1>\n"
        "<p>\n"
        "<form method=GET action=/mdb-lookup>\n"
        "lookup: <input type=text name=key>\n"
        "<input type=submit>\n"
        "</form>\n"
        "<p>\n";


    if (send(client_sock, MSG_200, strlen(MSG_200), 0) != strlen(MSG_200)){
        fprintf(stderr, "send failed\n");
        return;
    }
    if (send(client_sock, form, strlen(form), 0) != strlen(form)){
        fprintf(stderr, "send failed\n");
        return;
    }

    char *key_token = "?key=";
    char* key = strstr(requestURI, key_token);
    if(key == NULL){
        return;
    }

    key += strlen(key_token);
    char search[1000];
    fprintf(stderr,"looking up [%s]: ", key);
    fprintf(stderr, "%s 200 OK\n", logLine);
    snprintf(search, sizeof(search), "%s\n", key); 
    if (send(s->mdb_sock, search, strlen(search), 0) != strlen(search)){
        fprintf(stderr, "send failed\n");
        return;
    }   
    char message[100];
    char *table_top = 
        "<p>\n"
        "<table border=\"\">\n"
        "<tbody>\n";
    if (send(client_sock, table_top, strlen(table_top), 0) != strlen(table_top)){
        fprintf(stderr, "send failed\n");
        return;
    }

    int rowNum = 0;
    char rowBuf[200];
    while(fgets(message, sizeof(message), s->mdb_file) != NULL){
        if(message[0] == '\n'){
            break;
        }
        if(rowNum % 2 == 0){
            snprintf(rowBuf, sizeof(rowBuf), "<tr>\n<td>%s</td>\n</tr>\n", message);
        }else{
            snprintf(rowBuf, sizeof(rowBuf), "<tr>\n<td bgcolor=\"yellow\">%s</td>\n</tr>\n", message);
        }        
        if (send(client_sock, rowBuf, strlen(rowBuf), 0) != strlen(rowBuf)){
            fprintf(stderr, "send failed\n");
            return;
        }
        rowNum++;
    }
    char *table_bottom = 
        "</tbody>\n"
        "</table>\n"
        "</p>\n"
        "</body>\n"
        "</html>\n";
    if (send(client_sock, table_bottom, strlen(table_bottom), 0) != strlen(table_bottom)){
        fprintf(stderr, "send failed\n");
        return;
    }
}

void process_file_request(struct app_state* s, int client_sock, char *logLine, char* requestURI){
    int URIlen = strlen(requestURI);
   // fprintf(stderr, "requestURI:%s\n", requestURI);
    char fpath[1024];
    if(requestURI[URIlen -1] == '/'){
        snprintf(fpath, sizeof(fpath), "%s%sindex.html", s->web_root, requestURI);
     //   fprintf(stderr, "used index.html");
    }else{
        snprintf(fpath, sizeof(fpath), "%s%s", s->web_root, requestURI);
       // fprintf(stderr, "not index.html\n");
       // fprintf(stderr, "%s", s->web_root);
    }
   // fprintf(stderr, "Constructed file path: %s\n", fpath);
   // if(fpath[0] == '\0'){
     //   fprintf(stderr, "empty file path\n");
   // }

    struct stat statbuf;
   // int statOutput = stat(fpath, &statbuf);

   /* fprintf(stderr, "Constructed file path: %s\n", fpath);
    if(statOutput == -1) {
   switch(errno) {
   case EACCES:
       // Add code or at least 
       fprintf(stderr, "EACCES");
       break;

   case EIO:
       // ...
       fprintf(stderr, "EIO");
       break;

   case ELOOP:
       // ...
       fprintf(stderr, "ELOOP");
       break;
   case ENAMETOOLONG:
       // ...
       fprintf(stderr, "ENAMETOOLONG");
       // Do this to all possible errno's for the stat
       // ...
   case ENOENT:
       // ...
       fprintf(stderr, "ENOENT");
       break;
   case ENOTDIR:
       fprintf(stderr, "ENOTDIR");    
   }
}*/

   // fprintf(stderr, "%d", statOutput);
    if (stat(fpath, &statbuf) != 0){
        fprintf(stderr, "%s404\n", logLine);
        if (send(client_sock, MSG_404, strlen(MSG_404), 0) != strlen(MSG_404)){
            fprintf(stderr, "send failed\n");
        }
        return;
    }		

    if(S_ISDIR(statbuf.st_mode)){
        fprintf(stderr, "%s501\n", logLine);
        if (send(client_sock, MSG_501, strlen(MSG_501), 0) != strlen(MSG_501)){
            fprintf(stderr, "send failed\n");
        }
        return;
    }
    FILE *reqFile = fopen(fpath, "rb");
    if(reqFile == NULL){
        fprintf(stderr, "%s404 Not Found\n", logLine);
        if (send(client_sock, MSG_404, strlen(MSG_404), 0) != strlen(MSG_404)){
            fprintf(stderr, "send failed\n");
        }
        return;
    }

    fprintf(stderr, "%s200 OK\n", logLine);

    //send header 
    if (send(client_sock, MSG_200, strlen(MSG_200), 0) != strlen(MSG_200)){
        fprintf(stderr, "send failed\n");
        fclose(reqFile);
        return;
    }
    //fprintf(stderr, "header: %s", MSG_200);

    //send file 
    char buf[4096];
    int len; 
    while((len = fread(buf, 1, sizeof(buf), reqFile))>0){
  //      fprintf(stderr, "%s", buf);
        if(send(client_sock, buf, len, 0) != len){
            fprintf(stderr, "send failed\n"); 
            fclose(reqFile);
            return;
        }
    }
    fclose(reqFile);
}

void process_request(struct app_state* s,int client_sock, char *logLine, char* requestURI){

    char* dots = strstr(requestURI, "..");


    if(dots || requestURI[0] != '/'){
        fprintf(stderr, "%s400 Bad Request\n", logLine);
        if(send(client_sock, MSG_400, strlen(MSG_400), 0) != strlen(MSG_400)){
            fprintf(stderr, "send failed\n");
        }
        return;
    }

    char* dyn_path = "/mdb-lookup";
    int strncmpNum = strncmp(requestURI, dyn_path, strlen(dyn_path));
   // fprintf(stderr, "%d", strncmpNum);
    if(strncmpNum == 0){
        process_dyn_request(s, client_sock, logLine, requestURI);
    } else {
        process_file_request(s, client_sock, logLine, requestURI);
    }
} 

void accept_request(struct app_state* s){
    char request[2000];
    int r; 

    // Accept an incoming connection

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr); // initialize the in-out parameter
    int client_sock = accept(
            s->server_sock, 
            (struct sockaddr *) &client_addr,
            &client_len);

    if (client_sock < 0){
        fprintf(stderr, "accept failed\n");
        return;
    } 

    FILE *client = fdopen(client_sock, "r");
    if(client == NULL){
        fprintf(stderr, "No request recieved\n");
        close(client_sock);
        return;
    }

    if (fgets(request, sizeof(request), client) == NULL){
        fprintf(stderr, "No request recieved\n");
        return;
    }
   // fprintf(stderr, "original request: %s", request);
    char* client_ip = inet_ntoa(client_addr.sin_addr);
    char *token_separators = "\t \r\n"; // tab, space, new line
    char *method = strtok(request, token_separators);
    char *requestURI = strtok(NULL, token_separators);
    char *httpVersion = strtok(NULL, token_separators);
    
    char logLine[2048];
   snprintf(logLine, 
           sizeof(logLine), 
           "%s \"%s %s %s\"",
           client_ip,
           method,
           requestURI, 
           httpVersion);
                        
    char* is_get = strstr(method, "GET");
    int http10 = strcmp(httpVersion, "HTTP/1.0");
    int http11 = strcmp(httpVersion, "HTTP/1.1");

    if(!is_get || (http10 && http11)){
        r = send(client_sock, MSG_501, strlen(MSG_501), 0);
        if(r != strlen(MSG_501)){
            fprintf(stderr, "send failed\n");
        }
        fprintf(stderr, "%s 501 Not Implemented\n", logLine);
    } else {
        process_request(s, client_sock, logLine, requestURI); 
    }

    fclose(client);
}

int main(int argc, char* argv[]){
    if(argc != 5){
        fprintf(stderr, "usage: %s <server-port> <web-root> <mdb-lookup-host> <mdb-lookup-port>\r\n", argv[0]);
        exit(1);
    }


    int server_port = atoi(argv[1]);
    char* web_root = argv[2];
    char* mdb_host = argv[3];
    int mdb_port = atoi(argv[4]);

    struct app_state* app = initialize_app(server_port, web_root, mdb_host, mdb_port);
//    fprintf(stderr, "app initialized");

    while (1) {
        accept_request(app);
    }

    shutdown_app(app);
}


