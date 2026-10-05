; Z80 instruction decoder of the translator (see z80jit.c, translate_block):
; the properties of an instruction (length, T-states, kind, flags read and
; written, operands) are written into its record (jit_insn, z80jit_int.h)
; with a few STM, and the instructions of a block are decoded in one loop
; that also fills the instruction map of the block and the fields the
; analysis passes start from.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_decode
        EXPORT  jit_decode_block
        IMPORT  jit_dec_tab

; jit_insn (z80jit_int.h; offsets checked in z80jit.c).
I_PC            EQU     0
I_LEN           EQU     4
I_PRE           EQU     8
I_OP            EQU     12
I_DECODED       EQU     56              ; end of the fourteen decoded words
I_IRQ_CHECK     EQU     64
I_SET           EQU     84              ; end of the seven words set per block
I_SIZE          EQU     104

; jit_dec_block (z80jit_int.h).
D_NEXT          EQU     20
D_FLAGS         EQU     24

; z80j_ctx, from its start (z80jit.h).
C_SLOT_BANK     EQU     0x87C
C_PAGE_KIND     EQU     0x900
PAGE_KIND       EQU     0x0F
KEY_RAM         EQU     0x80000

; Opcode properties (jit_dec_tab, built by main_info in z80jit.c).
MI_T            EQU     31
MI_KIND_SH      EQU     5
MI_USE_SH       EQU     9
MI_DEF_SH       EQU     15
MI_REL          EQU     0x00200000
MI_TN           EQU     0x00400000
MI_RST          EQU     0x00800000
MI_HL           EQU     0x01000000
MI_MEM          EQU     0x02000000
MI_MEM_SH       EQU     25
MI_EI_SH        EQU     26
MI_CC_SH        EQU     27
MI_NB_SH        EQU     30

PRE_NONE        EQU     0
PRE_CB          EQU     1
PRE_ED          EQU     2
PRE_DD          EQU     3
PRE_FD          EQU     4
PRE_DDCB        EQU     5

K_NORMAL        EQU     0
K_JPCC          EQU     2
K_CALLCC        EQU     3
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
F_SIZE          EQU     20

DF_RAM          EQU     1               ; block of RAM code
DF_EI           EQU     2               ; the previous instruction is EI
DF_DYN          EQU     4               ; some operand is read when it runs
DF_SEAM         EQU     8               ; the block ends at a seam

;----------------------------------------------------------------------------
; jit_dec_core: decodes the instruction at r1 (16 bits) into the record at r0.
; In: r0 = record, r1 = address, r2 = context, r11 = jit_dec_tab.
; Out: the fourteen decoded words of the record written, r0 = record + 56,
; r1 = address of the next instruction, r2 = kind, r7 = EI.
; r3-r8 and r12 are lost; r9-r11 and lr are kept.
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
        mvn     r3,r1,lsr #8
        and     r3,r3,#0xFF
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
        stmia   r0!,{r1-r7}             ; pc len pre op d n t
        add     r1,r1,r2
        bic     r1,r1,#0x10000          ; next address
        tst     r8,#MI_REL + MI_TN + MI_RST
        moveq   r4,#0                   ; no target
        bne     dc_target
dc_fields
        mov     r2,r8,lsr #MI_KIND_SH
        and     r2,r2,#15               ; kind
        mov     r3,r8,lsr #MI_CC_SH
        and     r3,r3,#7                ; condition
        mov     r5,r8,lsr #MI_USE_SH
        and     r5,r5,#FL_ALL           ; flags read
        mov     r6,r8,lsr #MI_DEF_SH
        and     r6,r6,#FL_ALL           ; flags written
        mov     r7,r8,lsr #MI_EI_SH
        and     r7,r7,#1                ; EI
        mov     r8,r8,lsr #MI_MEM_SH
        and     r8,r8,#1                ; (HL) operand
        stmia   r0!,{r2-r8}             ; kind cc target use def ei mem
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
        mov     r2,#1
        mov     r3,#0
        mov     r4,#0
        mov     r5,#0
        mov     r6,#0
        mov     r7,#4
        stmia   r0!,{r1-r7}
        add     r1,r1,#1
        bic     r1,r1,#0x10000
        mov     r2,#0
        mov     r3,#0
        mov     r4,#0
        mov     r5,#0
        mov     r6,#0
        mov     r7,#0
        mov     r8,#0
        stmia   r0!,{r2-r8}
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
        mov     r6,#0
        stmia   r0!,{r1-r7}
        add     r1,r1,r2
        bic     r1,r1,#0x10000
        mov     r2,#0                   ; kind
        mov     r3,#0                   ; condition
        mov     r5,#0                   ; flags read
        movs    r7,r4,lsr #6
        moveq   r6,#FL_ALL              ; rotations and shifts
        andeq   r4,r4,#0x30
        cmpeq   r4,#0x10
        moveq   r5,#FL_C                ; RL, RR
        cmp     r7,#1
        moveq   r6,#FL_INCDEC           ; BIT
        mov     r4,#0                   ; target
        mov     r7,#0                   ; EI
        stmia   r0!,{r2-r8}
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
; kind << 16 while r1-r7 hold the first decoded words.
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
dc_edn
        stmia   r0!,{r1-r7}
        mov     r12,r1
        add     r1,r1,r2
        bic     r1,r1,#0x10000
        mov     r2,r8,lsr #16           ; kind
        mov     r3,#0
        cmp     r2,#K_REP
        moveq   r4,r12                  ; repeated: the instruction itself
        movne   r4,#0
        mov     r5,r8,lsr #8
        and     r5,r5,#0xFF             ; flags read
        and     r6,r8,#0xFF             ; flags written
        mov     r7,#0
        mov     r8,#0
        stmia   r0!,{r2-r8}
        mov     pc,lr

; The instruction starts in the last three bytes of a page: each byte is
; read through the page holding it.
dc_fetch_pages
        mov     r12,#0
        mov     r5,r1
        mov     r6,#4
dc_fp_loop
        mvn     r3,r5,lsr #8
        and     r3,r3,#0xFF
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
; Decodes the instruction at pc (16 bits) into the first fourteen words of
; *in (pc to mem).
;----------------------------------------------------------------------------
jit_decode
        stmfd   sp!,{r4-r8,r11,lr}
        ldr     r11,dc_tab
        bl      jit_dec_core
        ldmfd   sp!,{r4-r8,r11,pc}

dc_tab  DCD     jit_dec_tab

;----------------------------------------------------------------------------
; int32 jit_decode_block(jit_dec_block *d)
;
; Decodes the instructions of a block from d->pc into d->ins, up to an
; unconditional transfer, a page of another kind than d->key or the size
; limit, as translate_block describes, and returns their number (0 when
; the first one crosses a seam of RAM). For each one: the instruction map
; entry of its address, d->serial | its number from 1 (jit_dec_tab, 256
; entries from 256, indexed by the low byte of the address: a block spans
; at most 256 bytes); its decoded words; internal = -1, busy, run,
; seg_start and fused = 0; irq_check set when it follows EI and is no
; transfer or conditional (the EI gets it otherwise); dyn set in RAM for
; JP / CALL and the absolute loads and stores of A and HL. d->next_pc
; receives the address after the last one and d->flags DF_DYN and
; DF_SEAM.
;----------------------------------------------------------------------------
jit_decode_block
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#F_SIZE
        ldmia   r0,{r4-r8}              ; ins, pc, ctx, key, serial
        mov     r9,r6                   ; context
        mov     r10,r8                  ; serial | number of the instruction
        ldr     r11,dc_tab
        mov     r1,r5,lsl #16
        mov     r1,r1,lsr #16           ; address
        mvn     r3,r1,lsr #8
        and     r3,r3,#0xFF
        ldr     r3,[r9,r3,lsl #2]       ; read table entry of the first page
        cmp     r7,#KEY_RAM
        moveq   r2,#DF_RAM
        movne   r2,#0
        stmia   sp,{r0-r3,r7}           ; args, pc, flags, entry, key
        mov     r0,r4
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
        mvn     r3,#0                   ; internal
        mov     r4,#0                   ; busy
        mov     r6,#0                   ; run
        mov     r7,#0                   ; seg_start
        mov     r8,#0                   ; fused
        stmia   r0!,{r3-r8,r12}
        add     r0,r0,#I_SIZE - I_SET
        cmp     r2,#K_JR                ; an unconditional transfer ends the block
        bhs     db_end
        and     r3,r10,#0xFF
        cmp     r3,#MAX_INSNS
        beq     db_end
        and     r3,r1,#0xFF             ; a page boundary was crossed when the
        cmp     r3,#4                   ; low byte of the next address is below
        bhs     db_loop                 ; the length of the instruction
        ldr     r4,[r0,#I_LEN - I_SIZE]
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
        str     r1,[r3,#D_NEXT]
        str     lr,[r3,#D_FLAGS]
        and     r0,r10,#0xFF
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r11,pc}

; RAM code: the block may not reach a page whose host memory does not follow
; that of its first page (the entry check reads it as consecutive words);
; the operands of jumps and of the absolute loads and stores are read when
; they run.
db_ram
        sub     r3,r1,#1
        mov     r3,r3,lsl #16
        mov     r3,r3,lsr #16           ; last byte of the instruction
        ldr     r4,[sp,#F_PC0]
        cmp     r3,r4
        blo     db_seam
        mvn     r4,r3,lsr #8
        and     r4,r4,#0xFF
        ldr     r4,[r9,r4,lsl #2]
        ldr     r5,[sp,#F_RTAB0]
        cmp     r4,r5
        bne     db_seam
        cmp     r2,#K_JPCC
        cmpne   r2,#K_CALLCC
        cmpne   r2,#K_JP
        cmpne   r2,#K_CALL
        beq     db_dyn
        ldr     r3,[r0,#I_PRE - I_DECODED]
        cmp     r3,#PRE_NONE
        bne     db_ei
        ldr     r3,[r0,#I_OP - I_DECODED]
        cmp     r3,#0x3A                ; LD A,(nn)
        cmpne   r3,#0x32                ; LD (nn),A
        cmpne   r3,#0x2A                ; LD HL,(nn)
        cmpne   r3,#0x22                ; LD (nn),HL
        bne     db_ei
db_dyn
        mov     r12,#1
        orr     lr,lr,#DF_DYN
        str     lr,[sp,#F_FLAGS]
        b       db_ei

; The instruction crosses a seam: the block ends before it.
db_seam
        orr     lr,lr,#DF_SEAM
        str     lr,[sp,#F_FLAGS]
        sub     r10,r10,#1
        ldr     r1,[r0,#I_PC - I_DECODED]
        b       db_end

; The previous instruction is EI: the interrupt check comes after this one,
; or right after EI when this one is a transfer or a conditional.
db_after_ei
        bic     lr,lr,#DF_EI
        str     lr,[sp,#F_FLAGS]
        cmp     r2,#K_NORMAL
        moveq   r5,#1
        movne   r3,#1
        strne   r3,[r0,#I_IRQ_CHECK - I_DECODED - I_SIZE]
        b       db_set

        END
