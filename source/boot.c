#include <gccore.h>
#include <ogc/machine/processor.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "boot.h"

// The relocation stub lives in free low memory between the exception
// vectors and where DOLs normally start (0x80003100).
#define STUB_ADDR 0x80001800
#define DOL_MIN_ADDR 0x80003000
#define DOL_MAX_ADDR 0x81800000
#define ARGV_MAGIC 0x5f617267 // "_arg"
#define ENVP_MAGIC 0x5f656e76 // "_env"

extern u8 stub_start[], stub_end[];

typedef struct {
	u32 offset[18]; // 7 text, then 11 data
	u32 addr[18];
	u32 size[18];
	u32 bss_addr;
	u32 bss_size;
	u32 entry;
} DolHeader;

// libogc's struct __argv
typedef struct {
	u32 magic;
	u32 command_line;
	u32 length;
	u32 argc;
	u32 argv;
	u32 end_argv;
} ArgvBlock;

static bool range_ok(u32 addr, u32 size, u32 limit)
{
	return addr >= DOL_MIN_ADDR && addr + size >= addr && addr + size <= limit;
}

const char *boot_dol(const char *path, const char *const *args, const char *const *env,
                     void (*progress)(int), void (*before_jump)(void))
{
	struct stat st;
	if (stat(path, &st) != 0 || st.st_size < (off_t)sizeof(DolHeader))
		return "The file couldn't be read.";
	u32 size = st.st_size;

	// Put the file at the top of the heap so the DOL's sections (which load
	// low) can be copied down without overwriting the data still being read.
	u32 arena_hi = (u32)SYS_GetArena1Hi();
	u32 buf_addr = (arena_hi - size) & ~31;
	if (buf_addr < (u32)SYS_GetArena1Lo())
		return "The file is too large to load.";
	u8 *buf = (u8 *)buf_addr;

	// Keep the heap from growing into the buffer while we load.
	SYS_SetArena1Hi((void *)buf_addr);
	FILE *f = fopen(path, "rb");
	if (!f) {
		SYS_SetArena1Hi((void *)arena_hi);
		return "The file couldn't be opened.";
	}
	const u32 chunk = 64 * 1024;
	for (u32 done = 0; done < size;) {
		progress((int)((u64)done * 100 / size));
		u32 n = size - done < chunk ? size - done : chunk;
		if (fread(buf + done, 1, n, f) != n) {
			fclose(f);
			SYS_SetArena1Hi((void *)arena_hi);
			return "The SD card couldn't be read.";
		}
		done += n;
	}
	fclose(f);
	SYS_SetArena1Hi((void *)arena_hi);
	progress(100);

	DolHeader *hdr = (DolHeader *)buf;
	u32 end = 0;
	for (int i = 0; i < 18; i++) {
		if (hdr->size[i] == 0)
			continue;
		if (hdr->offset[i] + hdr->size[i] > size || !range_ok(hdr->addr[i], hdr->size[i], buf_addr))
			return "It isn't a valid DOL file.";
		if (hdr->addr[i] + hdr->size[i] > end)
			end = hdr->addr[i] + hdr->size[i];
	}
	if ((hdr->bss_size && !range_ok(hdr->bss_addr, hdr->bss_size, buf_addr)) ||
	    hdr->entry < DOL_MIN_ADDR || hdr->entry >= DOL_MAX_ADDR)
		return "It isn't a valid DOL file.";
	if (hdr->bss_addr + hdr->bss_size > end)
		end = hdr->bss_addr + hdr->bss_size;

	// Arguments go just past the DOL's memory: the argv block, then the
	// NUL-separated command line, then room for libogc to build argv[].
	u32 args_addr = (end + 31) & ~31;
	u32 cmd_len = strlen(path) + 1, argc = 1;
	for (int i = 0; args && args[i]; i++, argc++)
		cmd_len += strlen(args[i]) + 1;
	u32 env_addr = (args_addr + 32 + cmd_len + 4 * (argc + 2) + 31) & ~31;
	u32 env_len = 0, envc = 0;
	for (int i = 0; env && env[i]; i++, envc++)
		env_len += strlen(env[i]) + 1;
	u32 args_end = env_addr + 32 + env_len + 4 * (envc + 2);
	if (args_end > buf_addr)
		return "There's no room to pass arguments.";

	if (before_jump)
		before_jump();

	ArgvBlock *block = (ArgvBlock *)args_addr;
	char *cmd = (char *)(args_addr + 32);
	char *p = stpcpy(cmd, path) + 1;
	for (int i = 0; args && args[i]; i++)
		p = stpcpy(p, args[i]) + 1;
	memset(block, 0, sizeof(*block));
	block->magic = ARGV_MAGIC;
	block->command_line = (u32)cmd;
	block->length = cmd_len;
	DCFlushRange(block, 32 + cmd_len);

	ArgvBlock *env_block = NULL;
	if (envc) {
		env_block = (ArgvBlock *)env_addr;
		char *q = (char *)(env_addr + 32);
		for (int i = 0; env[i]; i++)
			q = stpcpy(q, env[i]) + 1;
		memset(env_block, 0, sizeof(*env_block));
		env_block->magic = ENVP_MAGIC;
		env_block->command_line = env_addr + 32;
		env_block->length = env_len;
		DCFlushRange(env_block, 32 + env_len);
	}

	u32 stub_size = stub_end - stub_start;
	memcpy((void *)STUB_ADDR, stub_start, stub_size);
	DCFlushRange((void *)STUB_ADDR, stub_size);
	ICInvalidateRange((void *)STUB_ADDR, stub_size);
	DCFlushRange(buf, size);

	SYS_ResetSystem(SYS_SHUTDOWN, 0, 0);

	u32 level;
	_CPU_ISR_Disable(level);
	(void)level;
	((void (*)(u32, u32, u32))STUB_ADDR)(buf_addr, args_addr, (u32)env_block);
	return NULL; // not reached
}
