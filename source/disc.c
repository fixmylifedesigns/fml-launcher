#include <gccore.h>
#include <malloc.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>

#include "disc.h"
#include "library.h"

#define CHUNK (512 * 1024)

static lwp_t thread = LWP_THREAD_NULL;
static u8 stack[32 * 1024] ATTRIBUTE_ALIGN(32);
static mutex_t lock;
static volatile bool quit;

static DiscInfo info;
static u32 changes;

static RipStatus rip;
static char rip_path[300];
static volatile bool rip_requested, rip_cancel;

static u32 crc_table[256];

// Drive lid: bit 0 of the DI cover register is set while it's open
static bool lid_open(void)
{
	volatile u32 *di = (volatile u32 *)0xCC006000;
	return di[1] & 1;
}

static void set_info(const DiscInfo *d)
{
	LWP_MutexLock(lock);
	if (memcmp(&info, d, sizeof(info)) != 0) {
		info = *d;
		changes++;
	}
	LWP_MutexUnlock(lock);
}

static void set_state(int state)
{
	DiscInfo d = { .state = state };
	set_info(&d);
}

static void read_header(void)
{
	static u8 head[0x60] ATTRIBUTE_ALIGN(32);
	DiscInfo d = { .state = DISC_NONE };
	DVD_Mount();
	dvdcmdblk blk;
	if (DVD_ReadPrio(&blk, head, sizeof(head), 0, 2) < 0) {
		set_info(&d); // no disc (or nothing the drive can read)
		return;
	}
	DCInvalidateRange(head, sizeof(head));
	u32 gc = (head[0x1C] << 24) | (head[0x1D] << 16) | (head[0x1E] << 8) | head[0x1F];
	u32 wii = (head[0x18] << 24) | (head[0x19] << 16) | (head[0x1A] << 8) | head[0x1B];
	if (wii == 0x5D1C9EA3) {
		d.state = DISC_WII;
	} else if (gc == 0xC2339F3D) {
		d.state = DISC_GAME;
		for (int i = 0; i < 6; i++)
			d.id[i] = isalnum(head[i]) ? head[i] : '_';
		d.disc_no = head[6];
		d.version = head[7];
		const char *t = (const char *)&head[0x20];
		bool ascii = t[0] != 0;
		for (int i = 0; i < 0x40 && t[i]; i++)
			if ((u8)t[i] < 0x20 || (u8)t[i] > 0x7E)
				ascii = false;
		snprintf(d.title, sizeof(d.title), "%.*s", 63, ascii ? t : d.id);
		snprintf(d.region, sizeof(d.region), "%s", region_from_id(d.id[3]));
	} else {
		d.state = DISC_UNKNOWN;
	}
	set_info(&d);
}

// ---------------------------------------------------------------- copying

static u32 crc32_update(u32 crc, const u8 *p, u32 n)
{
	crc = ~crc;
	while (n--)
		crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static volatile bool read_done;
static volatile s32 read_result;

static void read_cb(s32 result, dvdcmdblk *blk)
{
	(void)blk;
	read_result = result;
	read_done = true;
}

static dvdcmdblk async_blk;

static void start_read(u8 *buf, u32 len, u64 off)
{
	read_done = false;
	if (DVD_ReadAbsAsyncPrio(&async_blk, buf, len, off, read_cb, 2) < 0) {
		read_result = -1;
		read_done = true;
	}
}

static s32 wait_read(void)
{
	while (!read_done)
		usleep(1000);
	return read_result;
}

static void rip_fail(const char *fmt, u64 off)
{
	LWP_MutexLock(lock);
	rip.state = RIP_FAILED;
	snprintf(rip.error, sizeof(rip.error), fmt, (unsigned long)(off >> 20));
	LWP_MutexUnlock(lock);
}

static void do_rip(void)
{
	char part[310];
	snprintf(part, sizeof(part), "%s.part", rip_path);
	const u64 total = DISC_SIZE;
	u8 *bufs[2] = { memalign(32, CHUNK), memalign(32, CHUNK) };
	FILE *f = bufs[0] && bufs[1] ? fopen(part, "wb") : NULL;
	if (!f) {
		rip_fail("Couldn't create the file on the SD card.", 0);
		free(bufs[0]);
		free(bufs[1]);
		return;
	}
	DVD_Mount(); // fresh start for the drive (it may have spun down)

	u32 crc = 0;
	u64 off = 0;
	int cur = 0;
	bool ok = true, pending = true;
	start_read(bufs[0], CHUNK, 0);
	while (off < total) {
		u32 len = total - off < CHUNK ? (u32)(total - off) : CHUNK;
		s32 r = wait_read();
		pending = false;
		for (int retry = 0; r < 0 && retry < 3 && !rip_cancel; retry++) {
			dvdcmdblk blk;
			r = DVD_ReadPrio(&blk, bufs[cur], len, off, 2);
		}
		if (r < 0) {
			rip_fail("The disc couldn't be read at %lu MB. Clean it and try again.", off);
			ok = false;
			break;
		}
		DCInvalidateRange(bufs[cur], len);
		u64 next = off + len;
		if (next < total && !rip_cancel) {
			start_read(bufs[cur ^ 1], total - next < CHUNK ? (u32)(total - next) : CHUNK, next);
			pending = true;
		}
		crc = crc32_update(crc, bufs[cur], len);
		if (fwrite(bufs[cur], 1, len, f) != len) {
			rip_fail("Couldn't write to the SD card at %lu MB. Is it full?", off);
			ok = false;
			break;
		}
		off = next;
		cur ^= 1;
		LWP_MutexLock(lock);
		rip.done = off;
		LWP_MutexUnlock(lock);
		if (rip_cancel) {
			ok = false;
			break;
		}
	}
	if (pending)
		wait_read();
	fclose(f);
	free(bufs[0]);
	free(bufs[1]);

	LWP_MutexLock(lock);
	if (ok && rename(part, rip_path) == 0) {
		rip.state = RIP_DONE;
		rip.crc = crc;
	} else {
		remove(part); // only ever our own unfinished copy
		if (ok)
			snprintf(rip.error, sizeof(rip.error), "Couldn't rename the finished copy.");
		if (rip_cancel)
			rip.state = RIP_CANCELLED;
		else
			rip.state = RIP_FAILED;
	}
	LWP_MutexUnlock(lock);
}

// ---------------------------------------------------------------- thread

static void *drive_thread(void *arg)
{
	(void)arg;
	bool probed = false;
	while (!quit) {
		if (rip_requested) {
			rip_requested = false;
			do_rip();
			continue;
		}
		if (lid_open()) {
			if (probed)
				set_state(DISC_NONE);
			probed = false;
		} else if (!probed) {
			probed = true;
			set_state(DISC_READING);
			read_header();
		}
		usleep(250 * 1000);
	}
	return NULL;
}

void disc_init(void)
{
	for (u32 i = 0; i < 256; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++)
			c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
	LWP_MutexInit(&lock, false);
	DVD_Init();
	quit = false;
	LWP_CreateThread(&thread, drive_thread, NULL, stack, sizeof(stack), 40);
}

u32 disc_get(DiscInfo *out)
{
	LWP_MutexLock(lock);
	*out = info;
	u32 c = changes;
	LWP_MutexUnlock(lock);
	return c;
}

void disc_stop(void)
{
	quit = true;
	rip_cancel = true;
}

bool disc_rip_start(const char *path)
{
	LWP_MutexLock(lock);
	bool busy = rip.state == RIP_RUNNING || info.state != DISC_GAME;
	if (!busy) {
		memset(&rip, 0, sizeof(rip));
		rip.state = RIP_RUNNING;
		rip.total = DISC_SIZE;
		rip.started = gettick();
		snprintf(rip_path, sizeof(rip_path), "%s", path);
		rip_cancel = false;
		rip_requested = true;
	}
	LWP_MutexUnlock(lock);
	return !busy;
}

void disc_rip_cancel(void)
{
	rip_cancel = true;
}

void disc_rip_status(RipStatus *out)
{
	LWP_MutexLock(lock);
	*out = rip;
	LWP_MutexUnlock(lock);
}

void disc_rip_clear(void)
{
	LWP_MutexLock(lock);
	if (rip.state != RIP_RUNNING)
		memset(&rip, 0, sizeof(rip));
	LWP_MutexUnlock(lock);
}
