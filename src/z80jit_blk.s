; Translation of one block (see z80jit.c, translate_block): the management
; around the decoding, the analysis and the code emission of a block. The
; three parts share one block context (jit_bctx, z80jit_int.h), filled
; here with their inputs and read back for their outputs; the area, the
; zone, the block descriptor and the instruction count stay in registers;
; the statistics are updated once per block. Nothing about the generated
; code changes.
;
; z80j_block *jit_translate_block(z80j_state *j, uint32 pc, uint32 *targets,
;                                  uint32 max_targets)
;
; Returns the block descriptor, or 0 with j->error set: 5 when nothing could
; be decoded at pc (the caller leaves such code to the interpreter), another
; value when the emitter failed (the caller reports it). With a non-null
; targets, the static exits of the block are written there by the analysis
; (at most max_targets), their number in jit_bctx.ntargets.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  jit_translate_block
        IMPORT  jit_next_zone
        IMPORT  jit_decode_block
        IMPORT  jit_analyse_block
        IMPORT  jit_emit_block
        IMPORT  jit_bctx

; z80j_state (offsets checked in z80jit.c)
S_CTX           EQU     0
S_CODE          EQU     4
S_CUR           EQU     12
S_ZONE_END      EQU     16
S_AREA          EQU     0x1C            ; area[0]; area[1] 16 bytes further
S_SEEN          EQU     0x3C
S_ZONE_BLOCKS   EQU     0x403C
S_CHUNK_SHIFT   EQU     0x437C
S_SCAN_KEY      EQU     0x4384          ; the flag scan cache: 512 keys,
S_SCAN_KIND     EQU     0x4B84          ; page kinds (bytes) and values (bytes)
S_SCAN_VAL      EQU     0x4D84
S_FREE_BLOCKS   EQU     0x4FCC
S_NBLOCKS       EQU     0x4FD4
S_HASH          EQU     0x4FD8
S_GLUE          EQU     0x4F84
G_LINK          EQU     0               ; z80j_glue.link
G_MISS          EQU     4               ; z80j_glue.miss
S_LINKS         EQU     0x5FD8
S_NLINKS        EQU     0x5FDC
S_ERROR         EQU     0x5FE8
S_SEED_HOT      EQU     0x5FEC
ST_TRANSLATIONS EQU     0x8850          ; then insns, code_bytes, fused
ST_BUSY_LOOPS   EQU     0x8880          ; then line_loops
ST_SCAN_HITS    EQU     0x8888
ST_PROMOTED     EQU     0x8898
ST_DYN_BLOCKS   EQU     0x88B4
ST_SEAMS        EQU     0x88B8
S_ZONE_SHIFT    EQU     0x88C4
S_ZONE_HEAD     EQU     0x88C8

; z80j_area
A_CUR           EQU     0
A_END           EQU     4
A_COUNT         EQU     12

; z80j_block: the pc word holds the Z80 address in its low 16 bits and,
; with bit 31 set, the flag scan value of the block's code in bits 16-21.
BLK_PC          EQU     0
BLK_KEY         EQU     4
BLK_ENTRY       EQU     8
BLK_BODY        EQU     12
BLK_NEXT        EQU     16
BLK_ZNEXT       EQU     20              ; the blocks of the same zone
BLK_SCAN_VALID  EQU     0x80000000
BLK_SCAN_SHIFT  EQU     16
FL_ALL          EQU     0x3F

; jit_block_ctx
B_J             EQU     0
B_N             EQU     8
B_KEY           EQU     16
B_END_LEAVE     EQU     20
B_ENTRY_CHECK   EQU     32
B_HAS_DYN       EQU     36
B_MARK_OFF      EQU     40
B_BODY          EQU     44
B_PC            EQU     56
B_CTX           EQU     60
B_DEC_FLAGS     EQU     64
B_BUSY          EQU     68              ; then line, fused
B_FUSED         EQU     76
B_TARGETS       EQU     80
B_MAX_TARGETS   EQU     84
DEC_SEAM        EQU     8

; z80j_ctx
C_SLOT_BANK     EQU     0x57C
C_PAGE_KIND     EQU     0x600
C_LOOKUP        EQU     0x4100
GOFF_CHUNK_MARK EQU     0x900
PAGE_KIND       EQU     0x0F

KEY_FIXED       EQU     0
KEY_RAM         EQU     0x80000
; Free bytes a zone must have before a translation starts in it: enough for
; the entry check of a RAM block (at most 124 words measured) and the room
; the emitter checks before every instruction and stub (EMIT_ROOM_BYTES,
; z80jit_emt.s). A block that does not fit is emitted again in the next
; zone, so that a zone is left with less than a block of space unused.
BLOCK_BYTES     EQU     1024

; Stack frame: whether the emission was tried again in another zone.
F_RETRIED       EQU     0
F_SIZE          EQU     4

; Context offset of the run mark of the chunk holding j->cur, into the
; block context. Uses $a, $b.
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
        str     $a,[r7,#B_MARK_OFF]
        MEND

; Adds $v to the statistic at offset $off of the state. Uses $a, $b.
        MACRO
        STATADD $off,$v,$a,$b
        add     $a,r4,#$off :AND: 0xFF00
        ldr     $b,[$a,#$off :AND: 0xFF]
        add     $b,$b,$v
        str     $b,[$a,#$off :AND: 0xFF]
        MEND

; r4 = j, r5 = Z80 address, r6 = area, r7 = block context,
; r8 = instruction count, r9 = block descriptor, r11 = ctx.
jit_translate_block
        stmfd   sp!,{r4-r9,r11,lr}
        sub     sp,sp,#F_SIZE
        mov     r4,r0
        mov     r5,r1,lsl #16
        mov     r5,r5,lsr #16
        ldr     r11,[r4,#S_CTX]
        ldr     r7,tb_bctx
        mov     r0,#0
        str     r0,[sp,#F_RETRIED]
        ; The block context: the translator state, the block, the exits
        ; asked for (the records and stubs are bound once, by z80j_init).
        str     r4,[r7,#B_J]
        str     r2,[r7,#B_TARGETS]
        str     r3,[r7,#B_MAX_TARGETS]
        str     r5,[r7,#B_PC]
        str     r11,[r7,#B_CTX]
        str     r0,[r7,#B_END_LEAVE]

        ; The key of the code at pc (jit_block_key): the page kind, with
        ; the bank for a paged slot; then the entry check it calls for
        ; (none for fixed code, the bytes for RAM, the bank for a slot).
        add     r0,r11,r5,lsr #8
        ldrb    r0,[r0,#C_PAGE_KIND]
        and     r0,r0,#PAGE_KIND
        sub     r1,r0,#1
        cmp     r1,#3
        bls     tb_slot
        cmp     r0,#0
        movne   r0,#KEY_RAM
        moveq   r1,#0
        movne   r1,#2
        b       tb_keyed
tb_slot
        add     r1,r11,r1,lsl #2
        ldr     r1,[r1,#C_SLOT_BANK]
        mov     r1,r1,lsl #16
        orr     r0,r0,r1
        mov     r0,r0,ror #16           ; kind << 16 | bank
        mov     r1,#1
tb_keyed
        str     r0,[r7,#B_KEY]
        str     r1,[r7,#B_ENTRY_CHECK]

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
        sub     r2,r1,r0
        cmp     r2,#BLOCK_BYTES
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
        MARKOFF r0,r1

        ; Decoding (jit_decode_block, z80jit_dec.s).
        mov     r0,r7
        bl      jit_decode_block
        movs    r8,r0
        beq     tb_undecodable
        ldr     r1,[r7,#B_DEC_FLAGS]
        tst     r1,#DEC_SEAM
        beq     tb_noseam
        mov     r0,#1
        STATADD ST_SEAMS,r0,r2,r3
tb_noseam

        ; Analysis (jit_analyse_block, z80jit_ana.s): the busy-wait loops
        ; it found go to the statistics (its fused tests with the block's
        ; counters, at the commit).
        mov     r0,r7
        bl      jit_analyse_block
        add     r3,r7,#B_BUSY
        ldmia   r3,{r0,r1}              ; busy, line
        add     r3,r4,#ST_BUSY_LOOPS :AND: 0xFF00
        add     r3,r3,#ST_BUSY_LOOPS :AND: 0xFF
        ldmia   r3,{r2,r12}
        add     r2,r2,r0
        add     r12,r12,r1
        stmia   r3,{r2,r12}

        ; Code: the first free descriptor takes the block.
        add     r3,r4,#S_FREE_BLOCKS :AND: 0xFF00
        ldr     r9,[r3,#S_FREE_BLOCKS :AND: 0xFF]
tb_emit
        mov     r0,r5
        ldr     r1,[r7,#B_KEY]
        ldr     r2,[r4,#S_CUR]
        stmia   r9,{r0-r2}              ; pc, key, entry
        mov     r0,#0
        add     r3,r4,#S_ERROR :AND: 0xFF00
        str     r0,[r3,#S_ERROR :AND: 0xFF]
        mov     r0,r7
        bl      jit_emit_block
        add     r3,r4,#S_ERROR :AND: 0xFF00
        ldr     r0,[r3,#S_ERROR :AND: 0xFF]
        cmp     r0,#0
        beq     tb_commit
        cmp     r0,#4
        bne     tb_failed
        ; The zone was too full for this block (the emitter stopped at
        ; its room check): emit it again in the next one, once.
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
        ldr     r2,[r7,#B_BODY]
        str     r2,[r9,#BLK_BODY]
        mov     r2,r5,lsl #22           ; HASH(pc): the low 10 bits
        add     r3,r4,#S_HASH :AND: 0xFF00
        add     r3,r3,r2,lsr #20
        ldr     r2,[r3,#S_HASH :AND: 0xFF]
        str     r2,[r9,#BLK_NEXT]
        str     r9,[r3,#S_HASH :AND: 0xFF]
        add     r3,r11,#C_LOOKUP
        str     r1,[r3,r5,lsl #2]

        ; The flag scan value of the block's code, when the cache holds it
        ; (a block exiting here scanned it): kept in the descriptor, where
        ; the next block exiting here finds it with the lookup its link
        ; makes anyway. RAM code is never cached.
        ldr     r2,[r7,#B_KEY]
        cmp     r2,#KEY_RAM
        beq     tb_noscan
        eor     r3,r5,r5,lsr #9
        mov     r3,r3,lsl #23
        mov     r3,r3,lsr #23           ; cache slot of pc
        add     r12,r4,#S_SCAN_KEY :AND: 0xFF00
        add     r12,r12,r3,lsl #2
        ldr     r12,[r12,#S_SCAN_KEY :AND: 0xFF]
        orr     lr,r5,r2,lsl #16        ; pc and bank
        cmp     r12,lr
        bne     tb_noscan
        add     r12,r4,#S_SCAN_KIND :AND: 0xFF00
        add     r12,r12,r3
        ldrb    r12,[r12,#S_SCAN_KIND :AND: 0xFF]
        cmp     r12,r2,lsr #16          ; page kind
        bne     tb_noscan
        add     r12,r4,#S_SCAN_VAL :AND: 0xFF00
        add     r12,r12,r3
        ldrb    r12,[r12,#S_SCAN_VAL :AND: 0xFF]
        orr     r12,r5,r12,lsl #16
        orr     r12,r12,#BLK_SCAN_VALID
        str     r12,[r9,#BLK_PC]
tb_noscan

        ; Statistics: translations, instructions, bytes, fused tests in
        ; one go, then the dynamic blocks.
        add     r3,r4,#ST_TRANSLATIONS :AND: 0xFF00
        add     r3,r3,#ST_TRANSLATIONS :AND: 0xFF
        ldmia   r3,{r2,r12,lr}          ; translations, insns, code_bytes
        add     r2,r2,#1
        add     r12,r12,r8
        sub     r0,r0,r1                ; bytes of code
        add     lr,lr,r0
        stmia   r3,{r2,r12,lr}
        ldr     r0,[r7,#B_FUSED]
        ldr     r2,[r3,#12]             ; fused
        add     r2,r2,r0
        str     r2,[r3,#12]
        ldr     r2,[r7,#B_HAS_DYN]
        cmp     r2,#0
        beq     tb_nodyn
        STATADD ST_DYN_BLOCKS,r2,r3,r12
tb_nodyn
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

tb_bctx         DCD     jit_bctx

;----------------------------------------------------------------------------
; uint32 jit_flags_needed(jit_block_ctx *b, uint32 pc)
;
; Flags the code at pc may read before writing them (jit_scan_needed), for
; the block b; called by the analysis (z80jit_ana.s). The value depends on
; the bytes at pc and on the key of the scanning block only, so for ROM
; keys it is kept in two places: the descriptor of the block at pc with
; the same key, where an exit leading to a translated block finds it with
; the lookup the linker makes anyway, and a cache across blocks (slot (pc
; ^ pc >> 9) mod 512: pc | the bank of the key << 16, the page kind of the
; key, the value) for the other addresses. A RAM key is never cached (the
; scan stops at RAM). The answers found in either place are counted in
; stats.scan_hits.
; r4 = pc | bank << 16, r5 = descriptor of the block at pc, r6 = cache
; slot, r7 = b, r8 = j.
;----------------------------------------------------------------------------
        EXPORT  jit_flags_needed
        IMPORT  jit_scan_needed
jit_flags_needed
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16           ; pc
        ldr     r2,[r0,#B_KEY]
        cmp     r2,#KEY_RAM
        beq     jit_scan_needed
        stmfd   sp!,{r4-r8,lr}
        ldr     r8,[r0,#B_J]
        mov     r7,r0
        eor     r6,r1,r1,lsr #9
        mov     r6,r6,lsl #23
        mov     r6,r6,lsr #23           ; cache slot
        orr     r4,r1,r2,lsl #16        ; pc and bank
        add     r3,r8,#S_SCAN_KEY :AND: 0xFF00
        add     r3,r3,r6,lsl #2
        ldr     r3,[r3,#S_SCAN_KEY :AND: 0xFF]
        cmp     r3,r4
        bne     fn_miss
        add     r3,r8,#S_SCAN_KIND :AND: 0xFF00
        add     r3,r3,r6
        ldrb    r12,[r3,#S_SCAN_KIND :AND: 0xFF]
        cmp     r12,r2,lsr #16          ; page kind
        bne     fn_miss
        ldrb    r0,[r3,#(S_SCAN_KIND :AND: 0xFF) + S_SCAN_VAL - S_SCAN_KIND]
        add     r3,r8,#ST_SCAN_HITS :AND: 0xFF00
        ldr     r1,[r3,#ST_SCAN_HITS :AND: 0xFF]
        add     r1,r1,#1
        str     r1,[r3,#ST_SCAN_HITS :AND: 0xFF]
        ldmfd   sp!,{r4-r8,pc}

; Not in the cache: the descriptor of the block at pc with the same key
; (find_block, z80jit.c), else the scan, whose value the descriptor keeps.
fn_miss
        mov     r3,r1,lsl #22
        add     r5,r8,#S_HASH :AND: 0xFF00
        add     r5,r5,r3,lsr #20
        ldr     r5,[r5,#S_HASH :AND: 0xFF]      ; hash[pc & 1023]
fn_find
        cmp     r5,#0
        beq     fn_scan
        ldr     r3,[r5,#BLK_PC]
        mov     r3,r3,lsl #16
        cmp     r1,r3,lsr #16
        ldreq   r3,[r5,#BLK_KEY]
        cmpeq   r3,r2
        ldrne   r5,[r5,#BLK_NEXT]
        bne     fn_find
        ldr     r3,[r5,#BLK_PC]
        tst     r3,#BLK_SCAN_VALID
        beq     fn_scan
        mov     r0,r3,lsr #BLK_SCAN_SHIFT
        and     r0,r0,#FL_ALL
        add     r3,r8,#ST_SCAN_HITS :AND: 0xFF00
        ldr     r1,[r3,#ST_SCAN_HITS :AND: 0xFF]
        add     r1,r1,#1
        str     r1,[r3,#ST_SCAN_HITS :AND: 0xFF]
        b       fn_store
fn_scan
        mov     r0,r7
        bl      jit_scan_needed
        cmp     r5,#0
        movne   r1,r4,lsl #16
        movne   r1,r1,lsr #16
        orrne   r1,r1,r0,lsl #BLK_SCAN_SHIFT
        orrne   r1,r1,#BLK_SCAN_VALID
        strne   r1,[r5,#BLK_PC]
fn_store
        add     r3,r8,#S_SCAN_KEY :AND: 0xFF00
        add     r3,r3,r6,lsl #2
        str     r4,[r3,#S_SCAN_KEY :AND: 0xFF]
        ldr     r2,[r7,#B_KEY]
        mov     r2,r2,lsr #16           ; page kind
        add     r3,r8,#S_SCAN_KIND :AND: 0xFF00
        add     r3,r3,r6
        strb    r2,[r3,#S_SCAN_KIND :AND: 0xFF]
        strb    r0,[r3,#(S_SCAN_KIND :AND: 0xFF) + S_SCAN_VAL - S_SCAN_KIND]
        ldmfd   sp!,{r4-r8,pc}

        END
