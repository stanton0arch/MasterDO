/* The console as the host programs of this directory see it: the disc is
 * one file named on the command line, the allocator is the host's, the
 * log goes to stderr -- in full while the machine boots, then warnings
 * and errors only. The same terms as the picture check
 * (tests/cel8/romrun.c), written once for the judge and the two probes.
 *
 * The functions the core calls (OpenBlockFile, sys_alloc, log_begin and
 * the rest) are defined in host.c; this header only names what a program
 * sets and reads.
 */
#ifndef Z80C_HOST_H
#define Z80C_HOST_H

/* The ROM the boot loads, set before cart_init. Its extension decides
   the profile: a .gg image boots with the Game Gear profile. */
extern const char *host_rom_path;

/* Raised once the boot is over: from then on the log lets through
   warnings and errors only. */
extern int host_booted;

/* A positive count from the command line, the whole word or nothing:
   -1 when it is not one. */
long host_count_arg(const char *s);

/* FNV-1a, 32 bits, continued over several runs of bytes. */
unsigned long host_fnv_begin(void);
unsigned long host_fnv_add(unsigned long h, const unsigned char *p,
                           unsigned long n);
/* A word folded in as four bytes, low first, whatever the host's order. */
unsigned long host_fnv_add_u32(unsigned long h, unsigned long v);

/* The presentation as the console makes it once line 191 is counted:
   the journal undone, every band replayed and the journal emptied. The
   processor sees no difference, but without it the journal fills and
   the console never runs that way. */
void host_present(void);

#endif
