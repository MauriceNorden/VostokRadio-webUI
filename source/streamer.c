#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "streamer.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

//Evan Nikitin 2025, stream source supervisor

#define MAX_ARGS 32

static pid_t g_child=-1;
static int   g_state=SRC_OFF;
static char  g_text[192]="off";
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;

//what the running player was started with, so we notice a settings change
static char g_url[URL_LEN]="";
static char g_device[DEVICE_NAME_LEN]="";
static char g_player[PLAYER_NAME_LEN]="";
static int  g_cache=-1;
static int  g_rate=0;

static time_t g_started=0;
static time_t g_retry_at=0;
static int    g_fails=0;
static int    g_stopping=0;
static time_t g_kill_at=0;

//keeps the log readable, only complain about the same thing once
static char g_last_report[192]="";

static void set_status(int state,const char* fmt,...){
  char text[192];
  va_list ap;
  va_start(ap,fmt);
  vsnprintf(text,sizeof(text),fmt,ap);
  va_end(ap);

  pthread_mutex_lock(&g_lock);
  g_state=state;
  snprintf(g_text,sizeof(g_text),"%s",text);
  pthread_mutex_unlock(&g_lock);

  //straight into the meters, so the page sees it without waiting for the
  //next audio block to be published
  meters_set_source(state,text);

  if(strcmp(text,g_last_report)!=0){
    snprintf(g_last_report,sizeof(g_last_report),"%s",text);
    printf("stream: %s\n",text);
    fflush(stdout);
  }
}

int streamer_state(void){
  pthread_mutex_lock(&g_lock);
  int s=g_state;
  pthread_mutex_unlock(&g_lock);
  return s;
}

void streamer_text(char* out,size_t len){
  pthread_mutex_lock(&g_lock);
  snprintf(out,len,"%s",g_text);
  pthread_mutex_unlock(&g_lock);
}

//------------------------------------------------------------------ checking

//the url ends up as one argv entry, never in a shell, but a value starting
//with a dash would still be read as an option by the player
static int url_is_safe(const char* url){
  if(strncmp(url,"http://",7)!=0 && strncmp(url,"https://",8)!=0)
    return 0;

  for(const char* p=url;*p!=0;p++){
    unsigned char c=(unsigned char)*p;
    if(c<=0x20 || c==0x7f)
      return 0;
  }
  return 1;
}

static int device_is_safe(const char* device){
  if(device[0]==0 || device[0]=='-')
    return 0;

  for(const char* p=device;*p!=0;p++){
    unsigned char c=(unsigned char)*p;
    if(c<0x20 || c==0x7f)
      return 0;
  }
  return 1;
}

//The path is resolved here rather than by execvp in the child. execvp can
//allocate while searching PATH, and allocating after fork() in a process with
//threads can deadlock on the malloc lock. execv does not allocate.
static int resolve_program(const char* name,char* out,size_t outlen){
  const char* path=getenv("PATH");
  if(path==NULL)
    path="/usr/local/bin:/usr/bin:/bin";

  const char* start=path;
  while(*start!=0){
    const char* end=strchr(start,':');
    if(end==NULL)
      end=start+strlen(start);

    size_t len=(size_t)(end-start);
    if(len>0 && len+strlen(name)+2<outlen){
      memcpy(out,start,len);
      out[len]='/';
      snprintf(out+len+1,outlen-len-1,"%s",name);
      if(access(out,X_OK)==0)
        return 1;
    }

    start=(*end!=0)?end+1:end;
  }

  out[0]=0;
  return 0;
}

static const char* pick_player(const char* wanted,char* path,size_t pathlen){
  if(strcmp(wanted,"auto")!=0){
    if(resolve_program(wanted,path,pathlen))
      return wanted;
    return NULL;
  }

  //mpv first, it is the only one of the three that buffers the stream
  if(resolve_program("mpv",path,pathlen))
    return "mpv";
  if(resolve_program("ffmpeg",path,pathlen))
    return "ffmpeg";
  if(resolve_program("mpg123",path,pathlen))
    return "mpg123";

  return NULL;
}

//------------------------------------------------------------------ starting

//scratch space for the strings argv points at
static char a_device[DEVICE_NAME_LEN+32];
static char a_rate[16];
static char a_cache[32];
static char a_readahead[48];

static int build_argv(const char* player,const rt_params* p,const char* url,char** argv){
  int n=0;
  snprintf(a_rate,sizeof(a_rate),"%d",p->io_input_rate);

  if(strcmp(player,"mpv")==0){
    snprintf(a_device,sizeof(a_device),"--audio-device=alsa/%s",p->source_device);
    snprintf(a_cache,sizeof(a_cache),"--cache-secs=%d",p->source_cache_secs);
    snprintf(a_readahead,sizeof(a_readahead),"--demuxer-readahead-secs=%d",p->source_cache_secs);

    argv[n++]="mpv";
    argv[n++]="--no-video";
    argv[n++]="--no-terminal";
    argv[n++]=a_device;
    argv[n++]="--audio-format=s32";
    argv[n++]="--audio-channels=stereo";
    argv[n++]="--audio-samplerate";
    argv[n++]=a_rate;
    argv[n++]="--cache=yes";
    argv[n++]=a_cache;
    argv[n++]=a_readahead;
    argv[n++]="--stream-lavf-o=reconnect=1,reconnect_streamed=1,reconnect_delay_max=10";
    argv[n++]="--loop=inf";
    argv[n++]=(char*)url;
    argv[n]=NULL;
    return n;
  }

  if(strcmp(player,"ffmpeg")==0){
    snprintf(a_device,sizeof(a_device),"%s",p->source_device);

    argv[n++]="ffmpeg";
    argv[n++]="-hide_banner";
    argv[n++]="-loglevel";
    argv[n++]="warning";
    argv[n++]="-nostdin";
    argv[n++]="-reconnect";
    argv[n++]="1";
    argv[n++]="-reconnect_streamed";
    argv[n++]="1";
    argv[n++]="-reconnect_delay_max";
    argv[n++]="10";
    argv[n++]="-i";
    argv[n++]=(char*)url;
    argv[n++]="-ar";
    argv[n++]=a_rate;
    argv[n++]="-ac";
    argv[n++]="2";
    argv[n++]="-c:a";
    argv[n++]="pcm_s32le";
    argv[n++]="-f";
    argv[n++]="alsa";
    argv[n++]=a_device;
    argv[n]=NULL;
    return n;
  }

  //mpg123
  snprintf(a_device,sizeof(a_device),"%s",p->source_device);

  argv[n++]="mpg123";
  argv[n++]="-q";
  argv[n++]="--rate";
  argv[n++]=a_rate;
  argv[n++]="--stereo";
  argv[n++]="-e";
  argv[n++]="s32";
  argv[n++]="-o";
  argv[n++]="alsa";
  argv[n++]="-a";
  argv[n++]=a_device;
  argv[n++]=(char*)url;
  argv[n]=NULL;
  return n;
}

//record what this attempt was made with, so a later edit is noticed even when
//the attempt never got as far as a running process
static void remember(const rt_params* p,const char* player){
  snprintf(g_url,sizeof(g_url),"%s",p->source_url);
  snprintf(g_device,sizeof(g_device),"%s",p->source_device);
  snprintf(g_player,sizeof(g_player),"%s",player);
  g_cache=p->source_cache_secs;
  g_rate=p->io_input_rate;
}

static void start_player(const rt_params* p){
  remember(p,"");

  if(!url_is_safe(p->source_url)){
    if(p->source_url[0]==0)
      set_status(SRC_FAILED,"no stream url set");
    else
      set_status(SRC_FAILED,"the url must start with http:// or https:// and hold no spaces");

    g_retry_at=time(NULL)+30;
    return;
  }

  if(!device_is_safe(p->source_device)){
    set_status(SRC_FAILED,"the player output device is not a usable name");
    g_retry_at=time(NULL)+30;
    return;
  }

  char program[512];
  const char* player=pick_player(p->source_player,program,sizeof(program));
  if(player==NULL){
    if(strcmp(p->source_player,"auto")==0)
      set_status(SRC_FAILED,"no player installed, try: sudo apt install mpv");
    else
      set_status(SRC_FAILED,"'%s' is not installed",p->source_player);

    g_retry_at=time(NULL)+30;
    return;
  }

  char* argv[MAX_ARGS];
  build_argv(player,p,p->source_url,argv);

  pid_t pid=fork();
  if(pid<0){
    set_status(SRC_FAILED,"cannot fork a player: %s",strerror(errno));
    g_retry_at=time(NULL)+5;
    return;
  }

  if(pid==0){
    //child
    #ifdef PR_SET_PDEATHSIG
      prctl(PR_SET_PDEATHSIG,SIGTERM);
    #endif
    signal(SIGPIPE,SIG_DFL);
    signal(SIGINT,SIG_DFL);
    signal(SIGTERM,SIG_DFL);

    int devnull=open("/dev/null",O_RDWR);
    if(devnull>=0){
      dup2(devnull,STDIN_FILENO);
      dup2(devnull,STDOUT_FILENO);
      if(devnull>2)
        close(devnull);
    }
    //stderr is left alone so player errors show up in the log

    execv(program,argv);
    _exit(127);
  }

  g_child=pid;
  g_started=time(NULL);
  g_stopping=0;
  snprintf(g_player,sizeof(g_player),"%s",player);
  set_status(SRC_STARTING,"starting %s",player);
}

static void signal_stop(void){
  if(g_child<=0)
    return;

  kill(g_child,SIGTERM);
  g_stopping=1;
  g_kill_at=time(NULL)+3;
}

static int settings_changed(const rt_params* p){
  if(strcmp(g_url,p->source_url)!=0)
    return 1;
  if(strcmp(g_device,p->source_device)!=0)
    return 1;
  if(g_cache!=p->source_cache_secs)
    return 1;
  if(g_rate!=p->io_input_rate)
    return 1;

  //"auto" resolves to a concrete player, only react to a real change
  if(strcmp(p->source_player,"auto")!=0 && strcmp(g_player,p->source_player)!=0)
    return 1;

  return 0;
}

//a failed attempt backs off for up to 30 seconds. if the user just corrected
//the url they should not have to sit through the rest of that wait.
static void retry_now_if_edited(const rt_params* p){
  if(g_retry_at==0 || !settings_changed(p))
    return;

  g_retry_at=0;
  g_fails=0;
}

//--------------------------------------------------------------------- poll

void streamer_poll(const rt_params* p){
  time_t now=time(NULL);

  //make sure the page has something to show before anything has happened
  static int announced=0;
  if(!announced){
    announced=1;
    set_status(SRC_OFF,p->source_enable?"starting up":"off");
  }

  //collect the player if it has gone
  if(g_child>0){
    int status=0;
    pid_t done=waitpid(g_child,&status,WNOHANG);
    if(done==g_child || (done<0 && errno==ECHILD)){
      long lived=(long)(now-g_started);
      g_child=-1;

      if(g_stopping){
        g_stopping=0;
        set_status(SRC_OFF,"off");
      }else if(p->source_enable){
        if(lived<3)
          g_fails++;
        else
          g_fails=0;

        int delay=2+g_fails*3;
        if(delay>30)
          delay=30;

        g_retry_at=now+delay;

        if(WIFEXITED(status) && WEXITSTATUS(status)==127)
          set_status(SRC_FAILED,"could not run %s, is it installed?",g_player);
        else if(WIFEXITED(status))
          set_status(SRC_FAILED,"%s stopped (code %d), retrying in %ds",g_player,WEXITSTATUS(status),delay);
        else
          set_status(SRC_FAILED,"%s was killed, retrying in %ds",g_player,delay);
      }else{
        set_status(SRC_OFF,"off");
      }
    }else if(g_stopping && now>=g_kill_at){
      kill(g_child,SIGKILL);
      g_kill_at=now+3;
    }
  }

  if(!p->source_enable){
    if(g_child>0 && !g_stopping)
      signal_stop();
    else if(g_child<=0 && g_state!=SRC_OFF)
      set_status(SRC_OFF,"off");

    g_fails=0;
    g_retry_at=0;
    return;
  }

  //url or device edited while playing, swap the player over
  if(g_child>0 && !g_stopping && settings_changed(p)){
    set_status(SRC_STARTING,"settings changed, restarting the player");
    g_fails=0;
    g_retry_at=0;
    signal_stop();
    return;
  }

  if(g_child>0){
    if(!g_stopping && g_state==SRC_STARTING && now-g_started>=2)
      set_status(SRC_PLAYING,"playing with %s",g_player);

    return;
  }

  retry_now_if_edited(p);

  if(now>=g_retry_at)
    start_player(p);
}

void streamer_stop(void){
  if(g_child<=0)
    return;

  kill(g_child,SIGTERM);
  for(int i=0;i<30;i++){
    int status=0;
    pid_t done=waitpid(g_child,&status,WNOHANG);
    if(done==g_child || (done<0 && errno==ECHILD))
      break;

    usleep(100000);
    if(i==20)
      kill(g_child,SIGKILL);
  }
  g_child=-1;
  set_status(SRC_OFF,"off");
}
