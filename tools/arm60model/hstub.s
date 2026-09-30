; Host services of the ARM60 cycle model: each stub traps into the
; simulator with a SWI, arguments in r0-r3 (and the stack for printf).

        AREA    |!!!sim|, CODE, READONLY

        EXPORT  sim_start
        EXPORT  printf
        EXPORT  sprintf
        EXPORT  AllocMemFromMemLists
        EXPORT  FreeMemToMemLists
        EXPORT  plat_usec_now
        EXPORT  sim_rom
        EXPORT  sim_rom_size
        EXPORT  sim_dump
        EXPORT  sim_arg
        EXPORT  sim_pad
        EXPORT  sim_mark
        EXPORT  sim_codedump
        IMPORT  hmain

sim_start
        bl      hmain
        swi     0xF00000
printf
        swi     0xF00001
        mov     pc,lr
sprintf
        swi     0xF0000C
        mov     pc,lr
AllocMemFromMemLists
        swi     0xF00002
        mov     pc,lr
FreeMemToMemLists
        swi     0xF00003
        mov     pc,lr
plat_usec_now
        swi     0xF00004
        mov     pc,lr
sim_rom
        swi     0xF00005
        mov     pc,lr
sim_rom_size
        swi     0xF00006
        mov     pc,lr
sim_dump
        swi     0xF00007
        mov     pc,lr
sim_arg
        swi     0xF00008
        mov     pc,lr
sim_pad
        swi     0xF00009
        mov     pc,lr
sim_mark
        swi     0xF0000A
        mov     pc,lr
sim_codedump
        swi     0xF0000B
        mov     pc,lr

        END
