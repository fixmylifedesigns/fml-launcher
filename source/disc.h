#ifndef DISC_H
#define DISC_H
#include <gctypes.h>

// The disc drive. A background thread watches the lid, reads the header of
// whatever disc goes in, and copies discs to the SD card on request.

enum { DISC_NONE, DISC_READING, DISC_GAME, DISC_WII, DISC_UNKNOWN };

typedef struct {
	int state;
	char id[7];      // "GALE01"
	char title[64];  // from the disc header
	char region[16];
	u8 disc_no;      // 0 = disc 1
	u8 version;
} DiscInfo;

void disc_init(void);
// Copies the current state. Returns a counter that changes whenever the disc does.
u32 disc_get(DiscInfo *out);
// Stops the thread before another program starts (it may be busy with the drive).
void disc_stop(void);

// Copying a GameCube disc to the card. The file is written as `path`.part and
// renamed to `path` once the whole disc is in.
enum { RIP_IDLE, RIP_RUNNING, RIP_DONE, RIP_FAILED, RIP_CANCELLED };
typedef struct {
	int state;
	u64 done, total; // bytes
	u32 crc;         // CRC32 of the whole disc (when done)
	u32 started;     // ticks, for speed and time left
	char error[96];
} RipStatus;

#define DISC_SIZE 1459978240ULL // every GameCube disc image is this big

bool disc_rip_start(const char *path);
void disc_rip_cancel(void);
void disc_rip_status(RipStatus *out);
void disc_rip_clear(void); // back to RIP_IDLE after a finished copy

#endif
