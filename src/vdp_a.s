; Sprite evaluation of the SMS VDP (see vdp.h, vdp_sprites): which lines of
; each sprite the hardware shows (eight per line at most, in table order),
; the overflow flag and the collision flag.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  vdp_sprites

; vdp_state fields (vdp.h; offsets checked in vdp.c).
V_REG           EQU     0x40
V_STATUS        EQU     0x50
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400
V_SPR           EQU     0x4874          ; spr_n, then the fields below
S_N             EQU     0
S_H             EQU     4
S_ZOOM          EQU     8
S_PARTIAL       EQU     12
S_Y             EQU     16              ; 64 words
S_VIS           EQU     272             ; 64 words
S_ROWS          EQU     528             ; 64 words: bit r: row r has an opaque pixel
S_TROWS         EQU     784             ; 512 bytes: the same per tile
S_TOK           EQU     1296            ; 512 bytes: cache entry valid

ACTIVE          EQU     192
ST_OVERFLOW     EQU     0x40
ST_COLLIDE      EQU     0x20

; Stack frame.
F_DIFF          EQU     0               ; 196 bytes: sprites starting (+1) and
                                        ; ending (-1) on each line, then the
                                        ; count of sprites met on each line
F_RANGE         EQU     200             ; 64 words: first line | last line + 1 << 8
                                        ; of each sprite on the screen (0: none)
F_OVER          EQU     456             ; 8 words: lines with more than 8 sprites
F_HEAD          EQU     488             ; 192 bytes: first sprite starting on a line
F_NEXT          EQU     680             ; 64 bytes: next sprite starting on the line
F_ORDER         EQU     744             ; 64 words: sprites by first line, as
                                        ; sprite | x << 8 | first << 16 | last + 1 << 24
F_TILE          EQU     1000            ; tile base | 0x10000 for tall sprites
F_NCHAIN        EQU     1004            ; sprites chained (with an opaque pixel)
F_SIZE          EQU     1008

;----------------------------------------------------------------------------
; uint32 vdp_sprites(vdp_state *v)
;
; Fills spr_n, spr_h, spr_zoom, spr_partial, spr_y and spr_vis, ORs the
; sprite flags into the status and returns them. A pass over the sprites
; counts, per line, those that start and end there; the running sum gives
; the number of sprites on each line. When some line has more than eight,
; a second pass in table order, over those lines only, marks the lines
; each sprite loses. Collisions are looked for among the sprites whose
; boxes overlap: the sprites on the screen are ordered by their first line
; (a counting sort), each one is compared with those starting before its
; last line, and when their columns overlap too the opaque pixels of their
; rows are tested where they meet.
;----------------------------------------------------------------------------
vdp_sprites
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#F_SIZE
        mov     r11,r0                  ; v
        ldrb    r1,[r11,#V_REG+5]
        and     r1,r1,#0x7E
        add     r10,r11,#V_VRAM
        add     r10,r10,r1,lsl #7       ; r10 = sprite attribute table
        ldrb    r1,[r11,#V_REG+1]
        and     r9,r1,#1                ; r9 = zoom
        tst     r1,#2
        moveq   r8,#8
        movne   r8,#16
        mov     r8,r8,lsl r9            ; r8 = lines per sprite
        add     r6,r11,#0x4800
        add     r6,r6,#V_SPR-0x4800     ; r6 = &spr_n
        str     r8,[r6,#S_H]
        str     r9,[r6,#S_ZOOM]
        mov     r0,#0
        str     r0,[r6,#S_PARTIAL]
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        mov     r12,sp                  ; diff, over: 0
        mov     lr,#13
clr_loop
        stmia   r12!,{r0-r3}
        subs    lr,lr,#1
        bne     clr_loop
        add     r12,sp,#F_OVER
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        mvn     r0,#0
        mvn     r1,#0
        mvn     r2,#0
        mvn     r3,#0
        add     r12,sp,#F_HEAD          ; no sprite starts on any line yet
        mov     lr,#12
hd_clr
        stmia   r12!,{r0-r3}
        subs    lr,lr,#1
        bne     hd_clr
        ldrb    lr,[r11,#V_REG+6]
        and     lr,lr,#4
        mov     lr,lr,lsl #6            ; tile base
        ldrb    r0,[r11,#V_REG+1]
        and     r0,r0,#2
        orr     lr,lr,r0,lsl #15        ; | 0x10000 when tall
        str     lr,[sp,#F_TILE]
        ; Pass A, up to the terminator: ranges, the line counts, the
        ; opaque rows of each sprite, and the sprites on the screen with
        ; an opaque pixel chained by first line (the others cannot
        ; collide). The lines each sprite shows (spr_vis) are only written
        ; when some line holds more than eight sprites: nothing reads them
        ; otherwise.
        mov     r4,#0
        mov     r5,#0                   ; r5 = sprites chained
        rsb     r12,r8,#256             ; a Y past 256 - h wraps to the top
pa_loop
        cmp     r4,#64
        beq     pa_done
        ldrb    r0,[r10,r4]
        cmp     r0,#0xD0
        beq     pa_done
        add     r0,r0,#1
        cmp     r0,r12
        subgt   r0,r0,#256              ; screen line of the first row
        add     r1,r6,r4,lsl #2
        str     r0,[r1,#S_Y]
        movs    r2,r0
        movmi   r2,#0                   ; first line on the screen
        add     r3,r0,r8
        cmp     r3,#ACTIVE
        movgt   r3,#ACTIVE              ; last line + 1
        cmp     r2,r3
        movge   r1,#0
        bge     pa_store
        ldrb    lr,[sp,r2]
        add     lr,lr,#1
        strb    lr,[sp,r2]
        ldrb    lr,[sp,r3]
        sub     lr,lr,#1
        strb    lr,[sp,r3]
        orr     r1,r2,r3,lsl #8
        stmfd   sp!,{r1,r2,r3,r12,lr}
        add     lr,r10,r4,lsl #1
        ldrb    r0,[lr,#0x81]
        ldr     lr,[sp,#20+F_TILE]
        tst     lr,#0x10000
        bicne   r0,r0,#1
        and     lr,lr,#0x100
        add     r2,r0,lr                ; first tile of the sprite
        mov     r0,r2
        bl      tile_rows
        mov     r3,r0
        ldr     lr,[sp,#20+F_TILE]
        tst     lr,#0x10000
        beq     pa_rows
        add     r0,r2,#1
        bic     r0,r0,#0x200            ; the second tile of a tall sprite
        bl      tile_rows
        orr     r3,r3,r0,lsl #8
pa_rows
        add     lr,r6,r4,lsl #2
        str     r3,[lr,#S_ROWS]
        mov     r0,r3
        ldmfd   sp!,{r1,r2,r3,r12,lr}
        cmp     r0,#0
        beq     pa_store                ; no opaque pixel: not chained
        add     lr,sp,#F_HEAD
        ldrb    r0,[lr,r2]
        strb    r4,[lr,r2]              ; the sprite heads the chain of its line
        add     lr,sp,#F_NEXT
        strb    r0,[lr,r4]
        add     r5,r5,#1
pa_store
        add     lr,sp,#F_RANGE
        str     r1,[lr,r4,lsl #2]
        add     r4,r4,#1
        b       pa_loop
pa_done
        mov     r7,r4                   ; r7 = sprites before the terminator
        str     r7,[r6,#S_N]
        str     r5,[sp,#F_NCHAIN]
        mov     r4,#0                   ; r4 = flags
        cmp     r7,#2
        blo     done                    ; one sprite at most
        ; Pass B: the running sum, the lines with more than eight, four
        ; lines per word of counts; the bit of each line goes to the over
        ; words as the line's bit walks through r5.
        MACRO
        PBLINE  $shift
        mov     r12,lr,lsl #$shift
        add     r0,r0,r12,asr #24
        cmp     r0,#8
        orrgt   r6,r6,r5
        movs    r5,r5,lsl #1
        strcs   r6,[r8],#4              ; a word of 32 lines is complete
        movcs   r6,#0
        movcs   r5,#1
        MEND

        mov     r0,#0                   ; sprites on the line
        mov     r3,sp
        add     r2,sp,#ACTIVE           ; end of the counts
        mov     r5,#1
        mov     r6,#0
        add     r8,sp,#F_OVER
pb_loop
        ldr     lr,[r3],#4
        PBLINE  0
        PBLINE  8
        PBLINE  16
        PBLINE  24
        cmp     r3,r2
        bne     pb_loop
        add     r6,r11,#0x4800
        add     r6,r6,#V_SPR-0x4800     ; r6 = &spr_n again
        add     r12,sp,#F_OVER
        ldmia   r12,{r0-r3}
        orr     r0,r0,r1
        orr     r2,r2,r3
        ldr     r1,[r12,#16]
        ldr     r3,[r12,#20]
        orr     r0,r0,r2
        orr     r1,r1,r3
        orrs    r0,r0,r1
        beq     collide                 ; no line holds more than eight
        ; Pass C: on the lines with more than eight sprites, in table
        ; order, the ninth and later lose the line.
        orr     r4,r4,#ST_OVERFLOW
        ; Each sprite starts with the lines it has on the screen (a sprite
        ; with none is hidden entirely).
        mov     r5,#0
pc_vis
        add     lr,sp,#F_RANGE
        ldr     r1,[lr,r5,lsl #2]
        mov     r2,r1,lsr #8            ; last line + 1
        and     r1,r1,#0xFF             ; first line
        subs    r2,r2,r1                ; lines on the screen
        movls   r0,#0
        bls     pc_vstore
        add     r3,r6,r5,lsl #2
        ldr     r3,[r3,#S_Y]
        sub     r1,r1,r3                ; first row on the screen
        mov     r0,#1
        mov     r0,r0,lsl r2
        sub     r0,r0,#1
        mov     r0,r0,lsl r1
pc_vstore
        add     r3,r6,r5,lsl #2
        str     r0,[r3,#S_VIS]
        add     r5,r5,#1
        cmp     r5,r7
        blo     pc_vis
        mov     r0,#0
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        mov     r12,sp
        mov     lr,#12
pc_clr
        stmia   r12!,{r0-r3}
        subs    lr,lr,#1
        bne     pc_clr
        mov     r5,#0                   ; sprite
pc_loop
        cmp     r5,r7
        beq     collide
        add     lr,sp,#F_RANGE
        ldr     r1,[lr,r5,lsl #2]
        movs    r2,r1,lsr #8            ; last line + 1
        beq     pc_next
        and     r1,r1,#0xFF             ; first line
        sub     r2,r2,r1                ; lines
        add     lr,sp,#F_OVER
        mov     r12,r1,lsr #5
        add     lr,lr,r12,lsl #2
        ldmia   lr,{r0,r3}
        and     r12,r1,#31
        mov     r0,r0,lsr r12
        rsb     r12,r12,#32
        orr     r0,r0,r3,lsl r12        ; over bits from the first line
        cmp     r2,#32
        movlo   r3,#1
        movlo   r3,r3,lsl r2
        sublo   r3,r3,#1
        andlo   r0,r0,r3
        movs    r0,r0                   ; r0 = the sprite's lines with too many
        beq     pc_next
        add     r3,r6,r5,lsl #2
        ldr     r12,[r3,#S_Y]
        sub     r12,r1,r12              ; line - first row of the sprite
pc_line
        tst     r0,#1
        beq     pc_skip
        ldrb    lr,[sp,r1]
        add     lr,lr,#1
        strb    lr,[sp,r1]
        cmp     lr,#8
        bls     pc_skip
        ldr     lr,[r3,#S_VIS]
        mov     r2,#1
        bic     lr,lr,r2,lsl r12
        str     lr,[r3,#S_VIS]
        mov     lr,#1
        str     lr,[r6,#S_PARTIAL]
pc_skip
        add     r1,r1,#1
        add     r12,r12,#1
        movs    r0,r0,lsr #1
        bne     pc_line
pc_next
        add     r5,r5,#1
        b       pc_loop

        ; Collisions: the sprites on the screen by first line, from the
        ; chains of pass A (each line's chain is in reverse table order,
        ; which the pairs do not mind).
collide
        ldr     r5,[sp,#F_NCHAIN]
        cmp     r5,#2
        blo     done                    ; one sprite with opaque pixels at most
co_order
        mov     r5,#0                   ; r5 = sprites ordered
        mov     r1,#0                   ; line
        add     r2,sp,#F_HEAD
        add     r3,sp,#F_ORDER
        MACRO
        CHAIN   $shift                  ; the sprites starting on line r1
        [ $shift = 0
        mov     r12,r0
        |
        mov     r12,r0,lsr #$shift
        ]
        and     r12,r12,#0xFF
5       cmp     r12,#0xFF
        beq     %F6
        add     lr,sp,#F_RANGE
        ldr     lr,[lr,r12,lsl #2]
        orr     lr,r12,lr,lsl #16       ; sprite | first << 16 | last + 1 << 24
        add     r12,r10,r12,lsl #1
        ldrb    r12,[r12,#0x80]
        orr     lr,lr,r12,lsl #8        ; | x << 8
        str     lr,[r3,r5,lsl #2]
        add     r5,r5,#1
        add     r12,sp,#F_NEXT
        and     lr,lr,#0xFF
        ldrb    r12,[r12,lr]
        b       %B5
6       add     r1,r1,#1
        MEND

co_oword
        ldr     r0,[r2],#4              ; four lines of heads
        cmn     r0,#1
        addeq   r1,r1,#4
        beq     co_onext
        CHAIN   24
        CHAIN   16
        CHAIN   8
        CHAIN   0
co_onext
        cmp     r1,#ACTIVE
        bne     co_oword
        ; Pairs: i against the j after it whose first line is before the
        ; last line of i.
co_pairs
        mov     r0,#8
        mov     r8,r0,lsl r9
        mov     r8,r8,lsl #8            ; r8 = sprite width << 8
        mov     r7,#0                   ; i
co_i
        add     r3,r7,#1
        cmp     r3,r5
        bhs     done
        add     lr,sp,#F_ORDER
        ldr     r0,[lr,r7,lsl #2]       ; sprite a: its word
        tst     r4,#ST_OVERFLOW
        beq     co_ishown
        and     r12,r0,#0xFF
        add     r12,r6,r12,lsl #2
        ldr     r12,[r12,#S_VIS]
        cmp     r12,#0
        beq     co_inext                ; hidden on every line
co_ishown
        mov     r1,r0,lsr #24
        mov     r1,r1,lsl #16           ; last line + 1 of a << 16
        and     r2,r0,#0xFF00           ; x of a << 8
co_j
        add     lr,sp,#F_ORDER
        ldr     r12,[lr,r3,lsl #2]      ; sprite b: its word
        and     lr,r12,#0xFF0000
        cmp     lr,r1
        bhs     co_inext                ; b starts after a ends, so do the rest
        tst     r4,#ST_OVERFLOW
        beq     co_jshown
        and     lr,r12,#0xFF
        add     lr,r6,lr,lsl #2
        ldr     lr,[lr,#S_VIS]
        cmp     lr,#0
        beq     co_jnext
co_jshown
        and     lr,r12,#0xFF00
        sub     lr,lr,r2                ; dx << 8
        cmp     lr,r8
        bge     co_jnext
        cmn     lr,r8
        ble     co_jnext
        stmfd   sp!,{r0-r3}
        and     r0,r0,#0xFF
        and     r1,r12,#0xFF
        mov     r2,lr,asr #8
        bl      pair_test
        movs    r12,r0
        ldmfd   sp!,{r0-r3}
        bne     co_hit
co_jnext
        add     r3,r3,#1
        cmp     r3,r5
        blo     co_j
co_inext
        add     r7,r7,#1
        b       co_i
co_hit
        orr     r4,r4,#ST_COLLIDE
        b       done

done
        ldr     r0,[r11,#V_STATUS]
        orr     r0,r0,r4
        str     r0,[r11,#V_STATUS]
        mov     r0,r4
        add     sp,sp,#F_SIZE
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; pair_test: r0 = sprite a, r1 = sprite b (starting on or after a's first
; line), r2 = x of b - x of a (within a sprite width). Returns nonzero in
; r0 when opaque pixels of the two meet on a line both are shown on. Uses
; r6 (&spr_n), r9 (zoom), r10 (sat), r11 (v), the frame; r4, r5, r7, r8
; are saved.
;----------------------------------------------------------------------------
pair_test
        stmfd   sp!,{r4,r5,r7,r8,lr}
        add     r12,sp,#36+F_RANGE      ; the caller pushed r0-r3 as well
        ldr     r3,[r12,r0,lsl #2]
        ldr     r12,[r12,r1,lsl #2]
        mov     r4,r12,lsr #8           ; end: the lower of the two last lines
        cmp     r4,r3,lsr #8
        movhi   r4,r3,lsr #8
        and     r5,r12,#0xFF            ; line: the first line of b
        ldrb    r3,[r11,#V_REG+6]
        and     r3,r3,#4
        mov     r3,r3,lsl #6            ; tile base: 0 or 256
        ldrb    r12,[r11,#V_REG+1]
        tst     r12,#2
        mov     r12,#0xFF
        bicne   r12,r12,#1              ; tile mask
        add     lr,r10,r0,lsl #1
        ldrb    r7,[lr,#0x81]
        and     r7,r7,r12
        add     r7,r3,r7                ; tile of a
        add     lr,r10,r1,lsl #1
        ldrb    r8,[lr,#0x81]
        and     r8,r8,r12
        add     r8,r3,r8                ; tile of b
        add     r0,r6,r0,lsl #2         ; &spr_y of a
        add     r1,r6,r1,lsl #2         ; &spr_y of b
        ; Rows with opaque pixels of the two, aligned on their lines: when
        ; none meet there is nothing to test (zoomed sprites skip this).
        cmp     r9,#0
        bne     pt_line
        ldr     r3,[r0,#S_ROWS]
        ldr     lr,[r1,#S_ROWS]
        ldr     r12,[r1,#S_Y]
        stmfd   sp!,{r2}
        ldr     r2,[r0,#S_Y]
        subs    r12,r12,r2              ; first row of b - first row of a
        ldmfd   sp!,{r2}
        rsbmi   r12,r12,#0
        movpl   lr,lr,lsl r12
        movmi   r3,r3,lsl r12
        tst     r3,lr
        beq     pt_none
pt_line
        cmp     r5,r4
        bhs     pt_none
        ldr     r3,[r0,#S_Y]
        sub     r3,r5,r3                ; row of a (lines)
        ldr     lr,[r1,#S_Y]
        sub     lr,r5,lr                ; row of b
        ldr     r12,[r6,#S_PARTIAL]
        cmp     r12,#0
        beq     pt_shown                ; no sprite loses a line this frame
        ldr     r12,[r0,#S_VIS]
        movs    r12,r12,lsr r3
        tst     r12,#1
        beq     pt_next
        ldr     r12,[r1,#S_VIS]
        movs    r12,r12,lsr lr
        tst     r12,#1
        beq     pt_next
pt_shown
        add     r12,r11,#V_VRAM
        mov     r3,r3,lsr r9
        mov     lr,lr,lsr r9            ; tile rows
        stmfd   sp!,{r4,r5}
        mov     r4,r3,lsr #3
        add     r4,r7,r4                ; tile holding the row of a
        bic     r4,r4,#0xFE00           ; within the 512 tiles
        and     r3,r3,#7
        add     r4,r12,r4,lsl #5
        ldr     r3,[r4,r3,lsl #2]       ; its four planes
        orr     r3,r3,r3,lsr #16
        orr     r3,r3,r3,lsr #8
        and     r3,r3,#0xFF             ; opaque pixels of a, bit 7 leftmost
        mov     r4,lr,lsr #3
        add     r4,r8,r4
        bic     r4,r4,#0xFE00           ; within the 512 tiles
        and     lr,lr,#7
        add     r4,r12,r4,lsl #5
        ldr     lr,[r4,lr,lsl #2]
        orr     lr,lr,lr,lsr #16
        orr     lr,lr,lr,lsr #8
        and     lr,lr,#0xFF             ; opaque pixels of b
        ldmfd   sp!,{r4,r5}
        cmp     r9,#0
        beq     pt_test
        ; Zoom: every bit doubled.
        stmfd   sp!,{r0,r1,lr}
        mov     r0,r3
        bl      zoom_mask
        mov     r3,r0
        ldr     r0,[sp,#8]
        bl      zoom_mask
        mov     lr,r0
        ldmfd   sp!,{r0,r1}
        add     sp,sp,#4
pt_test
        ; b's pixels shifted to a's columns: right by dx, or a's pixels
        ; right by -dx, which is the same test.
        cmp     r2,#0
        blt     pt_neg
        tst     r3,lr,lsr r2
        bne     pt_hit
        b       pt_next
pt_neg
        stmfd   sp!,{r4}
        rsb     r4,r2,#0
        tst     lr,r3,lsr r4
        ldmfd   sp!,{r4}
        bne     pt_hit
pt_next
        add     r5,r5,#1
        b       pt_line
pt_hit
        mov     r0,#1
        ldmfd   sp!,{r4,r5,r7,r8,pc}
pt_none
        mov     r0,#0
        ldmfd   sp!,{r4,r5,r7,r8,pc}

; tile_rows: r0 = tile; returns in r0 the mask of its rows holding an
; opaque pixel, from the cache (spr_tile_rows), computed again when the
; entry is not valid or the tile was written (dirty byte). Uses r6
; (&spr_n) and r11 (v); clobbers r1 and r12 only.
tile_rows
        add     r1,r6,#S_TOK
        ldrb    r12,[r1,r0]
        cmp     r12,#0
        beq     tr_compute              ; never computed
        add     r1,r11,#V_DIRTY
        ldrb    r12,[r1,r0]
        cmp     r12,#0
        beq     tr_cached               ; not written since
tr_compute
        add     r1,r6,#S_TOK
        mov     r12,#1
        strb    r12,[r1,r0]
        stmfd   sp!,{r2-r5,lr}
        add     r1,r11,#V_VRAM
        add     r1,r1,r0,lsl #5         ; the eight rows of four planes
        ldmia   r1!,{r2,r3,r4,r5}
        mov     r12,#0
        cmp     r2,#0
        orrne   r12,r12,#1
        cmp     r3,#0
        orrne   r12,r12,#2
        cmp     r4,#0
        orrne   r12,r12,#4
        cmp     r5,#0
        orrne   r12,r12,#8
        ldmia   r1,{r2,r3,r4,r5}
        cmp     r2,#0
        orrne   r12,r12,#16
        cmp     r3,#0
        orrne   r12,r12,#32
        cmp     r4,#0
        orrne   r12,r12,#64
        cmp     r5,#0
        orrne   r12,r12,#128
        add     r1,r6,#S_TROWS
        strb    r12,[r1,r0]
        mov     r0,r12
        ldmfd   sp!,{r2-r5,pc}
tr_cached
        add     r1,r6,#S_TROWS
        ldrb    r0,[r1,r0]
        mov     pc,lr

; zoom_mask: r0 = 8-bit mask; returns in r0 the 16-bit mask with each bit
; doubled. Clobbers r1.
        MACRO
        ZBIT    $b
        tst     r0,#$b
        mov     r1,r1,lsl #2
        orrne   r1,r1,#3
        MEND

zoom_mask
        mov     r1,#0
        ZBIT    0x80
        ZBIT    0x40
        ZBIT    0x20
        ZBIT    0x10
        ZBIT    0x08
        ZBIT    0x04
        ZBIT    0x02
        ZBIT    0x01
        mov     r0,r1
        mov     pc,lr

        END
