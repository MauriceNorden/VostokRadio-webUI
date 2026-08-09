#ifndef VOSTOK_STRBUF
#define VOSTOK_STRBUF

//a growable text buffer, used to build json and http responses
//header only so both the parameter code and the web server can use it

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char* s;
  size_t len;
  size_t cap;
  int failed;
} strbuf;

static inline void sb_init(strbuf* b){
  b->cap=4096;
  b->len=0;
  b->failed=0;
  b->s=malloc(b->cap);
  if(b->s==NULL){
    b->cap=0;
    b->failed=1;
  }else{
    b->s[0]=0;
  }
}

static inline void sb_free(strbuf* b){
  free(b->s);
  b->s=NULL;
  b->len=0;
  b->cap=0;
}

static inline int sb_room(strbuf* b,size_t extra){
  if(b->failed)
    return 0;

  if(b->len+extra+1<=b->cap)
    return 1;

  size_t want=b->cap;
  if(want<4096)
    want=4096;

  while(want<b->len+extra+1)
    want=want*2;

  char* grown=realloc(b->s,want);
  if(grown==NULL){
    b->failed=1;
    return 0;
  }
  b->s=grown;
  b->cap=want;
  return 1;
}

static inline void sb_addn(strbuf* b,const char* data,size_t n){
  if(!sb_room(b,n))
    return;

  memcpy(b->s+b->len,data,n);
  b->len=b->len+n;
  b->s[b->len]=0;
}

static inline void sb_add(strbuf* b,const char* data){
  sb_addn(b,data,strlen(data));
}

static inline void sb_addc(strbuf* b,char c){
  sb_addn(b,&c,1);
}

static inline void sb_addf(strbuf* b,const char* fmt,...){
  va_list ap;
  va_list measure;
  va_start(ap,fmt);
  va_copy(measure,ap);
  int n=vsnprintf(NULL,0,fmt,measure);
  va_end(measure);

  if(n>0 && sb_room(b,(size_t)n)){
    vsnprintf(b->s+b->len,(size_t)n+1,fmt,ap);
    b->len=b->len+(size_t)n;
  }
  va_end(ap);
}

//write a json string, quotes included
static inline void sb_json(strbuf* b,const char* text){
  sb_addc(b,'"');
  if(text!=NULL){
    for(const char* p=text;*p!=0;p++){
      unsigned char c=(unsigned char)*p;
      if(c=='"' || c=='\\'){
        sb_addc(b,'\\');
        sb_addc(b,(char)c);
      }else if(c=='\n'){
        sb_add(b,"\\n");
      }else if(c=='\r'){
        sb_add(b,"\\r");
      }else if(c=='\t'){
        sb_add(b,"\\t");
      }else if(c<0x20){
        sb_addf(b,"\\u%04x",c);
      }else{
        sb_addc(b,(char)c);
      }
    }
  }
  sb_addc(b,'"');
}

//json has no infinity or nan, keep the output parseable no matter what the dsp did
static inline void sb_json_num(strbuf* b,double v){
  if(v!=v)
    sb_add(b,"0");
  else if(v>1e308)
    sb_add(b,"1e308");
  else if(v<-1e308)
    sb_add(b,"-1e308");
  else
    sb_addf(b,"%.10g",v);
}

#endif // !VOSTOK_STRBUF
