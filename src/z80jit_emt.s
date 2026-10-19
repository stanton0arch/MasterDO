; ARM code generation of the Z80 translator (see z80jit.c, translate_block
; and z80jit_int.h): the code of a block is emitted from its decoded and
; analysed instruction records. The words of the generated code are kept
; as template instructions in this file (the assembler encodes their
; immediates), loaded with LDM, given their register fields and written
; with STM: a store costs two non-sequential cycles on the ARM60, a block
; store of n words only one more sequential cycle per word.
;
; Conventions of the generated code (z80jit_int.h): r3 F, r4 A, r5 BC,
; r6 DE, r7 HL, r8 T-states left, r9 IX, r10 global pointer, r11 SP,
; r12 memory address; r0-r2, r12 and lr scratch. The templates are
; written with those registers; the emitter itself keeps:
;   r4  block context (jit_block_ctx)      r5  cursor (next free word)
;   r6  record of the instruction          r7  its index
;   r8  routines of the glue (z80j_glue)   r9  opcode of the instruction
;   r11 EF_* flags of the instruction (live flags, fused test, index
;       register, memory operand)
; r10 is the stack limit of the C code and is never touched. Helpers use
; r0-r3, r12 and lr; those calling other helpers save lr on the stack and
; never address the stack frame of the main routine.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_emit_block
        IMPORT  jit_link_host
        IMPORT  jit_link_stub
        IMPORT  z80j_glue_out_split
        IMPORT  z80j_glue_pop_af

; jit_insn (z80jit_int.h; offsets checked in z80jit.c).
I_PC            EQU     0
I_LEN           EQU     4
I_PRE           EQU     8
I_OP            EQU     12
I_D             EQU     16
I_N             EQU     20
I_T             EQU     24
I_KIND          EQU     28
I_CC            EQU     32
I_TARGET        EQU     36
I_USE           EQU     40
I_DEF           EQU     44
I_EI            EQU     48
I_MEM           EQU     52
I_INTERNAL      EQU     56
I_BUSY          EQU     60
I_IRQ_CHECK     EQU     64
I_RUN           EQU     68
I_SEG_START     EQU     72
I_FUSED         EQU     76
I_DYN           EQU     80
I_LIVE_AFTER    EQU     84
I_LIVE_BEFORE   EQU     88
I_SEG_T         EQU     92
I_T_AFTER       EQU     96
I_HOST          EQU     100
I_SIZE          EQU     104

; jit_block_ctx.
B_J             EQU     0
B_INS           EQU     4
B_N             EQU     8
B_NEXT_PC       EQU     12
B_KEY           EQU     16
B_END_LEAVE     EQU     20
B_STUBS         EQU     24
B_NSTUBS        EQU     28
B_ENTRY_CHECK   EQU     32
B_HAS_DYN       EQU     36
B_MARK_OFF      EQU     40
B_BODY          EQU     44
B_SKIP_SITE     EQU     48
B_SKIP_TO       EQU     52

; jit_stub.
ST_KIND         EQU     0
ST_SITE         EQU     4
ST_PC           EQU     8
ST_T            EQU     12
ST_RESUME       EQU     16
ST_INDEX        EQU     20
ST_REG          EQU     24
ST_SIZE         EQU     28
MAX_STUBS       EQU     256

STUB_TIMEOUT    EQU     0
STUB_FORWARD    EQU     1
STUB_LINK       EQU     2
STUB_TAKEN      EQU     3
STUB_WRITE      EQU     4
STUB_LEAVE      EQU     5
STUB_DYN        EQU     6

; z80j_state: the glue comes first for the emitter (r8), the rest is
; addressed from it (offsets checked in z80jit.c).
S_CTX           EQU     0
S_CUR           EQU     12
S_ZONE_END      EQU     16
S_GLUE          EQU     0x4F84
GE_ERROR        EQU     0x1060          ; error - glue
GE_FULLF        EQU     0x38D8          ; stats.full_flags - glue

; z80j_glue.
G_LINK          EQU     0
G_MISS          EQU     4
G_SEG_TIMEOUT   EQU     8
G_LEAVE         EQU     12
G_HALT          EQU     16
G_IO_IN         EQU     24
G_IO_OUT        EQU     28
G_IO_OUTN       EQU     32
G_WRITE         EQU     36
G_VERIFY        EQU     40
G_DAA           EQU     44
G_PUSH_SLOW     EQU     48
G_LDIR          EQU     56
G_LDDR          EQU     60

; z80j_ctx, from the global pointer (&wtab[0]) or from its start.
C_WTAB          EQU     0x400
C_PORT_IN       EQU     0x89C
C_PORT_OUT      EQU     0x8A0
C_PORT_OUTN     EQU     0x8A4
C_PAGE_KIND     EQU     0x900
GOFF_IY         EQU     0x424
GOFF_BC2        EQU     0x430
GOFF_I          EQU     0x43C
GOFF_R          EQU     0x440
GOFF_IFF1       EQU     0x444
GOFF_IFF2       EQU     0x448
GOFF_IM         EQU     0x44C
GOFF_IRQ_LINE   EQU     0x454
GOFF_NMI        EQU     0x458
GOFF_LINE       EQU     0x460
GOFF_SLOT_BANK  EQU     0x47C
GOFF_SCRATCH    EQU     0x494
GOFF_IDLE       EQU     0x498
PZST_OFF        EQU     0x600
FENC_OFF        EQU     0x700
FDEC_OFF        EQU     0x800
TAB_OFF         EQU     0x4000
OBUF_OFF        EQU     0xD00           ; ctx->obuf
PAGE_RFIXED     EQU     0x40
PAGE_WFIXED     EQU     0x80

RUN_PART        EQU     0xFFFFFFFF
RUN_LOOP        EQU     0xFFFFFFFE
RUN_MIXED       EQU     0x40000000
RUN_GRP         EQU     0x80000000      ; an element of a group of outputs
RUN_GRP_LAST    EQU     0x40000000      ; (z80jit_int.h)
RUN_GRP_N       EQU     0x04000000
RUN_GRP_HASN    EQU     0x02000000
RUN_GRP_HASC    EQU     0x01000000
RUN_BUF_HI      EQU     0x20            ; Z80J_RUN_BUF >> 8
RUN_BUF_N_HI    EQU     0x40            ; Z80J_RUN_BUF_N >> 8
OBUF_N          EQU     0x10000         ; Z80J_OBUF_N

K_JRCC          EQU     1
K_CALLCC        EQU     3
K_REP           EQU     6
K_JR            EQU     7

PRE_CB          EQU     1
PRE_ED          EQU     2
PRE_DD          EQU     3
PRE_FD          EQU     4
PRE_DDCB        EQU     5
PRE_FDCB        EQU     6

; Flags of the instruction being emitted (r11).
EF_LIVE         EQU     0x01            ; some flag it writes is read later
EF_FUSED        EQU     0x02            ; its result stays in the ARM flags
EF_IDX          EQU     0x0C            ; index register of H, L: 4 IX, 8 IY
EF_IX           EQU     0x04
EF_IY           EQU     0x08
EF_MEM          EQU     0x10            ; memory operand
EF_IRQ          EQU     0x20            ; interrupt check after the instruction

; Z80 flags in the internal format of r3.
PSR_S           EQU     0x08
PSR_Z           EQU     0x04
PSR_C           EQU     0x02
PSR_V           EQU     0x01
PSR_P           EQU     0x01
PSR_n           EQU     0x80
PSR_H           EQU     0x10

; Flags valid in the ARM flags after a fused producer.
FU_Z_EQ         EQU     0x01
FU_Z_CS         EQU     0x02
FU_C_CS         EQU     0x04
FU_C_CC         EQU     0x08
FU_S_MI         EQU     0x10
FU_V_VS         EQU     0x20

; Registers of the generated code.
RF              EQU     3
RA              EQU     4
RBC             EQU     5
RDE             EQU     6
RHL             EQU     7
RCYC            EQU     8
RIX             EQU     9
RG              EQU     10
RSP             EQU     11
RAD             EQU     12
RLR             EQU     14

; Operand of an 8-bit operation (get8): the operand field of a data
; processing instruction (a register with its left shift, or an immediate
; with bit 25 set), with the raw immediate in bits 16-23.
O_IMM           EQU     0x02000000
O_FIELD         EQU     0x02000FFF

; Conditions and branch words.
CC_EQ           EQU     0x00000000
CC_NE           EQU     0x10000000
CC_CS           EQU     0x20000000
CC_CC           EQU     0x30000000
CC_MI           EQU     0x40000000
CC_PL           EQU     0x50000000
CC_VS           EQU     0x60000000
CC_VC           EQU     0x70000000
CC_AL           EQU     0xE0000000
B_AL            EQU     0xEA000000
BL_AL           EQU     0xEB000000
B_NE            EQU     0x1A000000
B_EQ            EQU     0x0A000000
B_MI            EQU     0x4A000000
B_CC            EQU     0x3A000000
BL_EQ           EQU     0x0B000000
BL_NE           EQU     0x1B000000

IMM_NONE        EQU     0xFFFFFFFF

RAM_INLINE_WORDS EQU    16              ; RAM blocks up to this size checked inline

; Stack frame of the main routine.
F_J             EQU     0
F_MISS          EQU     4               ; site of the bank check branch
F_SIZE          EQU     8

;----------------------------------------------------------------------------
; Macros
;----------------------------------------------------------------------------

; Writes the word in $w to the code.
        MACRO
        EMIT    $w
        str     $w,[r5],#4
        MEND

; Loads the template word at $t into $w.
        MACRO
        TMPL    $w,$t
        ldr     $w,$t
        MEND

; Loads the template word at $t into $w under the condition $c.
        MACRO
        TMPLC   $c,$w,$t
        ldr$c   $w,$t
        MEND

; Emits a branch to the address in $t with the condition and link bits
; of $top (a word of the form 0xEA000000); $w is a scratch register.
        MACRO
        EMITB   $w,$t,$top
        sub     $w,$t,r5
        sub     $w,$w,#8
        mov     $w,$w,lsl #6
        mov     $w,$w,lsr #8
        orr     $w,$w,#$top
        str     $w,[r5],#4
        MEND

; Retargets the branch at $site to the address in $to, keeping its
; condition and link bits; $to and $w are lost.
        MACRO
        PATCHB  $site,$to,$w
        ldr     $w,[$site]
        sub     $to,$to,$site
        sub     $to,$to,#8
        mov     $to,$to,lsl #6
        and     $w,$w,#0xFF000000
        orr     $w,$w,$to,lsr #8
        str     $w,[$site]
        MEND

; Records a translation error ($code) in the translator state.
        MACRO
        SETERR  $w,$v,$code
        add     $w,r8,#GE_ERROR :AND: 0xFF00
        mov     $v,#$code
        str     $v,[$w,#GE_ERROR :AND: 0xFF]
        MEND

; Counts a full flag computation.
        MACRO
        FULLF   $w,$v
        add     $w,r8,#GE_FULLF :AND: 0xFF00
        ldr     $v,[$w,#GE_FULLF :AND: 0xFF]
        add     $v,$v,#1
        str     $v,[$w,#GE_FULLF :AND: 0xFF]
        MEND

; $w = the translator state.
        MACRO
        GETJ    $w
        sub     $w,r8,#S_GLUE :AND: 0xFF00
        sub     $w,$w,#S_GLUE :AND: 0xFF
        MEND

; $w = the translator context.
        MACRO
        GETCTX  $w
        sub     $w,r8,#S_GLUE :AND: 0xFF00
        ldr     $w,[$w,#S_CTX - (S_GLUE :AND: 0xFF)]
        MEND

; Address of the record of instruction $i (a register) in $w, $b = ins.
        MACRO
        RECORD  $w,$b,$i
        add     $w,$i,$i,lsl #2
        add     $w,$w,$i,lsl #3
        add     $w,$b,$w,lsl #3
        MEND

; One byte of a word of the inline RAM entry check (eb_ram_inline): r2 =
; recorded word, r3 = its mask, r12 = register fields; emits BICS of the
; byte at bit $sh when the mask clears it, else EORS of its value unless
; it is zero ($rot: rotation field of the immediate). Uses r0, r1.
        MACRO
        RIBYTE  $sh,$rot
        ands    r1,r3,#0xFF:SHL:$sh
        TMPLC   eq,r0,t_ri_bics
        moveq   r1,#0xFF
        TMPLC   ne,r0,t_ri_eors
        andne   r1,r2,#0xFF:SHL:$sh
        movne   r1,r1,lsr #$sh
        teq     r1,#0
        orrne   r0,r0,r1
        orrne   r0,r0,#$rot:SHL:8
        orrne   r0,r0,r12
        strne   r0,[r5],#4
        MEND

;----------------------------------------------------------------------------
; void jit_emit_block(jit_block_ctx *b)
;
; Emits the code of the block described by b at b->j->cur: the entry check
; (bank of a paged slot, or the bytes of a RAM block), the run mark of the
; chunk, the instructions with their segment checks and interrupt checks,
; the fall-through exit, then the stubs the code branched to. Leaves
; b->j->cur past the code, and b->j->error nonzero when an instruction could
; not be encoded, a stub table overflowed or the zone is full.
;----------------------------------------------------------------------------
jit_emit_block
        stmfd   sp!,{r4-r9,r11,lr}
        sub     sp,sp,#F_SIZE
        mov     r4,r0
        ldr     r1,[r4,#B_J]
        str     r1,[sp,#F_J]
        ldr     r5,[r1,#S_CUR]
        add     r8,r1,#S_GLUE :AND: 0xFF00
        add     r8,r8,#S_GLUE :AND: 0xFF
        mov     r0,#0
        str     r0,[r4,#B_NSTUBS]
        str     r0,[sp,#F_MISS]
        str     r0,[r4,#B_SKIP_SITE]
        ldr     r0,[r4,#B_ENTRY_CHECK]
        cmp     r0,#1
        beq     eb_bank_check
        cmp     r0,#2
        beq     eb_ram_check
eb_body
        ; The run mark of the chunk holding the block: STR r10,[r10,#off].
        str     r5,[r4,#B_BODY]
        ldr     r0,[r4,#B_MARK_OFF]
        TMPL    r1,t_mark
        orr     r0,r0,r1
        EMIT    r0

        ldr     r6,[r4,#B_INS]
        mov     r7,#0
        ldr     r0,[r4,#B_N]
        str     r0,[r4,#B_SKIP_TO]      ; no skip pending: the loop runs to the end
eb_loop
        str     r5,[r6,#I_HOST]
        ; The EF_* flags of the instruction: its interrupt check, fused
        ; test, live flags and memory operand, then a segment check when
        ; it starts one.
        add     r0,r6,#I_IRQ_CHECK
        ldmia   r0,{r0,r1,r2,r3,r12,lr} ; irq_check run seg_start fused dyn live_after
        add     r9,r6,#I_DEF
        ldmia   r9,{r9,r11,r12}         ; def ei mem
        tst     r9,lr
        movne   r11,#EF_LIVE
        moveq   r11,#0
        cmp     r12,#0
        orrne   r11,r11,#EF_MEM
        cmp     r3,#0
        orrne   r11,r11,#EF_FUSED
        cmp     r0,#0
        orrne   r11,r11,#EF_IRQ
        cmp     r2,#0
        bne     eb_seg
eb_insn
        add     r0,r6,#I_PRE
        ldmia   r0,{r0,r9}              ; pre, op
        cmp     r0,#PRE_ED
        beq     emit_ed
        bhi     eb_dd
        cmp     r0,#PRE_CB
        beq     emit_cb
eb_main
        adrl    r0,main_tab
        ldr     pc,[r0,r9,lsl #2]
eb_dd
        cmp     r0,#PRE_DDCB
        bhs     emit_cb
        tst     r11,#EF_MEM
        bne     eb_main
        cmp     r0,#PRE_DD
        orreq   r11,r11,#EF_IX
        orrne   r11,r11,#EF_IY
        b       eb_main

; Segment check: SUBS r8,r8,#t * CYCLE, then BMI to the time-out stub,
; which resumes at the check. r9 is loaded again at eb_insn.
eb_seg
        ldr     r9,[r6,#I_SEG_T]
        TMPL    r0,t_subs_cyc
        orr     r0,r0,#0xC00
        orr     r0,r0,r9                ; (always below 256: a byte in bits 8-15)
        mov     r12,r5                  ; the check, where the stub resumes
        EMIT    r0
        mov     r0,#STUB_TIMEOUT
        bl      new_stub
        ldr     r1,[r6,#I_PC]
        mov     r2,r9
        mov     r3,r12
        add     r0,r0,#ST_PC
        stmia   r0,{r1,r2,r3}           ; pc, t, resume
        mov     r0,#B_MI
        EMIT    r0
        b       eb_insn

; Every handler comes back here.
eb_done
        tst     r11,#EF_IRQ
        bne     eb_irq
eb_next
        add     r7,r7,#1
        add     r6,r6,#I_SIZE
        ldr     r0,[r4,#B_SKIP_TO]
        cmp     r7,r0
        blo     eb_loop

        ; The end of the instructions, or the instruction the branch past
        ; a port output loop skips to: the branch is patched here and the
        ; loop goes on to the end.
        ldr     r0,[r4,#B_SKIP_SITE]
        cmp     r0,#0
        beq     eb_noskip2
        mov     r1,r5
        PATCHB  r0,r1,r2
        mov     r0,#0
        str     r0,[r4,#B_SKIP_SITE]
        ldr     r0,[r4,#B_N]
        str     r0,[r4,#B_SKIP_TO]
        cmp     r7,r0
        blo     eb_loop
        b       eb_noskip2

; The instruction after EI: a pending interrupt is taken now (the block
; is left for it), otherwise the block goes on.
eb_irq
        adr     lr,t_irq_check
        ldmia   lr,{r0-r2}
        stmia   r5!,{r0-r2}
        mov     r0,#STUB_LEAVE
        bl      new_stub
        ldr     r1,[r6,#I_PC]
        ldr     r2,[r6,#I_LEN]
        add     r1,r1,r2
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16
        ldr     r2,[r6,#I_T_AFTER]
        add     r0,r0,#ST_PC
        stmia   r0,{r1,r2}              ; pc, t
        mov     r0,#B_NE
        EMIT    r0
        b       eb_next

eb_noskip2
        ; Fall-through exit when the block did not end with a transfer.
        sub     r6,r6,#I_SIZE           ; the last instruction
        ldr     r0,[r6,#I_KIND]
        cmp     r0,#K_JR
        ldrlo   r0,[r4,#B_NEXT_PC]
        bllo    e_exit

        ; The miss of the bank check.
        ldr     r0,[sp,#F_MISS]
        cmp     r0,#0
        beq     eb_stubs
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r0,[r4,#B_INS]
        ldr     r0,[r0,#I_PC]
        mov     r1,#0
        bl      e_mov32
        ldr     r0,[r8,#G_MISS]
        EMITB   r1,r0,B_AL

        ; The stubs, in the order they were made.
eb_stubs
        ldr     r7,[r4,#B_STUBS]        ; the stub
        ldr     r0,[r4,#B_NSTUBS]
        rsb     r0,r0,r0,lsl #3
        add     r9,r7,r0,lsl #2         ; the end of the stubs
        ldr     r11,[r8,#G_SEG_TIMEOUT] ; (r9 and r11 are free by now)
eb_stub_loop
        cmp     r7,r9
        bhs     eb_end
        ldr     r0,[r7,#ST_KIND]
        adr     r1,stub_tab
        ldr     pc,[r1,r0,lsl #2]
eb_stub_next
        add     r7,r7,#ST_SIZE
        b       eb_stub_loop
stub_tab
        DCD     st_timeout, st_forward, st_link, st_taken, st_write, st_leave, st_dyn

st_timeout
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        EMITB   r1,r11,BL_AL
        add     r3,r7,#ST_PC
        ldmia   r3,{r1,r2,r3}           ; pc, t, resume
        mov     r0,r2,lsl #8
        stmia   r5!,{r0,r1,r3}          ; t * CYCLE, pc, resume
        b       eb_stub_next
st_forward
        ldr     r0,[r7,#ST_SITE]
        ldr     r1,[r7,#ST_INDEX]
        ldr     r2,[r4,#B_INS]
        RECORD  r3,r2,r1
        ldr     r1,[r3,#I_HOST]
        PATCHB  r0,r1,r2
        b       eb_stub_next
st_link
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r0,[r8,#G_LINK]
        EMITB   r1,r0,BL_AL
        ldr     r0,[r7,#ST_PC]
        ldr     r1,[r7,#ST_SITE]
        ldr     r2,[r4,#B_KEY]
        mov     r3,r5                   ; the data words
        stmia   r5!,{r0-r2}
        bl      e_link
        b       eb_stub_next
st_taken
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r1,[r7,#ST_INDEX]
        ldr     r2,[r4,#B_INS]
        RECORD  r6,r2,r1                ; the instruction, for the helpers
        bl      e_taken
        b       eb_stub_next
st_dyn
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r1,[r7,#ST_INDEX]
        ldr     r2,[r4,#B_INS]
        RECORD  r6,r2,r1
        bl      e_dyn_exit
        b       eb_stub_next
st_write
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r0,[r7,#ST_REG]
        cmp     r0,#RAD
        TMPL    r1,t_mov_r12_r
        orrne   r1,r1,r0
        strne   r1,[r5],#4              ; MOV r12,reg
        TMPL    r0,t_mov_r2_lr
        EMIT    r0
        ldr     r0,[r8,#G_WRITE]
        EMITB   r1,r0,BL_AL
        ldr     r0,[r7,#ST_PC]
        ldr     r1,[r7,#ST_T]
        ldr     r2,[r4,#B_KEY]
        mov     r1,r1,lsl #8
        stmia   r5!,{r0-r2}
        b       eb_stub_next
st_leave
        ldr     r0,[r7,#ST_SITE]
        mov     r1,r5
        PATCHB  r0,r1,r2
        ldr     r0,[r7,#ST_PC]
        mov     r1,#0
        bl      e_mov32
        ldr     r1,[r7,#ST_T]
        mov     r1,r1,lsl #8
        TMPL    r0,t_mov_r1_imm
        bl      e_dpi
        ldr     r0,[r8,#G_LEAVE]
        EMITB   r1,r0,B_AL
        b       eb_stub_next

eb_end
        ldr     r1,[sp,#F_J]
        ldr     r0,[r1,#S_ZONE_END]
        sub     r0,r0,r5
        cmp     r0,#64                  ; 16 words
        blo     eb_full
eb_exit
        str     r5,[r1,#S_CUR]
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r9,r11,pc}
eb_full
        SETERR  r2,r3,4
        b       eb_exit

t_mark          STR     r10,[r10]       ; | offset
t_subs_cyc      SUBS    r8,r8,#0        ; | T-states * CYCLE
t_irq_check     LDR     r0,[r10,#GOFF_IRQ_LINE]
                LDR     r1,[r10,#GOFF_NMI]
                ORRS    r0,r0,r1
t_mov_r12_r     MOV     r12,r0          ; | register
t_mov_r2_lr     MOV     r2,lr
t_mov_r1_imm    MOV     r1,#0           ; | immediate

; Bank check of a block from a paged slot: LDR r0,[r10,#slot_bank + 4 *
; slot]; CMP r0,#bank (or through r1); BNE to the miss stub.
eb_bank_check
        ldr     r0,[r4,#B_KEY]
        mov     r1,r0,lsr #16
        sub     r1,r1,#1                ; slot
        TMPL    r2,t_ldr_r0_g
        add     r2,r2,#GOFF_SLOT_BANK :AND: 0xF00
        add     r2,r2,#GOFF_SLOT_BANK :AND: 0xFF
        add     r2,r2,r1,lsl #2
        EMIT    r2
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16           ; bank
        cmp     r0,#256
        bhs     eb_bank_wide
        TMPL    r1,t_cmp_r0_imm
        orr     r0,r1,r0
        EMIT    r0
eb_bank_site
        str     r5,[sp,#F_MISS]
        mov     r0,#B_NE
        EMIT    r0
        b       eb_body
eb_bank_wide
        mov     r1,#1
        bl      e_mov32
        TMPL    r0,t_cmp_r0_r1
        EMIT    r0
        b       eb_bank_site

t_ldr_r0_g      LDR     r0,[r10]        ; | offset
t_cmp_r0_imm    CMP     r0,#0           ; | bank
t_cmp_r0_r1     CMP     r0,r1

; Entry check of a RAM block: a BL, then the first address, the number of
; host words holding the block (from the one holding its first byte), those
; words as they are now, and a mask: zero outside the block and on the two
; operand bytes of each instruction that reads them when it runs. A block
; of up to RAM_INLINE_WORDS words is checked by code emitted after the mask,
; which the BL reaches (see eb_ram_inline); a longer one by the loop of the
; glue (glue.verify), which the BL calls and which returns after the mask.
; Either way lr points at the address word, as the glue expects it.
eb_ram_check
        ldr     r6,[r4,#B_INS]
        ldr     r7,[r6,#I_PC]           ; first address
        ldr     r0,[r4,#B_NEXT_PC]
        sub     r9,r0,r7
        mov     r9,r9,lsl #16
        mov     r9,r9,lsr #16           ; length
        and     r11,r7,#3               ; lead bytes
        add     r12,r11,r9
        add     r12,r12,#3
        mov     r12,r12,lsr #2          ; words
        cmp     r12,#RAM_INLINE_WORDS
        ldrhi   r0,[r8,#G_VERIFY]
        bhi     eb_ram_glue
        mov     r0,r12,lsl #1           ; BL over the data words: 2 n + 3 words on
        add     r0,r0,#1
        orr     r0,r0,#BL_AL
        EMIT    r0
        b       eb_ram_data
eb_ram_glue
        EMITB   r1,r0,BL_AL
eb_ram_data
        stmia   r5!,{r7,r12}
        mov     r0,r12,lsl #2           ; bytes
        mov     r1,#0                   ; byte index
        mov     r2,#0                   ; word so far
        GETCTX  r3
        add     r3,r3,#C_WTAB           ; global pointer
        add     r12,r11,r9              ; end of the block bytes
        ; The words: the byte of the block at each index inside it.
eb_ram_words
        mov     lr,#0
        cmp     r1,r11
        blo     eb_ram_w1
        cmp     r1,r12
        bhs     eb_ram_w1
        sub     lr,r1,r11
        add     lr,lr,r7
        mov     lr,lr,lsl #16
        mov     lr,lr,lsr #16           ; address
        mvn     r6,lr,lsr #8
        ldr     r6,[r3,r6,lsl #2]       ; read table entry of the page
        ldrb    lr,[r6,lr]
eb_ram_w1
        orr     r2,lr,r2,lsl #8
        add     r1,r1,#1
        tst     r1,#3
        streq   r2,[r5],#4
        cmp     r1,r0
        blo     eb_ram_words
        ; The mask: every byte of the block set.
        mov     r2,r5                   ; the mask words
        mov     r1,#0
        mov     lr,#0
eb_ram_mask
        mov     r3,#0
        cmp     r1,r11
        blo     eb_ram_m1
        cmp     r1,r12
        movlo   r3,#0xFF
eb_ram_m1
        orr     lr,r3,lr,lsl #8
        add     r1,r1,#1
        tst     r1,#3
        streq   lr,[r5],#4
        cmp     r1,r0
        blo     eb_ram_mask
        ; The operand bytes read at run time cleared.
        ldr     r6,[r4,#B_INS]
        ldr     r1,[r4,#B_HAS_DYN]
        cmp     r1,#0
        beq     eb_ram_done
        ldr     r1,[r4,#B_N]
eb_ram_dyn
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     eb_ram_dnext
        ldr     r0,[r6,#I_PC]
        ldr     r3,[r6,#I_LEN]
        add     r0,r0,r3
        sub     r0,r0,#2
        sub     r0,r0,r7
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16           ; offset of the operand in the block
        add     r0,r0,r11               ; byte index of its first byte
        mov     r3,#2
eb_ram_dbyte
        sub     lr,r0,r11
        cmp     lr,r9
        bhs     eb_ram_dskip
        and     lr,r0,#3
        mov     lr,lr,lsl #3
        rsb     lr,lr,#24
        mov     r12,#0xFF
        mov     lr,r12,lsl lr
        mov     r12,r0,lsr #2
        ldr     r12,[r2,r12,lsl #2]
        bic     r12,r12,lr
        mov     lr,r0,lsr #2
        str     r12,[r2,lr,lsl #2]
eb_ram_dskip
        add     r0,r0,#1
        subs    r3,r3,#1
        bne     eb_ram_dbyte
eb_ram_dnext
        add     r6,r6,#I_SIZE
        subs    r1,r1,#1
        bne     eb_ram_dyn
eb_ram_done
        ; r7 = first address, r9 = length, r11 = lead bytes
        add     r11,r11,r9
        add     r11,r11,#3
        mov     r11,r11,lsr #2          ; words
        cmp     r11,#RAM_INLINE_WORDS
        bhi     eb_body

; The inline check: the host words of the block loaded with LDM, from the
; read table entry of its page (whatever RAM the page shows), and each
; compared with its recorded bytes by immediate operations: BICS of the
; bytes the mask clears, EORS of the others (zero bytes left out, but for
; the last one, so that the flags always come from a word's last
; operation); a word differing branches to the glue's check (lr still
; points at the address word), which finds the change again and has the
; block translated anew. About 8 cycles per word against 25 in the glue's
; loop, for the short RAM routines entered on every line by raster effects.
eb_ram_inline
        sub     r6,r5,r11,lsl #3        ; the recorded words
        sub     r9,r5,r11,lsl #2        ; their mask
        ; LDR r12,[r10,#-4 * (page + 1)]: the read table entry of the page
        mov     r0,r7,lsr #8
        add     r0,r0,#1
        TMPL    r1,t_ri_ldr
        orr     r0,r1,r0,lsl #2
        EMIT    r0
        ; ADD r12,r12,#hi / #lo: the host address of the first word
        TMPL    r1,t_ri_add
        ands    r0,r7,#0xFF00
        orrne   r0,r1,r0,lsr #8
        orrne   r0,r0,#0xC00            ; rotated right by 24: bits 8-15
        strne   r0,[r5],#4
        ands    r0,r7,#0xFC
        orrne   r0,r1,r0
        strne   r0,[r5],#4
eb_ri_group
        ; LDMIA r12!,{r0-r2} while more than four words are left, then
        ; LDMIA r12,{...} of the rest, r12 itself taking a fourth one
        cmp     r11,#4
        movhi   lr,#3
        TMPLC   hi,r0,t_ri_ldm3
        bhi     eb_ri_ldm
        mov     lr,r11
        mov     r1,#1
        mov     r1,r1,lsl r11
        sub     r1,r1,#1                ; r0 to r(n - 1)
        cmp     r11,#4
        biceq   r1,r1,#8                ; r12 instead of r3
        orreq   r1,r1,#0x1000
        TMPL    r0,t_ri_ldm
        orr     r0,r0,r1
eb_ri_ldm
        EMIT    r0
        sub     r11,r11,lr
        mov     r7,#0                   ; register of the word
eb_ri_word
        ldr     r2,[r6],#4              ; the recorded word
        ldr     r3,[r9],#4              ; its mask
        cmp     r7,#3
        moveq   r7,#12
        orr     r12,r7,r7,lsl #4
        mov     r12,r12,lsl #12         ; Rn and Rd fields
        RIBYTE  24,4
        RIBYTE  16,8
        RIBYTE  8,12
        ands    r1,r3,#0xFF             ; the last byte, always
        TMPLC   eq,r0,t_ri_bics
        moveq   r1,#0xFF
        TMPLC   ne,r0,t_ri_eors
        andne   r1,r2,#0xFF
        orr     r0,r0,r1
        orr     r0,r0,r12
        EMIT    r0
        ldr     r0,[r8,#G_VERIFY]
        EMITB   r1,r0,B_NE
        add     r7,r7,#1
        subs    lr,lr,#1
        bne     eb_ri_word
        teq     r11,#0
        bne     eb_ri_group
        b       eb_body

t_ri_ldr        DCD     0xE51AC000      ; LDR r12,[r10,#-0] | offset
t_ri_add        ADD     r12,r12,#0      ; | immediate
t_ri_ldm3       LDMIA   r12!,{r0-r2}
t_ri_ldm        DCD     0xE89C0000      ; LDMIA r12,{} | registers
t_ri_bics       BICS    r0,r0,#0        ; | registers, immediate
t_ri_eors       EORS    r0,r0,#0        ; | registers, immediate

;----------------------------------------------------------------------------
; Stubs and encodings
;----------------------------------------------------------------------------

; new_stub: r0 = kind -> r0 = a new stub (site = the cursor; the other
; fields are left to the caller: each kind reads only the ones its creator
; sets). Uses r1-r3.
new_stub
        ldr     r1,[r4,#B_NSTUBS]
        ldr     r2,[r4,#B_STUBS]
        rsb     r3,r1,r1,lsl #3
        add     r2,r2,r3,lsl #2
        add     r1,r1,#1
        str     r1,[r4,#B_NSTUBS]
        cmp     r1,#MAX_STUBS
        bhi     ns_many
ns_fill
        mov     r1,r5
        stmia   r2,{r0,r1}
        mov     r0,r2
        mov     pc,lr
ns_many
        SETERR  r1,r3,3
        b       ns_fill

; Shifts $v right by its trailing zero count rounded down to even and
; takes half of it from $r ($t: scratch).
        MACRO
        TZEVEN  $v,$r,$t
        movs    $t,$v,lsl #16
        moveq   $v,$v,lsr #16
        subeq   $r,$r,#8
        tst     $v,#0xFF
        moveq   $v,$v,lsr #8
        subeq   $r,$r,#4
        tst     $v,#0xF
        moveq   $v,$v,lsr #4
        subeq   $r,$r,#2
        tst     $v,#3
        moveq   $v,$v,lsr #2
        subeq   $r,$r,#1
        MEND

; e_imm: r1 = value -> r1 = its immediate operand encoding, or IMM_NONE.
; The usual forms of the generated code (a byte in bits 16-23 or 24-31,
; register pairs and A, or in bits 8-15, T-states) come first; the rest
; is tried as a byte rotated right by an even amount, directly or
; wrapping around bit 31. Uses r2, r3, r12.
e_imm
        cmp     r1,#256
        movlo   pc,lr
        bics    r2,r1,#0xFF0000
        moveq   r1,r1,lsr #16
        orreq   r1,r1,#0x800
        moveq   pc,lr
        bics    r2,r1,#0xFF000000
        moveq   r1,r1,lsr #24
        orreq   r1,r1,#0x400
        moveq   pc,lr
        bics    r2,r1,#0xFF00
        moveq   r1,r1,lsr #8
        orreq   r1,r1,#0xC00
        moveq   pc,lr
        ; A byte shifted left by an even amount: 16 - tz / 2 rotations.
        mov     r2,r1
        mov     r3,#16
        TZEVEN  r2,r3,r12
        cmp     r2,#256
        andlo   r3,r3,#15
        orrlo   r1,r2,r3,lsl #8
        movlo   pc,lr
        ; Wrapping around bit 31: contiguous once rotated left by 8.
        mov     r2,r1,lsl #8
        orr     r2,r2,r1,lsr #24
        mov     r3,#4
        TZEVEN  r2,r3,r12
        cmp     r2,#256
        andlo   r3,r3,#15
        orrlo   r1,r2,r3,lsl #8
        mvnhs   r1,#0
        mov     pc,lr

; e_dpi: emits the data processing word r0 (with its immediate bit) with
; the immediate value r1 encoded in it; an unencodable value is an
; error. A byte, or a byte in bits 8-15 (T-states), is encoded here; the
; other forms go through e_imm. Uses r1-r3, r12.
e_dpi
        cmp     r1,#256
        orrlo   r0,r0,r1
        strlo   r0,[r5],#4
        movlo   pc,lr
        bics    r2,r1,#0xFF00
        orreq   r0,r0,#0xC00
        orreq   r0,r0,r1,lsr #8
        streq   r0,[r5],#4
        moveq   pc,lr
        stmfd   sp!,{lr}
        bl      e_imm
        cmp     r1,#IMM_NONE
        beq     ed_none
        orr     r0,r0,r1
        EMIT    r0
        ldmfd   sp!,{pc}
ed_none
        SETERR  r2,r3,1
        EMIT    r0
        ldmfd   sp!,{pc}

; e_mov32: emits r1 = r0 (32 bits), one to four instructions.
; Uses r0-r3, r12.
e_mov32
        ; A 16-bit value with both bytes set (the usual case: a Z80
        ; address): one MOV when its bits fit in 8 from an even position
        ; at or below its lowest set bit, else MOV + ORR. Nothing saved.
        cmp     r0,#0x10000
        bhs     em_general
        ands    r2,r0,#0xFF
        beq     em_general
        cmp     r0,#256
        blo     em_general
        mov     r12,r1,lsl #12          ; the destination field
        tst     r0,#0xF
        bne     em_p03
        mov     r2,#4
        tst     r0,#0x30
        moveq   r2,#6
        b       em_p
em_p03
        tst     r0,#3
        moveq   r2,#2
        movne   r2,#0
em_p
        mov     r3,r0,lsr r2
        cmp     r3,#256
        bhs     em_two
        rsb     r2,r2,#32
        mov     r2,r2,lsr #1            ; 16 - p / 2
        and     r2,r2,#15
        orr     r3,r3,r2,lsl #8
        TMPL    r1,t_mov_imm
        orr     r1,r1,r12
        orr     r1,r1,r3
        EMIT    r1
        mov     pc,lr
em_two
        TMPL    r1,t_mov_imm
        orr     r1,r1,r12
        and     r2,r0,#0xFF
        orr     r1,r1,r2
        TMPL    r2,t_orr_imm
        orr     r2,r2,r12
        orr     r2,r2,r12,lsl #4        ; the same register as source
        orr     r2,r2,#0xC00
        orr     r2,r2,r0,lsr #8
        stmia   r5!,{r1,r2}
        mov     pc,lr
em_general
        stmfd   sp!,{r4,r6,r7,lr}
        mov     r4,r1,lsl #12           ; the destination field
        mov     r6,r0                   ; the value
        mov     r1,r0
        bl      e_imm
        cmp     r1,#IMM_NONE
        beq     em_mvn
        TMPL    r0,t_mov_imm
        orr     r0,r0,r4
        orr     r0,r0,r1
        EMIT    r0
        ldmfd   sp!,{r4,r6,r7,pc}
em_mvn
        mvn     r1,r6
        bl      e_imm
        cmp     r1,#IMM_NONE
        beq     em_bytes
        TMPL    r0,t_mvn_imm
        orr     r0,r0,r4
        orr     r0,r0,r1
        EMIT    r0
        ldmfd   sp!,{r4,r6,r7,pc}
em_bytes
        ; Byte by byte: MOV for the first nonzero one, ORR for the others
        ; (the value has at least two nonzero bytes here).
        mov     r7,#0                   ; shift
em_byte
        mov     r1,#0xFF
        and     r1,r6,r1,lsl r7
        cmp     r1,#0
        beq     em_bnext
        bl      e_imm
        mov     r0,#1
        mov     r0,r0,lsl r7
        sub     r0,r0,#1
        tst     r6,r0                   ; a nonzero byte below this one
        TMPL    r0,t_mov_imm
        TMPL    r2,t_orr_imm
        movne   r0,r2
        orr     r0,r0,r4,lsl #4         ; the destination as Rn, MOV included
        orr     r0,r0,r4
        orr     r0,r0,r1
        EMIT    r0
em_bnext
        add     r7,r7,#8
        cmp     r7,#32
        blo     em_byte
        ldmfd   sp!,{r4,r6,r7,pc}

t_mov_imm       MOV     r0,#0           ; | destination, immediate
t_orr_imm       ORR     r0,r0,#0        ; | destination, source, immediate
t_mvn_imm       MVN     r0,#0

; e_link: a static exit to r0 from the link data words at r3 (the key is
; in r2): when the target is translated already, the stub becomes a
; direct branch (jit_link_host, jit_link_stub). Uses r0-r3, r12.
e_link
        stmfd   sp!,{r3,lr}
        mov     r1,r0
        GETJ    r0
        bl      jit_link_host
        cmp     r0,#0
        ldmfd   sp!,{r3,lr}
        moveq   pc,lr
        mov     r2,r0
        mov     r1,r3
        GETJ    r0
        b       jit_link_stub

;----------------------------------------------------------------------------
; Memory
;----------------------------------------------------------------------------

; e_read: emits r0 = byte at the Z80 address held in register r0 (bits
; 16-31): the page entry below the global pointer, then LDRB. Uses r1-r3,
; r12.
e_read
        adr     r12,t_read
        ldmia   r12,{r1,r2,r3}
        orr     r1,r1,r0
        orr     r3,r3,r0
        stmia   r5!,{r1,r2,r3}
        mov     pc,lr
t_read          MVN     r0,r0,LSR #24   ; | address register
                LDR     r0,[r10,r0,LSL #2]
                LDRB    r0,[r0,r0,LSR #16]      ; | address register

; e_write: emits the write of r0 to the Z80 address held in register r0
; (bits 16-31): the page entry above the global pointer, STRB when it is
; nonzero, else a special write through its stub, which gets the address
; of the next instruction (where to go if the machine asks to leave the
; block; never for a repeated instruction) and the T-states left after
; the instruction. Uses r1-r3, r12.
e_write
        stmfd   sp!,{r0,lr}
        adr     r12,t_write
        ldmia   r12,{r1,r2,r3,r12}
        orr     r1,r1,r0
        orr     r12,r12,r0
        stmia   r5!,{r1,r2,r3,r12}
        mov     r0,#STUB_WRITE
        bl      new_stub
        ldmfd   sp!,{r1,lr}
        str     r1,[r0,#ST_REG]
        ldr     r1,[r6,#I_KIND]
        cmp     r1,#K_REP
        mvneq   r1,#0
        beq     ew_pc
        ldr     r1,[r6,#I_PC]
        ldr     r2,[r6,#I_LEN]
        add     r1,r1,r2
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16
ew_pc
        str     r1,[r0,#ST_PC]
        ldr     r2,[r6,#I_T_AFTER]
        str     r2,[r0,#ST_T]
        mov     r0,#BL_EQ
        EMIT    r0
        mov     pc,lr
t_write         MOV     r1,r0,LSR #24   ; | address register
                LDR     r1,[r10,r1,LSL #2]
                TEQ     r1,#0
                STRNEB  r0,[r1,r0,LSR #16]      ; | address register

; e_fixed: r0 = Z80 address (16 bits), r1 = 1 for a write -> r0 = offset
; from the global pointer of its host byte when its mapping never changes
; (and the offset is positive, below 1 MiB), else IMM_NONE. Uses r1-r3.
e_fixed
        GETCTX  r2
        mov     r3,r0,lsr #8
        add     r3,r3,#C_PAGE_KIND
        ldrb    r3,[r2,r3]              ; kind of the page
        add     r2,r2,#C_WTAB           ; global pointer
        cmp     r1,#0
        beq     ef_read
        tst     r3,#PAGE_WFIXED
        beq     ef_none
        mov     r3,r0,lsr #8
        ldr     r3,[r2,r3,lsl #2]       ; write table entry
        cmp     r3,#0
        beq     ef_none
        b       ef_host
ef_read
        tst     r3,#PAGE_RFIXED
        beq     ef_none
        mvn     r3,r0,lsr #8
        ldr     r3,[r2,r3,lsl #2]       ; read table entry
ef_host
        add     r3,r3,r0                ; host address
        cmp     r3,r2
        blo     ef_none
        sub     r0,r3,r2
        cmp     r0,#0x100000
        movlo   pc,lr
ef_none
        mvn     r0,#0
        mov     pc,lr

; e_read_const: emits r0 = byte at the constant Z80 address r0: LDRB from
; the global pointer when the mapping is fixed (through r1 when the offset
; is above 4095), else the address in r12 and the generic read. Uses
; r0-r3, r12.
e_read_const
        stmfd   sp!,{r0,lr}
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#0
        bl      e_fixed
        cmp     r0,#IMM_NONE
        beq     erc_generic
        TMPL    r1,t_ldrb_g
        bl      e_fixed_base
        EMIT    r0
        ldmfd   sp!,{r1,pc}
erc_generic
        ldmfd   sp!,{r0,lr}
        stmfd   sp!,{lr}
        mov     r0,r0,lsl #16           ; the address in bits 16-31 of r12
        mov     r1,#RAD
        bl      e_mov32
        mov     r0,#RAD
        bl      e_read
        ldmfd   sp!,{pc}

; e_write_const: emits the write of r0 to the constant Z80 address r0.
; Uses r0-r3, r12.
e_write_const
        stmfd   sp!,{r0,lr}
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#1
        bl      e_fixed
        cmp     r0,#IMM_NONE
        beq     ewc_generic
        TMPL    r1,t_strb_g
        bl      e_fixed_base
        EMIT    r0
        ldmfd   sp!,{r1,pc}
ewc_generic
        ldmfd   sp!,{r0,lr}
        stmfd   sp!,{lr}
        mov     r0,r0,lsl #16
        mov     r1,#RAD
        bl      e_mov32
        mov     r0,#RAD
        bl      e_write
        ldmfd   sp!,{pc}

; e_fixed_base: r0 = fixed offset, r1 = the byte access word with the
; global pointer as base -> r0 = that word with the offset: when the
; offset is above 4095, ADD r1,r10,#high part is emitted and r1 is the
; base. Uses r1-r3, r12.
e_fixed_base
        movs    r2,r0,lsr #12
        orreq   r0,r1,r0
        moveq   pc,lr
        stmfd   sp!,{r0,r1,lr}
        TMPL    r0,t_add_r1_g
        mov     r1,r2,lsl #12           ; the part above 4095
        bl      e_dpi
        ldmfd   sp!,{r0,r1,lr}
        mov     r0,r0,lsl #20
        mov     r0,r0,lsr #20
        bic     r1,r1,#0xF0000
        orr     r1,r1,#0x10000          ; base r1
        orr     r0,r1,r0
        mov     pc,lr
t_ldrb_g        LDRB    r0,[r10]        ; | offset
t_strb_g        STRB    r0,[r10]        ; | offset
t_add_r1_g      ADD     r1,r10,#0       ; | immediate

; e_mem_addr -> r0 = the register (bits 16-31) holding the address of the
; memory operand of the instruction: HL, IX, or IY loaded in r12, with the
; displacement added in r12. Uses r1-r3, r12.
e_mem_addr
        ldr     r1,[r6,#I_PRE]
        ldr     r2,[r6,#I_D]
        cmp     r1,#PRE_DD
        moveq   r0,#RIX
        beq     ema_disp
        cmp     r1,#PRE_DDCB
        moveq   r0,#RIX
        beq     ema_disp
        cmp     r1,#PRE_FD
        cmpne   r1,#PRE_FDCB
        movne   r0,#RHL
        movne   pc,lr
        TMPL    r3,t_ldr_r12_iy
        EMIT    r3
        mov     r0,#RAD
ema_disp
        cmp     r2,#0
        moveq   pc,lr
        TMPL    r3,t_add_r12_d
        TMPL    r1,t_sub_r12_d
        movlt   r3,r1
        rsblt   r2,r2,#0
        orr     r3,r3,r0,lsl #16
        orr     r3,r3,r2                ; d << 16: a byte in bits 16-23
        EMIT    r3
        mov     r0,#RAD
        mov     pc,lr
t_ldr_r12_iy    LDR     r12,[r10,#GOFF_IY]
t_add_r12_d     DCD     0xE280C800      ; ADD r12,base,#d << 16 | base, d
t_sub_r12_d     DCD     0xE240C800      ; SUB r12,base,#d << 16

; e_push: emits the push of the 16-bit value in r0 (uses r1, r2 of the
; generated code; a special page goes through the glue).
e_push
        stmfd   sp!,{lr}
        adr     r12,t_push
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_push + 24
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        ldr     r0,[r8,#G_PUSH_SLOW]
        EMITB   r1,r0,BL_EQ
        ldmfd   sp!,{pc}
t_push          SUB     r11,r11,#0x20000
                MOV     r1,r11,LSR #24
                LDR     r1,[r10,r1,LSL #2]
                TEQ     r1,#0
                ADDNE   r1,r1,r11,LSR #16
                STRNEB  r0,[r1]
                MOVNE   r2,r0,LSR #8
                STRNEB  r2,[r1,#1]

; e_pop: emits the pop of 16 bits: low byte in r2, high byte in r1 (uses
; r0 of the generated code).
e_pop
        stmfd   sp!,{lr}
        adr     r12,t_pop
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        ldmfd   sp!,{pc}
t_pop           MVN     r0,r11,LSR #24
                LDR     r1,[r10,r0,LSL #2]
                LDRB    r2,[r1,r11,LSR #16]
                ADD     r11,r11,#0x10000
                LDRB    r1,[r1,r11,LSR #16]
                ADD     r11,r11,#0x10000

;----------------------------------------------------------------------------
; Registers
;----------------------------------------------------------------------------

; e_pair_of: r0 = Z80 register 0-7 (B C D E H L - A), r1 = a temporary
; register, r2 = index register of H and L (0, EF_IX, EF_IY) -> r0 = the
; ARM register holding its pair (IY is loaded in the temporary). Uses r3.
e_pair_of
        cmp     r0,#7
        moveq   r0,#RA
        moveq   pc,lr
        cmp     r0,#2
        movlo   r0,#RBC
        movlo   pc,lr
        cmp     r0,#4
        movlo   r0,#RDE
        movlo   pc,lr
        cmp     r2,#EF_IX
        moveq   r0,#RIX
        moveq   pc,lr
        cmp     r2,#EF_IY
        movne   r0,#RHL
        movne   pc,lr
        TMPL    r3,t_ldr_iy
        orr     r3,r3,r1,lsl #12
        EMIT    r3
        mov     r0,r1
        mov     pc,lr
t_ldr_iy        LDR     r0,[r10,#GOFF_IY]       ; | register

; e_get8: r0 = Z80 register, r1 = a scratch register, r2 = index register
; -> r0 = operand (clean value in bits 24-31 of the register shifted left:
; the high byte of a pair isolated in the scratch register). Uses r3.
e_get8
        stmfd   sp!,{lr}
        cmp     r0,#7
        moveq   r0,#RA
        beq     eg_done
        stmfd   sp!,{r0}
        bl      e_pair_of
        ldmfd   sp!,{r3}
        tst     r3,#1
        orrne   r0,r0,#0x400            ; the low byte: shifted left by 8
        bne     eg_done
        TMPL    r3,t_and_hi
        orr     r3,r3,r0,lsl #16
        orr     r3,r3,r1,lsl #12
        EMIT    r3
        mov     r0,r1
eg_done
        ldmfd   sp!,{pc}
t_and_hi        AND     r0,r0,#0xFF000000       ; | scratch, pair

; e_set8: r0 = Z80 register, r1 = operand (e_get8 form), r2 = index
; register: emits the register = the operand. Uses r3, r12.
e_set8
        stmfd   sp!,{r4,lr}
        mov     r4,r1
        cmp     r0,#7
        bne     es_pair
        tst     r4,#O_IMM
        TMPLC   ne,r3,t_mov_a_imm       ; A = immediate << 24
        TMPLC   eq,r3,t_mov_a_r         ; A = register << shift
        mov     r1,r4,lsl #20
        mov     r1,r1,lsr #20
        orr     r3,r3,r1
        EMIT    r3
        ldmfd   sp!,{r4,pc}
es_pair
        ; The pair, IY loaded in lr and stored back.
        stmfd   sp!,{r0,r2}
        mov     r1,#RLR
        bl      e_pair_of
        ldmfd   sp!,{r1,r2}
        mov     r3,r0,lsl #16
        orr     r3,r3,r0,lsl #12        ; pair as Rn and Rd
        tst     r1,#1
        bne     es_low
        ; High byte.
        tst     r4,#O_IMM
        beq     es_hreg
        tst     r4,#0xFF0000
        TMPL    r12,t_bic_hi
        orreq   r12,r12,r3
        streq   r12,[r5],#4             ; zero: BIC
        beq     es_iy
        TMPL    r12,t_and_lo
        orr     r12,r12,r3
        TMPL    lr,t_orr_imm_hi
        orr     lr,lr,r3
        and     r0,r4,#0xFF0000
        orr     lr,lr,r0,lsr #16
        stmia   r5!,{r12,lr}
        b       es_iy
es_hreg
        TMPL    r12,t_and_lo
        orr     r12,r12,r3
        TMPL    lr,t_orr_reg
        orr     lr,lr,r3
        orr     lr,lr,r4
        stmia   r5!,{r12,lr}
        b       es_iy
es_low
        tst     r4,#O_IMM
        bne     es_limm
        and     r12,r4,#0xF80
        cmp     r12,#0x400              ; a low byte operand: moved to r1
        TMPLC   eq,r12,t_mov_r1_lsl8
        andeq   r0,r4,#15
        orreq   r12,r12,r0
        streq   r12,[r5],#4
        moveq   r4,#1
        TMPL    r12,t_and_hi_p
        orr     r12,r12,r3
        and     lr,r4,#0xF80
        cmp     lr,#0xC00
        TMPLC   eq,lr,t_orr_lsl16
        TMPLC   ne,lr,t_orr_lsr8
        orr     lr,lr,r3
        and     r0,r4,#15
        orr     lr,lr,r0
        stmia   r5!,{r12,lr}
        b       es_iy
es_limm
        tst     r4,#0xFF0000
        TMPL    r12,t_bic_lo
        orreq   r12,r12,r3
        streq   r12,[r5],#4
        beq     es_iy
        TMPL    r12,t_and_hi_p
        orr     r12,r12,r3
        TMPL    lr,t_orr_imm_lo
        orr     lr,lr,r3
        and     r0,r4,#0xFF0000
        orr     lr,lr,r0,lsr #16
        stmia   r5!,{r12,lr}
es_iy
        cmp     r2,#EF_IY
        bne     es_done
        cmp     r1,#4
        cmpne   r1,#5
        TMPLC   eq,r3,t_str_lr_iy
        streq   r3,[r5],#4
es_done
        ldmfd   sp!,{r4,pc}
t_mov_a_imm     MOV     r4,#0           ; | immediate << 24
t_mov_a_r       MOV     r4,r0           ; | operand
t_bic_hi        BIC     r0,r0,#0xFF000000       ; | pair
t_and_lo        AND     r0,r0,#0x00FF0000
t_orr_imm_hi    DCD     0xE3800400      ; ORR pair,pair,#imm << 24 | pair, imm
t_orr_reg       ORR     r0,r0,r0        ; | pair, operand
t_mov_r1_lsl8   MOV     r1,r0,LSL #8    ; | register
t_and_hi_p      AND     r0,r0,#0xFF000000
t_orr_lsl16     ORR     r0,r0,r0,LSL #16        ; | pair, register
t_orr_lsr8      ORR     r0,r0,r0,LSR #8
t_bic_lo        BIC     r0,r0,#0x00FF0000
t_orr_imm_lo    DCD     0xE3800800      ; ORR pair,pair,#imm << 16 | pair, imm
t_str_lr_iy     STR     lr,[r10,#GOFF_IY]

; e_to_low: r1 = operand -> emits r0 = its value 0-255. Uses r0, r2.
e_to_low
        tst     r1,#O_IMM
        beq     etl_reg
        TMPL    r0,t_mov_r0_imm
        and     r2,r1,#0xFF0000
        orr     r0,r0,r2,lsr #16
        EMIT    r0
        mov     pc,lr
etl_reg
        and     r2,r1,#0xF80
        cmp     r2,#0xC00
        bne     etl_notlow
        ands    r2,r1,#15
        moveq   pc,lr                   ; already r0
        TMPL    r0,t_mov_r0_r
        orr     r0,r0,r2
        EMIT    r0
        mov     pc,lr
etl_notlow
        cmp     r2,#0
        TMPL    r0,t_mov_r0_lsr24
        orreq   r0,r0,r1
        streq   r0,[r5],#4
        moveq   pc,lr
        mov     r2,r0
        TMPL    r0,t_mov_r0_r
        orr     r0,r0,r1
        stmia   r5!,{r0,r2}             ; shifted left, then right by 24
        mov     pc,lr
t_mov_r0_imm    MOV     r0,#0           ; | immediate
t_mov_r0_r      MOV     r0,r0           ; | operand
t_mov_r0_lsr24  MOV     r0,r0,LSR #24   ; | register

; e_get_low: r0 = Z80 register 0-7 -> emits r0 = its value (possibly with
; higher bits). Uses r1.
e_get_low
        adr     r1,t_get_low
        ldr     r0,[r1,r0,lsl #2]
        EMIT    r0
        mov     pc,lr
t_get_low       MOV     r0,r5,LSR #24
                MOV     r0,r5,LSR #16
                MOV     r0,r6,LSR #24
                MOV     r0,r6,LSR #16
                MOV     r0,r7,LSR #24
                MOV     r0,r7,LSR #16
                MOV     r0,#0
                MOV     r0,r4,LSR #24

; e_idx_reg: r0 = a temporary register -> r0 = the register holding HL,
; IX, or IY loaded in the temporary, for the instruction. Uses r1, r2.
e_idx_reg
        ldr     r1,[r6,#I_PRE]
        cmp     r1,#PRE_DD
        moveq   r0,#RIX
        moveq   pc,lr
        cmp     r1,#PRE_FD
        movne   r0,#RHL
        movne   pc,lr
        TMPL    r2,t_ldr_iy
        orr     r2,r2,r0,lsl #12
        EMIT    r2
        mov     pc,lr

; e_idx_store: r0 = register: emits its store to IY for an FD instruction.
e_idx_store
        ldr     r1,[r6,#I_PRE]
        cmp     r1,#PRE_FD
        movne   pc,lr
        TMPL    r2,t_str_iy
        orr     r2,r2,r0,lsl #12
        EMIT    r2
        mov     pc,lr
t_str_iy        STR     r0,[r10,#GOFF_IY]       ; | register

; e_rp_reg: r0 = pair 0-3 (BC DE HL SP), r1 = a temporary -> r0 = its
; register (HL as e_idx_reg). Uses r1, r2.
e_rp_reg
        cmp     r0,#1
        movlo   r0,#RBC
        moveq   r0,#RDE
        movls   pc,lr
        cmp     r0,#3
        moveq   r0,#RSP
        moveq   pc,lr
        mov     r0,r1
        b       e_idx_reg

;----------------------------------------------------------------------------
; 8-bit arithmetic
;----------------------------------------------------------------------------

; e_alu: r0 = operation 0-7 (ADD ADC SUB SBC AND XOR OR CP), r1 = operand:
; emits A = A op operand, with the flags the instruction needs: all of
; them (full), only the ARM flags for a fused test, or none.
e_alu
        stmfd   sp!,{r4,r7,lr}
        mov     r4,r0
        mov     r7,r1
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE             ; full
        bne     ea_dispatch
        ; The half-carry check reads the operand after A has changed: a
        ; copy of A is taken first.
        tst     r7,#O_IMM
        bne     ea_dispatch
        mov     r0,r7,lsl #20
        mov     r0,r0,lsr #20
        cmp     r0,#RA
        bne     ea_dispatch
        cmp     r4,#0
        cmpne   r4,#2
        bne     ea_dispatch
        TMPL    r0,t_mov_r0_a
        EMIT    r0
        mov     r7,#0                   ; r0 of the generated code
ea_dispatch
        adr     r0,ea_tab
        ldr     pc,[r0,r4,lsl #2]
ea_tab          DCD     ea_add, ea_adc, ea_sub, ea_sbc, ea_logic, ea_logic, ea_logic, ea_sub

ea_add
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        TMPLC   eq,r0,t_mov_r1_a4
        streq   r0,[r5],#4
        TMPL    r0,t_add_a
        bl      ea_op
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     ea_done
        adr     r12,t_mrs_f
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        TMPL    r0,t_cmn_r1_r
        TMPL    r1,t_cmn_r1_i
        bl      e_cmp4
        TMPL    r0,t_orrcs_h
        EMIT    r0
        b       ea_full

ea_adc
        mov     r1,r7
        bl      e_to_low
        adr     r12,t_adc_carry
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     ea_adc_s
        adr     r12,t_adc_full
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        b       ea_full
ea_adc_s
        TMPL    r0,t_adc_a
        tst     r11,#EF_LIVE + EF_FUSED
        orrne   r0,r0,#0x100000
        EMIT    r0
        b       ea_done

ea_sub
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r4,#7
        cmpeq   r2,#0
        beq     ea_done                 ; CP with nothing to show
        cmp     r2,#EF_LIVE
        TMPLC   eq,r0,t_mov_r1_a4
        streq   r0,[r5],#4
        cmp     r4,#2
        bne     ea_cp_op
        TMPL    r0,t_sub_a
        bl      ea_op
        b       ea_sub_flags
ea_cp_op
        TMPL    r0,t_cmp_a
        bic     r1,r7,#0xFF0000
        orr     r0,r0,r1
        EMIT    r0
ea_sub_flags
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     ea_done
        adr     r12,t_mrs_f
        ldmia   r12,{r0,r1}
        TMPL    r2,t_eor_f_cn
        stmia   r5!,{r0-r2}
        TMPL    r0,t_cmp_r1_r
        TMPL    r1,t_cmp_r1_i
        bl      e_cmp4
        TMPL    r0,t_orrcc_h
        EMIT    r0
        b       ea_full

ea_sbc
        mov     r1,r7
        bl      e_to_low
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     ea_sbc_s
        adr     r12,t_sbc_full
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_sbc_full + 24
        ldmia   r12,{r0-r2}
        stmia   r5!,{r0-r2}
        b       ea_full
ea_sbc_s
        adr     r12,t_sbc_s
        ldmia   r12,{r0,r1,r2}
        tst     r11,#EF_LIVE + EF_FUSED
        orrne   r2,r2,#0x100000
        stmia   r5!,{r0,r1,r2}
        b       ea_done

ea_logic
        adr     r0,t_logic - 16
        ldr     r0,[r0,r4,lsl #2]
        bl      ea_op
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     ea_done
        adr     r12,t_szp_a
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        cmp     r4,#4
        TMPLC   eq,r0,t_orr_f_h
        streq   r0,[r5],#4
ea_full
        FULLF   r0,r1
ea_done
        ldmfd   sp!,{r4,r7,pc}

; The operation word r0 with the operand and the S bit of the instruction.
ea_op
        bic     r1,r7,#0xFF0000         ; the field and the immediate bit
        orr     r0,r0,r1
        tst     r11,#EF_LIVE + EF_FUSED
        orrne   r0,r0,#0x100000
        EMIT    r0
        mov     pc,lr

; e_cmp4: emits the compare of r1 with the operand r7 shifted left by 4:
; r0 = the word with a register operand, r1 = the word with an immediate.
e_cmp4
        tst     r7,#O_IMM
        bne     ec4_imm
        mov     r2,r7,lsl #20
        mov     r2,r2,lsr #20
        add     r2,r2,#0x200            ; the shift, plus 4
        orr     r0,r0,r2
        EMIT    r0
        mov     pc,lr
ec4_imm
        ands    r2,r7,#0xF0000          ; the low nibble of the immediate
        orrne   r1,r1,#0x400
        orrne   r1,r1,r2,lsr #12        ; in bits 28-31
        EMIT    r1
        mov     pc,lr

t_mov_r0_a      MOV     r0,r4
t_mov_r1_a4     MOV     r1,r4,LSL #4
t_add_a         ADD     r4,r4,r0        ; | operand, S
t_sub_a         SUB     r4,r4,r0
t_cmp_a         CMP     r4,r0           ; | operand
t_mrs_f         MRS     r3,CPSR
                MOV     r3,r3,LSR #28
t_cmn_r1_r      CMN     r1,r0           ; | operand << 4
t_cmn_r1_i      CMN     r1,#0           ; | immediate << 28
t_cmp_r1_r      CMP     r1,r0
t_cmp_r1_i      CMP     r1,#0
t_orrcs_h       ORRCS   r3,r3,#PSR_H
t_orrcc_h       ORRCC   r3,r3,#PSR_H
t_eor_f_cn      EOR     r3,r3,#PSR_C + PSR_n
t_adc_carry     MOVS    r1,r3,LSR #2
                SUBCS   r0,r0,#0x100
t_adc_full      EOR     r3,r0,r4,LSR #24
                ADCS    r4,r4,r0,ROR #8
                MRS     r0,CPSR
                EOR     r3,r3,r4,LSR #24
                AND     r3,r3,#PSR_H
                ORR     r3,r3,r0,LSR #28
t_adc_a         ADC     r4,r4,r0,ROR #8 ; | S
t_sbc_full      AND     r3,r3,#PSR_C
                SUBS    r0,r0,r3,LSL #7
                EOR     r3,r0,r4,LSR #24
                SBCS    r4,r4,r0,ROR #8
                MRS     r0,CPSR
                EOR     r3,r3,r4,LSR #24
                AND     r3,r3,#PSR_H
                ORR     r3,r3,r0,LSR #28
                EOR     r3,r3,#PSR_C + PSR_n
t_sbc_s         AND     r1,r3,#PSR_C
                SUBS    r0,r0,r1,LSL #7
                SBC     r4,r4,r0,ROR #8 ; | S
t_logic         AND     r4,r4,r0        ; | operand, S
                EOR     r4,r4,r0
                ORR     r4,r4,r0
t_szp_a         ADD     r2,r10,#PZST_OFF
                LDRB    r3,[r2,r4,LSR #24]
t_orr_f_h       ORR     r3,r3,#PSR_H

; e_incdec: r0 = register holding the clean value in bits 24-31, r1 = 1
; for DEC: emits the increment or decrement with the flags needed.
e_incdec
        stmfd   sp!,{lr}
        orr     r2,r0,r0,lsl #4
        mov     r2,r2,lsl #12           ; the register as Rn and Rd
        and     r3,r11,#EF_LIVE + EF_FUSED
        cmp     r3,#EF_LIVE
        bne     eid_short
        cmp     r1,#0
        bne     eid_dec
        adr     r12,t_inc_full
        ldmia   r12,{r0,r1,r3,r12,lr}   ; AND ADDS ORRMI ORRVS ORRCS
        orr     r1,r1,r2
        stmia   r5!,{r0,r1,r3,r12,lr}
        adr     r12,t_inc_full + 20
        ldmia   r12,{r0,r1}             ; TST ORREQ
        and     r3,r2,#0xF0000
        orr     r0,r0,r3
        stmia   r5!,{r0,r1}
        b       eid_full
eid_dec
        and     r3,r2,#0xF0000
        adr     r12,t_dec_full
        ldmia   r12,{r0,r1,r12,lr}      ; ORR TST BICNE SUBS
        orr     r1,r1,r3
        orr     lr,lr,r2
        stmia   r5!,{r0,r1,r12,lr}
        adr     r12,t_dec_full + 16
        ldmia   r12,{r0,r1,r2}          ; BICPL BICVC BICNE
        stmia   r5!,{r0,r1,r2}
eid_full
        FULLF   r0,r1
        ldmfd   sp!,{pc}
eid_short
        cmp     r1,#0
        TMPLC   eq,r0,t_inc_short
        TMPLC   ne,r0,t_dec_short
        orr     r0,r0,r2
        tst     r11,#EF_FUSED
        orrne   r0,r0,#0x100000
        EMIT    r0
        ldmfd   sp!,{pc}
t_inc_full      AND     r3,r3,#PSR_C
                ADDS    r0,r0,#0x01000000       ; | register
                ORRMI   r3,r3,#PSR_S
                ORRVS   r3,r3,#PSR_V
                ORRCS   r3,r3,#PSR_Z
                TST     r0,#0x0F000000  ; | register
                ORREQ   r3,r3,#PSR_H
t_dec_full      ORR     r3,r3,#PSR_n + PSR_H + PSR_S + PSR_V + PSR_Z
                TST     r0,#0x0F000000  ; | register
                BICNE   r3,r3,#PSR_H
                SUBS    r0,r0,#0x01000000       ; | register
                BICPL   r3,r3,#PSR_S
                BICVC   r3,r3,#PSR_V
                BICNE   r3,r3,#PSR_Z
t_inc_short     ADD     r0,r0,#0x01000000       ; | register, S
t_dec_short     SUB     r0,r0,#0x01000000

;----------------------------------------------------------------------------
; Rotations, shifts and bits (value in bits 24-31 of r0, ARM C = bit out)
;----------------------------------------------------------------------------

; e_rot: r0 = operation 0-7 (RLC RRC RL RR SLA SRA SLL SRL).
e_rot
        adr     r1,er_tab
        ldr     pc,[r1,r0,lsl #2]
er_tab          DCD     er_rlc, er_rrc, er_rl, er_rr, er_sla, er_sra, er_sll, er_srl
er_rlc
        adr     r12,t_rlc
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
er_rrc
        adr     r12,t_rrc
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        mov     pc,lr
er_rl
        adr     r12,t_rl
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        mov     pc,lr
er_rr
        adr     r12,t_rr
        ldmia   r12,{r0,r1,r2,r3,r12}
        stmia   r5!,{r0,r1,r2,r3,r12}
        mov     pc,lr
er_sla
        TMPL    r0,t_sla
        EMIT    r0
        mov     pc,lr
er_sra
        adr     r12,t_sra
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
er_sll
        adr     r12,t_sll
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
er_srl
        adr     r12,t_srl
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
t_rlc           MOVS    r0,r0,LSL #1
                ORRCS   r0,r0,#0x01000000
t_rrc           MOVS    r1,r0,LSR #25
                ORRCS   r1,r1,#0x80
                MOV     r0,r1,LSL #24
t_rl            TST     r3,#PSR_C
                DCD     0x13800880      ; ORRNE r0,r0,#0x00800000 (the rotation of the C emitter)
                MOVS    r0,r0,LSL #1
t_rr            MOV     r1,r0,LSR #24
                TST     r3,#PSR_C
                ORRNE   r1,r1,#0x100
                MOVS    r1,r1,LSR #1
                MOV     r0,r1,LSL #24
t_sla           MOVS    r0,r0,LSL #1
t_sra           MOVS    r1,r0,ASR #25
                MOV     r0,r1,LSL #24
t_sll           MOVS    r0,r0,LSL #1
                ORR     r0,r0,#0x01000000
t_srl           MOVS    r1,r0,LSR #25
                MOV     r0,r1,LSL #24

; e_rot_flags: the flags of a CB rotation when they are needed: S Z P of
; r0, H = N = 0, C = ARM C.
e_rot_flags
        tst     r11,#EF_LIVE
        moveq   pc,lr
        adr     r12,t_rot_flags
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        FULLF   r0,r1
        mov     pc,lr
t_rot_flags     ADD     r2,r10,#PZST_OFF
                LDRB    r3,[r2,r0,LSR #24]
                ORRCS   r3,r3,#PSR_C

; e_bit: r0 = register, r1 = bit position in it, r2 = Z80 bit 0-7: emits
; BIT with the flags needed.
e_bit
        cmp     r1,#16
        movlo   r3,#1
        movlo   r3,r3,lsl r1            ; below 8: the bit itself
        blo     eb_bit_enc
        cmp     r1,#24
        sublo   r1,r1,#16
        movlo   r3,#0x800               ; a byte in bits 16-23
        subhs   r1,r1,#24
        movhs   r3,#0x400               ; a byte in bits 24-31
        mov     r12,#1
        orr     r3,r3,r12,lsl r1
eb_bit_enc
        TMPL    r12,t_tst_bit
        orr     r12,r12,r0,lsl #16
        orr     r12,r12,r3
        tst     r11,#EF_FUSED
        strne   r12,[r5],#4
        movne   pc,lr
        tst     r11,#EF_LIVE
        moveq   pc,lr
        adr     r3,t_bit_flags
        ldmia   r3,{r0,r1}
        mov     r3,r12
        TMPL    r12,t_orreq_zp
        stmia   r5!,{r0,r1,r3,r12}
        cmp     r2,#7
        TMPLC   eq,r0,t_orrne_s
        streq   r0,[r5],#4
        FULLF   r0,r1
        mov     pc,lr
t_tst_bit       TST     r0,#0           ; | register, bit
t_bit_flags     AND     r3,r3,#PSR_C
                ORR     r3,r3,#PSR_H
t_orreq_zp      ORREQ   r3,r3,#PSR_Z + PSR_P
t_orrne_s       ORRNE   r3,r3,#PSR_S

;----------------------------------------------------------------------------
; Control flow
;----------------------------------------------------------------------------

; e_exit: r0 = static Z80 target: emits the transfer to it from the end of
; the block: a link stub (even when the target is known: the direct branch
; must be undone if the target is evicted), or the leave glue when the
; block ends after EI.
e_exit
        stmfd   sp!,{lr}
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        ldr     r1,[r4,#B_END_LEAVE]
        cmp     r1,#0
        bne     ex_leave
        ldr     r1,[r8,#G_LINK]
        EMITB   r2,r1,BL_AL
        mov     r1,#0
        ldr     r2,[r4,#B_KEY]
        mov     r3,r5                   ; the data words
        stmia   r5!,{r0,r1,r2}
        bl      e_link
        ldmfd   sp!,{pc}
ex_leave
        mov     r1,#0
        bl      e_mov32
        TMPL    r0,t_mov_r1_0
        EMIT    r0
        ldr     r0,[r8,#G_LEAVE]
        EMITB   r1,r0,B_AL
        ldmfd   sp!,{pc}
t_mov_r1_0      MOV     r1,#0

; e_dynamic: emits the jump to the address in r0 through the lookup table
; (or the leave glue when the block ends after EI).
e_dynamic
        ldr     r1,[r4,#B_END_LEAVE]
        cmp     r1,#0
        bne     edy_leave
        adr     r12,t_dynamic
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
edy_leave
        TMPL    r0,t_mov_r1_0
        EMIT    r0
        ldr     r0,[r8,#G_LEAVE]
        EMITB   r1,r0,B_AL
        mov     pc,lr
t_dynamic       DCD     0xE28A1C40      ; ADD r1,r10,#TAB_OFF (the rotation of the C emitter)
                LDR     pc,[r1,r0,LSL #2]

; e_load_target: emits r0 = the 16-bit operand of a JP / CALL in RAM, read
; from memory (uses r0-r2 and r12 of the generated code).
e_load_target
        stmfd   sp!,{r4,lr}
        ldr     r4,[r6,#I_PC]
        ldr     r0,[r6,#I_LEN]
        add     r4,r4,r0
        sub     r4,r4,#2
        mov     r4,r4,lsl #16
        mov     r4,r4,lsr #16           ; address of the operand
        add     r0,r4,#1
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        bl      e_read_const
        TMPL    r0,t_mov_r2_lsl8
        EMIT    r0
        mov     r0,r4
        bl      e_read_const
        TMPL    r0,t_orr_r0_r2
        EMIT    r0
        ldmfd   sp!,{r4,pc}
t_mov_r2_lsl8   MOV     r2,r0,LSL #8
t_orr_r0_r2     ORR     r0,r0,r2

; e_dyn_addr: emits r12 = the 16-bit address operand of an instruction in
; RAM, read from memory, in the form of the memory sequences.
e_dyn_addr
        stmfd   sp!,{lr}
        bl      e_load_target
        TMPL    r0,t_mov_r12_lsl16
        EMIT    r0
        ldmfd   sp!,{pc}
t_mov_r12_lsl16 MOV     r12,r0,LSL #16

; e_dyn_exit: the transfer of a JP / CALL whose target is read when it runs.
e_dyn_exit
        stmfd   sp!,{lr}
        bl      e_load_target
        bl      e_dynamic
        ldmfd   sp!,{pc}

; e_cond: r0 = Z80 condition 0-7 -> r0 = the ARM condition (in bits 28-31)
; that holds when it is true: from the ARM flags of a fused producer (the
; previous instruction), or after a TST emitted here. Uses r1, r2.
e_cond
        cmp     r7,#0
        ldrne   r1,[r6,#I_FUSED - I_SIZE]
        moveq   r1,#0
        and     r2,r0,#1                ; 1: flag set is true
        tst     r1,#FU_Z_EQ
        bne     ec_eq
        tst     r1,#FU_Z_CS + FU_C_CS
        bne     ec_cs
        tst     r1,#FU_C_CC
        bne     ec_cc
        tst     r1,#FU_V_VS
        bne     ec_vs
        tst     r1,#FU_S_MI
        bne     ec_mi
        adr     r1,ec_mask
        ldrb    r1,[r1,r0]
        TMPL    r0,t_tst_f
        orr     r0,r0,r1
        EMIT    r0
        cmp     r2,#0
        moveq   r0,#CC_EQ
        movne   r0,#CC_NE
        mov     pc,lr
ec_eq
        cmp     r2,#0
        moveq   r0,#CC_NE
        movne   r0,#CC_EQ
        mov     pc,lr
ec_cs
        cmp     r2,#0
        moveq   r0,#CC_CC
        movne   r0,#CC_CS
        mov     pc,lr
ec_cc
        cmp     r2,#0
        moveq   r0,#CC_CS
        movne   r0,#CC_CC
        mov     pc,lr
ec_vs
        cmp     r2,#0
        moveq   r0,#CC_VC
        movne   r0,#CC_VS
        mov     pc,lr
ec_mi
        cmp     r2,#0
        moveq   r0,#CC_PL
        movne   r0,#CC_MI
        mov     pc,lr
ec_mask         DCB     PSR_Z, PSR_Z, PSR_C, PSR_C, PSR_V, PSR_V, PSR_S, PSR_S
t_tst_f         TST     r3,#0           ; | flag

; e_idle: r0 = condition: emits the jump back of a busy-wait loop, under
; that condition: the T-states left in the stretch are counted as idle
; and dropped, so that the segment check at the loop head ends the stretch.
e_idle
        adr     r12,t_idle
        ldmia   r12,{r1,r2,r3,r12}
        orr     r1,r1,r0
        orr     r2,r2,r0
        orr     r3,r3,r0
        orr     r12,r12,r0
        stmia   r5!,{r1,r2,r3,r12}
        mov     pc,lr
t_idle          LDREQ   r0,[r10,#GOFF_IDLE]     ; | condition
                ADDEQ   r0,r0,r8
                STREQ   r0,[r10,#GOFF_IDLE]
                MOVEQ   r8,#0

; e_line_idle: emits the jump back of a loop waiting for a line: the time
; up to the next line boundary goes by idle (the remainder of the T-states
; left by the line length, taken with the multiplication of T_LINE).
e_line_idle
        stmfd   sp!,{lr}
        adr     r12,t_line_idle
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_line_idle + 24
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_line_idle + 48
        ldmia   r12,{r0-r3,r12}
        stmia   r5!,{r0-r3,r12}
        ldmfd   sp!,{pc}
t_line_idle     MOV     r0,r8,ASR #8
                MOV     r1,r0,LSR #2
                MOV     r2,#0x8F00
                ORR     r2,r2,#0xB9
                MUL     r1,r2,r1
                MOV     r1,r1,LSR #21
                MOV     r2,#228
                MUL     r2,r1,r2
                SUBS    r0,r0,r2
                MOVEQ   r0,#228
                CMP     r0,r8,ASR #8
                MOVHI   r0,r8,ASR #8
                MOV     r0,r0,LSL #8
                SUB     r8,r8,r0
                LDR     r1,[r10,#GOFF_IDLE]
                ADD     r1,r1,r0
                STR     r1,[r10,#GOFF_IDLE]

; e_jump_cond: r0 = ARM condition, r1 = extra T-states of the taken path:
; emits the taken path of the conditional jump of the instruction: the
; idle time of a busy-wait loop, or the T-states, then the branch (to the
; code of a target behind, a stub for one ahead, a dynamic target or the
; link stub of an outside target).
e_jump_cond
        stmfd   sp!,{r0,lr}             ; the condition is kept on the stack
        ldr     r2,[r6,#I_BUSY]
        cmp     r2,#2
        beq     ejc_line
        cmp     r2,#0
        bne     ejc_busy
        cmp     r1,#0
        beq     ejc_target
        TMPL    r0,t_sub_cyc_cc
        ldr     r2,[sp]
        orr     r0,r0,r2
        mov     r1,r1,lsl #8
        bl      e_dpi
ejc_target
        ldr     r0,[r6,#I_INTERNAL]
        cmp     r0,#0
        blt     ejc_outside
        cmp     r0,r7
        bhi     ejc_forward
        bl      ejc_host                ; a target behind: a direct branch
        ldr     r1,[sp]
        b       ejc_branch
ejc_outside
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        movne   r0,#STUB_DYN
        moveq   r0,#STUB_LINK
        bl      new_stub
        ldr     r1,[r6,#I_DYN]
        cmp     r1,#0
        strne   r7,[r0,#ST_INDEX]
        ldreq   r1,[r6,#I_TARGET]
        streq   r1,[r0,#ST_PC]
        b       ejc_place
ejc_forward
        mov     r0,#STUB_FORWARD
        bl      new_stub
        ldr     r1,[r6,#I_INTERNAL]
        str     r1,[r0,#ST_INDEX]
ejc_place
        ldr     r0,[sp]
        orr     r0,r0,#B_EQ
        EMIT    r0
        b       ejc_done
ejc_line
        ; A loop waiting for a line: the idle time is taken under the
        ; condition by skipping it, then the branch back.
        ldr     r0,[sp]
        cmp     r0,#CC_AL
        moveq   r2,#0
        beq     ejc_line_idle
        mov     r2,r5                   ; the skip branch
        eor     r0,r0,#0x10000000
        orr     r0,r0,#B_EQ
        EMIT    r0
ejc_line_idle
        stmfd   sp!,{r2}
        bl      e_line_idle
        bl      ejc_host
        EMITB   r1,r0,B_AL
        ldmfd   sp!,{r2}
        cmp     r2,#0
        beq     ejc_done
        mov     r1,r5
        PATCHB  r2,r1,r0
        b       ejc_done
ejc_busy
        ldr     r0,[sp]
        bl      e_idle
        bl      ejc_host
        ldr     r1,[sp]
ejc_branch
        sub     r0,r0,r5
        sub     r0,r0,#8
        mov     r0,r0,lsl #6
        mov     r0,r0,lsr #8
        orr     r0,r0,r1
        orr     r0,r0,#B_EQ
        EMIT    r0
ejc_done
        ldmfd   sp!,{r0,pc}
; r0 = the code of the internal target of the instruction.
ejc_host
        ldr     r0,[r6,#I_INTERNAL]
        ldr     r1,[r4,#B_INS]
        RECORD  r2,r1,r0
        ldr     r0,[r2,#I_HOST]
        mov     pc,lr
t_sub_cyc_cc    SUBEQ   r8,r8,#0        ; | condition, T-states * CYCLE

; e_leave_check: emits the leave of the block when the machine asked for
; it (r0 != 0), through a leave stub with the address of the next
; instruction.
e_leave_check
        stmfd   sp!,{lr}
        TMPL    r0,t_teq_r0
        EMIT    r0
        mov     r0,#STUB_LEAVE
        bl      new_stub
        ldr     r1,[r6,#I_PC]
        ldr     r2,[r6,#I_LEN]
        add     r1,r1,r2
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16
        ldr     r2,[r6,#I_T_AFTER]
        str     r1,[r0,#ST_PC]
        str     r2,[r0,#ST_T]
        mov     r0,#B_NE
        EMIT    r0
        ldmfd   sp!,{pc}
t_teq_r0        TEQ     r0,#0

; e_port_c: emits r1 = port C.
e_port_c
        adr     r12,t_port_c
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     pc,lr
t_port_c        MOV     r1,r5,LSR #16
                AND     r1,r1,#0xFF

; e_time_left: emits r2 = T-states (times CYCLE) left in the scanline at
; the end of the instruction (the segment check has already taken the
; rest of the segment).
e_time_left
        ldr     r1,[r6,#I_T_AFTER]
        cmp     r1,#0
        TMPLC   eq,r0,t_mov_r2_cyc
        TMPLC   ne,r0,t_add_r2_cyc
        orrne   r0,r0,#0xC00
        orrne   r0,r0,r1                ; (always below 256: a byte in bits 8-15)
        EMIT    r0
        mov     pc,lr
t_mov_r2_cyc    MOV     r2,r8
t_add_r2_cyc    ADD     r2,r8,#0        ; | T-states * CYCLE

;----------------------------------------------------------------------------
; ED instructions
;----------------------------------------------------------------------------

; e_out_loop: r0 = record of the last instruction of the loop (tail),
; r1 = T-states of a pass: emits the fast path of a port output loop
; (OTIR, OTDR, or OUTI / OUTD closed by JR NZ or JP NZ back to it) at its
; head, after the segment check: when the stretch has room for the B - 1
; further passes, the B bytes go out in one run (B = 0: 256) and B, HL,
; the flags and the T-states end as after the last pass; the bytes are
; spaced as in a run of OUTI. Returns r0 = the site of the branch past the
; loop, to be patched; the regular code of one pass follows.
e_out_loop
        stmfd   sp!,{r7,r9,lr}
        mov     r9,r0
        mov     r7,r1
        adr     r12,t_ol1
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        TMPL    r0,t2_mov_r1_imm
        mov     r1,r7,lsl #8
        bl      e_dpi
        adr     r12,t_ol2
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r7,r5                   ; the branch to the regular code
        mov     r0,#B_CC
        EMIT    r0
        TMPL    r0,t_sub_r8_r2
        EMIT    r0
        ldr     r0,[r6,#I_OP]
        tst     r0,#8
        TMPLC   ne,r0,t_rsb_r0
        strne   r0,[r5],#4              ; OUTD: a negative count
        bl      e_port_c
        bl      e_time_left
        ldr     r0,[r8,#G_IO_OUTN]
        EMITB   r1,r0,BL_AL
        adr     r12,t_ol3
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        ldr     r0,[r6,#I_OP]
        tst     r0,#8
        TMPLC   eq,r0,t_add_hl_r0
        TMPLC   ne,r0,t_sub_hl_r0
        adr     r12,t_ol4
        ldmia   r12,{r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        mov     r0,#STUB_LEAVE
        bl      new_stub
        ldr     r1,[r9,#I_PC]
        ldr     r2,[r9,#I_LEN]
        add     r1,r1,r2
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16
        ldr     r2,[r9,#I_T_AFTER]
        str     r1,[r0,#ST_PC]
        str     r2,[r0,#ST_T]
        mov     r0,#B_NE
        EMIT    r0
        mov     r9,r5                   ; the branch past the loop
        mov     r0,#B_AL
        EMIT    r0
        mov     r1,r5
        PATCHB  r7,r1,r0
        mov     r0,r9
        ldmfd   sp!,{r7,r9,pc}
t_ol1           MOVS    r0,r5,LSR #24
                MOVEQ   r0,#256
                SUB     r12,r0,#1
t_ol2           MUL     r2,r1,r12
                CMP     r8,r2
t_sub_r8_r2     SUB     r8,r8,r2
t_rsb_r0        RSB     r0,r0,#0
t_ol3           MOV     r12,r0
                MOVS    r0,r5,LSR #24
                MOVEQ   r0,#256
t_add_hl_r0     ADD     r7,r7,r0,LSL #16
t_sub_hl_r0     SUB     r7,r7,r0,LSL #16
t_ol4           BIC     r5,r5,#0xFF000000
                ORR     r3,r3,#PSR_Z + PSR_n
                TEQ     r12,#0

; e_copy_loop: emits the fast path of LDIR / LDDR at its head, after the
; segment check: when the stretch has room for the BC - 1 further passes,
; the glue copies the bytes page by page and the T-states are those of
; the passes made; it stops before a destination page with a special
; write, and the block goes back to the head, where that byte gets the
; regular code of one pass. Returns r0 = the site of the branch past the
; loop, to be patched; the regular code of one pass follows.
e_copy_loop
        stmfd   sp!,{r4,r7,r9,lr}
        ldr     r9,[r6,#I_SEG_T]
        add     r9,r9,#5                ; T-states of a pass
        adr     r12,t_cl1
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        TMPL    r0,t2_mov_r1_imm
        mov     r1,r9,lsl #8
        bl      e_dpi
        adr     r12,t_ol2
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r7,r5                   ; the branch to the regular code
        mov     r0,#B_CC
        EMIT    r0
        ldr     r0,[r6,#I_OP]
        tst     r0,#8
        ldreq   r0,[r8,#G_LDIR]
        ldrne   r0,[r8,#G_LDDR]
        EMITB   r1,r0,BL_AL
        TMPL    r0,t_movs_r1_r0
        EMIT    r0
        mov     r4,r5                   ; the second branch to the regular code
        mov     r0,#B_EQ
        EMIT    r0
        TMPL    r0,t_mov_r2_imm
        mov     r1,r9,lsl #8
        bl      e_dpi
        adr     r12,t_cl2
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        TMPL    r0,t_addeq_r8
        mov     r1,r9,lsl #8
        bl      e_dpi
        TMPL    r0,t_addne_r8
        ldr     r1,[r6,#I_SEG_T]
        mov     r1,r1,lsl #8
        bl      e_dpi
        ldr     r0,[r6,#I_HOST]
        EMITB   r1,r0,B_NE
        tst     r11,#EF_LIVE
        TMPLC   ne,r0,t_bic_hnv
        strne   r0,[r5],#4
        mov     r9,r5                   ; the branch past the loop
        mov     r0,#B_AL
        EMIT    r0
        mov     r1,r5
        PATCHB  r7,r1,r0
        mov     r1,r5
        PATCHB  r4,r1,r0
        mov     r0,r9
        ldmfd   sp!,{r4,r7,r9,pc}
t_cl1           MOVS    r0,r5,LSR #16
                MOVEQ   r0,#0x10000
                SUB     r12,r0,#1
t_movs_r1_r0    MOVS    r1,r0
t_mov_r2_imm    MOV     r2,#0           ; | T-states * CYCLE
t_cl2           MUL     r12,r2,r1
                SUB     r8,r8,r12
                MOVS    r1,r5,LSR #16
t_addeq_r8      ADDEQ   r8,r8,#0        ; | T-states * CYCLE
t_addne_r8      ADDNE   r8,r8,#0
t_bic_hnv       BIC     r3,r3,#PSR_H + PSR_n + PSR_V

; e_block_insn: emits a block instruction (LDI, CPI, INI, OUTI and their
; decrementing and repeated forms).
e_block_insn
        stmfd   sp!,{r7,r9,lr}
        mov     r0,#0
        stmfd   sp!,{r0}                ; the branch past a fast path
        and     r0,r9,#7                ; z
        and     r1,r9,#0x38
        mov     r1,r1,lsr #3            ; y
        and     r9,r1,#1                ; bit 0: decrementing
        cmp     r1,#6
        orrhs   r9,r9,#2                ; bit 1: repeated
        cmp     r0,#1
        beq     ebi_cp
        bhi     ebi_io
        ; LDI LDD LDIR LDDR
        tst     r9,#2
        beq     ebi_ld_pass
        bl      e_copy_loop
        str     r0,[sp]
ebi_ld_pass
        mov     r0,#RHL
        bl      e_read
        mov     r0,#RDE
        bl      e_write
        tst     r9,#1
        adr     r12,t_ld_step
        addne   r12,r12,#12
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        tst     r11,#EF_LIVE
        beq     ebi_ld_rep
        adr     r12,t_ld_flags
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
ebi_ld_rep
        tst     r9,#2
        beq     ebi_done
        TMPL    r0,t_teq_bc
        EMIT    r0
        mov     r0,#CC_NE
        mov     r1,#5
        bl      e_jump_cond
        b       ebi_patch
ebi_cp
        ; CPI CPD CPIR CPDR
        mov     r0,#RHL
        bl      e_read
        adr     r12,t_cp1
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_cp1 + 24
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        tst     r9,#1
        TMPLC   eq,r0,t_inc_hl
        TMPLC   ne,r0,t_dec_hl
        adr     r12,t_cp2
        ldmia   r12,{r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        tst     r9,#2
        beq     ebi_done
        adr     r12,t_cp_rep
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        mov     r0,#CC_NE
        mov     r1,#5
        bl      e_jump_cond
        b       ebi_done
ebi_io
        cmp     r0,#2
        bne     ebi_out
        ; INI IND INIR INDR
        bl      e_port_c
        bl      e_time_left
        ldr     r0,[r8,#G_IO_IN]
        EMITB   r1,r0,BL_AL
        mov     r0,#RHL
        bl      e_write
        tst     r9,#1
        TMPLC   eq,r0,t_inc_hl
        TMPLC   ne,r0,t_dec_hl
        TMPL    r1,t_sub_b
        stmia   r5!,{r0,r1}
        b       ebi_io_flags
ebi_out
        ; OUTI OUTD OTIR OTDR
        tst     r9,#2
        beq     ebi_out_pass
        mov     r0,r6
        ldr     r1,[r6,#I_SEG_T]
        add     r1,r1,#5
        bl      e_out_loop
        str     r0,[sp]
ebi_out_pass
        mov     r0,#RHL
        bl      e_read
        TMPL    r0,t_sub_b
        EMIT    r0
        bl      e_port_c
        bl      e_time_left
        ldr     r0,[r8,#G_IO_OUT]
        EMITB   r1,r0,BL_AL
        tst     r9,#1
        TMPLC   eq,r0,t_inc_hl
        TMPLC   ne,r0,t_dec_hl
        EMIT    r0
ebi_io_flags
        tst     r11,#EF_LIVE
        bne     ebi_io_flags2
        tst     r9,#2
        beq     ebi_done
ebi_io_flags2
        adr     r12,t_io_flags
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        tst     r9,#2
        beq     ebi_done
        mov     r0,#CC_NE
        mov     r1,#5
        bl      e_jump_cond
ebi_patch
        ldr     r2,[sp]
        cmp     r2,#0
        beq     ebi_done
        mov     r1,r5
        PATCHB  r2,r1,r0
ebi_done
        add     sp,sp,#4
        ldmfd   sp!,{r7,r9,pc}
t_ld_step       ADD     r7,r7,#0x10000
                ADD     r6,r6,#0x10000
                SUB     r5,r5,#0x10000
                SUB     r7,r7,#0x10000
                SUB     r6,r6,#0x10000
                SUB     r5,r5,#0x10000
t_ld_flags      BIC     r3,r3,#PSR_H + PSR_n + PSR_V
                TEQ     r5,#0
                ORRNE   r3,r3,#PSR_V
t_teq_bc        TEQ     r5,#0
t_cp1           MOV     r1,r4,LSL #4
                CMP     r4,r0,LSL #24
                AND     r3,r3,#PSR_C
                ORR     r3,r3,#PSR_n
                ORRMI   r3,r3,#PSR_S
                ORREQ   r3,r3,#PSR_Z
                CMP     r1,r0,LSL #28
                ORRCC   r3,r3,#PSR_H
t_inc_hl        ADD     r7,r7,#0x10000
t_dec_hl        SUB     r7,r7,#0x10000
t_cp2           SUB     r5,r5,#0x10000
                TEQ     r5,#0
                ORRNE   r3,r3,#PSR_V
t_cp_rep        TST     r3,#PSR_Z
                BNE     t_cp_rep + 20   ; found: past the jump back
                TEQ     r5,#0
t_sub_b         SUB     r5,r5,#0x01000000
t_io_flags      BIC     r3,r3,#PSR_Z
                ORR     r3,r3,#PSR_n
                TST     r5,#0xFF000000
                ORREQ   r3,r3,#PSR_Z

; e_out_run: emits a run of port outputs to port C in one call. A run of
; OUTI (OUTD): B decreases by the run length, the bytes go out, then HL
; moves. A mixed run (OUTI and OUT (C),r) passes its description and the
; byte of r in r0; B and HL move by its OUTI only. The flags are those of
; the last OUTI, and the block is left after the run when a handler asked
; for it.
e_out_run
        stmfd   sp!,{r6,r7,r9,r11,lr}
        ldr     r0,[r6,#I_RUN]
        and     r7,r0,#0xFF             ; n
        mov     r9,r0,lsl #8
        mov     r9,r9,lsr #16           ; mask
        sub     r0,r7,#1
        RECORD  r11,r6,r0               ; the last element
        ; nmem: the elements that are OUTI (or OUTD).
        mov     r0,r7
        mov     r1,r9
        mov     r2,r7
eor_count
        tst     r1,#1
        subne   r0,r0,#1
        mov     r1,r1,lsr #1
        subs    r2,r2,#1
        bne     eor_count
        stmfd   sp!,{r0}                ; nmem
        cmp     r0,#0
        TMPLC   ne,r1,t_sub_b_n
        orrne   r1,r1,r0
        strne   r1,[r5],#4
        cmp     r9,#0
        bne     eor_mixed
        bl      e_port_c
        ldr     r0,[r11,#I_OP]
        cmp     r0,#0xAB
        TMPLC   eq,r0,t_mvn_r0_imm
        subeq   r1,r7,#1
        TMPLC   ne,r0,t2_mov_r0_imm
        movne   r1,r7
        orr     r0,r0,r1
        EMIT    r0
        mov     r6,r11                  ; the time left after the last element
        bl      e_time_left
        b       eor_call
eor_mixed
        mov     r0,#RUN_MIXED
        orr     r0,r0,r9,lsl #13
        orr     r0,r0,r7,lsl #8
        mov     r1,#0
        bl      e_mov32
        ldr     r0,[r6,#I_RUN]
        mov     r0,r0,lsr #24           ; the OUT (C),r opcode
        cmp     r0,#0x79
        TMPLC   eq,r0,t_orr_r0_a
        streq   r0,[r5],#4
        beq     eor_mixed_port
        cmp     r0,#0x51
        TMPLC   eq,r0,t_orr_r0_d
        streq   r0,[r5],#4
        beq     eor_mixed_port
        adr     r12,t_orr_r0_e
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
eor_mixed_port
        bl      e_port_c
        bl      e_time_left
eor_call
        ldr     r0,[r8,#G_IO_OUTN]
        EMITB   r1,r0,BL_AL
        ldr     r1,[sp]                 ; nmem
        cmp     r1,#0
        beq     eor_leave
        ldr     r0,[r11,#I_OP]
        cmp     r0,#0xAB
        TMPLC   eq,r0,t_sub_hl_n
        TMPLC   ne,r0,t_add_hl_n
        orr     r0,r0,r1
        EMIT    r0
        ldr     r0,[r11,#I_DEF]
        ldr     r1,[r11,#I_LIVE_AFTER]
        tst     r0,r1
        beq     eor_leave
        adr     r12,t_io_flags
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
eor_leave
        mov     r6,r11                  ; the leave check of the last element
        bl      e_leave_check
        add     sp,sp,#4
        ldmfd   sp!,{r6,r7,r9,r11,pc}
t_sub_b_n       DCD     0xE2455400      ; SUB r5,r5,#nmem << 24 | nmem
t_mvn_r0_imm    MVN     r0,#0           ; | n - 1
t_orr_r0_a      ORR     r0,r0,r4,LSR #24
t_orr_r0_d      ORR     r0,r0,r6,LSR #24
t_orr_r0_e      MOV     r2,r6,LSL #8
                ORR     r0,r0,r2,LSR #24
t_sub_hl_n      DCD     0xE2477800      ; SUB r7,r7,#nmem << 16 | nmem
t_add_hl_n      DCD     0xE2877800      ; ADD r7,r7,#nmem << 16

; emit_ed: the ED instruction of the record.
; Copies of shared templates within reach of the literal loads below.
t2_mov_r1_imm   MOV     r1,#0           ; | immediate
t2_mov_r0_imm   MOV     r0,#0           ; | immediate
t2_mov_r0_lsr24 MOV     r0,r0,LSR #24   ; | register
t2_orrcc_h      ORRCC   r3,r3,#PSR_H

emit_ed
        ldr     r0,[r6,#I_RUN]
        cmp     r0,#RUN_LOOP
        beq     eed_loop
        cmp     r0,#0
        beq     eed_plain
        cmp     r0,#RUN_PART
        beq     eb_done                 ; a part of a run: nothing
        tst     r0,#RUN_GRP
        bne     eed_grp
        bl      e_out_run
        b       eb_done
eed_grp
        ; OUTI / OUTD in a group of outputs (jit_out_groups): the byte is
        ; stored, or the group sent at the last one; B, HL and the flags
        ; as for a single OUTI. Right after an instruction that left the
        ; byte at (HL) in r0 of the generated code without changing HL
        ; (LD r,(HL) to A, B, C, D or E; AND, XOR or OR (HL)), with no
        ; cycle check or interrupt check between them, that byte is taken
        ; instead of a second read.
        cmp     r7,#0
        beq     eg_read
        ldr     r0,[r6,#I_SEG_START]
        sub     r12,r6,#I_SIZE          ; the previous instruction
        ldr     r1,[r12,#I_PRE]
        ldr     r2,[r12,#I_IRQ_CHECK]
        orr     r0,r0,r1
        orrs    r0,r0,r2
        bne     eg_read
        ldr     r0,[r12,#I_OP]
        mov     r1,r0,lsr #3
        and     r1,r1,#7                ; y
        and     r2,r0,#0xC7
        cmp     r2,#0x46
        moveq   r2,#0x8F                ; LD r,(HL): B, C, D, E, A
        beq     eg_y
        cmp     r2,#0x86
        bne     eg_read
        mov     r2,#0x70                ; AND, XOR, OR (HL)
eg_y
        mov     r0,r2,lsr r1
        tst     r0,#1
        bne     eg_kept
eg_read
        mov     r0,#RHL
        bl      e_read
eg_kept
        TMPL    r0,t_sub_b
        EMIT    r0
        bl      e_grp_out
        tst     r9,#0x08                ; OUTD
        TMPLC   eq,r0,t_inc_hl
        TMPLC   ne,r0,t_dec_hl
        EMIT    r0
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_io_flags
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        b       eb_done

eed_loop
        ; The JR NZ / JP NZ that follows closes the loop: the fast path
        ; goes past it (patched by the block loop).
        ldr     r0,[r6,#I_KIND + I_SIZE]
        ldr     r1,[r6,#I_SEG_T]
        cmp     r0,#K_JRCC
        addeq   r1,r1,#5
        add     r0,r6,#I_SIZE
        bl      e_out_loop
        str     r0,[r4,#B_SKIP_SITE]
        add     r0,r7,#2                ; the block loop patches it there
        ldr     r1,[r4,#B_N]
        cmp     r0,r1
        movhs   r0,r1                   ; (or at the end of the instructions)
        str     r0,[r4,#B_SKIP_TO]
        bl      e_block_insn
        b       eb_done
eed_plain
        mov     r0,r9,lsr #6            ; x
        cmp     r0,#2
        beq     eed_block
        cmp     r0,#1
        bne     eb_done                 ; NOP
        and     r0,r9,#7
        adr     r1,eed_tab
        ldr     pc,[r1,r0,lsl #2]
eed_block
        and     r0,r9,#7
        cmp     r0,#3
        bhi     eb_done
        tst     r9,#0x20                ; y >= 4
        beq     eb_done
        bl      e_block_insn
        b       eb_done
eed_tab         DCD     eed_in, eed_out, eed_sbcadc, eed_ldnn, eed_neg, eed_retn, eed_im, eed_misc

eed_in                                  ; IN r,(C)
        bl      e_port_c
        bl      e_time_left
        ldr     r0,[r8,#G_IO_IN]
        EMITB   r1,r0,BL_AL
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3            ; y
        cmp     r0,#6
        movne   r1,#0xC00               ; r0 of the generated code, shifted by 24
        movne   r2,#0
        blne    e_set8
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_in_flags
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        b       eb_done
t_in_flags      ADD     r1,r10,#PZST_OFF
                LDRB    r1,[r1,r0]
                AND     r3,r3,#PSR_C
                ORR     r3,r3,r1

eed_out                                 ; OUT (C),r
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_get_low
        bl      e_port_c
        bl      e_time_left
        ldr     r0,[r8,#G_IO_OUT]
        EMITB   r1,r0,BL_AL
        bl      e_leave_check
        b       eb_done

eed_sbcadc                              ; SBC / ADC HL,rr
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4            ; p
        mov     r1,#2
        bl      e_rp_reg
        mov     r12,r0                  ; rr
        and     r2,r11,#EF_LIVE + EF_FUSED
        tst     r9,#8
        bne     eed_adc
        cmp     r2,#EF_LIVE
        bne     eed_sbc_s
        adr     r2,t_sbc_hl
        ldmia   r2,{r0,r1,r2,r3}        ; AND SUBS ORR(rr) MOV
        orr     r2,r2,r12,lsl #16
        stmia   r5!,{r0-r3}
        adr     r12,t_sbc_hl + 16
        ldmia   r12,{r0-r3,r12}         ; SBCS MRS MOV EOR CMP
        stmia   r5!,{r0-r3,r12}
        TMPL    r0,t2_orrcc_h
        EMIT    r0
        FULLF   r0,r1
        b       eb_done
eed_sbc_s
        adr     r2,t_sbc_hl_s
        ldmia   r2,{r0,r1,r2,r3}        ; AND SUBS ORR(rr) SBC
        orr     r2,r2,r12,lsl #16
        tst     r11,#EF_FUSED
        orrne   r3,r3,#0x100000
        stmia   r5!,{r0-r3}
        b       eb_done
eed_adc
        adr     r2,t_adc_hl
        ldmia   r2,{r0,r1,r2,r3}        ; MOVS MOV(rr) ORRCS ORRCS
        orr     r1,r1,r12
        stmia   r5!,{r0-r3}
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     eed_adc_s
        adr     r2,t_adc_hl + 16
        ldmia   r2,{r0,r1,r2,r3}        ; EOR(rr) ADCS MRS MOV
        orr     r0,r0,r12
        stmia   r5!,{r0-r3}
        adr     r12,t_adc_hl + 32
        ldmia   r12,{r0,r1,r2}          ; EOR TST ORRNE
        stmia   r5!,{r0,r1,r2}
        FULLF   r0,r1
        b       eb_done
eed_adc_s
        TMPL    r0,t_adc_hl_s
        tst     r11,#EF_FUSED
        orrne   r0,r0,#0x100000
        EMIT    r0
        b       eb_done
t_sbc_hl        AND     r3,r3,#PSR_C
                SUBS    r1,r3,r3,LSL #1
                ORR     r0,r0,r1,LSR #16        ; | rr
                MOV     r1,r7,LSL #4
                SBCS    r7,r7,r0
                MRS     r3,CPSR
                MOV     r3,r3,LSR #28
                EOR     r3,r3,#PSR_C + PSR_n
                CMP     r1,r0,LSL #4
t_sbc_hl_s      AND     r1,r3,#PSR_C
                SUBS    r1,r1,r1,LSL #1
                ORR     r0,r0,r1,LSR #16        ; | rr
                SBC     r7,r7,r0        ; | S
t_adc_hl        MOVS    r1,r3,LSR #2
                MOV     r0,r0           ; | rr
                ORRCS   r0,r0,#0xFF00
                ORRCS   r0,r0,#0xFF
                EOR     r2,r7,r0        ; | rr
                ADCS    r7,r7,r0
                MRS     r3,CPSR
                MOV     r3,r3,LSR #28
                EOR     r2,r2,r7
                DCD     0xE3120410      ; TST r2,#0x10000000 (the rotation of the C emitter)
                ORRNE   r3,r3,#PSR_H
t_adc_hl_s      ADC     r7,r7,r0        ; | S

eed_ldnn                                ; LD (nn),rr / LD rr,(nn)
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4            ; p
        tst     r9,#8
        bne     eed_ldnn_rd
        mov     r1,#2
        bl      e_rp_reg
        TMPL    r1,t_mov_r0_lsr16
        orr     r1,r1,r0
        EMIT    r1
        ldr     r0,[r6,#I_N]
        bl      e_write_const
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4
        mov     r1,#2
        bl      e_rp_reg
        TMPL    r1,t2_mov_r0_lsr24
        orr     r1,r1,r0
        EMIT    r1
        ldr     r0,[r6,#I_N]
        add     r0,r0,#1
        bl      e_write_const
        b       eb_done
eed_ldnn_rd
        ldr     r0,[r6,#I_N]
        bl      e_read_const
        TMPL    r0,t_mov_r2_r0
        EMIT    r0
        ldr     r0,[r6,#I_N]
        add     r0,r0,#1
        bl      e_read_const
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4
        adr     r1,eed_rr_tab
        ldrb    r0,[r1,r0]
        mov     r1,#0                   ; the high byte in r0
        bl      e_pair_lo_hi
        b       eb_done
eed_rr_tab      DCB     RBC, RDE, RHL, RSP
t_mov_r0_lsr16  MOV     r0,r0,LSR #16   ; | register
t_mov_r2_r0     MOV     r2,r0

; e_pair_lo_hi: emits register r0 = r2 (low byte) and register r1 (high
; byte) as a pair: MOV rr,r2,LSL #16; ORR rr,rr,rh,LSL #24.
e_pair_lo_hi
        adr     r12,t_pair_lo_hi
        ldmia   r12,{r2,r3}
        orr     r2,r2,r0,lsl #12
        orr     r3,r3,r0,lsl #12
        orr     r3,r3,r0,lsl #16
        orr     r3,r3,r1
        stmia   r5!,{r2,r3}
        mov     pc,lr
t_pair_lo_hi    MOV     r0,r2,LSL #16   ; | register
                ORR     r0,r0,r0,LSL #24        ; | register, register, high byte register

eed_neg                                 ; NEG
        and     r2,r11,#EF_LIVE + EF_FUSED
        cmp     r2,#EF_LIVE
        bne     eed_neg_s
        adr     r12,t_neg
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        TMPL    r0,t_orrne_h
        EMIT    r0
        FULLF   r0,r1
        b       eb_done
eed_neg_s
        TMPL    r0,t_rsb_a
        tst     r11,#EF_FUSED
        orrne   r0,r0,#0x100000
        EMIT    r0
        b       eb_done
t_neg           MOV     r1,r4,LSL #4
                RSBS    r4,r4,#0
                MRS     r3,CPSR
                MOV     r3,r3,LSR #28
                EOR     r3,r3,#PSR_C + PSR_n
                TEQ     r1,#0
t_orrne_h       ORRNE   r3,r3,#PSR_H
t_rsb_a         RSB     r4,r4,#0        ; | S

eed_retn                                ; RETN / RETI
        adr     r12,t_retn
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        bl      e_pop
        adr     r12,t_ret_addr
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        ldr     r0,[r8,#G_LEAVE]
        EMITB   r1,r0,B_AL
        b       eb_done
t_retn          LDR     r0,[r10,#GOFF_IFF2]
                STR     r0,[r10,#GOFF_IFF1]
t_ret_addr      ORR     r0,r2,r1,LSL #8
                MOV     r1,#0

eed_im                                  ; IM
        and     r0,r9,#0x18
        cmp     r0,#0x10
        moveq   r1,#1
        movhi   r1,#2
        movlo   r1,#0
        TMPL    r0,t2_mov_r0_imm
        orr     r0,r0,r1
        TMPL    r1,t_str_im
        stmia   r5!,{r0,r1}
        b       eb_done
t_str_im        STR     r0,[r10,#GOFF_IM]

eed_misc
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3            ; y
        cmp     r0,#2
        blo     eed_ld_ir               ; LD I,A / LD R,A
        cmp     r0,#4
        blo     eed_ld_air              ; LD A,I / LD A,R
        cmp     r0,#6
        bhs     eb_done                 ; NOP
        ; RRD / RLD
        mov     r9,r0                   ; y (the opcode is not needed any more)
        mov     r0,#RHL
        bl      e_read
        TMPL    r0,t_mov_r1_a24
        EMIT    r0
        cmp     r9,#5
        bne     eed_rrd
        adr     r12,t_rld
        ldmia   r12,{r0-r3}
        stmia   r5!,{r0-r3}
        b       eed_rxd_end
eed_rrd
        adr     r12,t_rrd
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
eed_rxd_end
        adr     r12,t_rxd_end
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        tst     r11,#EF_LIVE
        beq     eed_rxd_write
        adr     r12,t_rxd_flags
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
eed_rxd_write
        mov     r0,#RHL
        bl      e_write
        b       eb_done
eed_ld_ir
        TMPL    r1,t_mov_r0_a24
        cmp     r0,#0
        TMPLC   eq,r2,t_str_i
        TMPLC   ne,r2,t_str_r
        stmia   r5!,{r1,r2}
        b       eb_done
eed_ld_air
        cmp     r0,#2
        TMPLC   eq,r0,t_ldr_i
        streq   r0,[r5],#4
        beq     eed_ld_air2
        ; R is not counted per instruction: a changing value is derived
        ; from the scanline and the cycle counter.
        adr     r12,t_ld_a_r
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_ld_a_r + 24
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
eed_ld_air2
        TMPL    r0,t_mov_a_r0_24
        EMIT    r0
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_air_flags
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        adr     r12,t_air_flags + 24
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        b       eb_done
t_mov_r1_a24    MOV     r1,r4,LSR #24
t_rld           AND     r2,r1,#0x0F
                ORR     r2,r2,r0,LSL #4
                AND     r1,r1,#0xF0
                ORR     r1,r1,r0,LSR #4
t_rrd           AND     r2,r1,#0x0F
                MOV     r2,r2,LSL #4
                ORR     r2,r2,r0,LSR #4
                AND     r1,r1,#0xF0
                AND     r0,r0,#0x0F
                ORR     r1,r1,r0
t_rxd_end       MOV     r4,r1,LSL #24
                AND     r0,r2,#0xFF
t_rxd_flags     ADD     r1,r10,#PZST_OFF
                LDRB    r1,[r1,r4,LSR #24]
                AND     r3,r3,#PSR_C
                ORR     r3,r3,r1
t_mov_r0_a24    MOV     r0,r4,LSR #24
t_str_i         STR     r0,[r10,#GOFF_I]
t_str_r         STR     r0,[r10,#GOFF_R]
t_ldr_i         LDR     r0,[r10,#GOFF_I]
t_ld_a_r        LDR     r0,[r10,#GOFF_LINE]
                LDR     r1,[r10,#GOFF_R]
                ADD     r0,r0,r0,LSL #3
                ADD     r0,r0,r8,LSR #10
                ADD     r0,r0,r1
                AND     r0,r0,#0x7F
                AND     r1,r1,#0x80
                ORR     r0,r0,r1
t_mov_a_r0_24   MOV     r4,r0,LSL #24
t_air_flags     ADD     r1,r10,#PZST_OFF
                LDRB    r1,[r1,r0]
                AND     r1,r1,#PSR_S + PSR_Z
                AND     r3,r3,#PSR_C
                ORR     r3,r3,r1
                LDR     r0,[r10,#GOFF_IFF2]
                TEQ     r0,#0
                ORRNE   r3,r3,#PSR_V

; e_taken: emits the taken path of CALL cc / RET cc of the record.
e_taken
        stmfd   sp!,{lr}
        ldr     r0,[r6,#I_KIND]
        cmp     r0,#K_CALLCC
        bne     et_ret
        TMPL    r0,t_sub_cyc7
        EMIT    r0
        ldr     r0,[r6,#I_PC]
        ldr     r1,[r6,#I_LEN]
        add     r0,r0,r1
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#0
        bl      e_mov32
        bl      e_push
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        bne     et_call_dyn
        ldr     r0,[r6,#I_TARGET]
        bl      e_exit
        ldmfd   sp!,{pc}
et_call_dyn
        bl      e_dyn_exit
        ldmfd   sp!,{pc}
et_ret
        TMPL    r0,t_sub_cyc6
        EMIT    r0
        bl      e_pop
        TMPL    r0,t_ret_addr
        EMIT    r0
        bl      e_dynamic
        ldmfd   sp!,{pc}
t_sub_cyc7      SUB     r8,r8,#7 * 256
t_sub_cyc6      SUB     r8,r8,#6 * 256

;----------------------------------------------------------------------------
; Unprefixed and DD / FD instructions (main_tab, by opcode)
;----------------------------------------------------------------------------

; Copies of shared templates within reach of the literal loads below.
t3_orrcs_h      ORRCS   r3,r3,#PSR_H
t3_mov_r0_lsr24 MOV     r0,r0,LSR #24   ; | register
t3_mov_r0_r     MOV     r0,r0           ; | operand
t3_mov_r0_imm   MOV     r0,#0           ; | immediate

m_nop
        b       eb_done

m_exaf                                  ; EX AF,AF'
        ; F' and A' are the two words below BC' in the context, in the
        ; order of r3 and r4 (their own offset is not an ARM immediate).
        adr     r12,t_exaf
        ldmia   r12,{r0-r3,r12}
        stmia   r5!,{r0-r3,r12}
        b       eb_done
t_exaf          ADD     r12,r10,#GOFF_BC2
                LDMDB   r12,{r0,r1}
                STMDB   r12,{r3,r4}
                MOV     r3,r0
                MOV     r4,r1

m_djnz                                  ; DJNZ
        adr     r12,t_djnz
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r0,#CC_NE
        mov     r1,#5
        bl      e_jump_cond
        b       eb_done
t_djnz          SUB     r5,r5,#0x01000000
                TST     r5,#0xFF000000

m_jr                                    ; JR
        ldr     r0,[r6,#I_INTERNAL]
        cmp     r0,#0
        blt     m_jr_out
m_jump_in
        ; A static target inside the block: the idle time of a busy-wait
        ; loop, then a direct branch.
        ldr     r0,[r6,#I_BUSY]
        cmp     r0,#0
        movne   r0,#CC_AL
        blne    e_idle
        ldr     r0,[r6,#I_INTERNAL]
        ldr     r1,[r4,#B_INS]
        RECORD  r2,r1,r0
        ldr     r0,[r2,#I_HOST]
        EMITB   r1,r0,B_AL
        b       eb_done
m_jr_out
        ldr     r0,[r6,#I_TARGET]
        bl      e_exit
        b       eb_done

m_jrcc                                  ; JR cc
        and     r0,r9,#0x18
        mov     r0,r0,lsr #3            ; y - 4
        bl      e_cond
        mov     r1,#5
        bl      e_jump_cond
        b       eb_done

m_ld_rr_nn                              ; LD rr,nn
        ldr     r0,[r6,#I_N]
        mov     r0,r0,lsl #16           ; (16-bit operand)
        and     r1,r9,#0x30
        cmp     r1,#0x20
        bne     m_ld_rr_reg
        ldr     r1,[r6,#I_PRE]
        cmp     r1,#PRE_FD
        bne     m_ld_rr_reg
        mov     r1,#0
        bl      e_mov32
        TMPL    r0,t_str_r0_iy
        EMIT    r0
        b       eb_done
m_ld_rr_reg
        stmfd   sp!,{r0}
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4            ; p
        mov     r1,#2
        bl      e_rp_reg
        mov     r1,r0
        ldmfd   sp!,{r0}
        bl      e_mov32
        b       eb_done
t_str_r0_iy     STR     r0,[r10,#GOFF_IY]

m_add_hl                                ; ADD HL,rr
        mov     r0,#RLR
        bl      e_idx_reg
        mov     r12,r0                  ; rh
        and     r0,r9,#0x30
        cmp     r0,#0x20
        moveq   r0,r12
        beq     m_add_hl_rr
        stmfd   sp!,{r12}
        mov     r0,r0,lsr #4
        mov     r1,#2
        bl      e_rp_reg
        ldmfd   sp!,{r12}
m_add_hl_rr
        ; r12 = rh, r0 = rr
        and     r1,r11,#EF_LIVE + EF_FUSED
        cmp     r1,#EF_LIVE
        bne     m_add_hl_s
        cmp     r0,r12
        bne     m_add_hl_two
        adr     r1,t_add_hl_same
        ldmia   r1,{r0,r1,r2,r3,lr}     ; ADDS BIC ORRCS TST ORRNE
        orr     r0,r0,r12
        orr     r0,r0,r12,lsl #12
        orr     r0,r0,r12,lsl #16
        orr     r3,r3,r12,lsl #16
        stmia   r5!,{r0,r1,r2,r3,lr}
        b       m_add_hl_full
m_add_hl_two
        adr     r1,t_add_hl_rr
        ldmia   r1,{r1,r2,r3,lr}        ; MOV ADDS BIC ORRCS
        orr     r1,r1,r12
        orr     r2,r2,r0
        orr     r2,r2,r12,lsl #12
        orr     r2,r2,r12,lsl #16
        stmia   r5!,{r1,r2,r3,lr}
        TMPL    r1,t_cmn_r1_rr4
        orr     r1,r1,r0
        TMPL    r2,t3_orrcs_h
        stmia   r5!,{r1,r2}
m_add_hl_full
        FULLF   r0,r1
        b       m_add_hl_store
m_add_hl_s
        TMPL    r1,t_add_rh_rr
        orr     r1,r1,r0
        orr     r1,r1,r12,lsl #12
        orr     r1,r1,r12,lsl #16
        tst     r11,#EF_FUSED
        orrne   r1,r1,#0x100000
        EMIT    r1
m_add_hl_store
        mov     r0,r12
        bl      e_idx_store
        b       eb_done
t_add_hl_same   ADDS    r0,r0,r0        ; | rh, rh, rh
                BIC     r3,r3,#PSR_C + PSR_H + PSR_n
                ORRCS   r3,r3,#PSR_C
                DCD     0xE3100410      ; TST rh,#0x10000000 | rh (the rotation of the C emitter)
                ORRNE   r3,r3,#PSR_H
t_add_hl_rr     MOV     r1,r0,LSL #4    ; | rh
                ADDS    r0,r0,r0        ; | rh, rh, rr
                BIC     r3,r3,#PSR_C + PSR_H + PSR_n
                ORRCS   r3,r3,#PSR_C
t_cmn_r1_rr4    CMN     r1,r0,LSL #4    ; | rr
t_add_rh_rr     ADD     r0,r0,r0        ; | rh, rh, rr, S

m_ld_ind_a                              ; LD (BC),A / LD (DE),A
        TMPL    r0,t_mov_r0_a24
        EMIT    r0
        tst     r9,#0x10
        moveq   r0,#RBC
        movne   r0,#RDE
        bl      e_write
        b       eb_done

m_ld_a_ind                              ; LD A,(BC) / LD A,(DE)
        tst     r9,#0x10
        moveq   r0,#RBC
        movne   r0,#RDE
        bl      e_read
        TMPL    r0,t_mov_a_r0_24
        EMIT    r0
        b       eb_done

m_ld_nn_hl                              ; LD (nn),HL
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     m_ld_nn_hl_c
        ; Address read when the instruction runs: the generic write
        ; sequence, twice (its stub may lose r0-r2 and r12: the address
        ; is read again).
        bl      e_dyn_addr
        TMPL    r0,t_mov_r0_hl16
        EMIT    r0
        mov     r0,#RAD
        bl      e_write
        bl      e_dyn_addr
        adr     r12,t_ld_nn_hl_hi
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r0,#RAD
        bl      e_write
        b       eb_done
m_ld_nn_hl_c
        mov     r0,#2
        bl      e_idx_reg
        TMPL    r1,t_mov_r0_lsr16
        orr     r1,r1,r0
        EMIT    r1
        ldr     r0,[r6,#I_N]
        bl      e_write_const
        mov     r0,#2
        bl      e_idx_reg
        TMPL    r1,t3_mov_r0_lsr24
        orr     r1,r1,r0
        EMIT    r1
        ldr     r0,[r6,#I_N]
        add     r0,r0,#1
        bl      e_write_const
        b       eb_done
t_mov_r0_hl16   MOV     r0,r7,LSR #16
t_ld_nn_hl_hi   ADD     r12,r12,#0x10000
                MOV     r0,r7,LSR #24

m_ld_hl_nn                              ; LD HL,(nn)
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     m_ld_hl_nn_c
        bl      e_dyn_addr
        mov     r0,#RAD
        bl      e_read
        adr     r12,t_ld_hl_nn_dyn
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r0,#RAD
        bl      e_read
        TMPL    r0,t_orr_hl_r0_24
        EMIT    r0
        b       eb_done
m_ld_hl_nn_c
        ldr     r0,[r6,#I_N]
        bl      e_read_const
        TMPL    r0,t_mov_r2_r0
        EMIT    r0
        ldr     r0,[r6,#I_N]
        add     r0,r0,#1
        bl      e_read_const
        ldr     r0,[r6,#I_PRE]
        cmp     r0,#PRE_FD
        bne     m_ld_hl_nn_reg
        adr     r12,t_ld_iy_nn
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       eb_done
m_ld_hl_nn_reg
        cmp     r0,#PRE_DD
        moveq   r0,#RIX
        movne   r0,#RHL
        mov     r1,#0                   ; the high byte in r0
        bl      e_pair_lo_hi
        b       eb_done
t_ld_hl_nn_dyn  MOV     r7,r0,LSL #16
                ADD     r12,r12,#0x10000
t_orr_hl_r0_24  ORR     r7,r7,r0,LSL #24
t_ld_iy_nn      MOV     r1,r2,LSL #16
                ORR     r1,r1,r0,LSL #24
                STR     r1,[r10,#GOFF_IY]

m_ld_nn_a                               ; LD (nn),A
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     m_ld_nn_a_c
        bl      e_dyn_addr
        TMPL    r0,t_mov_r0_a24
        EMIT    r0
        mov     r0,#RAD
        bl      e_write
        b       eb_done
m_ld_nn_a_c
        TMPL    r0,t_mov_r0_a24
        EMIT    r0
        ldr     r0,[r6,#I_N]
        bl      e_write_const
        b       eb_done

m_ld_a_nn                               ; LD A,(nn)
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     m_ld_a_nn_c
        bl      e_dyn_addr
        mov     r0,#RAD
        bl      e_read
        b       m_ld_a_nn_end
m_ld_a_nn_c
        ldr     r0,[r6,#I_N]
        bl      e_read_const
m_ld_a_nn_end
        TMPL    r0,t_mov_a_r0_24
        EMIT    r0
        b       eb_done

m_inc_rr                                ; INC rr / DEC rr
        and     r0,r9,#0x30
        mov     r0,r0,lsr #4            ; p
        mov     r1,#RLR
        bl      e_rp_reg
        tst     r9,#8
        TMPLC   eq,r1,t_inc_rr
        TMPLC   ne,r1,t_dec_rr
        orr     r1,r1,r0,lsl #12
        orr     r1,r1,r0,lsl #16
        EMIT    r1
        and     r1,r9,#0x30
        cmp     r1,#0x20
        bleq    e_idx_store
        b       eb_done
t_inc_rr        ADD     r0,r0,#0x10000  ; | rr
t_dec_rr        SUB     r0,r0,#0x10000

m_inc_r                                 ; INC r / DEC r
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3            ; y
        and     r1,r9,#1                ; DEC
        cmp     r0,#6
        beq     m_inc_mem
        cmp     r0,#7
        bne     m_inc_reg
        mov     r0,#RA
        bl      e_incdec
        b       eb_done
m_inc_mem
        bl      e_mem_addr
        stmfd   sp!,{r0}
        bl      e_read
        TMPL    r0,t_mov_r0_lsl24
        EMIT    r0
        mov     r0,#0
        and     r1,r9,#1
        bl      e_incdec
        TMPL    r0,t3_mov_r0_lsr24
        EMIT    r0
        ldmfd   sp!,{r0}
        bl      e_write
        b       eb_done
m_inc_reg
        mov     r1,#0                   ; scratch r0
        and     r2,r11,#EF_IDX
        bl      e_get8
        cmp     r0,#0
        TMPLC   ne,r1,t3_mov_r0_r
        orrne   r1,r1,r0
        strne   r1,[r5],#4
        mov     r0,#0
        and     r1,r9,#1
        bl      e_incdec
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        mov     r1,#0                   ; operand r0
        and     r2,r11,#EF_IDX
        bl      e_set8
        b       eb_done
t_mov_r0_lsl24  MOV     r0,r0,LSL #24

m_ld_r_n                                ; LD r,n
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        cmp     r0,#6
        bne     m_ld_r_n_reg
        bl      e_mem_addr
        ldr     r1,[r6,#I_N]
        and     r1,r1,#0xFF
        TMPL    r2,t3_mov_r0_imm
        orr     r1,r1,r2
        EMIT    r1
        bl      e_write
        b       eb_done
m_ld_r_n_reg
        ldr     r1,[r6,#I_N]
        bl      e_imm8
        and     r2,r11,#EF_IDX
        bl      e_set8
        b       eb_done

; e_imm8: r1 = byte -> r1 = the immediate operand (e_get8 form) with the
; byte in bits 24-31 of the generated instruction.
e_imm8
        and     r1,r1,#0xFF
        mov     r2,r1,lsl #16
        orr     r2,r2,#O_IMM
        cmp     r1,#0
        orrne   r2,r2,#0x400
        orr     r1,r2,r1
        mov     pc,lr

m_rlca                                  ; RLCA
        tst     r11,#EF_LIVE
        TMPLC   ne,r0,t_bic_f_hnc
        strne   r0,[r5],#4
        adr     r12,t_rlca
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        tst     r11,#EF_LIVE
        TMPLC   ne,r0,t_orrcs_c
        strne   r0,[r5],#4
        b       eb_done
m_rrca                                  ; RRCA
        adr     r12,t_rrca
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
m_rot_a_flags
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_bic_f_hnc
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        b       eb_done
m_rla                                   ; RLA
        adr     r12,t_rla
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       m_rot_a_flags
m_rra                                   ; RRA
        adr     r12,t_rra
        ldmia   r12,{r0,r1,r2,r3,r12}
        stmia   r5!,{r0,r1,r2,r3,r12}
        b       m_rot_a_flags
t_bic_f_hnc     BIC     r3,r3,#PSR_H + PSR_n + PSR_C
t_orrcs_c       ORRCS   r3,r3,#PSR_C
t_rlca          MOVS    r4,r4,LSL #1
                ORRCS   r4,r4,#0x01000000
t_rrca          MOVS    r0,r4,LSR #25
                ORRCS   r0,r0,#0x80
                MOV     r4,r0,LSL #24
t_rla           TST     r3,#PSR_C
                DCD     0x13844880      ; ORRNE r4,r4,#0x00800000 (the rotation of the C emitter)
                MOVS    r4,r4,LSL #1
t_rra           MOV     r0,r4,LSR #24
                TST     r3,#PSR_C
                ORRNE   r0,r0,#0x100
                MOVS    r0,r0,LSR #1
                MOV     r4,r0,LSL #24

m_daa                                   ; DAA
        ldr     r0,[r8,#G_DAA]
        EMITB   r1,r0,BL_AL
        b       eb_done
m_cpl                                   ; CPL
        TMPL    r0,t_cpl
        EMIT    r0
        tst     r11,#EF_LIVE
        TMPLC   ne,r0,t_orr_f_hn
        strne   r0,[r5],#4
        b       eb_done
m_scf                                   ; SCF
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_scf
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        b       eb_done
m_ccf                                   ; CCF
        tst     r11,#EF_LIVE
        beq     eb_done
        adr     r12,t_ccf
        ldmia   r12,{r0,r1,r2,r3}
        stmia   r5!,{r0,r1,r2,r3}
        b       eb_done
t_cpl           EOR     r4,r4,#0xFF000000
t_orr_f_hn      ORR     r3,r3,#PSR_H + PSR_n
t_scf           BIC     r3,r3,#PSR_H + PSR_n
                ORR     r3,r3,#PSR_C
t_ccf           AND     r0,r3,#PSR_C
                BIC     r3,r3,#PSR_H + PSR_n
                EOR     r3,r3,#PSR_C
                ORR     r3,r3,r0,LSL #3

m_halt                                  ; HALT
        ldr     r0,[r6,#I_PC]
        ldr     r1,[r6,#I_LEN]
        add     r0,r0,r1
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#0
        bl      e_mov32
        ldr     r0,[r8,#G_HALT]
        EMITB   r1,r0,B_AL
        b       eb_done

m_ld_r_hl                               ; LD r,(HL)
        bl      e_mem_addr
        bl      e_read
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        mov     r1,#0xC00               ; r0 of the generated code, shifted by 24
        mov     r2,#0
        bl      e_set8
        b       eb_done

m_ld_hl_r                               ; LD (HL),r
        bl      e_mem_addr
        stmfd   sp!,{r0}
        and     r0,r9,#7
        bl      e_get_low
        ldmfd   sp!,{r0}
        bl      e_write
        b       eb_done

m_ld_r_r                                ; LD r,r'
        and     r0,r9,#7
        mov     r1,#0
        and     r2,r11,#EF_IDX
        bl      e_get8
        mov     r1,r0
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        and     r2,r11,#EF_IDX
        bl      e_set8
        b       eb_done

m_alu_r                                 ; ALU A,r
        and     r0,r9,#7
        cmp     r0,#6
        bne     m_alu_reg
        bl      e_mem_addr
        and     r1,r9,#0x38
        cmp     r1,#0x38                ; CP: the read is only made for
        bne     m_alu_read              ; a test that is shown
        tst     r11,#EF_FUSED + EF_LIVE
        beq     m_alu_noread
m_alu_read
        bl      e_read
m_alu_noread
        mov     r1,#0xC00
        b       m_alu_op
m_alu_reg
        mov     r1,#0
        and     r2,r11,#EF_IDX
        bl      e_get8
        mov     r1,r0
m_alu_op
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_alu
        b       eb_done

m_alu_n                                 ; ALU A,n
        ldr     r1,[r6,#I_N]
        bl      e_imm8
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_alu
        b       eb_done

m_ret_cc                                ; RET cc
m_call_cc                               ; CALL cc,nn
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_cond
        stmfd   sp!,{r0}
        mov     r0,#STUB_TAKEN
        bl      new_stub
        str     r7,[r0,#ST_INDEX]
        ldmfd   sp!,{r0}
        orr     r0,r0,#B_EQ
        EMIT    r0
        b       eb_done

m_pop                                   ; POP rr
        ; POP AF right after PUSH AF, with no cycle check or interrupt check
        ; between them, gives A and F their own values back: only SP moves,
        ; unless the push went through the special write path (r1 = 0 then,
        ; see z80j_glue_push_slow), where the bytes are read back as stored.
        and     r0,r9,#0x30
        cmp     r0,#0x30
        bne     m_pop_read
        cmp     r7,#0
        beq     m_pop_read
        ldr     r0,[r6,#I_SEG_START]
        ldr     r1,[r6,#I_PRE]
        sub     r12,r6,#I_SIZE          ; the previous instruction
        ldr     r2,[r12,#I_PRE]
        orr     r0,r0,r1
        ldr     r1,[r12,#I_IRQ_CHECK]
        orr     r0,r0,r2
        orrs    r0,r0,r1
        bne     m_pop_read
        ldr     r0,[r12,#I_OP]
        cmp     r0,#0xF5
        bne     m_pop_read
        adr     r12,t_pop_kept
        ldmia   r12,{r0,r1,r2}
        str     r0,[r5],#4
        EMITB   r0,r1,BL_EQ
        str     r2,[r5],#4
        b       eb_done
m_pop_read
        bl      e_pop
        and     r0,r9,#0x30
        cmp     r0,#0x30
        beq     m_pop_af
        cmp     r0,#0x20
        bne     m_pop_pair
        ldr     r1,[r6,#I_PRE]
        cmp     r1,#PRE_FD
        bne     m_pop_pair
        adr     r12,t_pop_iy
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       eb_done
m_pop_pair
        cmp     r0,#0x20
        beq     m_pop_hl
        cmp     r0,#0x10
        moveq   r0,#RDE
        movne   r0,#RBC
        b       m_pop_rr
m_pop_hl
        ldr     r1,[r6,#I_PRE]
        cmp     r1,#PRE_DD
        moveq   r0,#RIX
        movne   r0,#RHL
m_pop_rr
        mov     r1,#1                   ; the high byte in r1
        bl      e_pair_lo_hi
        b       eb_done
m_pop_af
        adr     r12,t_pop_af
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       eb_done
t_pop_iy        MOV     r0,r2,LSL #16
                ORR     r0,r0,r1,LSL #24
                STR     r0,[r10,#GOFF_IY]
t_pop_af        DCD     0xE28A0C08      ; ADD r0,r10,#FDEC_OFF (the rotation of the C emitter)
                LDRB    r3,[r0,r2]
                MOV     r4,r1,LSL #24
t_pop_kept      TEQ     r1,#0
                DCD     z80j_glue_pop_af
                ADD     r11,r11,#0x20000

m_ret                                   ; RET
        bl      e_pop
        TMPL    r0,t_ret_addr
        EMIT    r0
        bl      e_dynamic
        b       eb_done

m_exx                                   ; EXX
        ; BC', DE' and HL' are consecutive in the context.
        adr     r12,t_exx
        ldmia   r12,{r0-r3,r12,lr}
        stmia   r5!,{r0-r3,r12,lr}
        b       eb_done
t_exx           ADD     r12,r10,#GOFF_BC2
                LDMIA   r12,{r0,r1,r2}
                STMIA   r12,{r5,r6,r7}
                MOV     r5,r0
                MOV     r6,r1
                MOV     r7,r2

m_jp_hl                                 ; JP (HL)
        mov     r0,#0
        bl      e_idx_reg
        TMPL    r1,t_mov_r0_lsr16
        orr     r1,r1,r0
        EMIT    r1
        bl      e_dynamic
        b       eb_done

m_ld_sp_hl                              ; LD SP,HL
        mov     r0,#0
        bl      e_idx_reg
        TMPL    r1,t_mov_sp_r
        orr     r1,r1,r0
        EMIT    r1
        b       eb_done
t_mov_sp_r      MOV     r11,r0          ; | register

m_jp_cc                                 ; JP cc,nn
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_cond
        mov     r1,#0
        bl      e_jump_cond
        b       eb_done

m_jp                                    ; JP nn
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        beq     m_jp_static
        bl      e_dyn_exit
        b       eb_done
m_jp_static
        ldr     r0,[r6,#I_INTERNAL]
        cmp     r0,#0
        bge     m_jump_in
        b       m_jr_out

m_out_n                                 ; OUT (n),A
        ldr     r0,[r6,#I_RUN]
        cmp     r0,#0
        bne     mon_grp
        ldr     r1,[r6,#I_N]
        and     r1,r1,#0xFF
        TMPL    r0,t4_mov_r1_imm
        orr     r0,r0,r1
        TMPL    r1,t_mov_r0_a24
        stmia   r5!,{r0,r1}
        bl      e_time_left
        mov     r0,#4                   ; port_out
        bl      e_port_handler
        EMITB   r1,r0,BL_AL
        bl      e_leave_check
        b       eb_done
mon_grp                                 ; in a group of outputs (jit_out_groups)
        TMPL    r0,t_mov_r0_a24
        EMIT    r0
        bl      e_grp_out
        b       eb_done

m_in_n                                  ; IN A,(n)
        ldr     r1,[r6,#I_N]
        and     r1,r1,#0xFF
        TMPL    r0,t4_mov_r1_imm
        orr     r0,r0,r1
        EMIT    r0
        bl      e_time_left
        mov     r0,#0                   ; port_in
        bl      e_port_handler
        EMITB   r1,r0,BL_AL
        TMPL    r0,t_mov_a_r0_24
        EMIT    r0
        b       eb_done

; e_port_handler: r0 = 0 for the input table, 4 for the output table ->
; r0 = the handler of the port of the instruction's operand.
e_port_handler
        ldr     r1,[r6,#I_N]
        and     r1,r1,#0xFF
; e_port_table: the same for the port r1, r0 = 0, 4 or 8 for the input,
; output or run table.
e_port_table
        GETCTX  r2
        add     r2,r2,#C_PORT_IN :AND: 0xF00
        add     r2,r2,#C_PORT_IN :AND: 0xFF
        ldr     r2,[r2,r0]              ; the table
        ldr     r0,[r2,r1,lsl #2]
        mov     pc,lr

; e_grp_out: emits what an output of a group does with its byte, in r0 of
; the generated code (see jit_out_groups and Z80J_RUN_BUF in z80jit.h).
; Before the last one: ORR r0,r0,#d << 8 (and #Z80J_OBUF_N for an output
; to the constant port of a group that also outputs through C), then STR
; r0,[r10,#OBUF_OFF + 4 k]. The last one: ORR r0,r0,#(Z80J_RUN_BUF |
; count << 8) (and #Z80J_RUN_BUF_N for OUT (n),A), r2 = the T-states left
; after it, then the call of the run handler: through port C (BL io_outn),
; of the constant port n (MOV r1,#n, BL port_outn[n]), or of both (port C,
; then CMP r1,#n, MOVNE r12,#n, BLNE z80j_glue_out_split, which returns
; past BL port_outn[n] when C is not n). The constant port is a buffer port,
; whose handlers never ask to leave the block; port C is only known when
; the code runs, so a group with outputs through C ends with the leave
; check of its last instruction.
e_grp_out
        stmfd   sp!,{lr}
        ldr     r2,[r6,#I_RUN]
        tst     r2,#RUN_GRP_LAST
        bne     ego_last
        mov     r1,r2,lsr #16
        and     r1,r1,#0xFF             ; d
        TMPL    r0,t_orr_r0_hi
        orr     r0,r0,r1
        EMIT    r0
        tst     r2,#RUN_GRP_N
        TMPLC   ne,r0,t_orr_r0_obufn
        strne   r0,[r5],#4
        and     r1,r2,#0x0F             ; k
        TMPL    r0,t_str_obuf
        add     r0,r0,r1,lsl #2
        EMIT    r0
        ldmfd   sp!,{pc}
ego_last
        mov     r1,r2,lsr #8
        and     r1,r1,#0x1F             ; count
        orr     r1,r1,#RUN_BUF_HI
        ldr     r0,[r6,#I_PRE]
        cmp     r0,#0                   ; OUT (n),A
        orreq   r1,r1,#RUN_BUF_N_HI
        TMPL    r0,t_orr_r0_hi
        orr     r0,r0,r1
        EMIT    r0
        tst     r2,#RUN_GRP_HASC
        beq     ego_n
        bl      e_port_c
        bl      e_time_left
        ldr     r2,[r6,#I_RUN]
        tst     r2,#RUN_GRP_HASN
        ldreq   r0,[r8,#G_IO_OUTN]
        beq     ego_call
        mov     r1,r2,lsr #16
        and     r1,r1,#0xFF             ; n
        TMPL    r0,t_cmp_r1_imm
        orr     r0,r0,r1
        TMPL    r2,t_movne_r12_imm
        orr     r2,r2,r1
        stmia   r5!,{r0,r2}
        ldr     r0,ego_split
        EMITB   r2,r0,BL_NE
        b       ego_handler
ego_n
        ldr     r1,[r6,#I_RUN]
        mov     r1,r1,lsr #16
        and     r1,r1,#0xFF             ; n
        TMPL    r0,t_mov_r1_imm_g
        orr     r0,r0,r1
        EMIT    r0
        bl      e_time_left
        ldr     r1,[r6,#I_RUN]
        mov     r1,r1,lsr #16
        and     r1,r1,#0xFF
ego_handler
        mov     r0,#C_PORT_OUTN - C_PORT_IN
        bl      e_port_table
ego_call
        EMITB   r1,r0,BL_AL
        ldr     r2,[r6,#I_RUN]
        tst     r2,#RUN_GRP_HASC
        blne    e_leave_check
        ldmfd   sp!,{pc}
ego_split       DCD     z80j_glue_out_split
t_orr_r0_hi     DCD     0xE3800C00      ; ORR r0,r0,#byte << 8 (rotation 12) | byte
t_orr_r0_obufn  ORR     r0,r0,#OBUF_N
t_str_obuf      STR     r0,[r10,#OBUF_OFF]      ; + 4 k
t_cmp_r1_imm    CMP     r1,#0           ; | port
t_movne_r12_imm MOVNE   r12,#0          ; | port
t_mov_r1_imm_g  MOV     r1,#0           ; | port

m_ex_sp                                 ; EX (SP),HL
        mov     r0,#RSP
        bl      e_read
        adr     r12,t_ex_sp1
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r0,#RAD
        bl      e_read
        adr     r12,t_ex_sp2
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        mov     r0,#2
        bl      e_idx_reg
        TMPL    r1,t_mov_r0_lsr16
        orr     r1,r1,r0
        EMIT    r1
        mov     r0,#RSP
        bl      e_write
        mov     r0,#2
        bl      e_idx_reg
        TMPL    r1,t4_mov_r0_lsr24
        orr     r1,r1,r0
        TMPL    r2,t_add_r12_sp
        stmia   r5!,{r1,r2}
        mov     r0,#RAD
        bl      e_write
        ldr     r0,[r6,#I_PRE]
        cmp     r0,#PRE_FD
        bne     m_ex_sp_reg
        adr     r12,t_ex_sp_iy
        ldmia   r12,{r0,r1}
        stmia   r5!,{r0,r1}
        b       eb_done
m_ex_sp_reg
        cmp     r0,#PRE_DD
        moveq   r0,#RIX
        movne   r0,#RHL
        TMPL    r1,t_ldr_scratch
        orr     r1,r1,r0,lsl #12
        EMIT    r1
        b       eb_done
t_ex_sp1        MOV     r2,r0,LSL #16
                ADD     r12,r11,#0x10000
t_ex_sp2        ORR     r2,r2,r0,LSL #24
                STR     r2,[r10,#GOFF_SCRATCH]
t_add_r12_sp    ADD     r12,r11,#0x10000
t_ex_sp_iy      LDR     r0,[r10,#GOFF_SCRATCH]
                STR     r0,[r10,#GOFF_IY]
t_ldr_scratch   LDR     r0,[r10,#GOFF_SCRATCH]  ; | register

m_ex_de                                 ; EX DE,HL
        adr     r12,t_ex_de
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       eb_done
t_ex_de         MOV     r0,r6
                MOV     r6,r7
                MOV     r7,r0

m_di                                    ; DI / EI
        adr     r12,t_di
        ldmia   r12,{r0,r1,r2}
        tst     r9,#8                   ; EI
        orrne   r0,r0,#1
        stmia   r5!,{r0,r1,r2}
        b       eb_done
t_di            MOV     r0,#0           ; | 1 for EI
                STR     r0,[r10,#GOFF_IFF1]
                STR     r0,[r10,#GOFF_IFF2]

m_push                                  ; PUSH rr
        and     r0,r9,#0x30
        cmp     r0,#0x30
        bne     m_push_rr
        adr     r12,t_push_af
        ldmia   r12,{r0,r1,r2}
        stmia   r5!,{r0,r1,r2}
        b       m_push_go
m_push_rr
        mov     r0,r0,lsr #4
        mov     r1,#0
        bl      e_rp_reg
        TMPL    r1,t_mov_r0_lsr16
        orr     r1,r1,r0
        EMIT    r1
m_push_go
        bl      e_push
        b       eb_done
t_push_af       ADD     r1,r10,#FENC_OFF
                LDRB    r0,[r1,r3]
                ORR     r0,r0,r4,LSR #16

m_call                                  ; CALL nn
m_rst                                   ; RST
        ldr     r0,[r6,#I_PC]
        ldr     r1,[r6,#I_LEN]
        add     r0,r0,r1
        mov     r0,r0,lsl #16
        mov     r0,r0,lsr #16
        mov     r1,#0
        bl      e_mov32
        bl      e_push
        ldr     r0,[r6,#I_DYN]
        cmp     r0,#0
        bne     m_call_dyn
        ldr     r0,[r6,#I_TARGET]
        bl      e_exit
        b       eb_done
m_call_dyn
        bl      e_dyn_exit
        b       eb_done

;----------------------------------------------------------------------------
; CB instructions (rotations, BIT, RES, SET)
;----------------------------------------------------------------------------

; Copies of shared templates within reach of the literal loads below.
t4_mov_r1_imm   MOV     r1,#0           ; | immediate
t4_mov_r0_lsr24 MOV     r0,r0,LSR #24   ; | register
t4_mov_r0_r     MOV     r0,r0           ; | operand

emit_cb
        tst     r11,#EF_MEM
        beq     cb_reg
        bl      e_mem_addr
        stmfd   sp!,{r0}                ; the address register
        mov     r1,r9,lsr #6            ; x
        cmp     r1,#1
        bne     cb_mem_rw
        ; BIT b,(HL): only when the test is shown.
        tst     r11,#EF_FUSED + EF_LIVE
        beq     cb_mem_done
        bl      e_read
        mov     r0,#0
        and     r1,r9,#0x38
        mov     r1,r1,lsr #3
        mov     r2,r1
        bl      e_bit
cb_mem_done
        ldmfd   sp!,{r0}
        b       eb_done
cb_mem_rw
        bl      e_read
        mov     r1,r9,lsr #6
        cmp     r1,#0
        bne     cb_mem_bit
        ; A rotation of the byte read, with the undocumented copy to a
        ; register of the DD CB / FD CB forms.
        TMPL    r0,t_mov_r0_lsl24
        EMIT    r0
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_rot
        bl      e_rot_flags
        mov     r1,#0                   ; r0 of the generated code
        bl      cb_copy
        TMPL    r0,t4_mov_r0_lsr24
        EMIT    r0
        b       cb_mem_write
cb_mem_bit
        ; RES / SET on the byte read.
        cmp     r1,#2
        TMPLC   eq,r0,t_bic_r0_bit
        TMPLC   ne,r0,t_orr_r0_bit
        and     r1,r9,#0x38
        mov     r1,r1,lsr #3
        mov     r2,#1
        orr     r0,r0,r2,lsl r1
        EMIT    r0
        mov     r1,#0xC00               ; r0 shifted by 24, for the copy
        bl      cb_copy
cb_mem_write
        ldmfd   sp!,{r0}
        bl      e_write
        b       eb_done
; The undocumented copy of the result (operand r1: r0 of the generated
; code shifted by 0 or 24) to register z, for DD CB / FD CB with z != 6.
cb_copy
        ldr     r0,[r6,#I_PRE]
        cmp     r0,#PRE_DDCB
        movlo   pc,lr
        and     r0,r9,#7
        cmp     r0,#6
        moveq   pc,lr
        mov     r2,#0
        b       e_set8
t_bic_r0_bit    BIC     r0,r0,#0        ; | bit
t_orr_r0_bit    ORR     r0,r0,#0

cb_reg
        mov     r1,r9,lsr #6            ; x
        cmp     r1,#0
        bne     cb_reg_bit
        ; A rotation of register z.
        and     r0,r9,#7
        mov     r1,#0                   ; scratch r0
        mov     r2,#0
        bl      e_get8
        cmp     r0,#0
        TMPLC   ne,r1,t4_mov_r0_r
        orrne   r1,r1,r0
        strne   r1,[r5],#4
        and     r0,r9,#0x38
        mov     r0,r0,lsr #3
        bl      e_rot
        bl      e_rot_flags
        and     r0,r9,#7
        mov     r1,#0
        mov     r2,#0
        bl      e_set8
        b       eb_done
cb_reg_bit
        ; The bit position of bit y of register z in its ARM register.
        and     r0,r9,#7
        mov     r1,#0
        mov     r2,#0
        bl      e_pair_of               ; (emits nothing: no index register)
        and     r2,r9,#0x38
        mov     r2,r2,lsr #3            ; y, the bit
        and     r1,r9,#7
        cmp     r1,#7
        tstne   r1,#1                   ; A or a high byte: bits 24-31
        addeq   r1,r2,#24
        addne   r1,r2,#16
        mov     r3,r9,lsr #6
        cmp     r3,#1
        bne     cb_reg_setres
        bl      e_bit
        b       eb_done
cb_reg_setres
        ; RES / SET: BIC or ORR of the bit in the pair.
        cmp     r1,#24
        sublo   r1,r1,#16
        movlo   r2,#0x800
        subhs   r1,r1,#24
        movhs   r2,#0x400
        mov     r3,#1
        orr     r2,r2,r3,lsl r1
        mov     r3,r9,lsr #6
        cmp     r3,#2
        TMPLC   eq,r1,t_bic_pair_bit
        TMPLC   ne,r1,t_orr_pair_bit
        orr     r1,r1,r2
        orr     r1,r1,r0,lsl #12
        orr     r1,r1,r0,lsl #16
        EMIT    r1
        b       eb_done
t_bic_pair_bit  BIC     r0,r0,#0        ; | pair, bit
t_orr_pair_bit  ORR     r0,r0,#0

; Handlers of the unprefixed and DD / FD opcodes (the C switch of the
; emitter, opcode by opcode; a bare prefix never reaches here).
main_tab
        DCD     m_nop, m_ld_rr_nn, m_ld_ind_a, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_rlca
        DCD     m_exaf, m_add_hl, m_ld_a_ind, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_rrca
        DCD     m_djnz, m_ld_rr_nn, m_ld_ind_a, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_rla
        DCD     m_jr, m_add_hl, m_ld_a_ind, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_rra
        DCD     m_jrcc, m_ld_rr_nn, m_ld_nn_hl, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_daa
        DCD     m_jrcc, m_add_hl, m_ld_hl_nn, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_cpl
        DCD     m_jrcc, m_ld_rr_nn, m_ld_nn_a, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_scf
        DCD     m_jrcc, m_add_hl, m_ld_a_nn, m_inc_rr
        DCD     m_inc_r, m_inc_r, m_ld_r_n, m_ccf
        DCD     m_nop, m_ld_r_r, m_ld_r_r, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_r_r, m_nop, m_ld_r_r, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_nop, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_r, m_nop
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_r, m_ld_r_r
        DCD     m_nop, m_ld_r_r, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_r, m_ld_r_r
        DCD     m_ld_r_r, m_nop, m_ld_r_hl, m_ld_r_r
        DCD     m_ld_hl_r, m_ld_hl_r, m_ld_hl_r, m_ld_hl_r
        DCD     m_ld_hl_r, m_ld_hl_r, m_halt, m_ld_hl_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_r, m_ld_r_r
        DCD     m_ld_r_r, m_ld_r_r, m_ld_r_hl, m_nop
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_alu_r, m_alu_r, m_alu_r, m_alu_r
        DCD     m_ret_cc, m_pop, m_jp_cc, m_jp
        DCD     m_call_cc, m_push, m_alu_n, m_rst
        DCD     m_ret_cc, m_ret, m_jp_cc, m_nop
        DCD     m_call_cc, m_call, m_alu_n, m_rst
        DCD     m_ret_cc, m_pop, m_jp_cc, m_out_n
        DCD     m_call_cc, m_push, m_alu_n, m_rst
        DCD     m_ret_cc, m_exx, m_jp_cc, m_in_n
        DCD     m_call_cc, m_call, m_alu_n, m_rst
        DCD     m_ret_cc, m_pop, m_jp_cc, m_ex_sp
        DCD     m_call_cc, m_push, m_alu_n, m_rst
        DCD     m_ret_cc, m_jp_hl, m_jp_cc, m_ex_de
        DCD     m_call_cc, m_call, m_alu_n, m_rst
        DCD     m_ret_cc, m_pop, m_jp_cc, m_di
        DCD     m_call_cc, m_push, m_alu_n, m_rst
        DCD     m_ret_cc, m_ld_sp_hl, m_jp_cc, m_di
        DCD     m_call_cc, m_call, m_alu_n, m_rst

        END
