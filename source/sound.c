#include <asndlib.h>
#include <mp3player.h>

#include "sound.h"

static bool started;

void sound_start(void)
{
	if (started)
		return;
	ASND_Init();
	MP3Player_Init();
	started = true;
}

void sound_stop(void)
{
	if (!started)
		return;
	MP3Player_Stop();
	ASND_End();
	started = false;
}
