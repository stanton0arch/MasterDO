; bench_z80.s - Z80 interpreter micro-benchmark for the ARM60 (ARMv3, big endian).
;
; A reduced Z80 core built on the SMSAdvance scheme:
;   - Z80 registers live in the top bits of ARM registers (A in bits 24-31,
;     BC/DE/HL/SP in bits 16-31, lower bits kept at zero);
;   - flags are kept in the ARM NZCV order so that the ARM flag results can
;     be reused directly (MRS);
;   - opcodes are dispatched through a 256-entry table, memory is read
;     through a table of 1 KiB pages and written through one handler per
;     8 KiB region;
;   - T-states are counted down per scanline; when a line is used up the
;     core jumps to the "nexttimeout" handler.
; Only the opcodes of the benchmark program are implemented; any other
; opcode stops the run and is reported.
;
; ARMv3 differences with the original ARMv4 code: no LDRSB (relative jumps
; sign-extend the displacement with a shift), no LDRH/STRH (IX and IY are
; stored as full words, value in bits 16-31), no BX (MOV pc,lr).
;
; Context layout: CONTRACT with z80b_ctx in bench_cpu.h - keep in sync.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  z80b_setup
        EXPORT  z80b_run
        EXPORT  arm_loop_alu
        EXPORT  arm_loop_ldr
        EXPORT  arm_fill_words

z80f            RN      r3      ; flags, ARM order (see PSR_*)
z80a            RN      r4      ; A in bits 24-31
z80bc           RN      r5      ; BC in bits 16-31
z80de           RN      r6      ; DE in bits 16-31
z80hl           RN      r7      ; HL in bits 16-31
cycles          RN      r8      ; T-states left in the line, times CYCLE
z80pc           RN      r9      ; host address of the next opcode byte
globalptr       RN      r10     ; = &ctx->opz[0]
z80optbl        RN      r10
z80sp           RN      r11     ; SP in bits 16-31
addy            RN      r12     ; Z80 address of the current memory access
z80xy           RN      lr      ; pointer to IX or IY (within one instruction)

; Flags as kept in z80f.
PSR_S           EQU     0x00000008      ; sign (ARM N)
PSR_Z           EQU     0x00000004      ; zero
PSR_C           EQU     0x00000002      ; carry
PSR_V           EQU     0x00000001      ; overflow / parity
PSR_P           EQU     0x00000001
PSR_n           EQU     0x00000080      ; last operation was a subtraction
PSR_H           EQU     0x00000010      ; half carry

CYC_SHIFT       EQU     8
CYCLE           EQU     1<<CYC_SHIFT
LINE_CYCLES     EQU     228             ; T-states per scanline

; Context fields, relative to the global pointer (= &ctx->opz[0]).
        MAP     0,globalptr
memmap_offset   #       -64*4           ; memory map below the global pointer
memmap_tbl      #       64*4
opz             #       256*4
ddz             #       256*4
pzst            #       256
writemem_tbl    #       8*4
cpuregs         #       8*4             ; F A BC DE HL cycles PC SP
z80_ix          #       4
z80_iy          #       4
lastbank        #       4
nexttimeout     #       4
ram_c000        #       4
ram_e000        #       4
lines_left      #       4
stop_op         #       4

;----------------------------------------------------------------------------
; Macros
;----------------------------------------------------------------------------
        MACRO
        fetch   $count
        subs    cycles,cycles,#$count*CYCLE
        ldrplb  r0,[z80pc],#1
        ldrpl   pc,[z80optbl,r0,lsl#2]
        ldr     pc,nexttimeout
        MEND

        MACRO
        fetch_force
        ldrb    r0,[z80pc],#1
        ldr     pc,[z80optbl,r0,lsl#2]
        MEND

        MACRO
        eatcycles $count
        sub     cycles,cycles,#$count*CYCLE
        MEND

        MACRO                           ; z80pc: Z80 PC -> host address
        encodePC
        mvn     r1,z80pc,lsr#10
        ldr     r0,[globalptr,r1,lsl#2]
        str     r0,lastbank
        add     z80pc,z80pc,r0
        MEND

        MACRO                           ; r0 = byte at Z80 address addy
        readmem8
        mvn     r0,addy,lsr#10
        ldr     r0,[globalptr,r0,lsl#2]
        ldrb    r0,[r0,addy]
        MEND

        MACRO                           ; write r0 to Z80 address addy
        writemem8
        and     r1,addy,#0xE000
        adr     r2,writemem_tbl
        adr     lr,%F0
        ldr     pc,[r2,r1,lsr#11]
0
        MEND

        MACRO                           ; r0 = 16-bit immediate
        opLDIM16
        ldrb    r0,[z80pc],#1
        ldrb    r1,[z80pc],#1
        orr     r0,r0,r1,lsl#8
        MEND

        MACRO                           ; high byte of a pair = immediate
        opLDIM8H $x
        ldrb    r0,[z80pc],#1
        and     $x,$x,#0x00FF0000
        orr     $x,$x,r0,lsl#24
        MEND

        MACRO                           ; push the 16-bit value in r0
        push16
        sub     z80sp,z80sp,#0x00020000
        mvn     r1,z80sp,lsr#26
        ldr     r2,[globalptr,r1,lsl#2]
        strb    r0,[r2,z80sp,lsr#16]
        add     r1,z80sp,#0x00010000
        mov     r0,r0,lsr#8
        strb    r0,[r2,r1,lsr#16]
        MEND

        MACRO                           ; $x = 16-bit value popped
        pop16   $x
        mvn     r0,z80sp,lsr#26
        ldr     r1,[globalptr,r0,lsl#2]
        ldrb    $x,[r1,z80sp,lsr#16]
        add     z80sp,z80sp,#0x00010000
        ldrb    r1,[r1,z80sp,lsr#16]
        add     z80sp,z80sp,#0x00010000
        orr     $x,$x,r1,lsl#8
        MEND

        MACRO                           ; addy = IX/IY + signed displacement
        CalcIXd
        ldrb    r1,[z80pc],#1
        ldr     addy,[z80xy]
        mov     r1,r1,lsl#24
        add     addy,addy,r1,asr#8
        mov     addy,addy,lsr#16
        MEND

        MACRO                           ; A = A + ($x << $y)
        opADD   $x,$y
        mov     r1,z80a,lsl#4
        adds    z80a,z80a,$x,lsl#$y
        mrs     z80f,cpsr
        mov     z80f,z80f,lsr#28
        cmn     r1,$x,lsl#$y+4
        orrcs   z80f,z80f,#PSR_H
        MEND

        MACRO                           ; compare A with ($x << $y)
        opCP    $x,$y
        mov     r1,z80a,lsl#4
        cmp     z80a,$x,lsl#$y
        mrs     z80f,cpsr
        mov     z80f,z80f,lsr#28
        eor     z80f,z80f,#PSR_C|PSR_n
        cmp     r1,$x,lsl#$y+4
        orrcc   z80f,z80f,#PSR_H
        MEND

        MACRO                           ; 16-bit add, only C, H and N change
        opADD16 $y,$x
        mov     r1,$y,lsl#4
        adds    $y,$y,$x
        bic     z80f,z80f,#PSR_C+PSR_H+PSR_n
        orrcs   z80f,z80f,#PSR_C
        cmn     r1,$x,lsl#4
        orrcs   z80f,z80f,#PSR_H
        MEND

        MACRO
        opINC8  $x
        and     z80f,z80f,#PSR_C
        adds    $x,$x,#0x01000000
        orrmi   z80f,z80f,#PSR_S
        orrvs   z80f,z80f,#PSR_V
        orrcs   z80f,z80f,#PSR_Z
        tst     $x,#0x0F000000
        orreq   z80f,z80f,#PSR_H
        MEND

        MACRO
        opDEC8  $x
        orr     z80f,z80f,#PSR_n+PSR_H+PSR_S+PSR_V+PSR_Z
        tst     $x,#0x0F000000
        bicne   z80f,z80f,#PSR_H
        subs    $x,$x,#0x01000000
        bicpl   z80f,z80f,#PSR_S
        bicvc   z80f,z80f,#PSR_V
        bicne   z80f,z80f,#PSR_Z
        MEND

;----------------------------------------------------------------------------
; void z80b_setup(z80b_ctx *ctx)
; Fills both opcode tables and the write handlers. The memory map, the flag
; table and the registers are set up by the caller.
;----------------------------------------------------------------------------
z80b_setup
        stmfd   sp!,{r4,lr}
        add     r1,r0,#64*4             ; &opz[0]
        add     r4,r1,#256*4            ; &ddz[0]
        ldr     r2,=op_unimpl
        ldr     r3,=op_unimpl_dd
        mov     r12,#256
setup_fill
        subs    r12,r12,#1
        str     r2,[r1,r12,lsl#2]
        str     r3,[r4,r12,lsl#2]
        bne     setup_fill
        ldr     r2,=op_list
setup_ops
        ldmia   r2!,{r3,r12}
        cmn     r3,#1
        strne   r12,[r1,r3,lsl#2]
        bne     setup_ops
        ldr     r2,=dd_list
setup_dd
        ldmia   r2!,{r3,r12}
        cmn     r3,#1
        strne   r12,[r4,r3,lsl#2]
        bne     setup_dd
        add     r1,r4,#256*4+256        ; &writemem[0]
        ldr     r2,=wr_rom
        ldr     r3,=wr_ram0
        ldr     r12,=wr_ram1
        str     r2,[r1,#0*4]            ; $0000-$BFFF: ROM
        str     r2,[r1,#1*4]
        str     r2,[r1,#2*4]
        str     r2,[r1,#3*4]
        str     r2,[r1,#4*4]
        str     r2,[r1,#5*4]
        str     r3,[r1,#6*4]            ; $C000-$DFFF: RAM
        str     r12,[r1,#7*4]           ; $E000-$FFFF: RAM mirror
        ldmfd   sp!,{r4,pc}

;----------------------------------------------------------------------------
; int32 z80b_run(z80b_ctx *ctx, int32 lines)
; Runs the Z80 for the given number of scanlines (at least 1). Returns 0
; when every line ran, 1 when an unimplemented opcode stopped the run.
;----------------------------------------------------------------------------
z80b_run
        stmfd   sp!,{r4-r11,lr}
        add     globalptr,r0,#64*4
        str     r1,lines_left
        ldr     r0,=line_done
        str     r0,nexttimeout
        adr     r0,cpuregs
        ldmia   r0,{r3-r9,r11}
        encodePC
        add     cycles,cycles,#LINE_CYCLES*CYCLE
        fetch_force

line_done                               ; the current scanline is used up
        ldr     r0,lines_left
        subs    r0,r0,#1
        str     r0,lines_left
        addne   cycles,cycles,#LINE_CYCLES*CYCLE
        ldrneb  r0,[z80pc],#1
        ldrne   pc,[z80optbl,r0,lsl#2]
        mov     r0,#0
run_exit                                ; r0 = return code
        ldr     r1,lastbank
        sub     z80pc,z80pc,r1
        adr     r1,cpuregs
        stmia   r1,{r3-r9,r11}
        ldmfd   sp!,{r4-r11,pc}

op_unimpl                               ; r0 = opcode
        str     r0,stop_op
        sub     z80pc,z80pc,#1          ; back to the opcode
        mov     r0,#1
        b       run_exit

op_unimpl_dd                            ; r0 = opcode after the DD prefix
        orr     r0,r0,#0xDD00
        str     r0,stop_op
        sub     z80pc,z80pc,#2          ; back to the prefix
        mov     r0,#1
        b       run_exit

        LTORG

;----------------------------------------------------------------------------
; Memory write handlers: in addy = Z80 address, r0 = value. Return to lr.
;----------------------------------------------------------------------------
wr_rom                                  ; ROM area: ignored (no paging here)
        mov     pc,lr

wr_ram0                                 ; $C000-$DFFF
        ldr     r1,ram_c000
        strb    r0,[r1,addy]
        mov     pc,lr

wr_ram1                                 ; $E000-$FFFF, mirror of the RAM
        ldr     r1,ram_e000
        strb    r0,[r1,addy]
        mov     r1,#0x00040000
        adds    r1,r1,addy,lsl#16
        movmi   pc,lr                   ; below $FFFC
        mov     pc,lr                   ; $FFFC-$FFFF: paging registers, not modelled

;----------------------------------------------------------------------------
; Opcode handlers
;----------------------------------------------------------------------------
_00                                     ; NOP
        fetch   4

_01                                     ; LD BC,nn
        opLDIM16
        mov     z80bc,r0,lsl#16
        fetch   10

_06                                     ; LD B,n
        opLDIM8H z80bc
        fetch   7

_0B                                     ; DEC BC
        sub     z80bc,z80bc,#0x00010000
        fetch   6

_10                                     ; DJNZ e
        sub     z80bc,z80bc,#0x01000000
        tst     z80bc,#0xFF000000
        ldrb    r0,[z80pc],#1
        mov     r0,r0,lsl#24
        addne   z80pc,z80pc,r0,asr#24
        subne   cycles,cycles,#5*CYCLE
        fetch   8

_11                                     ; LD DE,nn
        opLDIM16
        mov     z80de,r0,lsl#16
        fetch   10

_12                                     ; LD (DE),A
        mov     r0,z80a,lsr#24
        mov     addy,z80de,lsr#16
        writemem8
        fetch   7

_13                                     ; INC DE
        add     z80de,z80de,#0x00010000
        fetch   6

_20                                     ; JR NZ,e
        tst     z80f,#PSR_Z
        ldrb    r0,[z80pc],#1
        mov     r0,r0,lsl#24
        addeq   z80pc,z80pc,r0,asr#24
        subeq   cycles,cycles,#5*CYCLE
        fetch   7

_21                                     ; LD HL,nn
        opLDIM16
        mov     z80hl,r0,lsl#16
        fetch   10

_23                                     ; INC HL
        add     z80hl,z80hl,#0x00010000
        fetch   6

_31                                     ; LD SP,nn
        opLDIM16
        mov     z80sp,r0,lsl#16
        fetch   10

_32                                     ; LD (nn),A
        ldrb    addy,[z80pc],#1
        ldrb    r0,[z80pc],#1
        orr     addy,addy,r0,lsl#8
        mov     r0,z80a,lsr#24
        writemem8
        fetch   13

_38                                     ; JR C,e
        tst     z80f,#PSR_C
        ldrb    r0,[z80pc],#1
        mov     r0,r0,lsl#24
        addne   z80pc,z80pc,r0,asr#24
        subne   cycles,cycles,#5*CYCLE
        fetch   7

_3A                                     ; LD A,(nn)
        ldrb    addy,[z80pc],#1
        ldrb    r0,[z80pc],#1
        orr     addy,addy,r0,lsl#8
        readmem8
        mov     z80a,r0,lsl#24
        fetch   13

_3C                                     ; INC A
        opINC8  z80a
        fetch   4

_3D                                     ; DEC A
        opDEC8  z80a
        fetch   4

_3E                                     ; LD A,n
        ldrb    r0,[z80pc],#1
        mov     z80a,r0,lsl#24
        fetch   7

_4F                                     ; LD C,A
        and     z80bc,z80bc,#0xFF000000
        orr     z80bc,z80bc,z80a,lsr#8
        fetch   4

_77                                     ; LD (HL),A
        mov     addy,z80hl,lsr#16
_DD77_                                  ; LD (IX+d),A, addy already computed
        mov     r0,z80a,lsr#24
        writemem8
        fetch   7

_78                                     ; LD A,B
        and     z80a,z80bc,#0xFF000000
        fetch   4

_7E                                     ; LD A,(HL)
        mov     addy,z80hl,lsr#16
_DD7E_                                  ; LD A,(IX+d), addy already computed
        readmem8
        mov     z80a,r0,lsl#24
        fetch   7

_86                                     ; ADD A,(HL)
        mov     addy,z80hl,lsr#16
_DD86_                                  ; ADD A,(IX+d), addy already computed
        readmem8
        opADD   r0,24
        fetch   7

_AF                                     ; XOR A
        mov     z80a,#0
        mov     z80f,#PSR_Z|PSR_P
        fetch   4

_B1                                     ; OR C
        orr     z80a,z80a,z80bc,lsl#8
        adr     r1,pzst
        ldrb    z80f,[r1,z80a,lsr#24]
        fetch   4

_C1                                     ; POP BC
        pop16   z80bc
        mov     z80bc,z80bc,lsl#16
        fetch   10

_C3                                     ; JP nn
        ldrb    r0,[z80pc],#1
        ldrb    z80pc,[z80pc]
        orr     z80pc,r0,z80pc,lsl#8
        encodePC
        fetch   10

_C5                                     ; PUSH BC
        mov     r0,z80bc,lsr#16
        push16
        fetch   11

_C9                                     ; RET
        pop16   z80pc
        encodePC
        fetch   10

_CD                                     ; CALL nn
        ldrb    r1,[z80pc],#1
        ldrb    r2,[z80pc],#1
        ldr     r0,lastbank
        sub     r0,z80pc,r0             ; return address
        orr     z80pc,r1,r2,lsl#8
        push16
        encodePC
        fetch   17

_DD                                     ; IX prefix
        adr     z80xy,z80_ix
        adr     r1,ddz
        ldrb    r0,[z80pc],#1
        ldr     pc,[r1,r0,lsl#2]

_E6                                     ; AND n
        ldrb    r0,[z80pc],#1
        and     z80a,z80a,r0,lsl#24
        adr     r1,pzst
        ldrb    z80f,[r1,z80a,lsr#24]
        orr     z80f,z80f,#PSR_H
        fetch   7

_FE                                     ; CP n
        ldrb    r0,[z80pc],#1
        opCP    r0,24
        fetch   7

;----------------------------------------------------------------------------
; DD (IX) prefixed opcodes. The prefix costs no cycles on its own: each
; handler counts the whole instruction.
;----------------------------------------------------------------------------
_DD19                                   ; ADD IX,DE
        ldr     r0,[z80xy]
        opADD16 r0,z80de
        str     r0,[z80xy]
        fetch   15

_DD21                                   ; LD IX,nn
        opLDIM16
        mov     r0,r0,lsl#16
        str     r0,[z80xy]
        fetch   14

_DD77                                   ; LD (IX+d),A
        CalcIXd
        eatcycles 12
        b       _DD77_

_DD7E                                   ; LD A,(IX+d)
        CalcIXd
        eatcycles 12
        b       _DD7E_

_DD86                                   ; ADD A,(IX+d)
        CalcIXd
        eatcycles 12
        b       _DD86_

;----------------------------------------------------------------------------
; Opcode lists used by z80b_setup: pairs of (opcode, handler), -1 ends.
;----------------------------------------------------------------------------
op_list
        DCD     0x00,_00, 0x01,_01, 0x06,_06, 0x0B,_0B, 0x10,_10, 0x11,_11
        DCD     0x12,_12, 0x13,_13, 0x20,_20, 0x21,_21, 0x23,_23, 0x31,_31
        DCD     0x32,_32, 0x38,_38, 0x3A,_3A, 0x3C,_3C, 0x3D,_3D, 0x3E,_3E
        DCD     0x4F,_4F, 0x77,_77, 0x78,_78, 0x7E,_7E, 0x86,_86, 0xAF,_AF
        DCD     0xB1,_B1, 0xC1,_C1, 0xC3,_C3, 0xC5,_C5, 0xC9,_C9, 0xCD,_CD
        DCD     0xDD,_DD, 0xE6,_E6, 0xFE,_FE
        DCD     -1,0

dd_list
        DCD     0x19,_DD19, 0x21,_DD21, 0x77,_DD77, 0x7E,_DD7E, 0x86,_DD86
        DCD     -1,0

;----------------------------------------------------------------------------
; Raw ARM60 throughput.
;----------------------------------------------------------------------------
; void arm_loop_alu(int32 iterations): 16 data-processing + 1 branch each.
arm_loop_alu
        mov     r1,#0
alu_loop
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        add     r1,r1,#1
        subs    r0,r0,#1
        bne     alu_loop
        mov     pc,lr

; void arm_loop_ldr(const uint32 *table, int32 iterations):
; 8 LDR + SUBS + branch each.
arm_loop_ldr
        stmfd   sp!,{r4,lr}
ldr_loop
        ldr     r2,[r0,#0]
        ldr     r3,[r0,#4]
        ldr     r12,[r0,#8]
        ldr     r4,[r0,#12]
        ldr     r2,[r0,#16]
        ldr     r3,[r0,#20]
        ldr     r12,[r0,#24]
        ldr     r4,[r0,#28]
        subs    r1,r1,#1
        bne     ldr_loop
        ldmfd   sp!,{r4,pc}

; void arm_fill_words(uint32 *dst, int32 blocks, uint32 value):
; writes blocks x 32 bytes (at least one block) with STM.
arm_fill_words
        stmfd   sp!,{r4-r9,lr}
        mov     r3,r2
        mov     r4,r2
        mov     r5,r2
        mov     r6,r2
        mov     r7,r2
        mov     r8,r2
        mov     r9,r2
fill_loop
        stmia   r0!,{r2-r9}
        subs    r1,r1,#1
        bne     fill_loop
        ldmfd   sp!,{r4-r9,pc}

        END
