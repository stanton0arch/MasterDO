#ifndef BENCH_BLOCKFILE_H
#define BENCH_BLOCKFILE_H
#include "types.h"
typedef struct BlockFile { Item fDevice; } BlockFile, *BlockFilePtr;
Err   OpenBlockFile(char *name, BlockFilePtr bf);
void  CloseBlockFile(BlockFilePtr bf);
int32 GetBlockFileSize(BlockFilePtr bf);
void *LoadFileHere(const char *fname, int32 *pfsize, void *buffer, int32 bufsize);
#endif
