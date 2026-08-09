#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "alsa_pipe/alsa_pipe.h"
#include "config/params.h"
#include "agc/agc.c"
#include "stereo_demux.c"
#include "./MPX/generator.h"
#include "./clippers/sin_clip.c"
#include "./sigmoidal_composite.c"
#include "./stereo/stereo_amp.c"
#include "./multiband_compressor/mbc.h"
#include "./lookahead_limiter/lookaheadlim.h"
#include "./downward_expander/dxpander.h"
#include "webui/http.h"
#include "source/streamer.h"
#include "rds/rds.h"
#include "ui.c"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

//Evan Nikitin 2025
//
//Every setting used to be a #define in DEFAULTS.h. They now live in a runtime
//struct (config/params.h) that the web ui writes to. This thread reads a
//private copy of it once per audio buffer, so nothing here has to lock.

typedef struct {
  fmux lmux;
  fmux rmux;
  fmux mmux;

  Multiband lmbt;
  Multiband rmbt;
  Multiband mmbt;

  afilter lpassfinal;
  afilter rpassfinal;

  afilter lbassc;   //agc sidechain high pass
  afilter rbassc;
  afilter lbassc2;  //bass cut after the compressor
  afilter rbassc2;
  afilter lbassc3;  //bass cut before the agc
  afilter rbassc3;

  AGC agc_left;
  AGC agc_right;

  Dexpander dxl;
  Dexpander dxr;

  SLim sigmoidal;

  int nbands;
} dsp_chain;

static volatile sig_atomic_t quit_requested=0;
static char** saved_argv=NULL;

static void on_signal(int sig){
  (void)sig;
  quit_requested=1;
}

//---------------------------------------------------- mono compression glue

//the mono path needs the left mux from inside the compressor callback
static fmux mono_lmux=NULL;
static double* mono_pvals=NULL;
static double* mono_gains=NULL;
static const int* mono_mix_stereo=NULL;

double h_compressor_left(double signal,double gain,int location){
  mono_gains[location]=gain;
  double amplitude=power_at(mono_lmux,location)/2;
  mono_pvals[location]=signal*gain;
  if(mono_mix_stereo[location]==1){
    set_power_at(mono_lmux,location, mono_pvals[location]);
  }else{
    set_power_at(mono_lmux,location, amplitude*gain);
  }
  return amplitude;
}

//---------------------------------------------------------------- dsp setup

//push everything that can change without reallocating anything
static void dsp_apply_live(dsp_chain* d,const rt_params* p){
  Multiband chain[3];
  chain[0]=d->lmbt;
  chain[1]=d->rmbt;
  chain[2]=d->mmbt;

  for(int i=0;i<d->nbands;i++){
    double attack=p->band_attack[i];
    double release=p->band_release[i];
    double ratio=p->band_ratio[i];

    if(attack<0.000000001)
      attack=0.000000001;
    if(release<0.000000001)
      release=0.000000001;
    if(ratio<0.000000001)
      ratio=0.000000001;

    //same conversion the old set_compressor_defaults did
    float attack_co=(float)(48000.0/(attack*48000000.0));
    float release_co=(float)(48000.0/(release*48000000.0));

    for(int c=0;c<3;c++){
      Multiband m=chain[c];
      set_attack(m,i,attack_co);
      set_release(m,i,release_co);
      set_target(m,i,p->band_target[i]);
      set_gate(m,i,p->band_gate[i]);
      set_max_gain(m,i,p->band_makeup[i]);
      set_post_amp(m,i,p->band_post_amp[i]);
      set_bypass(m,i,p->band_bypass[i]);
      set_ratio(m,i,1.0-(1.0/ratio));
      set_knee(m,i,p->band_knee[i]);
      set_dknee(m,i,p->band_knee_release[i]);
    }
  }

  d->agc_left->gain_max=p->agc_gain_max;
  d->agc_right->gain_max=p->agc_gain_max;

  d->dxl->attack=p->exp_attack;
  d->dxl->release=p->exp_release;
  d->dxl->ratio=p->exp_ratio;
  d->dxl->threshold=p->exp_threshold;

  d->dxr->attack=p->exp_attack;
  d->dxr->release=p->exp_release;
  d->dxr->ratio=p->exp_ratio;
  d->dxr->threshold=p->exp_threshold;

  //create_sigmoidal_limiter scales attack and release by the ratio
  d->sigmoidal->ratio=p->clip_ratio;
  d->sigmoidal->limit=31767+p->clip_drange;
  d->sigmoidal->range=p->clip_pre;
  d->sigmoidal->attack=p->clip_attack*p->clip_ratio;
  d->sigmoidal->release=p->clip_release*p->clip_ratio;
  d->sigmoidal->knee=p->clip_knee;

  //mpx scalars, none of these touch the wave cache.
  //written here rather than from the web thread so the composite clipper only
  //ever sees a value the audio thread put there.
  vostok_pilot_percent=p->mpx_percent_pilot;
  set_mpx_levels(p->mpx_percent_pilot,p->mpx_composite_limit);
  set_mpx_dac_harmonic(p->mpx_dac_2nd_harmonic);
  set_mpx_harmonic_dist(p->mpx_comp_dist);
  set_rds_level(p->rds_enable?p->rds_level:0.0);
  rds_configure(p);

  mono_mix_stereo=p->band_mix_stereo;
}

static void dsp_build(dsp_chain* d,const rt_params* p,int rate){
  int n=p->nbands;
  if(n<1)
    n=1;
  if(n>MAX_BANDS)
    n=MAX_BANDS;

  int freqs[MAX_BANDS];
  int lookahead[MAX_BANDS];
  for(int i=0;i<n;i++){
    freqs[i]=p->band_freq[i];
    lookahead[i]=(p->band_lookahead[i]<1)?1:p->band_lookahead[i];
  }

  int poles=(p->band_poles<1)?1:p->band_poles;
  d->nbands=n;

  d->lmux=create_fmux_from_pre(poles,rate,freqs,n);
  d->rmux=create_fmux_from_pre(poles,rate,freqs,n);
  d->mmux=create_fmux_from_pre(poles,rate,freqs,n);

  d->lmbt=create_mbt(d->lmux,lookahead);
  d->rmbt=create_mbt(d->rmux,lookahead);
  d->mmbt=create_mbt(d->mmux,lookahead);

  int lp_poles=(p->chain_lowpass_poles<1)?1:p->chain_lowpass_poles;
  d->lpassfinal=poled_f(rate,p->chain_lowpass_cutoff,lp_poles,0);
  d->rpassfinal=poled_f(rate,p->chain_lowpass_cutoff,lp_poles,0);

  int sc_poles=(p->agc_sidechain_poles<1)?1:p->agc_sidechain_poles;
  d->lbassc=poled_f(rate,p->agc_sidechain_cutoff,sc_poles,1);
  d->rbassc=poled_f(rate,p->agc_sidechain_cutoff,sc_poles,1);

  int hp_poles=(p->chain_highpass_poles<1)?1:p->chain_highpass_poles;
  d->lbassc2=poled_f(rate,p->chain_highpass_cutoff,hp_poles,1);
  d->rbassc2=poled_f(rate,p->chain_highpass_cutoff,hp_poles,1);
  d->lbassc3=poled_f(rate,p->chain_highpass_cutoff,hp_poles,1);
  d->rbassc3=poled_f(rate,p->chain_highpass_cutoff,hp_poles,1);

  int agc_look=(p->agc_lookahead<1)?1:p->agc_lookahead;
  //the third argument is stored but never read by the agc
  d->agc_left=create_agc(p->agc_gain_max,p->agc_gain_start,15,agc_look);
  d->agc_right=create_agc(p->agc_gain_max,p->agc_gain_start,15,agc_look);

  d->dxl=create_downward_expander(p->exp_attack,p->exp_release,p->exp_ratio,p->exp_threshold);
  d->dxr=create_downward_expander(p->exp_attack,p->exp_release,p->exp_ratio,p->exp_threshold);

  int clip_buffer=(p->clip_buffer<1)?1:p->clip_buffer;
  d->sigmoidal=create_sigmoidal_limiter(clip_buffer,p->clip_ratio,31767+p->clip_drange,
      p->clip_pre,p->clip_attack,p->clip_release,p->clip_knee,0,0,0);

  mono_lmux=d->lmux;
  dsp_apply_live(d,p);
}

static void dsp_free(dsp_chain* d){
  //free_multiband also frees the mux it was built on
  free_multiband(d->lmbt);
  free_multiband(d->rmbt);
  free_multiband(d->mmbt);

  free_f(d->lpassfinal);
  free_f(d->rpassfinal);
  free_f(d->lbassc);
  free_f(d->rbassc);
  free_f(d->lbassc2);
  free_f(d->rbassc2);
  free_f(d->lbassc3);
  free_f(d->rbassc3);

  free_agc(d->agc_left);
  free_agc(d->agc_right);

  free(d->dxl);
  free(d->dxr);

  free_sigmoidal(d->sigmoidal);

  mono_lmux=NULL;
}

//---------------------------------------------------------------- the sound card

//Everything that only exists once a card is actually open. The web ui comes up
//before this, so a busy or missing device is something you can see and fix in
//the browser instead of a startup crash.
typedef struct {
  int open;
  int rate_in;
  int rate_out;
  int prop;
  int buffer_size;
  int i_buffer_size;
  double block_seconds;

  int* buffer_o;
  int* buffer_t;
  double* buffer_tf;
  double* helper;

  int* buffer_end;
  int* o_buffer_end;
  double* buffer_endf;
  double* helper_end;
} audio_io;

static void audio_close(audio_io* a){
  if(a->open)
    alsa_pipe_exit();

  free(a->buffer_o);
  free(a->buffer_t);
  free(a->buffer_tf);
  free(a->helper);
  memset(a,0,sizeof(*a));
}

static int audio_open(audio_io* a,const rt_params* p,char* err,size_t errlen){
  memset(a,0,sizeof(*a));

  int ch1=2;
  int ch2=2;
  int rate1=p->io_input_rate;
  int rate2=p->io_output_rate;

  //the whole chain walks the buffers two samples at a time, an odd size
  //would read one past the end on the last pair
  int buffer_size=p->io_buffer_size;
  if(buffer_size<2)
    buffer_size=2;
  buffer_size=buffer_size-(buffer_size%2);

  set_latency(p->io_latency_buffers);

  if(setup_alsa_pipe((char*)p->io_record_device,(char*)p->io_playback_device,
        &ch1,&ch2,&rate1,&rate2,buffer_size)==-1){
    snprintf(err,errlen,"%s",alsa_pipe_error());
    return -1;
  }

  int prop=rate2/rate1;
  if(prop<1)
    prop=1;

  int i_buffer_size=buffer_size/prop;
  i_buffer_size=i_buffer_size-(i_buffer_size%2);
  if(i_buffer_size<2)
    i_buffer_size=2;

  //too big for the stack at large block sizes
  a->buffer_o=malloc(sizeof(int)*buffer_size);
  a->buffer_t=malloc(sizeof(int)*i_buffer_size);
  a->buffer_tf=malloc(sizeof(double)*i_buffer_size);
  a->helper=malloc(sizeof(double)*i_buffer_size);

  if(a->buffer_o==NULL || a->buffer_t==NULL || a->buffer_tf==NULL || a->helper==NULL){
    snprintf(err,errlen,"out of memory allocating the audio buffers");
    alsa_pipe_exit();
    free(a->buffer_o);
    free(a->buffer_t);
    free(a->buffer_tf);
    free(a->helper);
    memset(a,0,sizeof(*a));
    return -1;
  }

  memset(a->buffer_t,0,sizeof(int)*i_buffer_size);
  memset(a->buffer_o,0,sizeof(int)*buffer_size);

  a->rate_in=rate1;
  a->rate_out=rate2;
  a->prop=prop;
  a->buffer_size=buffer_size;
  a->i_buffer_size=i_buffer_size;
  a->block_seconds=((double)(i_buffer_size/2))/(double)rate1;

  a->buffer_end=a->buffer_t+i_buffer_size;
  a->o_buffer_end=a->buffer_o+buffer_size;
  a->buffer_endf=a->buffer_tf+i_buffer_size;
  a->helper_end=a->helper+i_buffer_size;

  a->open=1;
  return 0;
}

//---------------------------------------------------------------- utilities

static double now_seconds(void){
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  return (double)ts.tv_sec+(double)ts.tv_nsec/1000000000.0;
}

//the stream player publishes its own status, this adds the sound card and RDS
static void publish_state(rt_meters* m,int audio_ok,const char* audio_text){
  char rds_text[192];
  rds_status(rds_text,sizeof(rds_text));
  meters_set_rds(rds_is_ready() && mpx_has_rds_carrier(),rds_text);

  meters_set_audio(audio_ok,audio_text);
  meters_publish(m);
}

static void usage(const char* self){
  printf("usage: %s [options]\n",self);
  printf("  -c FILE   settings file (default %s)\n",params_config_path());
  printf("  -p PORT   web ui port, 0 turns the web ui off\n");
  printf("  -b ADDR   web ui bind address, 127.0.0.1 keeps it on this machine\n");
  printf("  -n        do not start the web ui\n");
  printf("  -h        this help\n");
}

//----------------------------------------------------------------- the loop

int main(int argc,char** argv){
  saved_argv=argv;

  //a restart re-executes /proc/self/exe, which would otherwise leave the
  //process called "exe" and out of reach of pgrep and pkill
  #ifdef PR_SET_NAME
    if(argv[0]!=NULL){
      const char* base=strrchr(argv[0],'/');
      prctl(PR_SET_NAME,(base!=NULL)?base+1:argv[0],0,0,0);
    }
  #endif

  rt_params P;
  params_defaults(&P);

  const char* config_path=NULL;
  const char* web_bind=NULL;
  int web_port=-1;
  int web_enabled=1;

  for(int i=1;i<argc;i++){
    if(strcmp(argv[i],"-c")==0 && i+1<argc){
      config_path=argv[++i];
    }else if(strcmp(argv[i],"-p")==0 && i+1<argc){
      web_port=atoi(argv[++i]);
      if(web_port<=0)
        web_enabled=0;
    }else if(strcmp(argv[i],"-b")==0 && i+1<argc){
      web_bind=argv[++i];
    }else if(strcmp(argv[i],"-n")==0){
      web_enabled=0;
    }else if(strcmp(argv[i],"-h")==0 || strcmp(argv[i],"--help")==0){
      usage(argv[0]);
      return 0;
    }else{
      printf("unknown option '%s'\n",argv[i]);
      usage(argv[0]);
      return 1;
    }
  }

  if(config_path!=NULL)
    params_set_config_path(config_path);

  if(params_load_file(params_config_path())>=0)
    printf("loaded settings from %s\n",params_config_path());
  else
    printf("no settings file at %s, using the defaults from DEFAULTS.h\n",params_config_path());

  params_take(&P,NULL);

  //the command line wins over the settings file
  if(web_port<0)
    web_port=P.web_port;
  if(web_bind==NULL)
    web_bind=P.web_bind;

  signal(SIGINT,on_signal);
  signal(SIGTERM,on_signal);
  signal(SIGPIPE,SIG_IGN);

  mono_gains=malloc(sizeof(double)*MAX_BANDS);
  mono_pvals=malloc(sizeof(double)*MAX_BANDS);
  if(mono_gains==NULL || mono_pvals==NULL){
    printf("out of memory\n");
    return -1;
  }

  //the control page comes up first, so a busy sound card is something you can
  //see and fix in the browser rather than a startup failure
  if(web_enabled){
    if(webui_start(web_bind,web_port)!=1)
      printf("web ui could not start, carrying on without it\n");
  }

  if(P.ui_terminal)
    init_inputs();

  audio_io io;
  memset(&io,0,sizeof(io));
  dsp_chain dsp;
  int dsp_ready=0;
  int mpx_cache_rate=0;
  char audio_error[192]="waiting for the sound card";
  double next_open_try=0;

  int time_off=0;
  int c=0;

  double avg_post_agc=0;
  double avg_pre_agc=0;
  double avg_post_clip=0;
  double avg_pre_clip=0;

  double local_right=0;
  int taken_sample=0;
  int meter_tick=0;

  rt_meters meters;
  memset(&meters,0,sizeof(meters));

  int restart=0;

  while(c!='q' && c!=CTRLC && !quit_requested){

    if(webui_restart_requested()){
      restart=1;
      break;
    }

    //pick up whatever the web ui changed since the last block
    param_flags flags;
    if(params_take(&P,&flags)){
      if(dsp_ready){
        if(flags.rebuild){
          dsp_free(&dsp);
          dsp_build(&dsp,&P,io.rate_in);
        }else{
          dsp_apply_live(&dsp,&P);
        }

        if(flags.mpxcache && P.mpx_enable){
          printf("regenerating MPX cache...\n");
          set_mpx_phase(P.mpx_offset_19k,P.mpx_offset_38k,P.mpx_offset_57k);
          set_rds_enabled(P.rds_enable);
          init_mpx(io.rate_out,P.mpx_percent_pilot,P.mpx_composite_limit);
          mpx_cache_rate=io.rate_out;
        }
      }
    }

    //the stream player, if the user asked for one
    streamer_poll(&P);

    if(!io.open){
      //no card yet. keep the ui and the player alive and try again in a while,
      //picking up whatever device name is configured at that moment.
      meters.running=0;
      publish_state(&meters,0,audio_error);

      double now=now_seconds();
      if(now>=next_open_try){
        next_open_try=now+3.0;
        if(audio_open(&io,&P,audio_error,sizeof(audio_error))==0){
          printf("starting rates: input: %d, output: %d\n\n",io.rate_in,io.rate_out);
          dsp_build(&dsp,&P,io.rate_in);
          dsp_ready=1;
          mpx_cache_rate=0;
        }
      }

      usleep(200000);
      continue;
    }

    //the cache is only built when the encoder is actually switched on
    if(P.mpx_enable && mpx_cache_rate!=io.rate_out){
      printf("generating MPX cache...\n");
      publish_state(&meters,1,"generating the MPX cache, this takes a moment");
      set_mpx_phase(P.mpx_offset_19k,P.mpx_offset_38k,P.mpx_offset_57k);
      set_rds_enabled(P.rds_enable);
      init_mpx(io.rate_out,P.mpx_percent_pilot,P.mpx_composite_limit);
      mpx_cache_rate=io.rate_out;
    }

    int* buffer_t=io.buffer_t;
    int* buffer_o=io.buffer_o;
    double* buffer_tf=io.buffer_tf;
    double* helper_buffer=io.helper;
    int* buffer_end=io.buffer_end;
    int* o_buffer_end=io.o_buffer_end;
    double* buffer_endf=io.buffer_endf;
    double* helper_buffer_end=io.helper_end;
    int input_buffer_prop=io.prop;
    int buffer_size=io.buffer_size;
    int rate2=io.rate_out;

    if(get_audio(buffer_t,io.i_buffer_size)==-1){
      //a failed read returns straight away, do not spin on a dead card
      usleep(1000);
      continue;
    }

    double block_start=now_seconds();

    //convert to float
    double* ittr=buffer_tf;
    for(int* pl=buffer_t;pl<buffer_end;pl++){

      double pcm = *pl;
      if(fabs(pcm)<=2){
        if(pcm<0)
          pcm = -2;
        else
          pcm = 2;
      }
      *ittr=(pcm / 65538.0 );
      ittr++;
    }

    if(!P.chain_bypass){
      if(P.stereo_enable==1){
        if(P.stereo_gain!=1){
          amplify_stereo_plex(buffer_tf,buffer_endf,P.stereo_gain);
        }
      }else{
        demux_mono(buffer_tf,buffer_endf);
      }
    }

    double buffer;
    int count=0;
    float ch_nobass;
    avg_pre_agc=0;
    avg_post_agc=0;
    avg_post_clip=0;
    avg_pre_clip=0;

    double* helper_dr=helper_buffer;
    for(double* start=buffer_tf;start<buffer_endf;start++){

      if(!P.chain_bypass){

        if(*start==0){
          if(time_off<=P.chain_silence_samples){
            time_off++;
          }
        }else{
          time_off=0;
        }

        if(time_off<P.chain_silence_samples){
          if(*start!=0){
            buffer = *start;
            if(avg_pre_agc<fabs(*start)){
              avg_pre_agc=fabs(*start);
            }

            if(count%2==0){

              if(P.chain_highpass_enable){
                buffer=run_f(dsp.lbassc3,buffer);
              }
              ch_nobass=run_f(dsp.lbassc,buffer);

              buffer=apply_agc(dsp.agc_right,buffer,P.agc_target,P.agc_speed,P.agc_gate,ch_nobass,P.agc_release);
              buffer = apply_expander(dsp.dxl,buffer,P.exp_gain);
            }else{

              if(P.chain_highpass_enable){
                buffer=run_f(dsp.rbassc3,buffer);
              }
              ch_nobass=run_f(dsp.rbassc,buffer);

              buffer=apply_agc(dsp.agc_left,buffer,P.agc_target,P.agc_speed,P.agc_gate,ch_nobass,P.agc_release);
              buffer = apply_expander(dsp.dxr,buffer,P.exp_gain);
            }

            if(avg_post_agc<fabs(buffer)){
              avg_post_agc=fabs(buffer);
            }

            if(P.chain_dynamic_compressor)
              buffer=dynamic_compressor(buffer,1);

            buffer = buffer * P.agc_post_gain;
          }else{
            buffer=*start;
            buffer = buffer * P.agc_post_gain;
          }

          if(count==0){
            if(P.mb_enable){
              mux(dsp.lmux,buffer);
              for(int i=0; i<dsp.nbands;i++){
                double val = power_at(dsp.lmux,i);
                set_power_at(dsp.lmux,i,val*P.band_pre_amp[i]);
              }

              //cheap band metering, once every 64 frames
              if((meter_tick&63)==0){
                for(int i=0;i<dsp.nbands;i++){
                  double level=fabs(power_at(dsp.lmux,i));
                  if(level>meters.band_level[i])
                    meters.band_level[i]=level;
                }
              }
              meter_tick++;

              if(P.mb_mono_compression){
                double value=(local_right+buffer)/2.0;

                mux(dsp.mmux,value);
                for(int i=0; i<dsp.nbands;i++){
                  double val = power_at(dsp.mmux,i);
                  set_power_at(dsp.mmux,i,val*P.band_pre_amp[i]);
                }
                run_compressors_advanced(dsp.mmbt,h_compressor_left);
                taken_sample=1;
              }else{
                run_compressors(dsp.lmbt);
              }

              buffer=demux(dsp.lmux) * P.chain_final_amp;
            }

            if(avg_pre_clip<fabs(buffer)){
              avg_pre_clip=fabs(buffer);
            }

            buffer=run_f(dsp.lpassfinal,buffer);
            if(P.chain_highpass_enable){
              buffer=run_f(dsp.lbassc2,buffer);
            }

            if(avg_post_clip<fabs(buffer)){
              avg_post_clip=fabs(buffer);
            }

          }else{
            if(P.mb_enable){
              mux(dsp.rmux,buffer);
              for(int i=0; i<dsp.nbands;i++){
                double val = power_at(dsp.rmux,i);
                set_power_at(dsp.rmux,i,val*P.band_pre_amp[i]);
              }

              if(P.mb_mono_compression){
                double copy=buffer;
                if(taken_sample==1){
                  for(int i=0;i<dsp.nbands;i++){
                    if(P.band_mix_stereo[i]==1){
                      set_power_at(dsp.rmux,i, mono_pvals[i]);
                    }else{
                      double amplitude=power_at(dsp.rmux,i);
                      set_power_at(dsp.rmux,i, amplitude*mono_gains[i]);
                    }
                  }
                }
                taken_sample=0;
                local_right=copy;
              }else{
                run_compressors(dsp.rmbt);
              }

              buffer=demux(dsp.rmux) * P.chain_final_amp;
            }

            if(avg_pre_clip<fabs(buffer)){
              avg_pre_clip=fabs(buffer);
            }

            buffer=run_f(dsp.rpassfinal,buffer);
            if(P.chain_highpass_enable){
              buffer=run_f(dsp.rbassc2,buffer);
            }

            if(avg_post_clip<fabs(buffer)){
              avg_post_clip=fabs(buffer);
            }
          }
        }else{
          buffer=0;
        }
      }else{
        buffer=(*start);
      }

      //tape saturation
      if(P.tape_enable){
        if(fabs(buffer)<0.00001)
          buffer=0.00001;

        buffer = asymetric_tanh(buffer * P.tape_drive, 1 , P.tape_threshold , P.tape_asymmetry,P.tape_wetness);
      }

      *helper_dr=buffer;
      helper_dr++;
      count=~count;
    }

    int clip_count=0;

    if(P.mpx_enable){
      to_mpx(helper_buffer,helper_buffer_end);

      if(P.clip_enable){
        mpx_clip(dsp.sigmoidal,helper_buffer,helper_buffer_end,32767 );
      }
      clip_count=get_clip_count(dsp.sigmoidal);

      gain_array(helper_buffer,helper_buffer_end,P.mpx_output_gain);
      resample_up_stereo_mpx(helper_buffer,buffer_o,helper_buffer_end,input_buffer_prop);

      if(rate2 == 96000||rate2 == 192000){
        int* right;
        for(int* loop=buffer_o;loop<o_buffer_end;loop=loop+2){
          right=loop+1;

          int mono_sample=*loop;
          double mpx=get_mpx_next_value(*loop,*right,P.mpx_percent_mono,P.mpx_percent_stereo);

          if(P.mpx_right)
            *right=mpx;
          else
            *right=mono_sample;

          if(P.mpx_left)
            *loop=mpx;
          else
            *loop=mono_sample;
        }
      }
    }else{
      //no mpx encoder, the two channels stay left and right
      double limit=31767+P.clip_drange;
      if(P.clip_enable){
        for(double* pt=helper_buffer;pt<helper_buffer_end;pt++){
          if(*pt>limit){
            *pt=limit;
            clip_count++;
          }else if(*pt<-limit){
            *pt=-limit;
            clip_count++;
          }
        }
      }
      gain_array(helper_buffer,helper_buffer_end,P.mpx_output_gain);
      resample_up_stereo_mpx(helper_buffer,buffer_o,helper_buffer_end,input_buffer_prop);
    }

    double percent_distortion = (clip_count/((double)buffer_size))*100;
    if(percent_distortion > 2){
      printf("\033[Asevere distortion: \x1b[31m%g %%\x1b[0m            \n",percent_distortion);
    }else if(percent_distortion > 0.5){
      printf("\033[Anoticable distortion: \x1b[33m%g %%\x1b[0m         \n",percent_distortion);
    }else if(percent_distortion > 0.1){
      printf("\033[Abarely noticable distortion: \x1b[32m%g %%\x1b[0m  \n",percent_distortion);
    }

    queue_audio(buffer_o);

    //publish what the web ui draws
    meters.running=1;
    meters.pre_agc=avg_pre_agc;
    meters.post_agc=avg_post_agc;
    meters.pre_clip=avg_pre_clip;
    meters.post_clip=avg_post_clip;
    meters.distortion=percent_distortion;
    meters.nbands=dsp.nbands;
    for(int i=0;i<dsp.nbands;i++)
      meters.band_gain[i]=dsp.lmbt->compressors[i]->gain;

    double spent=now_seconds()-block_start;
    if(io.block_seconds>0)
      meters.load=(spent/io.block_seconds)*100.0;

    publish_state(&meters,1,"running");
    for(int i=0;i<MAX_BANDS;i++)
      meters.band_level[i]=0;

    if(P.ui_terminal){
      float stereo_gain=P.stereo_gain;
      int stereo_on=P.stereo_enable;

      draw_ui(c,&stereo_gain,&stereo_on,(int)avg_post_agc,(int)avg_pre_agc,
          (int)avg_post_clip,(int)avg_pre_clip,dsp.lmbt,dsp.rmbt);
      c=wgetch_nblk();

      //keep the terminal display and the web ui in agreement
      char text[32];
      if(stereo_gain!=(float)P.stereo_gain){
        snprintf(text,sizeof(text),"%f",stereo_gain);
        params_set("stereo.gain",text);
      }
      if(stereo_on!=P.stereo_enable){
        snprintf(text,sizeof(text),"%d",stereo_on);
        params_set("stereo.enable",text);
      }
    }
  }

  if(P.ui_terminal)
    deguchi_nara();

  printf("\nshutting down\n");

  webui_stop();
  streamer_stop();

  if(dsp_ready)
    dsp_free(&dsp);

  free(mono_gains);
  free(mono_pvals);

  free_mpx_cache();

  audio_close(&io);

  if(restart){
    printf("restarting\n");
    fflush(stdout);
    execv("/proc/self/exe",saved_argv);
    //only reached when exec failed
    perror("restart failed");
    return 1;
  }

  return 0;
}
