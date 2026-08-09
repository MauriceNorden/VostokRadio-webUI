
#ifndef MPX
#define MPX
#include "../lookahead_limiter/lookaheadlim.h"
//Evan Nikitin 2025
//get a value from the mpx encoder
double get_mpx_next_value(double mono,double stereo,double percent_mono,double percent_stereo);
void free_mpx_cache();

void init_mpx(int rate,double percent_pilot,double max);

//runtime tuning, none of these touch the wave cache
void set_mpx_levels(double percent_pilot,double max);
void set_mpx_dac_harmonic(double second_harmonic);
void set_mpx_harmonic_dist(double percent);
//injection level of the RDS subcarrier, as a fraction of the composite peak
void set_rds_level(double percent);
//changing the phase offsets, or turning RDS on and off, needs the cache
//regenerated. call init_mpx() again afterwards.
void set_mpx_phase(double offset_19k,double offset_38k,double offset_57k);
void set_rds_enabled(int enabled);
int  mpx_has_rds_carrier(void);

double mpx_peak_38khz_modulation();//get mpx modulation value for time sliced clipping
//sample rate resampling
//void resample_up_stereo(int* input,int* output,int* input_end,int ratio);
void resample_up_stereo_mpx(double* input,int* output,double* input_end,int ratio);

double get_48_19k();
double get_48_38k();
void itterate_48k_sample();
#endif // !DEBUG

