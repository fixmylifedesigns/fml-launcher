// Minimal DOL for testing the launcher: shows a message, START hot-resets.
#include <gccore.h>
#include <stdio.h>

int main(void)
{
	VIDEO_Init();
	PAD_Init();
	GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
	void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();

	printf("\n\n   Test app booted by CubeLaunch!\n\n   Press START to restart.\n");
	for (;;) {
		PAD_ScanPads();
		if (PAD_ButtonsDown(0) & PAD_BUTTON_START)
			SYS_ResetSystem(SYS_HOTRESET, 0, 0);
		VIDEO_WaitVSync();
	}
}
