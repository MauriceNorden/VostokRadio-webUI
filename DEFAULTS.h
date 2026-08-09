#ifndef DEFAULTS
#define DEFAULTS

//Evan Nikitin 2025

int fdef[]={60,250,350,1000,6000,15000,18800}; //multiband compression filters
int fdef_size=7;


float mix_stereo[]={0           ,0              ,0          ,0 ,0         ,0   ,0  };
int   lookaheads[]={2 ,  2 , 2, 2 ,2,2, 2}; // will affect phase
float def_attack[]={1   ,0.5 ,  0.05  , 0.03 , 0.03 ,0.03 ,0.03};//multiband compression attack

float def_release[]={ 900 , 900  ,800 ,1200 ,1200 , 1120  ,4.5}; //multiband compression release
float def_target[]={8000,7000,11000,11000,11000,7000,29000}; //multiband compression target volume

float def_m_gain[]={1.50,1.00,1.00,1.00,1.00,1.00,7.00}; //make up gain
float pre_amp[]={0.5,0.7,0.7,0.7,0.7,1,50}; //multiband compressor pre compression gain
float def_gate[]={300,320,300,1000,1000,1000,300}; //multi band compressor gate
int bypass[]={0,0,0,0,0,0,0}; //band compression bypass
float post_amp[]={0.4,1,0.8,0.8,0.8,0.9,1}; // band compression post amplification
float effect[]={25,25,50,50,50,50,100};//ratio
float knee[]={4,1,1,1,1,1,1};//knee
float knee_release[]={1.002,1.001,1,1,1,1,1};//knee
//int types[]={COMP_PEAK,COMP_PEAK,COMP_PEAK,COMP_PEAK,COMP_PEAK,COMP_PEAK,COMP_PEAK};//band compression compressor types

//this maximizes loudness, you can comment this out if you are using mono
//#define MONO_COMPRESSION //turns the compressor from stereo to mono
//
//#define DYNAMIC_COMPRESSOR //makes the sound louder but ruins quality

#define MULTIBAND_COMPRESSION // comment this to disable the multiband compressor

//#define BYPASS //uncomment this to bypass compressor chain


#define EXPANDER_RATIO 0
#define EXPANDER_ATTACK 0.00048
#define EXPANDER_RELEASE 0.000048
#define EXPANDER_GAIN 0.0000001
#define EXPANDER_THRESHOLD 0.01

/* Not yet implemented
#define GATE
#define GATE_RELEASE 0.00001
#define GATE_ATTACK 0.001
#define GATE_THRESHOLD 2000
*/
#define PRE_CLIP_SATURATION 0
#define PRE_CLIP_SATURATION_LIMIT 60000
#define POST_SAT_GAIN 1

#define TAPE_SAT_THRESH 202767
#define TAPE_SAT_WETNESS 0
#define TAPE_SAT_OFFSET 1.00
#define TAPE_SAT_DRIVE 1.00
//#define TAPE_SAT_BYPASS


#define FINAL_AMP 1.5 // can change the global gain after the multiband compressor
#define FINAL_CLIP//comment to disable and use a gain leveler instead(not recommended)
//#define FINAL_CLIP_LOOKAHEAD 100 //samples
//#define FINAL_CLIP_LOOKAHEAD_RELEASE 0.004 //release coeficient, proportional to # samples
#define SIGMOIDAL_CO 2.5
#define SIGMOIDAL_ATTACK  1.667
#define SIGMOIDAL_RELEASE 14.765
#define SIGMOIDAL_BUFFER 80
#define SIGMOIDAL_KNEE 0
#define SIGMOIDAL_PRE 9200
#define SIGMOIDAL_DRANGE 1000 //this should be near the start of the convergance to 1 or -1 of the tanh function relative to the limit
//Vostok RF AM transmitters can handle low bass pretty well, you could set this to 20hz
//most other AM transmitters require bass cut, so set this to like 70hz
//some PLLVCO based FM transmitters might also require bass cut, our current model has trouble with bass.
//poor bass response in FM transmitters is caused by the milivolts of change even after filtering, from the power supply's rectifier circuit (PLL VCOs are more sensitive than you think)
//The PLL VCO removes this noise due to its' feedback loop mechanism but this then re generates that noise waveform at the varactor diode and your audio signal will be out of phase with it.

#define HIGH_PASS // for FM transmitters that have trouble with low frequency bass
#define HIGH_PASS_CUTOFF 5 //comment the line above to disable
#define HIGH_PASS_POLES 4
#define DC_REMOVAL_COEFF 0.0000001

//final low pass, applied per channel after the multiband compressor
#define FINAL_LOWPASS 17000
#define FINAL_LOWPASS_POLES 5

//number of poles used by the band splitter of the multiband compressor
#define BAND_POLES 5

//how many consecutive zero samples count as "silence" (processing is muted after this)
#define SILENCE_SAMPLES 20000


//alsa configuration
#define RECORDING_IFACE "hw:1,1,0"
#define PLAYBACK_IFACE "hw:0,0"
#define RATE 192000 //output rate, for MPX
#define IN_RATE 48000 //recording rate, the processing chain is tuned for 48khz
#define BUFFER_SIZE 50000 //output buffer size in samples (both channels)
#define LATENCY_BUFFERS 20 //how many output buffers the alsa pipe keeps in flight
//the program always records with a sample rate of 48khz

// 0 is false 1 is true
#define STEREO 1
#define STEREO_GAIN 3
//#define STEREO_GAIN 3 //the stereo amplification coefficient(good setting for streams)

#define POST_AGC_GAIN 1

#define AGC_TARG 7000 //input AGC baseline target
#define AGC_LOOKAHEAD 1
#define AGC_SPEED 0.58 //response coefficient
//#define AGC_SPEED 0 //response coefficient
//#define AGC_RELEASE 0.0000000001 //response coefficient
#define AGC_RELEASE 0.48 //response coefficient
#define AGC_GATE 0.0001
#define AGC_GAIN_MAX 40 //the AGC will never amplify by more than this
#define AGC_GAIN_START 10 //gain the AGC starts at
#define AGC_SIDECHAIN_CUTOFF 50 //high pass in front of the AGC level detector
#define AGC_SIDECHAIN_POLES 1

// set to 1 to show the levels in real time, 0 to keep silent
#define GUI 0 // just have this as zero, the terminal gui doesnt work at this moment
              // use the web ui instead (see WEB_PORT below)

//web ui, control every setting below from a browser while the processor runs
#define WEB_PORT 8080
#define WEB_BIND "0.0.0.0" //use "127.0.0.1" to only allow control from this machine
#define CONFIG_FILE "vostok.conf" //settings saved from the web ui land here

//internet radio source. when enabled the processor starts a player itself and
//feeds the stream into RECORDING_IFACE, so nothing else has to be running.
//needs mpv, ffmpeg or mpg123 installed, and usually the snd-aloop module.
#define SOURCE_ENABLE 0
#define SOURCE_URL ""
#define SOURCE_DEVICE "plughw:CARD=Loopback,DEV=0" //the other end of the loopback
#define SOURCE_PLAYER "auto" //auto, mpv, ffmpeg or mpg123
#define SOURCE_CACHE_SECS 20 //stream buffer, also added latency (mpv only)

//FM radio setings, only apply if the output sampling rate is 96khz or higher
#define MPX_ENABLE //uncomment to enable
//#define COMPOSITE_CLIPPER // recommended but likely unnesecary if using the final lookahead clipper
#define COMPOSITE_CLIPPER_LOOKAHEAD 20
#define COMPOSITE_CLIPPER_LOOKAHEAD_RELEASE 0.00006
#define PERCENT_PILOT 0.12 //percent of the signal devoted to the 19khz pilot tone
#define PERCENT_MONO 6.5 // percent of the signal devoted to mono audio
#define PERCENT_STEREO 6.5// percent of the signal devoted to mono audio
			   // sometimes if there is distortion, decreasing the percent stereo could help

#define MPX_OUTPUT_GAIN 32769 //scales the 16 bit domain of the chain up to the 32 bit output
#define MPX_COMPOSITE_LIMIT 2147483647 //peak value of the composite signal

//RDS, the data service on the 57khz subcarrier.
//needs MPX_ENABLE and a 192khz output rate, 57khz does not fit under nyquist
//at 96khz. everything here can also be set from the web ui.
#define RDS_ENABLE 0
#define RDS_PI "0000"     //four hex digits, assigned by your regulator
#define RDS_PS "VOSTOK  " //up to 8 characters, the name receivers show
#define RDS_RT ""         //up to 64 characters of free text
#define RDS_AF ""         //"100.1, 103.5", empty means no alternatives
#define RDS_PTY 0         //programme type, 0 is none
#define RDS_TP 0          //this station carries traffic announcements
#define RDS_MS 1          //1 music, 0 speech
#define RDS_DI 8          //bit 3 stereo, 2 artificial head, 1 compressed, 0 dynamic pty
#define RDS_LEVEL 0.03    //share of the composite peak, 3 percent is normal

//sound card dependent MPX tuning, see the comments in MPX/generator.c
#define MPX_OFFSET_19K 0        //phase offset of the pilot, in samples
#define MPX_OFFSET_38K 0.00793875 //phase offset of the 38khz subcarrier, in samples
#define MPX_OFFSET_57K 0        //phase offset of the RDS subcarrier, in samples
#define MPX_DAC_2ND_HARMONIC 0.00006 //measured 2nd harmonic of the 19khz pilot
#define MPX_COMPOSITE_DIST 0.000000001 //how much ultrasonic distortion the composite clipper may make


#define SYNTHESIZE_MPX_REALTIME


//MPX output channels enabeled
//sometimes having multiple channels sending MPX can cause cross talk in the cable and increase distortion

#define RIGHT_MPX
//#define LEFT_MPX

#endif // !DEFAULTS
