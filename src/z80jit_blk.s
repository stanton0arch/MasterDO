; Translation of one block (see z80jit.c, translate_block): the management
; around the decoding, the analysis and the code emission of a block, which
; the compiler had turned into a hundred stack loads and stores per block.
; The values that flow between the parts (the area, the zone, the block
; descriptor, the argument blocks of the three assembly routines) stay in
; registers here; nothing about the generated code changes.
;
; z80j_block *jit_translate_block(z80j_state *j, uint32 pc, uint32 *targets,
;                                  uint32 max_targets)
;
; Returns the block descriptor, or 0 with j->error set: 5 when nothing could
; be decoded at pc (the caller leaves such code to the interpreter), another
; value when the emitter failed (the caller reports it). With a non-null
; targets, the static exits of the block are written there by the analysis
; (at most max_targets), their number in jit_last_ntargets.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_translate_block
        IMPORT  jit_next_zone
        IMPORT  jit_block_key
        IMPORT  jit_decode_block
        IMPORT  jit_analyse_block
        IMPORT  jit_emit_block
        IMPORT  jit_insns
        IMPORT  jit_stubs
        IMPORT  jit_last_n
        IMPORT  jit_last_next
        IMPORT  jit_last_ntargets

; z80j_state (offsets checked in z80jit.c)
S_CTX           EQU     0
S_CODE          EQU     4
S_CUR           EQU     12
S_ZONE_END      EQU     16
S_AREA          EQU     0x1C            ; area[0]; area[1] 16 bytes further
S_SEEN          EQU     0x3C
S_ZONE_BLOCKS   EQU     0x403C
S_CHUNK_SHIFT   EQU     0x437C
S_FREE_BLOCKS   EQU     0x4FC8
S_NBLOCKS       EQU     0x4FD0
S_HASH          EQU     0x4FD4
S_GLUE          EQU     0x4F84
G_LINK          EQU     0               ; z80j_glue.link
G_MISS          EQU     4               ; z80j_glue.miss
S_LINKS         EQU     0x5FD4
S_NLINKS        EQU     0x5FD8
S_ERROR         EQU     0x5FE4
S_SEED_HOT      EQU     0x5FE8
ST_TRANSLATIONS EQU     0x884C
ST_INSNS        EQU     0x8850
ST_CODE_BYTES   EQU     0x8854
ST_FUSED        EQU     0x8858
ST_BUSY_LOOPS   EQU     0x887C
ST_LINE_LOOPS   EQU     0x8880
ST_PROMOTED     EQU     0x8894
ST_DYN_BLOCKS   EQU     0x88B0
ST_SEAMS        EQU     0x88B4
S_ZONE_SHIFT    EQU     0x88C0
S_ZONE_HEAD     EQU     0x88C4

; z80j_area
A_CUR           EQU     0
A_END           EQU     4
A_COUNT         EQU     12

; z80j_block
BLK_PC          EQU     0
BLK_KEY         EQU     4
BLK_ENTRY       EQU     8
BLK_BODY        EQU     12
BLK_NEXT        EQU     16
BLK_ZNEXT       EQU     20              ; the blocks of the same zone

; jit_block_ctx
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
B_NMEMO         EQU     0xB8
B_SIZE          EQU     0xBC

; jit_dec_block
D_SERIAL        EQU     16
D_NEXT_PC       EQU     20
D_FLAGS         EQU     24
D_SIZE          EQU     28
DEC_DYN         EQU     4
DEC_SEAM        EQU     8

; jit_ana_block
A_BC            EQU     20
A_BUSY          EQU     24
A_TARGETS       EQU     36
A_MAX_TARGETS   EQU     40
A_NTARGETS      EQU     44
A_SIZE          EQU     48

; z80j_ctx
C_LOOKUP        EQU     0x4400
GOFF_CHUNK_MARK EQU     0x900

KEY_FIXED       EQU     0
KEY_RAM         EQU     0x80000
BLOCK_BYTES     EQU     4096            ; free bytes required before a translation

; Stack frame: the block context, the decoder and analysis argument blocks,
; whether the emission was tried again in another zone, and the exits asked
; for.
F_BC            EQU     0
F_D             EQU     B_SIZE
F_A             EQU     F_D + D_SIZE
F_RETRIED       EQU     F_A + A_SIZE
F_TARGETS       EQU     F_RETRIED + 4   ; the exits asked for, and their room
F_MAX_TARGETS   EQU     F_TARGETS + 4
F_SIZE          EQU     F_MAX_TARGETS + 4

; Context offset of the run mark of the chunk holding j->cur, into
; [sp,#B_MARK_OFF]. Uses $a, $b.
        MACRO
        MARKOFF $a,$b
        ldr     $a,[r4,#S_CUR]
        ldr     $b,[r4,#S_CODE]
        sub     $a,$a,$b
        add     $b,r4,#S_CHUNK_SHIFT :AND: 0xFF00
        ldr     $b,[$b,#S_CHUNK_SHIFT :AND: 0xFF]
        add     $b,$b,#2                ; bytes to chunks
        mov     $a,$a,lsr $b
        mov     $a,$a,lsl #2
        add     $a,$a,#GOFF_CHUNK_MARK
        str     $a,[sp,#B_MARK_OFF]
        MEND

; Adds $v to the statistic at offset $off of the state. Uses $a, $b.
        MACRO
        STATADD $off,$v,$a,$b
        add     $a,r4,#$off :AND: 0xFF00
        ldr     $b,[$a,#$off :AND: 0xFF]
        add     $b,$b,$v
        str     $b,[$a,#$off :AND: 0xFF]
        MEND

; r4 = j, r5 = Z80 address, r6 = area, r7 = instruction records,
; r8 = instruction count, r9 = block descriptor, r11 = ctx.
jit_translate_block
        stmfd   sp!,{r4-r9,r11,lr}
        sub     sp,sp,#F_SIZE
        mov     r4,r0
        mov     r5,r1,lsl #16
        mov     r5,r5,lsr #16
        ldr     r11,[r4,#S_CTX]
        mov     r0,#0
        str     r0,[sp,#F_RETRIED]
        str     r2,[sp,#F_TARGETS]
        str     r3,[sp,#F_MAX_TARGETS]

        ; Area: hot for a block translated before (evicted since) when a
        ; hot area exists, or when every block is seeded there; cold
        ; otherwise. The address is marked as translated.
        mov     r0,r5,lsr #5
        add     r0,r4,r0,lsl #2
        ldr     r1,[r0,#S_SEEN]
        mov     r2,#1
        and     r3,r5,#31
        mov     r2,r2,lsl r3
        orr     r3,r1,r2
        str     r3,[r0,#S_SEEN]
        ldr     r12,[r4,#S_AREA + 16 + A_COUNT]
        cmp     r12,#0
        beq     tb_cold
        tst     r1,r2
        beq     tb_seed
        mov     r0,#1
        STATADD ST_PROMOTED,r0,r3,r12
        b       tb_hot
tb_seed
        add     r3,r4,#S_SEED_HOT :AND: 0xFF00
        ldr     r3,[r3,#S_SEED_HOT :AND: 0xFF]
        cmp     r3,#0
        beq     tb_cold
tb_hot
        add     r6,r4,#S_AREA + 16
        b       tb_zone
tb_cold
        add     r6,r4,#S_AREA
tb_zone
        ; The zone is emptied when it cannot take another block, or when
        ; no descriptor is free (an eviction frees some).
        ldmia   r6,{r0,r1}              ; cur, end
        sub     r1,r1,r0
        cmp     r1,#BLOCK_BYTES
        blo     tb_next_zone
        add     r2,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r2,[r2,#S_FREE_BLOCKS :AND: 0xFF]
        cmp     r2,#0
        bne     tb_zone_ok
tb_next_zone
        mov     r0,r4
        mov     r1,r6
        bl      jit_next_zone
        add     r2,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r2,[r2,#S_FREE_BLOCKS :AND: 0xFF]
        cmp     r2,#0
        beq     tb_no_descriptor
        ldmia   r6,{r0,r1}
tb_zone_ok
        str     r0,[r4,#S_CUR]
        str     r1,[r4,#S_ZONE_END]

        ; The block context.
        MARKOFF r0,r1
        ldr     r7,tb_insns
        ldr     r0,tb_stubs
        mov     r1,#0
        str     r4,[sp,#B_J]
        str     r7,[sp,#B_INS]
        str     r0,[sp,#B_STUBS]
        str     r1,[sp,#B_NSTUBS]
        str     r1,[sp,#B_END_LEAVE]
        str     r1,[sp,#B_HAS_DYN]
        str     r1,[sp,#B_NMEMO]
        mov     r0,r11
        mov     r1,r5
        bl      jit_block_key
        str     r0,[sp,#B_KEY]

        ; Decoding (jit_decode_block, z80jit_dec.s): ins, pc, ctx, key and
        ; the serial tagging the instruction map entries.
        mov     r3,r0
        add     r12,r4,#ST_TRANSLATIONS :AND: 0xFF00
        ldr     r12,[r12,#ST_TRANSLATIONS :AND: 0xFF]
        add     r12,r12,#1
        mov     r12,r12,lsl #8
        mov     r0,r7
        mov     r1,r5
        mov     r2,r11
        add     lr,sp,#F_D
        stmia   lr,{r0-r3,r12}
        mov     r0,lr
        bl      jit_decode_block
        movs    r8,r0
        beq     tb_undecodable
        ldr     r1,[sp,#F_D + D_FLAGS]
        tst     r1,#DEC_SEAM
        beq     tb_noseam
        mov     r0,#1
        STATADD ST_SEAMS,r0,r2,r3
tb_noseam
        and     r2,r1,#DEC_DYN
        mov     r2,r2,lsr #2
        str     r2,[sp,#B_HAS_DYN]
        ldr     r3,[sp,#F_D + D_NEXT_PC]
        str     r8,[sp,#B_N]
        str     r3,[sp,#B_NEXT_PC]
        ldr     r0,tb_last_n
        str     r8,[r0]
        ldr     r0,tb_last_next
        str     r3,[r0]

        ; Analysis (jit_analyse_block, z80jit_ana.s): ins, n, pc, next_pc,
        ; serial, then the block context for the flag scans.
        ldr     r12,[sp,#F_D + D_SERIAL]
        mov     r0,r7
        mov     r1,r8
        mov     r2,r5
        add     lr,sp,#F_A
        stmia   lr,{r0-r3,r12}
        mov     r0,sp
        str     r0,[lr,#A_BC]
        ldr     r0,[sp,#F_TARGETS]
        ldr     r1,[sp,#F_MAX_TARGETS]
        str     r0,[lr,#A_TARGETS]
        str     r1,[lr,#A_MAX_TARGETS]
        mov     r0,lr
        bl      jit_analyse_block
        ldr     r0,[sp,#F_A + A_NTARGETS]
        ldr     r1,tb_last_ntargets
        str     r0,[r1]
        add     lr,sp,#F_A + A_BUSY
        ldmia   lr,{r0,r1,r2}           ; busy, line, fused
        STATADD ST_BUSY_LOOPS,r0,r3,r12
        STATADD ST_LINE_LOOPS,r1,r3,r12
        STATADD ST_FUSED,r2,r3,r12

        ; Code: the first free descriptor takes the block; its entry check
        ; depends on the key (none for fixed code, the bytes for RAM, the
        ; bank for a paged slot).
        add     r3,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r9,[r3,#S_FREE_BLOCKS :AND: 0xFF]
tb_emit
        mov     r0,r5
        ldr     r1,[sp,#B_KEY]
        ldr     r2,[r4,#S_CUR]
        stmia   r9,{r0-r2}              ; pc, key, entry
        cmp     r1,#KEY_RAM
        moveq   r3,#2
        movne   r3,#1
        cmp     r1,#KEY_FIXED
        moveq   r3,#0
        str     r3,[sp,#B_ENTRY_CHECK]
        mov     r0,#0
        add     r3,r4,#S_ERROR :AND: 0xFF00
        str     r0,[r3,#S_ERROR :AND: 0xFF]
        mov     r0,sp
        bl      jit_emit_block
        add     r3,r4,#S_ERROR :AND: 0xFF00
        ldr     r0,[r3,#S_ERROR :AND: 0xFF]
        cmp     r0,#0
        beq     tb_commit
        cmp     r0,#4
        bne     tb_failed
        ; The zone was too full for this block: emit it again in the
        ; next one, once.
        ldr     r0,[sp,#F_RETRIED]
        cmp     r0,#0
        bne     tb_failed
        mov     r0,#1
        str     r0,[sp,#F_RETRIED]
        mov     r0,r4
        mov     r1,r6
        bl      jit_next_zone
        add     r3,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r9,[r3,#S_FREE_BLOCKS :AND: 0xFF]
        cmp     r9,#0
        beq     tb_no_descriptor
        ldmia   r6,{r0,r1}
        str     r0,[r4,#S_CUR]
        str     r1,[r4,#S_ZONE_END]
        MARKOFF r0,r1
        b       tb_emit

tb_failed
        mov     r0,#0
        str     r0,[r9,#BLK_ENTRY]
        b       tb_exit
tb_undecodable
        mov     r0,#5
        add     r3,r4,#S_ERROR :AND: 0xFF00
        str     r0,[r3,#S_ERROR :AND: 0xFF]
        mov     r0,#0
        b       tb_exit
tb_no_descriptor
        ; Every descriptor in use although the zones were emptied
        ; (jit_next_zone frees some whenever another zone holds blocks).
        mov     r0,#6
        add     r3,r4,#S_ERROR :AND: 0xFF00
        str     r0,[r3,#S_ERROR :AND: 0xFF]
        mov     r0,#0
        b       tb_exit

tb_commit
        ; The area goes on past the code; the descriptor leaves the free
        ; list and joins its zone, its hash chain and the lookup table.
        ldr     r0,[r4,#S_CUR]
        str     r0,[r6,#A_CUR]
        add     r3,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r1,[r9,#BLK_NEXT]
        str     r1,[r3,#S_FREE_BLOCKS :AND: 0xFF]
        ldr     r1,[r3,#S_NBLOCKS :AND: 0xFF]
        add     r1,r1,#1
        str     r1,[r3,#S_NBLOCKS :AND: 0xFF]
        ldr     r1,[r9,#BLK_ENTRY]
        ldr     r2,[r4,#S_CODE]
        sub     r2,r1,r2
        add     r3,r4,#S_ZONE_SHIFT :AND: 0xFF00
        ldr     r12,[r3,#S_ZONE_SHIFT :AND: 0xFF]
        add     r12,r12,#2              ; bytes to zones
        mov     r2,r2,lsr r12
        add     r3,r4,#S_ZONE_BLOCKS :AND: 0xFF00
        add     r3,r3,r2,lsl #2
        ldr     r12,[r3,#S_ZONE_BLOCKS :AND: 0xFF]
        add     r12,r12,#1
        str     r12,[r3,#S_ZONE_BLOCKS :AND: 0xFF]
        add     r3,r4,#S_ZONE_HEAD :AND: 0xFF00
        add     r3,r3,r2,lsl #2
        ldr     r12,[r3,#S_ZONE_HEAD :AND: 0xFF]
        str     r12,[r9,#BLK_ZNEXT]
        str     r9,[r3,#S_ZONE_HEAD :AND: 0xFF]
        ldr     r2,[sp,#B_BODY]
        str     r2,[r9,#BLK_BODY]
        mov     r2,r5,lsl #22           ; HASH(pc): the low 10 bits
        add     r3,r4,#S_HASH :AND: 0xFF00
        add     r3,r3,r2,lsr #20
        ldr     r2,[r3,#S_HASH :AND: 0xFF]
        str     r2,[r9,#BLK_NEXT]
        str     r9,[r3,#S_HASH :AND: 0xFF]
        add     r3,r11,#C_LOOKUP
        str     r1,[r3,r5,lsl #2]
        ; Statistics: translations, dynamic blocks, instructions, bytes.
        mov     r2,#1
        STATADD ST_TRANSLATIONS,r2,r3,r12
        ldr     r2,[sp,#B_HAS_DYN]
        cmp     r2,#0
        beq     tb_nodyn
        STATADD ST_DYN_BLOCKS,r2,r3,r12
tb_nodyn
        STATADD ST_INSNS,r8,r3,r12
        sub     r0,r0,r1
        STATADD ST_CODE_BYTES,r0,r3,r12
        mov     r0,r9
tb_exit
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r9,r11,pc}

;----------------------------------------------------------------------------
; jit_unlink_zone(j, lo, hi): undoes the direct branches into the code
; between lo and hi recorded in the link log (the call to the linker is
; restored, and the conditional jump that led to it points at the call
; again; the trampoline of a retranslated RAM block becomes a jump to the
; translator with the block's address), drops the entries whose own words
; lie there, compacts the log and returns the number of branches undone.
; r1 = lo, r2 = hi - lo; r5 reads the log, r7 writes it, r8 and r9 the
; link and miss glue, r11 the count. (The scan in C cost about 60 cycles
; per entry: a call to decode each branch, the log and its count
; reloaded at every entry.)
        EXPORT  jit_unlink_zone
jit_unlink_zone
        stmfd   sp!,{r4-r9,r11,lr}
        mov     r4,r0
        sub     r2,r2,r1
        add     r3,r4,#S_LINKS :AND: 0xFF00
        ldr     r5,[r3,#S_LINKS :AND: 0xFF]
        ldr     r6,[r3,#S_NLINKS :AND: 0xFF]
        mov     r7,r5
        add     r6,r5,r6,lsl #2
        add     r3,r4,#S_GLUE :AND: 0xFF00
        ldr     r8,[r3,#(S_GLUE :AND: 0xFF) + G_LINK]
        ldr     r9,[r3,#(S_GLUE :AND: 0xFF) + G_MISS]
        mov     r11,#0
ul_loop
        cmp     r5,r6
        bhs     ul_done
        ldr     r0,[r5],#4
        tst     r0,#1
        bne     ul_tramp
        ; A link stub: r0 = its data words, the branch is the word before.
        ; A stub inside the zone goes with its code: dropped.
        sub     r3,r0,r1
        cmp     r3,r2
        blo     ul_loop
        ldr     r3,[r0,#-4]
        mov     r3,r3,lsl #8
        add     r3,r0,r3,asr #6         ; target = site + 8 + offset
        add     r3,r3,#4
        sub     r3,r3,r1
        cmp     r3,r2
        bhs     ul_keep
        sub     r3,r8,r0                ; bl link from site = d - 4
        sub     r3,r3,#4
        mov     r3,r3,lsr #2
        bic     r3,r3,#0xFF000000
        orr     r3,r3,#0xEB000000
        str     r3,[r0,#-4]
        ldr     r3,[r0,#4]              ; the conditional jump, if any
        cmp     r3,#0
        beq     ul_undone
        ldr     r12,[r3]
        and     r12,r12,#0xFF000000
        sub     lr,r0,r3                ; its offset to the site
        sub     lr,lr,#12
        mov     lr,lr,lsr #2
        bic     lr,lr,#0xFF000000
        orr     r12,r12,lr
        str     r12,[r3]
ul_undone
        add     r11,r11,#1
        b       ul_loop
ul_keep
        sub     r3,r5,#4                ; stored only once the log is being compacted
        cmp     r3,r7
        strne   r0,[r7]
        add     r7,r7,#4
        b       ul_loop
ul_tramp
        ; The trampoline of a retranslated RAM block: the old entry holds
        ; a branch to the new one, and the block's address is kept in the
        ; word after it by the entry check it replaced.
        bic     r0,r0,#1
        sub     r3,r0,r1
        cmp     r3,r2
        blo     ul_loop
        ldr     r3,[r0]
        mov     r3,r3,lsl #8
        add     r3,r0,r3,asr #6
        add     r3,r3,#8
        sub     r3,r3,r1
        cmp     r3,r2
        orrhs   r0,r0,#1
        bhs     ul_keep
        ldr     r3,[r0,#4]
        mov     r3,r3,lsl #16
        mov     r3,r3,lsr #16           ; the Z80 address
        ldr     r12,ul_mov_r0
        orr     r12,r12,r3,lsr #8
        str     r12,[r0]
        ldr     r12,ul_orr_r0
        and     r3,r3,#0xFF
        orr     r12,r12,r3
        str     r12,[r0,#4]
        sub     r3,r9,r0                ; b miss from site + 8
        sub     r3,r3,#16
        mov     r3,r3,lsr #2
        bic     r3,r3,#0xFF000000
        orr     r3,r3,#0xEA000000
        str     r3,[r0,#8]
        b       ul_undone
ul_done
        add     r3,r4,#S_LINKS :AND: 0xFF00
        ldr     r0,[r3,#S_LINKS :AND: 0xFF]
        sub     r7,r7,r0
        mov     r7,r7,lsr #2
        str     r7,[r3,#S_NLINKS :AND: 0xFF]
        mov     r0,r11
        ldmfd   sp!,{r4-r9,r11,pc}
ul_mov_r0       DCD     0xE3A00C00      ; mov r0,#0
ul_orr_r0       DCD     0xE3800000      ; orr r0,r0,#0

tb_insns        DCD     jit_insns
tb_stubs        DCD     jit_stubs
tb_last_n       DCD     jit_last_n
tb_last_next    DCD     jit_last_next
tb_last_ntargets DCD    jit_last_ntargets

        END
