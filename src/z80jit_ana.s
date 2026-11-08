; Analysis passes of the translator over a decoded block (see z80jit.c,
; translate_block): the busy-wait loops, the segments and their T-states,
; the runs of port outputs and the output loops, the flag liveness and the
; flag tests fused with their jump, and the groups of port outputs sent in
; one call. The jump targets inside the block are resolved by the decoder
; (z80jit_dec.s: internal, the seg_start of the targets, the jumps back),
; which also cuts the segments: they start at the block start, after each
; conditional instruction, at the jump targets and wherever the T-states
; would exceed SEG_MAX_T (cut again here in the rare blocks the decoder
; marks with DF_RECUT). The passes read and write the packed instruction
; records (jit_insn, z80jit_int.h). One walk backward gives the T-states
; left in each segment, the liveness and the fusion. The runs and loops
; of port inputs and outputs are looked for only when the decoder met
; the instructions they are made of, and the groups (jit_out_groups, C)
; only when the block holds at least two port outputs. When the caller
; asks for the static exits of the block, a walk reports them first.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_analyse_block
        IMPORT  jit_busy_loop
        IMPORT  jit_flags_needed
        IMPORT  jit_out_groups

; jit_insn (z80jit_int.h; offsets checked in z80jit.c). The fields of a
; byte are given by their byte offset in the big-endian words.
I_PC            EQU     0
I_TARGET        EQU     4
I_DYN           EQU     11
I_LEN           EQU     16              ; len pre op t
I_W_LEN         EQU     16
I_KIND          EQU     20              ; kind cc use def
I_CC            EQU     21
I_INTERNAL      EQU     24              ; internal busy irq_check seg_start
I_BUSY          EQU     25
I_IRQ_CHECK     EQU     26
I_SEG_START     EQU     27
I_RUN           EQU     28
I_FUSED         EQU     32
I_W_LIVE        EQU     36              ; live_after live_before seg_t t_after
I_LIVE_BEFORE   EQU     37
I_SIZE          EQU     48

; jit_block_ctx (z80jit_int.h): the translator state comes first, then
; the decoded block (its records, their number, the address after it), the
; block's address, what the decoder met, the counts the analysis returns,
; the static exits it reports when asked and the jumps back the decoder
; found.
B_J             EQU     0
B_INS           EQU     4
B_N             EQU     8
B_NEXT_PC       EQU     12
B_PC            EQU     56
B_DEC_FLAGS     EQU     64
B_BUSY          EQU     68
B_TARGETS       EQU     80
B_MAX_TARGETS   EQU     84
B_NTARGETS      EQU     88
B_BACK          EQU     92

; b->dec_flags (JIT_DEC_*).
DF_EDIO         EQU     0x10            ; INI, IND, OUTI, OUTD or OUT (C),A / D / E met
DF_RECUT        EQU     0x20            ; the segments are to be cut again
NOUT_SHIFT      EQU     16              ; port outputs met

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
SEG_HARD        EQU     1               ; seg_start: block start, after a conditional
                                        ; instruction, jump target
SEG_CUT         EQU     2               ; seg_start: cut for the T-states
RUN_MAX         EQU     16              ; Z80J_RUN_MAX
RUN_LOOP        EQU     0xFE000000      ; | the NOP between the head and the jump
MAX_INSNS       EQU     64

; Stack frame.
F_ARGS          EQU     0               ; the block context
F_BC            EQU     4               ; the same (the flag scans' argument)
F_J             EQU     8               ; the translator state
F_N             EQU     12              ; instructions
F_CAFTER        EQU     16              ; flags read after the conditional
                                        ; instruction that follows the current one
F_FUSED         EQU     20              ; counts returned
F_BUSY          EQU     24
F_LINE          EQU     28
F_TARGETS       EQU     32              ; next static exit to report (0: none asked)
F_TEND          EQU     36              ; end of the room for them
F_END           EQU     40              ; end of the records
F_SIZE          EQU     44

; Reports the static exit $v (a Z80 address) when there is room left.
; Uses r12, lr.
        MACRO
        REPORT  $v
        ldr     r12,[sp,#F_TARGETS]
        ldr     lr,[sp,#F_TEND]
        cmp     r12,lr
        strlo   $v,[r12],#4
        strlo   r12,[sp,#F_TARGETS]
        MEND

;----------------------------------------------------------------------------
; void jit_analyse_block(jit_block_ctx *b)
;
; Runs the analysis passes over the b->n decoded instructions of the block
; at b->pc (see translate_block for what each one establishes). Returns
; the busy-wait loops found, those waiting for a line and the fused flag
; tests in b->busy, b->line and b->fused, and the static exits in
; b->targets (b->ntargets of them) when b->targets is set.
;----------------------------------------------------------------------------
jit_analyse_block
        stmfd   sp!,{r4-r9,r11,lr}
        sub     sp,sp,#F_SIZE
        str     r0,[sp,#F_ARGS]
        str     r0,[sp,#F_BC]
        add     r1,r0,#B_INS
        ldmia   r1,{r4,r5,r7}           ; ins, n, next_pc
        str     r5,[sp,#F_N]
        ldr     r1,[r0,#B_J]
        str     r1,[sp,#F_J]
        add     r1,r5,r5,lsl #1
        add     r5,r4,r1,lsl #4         ; end of the records
        str     r5,[sp,#F_END]
        mov     r1,#0
        str     r1,[sp,#F_BUSY]
        str     r1,[sp,#F_LINE]
        str     r1,[sp,#F_FUSED]
        ldr     r1,[r0,#B_TARGETS]
        ldr     r2,[r0,#B_MAX_TARGETS]
        add     r2,r1,r2,lsl #2
        str     r1,[sp,#F_TARGETS]
        str     r2,[sp,#F_TEND]
        cmp     r1,#0
        bne     pr_start

; Busy-wait loops: each jump back the decoder found (b->back, a bit per
; instruction) is given to jit_busy_loop (C) with the index of its
; target, the loop head; the answer, whether the loop waits for an
; interrupt (1) or for a line (2), goes to the jump's record.
; r7 = index, r8 = the bits of the instructions from r7 on, r9 = those of
; the instructions from 32 on.
pr_done
        ldr     r0,[sp,#F_ARGS]
        add     r0,r0,#B_BACK
        ldmia   r0,{r8,r9}
        orrs    r0,r8,r9
        beq     p3_start
        mov     r7,#0
pb_loop
        cmp     r7,#32
        moveq   r8,r9
        movs    r8,r8,lsr #1            ; carry: the jump r7 is one
        bcs     pb_jump
        beq     pb_skip                 ; none left in this word
pb_next
        add     r7,r7,#1
        cmp     r7,#MAX_INSNS
        blo     pb_loop
        b       p3_start
pb_skip
        cmp     r7,#32
        bhs     p3_start
        mov     r7,#32
        b       pb_loop
pb_jump
        add     r11,r4,r7,lsl #4
        add     r11,r11,r7,lsl #5       ; the record of the jump
        ldrb    r3,[r11,#I_INTERNAL]    ; the head of its loop
        ldr     r0,[sp,#F_J]
        mov     r1,r4
        ldr     r2,[sp,#F_N]
        str     r7,[sp,#-4]!            ; fifth argument
        bl      jit_busy_loop
        add     sp,sp,#4
        strb    r0,[r11,#I_BUSY]
        cmp     r0,#0
        beq     pb_next
        ldr     r1,[sp,#F_BUSY]
        add     r1,r1,#1
        str     r1,[sp,#F_BUSY]
        cmp     r0,#2
        ldreq   r1,[sp,#F_LINE]
        addeq   r1,r1,#1
        streq   r1,[sp,#F_LINE]
        b       pb_next

; The static exits of the block, reported when the caller asks for them
; (the code reachable from the entry points, translated before the run),
; in instruction order: the target of a conditional jump leaving the
; block (no instruction of the block starts there), of a JR, JP, CALL or
; RST (inside or not), the return address of a CALL or RST after its
; target, and the address after the block when it does not end with a
; transfer. r0 = record, r5 = end, r7 = next_pc.
pr_start
        mov     r0,r4
pr_loop
        ldrb    r1,[r0,#I_KIND]
        sub     r2,r1,#1
        cmp     r2,#5                   ; conditional (K_JRCC to K_REP)
        bls     pr_cond
        cmp     r1,#K_JR
        cmpne   r1,#K_JP
        beq     pr_static
        cmp     r1,#K_CALL
        cmpne   r1,#K_RST
        bne     pr_next
        ; CALL and RST: the target, then the return address.
        ldrb    r2,[r0,#I_DYN]
        cmp     r2,#0
        bne     pr_next
        ldr     r2,[r0,#I_TARGET]
        REPORT  r2
        ldr     r2,[r0,#I_PC]
        ldrb    r3,[r0,#I_LEN]
        add     r2,r2,r3
        mov     r2,r2,lsl #16
        mov     r2,r2,lsr #16
        REPORT  r2
pr_next
        add     r0,r0,#I_SIZE
        cmp     r0,r5
        blo     pr_loop
        ldrb    r1,[r5,#I_KIND - I_SIZE]
        cmp     r1,#K_JR
        bhs     pr_done
        REPORT  r7
        b       pr_done
pr_cond
        cmp     r1,#K_RETCC
        beq     pr_next
        ldrb    r2,[r0,#I_DYN]
        cmp     r2,#0                   ; the target is read when it runs
        bne     pr_next
        cmp     r1,#K_CALLCC            ; never a jump inside the block
        beq     pr_report
        ldrb    r2,[r0,#I_INTERNAL]
        cmp     r2,#0xFF                ; a target inside the block: no exit
        bne     pr_next
        b       pr_report
pr_static
        ldrb    r2,[r0,#I_DYN]
        cmp     r2,#0
        bne     pr_next
pr_report
        ldr     r2,[r0,#I_TARGET]
        REPORT  r2
        b       pr_next


; Pass 3, backwards: the T-states of each segment from each instruction
; on and after it; the flags live after and before each instruction (a
; conditional instruction reads its condition and whatever its taken path
; reads: everything for RET cc and dynamic targets, the liveness at a
; target inside the block, else the forward scan of jit_flags_needed);
; and the fusion of a flag producer with the conditional jump that is its
; only reader, decided when the producer is reached from what its jump
; needs (c_need, 0 when the jump cannot fuse) and the flags read after the
; jump either way (c_after). The four values of an instruction go to one
; word (live_after live_before seg_t t_after; seg_t is read at segment
; starts only; no segment holds more than SEG_MAX_T T-states).
; r4 = ins, r5 = record, r6 = flags live before the next instruction,
; r7 = T-states of the segment after the record, r8 = c_need; in the
; loop r0 = T-states, r1 = kind cc use def, r9 = flags read << 8, r11 =
; flags written, r12 = internal busy irq_check seg_start.
p3_start
        ldr     r0,[sp,#F_ARGS]
        ldr     r0,[r0,#B_DEC_FLAGS]
        tst     r0,#DF_RECUT
        bne     p2r_start
p3_main
        ldr     r5,[sp,#F_END]
        sub     r5,r5,#I_SIZE           ; the last instruction
        ldrb    r1,[r5,#I_KIND]
        cmp     r1,#K_RET
        movhs   r6,#FL_ALL              ; RET, RETN, JP (HL), HALT
        bhs     p3_init
        cmp     r1,#K_JR
        blo     p3_fall
        ldrb    r0,[r5,#I_DYN]          ; JR, JP, CALL, RST
        cmp     r0,#0
        movne   r6,#FL_ALL
        bne     p3_init
        ldr     r1,[r5,#I_TARGET]
        b       p3_scan0
p3_fall
        ldr     r0,[sp,#F_ARGS]
        ldr     r1,[r0,#B_NEXT_PC]
p3_scan0
        ldr     r0,[sp,#F_BC]
        bl      jit_flags_needed
        mov     r6,r0
p3_init
        mov     r7,#0
        mov     r8,#0
p3_loop
        add     r0,r5,#I_W_LEN
        ldmia   r0,{r0,r1,r12}          ; len pre op t; kind cc use def; internal busy irq_check seg_start
        and     r11,r1,#0xFF            ; flags written
        and     r9,r1,#0xFF00           ; flags read
        and     r0,r0,#0xFF             ; T-states
        cmp     r8,#0
        bne     p3_fuse
p3_live
        mov     r2,r1,lsr #24           ; kind
        sub     r3,r2,#1
        cmp     r3,#5
        bls     p3_cond
        mov     lr,r6                   ; live after
        bic     r6,r6,r11
        orr     r6,r6,r9,lsr #8         ; live before: read, or live after and not written
        mov     r8,#0
p3_store
        add     r2,r7,r0                ; T-states of the segment from here on
        orr     r3,r7,r2,lsl #8         ; seg_t, t_after
        orr     r3,r3,r6,lsl #16        ; live_before
        orr     r3,r3,lr,lsl #24        ; live_after
        str     r3,[r5,#I_W_LIVE]
        tst     r12,#0xFF
        moveq   r7,r2
        movne   r7,#0                   ; a segment start: the previous instruction ends its segment
        movne   r8,#0                   ; and no test is fused across it
        sub     r5,r5,#I_SIZE
        cmp     r5,r4
        bhs     p3_loop

        ; The runs and loops of port inputs and outputs, when the decoder
        ; met their instructions.
        ldr     r0,[sp,#F_ARGS]
        ldr     r0,[r0,#B_DEC_FLAGS]
        tst     r0,#DF_EDIO
        bne     p2_start

; The groups of port outputs (they need the T-states after each one, the
; runs and the loops), when the block holds at least two outputs.
p4_start
        ldr     r0,[sp,#F_ARGS]
        ldr     r0,[r0,#B_DEC_FLAGS]
        cmp     r0,#2 :SHL: NOUT_SHIFT
        blo     p4_done
        ldr     r0,[sp,#F_J]
        ldr     r1,[sp,#F_ARGS]
        add     r1,r1,#B_INS
        ldmia   r1,{r1,r2}              ; ins, n
        bl      jit_out_groups
p4_done

        ldr     r0,[sp,#F_ARGS]
        ldr     r1,[sp,#F_BUSY]
        ldr     r2,[sp,#F_LINE]
        ldr     r3,[sp,#F_FUSED]
        add     r0,r0,#B_BUSY
        stmia   r0,{r1-r3}
        ldr     r1,[sp,#F_TARGETS]
        ldr     r2,[r0,#B_TARGETS - B_BUSY]
        sub     r1,r1,r2
        mov     r1,r1,lsr #2
        str     r1,[r0,#B_NTARGETS - B_BUSY]
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r9,r11,pc}

; A conditional instruction: what its taken path reads.
p3_cond
        cmp     r2,#K_RETCC
        beq     p3_all
        ldrb    r3,[r5,#I_DYN]
        cmp     r3,#0
        bne     p3_all
        mov     r3,r12,lsr #24          ; internal
        cmp     r3,#0xFF
        beq     p3_scan
        add     lr,r4,r3,lsl #4
        add     lr,lr,r3,lsl #5
        cmp     lr,r5                   ; a target further on: its liveness is known
        ldrhib  r3,[lr,#I_LIVE_BEFORE]
        bhi     p3_taken
p3_scan
        stmfd   sp!,{r0-r2,r12}
        ldr     r0,[sp,#F_BC + 16]
        ldr     r1,[r5,#I_TARGET]
        bl      jit_flags_needed
        mov     r3,r0
        ldmfd   sp!,{r0-r2,r12}
        b       p3_taken
p3_all
        mov     r3,#FL_ALL
p3_taken
        mov     lr,r6                   ; live after
        orr     r6,r6,r9,lsr #8
        orr     r6,r6,r3                ; live before: the condition and both paths
        orr     r3,r3,lr                ; read after the jump, either way
        str     r3,[sp,#F_CAFTER]
        cmp     r2,#K_DJNZ
        cmpne   r2,#K_REP
        moveq   r8,#0
        beq     p3_store
        mov     r3,r1,lsr #16
        and     r3,r3,#0xFF             ; condition
        adr     r2,cc_need
        ldrb    r8,[r2,r3,lsr #1]       ; what the condition can use
        b       p3_store

; FU_* a condition can use: NZ Z, NC C, PO PE, P M.
cc_need DCB     FU_Z_EQ + FU_Z_CS, FU_C_CS + FU_C_CC, FU_V_VS, FU_S_MI

; The next instruction is a conditional jump that can take its flags from
; the ARM flags (c_need): this instruction fuses with it when it produces
; them (r2 from its opcode), has no interrupt check, and none of the flags
; it writes is read after the jump. r0, r1, r9, r11 and r12 hold the
; loaded fields; r2, r3 and lr are free.
p3_fuse
        tst     r12,#0xFF00             ; interrupt check
        bne     p3_live
        ldr     lr,[r5,#I_W_LEN]
        mov     r3,lr,lsr #16
        and     r3,r3,#0xFF             ; prefix
        mov     lr,lr,lsr #8
        and     lr,lr,#0xFF             ; opcode
        mov     r2,lr,lsr #6            ; x
        cmp     r3,#PRE_CB
        beq     fm_cb
        cmp     r3,#PRE_ED
        beq     fm_ed
        cmp     r3,#PRE_DDCB
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
        moveq   r2,#0
        movne   r2,#FU_Z_EQ + FU_S_MI + FU_V_VS        ; INC / DEC r
        b       fm_done
fm_x0
        cmp     r3,#1
        bne     fm_none
        tst     lr,#8
        movne   r2,#FU_C_CS             ; ADD HL,rr
        moveq   r2,#0
        b       fm_done
fm_alu
        and     r3,lr,#0x38
        cmp     r3,#0x08
        movls   r2,#FU_Z_EQ + FU_C_CS + FU_S_MI + FU_V_VS      ; ADD, ADC
        bls     fm_done
        cmp     r3,#0x18
        bls     fm_sub
        cmp     r3,#0x38
        beq     fm_sub
        mov     r2,#FU_Z_EQ + FU_S_MI   ; AND, XOR, OR
        b       fm_done
fm_sub
        mov     r2,#FU_Z_EQ + FU_C_CC + FU_S_MI + FU_V_VS      ; SUB, SBC, CP
        b       fm_done
fm_cb
        cmp     r2,#1                   ; BIT
        moveq   r2,#FU_Z_EQ
        movne   r2,#0
        b       fm_done
fm_ed
        cmp     r2,#1
        bne     fm_none
        and     r3,lr,#7
        cmp     r3,#4                   ; NEG
        moveq   r2,#FU_Z_EQ + FU_C_CC + FU_S_MI + FU_V_VS
        beq     fm_done
        cmp     r3,#2                   ; SBC / ADC HL,rr
        bne     fm_none
        tst     lr,#8
        movne   r2,#FU_Z_EQ + FU_S_MI + FU_V_VS + FU_C_CS
        moveq   r2,#FU_Z_EQ + FU_S_MI + FU_V_VS + FU_C_CC
        b       fm_done
fm_none
        mov     r2,#0
fm_done
        ands    r2,r2,r8
        beq     p3_live
        ldr     lr,[sp,#F_CAFTER]
        tst     r11,lr                  ; a flag written here is read after the jump
        bne     p3_live
        strb    r2,[r5,#I_FUSED]
        ldr     lr,[sp,#F_FUSED]
        add     lr,lr,#1
        str     lr,[sp,#F_FUSED]
        b       p3_live

; Pass 2r, when the decoder found a jump target before a segment it cut
; for its T-states (DF_RECUT): the cuts (seg_start SEG_CUT) are made again
; from the other segment starts (SEG_HARD: the block start, the
; instructions after a conditional one and the jump targets), whenever
; the running total of T-states would exceed SEG_MAX_T. Then pass 3.
; r0 = record, r1 = T-states of the segment so far, r2 = those of the
; instruction, r12 = its seg_start, lr = the new one.
p2r_start
        mov     r0,r4
        ldr     r5,[sp,#F_END]
        mov     r1,#0
p2r_loop
        ldr     r2,[r0,#I_W_LEN]
        ldrb    r12,[r0,#I_SEG_START]
        and     r2,r2,#0xFF             ; T-states
        cmp     r12,#SEG_HARD
        moveq   r1,#0
        beq     p2r_add
        add     lr,r1,r2
        cmp     lr,#SEG_MAX_T
        movhi   r1,#0
        movhi   lr,#SEG_CUT             ; the segment would be too long: cut it here
        movls   lr,#0
        cmp     lr,r12
        strneb  lr,[r0,#I_SEG_START]
p2r_add
        add     r1,r1,r2
        add     r0,r0,#I_SIZE
        cmp     r0,r5
        blo     p2r_loop
        b       p3_main

; Pass 2: port output runs. Consecutive OUTI, or OUTD, or OUTI mixed with
; OUT (C),r on one register among A, D and E, inside a segment, make one
; run (at most RUN_MAX elements, ended after an element with an interrupt
; check): the head gets the length, the mask of the OUT (C),r elements
; and their opcode, the others RUN_PART. The port loops are found by pass
; 2b. r0 = record, r2 = state: bit 1 the previous element of the run has
; an interrupt check; r3 = head of the open run or 0, r6 = its run word
; so far, r7 = the opcode of its head; r8 = the len pre op t word of the
; instruction, r11 = its seg_start.
p2_start
        mov     r0,r4
        ldr     r5,[sp,#F_END]
        mov     r2,#0
        mov     r3,#0
p2_loop
        ldr     r8,[r0,#I_W_LEN]        ; len pre op t
        ldrb    r11,[r0,#I_SEG_START]
        cmp     r3,#0
        bne     p2_open
p2_newrun
        mov     r12,r8,lsr #16
        and     r12,r12,#0xFF           ; prefix
        cmp     r12,#PRE_ED
        beq     p2_ed
p2_next
        add     r0,r0,#I_SIZE
        cmp     r0,r5
        blo     p2_loop
        cmp     r3,#0                   ; a run reaching the end of the block
        beq     p2b_start
        and     r12,r6,#0xFF
        cmp     r12,#2
        blcs    run_write
        b       p2b_start

; A run is open: this instruction extends it or ends it.
p2_open
        tst     r2,#2                   ; the run ends after an interrupt check
        bne     p2_close
        and     r12,r6,#0xFF
        cmp     r12,#RUN_MAX
        beq     p2_close
        cmp     r11,#0                  ; and before a segment start
        bne     p2_close
        mov     r12,r8,lsr #16
        and     r12,r12,#0xFF           ; prefix
        cmp     r12,#PRE_ED
        bne     p2_close
        mov     r9,r8,lsr #8
        and     r9,r9,#0xFF             ; opcode
        bl      run_elem
        cmp     r11,#0
        beq     p2_close
        cmp     r11,#2                  ; OUT (C),r: its bit in the mask
        andeq   r12,r6,#0xFF
        addeq   r12,r12,#8
        moveq   lr,#1
        orreq   r6,r6,lr,lsl r12
        add     r6,r6,#1                ; one more element
        ldrb    r12,[r0,#I_IRQ_CHECK]
        bic     r2,r2,#2
        cmp     r12,#0
        orrne   r2,r2,#2
        b       p2_next

; The open run ends before this instruction.
p2_close
        and     r12,r6,#0xFF
        cmp     r12,#2
        blhs    run_write
        mov     r3,#0
        b       p2_newrun

; No open run: this ED instruction may start one.
p2_ed
        mov     r7,r8,lsr #8
        and     r7,r7,#0xFF             ; opcode
        mov     r9,r7
        mov     r6,#0
        bl      run_elem
        cmp     r11,#0
        beq     p2_next
        mov     r3,r0
        cmp     r11,#2
        orreq   r6,r6,#0x100            ; mask bit of the first element
        add     r6,r6,#1
        ldrb    r12,[r0,#I_IRQ_CHECK]
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

; Pass 2b: port loops. INI, IND, OUTI or OUTD outside a run, followed by
; NOP only and then a JR NZ or JP NZ back to it, none of them starting a
; segment past the head or carrying an interrupt check, the jump with
; neither a busy-wait mark nor a dynamic target: the head gets RUN_LOOP |
; the number of NOP, and the B passes go in one call (emit_in_loop,
; emit_out_loop).
; r0 = record, r1 = its index, r2 = the record walked, r3 = NOP count;
; r6, r7, r12 hold the words read, lr is a temporary.
p2b_start
        mov     r0,r4
        mov     r1,#0
p2b_loop
        add     r2,r0,#I_W_LEN
        ldmia   r2,{r2,r3,r12,lr}       ; len pre op t; kind cc use def; internal busy irq_check seg_start; run
        cmp     lr,#0                   ; in a run
        bne     p2b_next
        mov     r3,r2,lsr #16
        and     r3,r3,#0xFF             ; prefix
        cmp     r3,#PRE_ED
        bne     p2b_next
        mov     r3,r2,lsr #8
        and     r3,r3,#0xF6             ; opcode without its direction and port bits
        cmp     r3,#0xA2                ; INI, IND, OUTI, OUTD
        bne     p2b_next
        tst     r12,#0xFF00             ; interrupt check
        bne     p2b_next
        mov     r2,r0
        mov     r3,#0
p2b_walk
        add     r2,r2,#I_SIZE
        cmp     r2,r5
        bhs     p2b_next                ; the end of the block
        add     r6,r2,#I_W_LEN
        ldmia   r6,{r6,r7,r12}          ; len pre op t; kind cc use def; internal busy irq_check seg_start
        mov     lr,r6,lsl #8
        movs    lr,lr,lsr #16           ; prefix and opcode: zero for NOP
        bne     p2b_jump
        tst     r12,#0xFF00             ; interrupt check
        tsteq   r12,#0xFF               ; or segment start
        bne     p2b_next
        add     r3,r3,#1
        b       p2b_walk
p2b_jump
        mov     lr,r7,lsr #24           ; kind
        cmp     lr,#K_JRCC
        cmpne   lr,#K_JPCC
        bne     p2b_next
        tst     r7,#0xFF0000            ; condition: NZ is 0
        bne     p2b_next
        tst     r12,#0xFF0000           ; busy-wait mark,
        tsteq   r12,#0xFF00             ; interrupt check
        tsteq   r12,#0xFF               ; or segment start
        bne     p2b_next
        ldrb    lr,[r2,#I_DYN]
        cmp     lr,#0
        bne     p2b_next
        mov     lr,r12,lsr #24          ; the target inside the block
        cmp     lr,r1                   ; back to the head
        bne     p2b_next
        orr     r3,r3,#RUN_LOOP
        str     r3,[r0,#I_RUN]
p2b_next
        add     r0,r0,#I_SIZE
        add     r1,r1,#1
        cmp     r0,r5
        blo     p2b_loop


        b       p4_start

        END
