/* Declarations the Z80 core expects from its host (as SMS Plus provides them). */
#define INLINE static inline
#define LSB_FIRST 1
int cpu_readop(int address);
int cpu_readop_arg(int address);
int cpu_readmem16(int address);
void cpu_writemem16(int address, int data);
int cpu_readport(int port);
void cpu_writeport(int port, int data);
void change_pc16(int pc);
void change_pc(int pc);
int cpu_getactivecpu(void);
static inline void logerror(const char *f, ...) { (void)f; }
