#ifndef VOSTOK_RDS
#define VOSTOK_RDS

//RDS encoder, the baseband half.
//
//Produces the 1187.5 bit/s biphase data signal that gets modulated onto the
//57khz subcarrier in MPX/generator.c. Sends group 0A (PI, PS name, PTY, TA,
//AF) and group 2A (RadioText).
//
//Everything here is owned by the audio thread. Settings arrive through
//rds_configure() from the usual parameter snapshot, and are picked up at the
//next group boundary so a name never changes halfway through a transmission.

#include <stddef.h>
#include "../config/params.h"

//builds the pulse shaping table and the bit clock for this sample rate.
//57khz needs at least a 152khz sample rate, below that rds_is_ready() stays 0.
void rds_init(int sample_rate);
void rds_free(void);

void rds_configure(const rt_params* p);

//one baseband sample, peak normalised to about 1. multiply by the carrier.
double rds_next_sample(void);

int  rds_is_ready(void);
void rds_status(char* out,size_t len);

#endif // !VOSTOK_RDS
