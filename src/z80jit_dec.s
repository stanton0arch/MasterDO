; Z80 instruction decoder of the translator (see z80jit.c, translate_block):
; the properties of an instruction (length, T-states, kind, flags read and
; written, operands) are packed into the six words of its record
; (jit_insn, z80jit_int.h: the fields of a byte share words) and written
; with one STM; the instructions of a block are decoded in one loop that
; also fills the instruction map of the block, writes the marks the
; analysis passes start from, and resolves the jumps whose static target
; is an instruction of the block once the block is complete.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_decode
        EXPORT  jit_decode_block
        EXPORT  jit_dec_core            ; for the profiles of the model
        IMPORT  jit_dec_tab

; jit_insn (z80jit_int.h; offsets checked in z80jit.c). The fields of a
; byte are given by their byte offset in the big-endian words.
I_PC            EQU     0
I_TARGET        EQU     4
I_EI            EQU     8               ; ei mem d dyn
I_DYN           EQU     11
I_N             EQU     12
I_LEN           EQU     16              ; len pre op t
I_PRE           EQU     17
I_OP            EQU     18
I_KIND          EQU     20              ; kind cc use def
I_INTERNAL      EQU     24              ; internal busy irq_check seg_start
I_IRQ_CHECK     EQU     26
I_SEG_START     EQU     27
I_RUN           EQU     28
I_FUSED         EQU     32
I_CORE          EQU     24              ; end of the six words the core writes
I_MARKED        EQU     36              ; end of the three words of marks
I_SIZE          EQU     48

; jit_block_ctx (z80jit_int.h).
B_INS           EQU     4
B_N             EQU     8
B_NEXT_PC       EQU     12
B_KEY           EQU     16
B_HAS_DYN       EQU     36
B_PC            EQU     56
B_SERIAL        EQU     60
B_CTX           EQU     64
B_DEC_FLAGS     EQU     68
B_BACK          EQU     96

; z80j_ctx, from its start (z80jit.h).
C_SLOT_BANK     EQU     0x57C
C_PAGE_KIND     EQU     0x600
PAGE_KIND       EQU     0x0F
KEY_RAM         EQU     0x80000

; Opcode properties (jit_dec_tab, built by main_info in z80jit.c).
MI_T            EQU     31
MI_KIND         EQU     0x000001E0
MI_USE          EQU     0x00007E00
MI_DEF          EQU     0x001F8000
MI_REL          EQU     0x00200000
MI_TN           EQU     0x00400000
MI_RST          EQU     0x00800000
MI_HL           EQU     0x01000000
MI_MEM          EQU     0x02000000
MI_EI           EQU     0x04000000
MI_CC           EQU     0x38000000
MI_NB_SH        EQU     30

PRE_NONE        EQU     0
PRE_CB          EQU     1
PRE_ED          EQU     2
PRE_DD          EQU     3
PRE_FD          EQU     4
PRE_DDCB        EQU     5

K_NORMAL        EQU     0
K_JRCC          EQU     1
K_JPCC          EQU     2
K_CALLCC        EQU     3
K_RETCC         EQU     4
K_REP           EQU     6
K_JR            EQU     7               ; first kind ending a block
K_JP            EQU     8
K_CALL          EQU     9
K_RETN          EQU     12

FL_Z            EQU     0x02
FL_H            EQU     0x04
FL_PV           EQU     0x08
FL_N            EQU     0x10
FL_C            EQU     0x20
FL_ALL          EQU     0x3F
FL_INCDEC       EQU     0x1F

MAX_INSNS       EQU     64

; Stack frame of jit_decode_block.
F_ARGS          EQU     0
F_PC0           EQU     4               ; address of the block
F_FLAGS         EQU     8               ; DF_*
F_RTAB0         EQU     12              ; read table entry of its first page
F_KEY           EQU     16              ; its key
F_NJUMPS        EQU     20              ; jumps with a static target met
F_JUMPS         EQU     24              ; their list: target | index << 16 | kind << 24
F_SIZE          EQU     F_JUMPS + 4 * MAX_INSNS

DF_RAM          EQU     1               ; block of RAM code
DF_EI           EQU     2               ; the previous instruction is EI
DF_DYN          EQU     4               ; some operand is read when it runs
DF_SEAM         EQU     8               ; the block ends at a seam

;----------------------------------------------------------------------------
; jit_dec_core: decodes the instruction at r1 (16 bits) into the record at r0.
; In: r0 = record, r1 = address, r2 = context, r11 = jit_dec_tab.
; Out: the six decoded words of the record written (pc; target; ei mem d
; dyn; n; len pre op t; kind cc use def), r0 = record + 24, r1 = address
; of the next instruction, r2 = kind, r4 = target, r7 = EI.
; r3, r5, r6, r8 and r12 are lost; r9-r11 and lr are kept.
;
; The four bytes at the address are read as one word, the first byte in
; bits 24-31 (two aligned words combined; a page starts on a word boundary
; of the host memory, so they stay in the page). Unprefixed opcodes and
; those after DD / FD are decoded from their properties in jit_dec_tab;
; the immediate operand bytes are then in bits 16-23 and 8-15 of r12.
;----------------------------------------------------------------------------
jit_dec_core
        and     r3,r1,#0xFF
        cmp     r3,#0xFC
        bhi     dc_fetch_pages
        mvn     r3,r1,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r2,r3,lsl #2]       ; read table entry of the page
        add     r3,r3,r1                ; host address of the instruction
        and     r4,r3,#3
        bic     r3,r3,#3
        ldmia   r3,{r5,r6}
        movs    r4,r4,lsl #3
        moveq   r12,r5
        movne   r12,r5,lsl r4
        rsbne   r4,r4,#32
        orrne   r12,r12,r6,lsr r4
dc_fetched
        mov     r4,r12,lsr #24          ; opcode
        ldr     r8,[r11,r4,lsl #2]      ; its properties
        ands    r7,r8,#MI_T             ; T-states
        beq     dc_prefix               ; CB, DD, ED, FD have none of their own
        mov     r2,#1                   ; length before the immediate operand
        mov     r3,#PRE_NONE
        mov     r5,#0                   ; displacement

; r1 = address, r2 = length before the immediate operand, r3 = prefix,
; r4 = opcode, r5 = displacement, r7 = T-states, r8 = properties,
; r12 = immediate operand bytes in bits 16-23 and 8-15.
dc_main
        movs    r6,r8,lsr #MI_NB_SH     ; immediate bytes (0: no operand)
        add     r2,r2,r6                ; length
        cmp     r6,#1
        andhs   r6,r12,#0xFF0000
        movhs   r6,r6,lsr #16
        andhi   r12,r12,#0xFF00
        orrhi   r6,r6,r12               ; operand
        orr     r7,r7,r4,lsl #8
        orr     r7,r7,r3,lsl #16
        orr     r7,r7,r2,lsl #24        ; len pre op t
        mov     r3,r1                   ; pc
        add     r1,r1,r2
        bic     r1,r1,#0x10000          ; next address
        tst     r8,#MI_REL + MI_TN + MI_RST
        moveq   r4,#0                   ; no target
        bne     dc_target
; r1 = next address, r3 = pc, r4 = target, r5 = displacement, r6 = operand,
; r7 = len pre op t, r8 = properties.
dc_fields
        and     r5,r5,#0xFF
        mov     r5,r5,lsl #8
        and     r12,r8,#MI_EI
        orr     r5,r5,r12,lsr #2
        and     r12,r8,#MI_MEM
        orr     r5,r5,r12,lsr #9        ; ei mem d (dyn 0)
        and     r12,r8,#MI_KIND
        mov     r12,r12,lsl #19
        and     r2,r8,#MI_CC
        orr     r12,r12,r2,lsr #11
        and     r2,r8,#MI_USE
        orr     r12,r12,r2,lsr #1
        and     r2,r8,#MI_DEF
        orr     r8,r12,r2,lsr #15       ; kind cc use def
        stmia   r0!,{r3-r8}
        mov     r2,r8,lsr #24           ; kind
        mov     r7,r5,lsr #24           ; EI
        mov     pc,lr

dc_target
        tst     r8,#MI_RST
        andne   r4,r4,#0x38             ; RST: its vector
        bne     dc_fields
        tst     r8,#MI_TN
        movne   r4,r6                   ; JP, CALL: the operand
        bne     dc_fields
        mov     r4,r6,lsl #24
        add     r4,r1,r4,asr #24        ; JR, DJNZ: next address + displacement
        mov     r4,r4,lsl #16
        mov     r4,r4,lsr #16
        b       dc_fields

dc_prefix
        cmp     r4,#0xCB
        beq     dc_cb
        cmp     r4,#0xED
        beq     dc_ed
        cmp     r4,#0xDD
        moveq   r3,#PRE_DD
        movne   r3,#PRE_FD
        mov     r12,r12,lsl #8          ; the bytes after the prefix
        mov     r4,r12,lsr #24          ; opcode
        cmp     r4,#0xCB
        beq     dc_xycb
        ldr     r8,[r11,r4,lsl #2]
        ands    r7,r8,#MI_T
        beq     dc_nop                  ; another prefix: this one is a no-op
        tst     r8,#MI_MEM
        bne     dc_xymem
        tst     r8,#MI_HL
        beq     dc_nop                  ; no H, L or (HL): the prefix alone
        add     r7,r7,#4                ; IX / IY for HL, H or L
        mov     r2,#2
        mov     r5,#0
        b       dc_main
dc_xymem
        mov     r5,r12,lsl #8
        mov     r5,r5,asr #24           ; displacement
        mov     r12,r12,lsl #8          ; the bytes after it
        cmp     r4,#0x36
        moveq   r7,#19                  ; LD (IX+d),n
        addne   r7,r7,#12
        mov     r2,#3
        b       dc_main

; A 4 T-state no-op (a prefix without effect); the next opcode is the next
; instruction.
dc_nop
        mov     r3,r1                   ; pc
        mov     r4,#0                   ; no target
        mov     r5,#0                   ; no EI, no memory operand, no displacement
        mov     r6,#0                   ; no operand
        mov     r7,#4                   ; 4 T-states, opcode 0, no prefix,
        orr     r7,r7,#0x01000000       ; one byte
        mov     r8,#0                   ; K_NORMAL, no condition, no flags
        stmia   r0!,{r3-r8}
        add     r1,r1,#1
        bic     r1,r1,#0x10000
        mov     r2,#K_NORMAL
        mov     r7,#0                   ; EI
        mov     pc,lr

dc_cb
        mov     r4,r12,lsl #8
        mov     r4,r4,lsr #24           ; opcode
        mov     r3,#PRE_CB
        mov     r2,#2
        mov     r5,#0
        and     r8,r4,#7
        cmp     r8,#6
        movne   r7,#8
        movne   r8,#0
        bne     dc_cbx
        mov     r8,#1                   ; (HL)
        mov     r7,r4,lsr #6
        cmp     r7,#1
        moveq   r7,#12                  ; BIT n,(HL)
        movne   r7,#15
; CB opcodes, with or without DD / FD: r1-r5 and r7 as for dc_main, r8 =
; (HL) or (IX+d) operand.
dc_cbx
        orr     r7,r7,r4,lsl #8
        orr     r7,r7,r3,lsl #16
        orr     r7,r7,r2,lsl #24        ; len pre op t
        mov     r3,r1                   ; pc
        add     r1,r1,r2
        bic     r1,r1,#0x10000
        and     r5,r5,#0xFF
        mov     r5,r5,lsl #8
        orr     r5,r5,r8,lsl #16        ; no EI, memory operand, displacement
        mov     r8,#0                   ; RES, SET: no flags
        movs    r6,r4,lsr #6            ; x
        moveq   r8,#FL_ALL              ; rotations and shifts write every flag
        andeq   r4,r4,#0x30
        cmpeq   r4,#0x10
        orreq   r8,r8,#FL_C :SHL: 8     ; RL, RR read C
        cmp     r6,#1
        moveq   r8,#FL_INCDEC           ; BIT
        mov     r4,#0                   ; no target
        mov     r6,#0                   ; no operand
        stmia   r0!,{r3-r8}
        mov     r2,#K_NORMAL
        mov     r7,#0                   ; EI
        mov     pc,lr

; r3 = PRE_DD or PRE_FD, r12 = CB d op.
dc_xycb
        add     r3,r3,#PRE_DDCB - PRE_DD
        mov     r5,r12,lsl #8
        mov     r5,r5,asr #24           ; displacement
        mov     r4,r12,lsl #16
        mov     r4,r4,lsr #24           ; opcode
        mov     r2,#4
        mov     r8,#1
        mov     r7,r4,lsr #6
        cmp     r7,#1
        moveq   r7,#20                  ; BIT n,(IX+d)
        movne   r7,#23
        b       dc_cbx

; ED opcodes: r8 gathers the flags written, the flags read << 8 and the
; kind << 16 while r1-r7 hold the first decoded fields.
dc_ed
        mov     r4,r12,lsl #8
        mov     r4,r4,lsr #24           ; opcode
        mov     r3,#PRE_ED
        mov     r2,#2
        mov     r7,#8
        mov     r8,#0
        mov     r5,r4,lsr #6            ; x
        and     r6,r4,#7                ; z
        cmp     r5,#1
        beq     dc_ed1
        cmp     r5,#2
        bne     dc_edx
        cmp     r6,#3
        bhi     dc_edx
        tst     r4,#0x20                ; block instructions: y >= 4
        beq     dc_edx
        mov     r7,#16
        cmp     r6,#1
        movlo   r8,#FL_H + FL_PV + FL_N ; LDI...
        moveq   r8,#FL_INCDEC           ; CPI...
        movhi   r8,#FL_Z + FL_N         ; INI..., OUTI...
        tst     r4,#0x10                ; y >= 6: repeated
        orrne   r8,r8,#K_REP :SHL: 16
        b       dc_edx
dc_ed1
        add     pc,pc,r6,lsl #2
        mov     r0,r0
        b       dc_ed_in
        b       dc_ed_out
        b       dc_ed_adc
        b       dc_ed_ldnn
        b       dc_ed_neg
        b       dc_ed_retn
        b       dc_edx                  ; IM
        mov     r5,r4,lsr #3
        and     r5,r5,#7                ; y
        cmp     r5,#1
        movls   r7,#9                   ; LD I,A / LD R,A
        bls     dc_edx
        cmp     r5,#3
        movls   r7,#9                   ; LD A,I / LD A,R
        movls   r8,#FL_INCDEC
        orrls   r8,r8,#FL_C :SHL: 8
        bls     dc_edx
        cmp     r5,#5
        movls   r7,#18                  ; RRD / RLD
        movls   r8,#FL_INCDEC
        b       dc_edx
dc_ed_in
        mov     r7,#12                  ; IN r,(C)
        mov     r8,#FL_INCDEC
        b       dc_edx
dc_ed_out
        mov     r7,#12                  ; OUT (C),r
        b       dc_edx
dc_ed_adc
        mov     r7,#15                  ; SBC / ADC HL,rr
        mov     r8,#FL_ALL
        orr     r8,r8,#FL_C :SHL: 8
        b       dc_edx
dc_ed_neg
        mov     r8,#FL_ALL              ; NEG
        b       dc_edx
dc_ed_retn
        mov     r7,#14                  ; RETN / RETI
        mov     r8,#K_RETN :SHL: 16
        b       dc_edx
dc_ed_ldnn
        mov     r7,#20                  ; LD (nn),rr / LD rr,(nn)
        mov     r2,#4
        and     r6,r12,#0xFF00
        mov     r6,r6,lsr #8
        and     r5,r12,#0xFF
        orr     r6,r6,r5,lsl #8         ; operand
        mov     r5,#0
        b       dc_edn
dc_edx
        mov     r5,#0                   ; displacement
        mov     r6,#0                   ; operand
; r1 = pc, r2 = length, r3 = PRE_ED, r4 = opcode, r5 = 0, r6 = operand,
; r7 = T-states, r8 = flags written | flags read << 8 | kind << 16.
dc_edn
        orr     r7,r7,r4,lsl #8
        orr     r7,r7,r3,lsl #16
        orr     r7,r7,r2,lsl #24        ; len pre op t
        mov     r3,r1                   ; pc
        add     r1,r1,r2
        bic     r1,r1,#0x10000          ; next address
        mov     r2,r8,lsr #16           ; kind
        cmp     r2,#K_REP
        moveq   r4,r3                   ; repeated: the instruction itself
        movne   r4,#0                   ; no target
        and     r12,r8,#0xFF
        and     r8,r8,#0xFF00
        orr     r8,r8,r12
        orr     r8,r8,r2,lsl #24        ; kind, no condition, use, def
        stmia   r0!,{r3-r8}             ; (r5: no EI, no memory operand)
        mov     r7,#0                   ; EI
        mov     pc,lr

; The instruction starts in the last three bytes of a page: each byte is
; read through the page holding it.
dc_fetch_pages
        mov     r12,#0
        mov     r5,r1
        mov     r6,#4
dc_fp_loop
        mvn     r3,r5,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r2,r3,lsl #2]
        ldrb    r3,[r3,r5]
        orr     r12,r3,r12,lsl #8
        add     r5,r5,#1
        bic     r5,r5,#0x10000
        subs    r6,r6,#1
        bne     dc_fp_loop
        b       dc_fetched

;----------------------------------------------------------------------------
; void jit_decode(jit_insn *in, uint32 pc, const z80j_ctx *c)
;
; Decodes the instruction at pc (16 bits) into the first six words of *in.
;----------------------------------------------------------------------------
jit_decode
        stmfd   sp!,{r4-r8,r11,lr}
        ldr     r11,dc_tab
        bl      jit_dec_core
        ldmfd   sp!,{r4-r8,r11,pc}

dc_tab  DCD     jit_dec_tab

;----------------------------------------------------------------------------
; int32 jit_decode_block(jit_block_ctx *b)
;
; Decodes the instructions of a block from b->pc into b->ins, up to an
; unconditional transfer, a page of another kind than b->key or the size
; limit, as translate_block describes, and returns their number (0 when
; the first one crosses a seam of RAM). For each one: the instruction map
; entry of its address, b->serial | its number from 1 (jit_dec_tab, 256
; entries from 256, indexed by the low byte of the address: a block spans
; at most 256 bytes); its decoded words; the marks: busy, run, fused and
; seg_start 0, irq_check set when it follows EI and is no transfer or
; conditional (the EI gets it otherwise), dyn set in RAM for JP / CALL
; and the absolute loads and stores of A and HL, internal the index of
; the instruction its static target starts, or -1.
;
; The jumps with a static target (JR, JP, JR cc, JP cc, DJNZ, a repeated
; block instruction) are listed as they are met and resolved once the
; block is complete: a target inside the block whose instruction map
; entry belongs to this decoding starts an instruction, which gets
; seg_start; a JR, JP, JR cc or JP cc to an instruction at or before it
; gets its bit in b->back, for the busy-wait loop analysis. The jumps
; whose target is outside the block, inside an instruction, or read when
; they run keep -1.
;
; b->n receives the count, b->next_pc the address after the last
; instruction, b->dec_flags DF_DYN and DF_SEAM, b->has_dyn whether
; DF_DYN is set.
;----------------------------------------------------------------------------
jit_decode_block
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#F_SIZE
        ldr     r4,[r0,#B_INS]
        ldr     r7,[r0,#B_KEY]
        add     r1,r0,#B_PC
        ldmia   r1,{r5,r6,r8}           ; pc, serial, ctx
        mov     r9,r8                   ; context
        mov     r10,r6                  ; serial | number of the instruction
        ldr     r11,dc_tab
        mov     r1,r5,lsl #16
        mov     r1,r1,lsr #16           ; address
        mvn     r3,r1,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r9,r3,lsl #2]       ; read table entry of the first page
        cmp     r7,#KEY_RAM
        moveq   r2,#DF_RAM
        movne   r2,#0
        mov     r8,#0
        stmia   sp,{r0-r3,r7,r8}        ; args, pc, flags, entry, key, no jump yet
        mov     r0,r4

; r0 = record, r1 = address, r9 = context, r10 = serial | number,
; r11 = jit_dec_tab; r2, r4 and r7 hold the kind, the target and the EI
; of the instruction once decoded, r12 its dyn, lr the flags.
db_loop
        add     r10,r10,#1
        and     r3,r1,#0xFF
        add     r3,r11,r3,lsl #2
        str     r10,[r3,#1024]          ; instruction map
        mov     r2,r9
        bl      jit_dec_core
        ldr     lr,[sp,#F_FLAGS]
        mov     r12,#0                  ; dyn
        tst     lr,#DF_RAM
        bne     db_ram
db_ei
        mov     r5,#0                   ; irq_check
        tst     lr,#DF_EI
        bne     db_after_ei
db_set
        teq     r7,#0
        orrne   lr,lr,#DF_EI
        strne   lr,[sp,#F_FLAGS]
        mov     r6,#0xFF000000          ; internal -1, busy 0
        orr     r6,r6,r5,lsl #8         ; irq_check, seg_start 0
        sub     r3,r2,#K_JRCC
        cmp     r3,#K_JP - K_JRCC       ; JR cc to JP: a static target
        bls     db_jump
db_marks
        mov     r7,#0                   ; run
        mov     r8,#0                   ; fused
        stmia   r0!,{r6-r8}
        add     r0,r0,#I_SIZE - I_MARKED
        cmp     r2,#K_JR                ; an unconditional transfer ends the block
        bhs     db_end
        and     r3,r10,#0xFF
        cmp     r3,#MAX_INSNS
        beq     db_end
        and     r3,r1,#0xFF             ; a page boundary was crossed when the
        cmp     r3,#4                   ; low byte of the next address is below
        bhs     db_loop                 ; the length of the instruction
        ldrb    r4,[r0,#I_LEN - I_SIZE]
        cmp     r3,r4
        bhs     db_loop
        add     r3,r9,r1,lsr #8         ; key of the new page (jit_block_key)
        ldrb    r3,[r3,#C_PAGE_KIND]
        and     r3,r3,#PAGE_KIND
        sub     r4,r3,#1
        cmp     r4,#3
        bls     db_slot
        cmp     r3,#0
        movne   r3,#KEY_RAM
        b       db_key
db_slot
        add     r4,r9,r4,lsl #2
        ldr     r4,[r4,#C_SLOT_BANK]
        mov     r4,r4,lsl #16
        orr     r3,r3,r4
        mov     r3,r3,ror #16           ; kind << 16 | bank
db_key
        ldr     r4,[sp,#F_KEY]
        cmp     r3,r4
        beq     db_loop
db_end
        ldr     r3,[sp,#F_ARGS]
        ldr     lr,[sp,#F_FLAGS]
        str     r1,[r3,#B_NEXT_PC]
        str     lr,[r3,#B_DEC_FLAGS]
        and     r0,r10,#0xFF
        str     r0,[r3,#B_N]
        and     lr,lr,#DF_DYN
        mov     lr,lr,lsr #2
        str     lr,[r3,#B_HAS_DYN]

        ; The jumps listed: a target inside the block (its offset from
        ; the block start below the span) whose instruction map entry is
        ; of this decoding starts an instruction of the block (the
        ; addresses of a block have distinct low bytes). The instruction
        ; excluded at a seam has the span as offset.
        ; r0 = span, lr = block address, r4 = ins, r6 = list, r5 = its
        ; end, r8 and r9 = the jumps back.
        ldr     r4,[r3,#B_INS]
        ldr     lr,[sp,#F_PC0]
        sub     r0,r1,lr
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r8,#0
        mov     r9,#0
        ldr     r5,[sp,#F_NJUMPS]
        add     r6,sp,#F_JUMPS
        add     r5,r6,r5,lsl #2
db_resolve
        cmp     r6,r5
        bhs     db_resolved
        ldr     r7,[r6],#4
        sub     r12,r7,lr
        mov     r12,r12,lsl #16
        mov     r12,r12,lsr #16         ; offset of the target
        cmp     r12,r0
        bhs     db_resolve              ; outside the block
        and     r12,r7,#0xFF
        add     r12,r11,r12,lsl #2
        ldr     r12,[r12,#1024]
        eor     r1,r12,r10
        cmp     r1,#0x100               ; an entry of this decoding
        bhs     db_resolve              ; inside an instruction
        and     r12,r12,#0xFF
        sub     r12,r12,#1              ; index of the target
        mov     r1,r7,lsr #16
        and     r1,r1,#0xFF             ; index of the jump
        add     r2,r4,r1,lsl #4
        add     r2,r2,r1,lsl #5
        strb    r12,[r2,#I_INTERNAL]
        add     r2,r4,r12,lsl #4
        add     r2,r2,r12,lsl #5        ; the record of the target
        mov     r3,#1
        strb    r3,[r2,#I_SEG_START]    ; a jump target starts a segment
        cmp     r12,r1                  ; a jump back, or to itself
        bhi     db_resolve
        mov     r2,r7,lsr #24           ; kind
        cmp     r2,#K_JRCC
        cmpne   r2,#K_JPCC
        cmpne   r2,#K_JR
        cmpne   r2,#K_JP
        bne     db_resolve
        cmp     r1,#32
        orrlo   r8,r8,r3,lsl r1
        subhs   r1,r1,#32
        orrhs   r9,r9,r3,lsl r1
        b       db_resolve
db_resolved
        ldr     r3,[sp,#F_ARGS]
        add     r3,r3,#B_BACK
        stmia   r3,{r8,r9}
        and     r0,r10,#0xFF            ; the count
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r11,pc}

; A jump with a static target (r4), listed with its index and kind
; unless the target is read when it runs.
db_jump
        cmp     r2,#K_CALLCC            ; never inside the block
        cmpne   r2,#K_RETCC
        beq     db_marks
        cmp     r12,#0
        bne     db_marks
        ldr     r3,[sp,#F_NJUMPS]
        and     r5,r10,#0xFF
        sub     r5,r5,#1                ; index of this instruction
        orr     r5,r4,r5,lsl #16
        orr     r5,r5,r2,lsl #24
        add     r12,sp,#F_JUMPS
        str     r5,[r12,r3,lsl #2]
        add     r3,r3,#1
        str     r3,[sp,#F_NJUMPS]
        b       db_marks

; RAM code: the block may not reach a page whose host memory does not follow
; that of its first page (the entry check reads it as consecutive words);
; the operands of jumps and of the absolute loads and stores are read when
; they run.
db_ram
        sub     r3,r1,#1
        mov     r3,r3,lsl #16
        mov     r3,r3,lsr #16           ; last byte of the instruction
        ldr     r6,[sp,#F_PC0]
        cmp     r3,r6
        blo     db_seam
        mvn     r6,r3,lsr #10
        and     r6,r6,#0x3F
        ldr     r6,[r9,r6,lsl #2]
        ldr     r5,[sp,#F_RTAB0]
        cmp     r6,r5
        bne     db_seam
        cmp     r2,#K_JPCC
        cmpne   r2,#K_CALLCC
        cmpne   r2,#K_JP
        cmpne   r2,#K_CALL
        beq     db_dyn
        ldrb    r3,[r0,#I_PRE - I_CORE]
        cmp     r3,#PRE_NONE
        bne     db_ei
        ldrb    r3,[r0,#I_OP - I_CORE]
        cmp     r3,#0x3A                ; LD A,(nn)
        cmpne   r3,#0x32                ; LD (nn),A
        cmpne   r3,#0x2A                ; LD HL,(nn)
        cmpne   r3,#0x22                ; LD (nn),HL
        bne     db_ei
db_dyn
        mov     r12,#1
        strb    r12,[r0,#I_DYN - I_CORE]
        orr     lr,lr,#DF_DYN
        str     lr,[sp,#F_FLAGS]
        b       db_ei

; The instruction crosses a seam: the block ends before it.
db_seam
        orr     lr,lr,#DF_SEAM
        str     lr,[sp,#F_FLAGS]
        sub     r10,r10,#1
        ldr     r1,[r0,#I_PC - I_CORE]
        b       db_end

; The previous instruction is EI: the interrupt check comes after this one,
; or right after EI when this one is a transfer or a conditional.
db_after_ei
        bic     lr,lr,#DF_EI
        str     lr,[sp,#F_FLAGS]
        cmp     r2,#K_NORMAL
        moveq   r5,#1
        movne   r3,#1
        strneb  r3,[r0,#I_IRQ_CHECK - I_CORE - I_SIZE]
        b       db_set

        END
