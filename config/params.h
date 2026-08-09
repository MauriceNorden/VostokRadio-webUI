#ifndef VOSTOK_PARAMS
#define VOSTOK_PARAMS

//Every knob the processor has, as runtime state instead of #defines.
//DEFAULTS.h still holds the compile time defaults, this just makes them live.
//
//The audio thread never touches g_params directly, it calls params_take() once
//per buffer and works from its own private copy, so the web server can write
//whenever it likes without tearing a value in the middle of the dsp loop.

#include <stddef.h>

#define MAX_BANDS 16
#define DEVICE_NAME_LEN 96
#define URL_LEN 512
#define PLAYER_NAME_LEN 16
#define RDS_PI_LEN 8
#define RDS_PS_LEN 9    //8 characters
#define RDS_RT_LEN 65   //64 characters
#define RDS_AF_LEN 160

typedef struct {
  //band splitter and multiband compressor
  int    nbands;
  int    band_poles;
  int    band_freq[MAX_BANDS];
  int    band_lookahead[MAX_BANDS];
  int    band_mix_stereo[MAX_BANDS];
  int    band_bypass[MAX_BANDS];
  double band_attack[MAX_BANDS];
  double band_release[MAX_BANDS];
  double band_target[MAX_BANDS];
  double band_makeup[MAX_BANDS];
  double band_pre_amp[MAX_BANDS];
  double band_post_amp[MAX_BANDS];
  double band_gate[MAX_BANDS];
  double band_ratio[MAX_BANDS];
  double band_knee[MAX_BANDS];
  double band_knee_release[MAX_BANDS];

  int    mb_enable;
  int    mb_mono_compression;

  //processing chain
  int    chain_bypass;
  int    chain_dynamic_compressor;
  double chain_final_amp;
  int    chain_silence_samples;
  double chain_lowpass_cutoff;
  int    chain_lowpass_poles;
  int    chain_highpass_enable;
  double chain_highpass_cutoff;
  int    chain_highpass_poles;

  //stereo
  int    stereo_enable;
  double stereo_gain;

  //input agc
  double agc_target;
  double agc_speed;
  double agc_release;
  double agc_gate;
  double agc_gain_max;
  double agc_gain_start;
  int    agc_lookahead;
  double agc_post_gain;
  double agc_sidechain_cutoff;
  int    agc_sidechain_poles;

  //downward expander
  double exp_ratio;
  double exp_attack;
  double exp_release;
  double exp_gain;
  double exp_threshold;

  //tape saturation
  int    tape_enable;
  double tape_drive;
  double tape_threshold;
  double tape_wetness;
  double tape_asymmetry;

  //final sigmoidal lookahead clipper
  int    clip_enable;
  int    clip_buffer;
  double clip_ratio;
  double clip_attack;
  double clip_release;
  double clip_knee;
  double clip_pre;
  double clip_drange;

  //mpx encoder
  int    mpx_enable;
  double mpx_percent_pilot;
  double mpx_percent_mono;
  double mpx_percent_stereo;
  double mpx_output_gain;
  double mpx_composite_limit;
  int    mpx_left;
  int    mpx_right;
  double mpx_offset_19k;
  double mpx_offset_38k;
  double mpx_offset_57k;
  double mpx_dac_2nd_harmonic;
  double mpx_comp_dist;

  //RDS, the data service on the 57khz subcarrier
  int    rds_enable;
  char   rds_pi[RDS_PI_LEN];
  char   rds_ps[RDS_PS_LEN];
  char   rds_rt[RDS_RT_LEN];
  char   rds_af[RDS_AF_LEN];
  int    rds_pty;
  int    rds_tp;
  int    rds_ta;
  int    rds_ms;
  int    rds_di;
  double rds_level;

  //audio interfaces, these only take effect on restart
  char   io_record_device[DEVICE_NAME_LEN];
  char   io_playback_device[DEVICE_NAME_LEN];
  int    io_input_rate;
  int    io_output_rate;
  int    io_buffer_size;
  int    io_latency_buffers;

  //web ui, also restart only
  char   web_bind[DEVICE_NAME_LEN];
  int    web_port;

  //internet radio source, the processor starts a player and feeds itself
  int    source_enable;
  char   source_url[URL_LEN];
  char   source_device[DEVICE_NAME_LEN];
  char   source_player[PLAYER_NAME_LEN];
  int    source_cache_secs;

  //terminal level meters, kept for compatibility with the old GUI define
  int    ui_terminal;
} rt_params;

typedef enum {
  PT_DOUBLE,
  PT_INT,
  PT_BOOL,
  PT_STRING
} ptype;

//how much work a change costs
typedef enum {
  PA_LIVE,      //picked up on the next audio buffer
  PA_REBUILD,   //filters/compressors are rebuilt, short glitch
  PA_MPXCACHE,  //the mpx wave cache is regenerated, audio drops for a moment
  PA_RESTART    //needs the process to restart
} papply;

typedef struct {
  const char* name;
  const char* group;
  const char* label;
  const char* unit;
  ptype  type;
  papply apply;
  size_t offset;   //offset inside rt_params
  int    is_band;  //1 when the field is a per band array
  double min;
  double max;      //for PT_STRING this is the size of the character array
  double step;
  const char* help;
} param_desc;

typedef struct {
  int changed;   //any parameter was written since the last take
  int rebuild;   //at least one of them needs the dsp objects rebuilt
  int mpxcache;  //at least one of them needs the mpx cache regenerated
} param_flags;

extern const param_desc PARAM_TABLE[];
extern const int PARAM_COUNT;

//the pilot level the composite clipper reserves room for.
//lives here because clippers/sigmoidal.c used to read the PERCENT_PILOT macro.
extern double vostok_pilot_percent;

void params_defaults(rt_params* p);

//grab the current settings, returns 1 when something changed since last time
int  params_take(rt_params* dst,param_flags* flags);
//read the settings without consuming the change flags
void params_peek(rt_params* dst);

//returns 1 ok, 0 unknown name, -1 unusable value.
//name may be indexed for band parameters: "band.target[2]"
int  params_set(const char* name,const char* value);

int  params_load_file(const char* path);
int  params_save_file(const char* path);
const char* params_config_path(void);
void params_set_config_path(const char* path);

//json for the web ui, caller frees the returned string
char* params_values_json(void);
char* params_schema_json(void);

//meters published by the audio thread
typedef struct {
  int    running;    //1 once audio is flowing
  double pre_agc;
  double post_agc;
  double pre_clip;
  double post_clip;
  double distortion;
  double load;       //percent of realtime spent processing
  int    nbands;
  double band_level[MAX_BANDS];
  double band_gain[MAX_BANDS];

  //state of the sound card, so the page can say why there is no audio
  int    audio_ok;
  char   audio_text[192];

  //state of the stream player
  int    source_state;
  char   source_text[192];

  //what the RDS encoder is doing
  int    rds_on;
  char   rds_text[192];
} rt_meters;

//publishes the level meters. the audio and source status fields are owned by
//the two setters below and are kept across a publish.
void meters_publish(const rt_meters* m);
void meters_set_audio(int ok,const char* text);
void meters_set_source(int state,const char* text);
void meters_set_rds(int on,const char* text);
void meters_read(rt_meters* m);
char* meters_json(void);

#endif // !VOSTOK_PARAMS
