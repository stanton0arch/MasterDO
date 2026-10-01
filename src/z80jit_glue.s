; z80jit_glue.s - entry, exit and service routines of the Z80 translator.
;
; Generated code runs with the Z80 state in registers (see z80jit_emit.c)
; and r10 pointing at &ctx->wtab[0]. These routines move between that state
; and C: running a number of scanlines, translating or linking blocks on
; demand, ending stretches of execution (machine events, interrupts), HALT,
; I/O ports, writes to special pages, checking code that runs from RAM,
; and DAA.
;
; Around C calls r10 is restored to the caller's stack limit (APCS sl);
; r4-r9 and r11 are preserved by C, r3 (Z80 flags) is saved here.
;
; Context layout: CONTRACT with z80j_ctx in z80jit.h - keep in sync
; (z80jit.c checks the offsets at compile time).

        AREA    |C$$code|, CODE, READONLY

        EXPORT  z80j_run
        EXPORT  z80j_glue_link
        EXPORT  z80j_glue_miss
        EXPORT  z80j_glue_seg_timeout
        EXPORT  z80j_glue_leave
        EXPORT  z80j_glue_halt
        EXPORT  z80j_glue_abort
        EXPORT  z80j_glue_io_in
        EXPORT  z80j_glue_io_out
        EXPORT  z80j_glue_io_outn
        EXPORT  z80j_glue_interp
        EXPORT  jit_save
        EXPORT  jit_continue
        EXPORT  z80j_port_in_c
        EXPORT  z80j_port_out_c
        EXPORT  z80j_port_outn_loop
        EXPORT  z80j_fill_words
        EXPORT  z80j_glue_write
        EXPORT  z80j_write_generic
        EXPORT  z80j_write_result
        EXPORT  z80j_glue_verify
        EXPORT  z80j_glue_daa
        EXPORT  z80j_glue_push_slow

        IMPORT  z80j_translate
        IMPORT  z80j_retranslate
        IMPORT  z80j_link
        IMPORT  z80j_entry
        IMPORT  z80j_stretch_end
        IMPORT  z80j_leave
        IMPORT  z80j_io_write
        IMPORT  z80j_push_slow
        IMPORT  z80i_loop

; Context fields, relative to the global pointer (= &ctx->wtab[0]).
GLOBAL          EQU     0x400           ; global pointer - context
REGS            EQU     0x400           ; F A BC DE HL cycles PC SP
R_PC            EQU     0x418
IX              EQU     0x420
HALTED          EQU     0x450
HOST_SL         EQU     0x468
EXIT_ARG        EQU     0x470
EXIT_REASON     EQU     0x46C
STATE           EQU     0x474
MACHINE         EQU     0x478
SLOT_BANK       EQU     0x47C
RESUME_HOST     EQU     0x48C
RESUME_PC       EQU     0x490
RESUME_TAB      EQU     0x4D8
EVENT_A         EQU     0x4DC
IRQ_COUNT       EQU     0x4E0
RESUMED_COUNT   EQU     0x4E4
IFF1            EQU     0x444
IFF2            EQU     0x448
IM              EQU     0x44C
IRQ_LINE        EQU     0x454
NMI             EQU     0x458
RUN_END         EQU     0x45C
LINE            EQU     0x460
EVENT_LINE      EQU     0x464
PAGE_KIND       EQU     0x100           ; page kinds (context + 0x500)
LOOKUP          EQU     0x4000
MAX_STRETCH     EQU     8192
PAGE_KIND_MASK  EQU     0x0F
PAGE_RAM        EQU     0x08
IDLE            EQU     0x498
PORT_IN         EQU     0x49C
PORT_OUT        EQU     0x4A0
PORT_OUTN       EQU     0x4A4
WRITE_A         EQU     0x4AC
PZST            EQU     0x600

; Machine callbacks (z80j_machine).
M_IN            EQU     0
M_OUT           EQU     4

; Flags as kept in r3.
PSR_S           EQU     0x08
PSR_Z           EQU     0x04
PSR_C           EQU     0x02
PSR_n           EQU     0x80
PSR_H           EQU     0x10

; Calls a C function of the translator (first argument: its state; the
; others in r1, r2). Preserves r3 and the global pointer.
        MACRO
        callc   $fn
        stmfd   sp!,{r3,r10}
        ldr     r0,[r10,#STATE]
        ldr     r10,[r10,#HOST_SL]
        bl      $fn
        ldmfd   sp!,{r3,r10}
        MEND

;----------------------------------------------------------------------------
; void z80j_run(z80j_ctx *ctx, int32 lines)
; Runs the Z80 for the given number of scanlines (at least 1).
;----------------------------------------------------------------------------
z80j_run
        stmfd   sp!,{r4-r11,lr}
        add     r2,r0,#GLOBAL
        str     r10,[r2,#HOST_SL]
        mov     r12,#0
        str     r12,[r2,#EXIT_REASON]
        mov     r10,r2
        callc   z80j_entry              ; r1 = lines
jit_continue                            ; r0 = code to run, 0: the run is over,
        teq     r0,#0                   ; or the interpreter entry
        ldmeqfd sp!,{r4-r11,pc}
        adr     r1,z80j_glue_interp
        cmp     r0,r1
        beq     z80j_glue_interp
jit_enter                               ; registers in the context
        mov     r12,r0
        add     r0,r10,#REGS
        ldmia   r0,{r3-r9,r11}
        ldr     r9,[r10,#IX]
        mov     pc,r12

;----------------------------------------------------------------------------
; Interpreter (z80int_a.s): runs with the registers of the generated code
; and the Z80 PC in r12, from the context here, until translated code is
; reached or the stretch ends.
;----------------------------------------------------------------------------
z80j_glue_interp
        add     r0,r10,#REGS
        ldmia   r0,{r3-r9,r11}
        mov     r12,r9                  ; PC
        ldr     r9,[r10,#IX]
        b       z80i_loop

; Live registers, Z80 PC in the context: to the interpreter.
jit_to_interp
        ldr     r12,[r10,#R_PC]
        b       z80i_loop

; Registers to the context. In: r0 = Z80 PC.
jit_save
        str     r9,[r10,#IX]
        mov     r9,r0
        add     r12,r10,#REGS
        stmia   r12,{r3-r9,r11}
        mov     pc,lr

; Leaves z80j_run. In: r0 = Z80 PC.
jit_exit
        bl      jit_save
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; End of a stretch: a segment does not fit in the T-states left. Called by
; "bl", followed by the segment's T-states (times 256), its Z80 address and
; its code, where execution resumes.
;----------------------------------------------------------------------------
z80j_glue_seg_timeout
        ldmia   lr,{r0-r2}
        add     r8,r8,r0                ; give the segment T-states back
        str     r2,[r10,#RESUME_HOST]
        str     r1,[r10,#RESUME_PC]
        ; The usual stretch end, without leaving the generated code: the
        ; machine event (through its assembly handler), a maskable
        ; interrupt in mode 1 (or 0) pushed to writable memory, the next
        ; stretch, then the vector or the interrupted segment. Anything
        ; else (end of the run, no assembly handler or one declining,
        ; NMI, mode 2, a push to a special page, HALT) takes the C path.
        ldr     r0,[r10,#LINE]
        ldr     r12,[r10,#RUN_END]
        cmp     r0,r12
        beq     seg_slow
        ldr     r12,[r10,#EVENT_LINE]
        cmp     r0,r12
        bne     seg_no_event
        ldr     r12,[r10,#EVENT_A]
        teq     r12,#0
        beq     seg_slow
        stmfd   sp!,{r1,r2,lr}
        mov     lr,pc
        mov     pc,r12
        ldmfd   sp!,{r1,r2,lr}
        teq     r0,#0
        bne     seg_slow                ; the handler declined (nothing changed)
seg_no_event
        ldr     r0,[r10,#NMI]
        teq     r0,#0
        bne     seg_slow
        ldr     r0,[r10,#IRQ_LINE]
        ldr     r12,[r10,#IFF1]
        ands    r0,r0,r12
        beq     seg_resume
        ldr     r0,[r10,#IM]
        cmp     r0,#2
        beq     seg_slow
        ; The stack must be in writable pages
        mov     r0,r11,lsr#16
        sub     r0,r0,#2
        bic     r0,r0,#0x10000          ; new SP
        mov     r12,r0,lsr#8
        ldr     r12,[r10,r12,lsl#2]
        teq     r12,#0
        beq     seg_slow
        add     r12,r0,#1
        bic     r12,r12,#0x10000
        mov     r12,r12,lsr#8
        ldr     r12,[r10,r12,lsl#2]
        teq     r12,#0
        beq     seg_slow
        ; Remember the interrupted segment for the return (not for RAM
        ; code, whose entry check alone notices a rewrite)
        bl      seg_key                 ; r12 = key of the code at r1, or -1 for RAM
        cmn     r12,#1
        beq     seg_push
        ldr     r0,[r10,#RESUME_TAB]
        eor     lr,r1,r1,lsr#6
        and     lr,lr,#63
        add     r0,r0,lr,lsl#4
        stmia   r0,{r1,r2,r12}
seg_push
        mov     r0,r11,lsr#16
        sub     r0,r0,#2
        bic     r0,r0,#0x10000
        mov     r11,r0,lsl#16           ; SP -= 2
        mov     r12,r0,lsr#8
        ldr     r12,[r10,r12,lsl#2]
        strb    r1,[r12,r0]             ; low byte of the return address
        add     r0,r0,#1
        bic     r0,r0,#0x10000
        mov     r12,r0,lsr#8
        ldr     r12,[r10,r12,lsl#2]
        mov     r1,r1,lsr#8
        strb    r1,[r12,r0]
        mov     r0,#0
        str     r0,[r10,#IFF1]
        str     r0,[r10,#IFF2]
        str     r0,[r10,#RESUME_HOST]
        ldr     r0,[r10,#IRQ_COUNT]
        add     r0,r0,#1
        str     r0,[r10,#IRQ_COUNT]
        sub     r8,r8,#13*256           ; the interrupt's T-states
        bl      seg_stretch
        add     r1,r10,#LOOKUP
        mov     r0,#0x38                ; for the miss glue, should the entry be gone
        ldr     pc,[r1,#0x38*4]         ; the vector (the entry checks its bank or bytes)
seg_resume
        mov     r0,#0
        str     r0,[r10,#RESUME_HOST]
        bl      seg_stretch
        mov     pc,r2                   ; the interrupted segment
seg_slow
        mov     r0,r1
        bl      jit_save
        callc   z80j_stretch_end
        b       jit_continue

; Next stretch: up to the event line, or to the end of the run if it comes
; first, at most MAX_STRETCH lines. Uses r0, r1, r12.
seg_stretch
        ldr     r0,[r10,#LINE]
        ldr     r1,[r10,#RUN_END]
        sub     r1,r1,r0                ; lines to the end of the run
        ldr     r12,[r10,#EVENT_LINE]
        subs    r12,r12,r0              ; lines to the event
        cmpne   r12,r1
        movlo   r1,r12
        cmp     r1,#MAX_STRETCH
        movhi   r1,#MAX_STRETCH
        add     r0,r0,r1
        str     r0,[r10,#LINE]
        mov     r12,#228
        mul     r1,r12,r1
        add     r8,r8,r1,lsl#8
        mov     pc,lr

; Key of the code at the Z80 address in r1 (jit_block_key): 0 for a fixed
; page, (kind << 16) | bank for a paged slot, -1 for RAM. Uses r0, r12.
seg_key
        add     r12,r10,#PAGE_KIND
        ldrb    r12,[r12,r1,lsr#8]
        ands    r12,r12,#PAGE_KIND_MASK
        moveq   pc,lr                   ; fixed: 0
        cmp     r12,#4
        mvnhi   r12,#0                  ; RAM (or unknown): -1
        movhi   pc,lr
        add     r0,r10,#0x400
        add     r0,r0,#SLOT_BANK-4-0x400
        ldr     r0,[r0,r12,lsl#2]       ; slot_bank[kind - 1]
        orr     r12,r0,r12,lsl#16
        mov     pc,lr

;----------------------------------------------------------------------------
; Leaves the block: the machine may have raised an interrupt or moved its
; event line. In: r0 = Z80 PC, r1 = T-states to give back (times 256).
;----------------------------------------------------------------------------
z80j_glue_leave
        add     r8,r8,r1
        bl      jit_save
        callc   z80j_leave
        b       jit_continue

;----------------------------------------------------------------------------
; HALT: the rest of the stretch goes by idle, and so do the next ones until
; an interrupt. In: r0 = Z80 PC after the HALT.
;----------------------------------------------------------------------------
z80j_glue_halt
        ldr     r1,[r10,#IDLE]          ; the rest of the stretch is idle
        add     r1,r1,r8
        str     r1,[r10,#IDLE]
        mov     r1,#1
        str     r1,[r10,#HALTED]
        mov     r8,#0
        bl      jit_save
        callc   z80j_stretch_end
        b       jit_continue

;----------------------------------------------------------------------------
; Untranslated code, or code of another bank. In: r0 = Z80 PC.
;----------------------------------------------------------------------------
z80j_glue_miss
        ; An interrupt return to a segment the glue remembered (same code
        ; under the same mapping) goes on inside its block
        ldr     r12,[r10,#RESUME_TAB]
        teq     r12,#0
        beq     miss_c
        eor     r1,r0,r0,lsr#6
        and     r1,r1,#63
        add     r12,r12,r1,lsl#4
        ldmia   r12,{r1,r2,lr}          ; pc, host, key
        cmp     r1,r0
        bne     miss_c
        teq     r2,#0
        beq     miss_c
        stmfd   sp!,{r0,lr}
        bl      seg_key                 ; r12 = key of the code at r1 now
        ldmfd   sp!,{r0,lr}
        cmp     r12,lr
        bne     miss_c
        ldr     r1,[r10,#RESUMED_COUNT]
        add     r1,r1,#1
        str     r1,[r10,#RESUMED_COUNT]
        mov     pc,r2
miss_c
        str     r0,[r10,#R_PC]
        mov     r1,r0
        callc   z80j_translate
        adr     r1,z80j_glue_interp
        cmp     r0,r1
        bne     jit_go                  ; translated
        b       jit_to_interp
jit_go
        mov     pc,r0

;----------------------------------------------------------------------------
; Static exit not linked yet: called by "bl", followed by the Z80 target,
; the conditional jump to patch (or 0) and the key of the source block.
;----------------------------------------------------------------------------
z80j_glue_link
        mov     r1,lr
        callc   z80j_link
        teq     r0,#0
        movne   pc,r0
        b       jit_to_interp           ; the target is interpreted

;----------------------------------------------------------------------------
; Translation failed: exit_reason and exit_arg (Z80 PC) set by C.
;----------------------------------------------------------------------------
z80j_glue_abort
        ldr     r0,[r10,#EXIT_ARG]
        b       jit_exit

;----------------------------------------------------------------------------
; I/O ports whose number is only known at run time (C register, block
; instructions): jump to the handler of the port in the context's tables
; (see z80jit.h for the handler conventions).
;----------------------------------------------------------------------------
z80j_glue_io_in
        ldr     r12,[r10,#PORT_IN]
        ldr     pc,[r12,r1,lsl#2]

z80j_glue_io_out
        ldr     r12,[r10,#PORT_OUT]
        ldr     pc,[r12,r1,lsl#2]

z80j_glue_io_outn
        ldr     r12,[r10,#PORT_OUTN]
        ldr     pc,[r12,r1,lsl#2]

;----------------------------------------------------------------------------
; Generic port handlers: the machine's C callbacks. IN: r1 = port, r2 =
; T-states left (times 256), returns r0. OUT: r0 = value, r1 = port, r2 =
; T-states left, returns r0 != 0 when the machine asks to leave the block.
;----------------------------------------------------------------------------
z80j_port_in_c
        stmfd   sp!,{r3,r10,lr}
        ldr     r0,[r10,#MACHINE]
        ldr     r10,[r10,#HOST_SL]
        mov     lr,pc
        ldr     pc,[r0,#M_IN]
        and     r0,r0,#0xFF
        ldmfd   sp!,{r3,r10,pc}

z80j_port_out_c
        stmfd   sp!,{r3,r10,lr}
        mov     r3,r2
        and     r2,r0,#0xFF
        ldr     r0,[r10,#MACHINE]
        ldr     r10,[r10,#HOST_SL]
        mov     lr,pc
        ldr     pc,[r0,#M_OUT]
        ldmfd   sp!,{r3,r10,pc}

; Run of OUTI / OUTD: one call of the port's output handler per byte.
; In: r0 = count (negative for OUTD), r1 = port, r2 = T-states left at the
; end of the run, r7 = HL. The n-th byte from the end goes out 16 (n - 1)
; T-states earlier.
z80j_port_outn_loop
        stmfd   sp!,{r3-r8,lr}
        mov     r3,r7                   ; source address (bits 16-31)
        mov     r5,#0x10000             ; step
        movs    r4,r0                   ; bytes left
        rsbmi   r4,r4,#0
        rsbmi   r5,r5,#0
        mov     r6,r1                   ; port
        mov     r8,r2
outn_loop
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r0,[r2,r3,lsr#16]
        add     r3,r3,r5
        mov     r1,r6
        sub     r2,r4,#1
        add     r2,r8,r2,lsl#12         ; 16 T-states (times 256) per byte
        ldr     r12,[r10,#PORT_OUT]
        mov     lr,pc
        ldr     pc,[r12,r6,lsl#2]
        subs    r4,r4,#1
        bne     outn_loop
        mov     r0,#0
        ldmfd   sp!,{r3-r8,pc}

;----------------------------------------------------------------------------
; Write to a special page. In: r0 = value, r12 = Z80 address (bits 16-31),
; r2 = where to return; lr -> Z80 PC of the next instruction (-1: never
; leave), T-states to give back when leaving, and the key of the block.
; The machine's handler (ctx->write_a, see z80jit.h) runs first; the
; generic one below calls the machine's C write callback. The result
; (Z80J_WRITE_*) is then acted on at z80j_write_result: after a change of
; paged banks (Z80J_WRITE_PAGING) the block goes on unless its own code
; comes from a slot that no longer shows its bank.
;----------------------------------------------------------------------------
z80j_glue_write
        ldr     pc,[r10,#WRITE_A]

z80j_write_generic
        stmfd   sp!,{r2,lr}
        mov     r2,r0
        mov     r1,r12,lsr#16
        callc   z80j_io_write
        ldmfd   sp!,{r2,lr}
z80j_write_result
        teq     r0,#0
        moveq   pc,r2
        cmp     r0,#2
        bne     write_leave
        ldr     r0,[lr,#8]              ; key: page kind << 16 | bank
        movs    r1,r0,lsr#16
        moveq   pc,r2                   ; fixed code
        cmp     r1,#4
        movhi   pc,r2                   ; code in RAM
        add     r1,r10,r1,lsl#2
        ldr     r1,[r1,#SLOT_BANK-4]    ; bank shown by the block's slot
        mov     r0,r0,lsl#16
        cmp     r1,r0,lsr#16
        moveq   pc,r2
write_leave
        ldmia   lr,{r0,r1}
        cmn     r0,#1
        moveq   pc,r2
        b       z80j_glue_leave

;----------------------------------------------------------------------------
; Push to a special page. In: r0 = 16-bit value, r11 = SP (bits 16-31).
;----------------------------------------------------------------------------
z80j_glue_push_slow
        stmfd   sp!,{lr}
        mov     r2,r0
        mov     r1,r11
        callc   z80j_push_slow
        ldmfd   sp!,{pc}

;----------------------------------------------------------------------------
; Entry of a block translated from RAM: called by "bl", followed by its Z80
; address, a word count, that many words of bytes (the memory words holding
; the block, from the one holding its first byte) and as many words of mask
; (zero outside the block and on the operand bytes the code reads for
; itself). Compared word by word with the host memory (RAM is contiguous);
; returns after the mask when the code is unchanged, else runs a new
; translation.
;----------------------------------------------------------------------------
z80j_glue_verify
        ldmia   lr!,{r0,r1}             ; r0 = Z80 address, r1 = words
        stmfd   sp!,{r0,r4}
        mvn     r2,r0,lsr#8
        ldr     r2,[r10,r2,lsl#2]       ; page entry
        add     r12,r2,r0               ; host address of the first byte
        bic     r12,r12,#3              ; the word holding it (RAM is contiguous)
        add     r4,lr,r1,lsl#2          ; the mask follows the bytes
verify_loop
        ldr     r2,[r12],#4
        ldr     r0,[lr],#4
        eor     r2,r2,r0
        ldr     r0,[r4],#4
        tst     r2,r0
        bne     verify_fail
        subs    r1,r1,#1
        bne     verify_loop
        mov     lr,r4                   ; the code follows the mask
        ldmfd   sp!,{r0,r4}
        mov     pc,lr
verify_fail
        ldmfd   sp!,{r1,r4}             ; r1 = Z80 address
        callc   z80j_retranslate
        teq     r0,#0
        movne   pc,r0
        b       jit_to_interp           ; the code is interpreted

;----------------------------------------------------------------------------
; void z80j_fill_words(uint32 *dst, uint32 count, uint32 value)
; Stores value in count words (a multiple of 4), eight at a time.
;----------------------------------------------------------------------------
z80j_fill_words
        stmfd   sp!,{r4-r9,lr}
        mov     r3,r2
        mov     r4,r2
        mov     r5,r2
        mov     r6,r2
        mov     r7,r2
        mov     r8,r2
        mov     r9,r2
        subs    r1,r1,#8
        blo     fill_4
fill_8
        stmia   r0!,{r2-r9}
        subs    r1,r1,#8
        bhs     fill_8
fill_4
        tst     r1,#4
        stmneia r0!,{r2-r5}
        ldmfd   sp!,{r4-r9,pc}

;----------------------------------------------------------------------------
; DAA on r4 (A in bits 24-31) and r3 (flags). Uses r0-r2, r12.
;----------------------------------------------------------------------------
z80j_glue_daa
        mov     r0,r4,lsr#24            ; A
        and     r1,r0,#0x0F             ; low nibble
        mov     r2,#0                   ; correction
        tst     r3,#PSR_H
        movne   r2,#0x06
        cmp     r1,#9
        movhi   r2,#0x06
        mov     r12,#0                  ; carry out
        tst     r3,#PSR_C
        movne   r12,#1
        cmp     r0,#0x99
        movhi   r12,#1
        teq     r12,#0
        orrne   r2,r2,#0x60
        tst     r3,#PSR_n
        bne     daa_sub
        cmp     r1,#9                   ; addition: H = low nibble > 9
        movls   r1,#0
        movhi   r1,#PSR_H
        add     r0,r0,r2
        b       daa_flags
daa_sub
        tst     r3,#PSR_H               ; subtraction: H = H and low nibble < 6
        moveq   r1,#0
        beq     daa_sub2
        cmp     r1,#6
        movcc   r1,#PSR_H
        movcs   r1,#0
daa_sub2
        sub     r0,r0,r2
daa_flags
        and     r0,r0,#0xFF
        mov     r4,r0,lsl#24
        and     r2,r3,#PSR_n            ; N is kept
        add     r3,r10,#PZST
        ldrb    r3,[r3,r0]              ; S, Z, P
        orr     r3,r3,r2
        orr     r3,r3,r1
        teq     r12,#0
        orrne   r3,r3,#PSR_C
        mov     pc,lr

        END
