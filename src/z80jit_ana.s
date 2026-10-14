; Analysis passes of the translator over a decoded block (see z80jit.c,
; translate_block): the jump targets inside the block, the busy-wait
; loops, the segments and their T-states, the runs of port outputs and the
; output loops, the flag liveness and the flag tests fused with their
; jump. They read and write the instruction records (jit_insn,
; z80jit_int.h) in three walks: one forward for the internal targets, one
; forward for the segments and the runs, one backward for the T-states
; left in a segment, the liveness and the fusion.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_analyse_block
        IMPORT  jit_dec_tab
        IMPORT  jit_busy_loop
        IMPORT  jit_flags_needed

; jit_insn (z80jit_int.h; offsets checked in z80jit.c).
I_PRE           EQU     8
I_OP            EQU     12
I_T             EQU     24
I_KIND          EQU     28
I_CC            EQU     32
I_TARGET        EQU     36
I_INTERNAL      EQU     56
I_BUSY          EQU     60
I_IRQ_CHECK     EQU     64
I_RUN           EQU     68
I_SEG_START     EQU     72
I_FUSED         EQU     76
I_DYN           EQU     80
I_LIVE_AFTER    EQU     84
I_LIVE_BEFORE   EQU     88
I_SIZE          EQU     104

; jit_ana_block (z80jit_int.h).
A_NEXT_PC       EQU     12
A_BUSY          EQU     24
A_TARGETS       EQU     36
A_MAX_TARGETS   EQU     40
A_NTARGETS      EQU     44

; jit_block_ctx: the translator state comes first.
B_J             EQU     0

K_JRCC          EQU     1
K_JPCC          EQU     2
K_CALLCC        EQU     3
K_RETCC         EQU     4
K_DJNZ          EQU     5
K_REP           EQU     6
K_JR            EQU     7
K_JP            EQU     8
K_CALL          EQU     9
K_RST           EQU     10
K_RET           EQU     11

PRE_CB          EQU     1
PRE_ED          EQU     2
PRE_DDCB        EQU     5

FL_ALL          EQU     0x3F

FU_Z_EQ         EQU     0x01
FU_Z_CS         EQU     0x02
FU_C_CS         EQU     0x04
FU_C_CC         EQU     0x08
FU_S_MI         EQU     0x10
FU_V_VS         EQU     0x20

SEG_MAX_T       EQU     200             ; a segment always fits in a fresh scanline
RUN_MAX         EQU     16              ; Z80J_RUN_MAX
MAX_INSNS       EQU     64

; Stack frame.
F_ARGS          EQU     0               ; the jit_ana_block
F_BC            EQU     4               ; its block context
F_J             EQU     8               ; the translator state
F_N             EQU     12              ; instructions
F_CAFTER        EQU     16              ; flags read after the conditional
                                        ; instruction that follows the current one
F_FUSED         EQU     20              ; counts returned
F_BUSY          EQU     24
F_LINE          EQU     28
F_NBACK         EQU     32              ; jumps back found by the first pass
F_BACK          EQU     36              ; head index | index << 8 of each
F_TARGETS       EQU     36 + 4 * MAX_INSNS      ; next static exit to report (0: none asked)
F_TEND          EQU     F_TARGETS + 4           ; end of the room for them
F_SIZE          EQU     F_TEND + 4

; Reports the static exit $v (a Z80 address) when exits are asked for and
; there is room left. Uses r12, lr.
        MACRO
        REPORT  $v
        ldr     r12,[sp,#F_TARGETS]
        ldr     lr,[sp,#F_TEND]
        cmp     r12,lr
        strlo   $v,[r12],#4
        strlo   r12,[sp,#F_TARGETS]
        MEND

;----------------------------------------------------------------------------
; void jit_analyse_block(jit_ana_block *a)
;
; Runs the analysis passes over the a->n decoded instructions of the block
; at a->pc (see translate_block for what each one establishes). Returns
; the busy-wait loops found, those waiting for a line and the fused flag
; tests in a->busy, a->line and a->fused.
;----------------------------------------------------------------------------
jit_analyse_block
        stmfd   sp!,{r4-r9,r11,lr}
        sub     sp,sp,#F_SIZE
        str     r0,[sp,#F_ARGS]
        ldmia   r0,{r4-r9}              ; ins, n, pc, next_pc, serial, bc
        str     r5,[sp,#F_N]
        str     r9,[sp,#F_BC]
        ldr     r1,[r9,#B_J]
        str     r1,[sp,#F_J]
        add     r1,r5,r5,lsl #2
        add     r1,r1,r5,lsl #3
        add     r5,r4,r1,lsl #3         ; end of the records
        sub     r7,r7,r6
        mov     r7,r7,lsl #16
        mov     r7,r7,lsr #16           ; span of the block
        ldr     r9,an_tab
        mov     r1,#0
        str     r1,[sp,#F_NBACK]
        str     r1,[sp,#F_BUSY]
        str     r1,[sp,#F_LINE]
        str     r1,[sp,#F_FUSED]
        ldr     r1,[r0,#A_TARGETS]
        ldr     r2,[r0,#A_MAX_TARGETS]
        add     r2,r1,r2,lsl #2
        str     r1,[sp,#F_TARGETS]
        str     r2,[sp,#F_TEND]

; Pass 1: a jump whose static target lies inside the block gets the index
; of the target instruction, which starts a segment. Jumps back by JR, JP,
; JR cc and JP cc are kept for the busy-wait loop analysis.
; When the caller asks for them (the code reachable from the entry points,
; translated before the run), the static exits of the block are reported
; on the way, in instruction order: the target of a conditional jump
; leaving the block, of a JR, JP, CALL or RST (inside or not), the return
; address of a CALL or RST after its target, and the address after the
; block when it does not end with a transfer.
; r0 = record, r4 = ins, r5 = end, r6 = pc, r7 = span, r8 = serial,
; r9 = jit_dec_tab, r11 = index.
        mov     r0,r4
        mov     r11,#0
p1_loop
        ldr     r1,[r0,#I_KIND]
        sub     r2,r1,#1
        cmp     r2,#5                   ; conditional (K_JRCC to K_REP)
        bls     p1_cond
        cmp     r1,#K_JR
        cmpne   r1,#K_JP
        beq     p1_target
        cmp     r1,#K_CALL
        cmpne   r1,#K_RST
        bne     p1_next
        ; CALL and RST: the target, then the return address.
        ldr     r2,[r0,#I_DYN]
        cmp     r2,#0
        bne     p1_next
        ldr     r2,[r0,#I_TARGET]
        REPORT  r2
        ldmia   r0,{r2,r3}              ; pc, len
        add     r2,r2,r3
        mov     r2,r2,lsl #16
        mov     r2,r2,lsr #16
        REPORT  r2
p1_next
        add     r11,r11,#1
        add     r0,r0,#I_SIZE
        cmp     r0,r5
        blo     p1_loop
        b       p1_done
p1_cond
        cmp     r1,#K_RETCC
        beq     p1_next
        cmp     r1,#K_CALLCC
        bne     p1_target
        ; CALL cc: never a jump inside the block, its target is an exit.
        ldr     r2,[r0,#I_DYN]
        cmp     r2,#0
        bne     p1_next
        ldr     r2,[r0,#I_TARGET]
        REPORT  r2
        b       p1_next
p1_target
        ldr     r2,[r0,#I_DYN]
        cmp     r2,#0                   ; the target is read when it runs
        bne     p1_next
        ldr     r2,[r0,#I_TARGET]
        sub     r3,r2,r6
        mov     r3,r3,lsl #16
        mov     r3,r3,lsr #16           ; offset from the block start
        cmp     r3,r7
        bhs     p1_outside
        and     r3,r2,#0xFF
        add     r3,r9,r3,lsl #2
        ldr     r3,[r3,#1024]           ; instruction map entry
        bic     r12,r3,#0xFF
        cmp     r12,r8                  ; of this decoding
        bne     p1_outside
        and     r3,r3,#0xFF
        sub     r3,r3,#1                ; index of the target
        str     r3,[r0,#I_INTERNAL]
        add     r2,r3,r3,lsl #2
        add     r2,r2,r3,lsl #3
        add     r2,r4,r2,lsl #3
        mov     r12,#1
        str     r12,[r2,#I_SEG_START]   ; the target starts a segment
        cmp     r1,#K_JR                ; an unconditional transfer is an
        cmpne   r1,#K_JP                ; exit even inside the block
        bne     p1_back
        ldr     r2,[r0,#I_TARGET]
        REPORT  r2
p1_back
        cmp     r3,r11                  ; a jump back
        bhi     p1_next
        cmp     r1,#K_JRCC
        cmpne   r1,#K_JPCC
        cmpne   r1,#K_JR
        cmpne   r1,#K_JP
        bne     p1_next
        ldr     r2,[sp,#F_NBACK]
        orr     r12,r3,r11,lsl #8
        add     lr,sp,#F_BACK
        str     r12,[lr,r2,lsl #2]
        add     r2,r2,#1
        str     r2,[sp,#F_NBACK]
        b       p1_next
p1_outside
        ; A jump leaving the block: an exit (r2 = its target).
        REPORT  r2
        b       p1_next
p1_done
        ; The address after the block is an exit when the block does not
        ; end with a transfer.
        ldr     r1,[r5,#I_KIND - I_SIZE]
        cmp     r1,#K_JR
        bhs     p1_nofall
        ldr     r2,[sp,#F_ARGS]
        ldr     r2,[r2,#A_NEXT_PC]
        REPORT  r2
p1_nofall

; Busy-wait loops: each jump back kept is given to jit_busy_loop (C),
; which says whether the loop it closes waits for an interrupt (1) or
; for a line (2); the answer goes to the jump's record.
        ldr     r11,[sp,#F_NBACK]
        cmp     r11,#0
        beq     p2_start
        add     r9,sp,#F_BACK
pb_loop
        ldr     r12,[r9],#4
        mov     r7,r12,lsr #8           ; index of the jump
        and     r3,r12,#0xFF            ; index of the loop head
        ldr     r0,[sp,#F_J]
        mov     r1,r4
        ldr     r2,[sp,#F_N]
        str     r7,[sp,#-4]!            ; fifth argument
        bl      jit_busy_loop
        add     sp,sp,#4
        add     r1,r7,r7,lsl #2
        add     r1,r1,r7,lsl #3
        add     r1,r4,r1,lsl #3
        str     r0,[r1,#I_BUSY]
        cmp     r0,#0
        beq     pb_next
        ldr     r1,[sp,#F_BUSY]
        add     r1,r1,#1
        str     r1,[sp,#F_BUSY]
        cmp     r0,#2
        ldreq   r1,[sp,#F_LINE]
        addeq   r1,r1,#1
        streq   r1,[sp,#F_LINE]
pb_next
        subs    r11,r11,#1
        bne     pb_loop

; Pass 2: segments and port output runs. A segment starts at the block
; start, at a jump target (pass 1), after a conditional instruction and
; whenever the running total of T-states would exceed SEG_MAX_T.
; Consecutive OUTI, or OUTD, or OUTI mixed with OUT (C),r on one register
; among A, D and E, inside a segment, make one run (at most RUN_MAX
; elements, ended after an element with an interrupt check): the head
; gets the length, the mask of the OUT (C),r elements and their opcode,
; the others RUN_PART. A lone OUTI or OUTD closed by a JR NZ or JP NZ back
; to it becomes RUN_LOOP.
; r0 = record, r1 = T-states of the segment so far, r2 = state: bit 0
; the previous instruction is conditional, bit 1 the previous element of
; the run has an interrupt check, bit 2 this instruction is conditional;
; r3 = head of the open run or 0, r6 = its run word so far, r7 = the
; opcode of its head.
p2_start
        mov     r0,r4
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        mov     r12,#1
        str     r12,[r0,#I_SEG_START]   ; the block start
p2_loop
        add     r8,r0,#I_T
        ldmia   r8,{r8,r9}              ; t, kind
        ldr     r11,[r0,#I_SEG_START]
        sub     r12,r9,#1
        cmp     r12,#5
        orrls   r2,r2,#4                ; conditional
        tst     r2,#1
        bne     p2_seg1                 ; after a conditional instruction
        cmp     r11,#0
        bne     p2_seg
        add     r12,r1,r8
        cmp     r12,#SEG_MAX_T
        bhi     p2_seg1
p2_noseg
        add     r1,r1,r8
        cmp     r3,#0
        bne     p2_open
p2_newrun
        ldr     r12,[r0,#I_PRE]
        cmp     r12,#PRE_ED
        beq     p2_ed
p2_next
        bic     r2,r2,#1
        tst     r2,#4
        orrne   r2,r2,#1
        bic     r2,r2,#4
        add     r0,r0,#I_SIZE
        cmp     r0,r5
        blo     p2_loop
        cmp     r3,#0                   ; a run reaching the end of the block
        beq     p3_start
        and     r12,r6,#0xFF
        cmp     r12,#2
        blcs    run_write
        b       p3_start

p2_seg1
        mov     r11,#1
        str     r11,[r0,#I_SEG_START]
p2_seg
        mov     r1,#0
        b       p2_noseg

; A run is open: this instruction extends it or ends it.
p2_open
        tst     r2,#2                   ; the run ends after an interrupt check
        bne     p2_close
        and     r12,r6,#0xFF
        cmp     r12,#RUN_MAX
        beq     p2_close
        cmp     r11,#0                  ; and before a segment start
        bne     p2_close
        ldr     r12,[r0,#I_PRE]
        cmp     r12,#PRE_ED
        bne     p2_close
        ldr     r9,[r0,#I_OP]
        bl      run_elem
        cmp     r11,#0
        beq     p2_close
        cmp     r11,#2                  ; OUT (C),r: its bit in the mask
        andeq   r12,r6,#0xFF
        addeq   r12,r12,#8
        moveq   lr,#1
        orreq   r6,r6,lr,lsl r12
        add     r6,r6,#1                ; one more element
        ldr     r12,[r0,#I_IRQ_CHECK]
        bic     r2,r2,#2
        cmp     r12,#0
        orrne   r2,r2,#2
        b       p2_next

; The open run ends before this instruction.
p2_close
        and     r12,r6,#0xFF
        cmp     r12,#2
        bhs     p2_write
        ; A lone OUTI / OUTD: when this instruction is a JR NZ / JP NZ
        ; back to it, the loop sends B bytes in one run (emit_out_loop).
        cmp     r7,#0xA3
        cmpne   r7,#0xAB
        bne     p2_lone
        tst     r2,#2                   ; the OUTI has an interrupt check
        bne     p2_lone
        ldr     r12,[r0,#I_KIND]
        cmp     r12,#K_JRCC
        cmpne   r12,#K_JPCC
        bne     p2_lone
        ldr     r12,[r0,#I_CC]
        cmp     r12,#0                  ; NZ
        bne     p2_lone
        ldr     r12,[r0,#I_INTERNAL]
        add     lr,r12,r12,lsl #2
        add     lr,lr,r12,lsl #3
        add     lr,r4,lr,lsl #3
        cmp     lr,r3                   ; back to the OUTI
        bne     p2_lone
        ldr     r12,[r0,#I_DYN]
        ldr     lr,[r0,#I_BUSY]
        orrs    r12,r12,lr
        ldreq   r12,[r0,#I_IRQ_CHECK]
        cmpeq   r12,#0
        bne     p2_lone
        mvn     r12,#1                  ; RUN_LOOP
        str     r12,[r3,#I_RUN]
        b       p2_lone
p2_write
        bl      run_write
p2_lone
        mov     r3,#0
        b       p2_newrun

; No open run: this ED instruction may start one.
p2_ed
        ldr     r7,[r0,#I_OP]
        mov     r9,r7
        mov     r6,#0
        bl      run_elem
        cmp     r11,#0
        beq     p2_next
        mov     r3,r0
        cmp     r11,#2
        orreq   r6,r6,#0x100            ; mask bit of the first element
        add     r6,r6,#1
        ldr     r12,[r0,#I_IRQ_CHECK]
        bic     r2,r2,#2
        cmp     r12,#0
        orrne   r2,r2,#2
        b       p2_next

; Element of the run started by the ED opcode r7 that the ED opcode r9
; makes: r11 = 1 for an OUTI (an OUTD in a run of OUTD), 2 for an
; OUT (C),r on the run's register (bits 24-31 of r6, set by the first
; one met), 0 when r9 ends the run. Uses r12.
run_elem
        cmp     r7,#0xAB
        bne     re_outi
        cmp     r9,#0xAB
        moveq   r11,#1
        movne   r11,#0
        mov     pc,lr
re_outi
        cmp     r9,#0xA3
        moveq   r11,#1
        moveq   pc,lr
        cmp     r9,#0x79                ; OUT (C),A
        cmpne   r9,#0x51                ; OUT (C),D
        cmpne   r9,#0x59                ; OUT (C),E
        movne   r11,#0
        movne   pc,lr
        movs    r12,r6,lsr #24
        cmpne   r12,r9
        movne   r11,#0
        movne   pc,lr
        bic     r6,r6,#0xFF000000
        orr     r6,r6,r9,lsl #24
        mov     r11,#2
        mov     pc,lr

; Writes the run word r6 to its head r3 and RUN_PART to the other
; elements. Uses r9 and r12; r3 ends on the last element.
run_write
        str     r6,[r3,#I_RUN]
        and     r12,r6,#0xFF
        sub     r12,r12,#1
        mvn     r9,#0
rw_loop
        add     r3,r3,#I_SIZE
        str     r9,[r3,#I_RUN]
        subs    r12,r12,#1
        bne     rw_loop
        mov     pc,lr

; Pass 3, backwards: the T-states of each segment from each instruction
; on and after it; the flags live after and before each instruction (a
; conditional instruction reads its condition and whatever its taken path
; reads: everything for RET cc and dynamic targets, the liveness at a
; target inside the block, else the forward scan of jit_flags_needed);
; and the fusion of a flag producer with the conditional jump that is its
; only reader, decided when the producer is reached from what its jump
; needs (c_need, 0 when the jump cannot fuse) and the flags read after the
; jump either way (c_after).
; r4 = ins, r5 = record, r6 = flags live before the next instruction,
; r7 = T-states of the segment after the record, r8 = c_need.
p3_start
        sub     r5,r5,#I_SIZE           ; the last instruction
        ldr     r1,[r5,#I_KIND]
        cmp     r1,#K_RET
        movhs   r6,#FL_ALL              ; RET, RETN, JP (HL), HALT
        bhs     p3_init
        cmp     r1,#K_JR
        blo     p3_fall
        ldr     r0,[r5,#I_DYN]          ; JR, JP, CALL, RST
        cmp     r0,#0
        movne   r6,#FL_ALL
        bne     p3_init
        ldr     r1,[r5,#I_TARGET]
        b       p3_scan0
p3_fall
        ldr     r0,[sp,#F_ARGS]
        ldr     r1,[r0,#A_NEXT_PC]
p3_scan0
        ldr     r0,[sp,#F_BC]
        bl      jit_flags_needed
        mov     r6,r0
p3_init
        mov     r7,#0
        mov     r8,#0
p3_loop
        add     r0,r5,#I_T
        ldmia   r0,{r0-r3,r9,r11}       ; t kind cc target use def
        cmp     r8,#0
        bne     p3_fuse
p3_live
        sub     r12,r1,#1
        cmp     r12,#5
        bls     p3_cond
        mov     lr,r6                   ; live after
        bic     r6,r6,r11
        orr     r6,r6,r9                ; live before: read, or live after and not written
        mov     r8,#0
p3_store
        ldr     r12,[r5,#I_SEG_START]
        add     r2,r7,r0                ; T-states of the segment from here on
        mov     r3,r7                   ; after this instruction
        mov     r0,lr
        mov     r1,r6
        add     lr,r5,#I_LIVE_AFTER
        stmia   lr,{r0-r3}              ; live_after live_before seg_t t_after
        cmp     r12,#0
        movne   r7,#0                   ; a segment start: the previous instruction ends its segment
        moveq   r7,r2
        movne   r8,#0                   ; and no test is fused across it
        sub     r5,r5,#I_SIZE
        cmp     r5,r4
        bhs     p3_loop

        ldr     r0,[sp,#F_ARGS]
        ldr     r1,[sp,#F_BUSY]
        ldr     r2,[sp,#F_LINE]
        ldr     r3,[sp,#F_FUSED]
        add     r0,r0,#A_BUSY
        stmia   r0,{r1-r3}
        ldr     r1,[sp,#F_TARGETS]
        ldr     r2,[r0,#A_TARGETS - A_BUSY]
        sub     r1,r1,r2
        mov     r1,r1,lsr #2
        str     r1,[r0,#A_NTARGETS - A_BUSY]
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r9,r11,pc}

; A conditional instruction: what its taken path reads.
p3_cond
        cmp     r1,#K_RETCC
        beq     p3_all
        ldr     r12,[r5,#I_DYN]
        cmp     r12,#0
        bne     p3_all
        ldr     r12,[r5,#I_INTERNAL]
        cmp     r12,#0
        blt     p3_scan
        add     lr,r12,r12,lsl #2
        add     lr,lr,r12,lsl #3
        add     lr,r4,lr,lsl #3
        cmp     lr,r5                   ; a target further on: its liveness is known
        ldrhi   r12,[lr,#I_LIVE_BEFORE]
        bhi     p3_taken
p3_scan
        stmfd   sp!,{r0-r3}
        ldr     r0,[sp,#F_BC + 16]
        mov     r1,r3
        bl      jit_flags_needed
        mov     r12,r0
        ldmfd   sp!,{r0-r3}
        b       p3_taken
p3_all
        mov     r12,#FL_ALL
p3_taken
        mov     lr,r6                   ; live after
        orr     r6,r6,r9
        orr     r6,r6,r12               ; live before: the condition and both paths
        orr     r12,r12,lr              ; read after the jump, either way
        str     r12,[sp,#F_CAFTER]
        cmp     r1,#K_DJNZ
        cmpne   r1,#K_REP
        moveq   r8,#0
        beq     p3_store
        adr     r12,cc_need
        ldrb    r8,[r12,r2,lsr #1]      ; what the condition can use
        b       p3_store

; The next instruction is a conditional jump that can take its flags from
; the ARM flags (c_need): this instruction fuses with it when it produces
; them (fuse_mask), has no interrupt check, and none of the flags it
; writes is read after the jump. r0-r3, r9 and r11 hold the loaded words.
p3_fuse
        ldr     r12,[r5,#I_IRQ_CHECK]
        cmp     r12,#0
        bne     p3_live
        ldr     r12,[r5,#I_PRE]
        ldr     lr,[r5,#I_OP]
        stmfd   sp!,{r2,r3}
        mov     r2,lr,lsr #6            ; x
        cmp     r12,#PRE_CB
        beq     fm_cb
        cmp     r12,#PRE_ED
        beq     fm_ed
        cmp     r12,#PRE_DDCB
        bhs     fm_cb                   ; DD CB, FD CB
        and     r3,lr,#7                ; z
        cmp     r2,#2
        beq     fm_alu                  ; ALU A,r
        cmp     r2,#3
        cmpeq   r3,#6
        beq     fm_alu                  ; ALU A,n
        cmp     r2,#0
        bne     fm_none
        cmp     r3,#4
        cmpne   r3,#5
        bne     fm_x0
        and     r3,lr,#0x38
        cmp     r3,#0x30                ; INC / DEC (HL)
        moveq   r12,#0
        movne   r12,#FU_Z_EQ + FU_S_MI + FU_V_VS       ; INC / DEC r
        b       fm_done
fm_x0
        cmp     r3,#1
        bne     fm_none
        tst     lr,#8
        movne   r12,#FU_C_CS            ; ADD HL,rr
        moveq   r12,#0
        b       fm_done
fm_alu
        and     r3,lr,#0x38
        cmp     r3,#0x08
        movls   r12,#FU_Z_EQ + FU_C_CS + FU_S_MI + FU_V_VS     ; ADD, ADC
        bls     fm_done
        cmp     r3,#0x18
        bls     fm_sub
        cmp     r3,#0x38
        beq     fm_sub
        mov     r12,#FU_Z_EQ + FU_S_MI  ; AND, XOR, OR
        b       fm_done
fm_sub
        mov     r12,#FU_Z_EQ + FU_C_CC + FU_S_MI + FU_V_VS     ; SUB, SBC, CP
        b       fm_done
fm_cb
        cmp     r2,#1                   ; BIT
        moveq   r12,#FU_Z_EQ
        movne   r12,#0
        b       fm_done
fm_ed
        cmp     r2,#1
        bne     fm_none
        and     r3,lr,#7
        cmp     r3,#4                   ; NEG
        moveq   r12,#FU_Z_EQ + FU_C_CC + FU_S_MI + FU_V_VS
        beq     fm_done
        cmp     r3,#2                   ; SBC / ADC HL,rr
        bne     fm_none
        tst     lr,#8
        movne   r12,#FU_Z_EQ + FU_S_MI + FU_V_VS + FU_C_CS
        moveq   r12,#FU_Z_EQ + FU_S_MI + FU_V_VS + FU_C_CC
        b       fm_done
fm_none
        mov     r12,#0
fm_done
        ldmfd   sp!,{r2,r3}
        ands    r12,r12,r8
        beq     p3_live
        ldr     lr,[sp,#F_CAFTER]
        tst     r11,lr                  ; a flag written here is read after the jump
        bne     p3_live
        str     r12,[r5,#I_FUSED]
        ldr     lr,[sp,#F_FUSED]
        add     lr,lr,#1
        str     lr,[sp,#F_FUSED]
        b       p3_live

; FU_* a condition can use: NZ Z, NC C, PO PE, P M.
cc_need DCB     FU_Z_EQ + FU_Z_CS, FU_C_CS + FU_C_CC, FU_V_VS, FU_S_MI

an_tab  DCD     jit_dec_tab

        END
