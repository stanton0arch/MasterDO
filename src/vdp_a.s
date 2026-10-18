; Sprite evaluation of the SMS VDP (see vdp.h, vdp_sprites): which lines of
; each sprite the hardware shows (eight per line at most, in table order),
; the overflow flag and the collision flag.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  vdp_sprites

; vdp_state fields (vdp.h; offsets checked in vdp.c).
V_REG           EQU     0x40
V_STATUS        EQU     0x50
V_ACTIVE        EQU     0x58           ; active lines: 192, 224 or 240
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
S_TOK           EQU     1296
S_ORDER         EQU     0x814           ; 64 words: the sprites on the screen with an
                                        ; opaque pixel, by first row (see vdp.h)
S_NORD          EQU     0x914           ; their count            ; 512 bytes: cache entry valid

ST_OVERFLOW     EQU     0x40
ST_COLLIDE      EQU     0x20

; Stack frame (lines: up to 240).
F_PLANES        EQU     0               ; pass C: 8 x 4 words of bit-sliced counts
                                        ; of the sprites met on each line
F_M1            EQU     128             ; pass C: a sprite's lines in its second word
F_FIRST         EQU     132             ; its first line
F_LOST          EQU     136             ; the lines it loses in its first word
F_RANGE         EQU     256             ; 64 words: first line | last line + 1 << 8
                                        ; of each sprite on the screen (0: none)
F_HEAD          EQU     512             ; 272 bytes: first sprite whose first row is
                                        ; on line y, at y + 32 (0xFF: none; bit 7:
                                        ; the sprite has no opaque pixel)
F_NEXT          EQU     784             ; 64 bytes: next sprite starting on the line
F_ORDER         EQU     848             ; 64 words: sprites by first row, as
                                        ; sprite | x << 8 | first << 16 | last + 1 << 24
                                        ; (bit 7: no opaque pixel; those without
                                        ; it go to spr_order)
F_TILE          EQU     1104            ; tile base | 0x10000 for tall sprites
F_NSORT         EQU     1108            ; sprites on the screen (ordered)
F_ACTIVE        EQU     1112            ; active lines
F_SIZE          EQU     1120

;----------------------------------------------------------------------------
; uint32 vdp_sprites(vdp_state *v)
;
; Fills spr_n, spr_h, spr_zoom, spr_partial, spr_y, spr_vis, spr_order and
; spr_nord, ORs the sprite flags into the status and returns them. The list ends at the
; $D0 terminator in the 192-line mode only. A pass over the sprites gives
; each its screen lines and the opaque rows of its tiles, and chains the
; sprites on the screen by first row (a counting sort). In that order a
; line holds more than eight sprites exactly when some sprite's first
; screen line is still covered by the sprite eight places before it (the
; sprites all have the same height), a test over the sprites instead of
; the lines; when it fires, a second pass in table order, over the lines,
; marks the lines each sprite loses. The ordered sprites with an opaque
; pixel are kept in spr_order (the renderer draws the priority layer
; under them), and collisions are looked for among them: each one is
; compared with those starting before its last line, and when their
; columns overlap too the opaque pixels of their rows are tested where
; they meet.
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
        mvn     r0,#0
        mvn     r1,#0
        mvn     r2,#0
        mvn     r3,#0
        add     r12,sp,#F_HEAD          ; no sprite starts on any line yet
        ldr     lr,[r11,#V_ACTIVE]
        str     lr,[sp,#F_ACTIVE]
        add     lr,lr,#32
        mov     lr,lr,lsr #4
hd_clr
        stmia   r12!,{r0-r3}
        subs    lr,lr,#1
        bne     hd_clr
        ldrb    r12,[r11,#V_REG+6]
        and     r12,r12,#4
        mov     r12,r12,lsl #6          ; tile base
        ldrb    r0,[r11,#V_REG+1]
        and     r0,r0,#2
        orr     r12,r12,r0,lsl #15      ; | 0x10000 when tall
        str     r12,[sp,#F_TILE]        ; r12 = the same, for pass A

        ; TROWS: the opaque rows of tile r2 into $dst, from the cache
        ; (spr_tile_rows), computed again when the entry is not valid or
        ; the tile was written (dirty byte). Clobbers lr.
        MACRO
        TROWS   $dst
        add     lr,r6,#S_TOK
        ldrb    $dst,[lr,r2]
        add     lr,r11,#V_DIRTY
        ldrb    lr,[lr,r2]
        subs    lr,$dst,lr              ; 1: valid and not written since
        add     $dst,r6,#S_TROWS
        cmp     lr,#1
        ldreqb  $dst,[$dst,r2]
        beq     %F8
        bl      tile_rows               ; into lr and the cache
        mov     $dst,lr
8
        MEND

        ; Pass A, up to the terminator: the screen line and the range of
        ; each sprite, the opaque rows of its tiles, and the sprites on the
        ; screen chained by first row, flagged when they have no opaque
        ; pixel (they cannot collide). The lines each sprite shows
        ; (spr_vis) are only written when some line holds more than eight
        ; sprites: nothing reads them otherwise.
        mov     r4,#0
        mov     r5,#0                   ; r5 = sprites on the screen
        ldr     r7,[sp,#F_ACTIVE]
        cmp     r7,#192
        orreq   r7,r7,#0xD000           ; r7 = active | terminator << 8
        orrne   r7,r7,#0x10000          ; (no terminator: 0x100)
pa_loop
        cmp     r4,#64
        beq     pa_done
        ldrb    r0,[r10,r4]
        cmp     r0,r7,lsr #8
        beq     pa_done
        add     r0,r0,#1
        rsb     lr,r8,#256
        cmp     r0,lr
        subgt   r0,r0,#256              ; screen line of the first row
        add     r1,r6,r4,lsl #2
        str     r0,[r1,#S_Y]
        movs    r2,r0
        movmi   r2,#0                   ; first line on the screen
        add     r3,r0,r8
        and     lr,r7,#0xFF
        cmp     r3,lr
        movgt   r3,lr                   ; last line + 1
        cmp     r2,r3
        movge   r1,#0
        bge     pa_store                ; off the screen
        add     r0,r0,#32
        orr     r1,r2,r3,lsl #8
        orr     r1,r1,r0,lsl #16        ; the range, with y + 32 above it for now
        add     lr,r10,r4,lsl #1
        ldrb    r2,[lr,#0x81]
        tst     r12,#0x10000
        bicne   r2,r2,#1
        and     lr,r12,#0x100
        add     r2,r2,lr                ; first tile of the sprite
        TROWS   r3
        tst     r12,#0x10000
        beq     pa_rows
        add     r2,r2,#1
        bic     r2,r2,#0x200            ; the second tile of a tall sprite
        TROWS   r0
        orr     r3,r3,r0,lsl #8
pa_rows
        add     lr,r6,r4,lsl #2
        str     r3,[lr,#S_ROWS]
        cmp     r3,#0
        moveq   r3,#0x80                ; no opaque pixel: flagged in the chain
        movne   r3,#0
        orr     r3,r3,r4
        mov     r2,r1,lsr #16           ; y + 32
        add     lr,sp,#F_HEAD
        ldrb    r0,[lr,r2]
        strb    r3,[lr,r2]              ; the sprite heads the chain of its row
        add     lr,sp,#F_NEXT
        strb    r0,[lr,r4]
        add     r5,r5,#1
        mov     r1,r1,lsl #16
        mov     r1,r1,lsr #16           ; the range alone
pa_store
        add     lr,sp,#F_RANGE
        str     r1,[lr,r4,lsl #2]
        add     r4,r4,#1
        b       pa_loop
pa_done
        str     r4,[r6,#S_N]            ; sprites before the terminator
        mov     r4,#0                   ; r4 = flags

        ; The sprites on the screen by first row, from the chains of pass A
        ; (each row's chain is in reverse table order, which nothing below
        ; minds).
        ldr     r7,[sp,#F_ACTIVE]
        add     r7,r7,#32               ; r7 = chain bytes
        mov     r5,#0                   ; r5 = sprites ordered
        mov     r1,#0                   ; chain byte
        add     r2,sp,#F_HEAD
        add     r3,sp,#F_ORDER
        MACRO
        CHAIN   $shift                  ; the sprites starting on row r1
        [ $shift = 0
        mov     r12,r0
        |
        mov     r12,r0,lsr #$shift
        ]
        and     r12,r12,#0xFF
5       cmp     r12,#0xFF
        beq     %F6
        and     r6,r12,#0x3F            ; the sprite
        add     lr,sp,#F_RANGE
        ldr     lr,[lr,r6,lsl #2]
        orr     lr,r12,lr,lsl #16       ; sprite (| 0x80) | first << 16 | last + 1 << 24
        add     r12,r10,r6,lsl #1
        ldrb    r12,[r12,#0x80]
        orr     lr,lr,r12,lsl #8        ; | x << 8
        str     lr,[r3,r5,lsl #2]
        add     r5,r5,#1
        add     r12,sp,#F_NEXT
        ldrb    r12,[r12,r6]
        b       %B5
6       add     r1,r1,#1
        MEND

co_oword
        ldr     r0,[r2],#4              ; four rows of heads
        cmn     r0,#1
        addeq   r1,r1,#4
        beq     co_onext
        CHAIN   24
        CHAIN   16
        CHAIN   8
        CHAIN   0
co_onext
        cmp     r1,r7
        bne     co_oword
        str     r5,[sp,#F_NSORT]
        add     r6,r11,#0x4800
        add     r6,r6,#V_SPR-0x4800     ; r6 = &spr_n again

        ; Overflow: the sprites being ordered by first row, nine of them
        ; share a line exactly when the first screen line of some sprite
        ; is before the line after the last row of the sprite eight
        ; places before it.
        cmp     r5,#8
        bls     co_sorted               ; eight sprites at most on the screen
        add     r2,sp,#F_ORDER
        add     r3,r2,#32               ; sprite i, r2 = sprite i - 8
        add     r7,r2,r5,lsl #2         ; end
ov_loop
        ldr     r0,[r2],#4
        ldr     r1,[r3],#4
        and     r0,r0,#0x3F
        add     r0,r6,r0,lsl #2
        ldr     r0,[r0,#S_Y]
        add     r0,r0,r8                ; the line after the last row of i - 8
        mov     r1,r1,lsl #8
        mov     r1,r1,lsr #24           ; first screen line of i
        cmp     r1,r0
        blt     overflow
        cmp     r3,r7
        bne     ov_loop
        b       co_sorted
overflow
        ldr     r7,[r6,#S_N]            ; r7 = sprites before the terminator
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
        ; The counts of the lines are bit-sliced: four planes of 32-line
        ; words (bits 0-2 of the count, then "full": eight sprites met),
        ; in the place of the line counts, which are done with. A sprite
        ; counts on each of its lines a word at a time; on the lines
        ; already full it loses the line. A count stops at eight.
        MACRO
        PCWORD                          ; lr = planes of a word, r4 = the
        ldmia   lr,{r0,r2,r3,r12}       ; sprite's lines in it; out: r8 =
        and     r8,r4,r12               ; the lines it loses there
        bic     r4,r4,r12
        and     r1,r0,r4
        eor     r0,r0,r4
        and     r4,r2,r1
        eor     r2,r2,r1
        and     r1,r3,r4
        eor     r3,r3,r4
        orr     r12,r12,r1
        stmia   lr,{r0,r2,r3,r12}
        MEND

        mov     r0,#0
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        add     r12,sp,#F_PLANES
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        mov     r5,#0                   ; sprite
pc_loop
        cmp     r5,r7
        beq     pc_end
        add     lr,sp,#F_RANGE
        ldr     r1,[lr,r5,lsl #2]
        movs    r2,r1,lsr #8            ; last line + 1
        beq     pc_next
        and     r1,r1,#0xFF             ; first line
        sub     r2,r2,r1                ; lines (1 to 32)
        mvn     r0,#0
        rsb     r2,r2,#32
        mov     r0,r0,lsr r2            ; one bit per line
        and     r12,r1,#31
        mov     r4,r0,lsl r12           ; the lines in the first word
        rsb     r12,r12,#32
        mov     r0,r0,lsr r12           ; in the next one (a shift by 32 gives 0)
        str     r0,[sp,#F_M1]
        str     r1,[sp,#F_FIRST]
        add     lr,sp,#F_PLANES
        mov     r1,r1,lsr #5
        add     lr,lr,r1,lsl #4
        PCWORD
        str     r8,[sp,#F_LOST]
        ldr     r4,[sp,#F_M1]
        movs    r4,r4
        moveq   r8,#0
        beq     pc_lost
        add     lr,lr,#16
        PCWORD
pc_lost                                 ; lost: [F_LOST] in the first word, r8 in the next
        ldr     r0,[sp,#F_LOST]
        orrs    r1,r0,r8
        beq     pc_next                 ; the sprite keeps all its lines
        ldr     r1,[sp,#F_FIRST]
        and     r2,r1,#31
        mov     r0,r0,lsr r2
        rsb     r2,r2,#32
        orr     r0,r0,r8,lsl r2         ; lines lost, from the first line on
        add     r3,r6,r5,lsl #2
        ldr     r2,[r3,#S_Y]
        sub     r2,r1,r2                ; first row on the screen
        mov     r0,r0,lsl r2            ; rows lost
        ldr     r1,[r3,#S_VIS]
        bic     r1,r1,r0
        str     r1,[r3,#S_VIS]
        mov     r1,#1
        str     r1,[r6,#S_PARTIAL]
pc_next
        add     r5,r5,#1
        b       pc_loop
pc_end
        mov     r4,#ST_OVERFLOW         ; the flags, which r4 held

        ; The ordered sprites with an opaque pixel, into spr_order (the
        ; flagged words are dropped, the order kept); collisions are
        ; looked for among them.
co_sorted
        ldr     r5,[sp,#F_NSORT]
        add     r2,sp,#F_ORDER
        add     r3,r6,#0x800
        add     r3,r3,#S_ORDER-0x800
        add     r7,r2,r5,lsl #2
        movs    r5,#0                   ; r5 = sprites kept
        cmp     r2,r7
        beq     cp_done                 ; no sprite on the screen
cp_loop
        ldr     r0,[r2],#4
        tst     r0,#0x80
        streq   r0,[r3],#4
        addeq   r5,r5,#1
        cmp     r2,r7
        bne     cp_loop
cp_done
        str     r5,[r6,#S_NORD]
        cmp     r5,#2
        blo     done                    ; one sprite with opaque pixels at most
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
        add     lr,r6,#0x800
        add     lr,lr,#S_ORDER-0x800
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
        add     lr,r6,#0x800
        add     lr,lr,#S_ORDER-0x800
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

; tile_rows: r2 = tile; returns in lr the mask of its rows holding an
; opaque pixel, computed from the video RAM and cached (spr_tile_rows,
; spr_tile_ok). Uses r6 (&spr_n) and r11 (v); preserves r0-r5 and r12.
tile_rows
        stmfd   sp!,{r0-r5,r12,lr}
        add     r1,r6,#S_TOK
        mov     r0,#1
        strb    r0,[r1,r2]
        add     r1,r11,#V_VRAM
        add     r1,r1,r2,lsl #5         ; the eight rows of four planes
        ldmia   r1!,{r3,r4,r5,r12}
        mov     lr,#0
        cmp     r3,#0
        orrne   lr,lr,#1
        cmp     r4,#0
        orrne   lr,lr,#2
        cmp     r5,#0
        orrne   lr,lr,#4
        cmp     r12,#0
        orrne   lr,lr,#8
        ldmia   r1,{r3,r4,r5,r12}
        cmp     r3,#0
        orrne   lr,lr,#16
        cmp     r4,#0
        orrne   lr,lr,#32
        cmp     r5,#0
        orrne   lr,lr,#64
        cmp     r12,#0
        orrne   lr,lr,#128
        add     r1,r6,#S_TROWS
        strb    lr,[r1,r2]
        ldmfd   sp!,{r0-r5,r12,pc}

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
