#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "params.h"
#include "strbuf.h"
#include "../DEFAULTS.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

//Evan Nikitin 2025, runtime parameter store for the web ui

double vostok_pilot_percent=PERCENT_PILOT;

static rt_params g_params;
static param_flags g_flags={0,0,0};
static pthread_mutex_t g_lock=PTHREAD_MUTEX_INITIALIZER;

static rt_meters g_meters;
static pthread_mutex_t g_meter_lock=PTHREAD_MUTEX_INITIALIZER;

static char g_config_path[512]=CONFIG_FILE;

#define OFF(field) offsetof(rt_params,field)

//name, group, label, unit, type, apply, offset, per band, min, max, step, help
const param_desc PARAM_TABLE[]={

  //----------------------------------------------------------------- source
  {"source.enable","Stream source","Play a stream","",PT_BOOL,PA_LIVE,OFF(source_enable),0,0,1,1,
   "Starts a player that decodes the stream into the recording device and keeps it running"},
  {"source.url","Stream source","Stream URL","",PT_STRING,PA_LIVE,OFF(source_url),0,0,URL_LEN,0,
   "An http or https mp3 or aac stream. Playlists (.m3u, .pls) work too if the player understands them"},
  {"source.device","Stream source","Player output","",PT_STRING,PA_LIVE,OFF(source_device),0,0,DEVICE_NAME_LEN,0,
   "Where the player writes. With snd-aloop this is the other end of the recording device"},
  {"source.player","Stream source","Player","",PT_STRING,PA_LIVE,OFF(source_player),0,0,PLAYER_NAME_LEN,0,
   "auto, mpv, ffmpeg or mpg123. auto takes the first one installed, mpv first because it buffers"},
  {"source.cache_secs","Stream source","Buffer","s",PT_INT,PA_LIVE,OFF(source_cache_secs),0,0,120,1,
   "Seconds of stream buffered ahead, this is also added latency. Only mpv honours it"},

  //------------------------------------------------------------------ input
  {"io.record_device","Audio I/O","Recording device","",PT_STRING,PA_RESTART,OFF(io_record_device),0,0,DEVICE_NAME_LEN,0,
   "ALSA capture device, list them with 'arecord -L'. While no card is open this is retried live"},
  {"io.playback_device","Audio I/O","Playback device","",PT_STRING,PA_RESTART,OFF(io_playback_device),0,0,DEVICE_NAME_LEN,0,
   "ALSA playback device, list them with 'aplay -L'. While no card is open this is retried live"},
  {"io.input_rate","Audio I/O","Input rate","Hz",PT_INT,PA_RESTART,OFF(io_input_rate),0,8000,192000,1000,
   "The chain is tuned for 48000, changing this retunes every filter"},
  {"io.output_rate","Audio I/O","Output rate","Hz",PT_INT,PA_RESTART,OFF(io_output_rate),0,48000,192000,1000,
   "MPX needs 96000 or 192000"},
  {"io.buffer_size","Audio I/O","Buffer size","samples",PT_INT,PA_RESTART,OFF(io_buffer_size),0,2000,200000,1000,
   "Output samples per block, both channels. Smaller means lower latency and more cpu"},
  {"io.latency_buffers","Audio I/O","Latency buffers","",PT_INT,PA_RESTART,OFF(io_latency_buffers),0,3,100,1,
   "How many blocks the alsa pipe keeps queued"},

  //-------------------------------------------------------------------- agc
  {"agc.target","AGC","Target level","",PT_DOUBLE,PA_LIVE,OFF(agc_target),0,0,32767,10,
   "Level the AGC drives the input towards"},
  {"agc.speed","AGC","Speed","",PT_DOUBLE,PA_LIVE,OFF(agc_speed),0,0,1,0.001,
   "How fast the gain comes down, 0 bypasses the AGC into a sine shaper"},
  {"agc.release","AGC","Release","",PT_DOUBLE,PA_LIVE,OFF(agc_release),0,0,1,0.001,
   "How fast the gain comes back up"},
  {"agc.gate","AGC","Gate","",PT_DOUBLE,PA_LIVE,OFF(agc_gate),0,0,32767,1,
   "Below this level the AGC stops tracking. The AGC rounds this down to a whole number"},
  {"agc.gain_max","AGC","Maximum gain","x",PT_DOUBLE,PA_LIVE,OFF(agc_gain_max),0,1,1000,1,
   "Hard ceiling on the AGC gain"},
  {"agc.gain_start","AGC","Start gain","x",PT_DOUBLE,PA_REBUILD,OFF(agc_gain_start),0,0.001,1000,1,
   "Gain the AGC boots with"},
  {"agc.lookahead","AGC","Lookahead","samples",PT_INT,PA_REBUILD,OFF(agc_lookahead),0,1,512,1,
   "Delay between the level detector and the audio it acts on"},
  {"agc.post_gain","AGC","Post gain","x",PT_DOUBLE,PA_LIVE,OFF(agc_post_gain),0,0,10,0.01,
   "Gain applied straight after the AGC"},
  {"agc.sidechain_cutoff","AGC","Sidechain high pass","Hz",PT_DOUBLE,PA_REBUILD,OFF(agc_sidechain_cutoff),0,5,500,1,
   "Bass is removed from the level detector so kick drums do not pump the whole mix"},
  {"agc.sidechain_poles","AGC","Sidechain poles","",PT_INT,PA_REBUILD,OFF(agc_sidechain_poles),0,1,8,1,
   "Steepness of the sidechain high pass"},

  //----------------------------------------------------------------- stereo
  {"stereo.enable","Stereo","Stereo","",PT_BOOL,PA_LIVE,OFF(stereo_enable),0,0,1,1,
   "Off collapses the input to mono"},
  {"stereo.gain","Stereo","Stereo width","x",PT_DOUBLE,PA_LIVE,OFF(stereo_gain),0,0,10,0.01,
   "Amplifies the L-R difference before the compressor"},

  //--------------------------------------------------------------- expander
  {"expander.ratio","Downward expander","Amount","",PT_DOUBLE,PA_LIVE,OFF(exp_ratio),0,0,1,0.01,
   "0 disables the expander, 1 applies it fully"},
  {"expander.attack","Downward expander","Attack","",PT_DOUBLE,PA_LIVE,OFF(exp_attack),0,0,1,0.00001,
   "How fast quiet passages are pulled down"},
  {"expander.release","Downward expander","Release","",PT_DOUBLE,PA_LIVE,OFF(exp_release),0,0,1,0.00001,
   "How fast the expander lets go once the signal is back"},
  {"expander.gain","Downward expander","Gain floor","",PT_DOUBLE,PA_LIVE,OFF(exp_gain),0,0.0000001,1,0.0000001,
   "Lowest gain the expander may reach"},
  {"expander.threshold","Downward expander","Threshold","",PT_DOUBLE,PA_LIVE,OFF(exp_threshold),0,0,32767,0.01,
   "Level under which the expander starts pulling down"},

  //-------------------------------------------------------------- multiband
  {"mb.enable","Multiband","Multiband compressor","",PT_BOOL,PA_LIVE,OFF(mb_enable),0,0,1,1,
   "Off also bypasses the final gain, exactly like commenting out MULTIBAND_COMPRESSION"},
  {"mb.mono_compression","Multiband","Mono compression","",PT_BOOL,PA_LIVE,OFF(mb_mono_compression),0,0,1,1,
   "Both channels share one set of band gains. Louder, kills some stereo detail"},
  {"mb.bands","Multiband","Band count","",PT_INT,PA_REBUILD,OFF(nbands),0,1,MAX_BANDS,1,
   "Number of frequency bands"},
  {"mb.poles","Multiband","Splitter poles","",PT_INT,PA_REBUILD,OFF(band_poles),0,1,8,1,
   "Steepness of the crossover filters"},

  //------------------------------------------------------------------ bands
  {"band.freq","Bands","Crossover","Hz",PT_INT,PA_REBUILD,OFF(band_freq),1,20,20000,10,
   "Upper edge of the band, keep them in ascending order"},
  {"band.pre_amp","Bands","Pre gain","x",PT_DOUBLE,PA_LIVE,OFF(band_pre_amp),1,0,100,0.01,
   "Gain into the band compressor"},
  {"band.attack","Bands","Attack","",PT_DOUBLE,PA_LIVE,OFF(band_attack),1,0.000001,100,0.001,
   "Smaller is faster. Converted to a per sample coefficient at 48khz"},
  {"band.release","Bands","Release","",PT_DOUBLE,PA_LIVE,OFF(band_release),1,0.000001,10000,1,
   "Larger is slower"},
  {"band.target","Bands","Target","",PT_DOUBLE,PA_LIVE,OFF(band_target),1,0,40000,100,
   "Level the band is compressed towards"},
  {"band.gate","Bands","Gate","",PT_DOUBLE,PA_LIVE,OFF(band_gate),1,0,40000,10,
   "Under this level the band releases instead of tracking"},
  {"band.makeup","Bands","Max gain","x",PT_DOUBLE,PA_LIVE,OFF(band_makeup),1,0,1000,0.01,
   "Ceiling on the make up gain of the band"},
  {"band.ratio","Bands","Ratio","",PT_DOUBLE,PA_LIVE,OFF(band_ratio),1,1,1000,1,
   "1 is no compression, large values approach a limiter"},
  {"band.knee","Bands","Knee","",PT_DOUBLE,PA_LIVE,OFF(band_knee),1,0.01,100,0.01,
   "Softens the attack as the band approaches the target"},
  {"band.knee_release","Bands","Release knee","",PT_DOUBLE,PA_LIVE,OFF(band_knee_release),1,0.5,2,0.001,
   "Softens the release under the gate"},
  {"band.post_amp","Bands","Post gain","x",PT_DOUBLE,PA_LIVE,OFF(band_post_amp),1,0,10,0.01,
   "Gain out of the band compressor"},
  {"band.lookahead","Bands","Lookahead","samples",PT_INT,PA_REBUILD,OFF(band_lookahead),1,1,256,1,
   "Peak detector window, affects phase"},
  {"band.mix_stereo","Bands","Link to stereo","",PT_BOOL,PA_LIVE,OFF(band_mix_stereo),1,0,1,1,
   "Only used with mono compression, feeds the mono result straight into the band"},
  {"band.bypass","Bands","Bypass","",PT_BOOL,PA_LIVE,OFF(band_bypass),1,0,1,1,
   "Leaves the band uncompressed"},

  //------------------------------------------------------------------ chain
  {"chain.bypass","Chain","Bypass everything","",PT_BOOL,PA_LIVE,OFF(chain_bypass),0,0,1,1,
   "Sends the input straight to the output stage"},
  {"chain.dynamic_compressor","Chain","Dynamic compressor","",PT_BOOL,PA_LIVE,OFF(chain_dynamic_compressor),0,0,1,1,
   "Extra sine shaper after the AGC. Louder but rougher"},
  {"chain.final_amp","Chain","Final gain","x",PT_DOUBLE,PA_LIVE,OFF(chain_final_amp),0,0,10,0.01,
   "Applied after the multiband compressor"},
  {"chain.lowpass_cutoff","Chain","Final low pass","Hz",PT_DOUBLE,PA_REBUILD,OFF(chain_lowpass_cutoff),0,3000,20000,100,
   "15000 for AM, 17000 for FM"},
  {"chain.lowpass_poles","Chain","Low pass poles","",PT_INT,PA_REBUILD,OFF(chain_lowpass_poles),0,1,8,1,
   "Steepness of the final low pass"},
  {"chain.highpass_enable","Chain","Bass cut","",PT_BOOL,PA_LIVE,OFF(chain_highpass_enable),0,0,1,1,
   "For transmitters that cannot handle low bass"},
  {"chain.highpass_cutoff","Chain","Bass cut frequency","Hz",PT_DOUBLE,PA_REBUILD,OFF(chain_highpass_cutoff),0,1,200,1,
   "20hz suits Vostok RF AM transmitters, around 70hz suits most other AM rigs"},
  {"chain.highpass_poles","Chain","Bass cut poles","",PT_INT,PA_REBUILD,OFF(chain_highpass_poles),0,1,8,1,
   "Steepness of the bass cut"},
  {"chain.silence_samples","Chain","Silence timeout","samples",PT_INT,PA_LIVE,OFF(chain_silence_samples),0,0,1000000,1000,
   "After this many zero samples the chain mutes instead of chasing noise"},

  //--------------------------------------------------------- tape saturation
  {"tape.enable","Tape saturation","Enabled","",PT_BOOL,PA_LIVE,OFF(tape_enable),0,0,1,1,
   "Asymmetric tanh shaper at the end of the per channel chain"},
  {"tape.drive","Tape saturation","Drive","x",PT_DOUBLE,PA_LIVE,OFF(tape_drive),0,0,10,0.01,
   "Gain into the shaper"},
  {"tape.threshold","Tape saturation","Threshold","",PT_DOUBLE,PA_LIVE,OFF(tape_threshold),0,1000,1000000,1000,
   "Level the tanh curve bends towards"},
  {"tape.wetness","Tape saturation","Wetness","",PT_DOUBLE,PA_LIVE,OFF(tape_wetness),0,0,1,0.01,
   "0 is fully dry, 1 is fully saturated"},
  {"tape.asymmetry","Tape saturation","Asymmetry","",PT_DOUBLE,PA_LIVE,OFF(tape_asymmetry),0,0,2,0.01,
   "Scales the negative half of the curve, 1 is symmetric"},

  //----------------------------------------------------------- final clipper
  {"clip.enable","Final clipper","Enabled","",PT_BOOL,PA_LIVE,OFF(clip_enable),0,0,1,1,
   "The lookahead sigmoidal clipper on the composite signal"},
  {"clip.ratio","Final clipper","Ratio","",PT_DOUBLE,PA_LIVE,OFF(clip_ratio),0,0.1,20,0.01,
   "Base softness of the clipping curve"},
  {"clip.attack","Final clipper","Attack","",PT_DOUBLE,PA_LIVE,OFF(clip_attack),0,0,100,0.001,
   "How fast the clipper tightens up"},
  {"clip.release","Final clipper","Release","",PT_DOUBLE,PA_LIVE,OFF(clip_release),0,0,100,0.001,
   "How fast it opens back up"},
  {"clip.knee","Final clipper","Knee","",PT_DOUBLE,PA_LIVE,OFF(clip_knee),0,0,10,0.01,
   "Softens the transition into limiting"},
  {"clip.pre","Final clipper","Range","",PT_DOUBLE,PA_LIVE,OFF(clip_pre),0,0,32767,100,
   "How far under the limit the clipper starts working"},
  {"clip.drange","Final clipper","Headroom","",PT_DOUBLE,PA_LIVE,OFF(clip_drange),0,0,20000,100,
   "Added to 31767 to get the clipping limit"},
  {"clip.buffer","Final clipper","Lookahead","samples",PT_INT,PA_REBUILD,OFF(clip_buffer),0,1,1000,1,
   "Longer lookahead means less distortion and more delay"},

  //-------------------------------------------------------------------- mpx
  {"mpx.enable","MPX","MPX encoder","",PT_BOOL,PA_LIVE,OFF(mpx_enable),0,0,1,1,
   "Needs an output rate of 96000 or 192000"},
  {"mpx.percent_pilot","MPX","Pilot","",PT_DOUBLE,PA_LIVE,OFF(mpx_percent_pilot),0,0,1,0.001,
   "Share of the composite signal given to the 19khz pilot tone"},
  {"mpx.percent_mono","MPX","Mono drive","",PT_DOUBLE,PA_LIVE,OFF(mpx_percent_mono),0,0,50,0.1,
   "Drive of the mono sum into the composite limiter"},
  {"mpx.percent_stereo","MPX","Stereo drive","",PT_DOUBLE,PA_LIVE,OFF(mpx_percent_stereo),0,0,50,0.1,
   "Drive of the L-R difference. Lower this first if the stereo sounds dirty"},
  {"mpx.output_gain","MPX","Output gain","x",PT_DOUBLE,PA_LIVE,OFF(mpx_output_gain),0,1,100000,1,
   "Scales the 16 bit chain up into the 32 bit output word"},
  {"mpx.composite_limit","MPX","Composite limit","",PT_DOUBLE,PA_LIVE,OFF(mpx_composite_limit),0,1000000,2147483647,1000000,
   "Peak value of the generated composite signal"},
  {"mpx.left","MPX","MPX on left channel","",PT_BOOL,PA_LIVE,OFF(mpx_left),0,0,1,1,
   "Turn one channel off if a thin cable is causing cross talk"},
  {"mpx.right","MPX","MPX on right channel","",PT_BOOL,PA_LIVE,OFF(mpx_right),0,0,1,1,
   "Turn one channel off if a thin cable is causing cross talk"},
  {"mpx.dac_2nd_harmonic","MPX","DAC 2nd harmonic","",PT_DOUBLE,PA_LIVE,OFF(mpx_dac_2nd_harmonic),0,0,0.01,0.000001,
   "Measured second harmonic of the pilot on your sound card"},
  {"mpx.comp_dist","MPX","Composite distortion","",PT_DOUBLE,PA_LIVE,OFF(mpx_comp_dist),0,0,0.000001,0.0000000001,
   "How much ultrasonic harmonic content the composite clipper may produce"},
  {"mpx.offset_19k","MPX","19khz phase offset","samples",PT_DOUBLE,PA_MPXCACHE,OFF(mpx_offset_19k),0,-1,1,0.000001,
   "Sound card phase compensation. Changing it regenerates the wave cache and interrupts audio"},
  {"mpx.offset_38k","MPX","38khz phase offset","samples",PT_DOUBLE,PA_MPXCACHE,OFF(mpx_offset_38k),0,-1,1,0.000001,
   "Sound card phase compensation. Changing it regenerates the wave cache and interrupts audio"},
  {"mpx.offset_57k","MPX","57khz phase offset","samples",PT_DOUBLE,PA_MPXCACHE,OFF(mpx_offset_57k),0,-1,1,0.000001,
   "Phase of the RDS subcarrier. Changing it regenerates the wave cache and interrupts audio"},

  //-------------------------------------------------------------------- rds
  {"rds.enable","RDS","RDS encoder","",PT_BOOL,PA_MPXCACHE,OFF(rds_enable),0,0,1,1,
   "Needs MPX on and a 192khz output rate. Switching it builds the 57khz table, which interrupts audio briefly"},
  {"rds.pi","RDS","PI code","hex",PT_STRING,PA_LIVE,OFF(rds_pi),0,0,RDS_PI_LEN,0,
   "Four hex digits identifying the station, for example 8001. Your regulator assigns this, do not invent one for a real transmission"},
  {"rds.ps","RDS","Station name","",PT_STRING,PA_LIVE,OFF(rds_ps),0,0,RDS_PS_LEN,0,
   "Up to 8 characters, the name a receiver shows. Refreshed about every 0.7 seconds"},
  {"rds.rt","RDS","RadioText","",PT_STRING,PA_LIVE,OFF(rds_rt),0,0,RDS_RT_LEN,0,
   "Up to 64 characters of free text. Leave empty to send station name groups only"},
  {"rds.pty","RDS","Programme type","",PT_INT,PA_LIVE,OFF(rds_pty),0,0,31,1,
   "0 none, 1 news, 10 pop music, 15 other music. The list differs between Europe and North America"},
  {"rds.tp","RDS","Traffic programme","",PT_BOOL,PA_LIVE,OFF(rds_tp),0,0,1,1,
   "The station carries traffic announcements at some point"},
  {"rds.ta","RDS","Traffic announcement","",PT_BOOL,PA_LIVE,OFF(rds_ta),0,0,1,1,
   "A traffic announcement is on air right now. Switch it back off afterwards"},
  {"rds.ms","RDS","Music","",PT_BOOL,PA_LIVE,OFF(rds_ms),0,0,1,1,
   "On for music, off for speech"},
  {"rds.di","RDS","Decoder info","",PT_INT,PA_LIVE,OFF(rds_di),0,0,15,1,
   "Bit 3 stereo, bit 2 artificial head, bit 1 compressed, bit 0 dynamic PTY. 8 means plain stereo"},
  {"rds.af","RDS","Alternative frequencies","MHz",PT_STRING,PA_LIVE,OFF(rds_af),0,0,RDS_AF_LEN,0,
   "Comma separated list, for example 100.1, 103.5. Up to 25. Leave empty to say no alternatives exist"},
  {"rds.level","RDS","Injection","",PT_DOUBLE,PA_LIVE,OFF(rds_level),0,0,0.2,0.001,
   "Share of the composite peak given to RDS. 0.03 is the usual 3 percent, raise it if receivers do not lock"},

  //--------------------------------------------------------------------- ui
  {"ui.terminal","Interface","Terminal meters","",PT_BOOL,PA_LIVE,OFF(ui_terminal),0,0,1,1,
   "The old ANSI level display. It is unfinished, the web ui replaces it"},
  {"web.bind","Interface","Web ui address","",PT_STRING,PA_RESTART,OFF(web_bind),0,0,DEVICE_NAME_LEN,0,
   "0.0.0.0 listens on every interface, 127.0.0.1 only on this machine"},
  {"web.port","Interface","Web ui port","",PT_INT,PA_RESTART,OFF(web_port),0,1,65535,1,
   "Port the control page is served on"}
};

const int PARAM_COUNT=(int)(sizeof(PARAM_TABLE)/sizeof(PARAM_TABLE[0]));

//-------------------------------------------------------------- defaults

//the tables in DEFAULTS.h are float, so 0.7 arrives here as 0.699999988.
//round trip through text to get the number the author actually wrote back.
static double fromf(float v){
  char text[32];
  snprintf(text,sizeof(text),"%.7g",(double)v);
  return strtod(text,NULL);
}

static void band_defaults(rt_params* p,int i){
  //used for bands the compile time defaults do not cover
  int prev=(i>0)?p->band_freq[i-1]:100;
  int next=(int)(prev*1.7);
  if(next>19000)
    next=19000;

  p->band_freq[i]=next;
  p->band_lookahead[i]=2;
  p->band_mix_stereo[i]=0;
  p->band_bypass[i]=0;
  p->band_attack[i]=0.03;
  p->band_release[i]=1000;
  p->band_target[i]=11000;
  p->band_makeup[i]=1;
  p->band_pre_amp[i]=1;
  p->band_post_amp[i]=1;
  p->band_gate[i]=1000;
  p->band_ratio[i]=50;
  p->band_knee[i]=1;
  p->band_knee_release[i]=1;
}

void params_defaults(rt_params* p){
  memset(p,0,sizeof(rt_params));

  int n=fdef_size;
  if(n>MAX_BANDS)
    n=MAX_BANDS;

  p->nbands=n;
  p->band_poles=BAND_POLES;

  for(int i=0;i<MAX_BANDS;i++){
    if(i<n){
      p->band_freq[i]=fdef[i];
      p->band_lookahead[i]=lookaheads[i];
      p->band_mix_stereo[i]=(mix_stereo[i]==1)?1:0;
      p->band_bypass[i]=bypass[i];
      p->band_attack[i]=fromf(def_attack[i]);
      p->band_release[i]=fromf(def_release[i]);
      p->band_target[i]=fromf(def_target[i]);
      p->band_makeup[i]=fromf(def_m_gain[i]);
      p->band_pre_amp[i]=fromf(pre_amp[i]);
      p->band_post_amp[i]=fromf(post_amp[i]);
      p->band_gate[i]=fromf(def_gate[i]);
      p->band_ratio[i]=fromf(effect[i]);
      p->band_knee[i]=fromf(knee[i]);
      p->band_knee_release[i]=fromf(knee_release[i]);
    }else{
      band_defaults(p,i);
    }
  }

  #ifdef MULTIBAND_COMPRESSION
    p->mb_enable=1;
  #endif
  #ifdef MONO_COMPRESSION
    p->mb_mono_compression=1;
  #endif
  #ifdef BYPASS
    p->chain_bypass=1;
  #endif
  #ifdef DYNAMIC_COMPRESSOR
    p->chain_dynamic_compressor=1;
  #endif

  p->chain_final_amp=FINAL_AMP;
  p->chain_silence_samples=SILENCE_SAMPLES;
  p->chain_lowpass_cutoff=FINAL_LOWPASS;
  p->chain_lowpass_poles=FINAL_LOWPASS_POLES;
  #ifdef HIGH_PASS
    p->chain_highpass_enable=1;
  #endif
  p->chain_highpass_cutoff=HIGH_PASS_CUTOFF;
  p->chain_highpass_poles=HIGH_PASS_POLES;

  p->stereo_enable=STEREO;
  p->stereo_gain=STEREO_GAIN;

  p->agc_target=AGC_TARG;
  p->agc_speed=AGC_SPEED;
  p->agc_release=AGC_RELEASE;
  p->agc_gate=AGC_GATE;
  p->agc_gain_max=AGC_GAIN_MAX;
  p->agc_gain_start=AGC_GAIN_START;
  p->agc_lookahead=AGC_LOOKAHEAD;
  p->agc_post_gain=POST_AGC_GAIN;
  p->agc_sidechain_cutoff=AGC_SIDECHAIN_CUTOFF;
  p->agc_sidechain_poles=AGC_SIDECHAIN_POLES;

  p->exp_ratio=EXPANDER_RATIO;
  p->exp_attack=EXPANDER_ATTACK;
  p->exp_release=EXPANDER_RELEASE;
  p->exp_gain=EXPANDER_GAIN;
  p->exp_threshold=EXPANDER_THRESHOLD;

  #ifndef TAPE_SAT_BYPASS
    p->tape_enable=1;
  #endif
  p->tape_drive=TAPE_SAT_DRIVE;
  p->tape_threshold=TAPE_SAT_THRESH;
  p->tape_wetness=TAPE_SAT_WETNESS;
  p->tape_asymmetry=TAPE_SAT_OFFSET;

  #ifdef FINAL_CLIP
    p->clip_enable=1;
  #endif
  p->clip_buffer=SIGMOIDAL_BUFFER;
  p->clip_ratio=SIGMOIDAL_CO;
  p->clip_attack=SIGMOIDAL_ATTACK;
  p->clip_release=SIGMOIDAL_RELEASE;
  p->clip_knee=SIGMOIDAL_KNEE;
  p->clip_pre=SIGMOIDAL_PRE;
  p->clip_drange=SIGMOIDAL_DRANGE;

  #ifdef MPX_ENABLE
    p->mpx_enable=1;
  #endif
  p->mpx_percent_pilot=PERCENT_PILOT;
  p->mpx_percent_mono=PERCENT_MONO;
  p->mpx_percent_stereo=PERCENT_STEREO;
  p->mpx_output_gain=MPX_OUTPUT_GAIN;
  p->mpx_composite_limit=MPX_COMPOSITE_LIMIT;
  //both channels used to carry the composite signal no matter what the
  //RIGHT_MPX / LEFT_MPX defines said, the defaults keep that behaviour
  p->mpx_left=1;
  p->mpx_right=1;
  p->mpx_offset_19k=MPX_OFFSET_19K;
  p->mpx_offset_38k=MPX_OFFSET_38K;
  p->mpx_offset_57k=MPX_OFFSET_57K;
  p->mpx_dac_2nd_harmonic=MPX_DAC_2ND_HARMONIC;
  p->mpx_comp_dist=MPX_COMPOSITE_DIST;

  p->rds_enable=RDS_ENABLE;
  snprintf(p->rds_pi,RDS_PI_LEN,"%s",RDS_PI);
  snprintf(p->rds_ps,RDS_PS_LEN,"%s",RDS_PS);
  snprintf(p->rds_rt,RDS_RT_LEN,"%s",RDS_RT);
  snprintf(p->rds_af,RDS_AF_LEN,"%s",RDS_AF);
  p->rds_pty=RDS_PTY;
  p->rds_tp=RDS_TP;
  p->rds_ta=0;
  p->rds_ms=RDS_MS;
  p->rds_di=RDS_DI;
  p->rds_level=RDS_LEVEL;

  snprintf(p->io_record_device,DEVICE_NAME_LEN,"%s",RECORDING_IFACE);
  snprintf(p->io_playback_device,DEVICE_NAME_LEN,"%s",PLAYBACK_IFACE);
  p->io_input_rate=IN_RATE;
  p->io_output_rate=RATE;
  p->io_buffer_size=BUFFER_SIZE;
  p->io_latency_buffers=LATENCY_BUFFERS;

  snprintf(p->web_bind,DEVICE_NAME_LEN,"%s",WEB_BIND);
  p->web_port=WEB_PORT;

  p->source_enable=SOURCE_ENABLE;
  snprintf(p->source_url,URL_LEN,"%s",SOURCE_URL);
  snprintf(p->source_device,DEVICE_NAME_LEN,"%s",SOURCE_DEVICE);
  snprintf(p->source_player,PLAYER_NAME_LEN,"%s",SOURCE_PLAYER);
  p->source_cache_secs=SOURCE_CACHE_SECS;

  p->ui_terminal=(GUI==1)?1:0;

  vostok_pilot_percent=p->mpx_percent_pilot;

  pthread_mutex_lock(&g_lock);
  g_params=*p;
  g_flags.changed=0;
  g_flags.rebuild=0;
  g_flags.mpxcache=0;
  pthread_mutex_unlock(&g_lock);
}

//------------------------------------------------------------ accessors

int params_take(rt_params* dst,param_flags* flags){
  pthread_mutex_lock(&g_lock);
  *dst=g_params;
  if(flags!=NULL)
    *flags=g_flags;

  int changed=g_flags.changed;
  g_flags.changed=0;
  g_flags.rebuild=0;
  g_flags.mpxcache=0;
  pthread_mutex_unlock(&g_lock);
  return changed;
}

void params_peek(rt_params* dst){
  pthread_mutex_lock(&g_lock);
  *dst=g_params;
  pthread_mutex_unlock(&g_lock);
}

const char* params_config_path(void){
  return g_config_path;
}

void params_set_config_path(const char* path){
  snprintf(g_config_path,sizeof(g_config_path),"%s",path);
}

//------------------------------------------------------------ set by name

static const param_desc* find_param(const char* name){
  for(int i=0;i<PARAM_COUNT;i++){
    if(strcmp(PARAM_TABLE[i].name,name)==0)
      return &PARAM_TABLE[i];
  }
  return NULL;
}

static void* field_ptr(rt_params* p,const param_desc* d,int index){
  char* base=(char*)p+d->offset;
  if(d->is_band==0)
    return base;

  switch(d->type){
    case PT_DOUBLE: return base+(size_t)index*sizeof(double);
    case PT_INT:
    case PT_BOOL:   return base+(size_t)index*sizeof(int);
    default:        return base;
  }
}

static int parse_bool(const char* v,int* out){
  if(strcasecmp(v,"1")==0 || strcasecmp(v,"true")==0 || strcasecmp(v,"on")==0 || strcasecmp(v,"yes")==0){
    *out=1;
    return 1;
  }
  if(strcasecmp(v,"0")==0 || strcasecmp(v,"false")==0 || strcasecmp(v,"off")==0 || strcasecmp(v,"no")==0){
    *out=0;
    return 1;
  }
  return 0;
}

static int parse_number(const char* v,double* out){
  char* end=NULL;
  errno=0;
  double d=strtod(v,&end);
  if(end==v)
    return 0;

  while(*end!=0 && isspace((unsigned char)*end))
    end++;

  if(*end!=0)
    return 0;

  if(d!=d)
    return 0;

  *out=d;
  return 1;
}

//assumes the lock is held
static int assign_one(const param_desc* d,int index,const char* value){
  void* target=field_ptr(&g_params,d,index);

  if(d->type==PT_STRING){
    //for strings the max field carries the size of the character array
    size_t cap=(size_t)d->max;
    if(cap<2 || cap>URL_LEN)
      cap=DEVICE_NAME_LEN;

    snprintf((char*)target,cap,"%s",value);
    return 1;
  }

  if(d->type==PT_BOOL){
    int b=0;
    if(!parse_bool(value,&b)){
      double n=0;
      if(!parse_number(value,&n))
        return -1;
      b=(n!=0)?1:0;
    }
    *(int*)target=b;
    return 1;
  }

  double n=0;
  if(!parse_number(value,&n))
    return -1;

  if(n<d->min)
    n=d->min;
  if(n>d->max)
    n=d->max;

  if(d->type==PT_INT)
    *(int*)target=(int)(n+((n<0)?-0.5:0.5));
  else
    *(double*)target=n;

  return 1;
}

static void note_change(const param_desc* d){
  g_flags.changed=1;
  if(d->apply==PA_REBUILD)
    g_flags.rebuild=1;
  else if(d->apply==PA_MPXCACHE)
    g_flags.mpxcache=1;
}

int params_set(const char* name,const char* value){
  char base[128];
  int index=-1;

  const char* bracket=strchr(name,'[');
  if(bracket!=NULL){
    size_t len=(size_t)(bracket-name);
    if(len>=sizeof(base))
      return 0;

    memcpy(base,name,len);
    base[len]=0;
    index=atoi(bracket+1);
  }else{
    snprintf(base,sizeof(base),"%s",name);
  }

  const param_desc* d=find_param(base);
  if(d==NULL)
    return 0;

  if(d->is_band==0 && index>=0)
    return 0;

  if(index>=MAX_BANDS)
    return -1;

  int result=1;
  pthread_mutex_lock(&g_lock);

  if(d->is_band!=0 && index<0){
    //a whole array, given as a comma separated list
    char* copy=strdup(value);
    if(copy==NULL){
      pthread_mutex_unlock(&g_lock);
      return -1;
    }
    int i=0;
    char* save=NULL;
    for(char* tok=strtok_r(copy,",",&save);tok!=NULL && i<MAX_BANDS;tok=strtok_r(NULL,",",&save)){
      while(*tok!=0 && isspace((unsigned char)*tok))
        tok++;

      if(assign_one(d,i,tok)<0)
        result=-1;
      i++;
    }
    free(copy);
  }else{
    result=assign_one(d,(index<0)?0:index,value);
  }

  if(result==1)
    note_change(d);

  pthread_mutex_unlock(&g_lock);
  return result;
}

//---------------------------------------------------------------- config io

static void write_value(strbuf* b,const rt_params* p,const param_desc* d,int index){
  const char* base=(const char*)p+d->offset;

  if(d->type==PT_STRING){
    sb_add(b,base);
    return;
  }

  if(d->type==PT_DOUBLE){
    const double* v=(const double*)base;
    sb_addf(b,"%.10g",v[index]);
    return;
  }

  const int* v=(const int*)base;
  sb_addf(b,"%d",v[index]);
}

int params_save_file(const char* path){
  rt_params p;
  params_peek(&p);

  FILE* f=fopen(path,"w");
  if(f==NULL)
    return -1;

  strbuf b;
  sb_init(&b);
  sb_add(&b,"# Vostok Radio processor settings\n");
  sb_add(&b,"# written by the web ui, edit by hand if you like\n");
  sb_add(&b,"# per band values are comma separated, lowest band first\n\n");

  const char* group=NULL;
  for(int i=0;i<PARAM_COUNT;i++){
    const param_desc* d=&PARAM_TABLE[i];
    if(group==NULL || strcmp(group,d->group)!=0){
      group=d->group;
      sb_addf(&b,"\n# --- %s ---\n",group);
    }

    sb_addf(&b,"%-26s = ",d->name);
    if(d->is_band!=0){
      for(int k=0;k<p.nbands;k++){
        if(k>0)
          sb_addc(&b,',');
        write_value(&b,&p,d,k);
      }
    }else{
      write_value(&b,&p,d,0);
    }
    sb_addc(&b,'\n');
  }

  int ok=0;
  if(b.s!=NULL && b.failed==0){
    if(fwrite(b.s,1,b.len,f)!=b.len)
      ok=-1;
  }else{
    ok=-1;
  }

  sb_free(&b);
  if(fclose(f)!=0)
    ok=-1;

  return ok;
}

int params_load_file(const char* path){
  FILE* f=fopen(path,"r");
  if(f==NULL)
    return -1;

  char line[1024];
  int applied=0;
  int lineno=0;

  while(fgets(line,sizeof(line),f)!=NULL){
    lineno++;
    char* p=line;
    while(*p!=0 && isspace((unsigned char)*p))
      p++;

    if(*p=='#' || *p==';' || *p==0)
      continue;

    //split on = or the first run of spaces
    char* sep=strchr(p,'=');
    if(sep==NULL){
      sep=p;
      while(*sep!=0 && !isspace((unsigned char)*sep))
        sep++;

      if(*sep==0)
        continue;
    }

    char* value=sep+1;
    *sep=0;

    //trim the key
    char* keyend=sep-1;
    while(keyend>=p && isspace((unsigned char)*keyend)){
      *keyend=0;
      keyend--;
    }

    while(*value!=0 && isspace((unsigned char)*value))
      value++;

    char* vend=value+strlen(value);
    while(vend>value && isspace((unsigned char)*(vend-1))){
      vend--;
      *vend=0;
    }

    if(*p==0)
      continue;

    int rc=params_set(p,value);
    if(rc==1)
      applied++;
    else
      fprintf(stderr,"config %s:%d: ignoring '%s'\n",path,lineno,p);
  }

  fclose(f);
  return applied;
}

//--------------------------------------------------------------------- json

static const char* type_name(ptype t){
  switch(t){
    case PT_DOUBLE: return "double";
    case PT_INT:    return "int";
    case PT_BOOL:   return "bool";
    default:        return "string";
  }
}

static const char* apply_name(papply a){
  switch(a){
    case PA_LIVE:     return "live";
    case PA_REBUILD:  return "rebuild";
    case PA_MPXCACHE: return "mpxcache";
    default:          return "restart";
  }
}

char* params_schema_json(void){
  strbuf b;
  sb_init(&b);
  sb_addf(&b,"{\"max_bands\":%d,\"config\":",MAX_BANDS);
  sb_json(&b,g_config_path);
  sb_add(&b,",\"params\":[");

  for(int i=0;i<PARAM_COUNT;i++){
    const param_desc* d=&PARAM_TABLE[i];
    if(i>0)
      sb_addc(&b,',');

    sb_add(&b,"{\"name\":");
    sb_json(&b,d->name);
    sb_add(&b,",\"group\":");
    sb_json(&b,d->group);
    sb_add(&b,",\"label\":");
    sb_json(&b,d->label);
    sb_add(&b,",\"unit\":");
    sb_json(&b,d->unit);
    sb_add(&b,",\"type\":");
    sb_json(&b,type_name(d->type));
    sb_add(&b,",\"apply\":");
    sb_json(&b,apply_name(d->apply));
    sb_addf(&b,",\"band\":%d,\"min\":",d->is_band);
    sb_json_num(&b,d->min);
    sb_add(&b,",\"max\":");
    sb_json_num(&b,d->max);
    sb_add(&b,",\"step\":");
    sb_json_num(&b,d->step);
    sb_add(&b,",\"help\":");
    sb_json(&b,d->help);
    sb_addc(&b,'}');
  }
  sb_add(&b,"]}");

  if(b.failed){
    sb_free(&b);
    return NULL;
  }
  return b.s;
}

static void json_value(strbuf* b,const rt_params* p,const param_desc* d,int index){
  const char* base=(const char*)p+d->offset;

  if(d->type==PT_STRING){
    sb_json(b,base);
    return;
  }
  if(d->type==PT_DOUBLE){
    const double* v=(const double*)base;
    sb_json_num(b,v[index]);
    return;
  }
  const int* v=(const int*)base;
  sb_addf(b,"%d",v[index]);
}

char* params_values_json(void){
  rt_params p;
  params_peek(&p);

  strbuf b;
  sb_init(&b);
  sb_addf(&b,"{\"nbands\":%d,\"values\":{",p.nbands);

  for(int i=0;i<PARAM_COUNT;i++){
    const param_desc* d=&PARAM_TABLE[i];
    if(i>0)
      sb_addc(&b,',');

    sb_json(&b,d->name);
    sb_addc(&b,':');

    if(d->is_band!=0){
      sb_addc(&b,'[');
      for(int k=0;k<MAX_BANDS;k++){
        if(k>0)
          sb_addc(&b,',');
        json_value(&b,&p,d,k);
      }
      sb_addc(&b,']');
    }else{
      json_value(&b,&p,d,0);
    }
  }
  sb_add(&b,"}}");

  if(b.failed){
    sb_free(&b);
    return NULL;
  }
  return b.s;
}

//------------------------------------------------------------------- meters

void meters_publish(const rt_meters* m){
  pthread_mutex_lock(&g_meter_lock);

  //the status lines are written by their own setters, often from a different
  //place than the level meters, so a publish must not roll them back
  int audio_ok=g_meters.audio_ok;
  int source_state=g_meters.source_state;
  int rds_on=g_meters.rds_on;
  char audio_text[sizeof(g_meters.audio_text)];
  char source_text[sizeof(g_meters.source_text)];
  char rds_text[sizeof(g_meters.rds_text)];
  memcpy(audio_text,g_meters.audio_text,sizeof(audio_text));
  memcpy(source_text,g_meters.source_text,sizeof(source_text));
  memcpy(rds_text,g_meters.rds_text,sizeof(rds_text));

  g_meters=*m;

  g_meters.audio_ok=audio_ok;
  g_meters.source_state=source_state;
  g_meters.rds_on=rds_on;
  memcpy(g_meters.audio_text,audio_text,sizeof(audio_text));
  memcpy(g_meters.source_text,source_text,sizeof(source_text));
  memcpy(g_meters.rds_text,rds_text,sizeof(rds_text));

  pthread_mutex_unlock(&g_meter_lock);
}

void meters_set_rds(int on,const char* text){
  pthread_mutex_lock(&g_meter_lock);
  g_meters.rds_on=on;
  snprintf(g_meters.rds_text,sizeof(g_meters.rds_text),"%s",text);
  pthread_mutex_unlock(&g_meter_lock);
}

void meters_set_audio(int ok,const char* text){
  pthread_mutex_lock(&g_meter_lock);
  g_meters.audio_ok=ok;
  snprintf(g_meters.audio_text,sizeof(g_meters.audio_text),"%s",text);
  pthread_mutex_unlock(&g_meter_lock);
}

void meters_set_source(int state,const char* text){
  pthread_mutex_lock(&g_meter_lock);
  g_meters.source_state=state;
  snprintf(g_meters.source_text,sizeof(g_meters.source_text),"%s",text);
  pthread_mutex_unlock(&g_meter_lock);
}

void meters_read(rt_meters* m){
  pthread_mutex_lock(&g_meter_lock);
  *m=g_meters;
  pthread_mutex_unlock(&g_meter_lock);
}

char* meters_json(void){
  rt_meters m;
  meters_read(&m);

  strbuf b;
  sb_init(&b);
  sb_addf(&b,"{\"running\":%d,\"pre_agc\":",m.running);
  sb_json_num(&b,m.pre_agc);
  sb_add(&b,",\"post_agc\":");
  sb_json_num(&b,m.post_agc);
  sb_add(&b,",\"pre_clip\":");
  sb_json_num(&b,m.pre_clip);
  sb_add(&b,",\"post_clip\":");
  sb_json_num(&b,m.post_clip);
  sb_add(&b,",\"distortion\":");
  sb_json_num(&b,m.distortion);
  sb_add(&b,",\"load\":");
  sb_json_num(&b,m.load);
  sb_addf(&b,",\"nbands\":%d,\"band_level\":[",m.nbands);

  for(int i=0;i<m.nbands && i<MAX_BANDS;i++){
    if(i>0)
      sb_addc(&b,',');
    sb_json_num(&b,m.band_level[i]);
  }
  sb_add(&b,"],\"band_gain\":[");
  for(int i=0;i<m.nbands && i<MAX_BANDS;i++){
    if(i>0)
      sb_addc(&b,',');
    sb_json_num(&b,m.band_gain[i]);
  }
  sb_addf(&b,"],\"audio\":{\"ok\":%d,\"text\":",m.audio_ok);
  sb_json(&b,m.audio_text);
  sb_addf(&b,"},\"source\":{\"state\":%d,\"text\":",m.source_state);
  sb_json(&b,m.source_text);
  sb_addf(&b,"},\"rds\":{\"on\":%d,\"text\":",m.rds_on);
  sb_json(&b,m.rds_text);
  sb_add(&b,"}}");

  if(b.failed){
    sb_free(&b);
    return NULL;
  }
  return b.s;
}
