#ifndef VOSTOK_WEBUI
#define VOSTOK_WEBUI

//A very small http server that serves the control page and a json api.
//It runs on its own thread and only talks to the audio thread through
//the parameter store in config/params.h

//returns 1 on success, -1 when the socket could not be opened
int webui_start(const char* bind_addr,int port);
void webui_stop(void);

//set by the web ui when the user asks for a restart, the audio loop polls it
int  webui_restart_requested(void);

#endif // !VOSTOK_WEBUI
