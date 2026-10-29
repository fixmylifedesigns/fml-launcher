#ifndef BOOT_H
#define BOOT_H

// Loads a .dol from the card and runs it, passing argv[0] = path followed by
// `args`, and environment variables `env` ("KEY=value"), both NULL-terminated
// lists or NULL. `progress` is called with 0..100 while reading and
// `before_jump` right before the DOL starts. Returns an error message only
// if the DOL couldn't be started.
const char *boot_dol(const char *path, const char *const *args, const char *const *env,
                     void (*progress)(int percent), void (*before_jump)(void));

#endif
