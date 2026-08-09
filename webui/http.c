#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "http.h"
#include "../config/params.h"
#include "../config/strbuf.h"
#include "page.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

//Evan Nikitin 2025, control surface for the processor

#define MAX_REQUEST (256*1024)

static int g_listen_fd=-1;
static pthread_t g_thread;
static int g_thread_running=0;
static volatile int g_stop=0;
static volatile int g_restart=0;

int webui_restart_requested(void){
  return g_restart;
}

//------------------------------------------------------------------ helpers

static int write_all(int fd,const char* data,size_t len){
  size_t sent=0;
  while(sent<len){
    ssize_t n=send(fd,data+sent,len-sent,MSG_NOSIGNAL);
    if(n<=0){
      if(n<0 && errno==EINTR)
        continue;
      return -1;
    }
    sent=sent+(size_t)n;
  }
  return 0;
}

static void send_response(int fd,const char* status,const char* type,const char* body,size_t len){
  char head[512];
  int n=snprintf(head,sizeof(head),
      "HTTP/1.1 %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n"
      "\r\n",
      status,type,len);

  if(n<0)
    return;

  if(write_all(fd,head,(size_t)n)==0 && len>0)
    write_all(fd,body,len);
}

static void send_text(int fd,const char* status,const char* text){
  send_response(fd,status,"text/plain; charset=utf-8",text,strlen(text));
}

//takes ownership of json and frees it
static void send_json(int fd,char* json){
  if(json==NULL){
    send_text(fd,"500 Internal Server Error","out of memory\n");
    return;
  }
  send_response(fd,"200 OK","application/json",json,strlen(json));
  free(json);
}

static int hex_digit(char c){
  if(c>='0' && c<='9')
    return c-'0';
  if(c>='a' && c<='f')
    return c-'a'+10;
  if(c>='A' && c<='F')
    return c-'A'+10;
  return -1;
}

//in place url decoding
static void url_decode(char* s){
  char* out=s;
  for(char* in=s;*in!=0;in++){
    if(*in=='+'){
      *out=' ';
      out++;
    }else if(*in=='%' && in[1]!=0 && in[2]!=0){
      int hi=hex_digit(in[1]);
      int lo=hex_digit(in[2]);
      if(hi>=0 && lo>=0){
        *out=(char)((hi<<4)|lo);
        out++;
        in=in+2;
      }else{
        *out=*in;
        out++;
      }
    }else{
      *out=*in;
      out++;
    }
  }
  *out=0;
}

//------------------------------------------------------------------- routes

//body is "name=value&other=value", values are url encoded
static void handle_set(int fd,char* body){
  int ok=0;
  int unknown=0;
  int bad=0;
  char first_bad[160];
  first_bad[0]=0;

  char* save=NULL;
  for(char* pair=strtok_r(body,"&",&save);pair!=NULL;pair=strtok_r(NULL,"&",&save)){
    char* eq=strchr(pair,'=');
    if(eq==NULL)
      continue;

    *eq=0;
    char* key=pair;
    char* value=eq+1;
    url_decode(key);
    url_decode(value);

    int rc=params_set(key,value);
    if(rc==1){
      ok++;
    }else{
      if(first_bad[0]==0)
        snprintf(first_bad,sizeof(first_bad),"%s",key);

      if(rc==0)
        unknown++;
      else
        bad++;
    }
  }

  strbuf b;
  sb_init(&b);
  sb_addf(&b,"{\"applied\":%d,\"unknown\":%d,\"invalid\":%d,\"first_error\":",ok,unknown,bad);
  sb_json(&b,first_bad);
  sb_addc(&b,'}');

  if(b.failed){
    sb_free(&b);
    send_text(fd,"500 Internal Server Error","out of memory\n");
    return;
  }
  send_response(fd,"200 OK","application/json",b.s,b.len);
  sb_free(&b);
}

static void handle_save(int fd){
  const char* path=params_config_path();
  if(params_save_file(path)<0){
    char msg[600];
    snprintf(msg,sizeof(msg),"{\"ok\":false,\"path\":\"%s\",\"error\":\"%s\"}",path,strerror(errno));
    send_response(fd,"500 Internal Server Error","application/json",msg,strlen(msg));
    return;
  }
  char msg[600];
  snprintf(msg,sizeof(msg),"{\"ok\":true,\"path\":\"%s\"}",path);
  send_response(fd,"200 OK","application/json",msg,strlen(msg));
}

static void handle_load(int fd){
  const char* path=params_config_path();
  int n=params_load_file(path);
  if(n<0){
    char msg[600];
    snprintf(msg,sizeof(msg),"{\"ok\":false,\"path\":\"%s\",\"error\":\"%s\"}",path,strerror(errno));
    send_response(fd,"404 Not Found","application/json",msg,strlen(msg));
    return;
  }
  char msg[600];
  snprintf(msg,sizeof(msg),"{\"ok\":true,\"path\":\"%s\",\"applied\":%d}",path,n);
  send_response(fd,"200 OK","application/json",msg,strlen(msg));
}

static void handle_restart(int fd){
  //settings that need a restart only live in the config file, so save first
  const char* path=params_config_path();
  int saved=(params_save_file(path)==0)?1:0;
  g_restart=1;

  char msg[600];
  snprintf(msg,sizeof(msg),"{\"ok\":true,\"saved\":%s,\"path\":\"%s\"}",saved?"true":"false",path);
  send_response(fd,"200 OK","application/json",msg,strlen(msg));
}

static void route(int fd,const char* method,char* path,char* body){
  //strip the query string, nothing here needs it
  char* q=strchr(path,'?');
  if(q!=NULL)
    *q=0;

  int is_get=(strcmp(method,"GET")==0);
  int is_post=(strcmp(method,"POST")==0);

  if(is_get && (strcmp(path,"/")==0 || strcmp(path,"/index.html")==0)){
    send_response(fd,"200 OK","text/html; charset=utf-8",WEBUI_INDEX_HTML,strlen(WEBUI_INDEX_HTML));
    return;
  }
  if(is_get && strcmp(path,"/api/schema")==0){
    send_json(fd,params_schema_json());
    return;
  }
  if(is_get && strcmp(path,"/api/state")==0){
    send_json(fd,params_values_json());
    return;
  }
  if(is_get && strcmp(path,"/api/meters")==0){
    send_json(fd,meters_json());
    return;
  }
  if(is_post && strcmp(path,"/api/set")==0){
    handle_set(fd,body);
    return;
  }
  if(is_post && strcmp(path,"/api/save")==0){
    handle_save(fd);
    return;
  }
  if(is_post && strcmp(path,"/api/load")==0){
    handle_load(fd);
    return;
  }
  if(is_post && strcmp(path,"/api/restart")==0){
    handle_restart(fd);
    return;
  }

  send_text(fd,"404 Not Found","not found\n");
}

//--------------------------------------------------------------- connection

static void serve(int fd){
  struct timeval tv;
  tv.tv_sec=5;
  tv.tv_usec=0;
  setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
  setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));

  char* buf=malloc(MAX_REQUEST+1);
  if(buf==NULL)
    return;

  size_t have=0;
  char* header_end=NULL;

  //read until the end of the headers
  while(have<MAX_REQUEST){
    ssize_t n=recv(fd,buf+have,MAX_REQUEST-have,0);
    if(n<=0){
      if(n<0 && errno==EINTR)
        continue;
      break;
    }
    have=have+(size_t)n;
    buf[have]=0;
    header_end=strstr(buf,"\r\n\r\n");
    if(header_end!=NULL)
      break;
  }

  if(header_end==NULL){
    free(buf);
    return;
  }

  size_t header_len=(size_t)(header_end-buf)+4;

  //content length, so we know how much body is still coming
  size_t want=0;
  for(char* line=buf;line<header_end;){
    char* eol=strstr(line,"\r\n");
    if(eol==NULL || eol>header_end)
      break;

    if(strncasecmp(line,"Content-Length:",15)==0)
      want=(size_t)strtoul(line+15,NULL,10);

    line=eol+2;
  }

  if(want>MAX_REQUEST-header_len)
    want=MAX_REQUEST-header_len;

  while(have<header_len+want){
    ssize_t n=recv(fd,buf+have,header_len+want-have,0);
    if(n<=0){
      if(n<0 && errno==EINTR)
        continue;
      break;
    }
    have=have+(size_t)n;
  }
  buf[have]=0;

  //request line
  char method[16];
  char path[512];
  const char* sp1=strchr(buf,' ');
  if(sp1==NULL){
    free(buf);
    return;
  }
  size_t mlen=(size_t)(sp1-buf);
  if(mlen>=sizeof(method))
    mlen=sizeof(method)-1;

  memcpy(method,buf,mlen);
  method[mlen]=0;

  const char* sp2=strchr(sp1+1,' ');
  if(sp2==NULL){
    free(buf);
    return;
  }
  size_t plen=(size_t)(sp2-sp1-1);
  if(plen>=sizeof(path))
    plen=sizeof(path)-1;

  memcpy(path,sp1+1,plen);
  path[plen]=0;

  char* body=buf+header_len;
  route(fd,method,path,body);
  free(buf);
}

static void* server_thread(void* arg){
  (void)arg;
  while(!g_stop){
    struct sockaddr_in peer;
    socklen_t plen=sizeof(peer);
    int fd=accept(g_listen_fd,(struct sockaddr*)&peer,&plen);
    if(fd<0){
      if(errno==EINTR)
        continue;
      if(g_stop)
        break;

      usleep(20000);
      continue;
    }

    int one=1;
    setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
    serve(fd);
    shutdown(fd,SHUT_RDWR);
    close(fd);
  }
  return NULL;
}

//--------------------------------------------------------------- life cycle

//headless machines make you guess the address, so just print it
static void print_urls(const char* bind_addr,int port){
  int any=(bind_addr==NULL || strcmp(bind_addr,"0.0.0.0")==0 || strcmp(bind_addr,"*")==0);

  if(!any){
    printf("web ui: http://%s:%d/\n",bind_addr,port);
  }else{
    printf("web ui: http://localhost:%d/\n",port);

    struct ifaddrs* list=NULL;
    if(getifaddrs(&list)==0){
      for(struct ifaddrs* i=list;i!=NULL;i=i->ifa_next){
        if(i->ifa_addr==NULL || i->ifa_addr->sa_family!=AF_INET)
          continue;

        struct sockaddr_in* a=(struct sockaddr_in*)i->ifa_addr;
        if(ntohl(a->sin_addr.s_addr)==INADDR_LOOPBACK)
          continue;

        char text[INET_ADDRSTRLEN];
        if(inet_ntop(AF_INET,&a->sin_addr,text,sizeof(text))!=NULL)
          printf("        http://%s:%d/  (%s)\n",text,port,i->ifa_name);
      }
      freeifaddrs(list);
    }
  }
  printf("        there is no password, keep this off the open internet\n");
}

int webui_start(const char* bind_addr,int port){
  g_listen_fd=socket(AF_INET,SOCK_STREAM,0);
  if(g_listen_fd<0){
    fprintf(stderr,"web ui: socket failed: %s\n",strerror(errno));
    return -1;
  }

  int one=1;
  setsockopt(g_listen_fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));

  struct sockaddr_in addr;
  memset(&addr,0,sizeof(addr));
  addr.sin_family=AF_INET;
  addr.sin_port=htons((unsigned short)port);

  if(bind_addr==NULL || strcmp(bind_addr,"0.0.0.0")==0 || strcmp(bind_addr,"*")==0){
    addr.sin_addr.s_addr=htonl(INADDR_ANY);
  }else if(inet_pton(AF_INET,bind_addr,&addr.sin_addr)!=1){
    fprintf(stderr,"web ui: '%s' is not an ipv4 address\n",bind_addr);
    close(g_listen_fd);
    g_listen_fd=-1;
    return -1;
  }

  if(bind(g_listen_fd,(struct sockaddr*)&addr,sizeof(addr))<0){
    fprintf(stderr,"web ui: cannot bind %s:%d: %s\n",bind_addr,port,strerror(errno));
    close(g_listen_fd);
    g_listen_fd=-1;
    return -1;
  }

  if(listen(g_listen_fd,16)<0){
    fprintf(stderr,"web ui: listen failed: %s\n",strerror(errno));
    close(g_listen_fd);
    g_listen_fd=-1;
    return -1;
  }

  g_stop=0;
  if(pthread_create(&g_thread,NULL,&server_thread,NULL)!=0){
    fprintf(stderr,"web ui: cannot start the server thread\n");
    close(g_listen_fd);
    g_listen_fd=-1;
    return -1;
  }
  g_thread_running=1;
  print_urls(bind_addr,port);
  return 1;
}

void webui_stop(void){
  g_stop=1;
  if(g_listen_fd>=0){
    shutdown(g_listen_fd,SHUT_RDWR);
    close(g_listen_fd);
    g_listen_fd=-1;
  }
  if(g_thread_running){
    pthread_join(g_thread,NULL);
    g_thread_running=0;
  }
}
