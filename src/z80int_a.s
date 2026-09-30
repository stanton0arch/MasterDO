; z80int_a.s - Z80 interpreter on the translator's context, in assembly.
;
; Runs the code that has not been translated, instruction by instruction,
; with the registers exactly as the generated code keeps them (see
; z80jit_emit.c): r3 F (internal layout), r4 A in bits 24-31, r5 BC, r6 DE,
; r7 HL, r9 IX, r11 SP in bits 16-31, r8 T-states left in the stretch
; times 256, r10 the global pointer; the Z80 PC is in r12 and r0-r2, lr
; are scratch. Control passes to and from translated code without any
; register traffic: a transfer whose target is translated jumps to it, and
; a translated block that misses (z80j_glue_miss) comes back here.
;
; Conventions of the translator that are followed here: T-states are
; subtracted per instruction and the stretch ends before an instruction
; that may not fit (23 T-states, the longest); port handlers and special
; writes get the T-states left after the instruction; the undocumented
; flag bits 3 and 5 are 0; R is approximated as in the generated code.
; The flag sequences are those emitted by z80jit_emit.c.
;
; EI enables interrupts after the next instruction: the T-state counter
; is saved in the context and set to the minimum that still runs one
; instruction, so that the next instruction runs and the following check
; ends the stretch; the counter is restored on the way out (int_leave).
;
; Entries of each address reached by a transfer are counted in the hot
; table (when there is one); reaching queue_at hands the address to the C
; side (z80j_hot), which queues it, and so does every entry from sync_at
; on while the frame's budget of translations at once is open: when the
; C side refuses one, it sets the count of interpreted instructions from
; which it may be asked again (hot_sync_gate). Instructions interpreted
; are counted (int_insns) for the logs and for that budget.
;
; Context layout: CONTRACT with z80j_ctx (z80jit.h) - keep in sync
; (z80jit.c checks the offsets at compile time).

        AREA    |C$$code|, CODE, READONLY

        EXPORT  z80i_loop

        IMPORT  jit_save
        IMPORT  jit_continue
        IMPORT  z80j_glue_leave
        IMPORT  z80j_glue_halt
        IMPORT  z80j_glue_miss
        IMPORT  z80j_glue_daa
        IMPORT  z80j_stretch_end
        IMPORT  z80j_hot
        IMPORT  z80j_io_write
        IMPORT  z80j_push_slow

; Context fields, relative to the global pointer.
R_PC            EQU     0x418
IY              EQU     0x424
BC2             EQU     0x430
I_REG           EQU     0x43C
R_REG           EQU     0x440
IFF1            EQU     0x444
IFF2            EQU     0x448
IM              EQU     0x44C
LINE            EQU     0x460
HOST_SL         EQU     0x468
STATE           EQU     0x474
SCRATCH         EQU     0x494
PORT_IN         EQU     0x49C
PORT_OUT        EQU     0x4A0
HOT_TAB         EQU     0x4B4
HOT_QUEUE_AT    EQU     0x4B8
HOT_SYNC_AT     EQU     0x4BC
EI_SAVED        EQU     0x4C0
INT_LEAVE       EQU     0x4C4
INT_RUNS        EQU     0x4C8
INT_INSNS       EQU     0x4CC
HOT_SYNC_GATE   EQU     0x4D0
PZST            EQU     0x600
FENC            EQU     0x700
FDEC            EQU     0x800
LOOKUP          EQU     0x4000

; Flags as kept in r3.
PSR_S           EQU     0x08
PSR_Z           EQU     0x04
PSR_C           EQU     0x02
PSR_V           EQU     0x01
PSR_N           EQU     0x80
PSR_H           EQU     0x10

MIN_T           EQU     0x1700          ; 23 T-states times 256

;============================================================================
; Macros
;============================================================================

; Calls a C function of the translator (first argument: its state; the
; others in r1, r2). Preserves r3 and the global pointer; r12 is lost.
        MACRO
        callc   $fn
        stmfd   sp!,{r3,r10}
        ldr     r0,[r10,#STATE]
        ldr     r10,[r10,#HOST_SL]
        bl      $fn
        ldmfd   sp!,{r3,r10}
        MEND

; Next code byte into $t (0-255), PC advanced. Uses $t only.
        MACRO
        FETCH   $t
        mvn     $t,r12,lsr#8
        ldr     $t,[r10,$t,lsl#2]
        ldrb    $t,[$t,r12]
        add     r12,r12,#1
        MEND

; Next two code bytes into $t (0-65535), using $u.
        MACRO
        FETCH16 $t,$u
        FETCH   $t
        FETCH   $u
        orr     $t,$t,$u,lsl#8
        MEND

; Byte at the Z80 address in bits 16-31 of $a into $t ($t differs from $a).
        MACRO
        RDB     $t,$a
        mvn     $t,$a,lsr#24
        ldr     $t,[r10,$t,lsl#2]
        ldrb    $t,[$t,$a,lsr#16]
        MEND

; Writes r0 (bits 0-7) to the Z80 address in bits 16-31 of r1; a special
; page goes through the machine (int_wr_slow). Uses r2; the handler must
; end with next_w.
        MACRO
        WRB
        mov     r2,r1,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        teq     r2,#0
        strneb  r0,[r2,r1,lsr#16]
        bleq    int_wr_slow
        MEND

; 8-bit register $r (A B C D E H L XH XL YH YL) into bits 24-31 of $t,
; zeros below. YH and YL are read from the context.
        MACRO
        GET8    $t,$r
        [ "$r" = "A"
        mov     $t,r4
        ]
        [ "$r" = "B"
        and     $t,r5,#0xFF000000
        ]
        [ "$r" = "C"
        mov     $t,r5,lsl#8
        ]
        [ "$r" = "D"
        and     $t,r6,#0xFF000000
        ]
        [ "$r" = "E"
        mov     $t,r6,lsl#8
        ]
        [ "$r" = "H"
        and     $t,r7,#0xFF000000
        ]
        [ "$r" = "L"
        mov     $t,r7,lsl#8
        ]
        [ "$r" = "XH"
        and     $t,r9,#0xFF000000
        ]
        [ "$r" = "XL"
        mov     $t,r9,lsl#8
        ]
        [ "$r" = "YH"
        ldr     $t,[r10,#IY]
        and     $t,$t,#0xFF000000
        ]
        [ "$r" = "YL"
        ldr     $t,[r10,#IY]
        mov     $t,$t,lsl#8
        ]
        MEND

; Bits 24-31 of $t (zeros below) into 8-bit register $r. Uses lr for YH
; and YL.
        MACRO
        PUT8    $r,$t
        [ "$r" = "A"
        mov     r4,$t
        ]
        [ "$r" = "B"
        and     r5,r5,#0x00FF0000
        orr     r5,r5,$t
        ]
        [ "$r" = "C"
        and     r5,r5,#0xFF000000
        orr     r5,r5,$t,lsr#8
        ]
        [ "$r" = "D"
        and     r6,r6,#0x00FF0000
        orr     r6,r6,$t
        ]
        [ "$r" = "E"
        and     r6,r6,#0xFF000000
        orr     r6,r6,$t,lsr#8
        ]
        [ "$r" = "H"
        and     r7,r7,#0x00FF0000
        orr     r7,r7,$t
        ]
        [ "$r" = "L"
        and     r7,r7,#0xFF000000
        orr     r7,r7,$t,lsr#8
        ]
        [ "$r" = "XH"
        and     r9,r9,#0x00FF0000
        orr     r9,r9,$t
        ]
        [ "$r" = "XL"
        and     r9,r9,#0xFF000000
        orr     r9,r9,$t,lsr#8
        ]
        [ "$r" = "YH"
        ldr     lr,[r10,#IY]
        and     lr,lr,#0x00FF0000
        orr     lr,lr,$t
        str     lr,[r10,#IY]
        ]
        [ "$r" = "YL"
        ldr     lr,[r10,#IY]
        and     lr,lr,#0xFF000000
        orr     lr,lr,$t,lsr#8
        str     lr,[r10,#IY]
        ]
        MEND

; A = A op (bits 24-31 of r0), y = 0 ADD, 1 ADC, 2 SUB, 3 SBC, 4 AND,
; 5 XOR, 6 OR, 7 CP. Uses r0-r2, lr.
        MACRO
        ALUOP   $y
        [ $y = 0
        mov     r1,r4,lsl#4
        adds    r4,r4,r0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        cmn     r1,r0,lsl#4
        orrcs   r3,r3,#PSR_H
        ]
        [ $y = 1
        mov     r0,r0,lsr#24
        movs    r1,r3,lsr#2
        subcs   r0,r0,#0x100
        eor     r3,r0,r4,lsr#24
        adcs    r4,r4,r0,ror#8
        mrs     r0,cpsr
        eor     r3,r3,r4,lsr#24
        and     r3,r3,#PSR_H
        orr     r3,r3,r0,lsr#28
        ]
        [ $y = 2
        mov     r1,r4,lsl#4
        subs    r4,r4,r0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        eor     r3,r3,#PSR_C+PSR_N
        cmp     r1,r0,lsl#4
        orrcc   r3,r3,#PSR_H
        ]
        [ $y = 3
        mov     r0,r0,lsr#24
        and     r3,r3,#PSR_C
        subs    r0,r0,r3,lsl#7
        eor     r3,r0,r4,lsr#24
        sbcs    r4,r4,r0,ror#8
        mrs     r0,cpsr
        eor     r3,r3,r4,lsr#24
        and     r3,r3,#PSR_H
        orr     r3,r3,r0,lsr#28
        eor     r3,r3,#PSR_C+PSR_N
        ]
        [ $y = 4
        and     r4,r4,r0
        add     lr,r10,#PZST
        ldrb    r3,[lr,r4,lsr#24]
        orr     r3,r3,#PSR_H
        ]
        [ $y = 5
        eor     r4,r4,r0
        add     lr,r10,#PZST
        ldrb    r3,[lr,r4,lsr#24]
        ]
        [ $y = 6
        orr     r4,r4,r0
        add     lr,r10,#PZST
        ldrb    r3,[lr,r4,lsr#24]
        ]
        [ $y = 7
        mov     r1,r4,lsl#4
        cmp     r4,r0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        eor     r3,r3,#PSR_C+PSR_N
        cmp     r1,r0,lsl#4
        orrcc   r3,r3,#PSR_H
        ]
        MEND

; INC / DEC of the byte in bits 24-31 of $reg (the bits below do not
; matter), with the flags.
        MACRO
        INC8    $reg
        and     r3,r3,#PSR_C
        adds    $reg,$reg,#0x01000000
        orrmi   r3,r3,#PSR_S
        orrvs   r3,r3,#PSR_V
        orrcs   r3,r3,#PSR_Z
        tst     $reg,#0x0F000000
        orreq   r3,r3,#PSR_H
        MEND

        MACRO
        DEC8    $reg
        orr     r3,r3,#PSR_N+PSR_H+PSR_S+PSR_V+PSR_Z
        tst     $reg,#0x0F000000
        bicne   r3,r3,#PSR_H
        subs    $reg,$reg,#0x01000000
        bicpl   r3,r3,#PSR_S
        bicvc   r3,r3,#PSR_V
        bicne   r3,r3,#PSR_Z
        MEND

; $hl = $hl + $rr (16-bit registers in bits 16-31), flags of ADD HL,rr.
        MACRO
        ADD16   $hl,$rr
        [ "$hl" = "$rr"
        adds    $hl,$hl,$hl
        bic     r3,r3,#PSR_C+PSR_H+PSR_N
        orrcs   r3,r3,#PSR_C
        tst     $hl,#0x10000000
        orrne   r3,r3,#PSR_H
        |
        mov     r1,$hl,lsl#4
        adds    $hl,$hl,$rr
        bic     r3,r3,#PSR_C+PSR_H+PSR_N
        orrcs   r3,r3,#PSR_C
        cmn     r1,$rr,lsl#4
        orrcs   r3,r3,#PSR_H
        ]
        MEND

; Port access through the context's handler tables: PC saved around the
; call. PORTIN: r1 = port, returns r0 = value. PORTOUT: r0 = value, r1 =
; port, returns r0 = leave request.
        MACRO
        PORTIN
        str     r12,[r10,#R_PC]
        mov     r2,r8
        ldr     r12,[r10,#PORT_IN]
        mov     lr,pc
        ldr     pc,[r12,r1,lsl#2]
        ldr     r12,[r10,#R_PC]
        MEND

        MACRO
        PORTOUT
        str     r12,[r10,#R_PC]
        mov     r2,r8
        ldr     r12,[r10,#PORT_OUT]
        mov     lr,pc
        ldr     pc,[r12,r1,lsl#2]
        ldr     r12,[r10,#R_PC]
        MEND

; True condition cc (0 NZ, 1 Z, 2 NC, 3 C, 4 PO, 5 PE, 6 P, 7 M) branches
; to $label.
        MACRO
        BCOND   $cc,$label
        [ $cc = 0
        tst     r3,#PSR_Z
        beq     $label
        ]
        [ $cc = 1
        tst     r3,#PSR_Z
        bne     $label
        ]
        [ $cc = 2
        tst     r3,#PSR_C
        beq     $label
        ]
        [ $cc = 3
        tst     r3,#PSR_C
        bne     $label
        ]
        [ $cc = 4
        tst     r3,#PSR_V
        beq     $label
        ]
        [ $cc = 5
        tst     r3,#PSR_V
        bne     $label
        ]
        [ $cc = 6
        tst     r3,#PSR_S
        beq     $label
        ]
        [ $cc = 7
        tst     r3,#PSR_S
        bne     $label
        ]
        MEND

;============================================================================
; Handler macros
;============================================================================

; LD $d,$s in $t T-states.
        MACRO
$lab    LDRR    $d,$s,$t
$lab
        GET8    r0,$s
        PUT8    $d,r0
        sub     r8,r8,#$t*256
        b       next
        MEND

; LD $d,n
        MACRO
$lab    LDRN    $d,$t
$lab
        FETCH   r0
        mov     r0,r0,lsl#24
        PUT8    $d,r0
        sub     r8,r8,#$t*256
        b       next
        MEND

; ALU A,$s
        MACRO
$lab    ALUR    $y,$s,$t
$lab
        GET8    r0,$s
        ALUOP   $y
        sub     r8,r8,#$t*256
        b       next
        MEND

; INC / DEC $r
        MACRO
$lab    INCR    $r,$t
$lab
        GET8    r0,$r
        INC8    r0
        PUT8    $r,r0
        sub     r8,r8,#$t*256
        b       next
        MEND

        MACRO
$lab    DECR    $r,$t
$lab
        GET8    r0,$r
        DEC8    r0
        PUT8    $r,r0
        sub     r8,r8,#$t*256
        b       next
        MEND

; Tails of the memory operand instructions: r1 = address (bits 16-31).
; LD $d,(r1)
        MACRO
$lab    LDRM    $d
$lab
        RDB     r0,r1
        mov     r0,r0,lsl#24
        PUT8    $d,r0
        sub     r8,r8,#7*256
        b       next
        MEND

; LD (r1),$s
        MACRO
$lab    LDMR    $s
$lab
        GET8    r0,$s
        mov     r0,r0,lsr#24
        WRB
        sub     r8,r8,#7*256
        b       next_w
        MEND

; ALU A,(r1)
        MACRO
$lab    ALUM    $y
$lab
        RDB     r0,r1
        mov     r0,r0,lsl#24
        ALUOP   $y
        sub     r8,r8,#7*256
        b       next
        MEND

; ALU A,n
        MACRO
$lab    ALUN    $y
$lab
        FETCH   r0
        mov     r0,r0,lsl#24
        ALUOP   $y
        sub     r8,r8,#7*256
        b       next
        MEND

; JR cc,e
        MACRO
$lab    JRCC    $cc
$lab
        sub     r8,r8,#7*256
        BCOND   $cc,jr_taken
        add     r12,r12,#1
        b       next
        MEND

; JP cc,nn
        MACRO
$lab    JPCC    $cc
$lab
        sub     r8,r8,#10*256
        BCOND   $cc,jp_taken
        add     r12,r12,#2
        b       next
        MEND

; CALL cc,nn
        MACRO
$lab    CALLCC  $cc
$lab
        sub     r8,r8,#10*256
        BCOND   $cc,call_taken
        add     r12,r12,#2
        b       next
        MEND

; RET cc
        MACRO
$lab    RETCC   $cc
$lab
        sub     r8,r8,#5*256
        BCOND   $cc,ret_taken
        b       next
        MEND

; RST p
        MACRO
$lab    RST     $p
$lab
        sub     r8,r8,#11*256
        mov     r0,r12
        bl      int_push
        mov     r12,#$p
        b       int_jump
        MEND

; Memory operand of a DD / FD prefixed instruction: r1 = IX+d or IY+d,
; 12 T-states charged, then the tail.
        MACRO
$lab    DDMEM   $ixd,$tail
$lab
        add     r12,r12,#1
        bl      $ixd
        b       $tail
        MEND

;============================================================================
; Main loop
;============================================================================

z80i_loop
        ldr     r0,[r10,#INT_RUNS]
        add     r0,r0,#1
        str     r0,[r10,#INT_RUNS]

; Next instruction: the stretch must have room for the longest one.
next
        cmp     r8,#MIN_T
        blt     int_done
        ldr     r0,[r10,#INT_INSNS]
        add     r0,r0,#1
        str     r0,[r10,#INT_INSNS]
        bic     r12,r12,#0x10000
        mvn     r0,r12,lsr#8
        ldr     r0,[r10,r0,lsl#2]
        ldrb    r0,[r0,r12]
        add     r12,r12,#1
        adrl    lr,optable
        ldr     pc,[lr,r0,lsl#2]

; After an instruction that may have written a special page.
next_w
        ldr     r0,[r10,#INT_LEAVE]
        teq     r0,#0
        beq     next
        mov     r0,#0
        str     r0,[r10,#INT_LEAVE]
        b       int_leave

; Transfer of control to r12: translated code is entered directly, else
; the entry is counted and interpretation goes on.
int_jump
        mov     r12,r12,lsl#16
        mov     r12,r12,lsr#16
        ldr     r1,[r10,#EI_SAVED]
        teq     r1,#0
        bne     int_leave
        add     r0,r10,#LOOKUP
        ldr     r0,[r0,r12,lsl#2]
        ldr     r1,lit_miss
        cmp     r0,r1
        movne   pc,r0
        ldr     r1,[r10,#HOT_TAB]
        teq     r1,#0
        beq     next
        ldrb    r0,[r1,r12]
        cmp     r0,#255
        addne   r0,r0,#1
        strneb  r0,[r1,r12]
        ldr     r2,[r10,#HOT_QUEUE_AT]
        cmp     r0,r2
        beq     int_hot
        ldr     r2,[r10,#HOT_SYNC_AT]
        cmp     r0,r2
        blt     next
        ldr     r2,[r10,#INT_INSNS]
        ldr     r1,[r10,#HOT_SYNC_GATE]
        cmp     r2,r1
        bcc     next
int_hot
        mov     r0,r12
        bl      jit_save
        callc   z80j_hot
        b       jit_continue
lit_miss
        DCD     z80j_glue_miss

; The stretch cannot take the next instruction.
int_done
        ldr     r0,[r10,#EI_SAVED]
        teq     r0,#0
        bne     int_leave
        mov     r0,r12
        bl      jit_save
        callc   z80j_stretch_end
        b       jit_continue

; Leaves to check the interrupt lines (after EI + 1, RETN, a port or a
; write that asked for it); the counter saved by EI is restored first.
int_leave
        ldr     r0,[r10,#EI_SAVED]
        teq     r0,#0
        beq     int_leave2
        sub     r1,r8,#MIN_T
        add     r8,r0,r1
        mov     r1,#0
        str     r1,[r10,#EI_SAVED]
int_leave2
        mov     r0,r12
        mov     r1,#0
        b       z80j_glue_leave

; HALT: the rest of the stretch is idle.
int_halt
        ldr     r0,[r10,#EI_SAVED]
        teq     r0,#0
        subne   r1,r8,#MIN_T
        addne   r8,r0,r1
        movne   r0,#0
        strne   r0,[r10,#EI_SAVED]
        mov     r0,r12
        b       z80j_glue_halt

; Write to a special page: r0 = value, r1 = address (bits 16-31).
int_wr_slow
        stmfd   sp!,{r1,r12,lr}
        mov     r2,r0
        mov     r1,r1,lsr#16
        callc   z80j_io_write
        cmp     r0,#1                   ; Z80J_WRITE_LEAVE
        moveq   r0,#1
        streq   r0,[r10,#INT_LEAVE]
        ldmfd   sp!,{r1,r12,pc}

; Pushes the 16-bit value in r0; keeps r1.
int_push
        sub     r11,r11,#0x20000
        mov     r2,r11,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        teq     r2,#0
        beq     int_push_slow
        add     r2,r2,r11,lsr#16
        strb    r0,[r2]
        mov     r0,r0,lsr#8
        strb    r0,[r2,#1]
        mov     pc,lr
int_push_slow
        stmfd   sp!,{r1,r12,lr}
        mov     r2,r0
        mov     r1,r11
        callc   z80j_push_slow
        ldmfd   sp!,{r1,r12,pc}

; Pops 16 bits into r0; uses r1, r2.
int_pop
        mvn     r0,r11,lsr#24
        ldr     r1,[r10,r0,lsl#2]
        ldrb    r2,[r1,r11,lsr#16]
        add     r11,r11,#0x10000
        mvn     r0,r11,lsr#24
        ldr     r1,[r10,r0,lsl#2]
        ldrb    r0,[r1,r11,lsr#16]
        add     r11,r11,#0x10000
        orr     r0,r2,r0,lsl#8
        mov     pc,lr

; r1 = IX+d or IY+d (bits 16-31), d fetched, 12 T-states charged.
ixd_x
        FETCH   r0
        mov     r0,r0,lsl#24
        add     r1,r9,r0,asr#8
        sub     r8,r8,#12*256
        mov     pc,lr
ixd_y
        FETCH   r0
        mov     r0,r0,lsl#24
        ldr     r1,[r10,#IY]
        add     r1,r1,r0,asr#8
        sub     r8,r8,#12*256
        mov     pc,lr

; Taken conditional transfers.
jr_taken
        sub     r8,r8,#5*256
op_jr
        FETCH   r0
        mov     r0,r0,lsl#24
        add     r12,r12,r0,asr#24
        b       int_jump
jp_taken
op_jp
        FETCH16 r0,r1
        mov     r12,r0
        b       int_jump
call_taken
        sub     r8,r8,#7*256
op_call
        FETCH16 r1,r0
        mov     r0,r12
        bl      int_push
        mov     r12,r1
        b       int_jump
ret_taken
        sub     r8,r8,#6*256
op_ret
        bl      int_pop
        mov     r12,r0
        b       int_jump

;============================================================================
; Unprefixed instructions, x = 0
;============================================================================

op_nop
        sub     r8,r8,#4*256
        b       next

op_exaf                                 ; EX AF,AF': F' and A' below BC'
        add     r0,r10,#BC2
        ldmdb   r0,{r1,r2}
        stmdb   r0,{r3,r4}
        mov     r3,r1
        mov     r4,r2
        sub     r8,r8,#4*256
        b       next

op_djnz
        sub     r8,r8,#8*256
        sub     r5,r5,#0x01000000
        tst     r5,#0xFF000000
        bne     jr_taken
        add     r12,r12,#1
        b       next

op_20   JRCC    0
op_28   JRCC    1
op_30   JRCC    2
op_38   JRCC    3

op_01                                   ; LD BC,nn
        FETCH16 r0,r1
        mov     r5,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_11
        FETCH16 r0,r1
        mov     r6,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_21
        FETCH16 r0,r1
        mov     r7,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_31
        FETCH16 r0,r1
        mov     r11,r0,lsl#16
        sub     r8,r8,#10*256
        b       next

op_09                                   ; ADD HL,rr
        ADD16   r7,r5
        sub     r8,r8,#11*256
        b       next
op_19
        ADD16   r7,r6
        sub     r8,r8,#11*256
        b       next
op_29
        ADD16   r7,r7
        sub     r8,r8,#11*256
        b       next
op_39
        ADD16   r7,r11
        sub     r8,r8,#11*256
        b       next

op_02                                   ; LD (BC),A
        mov     r0,r4,lsr#24
        mov     r1,r5
        WRB
        sub     r8,r8,#7*256
        b       next_w
op_12                                   ; LD (DE),A
        mov     r0,r4,lsr#24
        mov     r1,r6
        WRB
        sub     r8,r8,#7*256
        b       next_w
op_0a                                   ; LD A,(BC)
        RDB     r0,r5
        mov     r4,r0,lsl#24
        sub     r8,r8,#7*256
        b       next
op_1a                                   ; LD A,(DE)
        RDB     r0,r6
        mov     r4,r0,lsl#24
        sub     r8,r8,#7*256
        b       next
op_22                                   ; LD (nn),HL
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        mov     r0,r7,lsr#16
        and     r0,r0,#0xFF
        WRB
        add     r1,r1,#0x10000
        mov     r0,r7,lsr#24
        WRB
        sub     r8,r8,#16*256
        b       next_w
op_2a                                   ; LD HL,(nn)
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        RDB     r0,r1
        add     r1,r1,#0x10000
        RDB     r2,r1
        mov     r7,r0,lsl#16
        orr     r7,r7,r2,lsl#24
        sub     r8,r8,#16*256
        b       next
op_32                                   ; LD (nn),A
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        mov     r0,r4,lsr#24
        WRB
        sub     r8,r8,#13*256
        b       next_w
op_3a                                   ; LD A,(nn)
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        RDB     r0,r1
        mov     r4,r0,lsl#24
        sub     r8,r8,#13*256
        b       next

op_03                                   ; INC / DEC rr
        add     r5,r5,#0x10000
        sub     r8,r8,#6*256
        b       next
op_13
        add     r6,r6,#0x10000
        sub     r8,r8,#6*256
        b       next
op_23
        add     r7,r7,#0x10000
        sub     r8,r8,#6*256
        b       next
op_33
        add     r11,r11,#0x10000
        sub     r8,r8,#6*256
        b       next
op_0b
        sub     r5,r5,#0x10000
        sub     r8,r8,#6*256
        b       next
op_1b
        sub     r6,r6,#0x10000
        sub     r8,r8,#6*256
        b       next
op_2b
        sub     r7,r7,#0x10000
        sub     r8,r8,#6*256
        b       next
op_3b
        sub     r11,r11,#0x10000
        sub     r8,r8,#6*256
        b       next

op_04   INCR    B,4
op_0c   INCR    C,4
op_14   INCR    D,4
op_1c   INCR    E,4
op_24   INCR    H,4
op_2c   INCR    L,4
op_3c
        INC8    r4
        sub     r8,r8,#4*256
        b       next
op_05   DECR    B,4
op_0d   DECR    C,4
op_15   DECR    D,4
op_1d   DECR    E,4
op_25   DECR    H,4
op_2d   DECR    L,4
op_3d
        DEC8    r4
        sub     r8,r8,#4*256
        b       next

hl_34   mov     r1,r7                   ; INC (HL)
tail_34
        RDB     r0,r1
        mov     r0,r0,lsl#24
        INC8    r0
        mov     r0,r0,lsr#24
        WRB
        sub     r8,r8,#11*256
        b       next_w
hl_35   mov     r1,r7                   ; DEC (HL)
tail_35
        RDB     r0,r1
        mov     r0,r0,lsl#24
        DEC8    r0
        mov     r0,r0,lsr#24
        WRB
        sub     r8,r8,#11*256
        b       next_w

op_06   LDRN    B,7
op_0e   LDRN    C,7
op_16   LDRN    D,7
op_1e   LDRN    E,7
op_26   LDRN    H,7
op_2e   LDRN    L,7
op_3e   LDRN    A,7
hl_36   mov     r1,r7                   ; LD (HL),n
tail_36
        FETCH   r0
        WRB
        sub     r8,r8,#10*256
        b       next_w

op_rlca
        bic     r3,r3,#PSR_H+PSR_N+PSR_C
        movs    r4,r4,lsl#1
        orrcs   r4,r4,#0x01000000
        orrcs   r3,r3,#PSR_C
        sub     r8,r8,#4*256
        b       next
op_rrca
        movs    r0,r4,lsr#25
        orrcs   r0,r0,#0x80
        mov     r4,r0,lsl#24
        bic     r3,r3,#PSR_H+PSR_N+PSR_C
        orrcs   r3,r3,#PSR_C
        sub     r8,r8,#4*256
        b       next
op_rla
        tst     r3,#PSR_C
        orrne   r4,r4,#0x00800000
        movs    r4,r4,lsl#1
        bic     r3,r3,#PSR_H+PSR_N+PSR_C
        orrcs   r3,r3,#PSR_C
        sub     r8,r8,#4*256
        b       next
op_rra
        mov     r0,r4,lsr#24
        tst     r3,#PSR_C
        orrne   r0,r0,#0x100
        movs    r0,r0,lsr#1
        mov     r4,r0,lsl#24
        bic     r3,r3,#PSR_H+PSR_N+PSR_C
        orrcs   r3,r3,#PSR_C
        sub     r8,r8,#4*256
        b       next
op_daa
        stmfd   sp!,{r12,lr}
        bl      z80j_glue_daa
        ldmfd   sp!,{r12,lr}
        sub     r8,r8,#4*256
        b       next
op_cpl
        eor     r4,r4,#0xFF000000
        orr     r3,r3,#PSR_H+PSR_N
        sub     r8,r8,#4*256
        b       next
op_scf
        bic     r3,r3,#PSR_H+PSR_N
        orr     r3,r3,#PSR_C
        sub     r8,r8,#4*256
        b       next
op_ccf
        and     r0,r3,#PSR_C
        bic     r3,r3,#PSR_H+PSR_N
        eor     r3,r3,#PSR_C
        orr     r3,r3,r0,lsl#3
        sub     r8,r8,#4*256
        b       next

;============================================================================
; x = 1: LD r,r' and HALT
;============================================================================

op_40   LDRR    B,B,4
op_41   LDRR    B,C,4
op_42   LDRR    B,D,4
op_43   LDRR    B,E,4
op_44   LDRR    B,H,4
op_45   LDRR    B,L,4
op_47   LDRR    B,A,4
op_48   LDRR    C,B,4
op_49   LDRR    C,C,4
op_4a   LDRR    C,D,4
op_4b   LDRR    C,E,4
op_4c   LDRR    C,H,4
op_4d   LDRR    C,L,4
op_4f   LDRR    C,A,4
op_50   LDRR    D,B,4
op_51   LDRR    D,C,4
op_52   LDRR    D,D,4
op_53   LDRR    D,E,4
op_54   LDRR    D,H,4
op_55   LDRR    D,L,4
op_57   LDRR    D,A,4
op_58   LDRR    E,B,4
op_59   LDRR    E,C,4
op_5a   LDRR    E,D,4
op_5b   LDRR    E,E,4
op_5c   LDRR    E,H,4
op_5d   LDRR    E,L,4
op_5f   LDRR    E,A,4
op_60   LDRR    H,B,4
op_61   LDRR    H,C,4
op_62   LDRR    H,D,4
op_63   LDRR    H,E,4
op_64   LDRR    H,H,4
op_65   LDRR    H,L,4
op_67   LDRR    H,A,4
op_68   LDRR    L,B,4
op_69   LDRR    L,C,4
op_6a   LDRR    L,D,4
op_6b   LDRR    L,E,4
op_6c   LDRR    L,H,4
op_6d   LDRR    L,L,4
op_6f   LDRR    L,A,4
op_78   LDRR    A,B,4
op_79   LDRR    A,C,4
op_7a   LDRR    A,D,4
op_7b   LDRR    A,E,4
op_7c   LDRR    A,H,4
op_7d   LDRR    A,L,4
op_7f   LDRR    A,A,4

hl_46   mov     r1,r7
tail_46 LDRM    B
hl_4e   mov     r1,r7
tail_4e LDRM    C
hl_56   mov     r1,r7
tail_56 LDRM    D
hl_5e   mov     r1,r7
tail_5e LDRM    E
hl_66   mov     r1,r7
tail_66 LDRM    H
hl_6e   mov     r1,r7
tail_6e LDRM    L
hl_7e   mov     r1,r7
tail_7e LDRM    A

hl_70   mov     r1,r7
tail_70 LDMR    B
hl_71   mov     r1,r7
tail_71 LDMR    C
hl_72   mov     r1,r7
tail_72 LDMR    D
hl_73   mov     r1,r7
tail_73 LDMR    E
hl_74   mov     r1,r7
tail_74 LDMR    H
hl_75   mov     r1,r7
tail_75 LDMR    L
hl_77   mov     r1,r7
tail_77 LDMR    A

op_halt
        sub     r8,r8,#4*256
        b       int_halt

;============================================================================
; x = 2: ALU A,r
;============================================================================

op_80   ALUR    0,B,4
op_81   ALUR    0,C,4
op_82   ALUR    0,D,4
op_83   ALUR    0,E,4
op_84   ALUR    0,H,4
op_85   ALUR    0,L,4
op_87   ALUR    0,A,4
op_88   ALUR    1,B,4
op_89   ALUR    1,C,4
op_8a   ALUR    1,D,4
op_8b   ALUR    1,E,4
op_8c   ALUR    1,H,4
op_8d   ALUR    1,L,4
op_8f   ALUR    1,A,4
op_90   ALUR    2,B,4
op_91   ALUR    2,C,4
op_92   ALUR    2,D,4
op_93   ALUR    2,E,4
op_94   ALUR    2,H,4
op_95   ALUR    2,L,4
op_97   ALUR    2,A,4
op_98   ALUR    3,B,4
op_99   ALUR    3,C,4
op_9a   ALUR    3,D,4
op_9b   ALUR    3,E,4
op_9c   ALUR    3,H,4
op_9d   ALUR    3,L,4
op_9f   ALUR    3,A,4
op_a0   ALUR    4,B,4
op_a1   ALUR    4,C,4
op_a2   ALUR    4,D,4
op_a3   ALUR    4,E,4
op_a4   ALUR    4,H,4
op_a5   ALUR    4,L,4
op_a7   ALUR    4,A,4
op_a8   ALUR    5,B,4
op_a9   ALUR    5,C,4
op_aa   ALUR    5,D,4
op_ab   ALUR    5,E,4
op_ac   ALUR    5,H,4
op_ad   ALUR    5,L,4
op_af   ALUR    5,A,4
op_b0   ALUR    6,B,4
op_b1   ALUR    6,C,4
op_b2   ALUR    6,D,4
op_b3   ALUR    6,E,4
op_b4   ALUR    6,H,4
op_b5   ALUR    6,L,4
op_b7   ALUR    6,A,4
op_b8   ALUR    7,B,4
op_b9   ALUR    7,C,4
op_ba   ALUR    7,D,4
op_bb   ALUR    7,E,4
op_bc   ALUR    7,H,4
op_bd   ALUR    7,L,4
op_bf   ALUR    7,A,4

hl_86   mov     r1,r7
tail_86 ALUM    0
hl_8e   mov     r1,r7
tail_8e ALUM    1
hl_96   mov     r1,r7
tail_96 ALUM    2
hl_9e   mov     r1,r7
tail_9e ALUM    3
hl_a6   mov     r1,r7
tail_a6 ALUM    4
hl_ae   mov     r1,r7
tail_ae ALUM    5
hl_b6   mov     r1,r7
tail_b6 ALUM    6
hl_be   mov     r1,r7
tail_be ALUM    7

op_c6   ALUN    0
op_ce   ALUN    1
op_d6   ALUN    2
op_de   ALUN    3
op_e6   ALUN    4
op_ee   ALUN    5
op_f6   ALUN    6
op_fe   ALUN    7

;============================================================================
; x = 3
;============================================================================

op_c0   RETCC   0
op_c8   RETCC   1
op_d0   RETCC   2
op_d8   RETCC   3
op_e0   RETCC   4
op_e8   RETCC   5
op_f0   RETCC   6
op_f8   RETCC   7

op_c1                                   ; POP rr
        bl      int_pop
        mov     r5,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_d1
        bl      int_pop
        mov     r6,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_e1
        bl      int_pop
        mov     r7,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
op_f1                                   ; POP AF
        bl      int_pop
        and     r1,r0,#0xFF
        add     r2,r10,#FDEC
        ldrb    r3,[r2,r1]
        mov     r4,r0,lsr#8
        mov     r4,r4,lsl#24
        sub     r8,r8,#10*256
        b       next

op_c9                                   ; RET
        sub     r8,r8,#10*256
        b       op_ret
op_exx
        add     r0,r10,#BC2
        ldmia   r0,{r1,r2,lr}
        stmia   r0,{r5,r6,r7}
        mov     r5,r1
        mov     r6,r2
        mov     r7,lr
        sub     r8,r8,#4*256
        b       next
op_e9                                   ; JP (HL)
        sub     r8,r8,#4*256
        mov     r12,r7,lsr#16
        b       int_jump
op_f9                                   ; LD SP,HL
        mov     r11,r7
        sub     r8,r8,#6*256
        b       next

op_c2   JPCC    0
op_ca   JPCC    1
op_d2   JPCC    2
op_da   JPCC    3
op_e2   JPCC    4
op_ea   JPCC    5
op_f2   JPCC    6
op_fa   JPCC    7

op_c3                                   ; JP nn
        sub     r8,r8,#10*256
        b       op_jp
op_d3                                   ; OUT (n),A
        FETCH   r1
        mov     r0,r4,lsr#24
        sub     r8,r8,#11*256
        PORTOUT
        teq     r0,#0
        bne     int_leave
        b       next
op_db                                   ; IN A,(n)
        FETCH   r1
        sub     r8,r8,#11*256
        PORTIN
        mov     r4,r0,lsl#24
        b       next
op_e3                                   ; EX (SP),HL
        RDB     r0,r11
        add     r1,r11,#0x10000
        RDB     r2,r1
        orr     r2,r0,r2,lsl#8
        str     r2,[r10,#SCRATCH]
        mov     r0,r7,lsr#16
        and     r0,r0,#0xFF
        mov     r1,r11
        WRB
        add     r1,r11,#0x10000
        mov     r0,r7,lsr#24
        WRB
        ldr     r7,[r10,#SCRATCH]
        mov     r7,r7,lsl#16
        sub     r8,r8,#19*256
        b       next_w
op_eb                                   ; EX DE,HL
        mov     r0,r6
        mov     r6,r7
        mov     r7,r0
        sub     r8,r8,#4*256
        b       next
op_di
        mov     r0,#0
        str     r0,[r10,#IFF1]
        str     r0,[r10,#IFF2]
        sub     r8,r8,#4*256
        b       next
op_ei
        mov     r0,#1
        str     r0,[r10,#IFF1]
        str     r0,[r10,#IFF2]
        sub     r8,r8,#4*256
        ldr     r0,[r10,#EI_SAVED]
        teq     r0,#0
        subne   r0,r0,#4*256            ; already pending: this EI's time
        moveq   r0,r8
        str     r0,[r10,#EI_SAVED]
        mov     r8,#MIN_T               ; one more instruction runs
        b       next

op_c4   CALLCC  0
op_cc   CALLCC  1
op_d4   CALLCC  2
op_dc   CALLCC  3
op_e4   CALLCC  4
op_ec   CALLCC  5
op_f4   CALLCC  6
op_fc   CALLCC  7

op_c5                                   ; PUSH rr
        mov     r0,r5,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
op_d5
        mov     r0,r6,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
op_e5
        mov     r0,r7,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
op_f5                                   ; PUSH AF
        add     r1,r10,#FENC
        ldrb    r0,[r1,r3]
        orr     r0,r0,r4,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
op_cd                                   ; CALL nn
        sub     r8,r8,#17*256
        b       op_call

op_c7   RST     0x00
op_cf   RST     0x08
op_d7   RST     0x10
op_df   RST     0x18
op_e7   RST     0x20
op_ef   RST     0x28
op_f7   RST     0x30
op_ff   RST     0x38

;============================================================================
; CB prefix: rotations, shifts, BIT, RES, SET. The operand is fetched by
; cb_get (r2 = z), the operation is dispatched on x * 8 + y, and the result
; in r0 is stored by cb_put (r2 = z, or 8 + z for a DD CB / FD CB memory
; operand whose address is in the context's scratch word).
;============================================================================

op_cb
        FETCH   r0
        and     r2,r0,#7
        mov     r1,r0,lsr#3
        cmp     r2,#6
        bne     cb_reg_t
        cmp     r1,#8
        blo     cb_hl_15
        cmp     r1,#16
        bhs     cb_hl_15
        sub     r8,r8,#12*256           ; BIT b,(HL)
        b       cb_get_hl
cb_hl_15
        sub     r8,r8,#15*256
cb_get_hl
        RDB     r0,r7
        mov     r0,r0,lsl#24
        b       cb_op
cb_reg_t
        sub     r8,r8,#8*256
        adrl    lr,cb_gettab
        ldr     pc,[lr,r2,lsl#2]
cbget_b
        and     r0,r5,#0xFF000000
        b       cb_op
cbget_c
        mov     r0,r5,lsl#8
        b       cb_op
cbget_d
        and     r0,r6,#0xFF000000
        b       cb_op
cbget_e
        mov     r0,r6,lsl#8
        b       cb_op
cbget_h
        and     r0,r7,#0xFF000000
        b       cb_op
cbget_l
        mov     r0,r7,lsl#8
        b       cb_op
cbget_a
        mov     r0,r4
cb_op
        adrl    lr,cb_optab
        ldr     pc,[lr,r1,lsl#2]

; Rotations and shifts of bits 24-31 of r0: result in r0, flags S Z P of
; the result, H = N = 0, C = the bit out.
cb_rlc
        movs    r0,r0,lsl#1
        orrcs   r0,r0,#0x01000000
        b       cb_rotflags
cb_rrc
        movs    r1,r0,lsr#25
        orrcs   r1,r1,#0x80
        mov     r0,r1,lsl#24
        b       cb_rotflags
cb_rl
        tst     r3,#PSR_C
        orrne   r0,r0,#0x00800000
        movs    r0,r0,lsl#1
        b       cb_rotflags
cb_rr
        mov     r1,r0,lsr#24
        tst     r3,#PSR_C
        orrne   r1,r1,#0x100
        movs    r1,r1,lsr#1
        mov     r0,r1,lsl#24
        b       cb_rotflags
cb_sla
        movs    r0,r0,lsl#1
        b       cb_rotflags
cb_sra
        movs    r1,r0,asr#25
        mov     r0,r1,lsl#24
        b       cb_rotflags
cb_sll
        movs    r0,r0,lsl#1
        orr     r0,r0,#0x01000000
        b       cb_rotflags
cb_srl
        movs    r1,r0,lsr#25
        mov     r0,r1,lsl#24
cb_rotflags
        add     lr,r10,#PZST
        ldrb    r3,[lr,r0,lsr#24]
        orrcs   r3,r3,#PSR_C
        b       cb_put

; BIT b: flags only.
        MACRO
$lab    CBBIT   $b
$lab
        and     r3,r3,#PSR_C
        orr     r3,r3,#PSR_H
        tst     r0,#1:SHL:(24+$b)
        orreq   r3,r3,#PSR_Z+PSR_V
        [ $b = 7
        orrne   r3,r3,#PSR_S
        ]
        b       next
        MEND

cb_bit0 CBBIT   0
cb_bit1 CBBIT   1
cb_bit2 CBBIT   2
cb_bit3 CBBIT   3
cb_bit4 CBBIT   4
cb_bit5 CBBIT   5
cb_bit6 CBBIT   6
cb_bit7 CBBIT   7

        MACRO
$lab    CBRES   $b
$lab
        bic     r0,r0,#1:SHL:(24+$b)
        b       cb_put
        MEND
        MACRO
$lab    CBSET   $b
$lab
        orr     r0,r0,#1:SHL:(24+$b)
        b       cb_put
        MEND

cb_res0 CBRES   0
cb_res1 CBRES   1
cb_res2 CBRES   2
cb_res3 CBRES   3
cb_res4 CBRES   4
cb_res5 CBRES   5
cb_res6 CBRES   6
cb_res7 CBRES   7
cb_set0 CBSET   0
cb_set1 CBSET   1
cb_set2 CBSET   2
cb_set3 CBSET   3
cb_set4 CBSET   4
cb_set5 CBSET   5
cb_set6 CBSET   6
cb_set7 CBSET   7

cb_put
        adrl    lr,cb_puttab
        ldr     pc,[lr,r2,lsl#2]
cbput_b
        and     r5,r5,#0x00FF0000
        orr     r5,r5,r0
        b       next
cbput_c
        and     r5,r5,#0xFF000000
        orr     r5,r5,r0,lsr#8
        b       next
cbput_d
        and     r6,r6,#0x00FF0000
        orr     r6,r6,r0
        b       next
cbput_e
        and     r6,r6,#0xFF000000
        orr     r6,r6,r0,lsr#8
        b       next
cbput_h
        and     r7,r7,#0x00FF0000
        orr     r7,r7,r0
        b       next
cbput_l
        and     r7,r7,#0xFF000000
        orr     r7,r7,r0,lsr#8
        b       next
cbput_hl
        mov     r0,r0,lsr#24
        mov     r1,r7
        WRB
        b       next_w
cbput_a
        mov     r4,r0
        b       next

; DD CB / FD CB: the memory result also goes to register z (undocumented),
; except for z = 6.
        MACRO
$lab    CBPUTM  $r
$lab
        [ "$r" /= "M"
        PUT8    $r,r0
        ]
        mov     r0,r0,lsr#24
        ldr     r1,[r10,#SCRATCH]
        WRB
        b       next_w
        MEND

cbputm_b CBPUTM B
cbputm_c CBPUTM C
cbputm_d CBPUTM D
cbputm_e CBPUTM E
cbputm_h CBPUTM H
cbputm_l CBPUTM L
cbputm_m CBPUTM M
cbputm_a CBPUTM A

; DD CB d op: r1 = IX+d or IY+d.
dd_cb
        add     r12,r12,#1
        bl      ixd_x
        b       ddcb_common
fd_cb
        add     r12,r12,#1
        bl      ixd_y
ddcb_common
        str     r1,[r10,#SCRATCH]
        FETCH   r0
        and     r2,r0,#7
        add     r2,r2,#8
        mov     lr,r0,lsr#3
        cmp     lr,#8
        blo     ddcb_23
        cmp     lr,#16
        bhs     ddcb_23
        sub     r8,r8,#8*256            ; BIT: 20
        b       ddcb_get
ddcb_23
        sub     r8,r8,#11*256           ; others: 23
ddcb_get
        RDB     r0,r1
        mov     r0,r0,lsl#24
        mov     r1,lr
        b       cb_op

;============================================================================
; ED prefix
;============================================================================

op_ed
        FETCH   r0
        adrl    lr,edtable
        ldr     pc,[lr,r0,lsl#2]

ed_nop
        sub     r8,r8,#8*256
        b       next

; IN r,(C)
        MACRO
$lab    INRC    $r
$lab
        mov     r1,r5,lsr#16
        and     r1,r1,#0xFF
        sub     r8,r8,#12*256
        PORTIN
        add     r1,r10,#PZST
        ldrb    r1,[r1,r0]
        and     r3,r3,#PSR_C
        orr     r3,r3,r1
        [ "$r" /= "M"
        mov     r0,r0,lsl#24
        PUT8    $r,r0
        ]
        b       next
        MEND

ed_40   INRC    B
ed_48   INRC    C
ed_50   INRC    D
ed_58   INRC    E
ed_60   INRC    H
ed_68   INRC    L
ed_70   INRC    M
ed_78   INRC    A

; OUT (C),r
        MACRO
$lab    OUTCR   $r
$lab
        [ "$r" = "M"
        mov     r0,#0
        |
        GET8    r0,$r
        mov     r0,r0,lsr#24
        ]
        mov     r1,r5,lsr#16
        and     r1,r1,#0xFF
        sub     r8,r8,#12*256
        PORTOUT
        teq     r0,#0
        bne     int_leave
        b       next
        MEND

ed_41   OUTCR   B
ed_49   OUTCR   C
ed_51   OUTCR   D
ed_59   OUTCR   E
ed_61   OUTCR   H
ed_69   OUTCR   L
ed_71   OUTCR   M
ed_79   OUTCR   A

; SBC HL,rr
        MACRO
$lab    SBC16   $rr
$lab
        and     r3,r3,#PSR_C
        subs    r1,r3,r3,lsl#1
        orr     r0,$rr,r1,lsr#16
        mov     r1,r7,lsl#4
        sbcs    r7,r7,r0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        eor     r3,r3,#PSR_C+PSR_N
        cmp     r1,r0,lsl#4
        orrcc   r3,r3,#PSR_H
        sub     r8,r8,#15*256
        b       next
        MEND

; ADC HL,rr
        MACRO
$lab    ADC16   $rr
$lab
        movs    r1,r3,lsr#2
        mov     r0,$rr
        orrcs   r0,r0,#0xFF00
        orrcs   r0,r0,#0xFF
        eor     r2,r7,$rr
        adcs    r7,r7,r0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        eor     r2,r2,r7
        tst     r2,#0x10000000
        orrne   r3,r3,#PSR_H
        sub     r8,r8,#15*256
        b       next
        MEND

ed_42   SBC16   r5
ed_52   SBC16   r6
ed_62   SBC16   r7
ed_72   SBC16   r11
ed_4a   ADC16   r5
ed_5a   ADC16   r6
ed_6a   ADC16   r7
ed_7a   ADC16   r11

; LD (nn),rr / LD rr,(nn)
        MACRO
$lab    LDNNRR  $rr
$lab
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        mov     r0,$rr,lsr#16
        and     r0,r0,#0xFF
        WRB
        add     r1,r1,#0x10000
        mov     r0,$rr,lsr#24
        WRB
        sub     r8,r8,#20*256
        b       next_w
        MEND

        MACRO
$lab    LDRRNN  $rr
$lab
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        RDB     r0,r1
        add     r1,r1,#0x10000
        RDB     r2,r1
        mov     $rr,r0,lsl#16
        orr     $rr,$rr,r2,lsl#24
        sub     r8,r8,#20*256
        b       next
        MEND

ed_43   LDNNRR  r5
ed_53   LDNNRR  r6
ed_63   LDNNRR  r7
ed_73   LDNNRR  r11
ed_4b   LDRRNN  r5
ed_5b   LDRRNN  r6
ed_6b   LDRRNN  r7
ed_7b   LDRRNN  r11

ed_neg
        mov     r1,r4,lsl#4
        rsbs    r4,r4,#0
        mrs     r3,cpsr
        mov     r3,r3,lsr#28
        eor     r3,r3,#PSR_C+PSR_N
        teq     r1,#0
        orrne   r3,r3,#PSR_H
        sub     r8,r8,#8*256
        b       next

ed_retn
        ldr     r0,[r10,#IFF2]
        str     r0,[r10,#IFF1]
        bl      int_pop
        mov     r12,r0
        sub     r8,r8,#14*256
        b       int_leave

ed_im0
        mov     r0,#0
        str     r0,[r10,#IM]
        sub     r8,r8,#8*256
        b       next
ed_im1
        mov     r0,#1
        str     r0,[r10,#IM]
        sub     r8,r8,#8*256
        b       next
ed_im2
        mov     r0,#2
        str     r0,[r10,#IM]
        sub     r8,r8,#8*256
        b       next

ed_ldia                                 ; LD I,A
        mov     r0,r4,lsr#24
        str     r0,[r10,#I_REG]
        sub     r8,r8,#9*256
        b       next
ed_ldra                                 ; LD R,A
        mov     r0,r4,lsr#24
        str     r0,[r10,#R_REG]
        sub     r8,r8,#9*256
        b       next
ed_ldai                                 ; LD A,I
        sub     r8,r8,#9*256
        ldr     r0,[r10,#I_REG]
        b       ed_ldai_flags
ed_ldar                                 ; LD A,R (approximation)
        sub     r8,r8,#9*256
        ldr     r0,[r10,#LINE]
        ldr     r1,[r10,#R_REG]
        add     r0,r0,r0,lsl#3
        add     r0,r0,r8,lsr#10
        add     r0,r0,r1
        and     r0,r0,#0x7F
        and     r1,r1,#0x80
        orr     r0,r0,r1
ed_ldai_flags
        mov     r4,r0,lsl#24
        add     r1,r10,#PZST
        ldrb    r1,[r1,r0]
        and     r1,r1,#PSR_S+PSR_Z
        and     r3,r3,#PSR_C
        orr     r3,r3,r1
        ldr     r0,[r10,#IFF2]
        teq     r0,#0
        orrne   r3,r3,#PSR_V
        b       next

ed_rrd
        RDB     r0,r7
        mov     r1,r4,lsr#24
        and     r2,r1,#0x0F
        mov     r2,r2,lsl#4
        orr     r2,r2,r0,lsr#4
        and     r1,r1,#0xF0
        and     r0,r0,#0x0F
        orr     r1,r1,r0
        b       ed_rxd_end
ed_rld
        RDB     r0,r7
        mov     r1,r4,lsr#24
        and     r2,r1,#0x0F
        orr     r2,r2,r0,lsl#4
        and     r1,r1,#0xF0
        orr     r1,r1,r0,lsr#4
ed_rxd_end
        mov     r4,r1,lsl#24
        and     r0,r2,#0xFF
        add     r1,r10,#PZST
        ldrb    r1,[r1,r4,lsr#24]
        and     r3,r3,#PSR_C
        orr     r3,r3,r1
        mov     r1,r7
        WRB
        sub     r8,r8,#18*256
        b       next_w

; Block instructions: $dec 0 / 1, $rep 0 / 1.
        MACRO
$lab    LDBLK   $dec,$rep
$lab
        RDB     r0,r7
        mov     r1,r6
        WRB
        [ $dec = 0
        add     r7,r7,#0x10000
        add     r6,r6,#0x10000
        |
        sub     r7,r7,#0x10000
        sub     r6,r6,#0x10000
        ]
        sub     r5,r5,#0x10000
        bic     r3,r3,#PSR_H+PSR_N+PSR_V
        teq     r5,#0
        orrne   r3,r3,#PSR_V
        sub     r8,r8,#16*256
        [ $rep = 1
        subne   r8,r8,#5*256
        subne   r12,r12,#2
        ]
        b       next_w
        MEND

        MACRO
$lab    CPBLK   $dec,$rep
$lab
        RDB     r0,r7
        mov     r1,r4,lsl#4
        cmp     r4,r0,lsl#24
        and     r3,r3,#PSR_C
        orr     r3,r3,#PSR_N
        orrmi   r3,r3,#PSR_S
        orreq   r3,r3,#PSR_Z
        cmp     r1,r0,lsl#28
        orrcc   r3,r3,#PSR_H
        [ $dec = 0
        add     r7,r7,#0x10000
        |
        sub     r7,r7,#0x10000
        ]
        sub     r5,r5,#0x10000
        teq     r5,#0
        orrne   r3,r3,#PSR_V
        sub     r8,r8,#16*256
        [ $rep = 1
        beq     next
        tst     r3,#PSR_Z
        subeq   r8,r8,#5*256
        subeq   r12,r12,#2
        ]
        b       next
        MEND

        MACRO
$lab    INBLK   $dec,$rep
$lab
        mov     r1,r5,lsr#16
        and     r1,r1,#0xFF
        sub     r8,r8,#16*256
        PORTIN
        mov     r1,r7
        WRB
        [ $dec = 0
        add     r7,r7,#0x10000
        |
        sub     r7,r7,#0x10000
        ]
        sub     r5,r5,#0x01000000
        bic     r3,r3,#PSR_Z
        orr     r3,r3,#PSR_N
        tst     r5,#0xFF000000
        orreq   r3,r3,#PSR_Z
        [ $rep = 1
        subne   r8,r8,#5*256
        subne   r12,r12,#2
        ]
        b       next_w
        MEND

        MACRO
$lab    OUTBLK  $dec,$rep
$lab
        RDB     r0,r7
        sub     r5,r5,#0x01000000
        mov     r1,r5,lsr#16
        and     r1,r1,#0xFF
        sub     r8,r8,#16*256
        [ $dec = 0
        add     r7,r7,#0x10000
        |
        sub     r7,r7,#0x10000
        ]
        PORTOUT
        bic     r3,r3,#PSR_Z
        orr     r3,r3,#PSR_N
        tst     r5,#0xFF000000
        orreq   r3,r3,#PSR_Z
        [ $rep = 1
        subne   r8,r8,#5*256
        subne   r12,r12,#2
        ]
        teq     r0,#0
        bne     int_leave
        b       next
        MEND

ed_ldi  LDBLK   0,0
ed_ldd  LDBLK   1,0
ed_ldir LDBLK   0,1
ed_lddr LDBLK   1,1
ed_cpi  CPBLK   0,0
ed_cpd  CPBLK   1,0
ed_cpir CPBLK   0,1
ed_cpdr CPBLK   1,1
ed_ini  INBLK   0,0
ed_ind  INBLK   1,0
ed_inir INBLK   0,1
ed_indr INBLK   1,1
ed_outi OUTBLK  0,0
ed_outd OUTBLK  1,0
ed_otir OUTBLK  0,1
ed_otdr OUTBLK  1,1

;============================================================================
; DD and FD prefixes. The next opcode is looked up in the prefix's table
; without being consumed: an opcode that does not use HL makes the prefix
; a 4 T-state no-op and is decoded afresh.
;============================================================================

op_dd
        mvn     r0,r12,lsr#8
        ldr     r0,[r10,r0,lsl#2]
        ldrb    r0,[r0,r12]
        adrl    lr,ddtable
        ldr     pc,[lr,r0,lsl#2]
op_fd
        mvn     r0,r12,lsr#8
        ldr     r0,[r10,r0,lsl#2]
        ldrb    r0,[r0,r12]
        adrl    lr,fdtable
        ldr     pc,[lr,r0,lsl#2]
dd_nop
        sub     r8,r8,#4*256
        b       next

; Register forms (IXH, IXL, IYH, IYL and the pairs): the opcode is
; consumed here, the prefix costs 4 T-states more.
        MACRO
$lab    DDX     $handler
$lab
        add     r12,r12,#1
        sub     r8,r8,#4*256
        b       $handler
        MEND

; LD IX,nn / LD IY,nn
dx_21   DDX     dxr_21
dxr_21
        FETCH16 r0,r1
        mov     r9,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
fy_21   DDX     fyr_21
fyr_21
        FETCH16 r0,r1
        mov     r0,r0,lsl#16
        str     r0,[r10,#IY]
        sub     r8,r8,#10*256
        b       next

; ADD IX,rr
dx_09   DDX     dxr_09
dxr_09
        ADD16   r9,r5
        sub     r8,r8,#11*256
        b       next
dx_19   DDX     dxr_19
dxr_19
        ADD16   r9,r6
        sub     r8,r8,#11*256
        b       next
dx_29   DDX     dxr_29
dxr_29
        ADD16   r9,r9
        sub     r8,r8,#11*256
        b       next
dx_39   DDX     dxr_39
dxr_39
        ADD16   r9,r11
        sub     r8,r8,#11*256
        b       next
fy_09   DDX     fyr_09
fyr_09
        ldr     lr,[r10,#IY]
        ADD16   lr,r5
        str     lr,[r10,#IY]
        sub     r8,r8,#11*256
        b       next
fy_19   DDX     fyr_19
fyr_19
        ldr     lr,[r10,#IY]
        ADD16   lr,r6
        str     lr,[r10,#IY]
        sub     r8,r8,#11*256
        b       next
fy_29   DDX     fyr_29
fyr_29
        ldr     lr,[r10,#IY]
        ADD16   lr,lr
        str     lr,[r10,#IY]
        sub     r8,r8,#11*256
        b       next
fy_39   DDX     fyr_39
fyr_39
        ldr     lr,[r10,#IY]
        ADD16   lr,r11
        str     lr,[r10,#IY]
        sub     r8,r8,#11*256
        b       next

; LD (nn),IX / LD IX,(nn)
dx_22   DDX     dxr_22
dxr_22
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        mov     r0,r9,lsr#16
        and     r0,r0,#0xFF
        WRB
        add     r1,r1,#0x10000
        mov     r0,r9,lsr#24
        WRB
        sub     r8,r8,#16*256
        b       next_w
fy_22   DDX     fyr_22
fyr_22
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        ldr     r0,[r10,#IY]
        mov     r0,r0,lsr#16
        and     r0,r0,#0xFF
        WRB
        add     r1,r1,#0x10000
        ldr     r0,[r10,#IY]
        mov     r0,r0,lsr#24
        WRB
        sub     r8,r8,#16*256
        b       next_w
dx_2a   DDX     dxr_2a
dxr_2a
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        RDB     r0,r1
        add     r1,r1,#0x10000
        RDB     r2,r1
        mov     r9,r0,lsl#16
        orr     r9,r9,r2,lsl#24
        sub     r8,r8,#16*256
        b       next
fy_2a   DDX     fyr_2a
fyr_2a
        FETCH16 r1,r0
        mov     r1,r1,lsl#16
        RDB     r0,r1
        add     r1,r1,#0x10000
        RDB     r2,r1
        mov     r0,r0,lsl#16
        orr     r0,r0,r2,lsl#24
        str     r0,[r10,#IY]
        sub     r8,r8,#16*256
        b       next

; INC / DEC IX
dx_23   DDX     dxr_23
dxr_23
        add     r9,r9,#0x10000
        sub     r8,r8,#6*256
        b       next
dx_2b   DDX     dxr_2b
dxr_2b
        sub     r9,r9,#0x10000
        sub     r8,r8,#6*256
        b       next
fy_23   DDX     fyr_23
fyr_23
        ldr     r0,[r10,#IY]
        add     r0,r0,#0x10000
        str     r0,[r10,#IY]
        sub     r8,r8,#6*256
        b       next
fy_2b   DDX     fyr_2b
fyr_2b
        ldr     r0,[r10,#IY]
        sub     r0,r0,#0x10000
        str     r0,[r10,#IY]
        sub     r8,r8,#6*256
        b       next

; INC / DEC IXH, IXL, IYH, IYL
dx_24   DDX     dxr_24
dxr_24  INCR    XH,4
dx_2c   DDX     dxr_2c
dxr_2c  INCR    XL,4
dx_25   DDX     dxr_25
dxr_25  DECR    XH,4
dx_2d   DDX     dxr_2d
dxr_2d  DECR    XL,4
fy_24   DDX     fyr_24
fyr_24  INCR    YH,4
fy_2c   DDX     fyr_2c
fyr_2c  INCR    YL,4
fy_25   DDX     fyr_25
fyr_25  DECR    YH,4
fy_2d   DDX     fyr_2d
fyr_2d  DECR    YL,4

; LD IXH,n / LD IXL,n
dx_26   DDX     dxr_26
dxr_26  LDRN    XH,7
dx_2e   DDX     dxr_2e
dxr_2e  LDRN    XL,7
fy_26   DDX     fyr_26
fyr_26  LDRN    YH,7
fy_2e   DDX     fyr_2e
fyr_2e  LDRN    YL,7

; LD r,IXH / LD r,IXL / LD IXH,r / LD IXL,r (H and L mean IXH and IXL)
dx_44   DDX     dxr_44
dxr_44  LDRR    B,XH,4
dx_45   DDX     dxr_45
dxr_45  LDRR    B,XL,4
dx_4c   DDX     dxr_4c
dxr_4c  LDRR    C,XH,4
dx_4d   DDX     dxr_4d
dxr_4d  LDRR    C,XL,4
dx_54   DDX     dxr_54
dxr_54  LDRR    D,XH,4
dx_55   DDX     dxr_55
dxr_55  LDRR    D,XL,4
dx_5c   DDX     dxr_5c
dxr_5c  LDRR    E,XH,4
dx_5d   DDX     dxr_5d
dxr_5d  LDRR    E,XL,4
dx_60   DDX     dxr_60
dxr_60  LDRR    XH,B,4
dx_61   DDX     dxr_61
dxr_61  LDRR    XH,C,4
dx_62   DDX     dxr_62
dxr_62  LDRR    XH,D,4
dx_63   DDX     dxr_63
dxr_63  LDRR    XH,E,4
dx_64   DDX     dxr_64
dxr_64  LDRR    XH,XH,4
dx_65   DDX     dxr_65
dxr_65  LDRR    XH,XL,4
dx_67   DDX     dxr_67
dxr_67  LDRR    XH,A,4
dx_68   DDX     dxr_68
dxr_68  LDRR    XL,B,4
dx_69   DDX     dxr_69
dxr_69  LDRR    XL,C,4
dx_6a   DDX     dxr_6a
dxr_6a  LDRR    XL,D,4
dx_6b   DDX     dxr_6b
dxr_6b  LDRR    XL,E,4
dx_6c   DDX     dxr_6c
dxr_6c  LDRR    XL,XH,4
dx_6d   DDX     dxr_6d
dxr_6d  LDRR    XL,XL,4
dx_6f   DDX     dxr_6f
dxr_6f  LDRR    XL,A,4
dx_7c   DDX     dxr_7c
dxr_7c  LDRR    A,XH,4
dx_7d   DDX     dxr_7d
dxr_7d  LDRR    A,XL,4

fy_44   DDX     fyr_44
fyr_44  LDRR    B,YH,4
fy_45   DDX     fyr_45
fyr_45  LDRR    B,YL,4
fy_4c   DDX     fyr_4c
fyr_4c  LDRR    C,YH,4
fy_4d   DDX     fyr_4d
fyr_4d  LDRR    C,YL,4
fy_54   DDX     fyr_54
fyr_54  LDRR    D,YH,4
fy_55   DDX     fyr_55
fyr_55  LDRR    D,YL,4
fy_5c   DDX     fyr_5c
fyr_5c  LDRR    E,YH,4
fy_5d   DDX     fyr_5d
fyr_5d  LDRR    E,YL,4
fy_60   DDX     fyr_60
fyr_60  LDRR    YH,B,4
fy_61   DDX     fyr_61
fyr_61  LDRR    YH,C,4
fy_62   DDX     fyr_62
fyr_62  LDRR    YH,D,4
fy_63   DDX     fyr_63
fyr_63  LDRR    YH,E,4
fy_64   DDX     fyr_64
fyr_64  LDRR    YH,YH,4
fy_65   DDX     fyr_65
fyr_65  LDRR    YH,YL,4
fy_67   DDX     fyr_67
fyr_67  LDRR    YH,A,4
fy_68   DDX     fyr_68
fyr_68  LDRR    YL,B,4
fy_69   DDX     fyr_69
fyr_69  LDRR    YL,C,4
fy_6a   DDX     fyr_6a
fyr_6a  LDRR    YL,D,4
fy_6b   DDX     fyr_6b
fyr_6b  LDRR    YL,E,4
fy_6c   DDX     fyr_6c
fyr_6c  LDRR    YL,YH,4
fy_6d   DDX     fyr_6d
fyr_6d  LDRR    YL,YL,4
fy_6f   DDX     fyr_6f
fyr_6f  LDRR    YL,A,4
fy_7c   DDX     fyr_7c
fyr_7c  LDRR    A,YH,4
fy_7d   DDX     fyr_7d
fyr_7d  LDRR    A,YL,4

; ALU A,IXH / IXL / IYH / IYL
dx_84   DDX     dxr_84
dxr_84  ALUR    0,XH,4
dx_85   DDX     dxr_85
dxr_85  ALUR    0,XL,4
dx_8c   DDX     dxr_8c
dxr_8c  ALUR    1,XH,4
dx_8d   DDX     dxr_8d
dxr_8d  ALUR    1,XL,4
dx_94   DDX     dxr_94
dxr_94  ALUR    2,XH,4
dx_95   DDX     dxr_95
dxr_95  ALUR    2,XL,4
dx_9c   DDX     dxr_9c
dxr_9c  ALUR    3,XH,4
dx_9d   DDX     dxr_9d
dxr_9d  ALUR    3,XL,4
dx_a4   DDX     dxr_a4
dxr_a4  ALUR    4,XH,4
dx_a5   DDX     dxr_a5
dxr_a5  ALUR    4,XL,4
dx_ac   DDX     dxr_ac
dxr_ac  ALUR    5,XH,4
dx_ad   DDX     dxr_ad
dxr_ad  ALUR    5,XL,4
dx_b4   DDX     dxr_b4
dxr_b4  ALUR    6,XH,4
dx_b5   DDX     dxr_b5
dxr_b5  ALUR    6,XL,4
dx_bc   DDX     dxr_bc
dxr_bc  ALUR    7,XH,4
dx_bd   DDX     dxr_bd
dxr_bd  ALUR    7,XL,4

fy_84   DDX     fyr_84
fyr_84  ALUR    0,YH,4
fy_85   DDX     fyr_85
fyr_85  ALUR    0,YL,4
fy_8c   DDX     fyr_8c
fyr_8c  ALUR    1,YH,4
fy_8d   DDX     fyr_8d
fyr_8d  ALUR    1,YL,4
fy_94   DDX     fyr_94
fyr_94  ALUR    2,YH,4
fy_95   DDX     fyr_95
fyr_95  ALUR    2,YL,4
fy_9c   DDX     fyr_9c
fyr_9c  ALUR    3,YH,4
fy_9d   DDX     fyr_9d
fyr_9d  ALUR    3,YL,4
fy_a4   DDX     fyr_a4
fyr_a4  ALUR    4,YH,4
fy_a5   DDX     fyr_a5
fyr_a5  ALUR    4,YL,4
fy_ac   DDX     fyr_ac
fyr_ac  ALUR    5,YH,4
fy_ad   DDX     fyr_ad
fyr_ad  ALUR    5,YL,4
fy_b4   DDX     fyr_b4
fyr_b4  ALUR    6,YH,4
fy_b5   DDX     fyr_b5
fyr_b5  ALUR    6,YL,4
fy_bc   DDX     fyr_bc
fyr_bc  ALUR    7,YH,4
fy_bd   DDX     fyr_bd
fyr_bd  ALUR    7,YL,4

; POP IX, PUSH IX, JP (IX), LD SP,IX, EX (SP),IX
dx_e1   DDX     dxr_e1
dxr_e1
        bl      int_pop
        mov     r9,r0,lsl#16
        sub     r8,r8,#10*256
        b       next
fy_e1   DDX     fyr_e1
fyr_e1
        bl      int_pop
        mov     r0,r0,lsl#16
        str     r0,[r10,#IY]
        sub     r8,r8,#10*256
        b       next
dx_e5   DDX     dxr_e5
dxr_e5
        mov     r0,r9,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
fy_e5   DDX     fyr_e5
fyr_e5
        ldr     r0,[r10,#IY]
        mov     r0,r0,lsr#16
        bl      int_push
        sub     r8,r8,#11*256
        b       next
dx_e9   DDX     dxr_e9
dxr_e9
        sub     r8,r8,#4*256
        mov     r12,r9,lsr#16
        b       int_jump
fy_e9   DDX     fyr_e9
fyr_e9
        sub     r8,r8,#4*256
        ldr     r12,[r10,#IY]
        mov     r12,r12,lsr#16
        b       int_jump
dx_f9   DDX     dxr_f9
dxr_f9
        mov     r11,r9
        sub     r8,r8,#6*256
        b       next
fy_f9   DDX     fyr_f9
fyr_f9
        ldr     r11,[r10,#IY]
        sub     r8,r8,#6*256
        b       next
dx_e3   DDX     dxr_e3
dxr_e3
        RDB     r0,r11
        add     r1,r11,#0x10000
        RDB     r2,r1
        orr     r2,r0,r2,lsl#8
        str     r2,[r10,#SCRATCH]
        mov     r0,r9,lsr#16
        and     r0,r0,#0xFF
        mov     r1,r11
        WRB
        add     r1,r11,#0x10000
        mov     r0,r9,lsr#24
        WRB
        ldr     r9,[r10,#SCRATCH]
        mov     r9,r9,lsl#16
        sub     r8,r8,#19*256
        b       next_w
fy_e3   DDX     fyr_e3
fyr_e3
        RDB     r0,r11
        add     r1,r11,#0x10000
        RDB     r2,r1
        orr     r2,r0,r2,lsl#8
        str     r2,[r10,#SCRATCH]
        ldr     r0,[r10,#IY]
        mov     r0,r0,lsr#16
        and     r0,r0,#0xFF
        mov     r1,r11
        WRB
        add     r1,r11,#0x10000
        ldr     r0,[r10,#IY]
        mov     r0,r0,lsr#24
        WRB
        ldr     r0,[r10,#SCRATCH]
        mov     r0,r0,lsl#16
        str     r0,[r10,#IY]
        sub     r8,r8,#19*256
        b       next_w

; Memory operands (IX+d) / (IY+d): the shared tails.
dm_34   DDMEM   ixd_x,tail_34
dm_35   DDMEM   ixd_x,tail_35
dm_36
        add     r12,r12,#1
        bl      ixd_x
        add     r8,r8,#3*256            ; LD (IX+d),n: 19
        b       tail_36
dm_46   DDMEM   ixd_x,tail_46
dm_4e   DDMEM   ixd_x,tail_4e
dm_56   DDMEM   ixd_x,tail_56
dm_5e   DDMEM   ixd_x,tail_5e
dm_66   DDMEM   ixd_x,tail_66
dm_6e   DDMEM   ixd_x,tail_6e
dm_7e   DDMEM   ixd_x,tail_7e
dm_70   DDMEM   ixd_x,tail_70
dm_71   DDMEM   ixd_x,tail_71
dm_72   DDMEM   ixd_x,tail_72
dm_73   DDMEM   ixd_x,tail_73
dm_74   DDMEM   ixd_x,tail_74
dm_75   DDMEM   ixd_x,tail_75
dm_77   DDMEM   ixd_x,tail_77
dm_86   DDMEM   ixd_x,tail_86
dm_8e   DDMEM   ixd_x,tail_8e
dm_96   DDMEM   ixd_x,tail_96
dm_9e   DDMEM   ixd_x,tail_9e
dm_a6   DDMEM   ixd_x,tail_a6
dm_ae   DDMEM   ixd_x,tail_ae
dm_b6   DDMEM   ixd_x,tail_b6
dm_be   DDMEM   ixd_x,tail_be

fm_34   DDMEM   ixd_y,tail_34
fm_35   DDMEM   ixd_y,tail_35
fm_36
        add     r12,r12,#1
        bl      ixd_y
        add     r8,r8,#3*256
        b       tail_36
fm_46   DDMEM   ixd_y,tail_46
fm_4e   DDMEM   ixd_y,tail_4e
fm_56   DDMEM   ixd_y,tail_56
fm_5e   DDMEM   ixd_y,tail_5e
fm_66   DDMEM   ixd_y,tail_66
fm_6e   DDMEM   ixd_y,tail_6e
fm_7e   DDMEM   ixd_y,tail_7e
fm_70   DDMEM   ixd_y,tail_70
fm_71   DDMEM   ixd_y,tail_71
fm_72   DDMEM   ixd_y,tail_72
fm_73   DDMEM   ixd_y,tail_73
fm_74   DDMEM   ixd_y,tail_74
fm_75   DDMEM   ixd_y,tail_75
fm_77   DDMEM   ixd_y,tail_77
fm_86   DDMEM   ixd_y,tail_86
fm_8e   DDMEM   ixd_y,tail_8e
fm_96   DDMEM   ixd_y,tail_96
fm_9e   DDMEM   ixd_y,tail_9e
fm_a6   DDMEM   ixd_y,tail_a6
fm_ae   DDMEM   ixd_y,tail_ae
fm_b6   DDMEM   ixd_y,tail_b6
fm_be   DDMEM   ixd_y,tail_be

;============================================================================
; Tables
;============================================================================

optable
        DCD     op_nop, op_01, op_02, op_03, op_04, op_05, op_06, op_rlca
        DCD     op_exaf, op_09, op_0a, op_0b, op_0c, op_0d, op_0e, op_rrca
        DCD     op_djnz, op_11, op_12, op_13, op_14, op_15, op_16, op_rla
        DCD     op_jr_18, op_19, op_1a, op_1b, op_1c, op_1d, op_1e, op_rra
        DCD     op_20, op_21, op_22, op_23, op_24, op_25, op_26, op_daa
        DCD     op_28, op_29, op_2a, op_2b, op_2c, op_2d, op_2e, op_cpl
        DCD     op_30, op_31, op_32, op_33, hl_34, hl_35, hl_36, op_scf
        DCD     op_38, op_39, op_3a, op_3b, op_3c, op_3d, op_3e, op_ccf
        DCD     op_40, op_41, op_42, op_43, op_44, op_45, hl_46, op_47
        DCD     op_48, op_49, op_4a, op_4b, op_4c, op_4d, hl_4e, op_4f
        DCD     op_50, op_51, op_52, op_53, op_54, op_55, hl_56, op_57
        DCD     op_58, op_59, op_5a, op_5b, op_5c, op_5d, hl_5e, op_5f
        DCD     op_60, op_61, op_62, op_63, op_64, op_65, hl_66, op_67
        DCD     op_68, op_69, op_6a, op_6b, op_6c, op_6d, hl_6e, op_6f
        DCD     hl_70, hl_71, hl_72, hl_73, hl_74, hl_75, op_halt, hl_77
        DCD     op_78, op_79, op_7a, op_7b, op_7c, op_7d, hl_7e, op_7f
        DCD     op_80, op_81, op_82, op_83, op_84, op_85, hl_86, op_87
        DCD     op_88, op_89, op_8a, op_8b, op_8c, op_8d, hl_8e, op_8f
        DCD     op_90, op_91, op_92, op_93, op_94, op_95, hl_96, op_97
        DCD     op_98, op_99, op_9a, op_9b, op_9c, op_9d, hl_9e, op_9f
        DCD     op_a0, op_a1, op_a2, op_a3, op_a4, op_a5, hl_a6, op_a7
        DCD     op_a8, op_a9, op_aa, op_ab, op_ac, op_ad, hl_ae, op_af
        DCD     op_b0, op_b1, op_b2, op_b3, op_b4, op_b5, hl_b6, op_b7
        DCD     op_b8, op_b9, op_ba, op_bb, op_bc, op_bd, hl_be, op_bf
        DCD     op_c0, op_c1, op_c2, op_c3, op_c4, op_c5, op_c6, op_c7
        DCD     op_c8, op_c9, op_ca, op_cb, op_cc, op_cd, op_ce, op_cf
        DCD     op_d0, op_d1, op_d2, op_d3, op_d4, op_d5, op_d6, op_d7
        DCD     op_d8, op_exx, op_da, op_db, op_dc, op_dd, op_de, op_df
        DCD     op_e0, op_e1, op_e2, op_e3, op_e4, op_e5, op_e6, op_e7
        DCD     op_e8, op_e9, op_ea, op_eb, op_ec, op_ed, op_ee, op_ef
        DCD     op_f0, op_f1, op_f2, op_di, op_f4, op_f5, op_f6, op_f7
        DCD     op_f8, op_f9, op_fa, op_ei, op_fc, op_fd, op_fe, op_ff

op_jr_18
        sub     r8,r8,#12*256
        b       op_jr

cb_gettab
        DCD     cbget_b, cbget_c, cbget_d, cbget_e, cbget_h, cbget_l, cb_get_hl, cbget_a
cb_optab
        DCD     cb_rlc, cb_rrc, cb_rl, cb_rr, cb_sla, cb_sra, cb_sll, cb_srl
        DCD     cb_bit0, cb_bit1, cb_bit2, cb_bit3, cb_bit4, cb_bit5, cb_bit6, cb_bit7
        DCD     cb_res0, cb_res1, cb_res2, cb_res3, cb_res4, cb_res5, cb_res6, cb_res7
        DCD     cb_set0, cb_set1, cb_set2, cb_set3, cb_set4, cb_set5, cb_set6, cb_set7
cb_puttab
        DCD     cbput_b, cbput_c, cbput_d, cbput_e, cbput_h, cbput_l, cbput_hl, cbput_a
        DCD     cbputm_b, cbputm_c, cbputm_d, cbputm_e, cbputm_h, cbputm_l, cbputm_m, cbputm_a

edtable
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_40, ed_41, ed_42, ed_43, ed_neg, ed_retn, ed_im0, ed_ldia
        DCD     ed_48, ed_49, ed_4a, ed_4b, ed_neg, ed_retn, ed_im0, ed_ldra
        DCD     ed_50, ed_51, ed_52, ed_53, ed_neg, ed_retn, ed_im1, ed_ldai
        DCD     ed_58, ed_59, ed_5a, ed_5b, ed_neg, ed_retn, ed_im2, ed_ldar
        DCD     ed_60, ed_61, ed_62, ed_63, ed_neg, ed_retn, ed_im0, ed_rrd
        DCD     ed_68, ed_69, ed_6a, ed_6b, ed_neg, ed_retn, ed_im0, ed_rld
        DCD     ed_70, ed_71, ed_72, ed_73, ed_neg, ed_retn, ed_im1, ed_nop
        DCD     ed_78, ed_79, ed_7a, ed_7b, ed_neg, ed_retn, ed_im2, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_ldi, ed_cpi, ed_ini, ed_outi, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_ldd, ed_cpd, ed_ind, ed_outd, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_ldir, ed_cpir, ed_inir, ed_otir, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_lddr, ed_cpdr, ed_indr, ed_otdr, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop
        DCD     ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop, ed_nop

ddtable
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dx_09, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dx_19, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dx_21, dx_22, dx_23, dx_24, dx_25, dx_26, dd_nop
        DCD     dd_nop, dx_29, dx_2a, dx_2b, dx_2c, dx_2d, dx_2e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dm_34, dm_35, dm_36, dd_nop
        DCD     dd_nop, dx_39, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_44, dx_45, dm_46, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_4c, dx_4d, dm_4e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_54, dx_55, dm_56, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_5c, dx_5d, dm_5e, dd_nop
        DCD     dx_60, dx_61, dx_62, dx_63, dx_64, dx_65, dm_66, dx_67
        DCD     dx_68, dx_69, dx_6a, dx_6b, dx_6c, dx_6d, dm_6e, dx_6f
        DCD     dm_70, dm_71, dm_72, dm_73, dm_74, dm_75, dd_nop, dm_77
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_7c, dx_7d, dm_7e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_84, dx_85, dm_86, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_8c, dx_8d, dm_8e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_94, dx_95, dm_96, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_9c, dx_9d, dm_9e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_a4, dx_a5, dm_a6, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_ac, dx_ad, dm_ae, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_b4, dx_b5, dm_b6, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dx_bc, dx_bd, dm_be, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_cb, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dx_e1, dd_nop, dx_e3, dd_nop, dx_e5, dd_nop, dd_nop
        DCD     dd_nop, dx_e9, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dx_f9, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop

fdtable
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, fy_09, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, fy_19, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, fy_21, fy_22, fy_23, fy_24, fy_25, fy_26, dd_nop
        DCD     dd_nop, fy_29, fy_2a, fy_2b, fy_2c, fy_2d, fy_2e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fm_34, fm_35, fm_36, dd_nop
        DCD     dd_nop, fy_39, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_44, fy_45, fm_46, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_4c, fy_4d, fm_4e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_54, fy_55, fm_56, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_5c, fy_5d, fm_5e, dd_nop
        DCD     fy_60, fy_61, fy_62, fy_63, fy_64, fy_65, fm_66, fy_67
        DCD     fy_68, fy_69, fy_6a, fy_6b, fy_6c, fy_6d, fm_6e, fy_6f
        DCD     fm_70, fm_71, fm_72, fm_73, fm_74, fm_75, dd_nop, fm_77
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_7c, fy_7d, fm_7e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_84, fy_85, fm_86, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_8c, fy_8d, fm_8e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_94, fy_95, fm_96, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_9c, fy_9d, fm_9e, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_a4, fy_a5, fm_a6, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_ac, fy_ad, fm_ae, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_b4, fy_b5, fm_b6, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, fy_bc, fy_bd, fm_be, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, fd_cb, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, fy_e1, dd_nop, fy_e3, dd_nop, fy_e5, dd_nop, dd_nop
        DCD     dd_nop, fy_e9, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop
        DCD     dd_nop, fy_f9, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop, dd_nop

        END
