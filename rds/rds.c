#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "rds.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//Evan Nikitin 2025
//
//RDS baseband generator, following IEC 62106.
//
//  104 bits per group, 4 blocks of 16 data bits plus a 10 bit checkword.
//  The checkword is the remainder of the data times x^10 divided by
//  g(x) = x^10+x^8+x^7+x^5+x^4+x^3+1, with a per block offset word added.
//  The bit stream is differentially encoded and each bit becomes one biphase
//  symbol, shaped by the raised cosine filter the standard specifies.

#define GROUP_BITS 104
#define BIT_RATE_NUM 19000   //1187.5 bit/s = 19000/16, kept as integers so the
#define BIT_RATE_DEN 16      //bit clock cannot drift against the sample clock
#define RDS_MIN_RATE 152000  //57khz needs room under nyquist

//offset words, added to the checkword so a receiver can find block boundaries
#define OFFSET_A  0x0FCu
#define OFFSET_B  0x198u
#define OFFSET_C  0x168u
#define OFFSET_CP 0x350u
#define OFFSET_D  0x1B4u

#define AF_FILLER 205u  //"no information"
#define AF_NONE   224u  //"no alternative frequency exists"

//------------------------------------------------------------------- state

static int g_rate=0;
static int g_ready=0;

//settings, only touched by the audio thread
static int      cfg_enable=0;
static uint16_t cfg_pi=0;
static int      cfg_pty=0;
static int      cfg_tp=0;
static int      cfg_ta=0;
static int      cfg_ms=1;
static int      cfg_di=8;

static char cfg_ps[9]="        ";
static char cfg_rt[65];
static int  cfg_rt_groups=0;   //how many 2A groups carry the text
static int  cfg_rt_ab=0;       //toggles whenever the text changes

//alternative frequency bytes, sent two per 0A group
static uint8_t cfg_af[32];
static int cfg_af_bytes=2;
static int af_cursor=0;

//what we last configured from, to notice a change
static char last_ps[9]="";
static char last_rt[65]="";
static char last_af[RDS_AF_LEN]="";

//scheduler
static int ps_segment=0;
static int rt_segment=0;
static int slot=0;

//bit stream of the group being transmitted
static uint8_t bits[GROUP_BITS];
static int bit_pos=GROUP_BITS;

//bit clock, exact integer ratio against the sample rate
static long bit_acc=0;
static long bit_limit=0;

static int last_level=0;   //differential encoder state

//pulse shaping
static double* pulse=NULL;
static int pulse_len=0;
static double* acc=NULL;
static int acc_cursor=0;

//--------------------------------------------------------------- block maths

//remainder of data*x^10 divided by the generator, plus the offset word
static uint16_t checkword(uint16_t data,uint16_t offset){
  uint32_t reg=(uint32_t)data<<10;

  for(int i=25;i>=10;i--){
    if(reg & (1u<<i))
      reg^=(0x5B9u<<(i-10));
  }

  return (uint16_t)((reg & 0x3FFu) ^ offset);
}

static void emit_group(const uint16_t block[4],const uint16_t offset[4]){
  int n=0;
  for(int b=0;b<4;b++){
    uint16_t data=block[b];
    uint16_t check=checkword(data,offset[b]);

    for(int i=15;i>=0;i--)
      bits[n++]=(uint8_t)((data>>i)&1);

    for(int i=9;i>=0;i--)
      bits[n++]=(uint8_t)((check>>i)&1);
  }
  bit_pos=0;
}

//------------------------------------------------------------------- groups

//group 0A: PI, PTY, TA/TP, one DI bit, one AF pair and two characters of the
//programme service name
static void build_0a(int segment){
  uint16_t block[4];
  uint16_t offset[4]={OFFSET_A,OFFSET_B,OFFSET_C,OFFSET_D};

  int di_bit=(cfg_di>>(3-segment))&1;

  block[0]=cfg_pi;
  block[1]=(uint16_t)((0u<<12)          //group type 0
                     |(0u<<11)          //version A
                     |((cfg_tp&1)<<10)
                     |((cfg_pty&0x1F)<<5)
                     |((cfg_ta&1)<<4)
                     |((cfg_ms&1)<<3)
                     |((di_bit&1)<<2)
                     |(segment&3));

  block[2]=(uint16_t)((cfg_af[af_cursor]<<8)|cfg_af[af_cursor+1]);
  af_cursor+=2;
  if(af_cursor>=cfg_af_bytes)
    af_cursor=0;

  block[3]=(uint16_t)(((unsigned char)cfg_ps[segment*2]<<8)
                     |((unsigned char)cfg_ps[segment*2+1]));

  emit_group(block,offset);
}

//group 2A: four characters of RadioText
static void build_2a(int segment){
  uint16_t block[4];
  uint16_t offset[4]={OFFSET_A,OFFSET_B,OFFSET_C,OFFSET_D};

  block[0]=cfg_pi;
  block[1]=(uint16_t)((2u<<12)          //group type 2
                     |(0u<<11)          //version A
                     |((cfg_tp&1)<<10)
                     |((cfg_pty&0x1F)<<5)
                     |((cfg_rt_ab&1)<<4)
                     |(segment&0x0F));

  block[2]=(uint16_t)(((unsigned char)cfg_rt[segment*4]<<8)
                     |((unsigned char)cfg_rt[segment*4+1]));
  block[3]=(uint16_t)(((unsigned char)cfg_rt[segment*4+2]<<8)
                     |((unsigned char)cfg_rt[segment*4+3]));

  emit_group(block,offset);
}

//four 0A groups then four 2A groups, so the name refreshes about every 0.7s
static void build_next_group(void){
  int want_rt=(cfg_rt_groups>0 && (slot&4)!=0);

  if(want_rt){
    build_2a(rt_segment);
    rt_segment++;
    if(rt_segment>=cfg_rt_groups)
      rt_segment=0;
  }else{
    build_0a(ps_segment);
    ps_segment=(ps_segment+1)&3;
  }

  slot=(slot+1)&7;
}

//----------------------------------------------------------------- settings

//the RDS character set matches ascii over the printable range, anything else
//would show up as noise on a receiver
static char safe_char(char c){
  unsigned char u=(unsigned char)c;
  if(u<0x20 || u>0x7E)
    return ' ';
  return (char)u;
}

static void set_ps(const char* text){
  for(int i=0;i<8;i++)
    cfg_ps[i]=(text[i]!=0)?safe_char(text[i]):' ';

  cfg_ps[8]=0;

  //stop copying past the end of a short name
  size_t len=strlen(text);
  for(size_t i=len;i<8;i++)
    cfg_ps[i]=' ';
}

static void set_rt(const char* text){
  size_t len=strlen(text);
  if(len>64)
    len=64;

  memset(cfg_rt,' ',sizeof(cfg_rt));
  cfg_rt[64]=0;

  for(size_t i=0;i<len;i++)
    cfg_rt[i]=safe_char(text[i]);

  if(len==0){
    cfg_rt_groups=0;
    return;
  }

  //a carriage return tells the receiver the text ends here
  size_t used=len;
  if(used<64){
    cfg_rt[used]=0x0D;
    used++;
  }

  cfg_rt_groups=(int)((used+3)/4);
  if(cfg_rt_groups>16)
    cfg_rt_groups=16;
}

//"100.1, 103.5" turns into the byte sequence the standard wants
static void set_af(const char* list){
  int codes[25];
  int count=0;

  const char* p=list;
  while(*p!=0 && count<25){
    while(*p==' ' || *p==',' || *p=='\t')
      p++;

    if(*p==0)
      break;

    char* end=NULL;
    double mhz=strtod(p,&end);
    if(end==p){
      p++;
      continue;
    }
    p=end;

    int code=(int)((mhz-87.5)*10.0+0.5);
    if(code>=1 && code<=204){
      codes[count]=code;
      count++;
    }
  }

  memset(cfg_af,AF_FILLER,sizeof(cfg_af));

  if(count==0){
    cfg_af[0]=AF_NONE;
    cfg_af[1]=AF_FILLER;
    cfg_af_bytes=2;
  }else{
    cfg_af[0]=(uint8_t)(AF_NONE+count);   //"n alternative frequencies follow"
    for(int i=0;i<count;i++)
      cfg_af[1+i]=(uint8_t)codes[i];

    int used=1+count;
    if(used&1)
      used++;                              //pad the last pair with the filler

    cfg_af_bytes=used;
  }

  af_cursor=0;
}

void rds_configure(const rt_params* p){
  cfg_enable=p->rds_enable;
  cfg_pty=p->rds_pty;
  cfg_tp=p->rds_tp;
  cfg_ta=p->rds_ta;
  cfg_ms=p->rds_ms;
  cfg_di=p->rds_di;

  cfg_pi=(uint16_t)strtol(p->rds_pi,NULL,16);

  if(strcmp(last_ps,p->rds_ps)!=0){
    snprintf(last_ps,sizeof(last_ps),"%s",p->rds_ps);
    set_ps(p->rds_ps);
  }

  if(strcmp(last_rt,p->rds_rt)!=0){
    snprintf(last_rt,sizeof(last_rt),"%s",p->rds_rt);
    set_rt(p->rds_rt);
    //the A/B flag has to change so receivers clear the old text
    cfg_rt_ab=!cfg_rt_ab;
    rt_segment=0;
  }

  if(strcmp(last_af,p->rds_af)!=0){
    snprintf(last_af,sizeof(last_af),"%s",p->rds_af);
    set_af(p->rds_af);
  }
}

//------------------------------------------------------------ pulse shaping

//impulse response of the data shaping filter the standard defines,
//Ht(f) = cos(pi f / 4750) for |f| <= 2375 and nothing above that.
static double rc_impulse(double t){
  const double a=M_PI/4750.0;
  double b=2.0*M_PI*t;
  double d=a*a-b*b;

  //removable singularity at t = +-1/9500, the limit is 2375
  if(fabs(d)<a*a*1e-6)
    return 2375.0;

  return cos(4750.0*M_PI*t)*2.0*a/d;
}

//scale the pulse so a normal bit stream peaks near 1
static double measure_peak(void){
  double* sim=calloc((size_t)pulse_len,sizeof(double));
  if(sim==NULL)
    return 1.0;

  uint32_t rnd=12345;
  int cursor=0;
  int level=0;
  double peak=0;
  long clock=0;

  for(int n=0;n<pulse_len*40;n++){
    clock+=BIT_RATE_NUM;
    if(clock>=bit_limit){
      clock-=bit_limit;

      rnd=rnd*1103515245u+12345u;
      level^=(int)((rnd>>16)&1);
      double sign=level?1.0:-1.0;

      int at=cursor;
      for(int i=0;i<pulse_len;i++){
        sim[at]+=sign*pulse[i];
        at++;
        if(at>=pulse_len)
          at=0;
      }
    }

    double v=fabs(sim[cursor]);
    if(v>peak)
      peak=v;

    sim[cursor]=0;
    cursor++;
    if(cursor>=pulse_len)
      cursor=0;
  }

  free(sim);
  if(peak<1e-9)
    peak=1.0;

  return peak;
}

void rds_init(int sample_rate){
  rds_free();

  g_rate=sample_rate;
  if(sample_rate<RDS_MIN_RATE)
    return;

  //samples per bit = rate * 16 / 19000, kept exact with integer accumulation
  bit_limit=(long)sample_rate*BIT_RATE_DEN;
  bit_acc=0;

  double bit_period=(double)BIT_RATE_DEN/(double)BIT_RATE_NUM;

  //Four bit periods is far enough out for the tails to be negligible.
  //The length has to be odd: the symbol is an odd function about its centre,
  //so only a table with a real centre sample sums to exactly zero. An even
  //one is truncated a sample off centre, and the leftover DC would put a
  //carrier back at 57khz where the standard wants it suppressed.
  pulse_len=(int)(4.0*bit_period*sample_rate);
  if(pulse_len<8)
    pulse_len=8;
  if((pulse_len%2)==0)
    pulse_len++;

  pulse=malloc(sizeof(double)*(size_t)pulse_len);
  acc=calloc((size_t)pulse_len,sizeof(double));
  if(pulse==NULL || acc==NULL){
    rds_free();
    return;
  }

  //one biphase symbol: two opposite impulses half a bit apart, shaped
  double quarter=bit_period/4.0;
  int centre=(pulse_len-1)/2;
  for(int i=0;i<pulse_len;i++){
    double t=(double)(i-centre)/(double)sample_rate;
    pulse[i]=rc_impulse(t+quarter)-rc_impulse(t-quarter);
  }

  double peak=measure_peak();
  for(int i=0;i<pulse_len;i++)
    pulse[i]/=peak;

  memset(acc,0,sizeof(double)*(size_t)pulse_len);
  acc_cursor=0;
  bit_pos=GROUP_BITS;
  last_level=0;
  slot=0;
  ps_segment=0;
  rt_segment=0;

  g_ready=1;
}

void rds_free(void){
  free(pulse);
  free(acc);
  pulse=NULL;
  acc=NULL;
  pulse_len=0;
  g_ready=0;
}

//------------------------------------------------------------------ running

static void add_symbol(double sign){
  int at=acc_cursor;
  for(int i=0;i<pulse_len;i++){
    acc[at]+=sign*pulse[i];
    at++;
    if(at>=pulse_len)
      at=0;
  }
}

static void next_bit(void){
  if(bit_pos>=GROUP_BITS)
    build_next_group();

  int b=bits[bit_pos];
  bit_pos++;

  //differential encoding, so a receiver does not care about our polarity
  last_level^=b;
  add_symbol(last_level?1.0:-1.0);
}

double rds_next_sample(void){
  if(!g_ready || !cfg_enable)
    return 0.0;

  bit_acc+=BIT_RATE_NUM;
  if(bit_acc>=bit_limit){
    bit_acc-=bit_limit;
    next_bit();
  }

  double out=acc[acc_cursor];
  acc[acc_cursor]=0.0;
  acc_cursor++;
  if(acc_cursor>=pulse_len)
    acc_cursor=0;

  return out;
}

int rds_is_ready(void){
  return g_ready;
}

void rds_status(char* out,size_t len){
  if(!cfg_enable){
    snprintf(out,len,"off");
    return;
  }

  if(!g_ready){
    if(g_rate>0 && g_rate<RDS_MIN_RATE)
      snprintf(out,len,"needs a 192khz output rate, 57khz does not fit in %d",g_rate);
    else
      snprintf(out,len,"waiting for the MPX encoder");

    return;
  }

  char name[9];
  snprintf(name,sizeof(name),"%s",cfg_ps);
  for(int i=7;i>=0 && name[i]==' ';i--)
    name[i]=0;

  if(cfg_pi==0){
    snprintf(out,len,"sending, but the PI code is 0000. set a real one or receivers will ignore it");
    return;
  }

  if(cfg_rt_groups>0)
    snprintf(out,len,"sending PI %04X, PS '%s', RadioText over %d groups",cfg_pi,name,cfg_rt_groups);
  else
    snprintf(out,len,"sending PI %04X, PS '%s', no RadioText",cfg_pi,name);
}
