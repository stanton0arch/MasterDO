#ifndef BENCH_OPERROR_H
#define BENCH_OPERROR_H
/* The fields of an error word, as include/3do/operror.h lays them out. */
#define Make6Bit(n) ((n >= 'a')? (n-'a'+11): ((n <= '9')? (n-'0'+1):(n-'A'+37)))
#define ERR_ERRSIZE (8)
#define ERR_CLASSIZE (1)
#define ERR_ENVSIZE (2)
#define ERR_SEVERESIZE (2)
#define ERR_IDSIZE (12)
#define ERR_ERRSHIFT (0)
#define ERR_CLASHIFT (ERR_ERRSHIFT+ERR_ERRSIZE)
#define ERR_ENVSHIFT (ERR_CLASHIFT+ERR_CLASSIZE)
#define ERR_SEVERESHIFT (ERR_ENVSHIFT+ERR_ENVSIZE)
#define ERR_IDSHIFT (ERR_SEVERESHIFT+ERR_SEVERESIZE)
#define MakeErrId(a,b) ((Make6Bit((unsigned char)(a))<<6)|Make6Bit((unsigned char)(b)))
#define ER_FSYS MakeErrId('F','S')
#define ER_KRNL MakeErrId('K','r')
#define ER_SEVERE 2
#define ER_C_NSTND 1
#define MAKEFERR(svr,cls,err) ((Err)(0x80000000UL | ((unsigned)(ER_FSYS)<<ERR_IDSHIFT) | ((svr)<<ERR_SEVERESHIFT) | ((cls)<<ERR_CLASHIFT) | (err)))
#define MAKEKERR(svr,cls,err) ((Err)(0x80000000UL | ((unsigned)(ER_KRNL)<<ERR_IDSHIFT) | ((svr)<<ERR_SEVERESHIFT) | ((cls)<<ERR_CLASHIFT) | (err)))
#endif
