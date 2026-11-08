; Z80 instruction decoder of the translator (see z80jit.c, translate_block):
; the properties of an instruction (length, T-states, kind, flags read and
; written, operands) are packed into the six words of its record
; (jit_insn, z80jit_int.h: the fields of a byte share words) and written
; with one STM; the instructions of a block are decoded in one loop that
; also writes the marks the analysis passes start from (the segment
; starts among them), and resolves the jumps whose static target is an
; instruction of the block once the block is complete. In a block of
; fixed or paged code, the unprefixed instructions other than EI and OUT
; (n),A, the CB ones and most of those after DD / FD are decoded inline by
; the block loop from the two-word opcode records, the record of the
; instruction written with one STM of nine words; the others go through
; the full decoder.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_decode
        EXPORT  jit_scan_needed
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
B_CTX           EQU     60
B_DEC_FLAGS     EQU     64
B_BACK          EQU     92

; z80j_ctx, from its start (z80jit.h).
C_SLOT_BANK     EQU     0x57C
C_PAGE_KIND     EQU     0x600
PAGE_KIND       EQU     0x0F
KEY_RAM         EQU     0x80000

; Opcode records (jit_dec_tab, built by dec_entry in z80jit.c): two words
; per opcode, len ctl op t and kind cc use def, the control bits in the
; prefix byte (DC_* of z80jit.c).
C_MEM           EQU     0x00010000      ; (HL) operand: the mem byte of the record
C_HL            EQU     0x00020000      ; uses H or L
C_TGT           EQU     0x00180000      ; the target: next address +
C_OPND          EQU     0x00100000      ; displacement (0x80000), the operand,
                                        ; the RST vector (0x180000)
C_PREFIX        EQU     0x00200000      ; CB, DD, ED, FD
C_BRANCH        EQU     0x00400000      ; an unprefixed transfer or conditional
C_SLOW          EQU     0x00800000      ; a prefix, EI or OUT (n),A
C_ROM           EQU     0x00C00000      ; not a plain instruction
C_SLOW_RAM      EQU     0x03800000      ; any opcode (its length is 1 to 3)
C_RAM           EQU     0x01000000      ; in C_SLOW_RAM, not in C_ROM

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
K_RST           EQU     10
K_RET           EQU     11
K_RETN          EQU     12

FL_Z            EQU     0x02
FL_H            EQU     0x04
FL_PV           EQU     0x08
FL_N            EQU     0x10
FL_C            EQU     0x20
FL_ALL          EQU     0x3F
FL_INCDEC       EQU     0x1F

MAX_INSNS       EQU     64
SEG_MAX_T       EQU     200             ; a segment always fits in a fresh scanline
SCAN_INSNS      EQU     8               ; JIT_SCAN_INSNS
SCAN_DEPTH      EQU     1               ; JIT_SCAN_DEPTH

; Marks word of a record (internal busy irq_check seg_start).
M_NONE          EQU     0xFF000000      ; internal -1, nothing else
M_IRQ           EQU     0x00000100      ; irq_check
M_SEG           EQU     0x00000001      ; seg_start: block start, after a conditional
                                        ; instruction, jump target
M_CUT           EQU     0x00000002      ; seg_start: cut for the T-states

; Stack frame of jit_decode_block.
F_ARGS          EQU     0
F_PC0           EQU     4               ; address of the block
F_FLAGS         EQU     8               ; DF_* | outputs << NOUT_SHIFT
F_RTAB0         EQU     12              ; read table entry of its first page
F_KEY           EQU     16              ; its key
F_CTX           EQU     20              ; the context
F_MASK          EQU     24              ; control bits sending an opcode off the plain path
F_MARKS         EQU     28              ; marks of the instruction (full decoder)
F_NJUMPS        EQU     32              ; jumps with a static target met
F_LASTCUT       EQU     36              ; number of the last instruction given a
                                        ; segment start for its length (0: none)
F_JUMPS         EQU     40              ; their list: target | index << 16 | kind << 24
F_SIZE          EQU     F_JUMPS + 4 * MAX_INSNS

DF_RAM          EQU     1               ; block of RAM code
DF_DYN          EQU     4               ; some operand is read when it runs
DF_SEAM         EQU     8               ; the block ends at a seam
DF_EDIO         EQU     0x10            ; INI, IND, OUTI, OUTD, OUT (C),A / D / E
DF_RECUT        EQU     0x20            ; the segments are to be cut again
NOUT_SHIFT      EQU     16              ; OUTI, OUTD and OUT (n),A met

; The segment of the instruction being decoded (r8 its marks, $len its len
; pre op t word, r10 the number of the instruction | the T-states of the
; segment before it << 8): when the T-states would exceed SEG_MAX_T, the
; instruction starts a new segment (seg_start M_CUT in r8, its number
; kept in F_LASTCUT). Segments also start (M_SEG) at the block start and
; after a conditional instruction, where r10 is cleared of the T-states,
; and at the jump targets inside the block, set once the block is
; complete (db_resolve). Uses $t.
        MACRO
        SEGCUT  $t,$len
        and     $t,$len,#0xFF           ; T-states
        add     r10,r10,$t,lsl #8
        cmp     r10,#(SEG_MAX_T + 1) :SHL: 8
        bichs   r10,r10,#0xFF00
        addhs   r10,r10,$t,lsl #8
        orrhs   r8,r8,#M_CUT
        strhs   r10,[sp,#F_LASTCUT]
        MEND

;----------------------------------------------------------------------------
; jit_dec_core: decodes the instruction at r1 (16 bits) into the record at r0.
; In: r0 = record, r1 = address, r2 = context, r11 = jit_dec_tab.
; Out: the six decoded words of the record written (pc; target; ei mem d
; dyn; n; len pre op t; kind cc use def), r0 = record + 24, r1 = address
; of the next instruction, r2 = kind, r4 = target, r7 = EI, r12 = the len
; pre op t word. r3, r5, r6 and r8 are lost; r9-r11 and lr are kept.
;
; The four bytes at the address are read as one word, the first byte in
; bits 24-31 (two aligned words combined; a page starts on a word boundary
; of the host memory, so they stay in the page). Unprefixed opcodes and
; those after DD / FD are decoded from their records in jit_dec_tab; the
; immediate operand bytes are then in bits 16-23 and 8-15 of r12.
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
        add     r8,r11,r4,lsl #3
        ldmia   r8,{r7,r8}              ; len ctl op t; kind cc use def
        tst     r7,#C_PREFIX
        bne     dc_prefix
        and     r5,r7,#C_MEM            ; ei mem d dyn: the (HL) operand
        teq     r4,#0xFB
        orreq   r5,r5,#0x01000000       ; EI
        mov     r2,#PRE_NONE
        mov     r6,r7,lsr #24           ; 1 + the operand bytes

; r1 = address, r2 = prefix << 16, r4 = opcode, r5 = ei mem d dyn, r6 = 1 +
; the immediate operand bytes, r7 = len ctl op t of the instruction (its
; length and T-states, the control bits of the opcode), r8 = kind cc use
; def, r12 = the immediate operand bytes in bits 16-23 and 8-15.
dc_main
        cmp     r6,#2
        movlo   r6,#0                   ; no operand
        andhs   r6,r12,#0xFF0000
        movhs   r6,r6,lsr #16
        andhi   r12,r12,#0xFF00
        orrhi   r6,r6,r12               ; operand
        mov     r3,r1                   ; pc
        add     r1,r1,r7,lsr #24
        bic     r1,r1,#0x10000          ; next address
        ands    r12,r7,#C_TGT
        bic     r7,r7,#0xFF0000
        orr     r7,r7,r2                ; len pre op t
        bne     dc_target
        mov     r4,#0                   ; no target
; r3 = pc, r4 = target, r5 = ei mem d dyn, r6 = operand, r7 = len pre op t,
; r8 = kind cc use def.
dc_store
        stmia   r0!,{r3-r8}
        mov     r12,r7                  ; len pre op t
        mov     r2,r8,lsr #24           ; kind
        mov     r7,r5,lsr #24           ; EI
        mov     pc,lr

dc_target
        cmp     r12,#C_OPND
        moveq   r4,r6                   ; JP, CALL: the operand
        andhi   r4,r4,#0x38             ; RST: its vector
        bhs     dc_store
        mov     r4,r6,lsl #24
        add     r4,r1,r4,asr #24        ; JR, DJNZ: next address + displacement
        mov     r4,r4,lsl #16
        mov     r4,r4,lsr #16
        b       dc_store

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
        add     r8,r11,r4,lsl #3
        ldmia   r8,{r7,r8}
        tst     r7,#C_PREFIX
        bne     dc_nop                  ; another prefix: this one is a no-op
        tst     r7,#C_MEM
        bne     dc_xymem
        tst     r7,#C_HL
        beq     dc_nop                  ; no H, L or (HL): the prefix alone
        mov     r6,r7,lsr #24
        add     r7,r7,#0x01000000       ; the prefix byte
        add     r7,r7,#4                ; IX / IY for HL, H or L
        mov     r2,r3,lsl #16
        mov     r5,#0                   ; no EI, no memory operand, no displacement
        b       dc_main
dc_xymem
        and     r5,r12,#0xFF0000
        mov     r5,r5,lsr #8            ; displacement
        orr     r5,r5,#C_MEM            ; and the memory operand
        mov     r12,r12,lsl #8          ; the bytes after it
        mov     r6,r7,lsr #24
        cmp     r4,#0x36
        biceq   r7,r7,#0xFF
        orreq   r7,r7,#19               ; LD (IX+d),n
        addne   r7,r7,#12
        add     r7,r7,#0x02000000       ; the prefix and displacement bytes
        mov     r2,r3,lsl #16
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
        mov     r12,r7                  ; len pre op t
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
        mov     r12,r7                  ; len pre op t
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
        mov     r12,r7                  ; len pre op t
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
; uint32 jit_scan_needed(const jit_block_ctx *b, uint32 pc)
;
; The flags the code at pc (16 bits) may read before writing them, for the
; block b (see jit_flags_needed): up to SCAN_INSNS instructions are walked
; from pc, through the unconditional transfers with a static target and,
; SCAN_DEPTH deep, both arms of the conditional ones. A page whose content
; may change while b is valid (neither fixed nor a slot showing the bank
; of b's key), a transfer whose target is read when it runs, a conditional
; one past the depth and the end of the walk count as reading every flag
; not written before. The unprefixed opcodes are decoded from their
; records, the others (and those in the last three bytes of a 1 KiB page)
; by jit_dec_core.
;----------------------------------------------------------------------------
jit_scan_needed
        stmfd   sp!,{r4-r11,lr}
        ldr     r9,[r0,#B_KEY]
        ldr     r10,[r0,#B_CTX]
        ldr     r11,dc_tab
        mov     r0,r1,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#SCAN_DEPTH
        bl      sn_scan
        ldmfd   sp!,{r4-r11,pc}

; In: r0 = address, r1 = conditional jumps still followed; r9 = key of
; the block, r10 = context, r11 = jit_dec_tab. Out: r0 = the flags.
; r0-r3, r12 and lr are lost.
; r4 = flags written so far, r5 = flags read before being written, r6 =
; page (address >> 8) found safe, r7 = instructions left | depth << 8,
; r8 = address.
sn_scan
        stmfd   sp!,{r4-r8,lr}
        mov     r8,r0
        mov     r7,#SCAN_INSNS
        orr     r7,r7,r1,lsl #8
        mov     r4,#0
        mov     r5,#0
        mvn     r6,#0                   ; no page found safe yet
sn_loop
        cmp     r6,r8,lsr #8
        bne     sn_page
sn_safe
        mov     r3,r8,lsl #22
        cmp     r3,#0xFF000000          ; in the last three bytes of a 1 KiB page
        bhi     sn_core
        mvn     r3,r8,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r10,r3,lsl #2]      ; read table entry of the page
        add     r12,r3,r8               ; host address of the instruction
        ldrb    r0,[r12]                ; opcode
        add     r1,r11,r0,lsl #3
        ldmia   r1,{r1,r2}              ; len ctl op t; kind cc use def
        tst     r1,#C_PREFIX
        bne     sn_core
        mov     r3,r2,lsr #8
        and     r3,r3,#0xFF             ; read
        bic     r3,r3,r4
        orr     r5,r5,r3                ; before being written
        and     r3,r2,#0xFF
        orr     r4,r4,r3                ; written
        orr     r3,r4,r5
        cmp     r3,#FL_ALL
        beq     sn_known
        movs    r2,r2,lsr #24           ; kind
        beq     sn_next
        ands    r3,r1,#C_TGT            ; the static target
        beq     sn_all                  ; none: RET, RET cc, JP (HL), HALT
        cmp     r3,#C_OPND
        andhi   r12,r0,#0x38            ; RST: its vector
        bhi     sn_kind
        ldrb    r3,[r12,#1]
        ldreqb  lr,[r12,#2]
        orreq   r12,r3,lr,lsl #8        ; JP, CALL: the operand
        beq     sn_kind
        add     lr,r8,r1,lsr #24        ; JR, DJNZ: next address + displacement
        mov     r3,r3,lsl #24
        add     r12,lr,r3,asr #24
        mov     r12,r12,lsl #16
        mov     r12,r12,lsr #16

; A transfer or conditional: r1 = its len word, r2 = kind, r12 = target.
sn_kind
        sub     r3,r2,#K_JR
        cmp     r3,#K_RST - K_JR
        movls   r8,r12                  ; JR, JP, CALL, RST: followed
        bls     sn_count
        cmp     r2,#K_RETCC
        cmpne   r2,#K_RET
        bhs     sn_all                  ; RET cc, and RET to HALT
        tst     r7,#0xFF00
        beq     sn_all                  ; past the depth
        str     r1,[sp,#-4]!
        mov     r0,r12
        mov     r1,#0
        bl      sn_scan                 ; the taken arm
        ldr     r1,[sp],#4
        bic     r0,r0,r4
        orr     r5,r5,r0
sn_next
        add     r8,r8,r1,lsr #24
        bic     r8,r8,#0x10000          ; next address
sn_count
        sub     r7,r7,#1
        tst     r7,#0xFF
        bne     sn_loop
sn_all
        mvn     r0,r4
        and     r0,r0,#FL_ALL
        orr     r0,r0,r5                ; every flag not written counts as read
        ldmfd   sp!,{r4-r8,pc}
sn_known
        mov     r0,r5
        ldmfd   sp!,{r4-r8,pc}

; A new page: fixed, or a slot showing the bank of the block's key.
sn_page
        mov     r3,r8,lsr #8
        add     r0,r10,r3
        ldrb    r0,[r0,#C_PAGE_KIND]
        ands    r0,r0,#PAGE_KIND
        beq     sn_page_safe            ; fixed
        sub     r1,r0,#1
        cmp     r1,#3
        bhi     sn_all
        add     r1,r10,r1,lsl #2
        ldr     r1,[r1,#C_SLOT_BANK]
        mov     r1,r1,lsl #16
        orr     r0,r0,r1
        mov     r0,r0,ror #16           ; kind << 16 | bank
        cmp     r0,r9
        bne     sn_all
sn_page_safe
        mov     r6,r3
        b       sn_safe

; A prefixed instruction, or one that may cross a 1 KiB page: the full
; decoder, with the walk's registers saved around it.
sn_core
        stmfd   sp!,{r4-r8}
        sub     sp,sp,#I_CORE
        mov     r0,sp
        mov     r1,r8
        mov     r2,r10
        bl      jit_dec_core
        mov     r1,r12                  ; len pre op t
        mov     r12,r4                  ; target
        ldr     r3,[sp,#I_KIND]         ; kind cc use def
        add     sp,sp,#I_CORE
        ldmfd   sp!,{r4-r8}
        mov     r0,r3,lsr #8
        and     r0,r0,#0xFF             ; read
        bic     r0,r0,r4
        orr     r5,r5,r0                ; before being written
        and     r0,r3,#0xFF
        orr     r4,r4,r0                ; written
        orr     r0,r4,r5
        cmp     r0,#FL_ALL
        beq     sn_known
        cmp     r2,#K_NORMAL
        beq     sn_next
        b       sn_kind

;----------------------------------------------------------------------------
; int32 jit_decode_block(jit_block_ctx *b)
;
; Decodes the instructions of a block from b->pc into b->ins, up to an
; unconditional transfer, a page of another kind than b->key or the size
; limit, as translate_block describes, and returns their number (0 when
; the first one crosses a seam of RAM). For each one: its decoded words;
; the marks: busy, run and fused 0, seg_start set for the first
; instruction, for those after a conditional one (K_JRCC to K_REP) and
; for those that would take their segment past SEG_MAX_T T-states (the
; segments as the analysis cuts them, but for the jump targets: see
; below), irq_check set when it follows EI and is no transfer or
; conditional (the EI gets it otherwise), dyn set in RAM for JP / CALL
; and the absolute loads and stores of A and HL, internal the index of
; the instruction its static target starts, or -1.
;
; The jumps with a static target (JR, JP, JR cc, JP cc, DJNZ, a repeated
; block instruction) are listed as they are met and resolved once the
; block is complete: a target inside the block that starts an
; instruction of the block gets seg_start; a JR, JP, JR cc or JP cc to an
; instruction at or before it gets its bit in b->back, for the busy-wait
; loop analysis. The jumps whose target is outside the block, inside an
; instruction, or read when they run keep -1. A target that did not
; start a segment, before a segment cut for its length, sets DF_RECUT:
; the cuts after it are those of a segment from the block start or a
; conditional instruction, and the analysis makes them again from the
; target.
;
; b->n receives the count, b->next_pc the address after the last
; instruction, b->dec_flags DF_DYN, DF_SEAM, DF_EDIO, DF_RECUT and the
; outputs met (JIT_DEC_*), b->has_dyn whether DF_DYN is set.
;
; The instructions of a block of fixed or paged code not in the last
; three bytes of a 1 KiB page are decoded inline when they are unprefixed
; (an opcode record without C_SLOW; not a transfer right after EI), CB, or
; DD / FD on (IX+d) or IX / IY (db_prefix): the opcode and its operand
; bytes read through the read table entry of the page (r9), the record
; written from the opcode record with one STM of its nine words, a plain
; instruction (no C_BRANCH) with nothing more. The others, and every
; instruction of a RAM block, go through jit_dec_core and the marks
; below.
;----------------------------------------------------------------------------
jit_decode_block
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#F_SIZE
        ldr     r4,[r0,#B_INS]
        ldr     r7,[r0,#B_KEY]
        add     r1,r0,#B_PC
        ldmia   r1,{r5,r8}              ; pc, ctx
        mov     r10,#0                  ; number, segment T-states
        ldr     r11,dc_tab
        mov     r1,r5,lsl #16
        mov     r1,r1,lsr #16           ; address
        mvn     r3,r1,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r8,r3,lsl #2]       ; read table entry of the first page
        mov     r9,r3
        cmp     r7,#KEY_RAM
        moveq   r2,#DF_RAM
        movne   r2,#0
        stmia   sp,{r0-r3,r7,r8}        ; args, pc, flags, entry, key, ctx
        moveq   r2,#C_SLOW_RAM          ; RAM: every instruction through the core
        movne   r2,#C_ROM
        str     r2,[sp,#F_MASK]
        mov     r3,#0
        str     r3,[sp,#F_NJUMPS]       ; no jump yet
        str     r3,[sp,#F_LASTCUT]      ; no segment cut yet
        mov     r0,r4
        mov     r8,#M_NONE
        orr     r8,r8,#M_SEG            ; the first instruction starts a segment
        mov     lr,#0

; r0 = record, r1 = address, r2 = the control bits that send an opcode off
; the plain path, r8 = the marks of the instruction, r9 = read table
; entry of the 1 KiB page of the address, r10 = number of the instruction
; | the T-states of its segment before it << 8, r11 = jit_dec_tab, lr = 0.
db_loop
        add     r10,r10,#1
        mov     r3,r1,lsl #22
        cmp     r3,#0xFF000000          ; in the last three bytes of a 1 KiB page
        bhi     db_slow
        ldrb    r4,[r9,r1]              ; opcode
        add     r3,r11,r4,lsl #3
        ldmia   r3,{r6,r7}              ; len ctl op t; kind cc use def
        tst     r6,r2
        bne     db_other                ; not a plain instruction
        add     r12,r9,r1
        mov     r5,r6,lsr #24           ; 1 + the operand bytes
        cmp     r5,#2
        movlo   r5,#0
        ldrhsb  r5,[r12,#1]
        ldrhib  r12,[r12,#2]
        orrhi   r5,r5,r12,lsl #8        ; operand
        SEGCUT  r3,r6
        mov     r3,#0                   ; no target
        and     r4,r6,#C_MEM            ; ei mem d dyn
        bic     r6,r6,#0xFF0000         ; no prefix
        mov     r12,#0                  ; run (lr: fused)
        stmia   r0!,{r1,r3-r8,r12,lr}
        add     r0,r0,#I_SIZE - I_MARKED
        add     r1,r1,r6,lsr #24
        bic     r1,r1,#0x10000          ; next address
        mov     r8,#M_NONE              ; the marks of the next one

; The end of the block: the size limit, or a page of another key.
db_next
        tst     r10,#MAX_INSNS          ; (a power of two the count never passes)
        bne     db_end
        tst     r1,#0xFC                ; a page boundary was crossed when the
        bne     db_loop                 ; low byte of the next address is below
        and     r3,r1,#0xFF             ; the length of the instruction
        ldrb    r4,[r0,#I_LEN - I_SIZE]
        cmp     r3,r4
        bhs     db_loop
        ldr     r5,[sp,#F_CTX]
        add     r3,r5,r1,lsr #8         ; key of the new page (jit_block_key)
        ldrb    r3,[r3,#C_PAGE_KIND]
        and     r3,r3,#PAGE_KIND
        sub     r4,r3,#1
        cmp     r4,#3
        bls     db_slot
        cmp     r3,#0
        movne   r3,#KEY_RAM
        b       db_key
db_slot
        add     r4,r5,r4,lsl #2
        ldr     r4,[r4,#C_SLOT_BANK]
        mov     r4,r4,lsl #16
        orr     r3,r3,r4
        mov     r3,r3,ror #16           ; kind << 16 | bank
db_key
        ldr     r4,[sp,#F_KEY]
        cmp     r3,r4
        bne     db_end
        mvn     r3,r1,lsr #10
        and     r3,r3,#0x3F
        ldr     r9,[r5,r3,lsl #2]       ; read table entry of the new page
        b       db_loop

; Not a plain instruction. In a block of fixed or paged code, an
; unprefixed transfer or conditional not right after EI is decoded inline
; too: its target from the record's C_TGT bits, the next instruction
; starting a segment after a conditional one, the static targets listed
; for db_resolve, the block ended after an unconditional transfer. So are
; the CB opcodes and the DD / FD ones on (IX+d) or on IX / IY for HL, H or
; L (db_prefix). r4 = opcode, r6 and r7 = its record.
db_other
        tst     r2,#C_RAM
        bne     db_slow                 ; RAM: every instruction through the core
        tst     r6,#C_SLOW
        bne     db_prefix
        tst     r8,#M_IRQ
        bne     db_slow                 ; a transfer right after EI
        add     r12,r9,r1
        mov     r5,r6,lsr #24           ; 1 + the operand bytes
        cmp     r5,#2
        movlo   r5,#0
        ldrhsb  r5,[r12,#1]
        ldrhib  r12,[r12,#2]
        orrhi   r5,r5,r12,lsl #8        ; operand
        ands    r12,r6,#C_TGT
        moveq   r3,#0                   ; no target: RET, JP (HL), HALT
        beq     db_br_store
        cmp     r12,#C_OPND
        moveq   r3,r5                   ; JP, CALL: the operand
        andhi   r3,r4,#0x38             ; RST: its vector
        bhs     db_br_store
        add     r3,r1,r6,lsr #24        ; JR, DJNZ: next address + displacement
        mov     r12,r5,lsl #24
        add     r3,r3,r12,asr #24
        mov     r3,r3,lsl #16
        mov     r3,r3,lsr #16
db_br_store
        SEGCUT  r12,r6
        and     r4,r6,#C_MEM            ; ei mem d dyn
        bic     r6,r6,#0xFF0000         ; no prefix
        mov     r12,#0                  ; run (lr: fused)
        stmia   r0!,{r1,r3-r8,r12,lr}
        add     r0,r0,#I_SIZE - I_MARKED
        add     r1,r1,r6,lsr #24
        bic     r1,r1,#0x10000          ; next address
        mov     r8,#M_NONE              ; the marks of the next one
        mov     r12,r7,lsr #24          ; kind
        sub     r4,r12,#K_JRCC
        cmp     r4,#K_REP - K_JRCC
        orrls   r8,r8,#M_SEG            ; a conditional one: the next starts a segment
        bicls   r10,r10,#0xFF00
        cmp     r4,#K_JP - K_JRCC       ; JR cc to JP: a static target
        bhi     db_br_next
        cmp     r12,#K_CALLCC           ; never inside the block
        cmpne   r12,#K_RETCC
        beq     db_br_next
        ldr     r4,[sp,#F_NJUMPS]
        and     lr,r10,#0xFF
        sub     lr,lr,#1                ; index of this instruction
        orr     lr,r3,lr,lsl #16
        orr     lr,lr,r12,lsl #24
        add     r5,sp,#F_JUMPS
        str     lr,[r5,r4,lsl #2]
        add     r4,r4,#1
        str     r4,[sp,#F_NJUMPS]
        mov     lr,#0
db_br_next
        cmp     r12,#K_JR               ; an unconditional transfer ends the block
        bhs     db_end
        b       db_next

; A prefix, EI or OUT (n),A: CB, and DD / FD followed by an opcode whose
; record has C_MEM or C_HL and is neither a prefix nor a transfer, are
; decoded as jit_dec_core does (dc_cb, dc_prefix); the others go to it.
; r3 = the opcode after the prefix, r12 = host address of the instruction.
db_prefix
        add     r12,r9,r1
        ldrb    r3,[r12,#1]             ; the byte after the prefix
        cmp     r4,#0xCB
        beq     db_cb
        cmp     r4,#0xDD
        cmpne   r4,#0xFD
        bne     db_slow
        add     r5,r11,r3,lsl #3
        ldmia   r5,{r6,r7}              ; record of the opcode after DD / FD
        tst     r6,#C_PREFIX + C_BRANCH
        bne     db_slow                 ; DD CB, another prefix, JP (IX)
        cmp     r4,#0xDD
        moveq   r4,#PRE_DD :SHL: 16
        movne   r4,#PRE_FD :SHL: 16
        tst     r6,#C_MEM
        bne     db_xymem
        tst     r6,#C_HL
        beq     db_slow                 ; the prefix alone, a no-op
        bic     r6,r6,#0xFF0000
        orr     r6,r6,r4                ; the prefix
        mov     r5,r6,lsr #24           ; 1 + the operand bytes
        cmp     r5,#2
        movlo   r5,#0
        ldrhsb  r5,[r12,#2]
        ldrhib  lr,[r12,#3]
        orrhi   r5,r5,lr,lsl #8         ; operand
        add     r6,r6,#0x01000000       ; the prefix byte
        add     r6,r6,#4                ; and its 4 T-states: IX / IY for HL, H or L
        mov     r4,#0                   ; no EI, memory operand or displacement
        b       db_xyset
db_xymem
        bic     r6,r6,#0xFF0000
        orr     r6,r6,r4                ; the prefix
        ldrb    r4,[r12,#2]             ; displacement
        mov     r4,r4,lsl #8
        orr     r4,r4,#C_MEM            ; and the memory operand
        mov     r5,r6,lsr #24
        cmp     r5,#2
        movlo   r5,#0
        ldrhsb  r5,[r12,#3]             ; operand: LD (IX+d),n
        cmp     r3,#0x36
        biceq   r6,r6,#0xFF
        orreq   r6,r6,#19               ; LD (IX+d),n
        addne   r6,r6,#12
        add     r6,r6,#0x02000000       ; the prefix and displacement bytes
; r4 = ei mem d dyn, r5 = operand, r6 = len pre op t, r7 = kind cc use
; def of a plain prefixed instruction.
db_xyset
        SEGCUT  r3,r6
        mov     r3,#0                   ; no target
        mov     r12,#0                  ; run
        mov     lr,#0                   ; fused
        stmia   r0!,{r1,r3-r8,r12,lr}
        add     r0,r0,#I_SIZE - I_MARKED
        add     r1,r1,r6,lsr #24
        bic     r1,r1,#0x10000          ; next address
        mov     r8,#M_NONE              ; the marks of the next one
        b       db_next

; CB: the opcode gives the T-states, the (HL) operand and the flags.
db_cb
        mov     r7,r3,lsr #6            ; x
        and     r5,r3,#7
        cmp     r5,#6                   ; (HL)
        movne   r4,#0
        movne   r6,#8
        moveq   r4,#C_MEM
        moveq   r6,#15
        cmpeq   r7,#1
        moveq   r6,#12                  ; BIT n,(HL)
        orr     r6,r6,r3,lsl #8
        orr     r6,r6,#PRE_CB :SHL: 16
        orr     r6,r6,#0x02000000       ; len pre op t
        cmp     r7,#1
        movhi   r7,#0                   ; RES, SET: no flags
        moveq   r7,#FL_INCDEC           ; BIT
        bhs     db_cbset
        mov     r7,#FL_ALL              ; rotations and shifts write every flag
        and     r5,r3,#0x30
        cmp     r5,#0x10
        orreq   r7,r7,#FL_C :SHL: 8     ; RL, RR read C
db_cbset
        mov     r5,#0                   ; no operand
        b       db_xyset

; The full decoder: r2 = kind, r4 = target, r7 = EI, r12 = len pre op t of
; the instruction once decoded, r6 its dyn, lr the flags, r8 its marks,
; r5 the marks of the next one.
db_slow
        str     r8,[sp,#F_MARKS]
        ldr     r2,[sp,#F_CTX]
        bl      jit_dec_core
        ldr     r8,[sp,#F_MARKS]
        ldr     lr,[sp,#F_FLAGS]
        ; The port inputs and outputs the analysis looks at.
        and     r3,r12,#0xFF0000        ; prefix
        cmp     r3,#PRE_ED :SHL: 16
        beq     db_edio
        cmp     r3,#PRE_NONE
        bne     db_counted
        and     r3,r12,#0xFF00
        cmp     r3,#0xD300              ; OUT (n),A
        addeq   lr,lr,#1 :SHL: NOUT_SHIFT
        streq   lr,[sp,#F_FLAGS]
db_counted
        mov     r6,#0                   ; dyn
        tst     lr,#DF_RAM
        bne     db_ram
db_ei
        tst     r8,#M_IRQ               ; after EI
        bne     db_after_ei
db_set
        SEGCUT  r3,r12
        mov     r5,#M_NONE
        teq     r7,#0
        orrne   r5,r5,#M_IRQ            ; EI: the next one checks for an interrupt
        sub     r3,r2,#K_JRCC
        cmp     r3,#K_REP - K_JRCC
        orrls   r5,r5,#M_SEG            ; a conditional one: the next starts a segment
        bicls   r10,r10,#0xFF00
        cmp     r3,#K_JP - K_JRCC       ; JR cc to JP: a static target
        bls     db_jump
db_marks
        mov     r3,r8
        mov     r6,#0                   ; run
        mov     r7,#0                   ; fused
        stmia   r0!,{r3,r6,r7}
        add     r0,r0,#I_SIZE - I_MARKED
        mov     r8,r5
        cmp     r2,#K_JR                ; an unconditional transfer ends the block
        bhs     db_end
        ldr     r2,[sp,#F_MASK]
        mov     lr,#0
        b       db_next

; ED: INI, IND, OUTI, OUTD and OUT (C),A / D / E make runs and loops; OUTI
; and OUTD count as outputs for the groups.
db_edio
        and     r3,r12,#0xFF00          ; opcode
        cmp     r3,#0xA300
        cmpne   r3,#0xAB00
        addeq   lr,lr,#1 :SHL: NOUT_SHIFT
        beq     db_edio_set
        cmp     r3,#0xA200
        cmpne   r3,#0xAA00
        cmpne   r3,#0x7900
        cmpne   r3,#0x5100
        cmpne   r3,#0x5900
        bne     db_counted
db_edio_set
        orr     lr,lr,#DF_EDIO
        str     lr,[sp,#F_FLAGS]
        b       db_counted

db_end
        ldr     r3,[sp,#F_ARGS]
        ldr     lr,[sp,#F_FLAGS]
        str     r1,[r3,#B_NEXT_PC]
        and     r0,r10,#0xFF
        str     r0,[r3,#B_N]
        and     lr,lr,#DF_DYN
        mov     lr,lr,lsr #2
        str     lr,[r3,#B_HAS_DYN]

        ; The jumps listed: a target inside the block (its offset from the
        ; block start below the span) that starts an instruction of the
        ; block, found from the jump's own record forwards or backwards
        ; (the offsets of the records grow). The instruction excluded at a
        ; seam has the span as offset. Offsets are compared << 16.
        ; r0 = span, lr = block address, r4 = ins, r6 = list, r5 = its
        ; end, r7 = the jump, r8 and r9 = the jumps back, r11 = offset of
        ; the last instruction, r12 = offset of the target; r1 = index of
        ; the jump, r2 and r3 = record and index looked at, r10 = scratch.
        ldr     r4,[r3,#B_INS]
        ldr     lr,[sp,#F_PC0]
        sub     r0,r1,lr
        mov     r0,r0,lsl #16
        sub     r0,r0,#1                ; (offsets below the span)
        mov     r8,#0
        mov     r9,#0
        ldr     r5,[sp,#F_NJUMPS]
        add     r6,sp,#F_JUMPS
        add     r5,r6,r5,lsl #2
        cmp     r5,r6
        beq     db_resolved             ; no jump listed
        and     r11,r10,#0xFF
        sub     r11,r11,#1
        add     r2,r4,r11,lsl #4
        add     r2,r2,r11,lsl #5
        ldr     r11,[r2,#I_PC]
        sub     r11,r11,lr
        mov     r11,r11,lsl #16         ; offset of the last instruction
db_resolve
        cmp     r6,r5
        bhs     db_resolved
        ldr     r7,[r6],#4
        sub     r12,r7,lr
        mov     r12,r12,lsl #16         ; offset of the target
        cmp     r12,r0
        bhi     db_resolve              ; outside the block
        mov     r1,r7,lsr #16
        and     r1,r1,#0xFF             ; index of the jump
        add     r2,r4,r1,lsl #4
        add     r2,r2,r1,lsl #5         ; its record
        ldr     r3,[r2,#I_PC]
        sub     r3,r3,lr
        cmp     r12,r3,lsl #16
        mov     r3,r1
        beq     db_found                ; a jump to itself
        blo     db_back
        cmp     r12,r11
        bhi     db_resolve              ; inside the last instruction
db_fwd
        add     r3,r3,#1
        ldr     r10,[r2,#I_SIZE]!
        sub     r10,r10,lr
        mov     r10,r10,lsl #16
        cmp     r10,r12
        blo     db_fwd
        bne     db_resolve              ; inside an instruction
        b       db_found
db_back
        sub     r3,r3,#1
        ldr     r10,[r2,#-I_SIZE]!
        sub     r10,r10,lr
        mov     r10,r10,lsl #16
        cmp     r10,r12
        bhi     db_back
        bne     db_resolve              ; inside an instruction

        ; The target, instruction r3 at r2: the jump gets its index, and the
        ; target starts a segment (M_SEG); when it started none and the
        ; decoder cut a segment past it, the cuts may differ from those
        ; made from the target (DF_RECUT: the analysis cuts the segments
        ; again). A segment cut there for its T-states already made the
        ; cuts that follow from the target.
db_found
        add     r10,r4,r1,lsl #4
        add     r10,r10,r1,lsl #5
        strb    r3,[r10,#I_INTERNAL]
        ldrb    r10,[r2,#I_SEG_START]
        cmp     r10,#M_SEG
        beq     db_started
        cmp     r10,#0
        mov     r10,#M_SEG
        strb    r10,[r2,#I_SEG_START]   ; a jump target starts a segment
        bne     db_started
        ldr     r10,[sp,#F_LASTCUT]
        and     r10,r10,#0xFF
        sub     r10,r10,#1              ; index of the last cut, or -1
        cmp     r10,r3
        ldrgt   r10,[sp,#F_FLAGS]
        orrgt   r10,r10,#DF_RECUT
        strgt   r10,[sp,#F_FLAGS]
db_started
        cmp     r3,r1                   ; a jump back, or to itself
        bhi     db_resolve
        mov     r10,r7,lsr #24          ; kind
        cmp     r10,#K_JRCC
        cmpne   r10,#K_JPCC
        cmpne   r10,#K_JR
        cmpne   r10,#K_JP
        bne     db_resolve
        mov     r10,#1
        cmp     r1,#32
        orrlo   r8,r8,r10,lsl r1
        subhs   r1,r1,#32
        orrhs   r9,r9,r10,lsl r1
        b       db_resolve
db_resolved
        ldr     r3,[sp,#F_ARGS]
        ldr     lr,[sp,#F_FLAGS]
        str     lr,[r3,#B_DEC_FLAGS]
        ldr     r0,[r3,#B_N]            ; the count
        add     r3,r3,#B_BACK
        stmia   r3,{r8,r9}
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r11,pc}

; A jump with a static target (r4), listed with its index and kind
; unless the target is read when it runs.
db_jump
        cmp     r2,#K_CALLCC            ; never inside the block
        cmpne   r2,#K_RETCC
        beq     db_marks
        cmp     r6,#0
        bne     db_marks
        ldr     r3,[sp,#F_NJUMPS]
        and     r6,r10,#0xFF
        sub     r6,r6,#1                ; index of this instruction
        orr     r6,r4,r6,lsl #16
        orr     r6,r6,r2,lsl #24
        add     r7,sp,#F_JUMPS
        str     r6,[r7,r3,lsl #2]
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
        ldr     r5,[sp,#F_PC0]
        cmp     r3,r5
        blo     db_seam
        ldr     r5,[sp,#F_CTX]
        mvn     r3,r3,lsr #10
        and     r3,r3,#0x3F
        ldr     r3,[r5,r3,lsl #2]
        ldr     r5,[sp,#F_RTAB0]
        cmp     r3,r5
        bne     db_seam
        cmp     r2,#K_JPCC
        cmpne   r2,#K_CALLCC
        cmpne   r2,#K_JP
        cmpne   r2,#K_CALL
        beq     db_dyn
        tst     r12,#0xFF0000           ; prefix
        bne     db_ei
        and     r3,r12,#0xFF00          ; opcode
        cmp     r3,#0x3A00              ; LD A,(nn)
        cmpne   r3,#0x3200              ; LD (nn),A
        cmpne   r3,#0x2A00              ; LD HL,(nn)
        cmpne   r3,#0x2200              ; LD (nn),HL
        bne     db_ei
db_dyn
        mov     r6,#1
        strb    r6,[r0,#I_DYN - I_CORE]
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
        cmp     r2,#K_NORMAL
        bicne   r8,r8,#M_IRQ
        movne   r3,#1
        strneb  r3,[r0,#I_IRQ_CHECK - I_CORE - I_SIZE]
        b       db_set

        END
