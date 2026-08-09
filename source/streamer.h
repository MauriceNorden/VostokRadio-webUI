#ifndef VOSTOK_STREAMER
#define VOSTOK_STREAMER

//Runs an internet radio stream into the recording device.
//
//The processor starts a player of its own (mpv, ffmpeg or mpg123) and keeps it
//alive, so a Pi only needs this one program to go from a stream url to a
//transmitter. The player is started with execvp and an argv array, never
//through a shell, and the url is checked before it is handed over.

#include <stddef.h>
#include "../config/params.h"

#define SRC_OFF     0
#define SRC_STARTING 1
#define SRC_PLAYING 2
#define SRC_FAILED  3

//call regularly, a few times a second is plenty.
//starts, stops and restarts the player to match the settings.
void streamer_poll(const rt_params* p);

//stop the player and wait for it, used on shutdown
void streamer_stop(void);

//current state and a line of text for the web ui
int  streamer_state(void);
void streamer_text(char* out,size_t len);

#endif // !VOSTOK_STREAMER
