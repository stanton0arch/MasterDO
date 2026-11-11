; Sprite evaluation of the SMS VDP (see vdp.h, vdp_sprites): which lines of
; each sprite the hardware shows (eight per line at most, in table order),
; the overflow flag and the collision flag.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  vdp_sprites

; vdp_state fields (vdp.h; offsets checked in vdp.c).
V_REG           EQU     0x40
V_STATUS        EQU     0x50
V_ACTIVE        EQU     0x18           ; active lines: 192, 224 or 240
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400
V_SPR           EQU     0x4874          ; spr_n, then the fields below
S_N             EQU     0
S_H             EQU     4
S_ZOOM          EQU     8
S_PARTIAL       EQU     12
S_Y             EQU     16              ; 64 words
S_VIS           EQU     272             ; 64 words
S_TROWS         EQU     528             ; 512 bytes: bit r: row r of the tile has an
                                        ; opaque pixel
S_TOK           EQU     1040            ; 512 bytes: cache entry valid
S_ORDER         EQU     0x714           ; 64 words: the sprites on the screen with an
                                        ; opaque pixel, by first row (see vdp.h)
S_NORD          EQU     0x814           ; their count

ST_OVERFLOW     EQU     0x40
ST_COLLIDE      EQU     0x20

; Stack frame (lines: up to 240). F_REC up to F_NEXT is cleared on entry
; (the clear may run into F_NEXT, which pass A writes after it).
F_PLANES        EQU     0               ; pass C: 8 x 4 words of bit-sliced counts
                                        ; of the sprites met on each line
F_M1            EQU     128             ; pass C: a sprite's lines in its second word
F_FIRST         EQU     132             ; its first line
F_LOST          EQU     136             ; the lines it loses in its first word
F_REC           EQU     256             ; 64 words: the record of each sprite on the
                                        ; screen, sprite | x << 8 | first line << 16
                                        ; | last line + 1 << 24, bit 7 set when it has
                                        ; no opaque pixel; 0 for the others
F_HEAD          EQU     512             ; 272 bytes: the sprite starting on row y, + 1,
                                        ; at y + 32 (0: none)
F_NEXT          EQU     784             ; 64 bytes: next sprite starting on the row, + 1
F_ORDER         EQU     848             ; 64 words: the records by first row
F_SIZE          EQU     1104

;----------------------------------------------------------------------------
; uint32 vdp_sprites(vdp_state *v)
;
; Fills spr_n, spr_h, spr_zoom, spr_partial, spr_y, spr_vis, spr_order and
; spr_nord, ORs the sprite flags into the status and returns them. The
; list ends at the $D0 terminator in the 192-line mode only. A pass over
; the sprites gives each one on the screen its screen line, its record and
; the opaque rows of its tiles, and chains those sprites by first row (a
; counting sort); the sprites off the screen are told apart by one
; unsigned compare of their table byte and skipped, which only leaves
; spr_y stale for them. In that order a line holds more than eight sprites
; exactly when the first screen line of some sprite is still covered by
; the sprite eight places before it (the sprites all have the same
; height), a test made while the records with an opaque pixel are copied
; to spr_order; when it fires, a second pass in table order, over the
; lines, marks the lines each sprite loses. Collisions are looked for
; among the sprites of spr_order: each one is compared with those
; starting before its last line, and when their columns overlap too the
; opaque pixels of their rows are tested where they meet.
;----------------------------------------------------------------------------
vdp_sprites
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#F_SIZE
        mov     r11,r0                  ; v
        ; No record and no chain head yet: 256 bytes of records, then the
        ; heads of rows -32 to active - 1, in 32-byte stores (the last may
        ; reach into F_NEXT).
        mov     r0,#0
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        mov     r4,#0
        mov     r5,#0
        mov     r6,#0
        mov     r7,#0
        add     r12,sp,#F_REC
        ldr     r9,[r11,#V_ACTIVE]
        add     lr,r9,#0x140
        sub     lr,lr,#1
        mov     lr,lr,lsr #5
cl_loop
        stmia   r12!,{r0-r7}
        subs    lr,lr,#1
        bne     cl_loop
        ldrb    r1,[r11,#V_REG+5]
        and     r1,r1,#0x7E
        add     r10,r11,#V_VRAM
        add     r10,r10,r1,lsl #7       ; r10 = sprite attribute table
        ldrb    r1,[r11,#V_REG+1]
        and     r2,r1,#1                ; zoom
        tst     r1,#2
        moveq   r8,#8
        movne   r8,#16
        mov     r8,r8,lsl r2            ; r8 = lines per sprite
        add     r6,r11,#0x4800
        add     r6,r6,#V_SPR-0x4800     ; r6 = &spr_n
        str     r8,[r6,#S_H]
        str     r2,[r6,#S_ZOOM]
        str     r0,[r6,#S_PARTIAL]
        ldrb    r12,[r11,#V_REG+6]
        and     r12,r12,#4
        mov     r12,r12,lsl #6          ; tile base
        and     r0,r1,#2
        orr     r12,r12,r0,lsl #15      ; r12 = tile base | 0x10000 when tall

        ; TROWS: the opaque rows of tile r2 into $dst, from the cache
        ; (spr_tile_rows), computed again when the entry is not valid or
        ; the tile was written (dirty byte). Clobbers lr.
        MACRO
        TROWS   $dst
        add     lr,r11,r2
        ldrb    lr,[lr,#V_DIRTY]
        add     $dst,r6,r2
        cmp     lr,#0                   ; not written since
        ldreqb  lr,[$dst,#S_TOK]
        cmpeq   lr,#1                   ; and the entry valid
        ldreqb  $dst,[$dst,#S_TROWS]
        beq     %F8
        bl      tile_rows               ; into lr and the cache
        mov     $dst,lr
8
        MEND

        ; Pass A, up to the terminator. A sprite is off the screen when
        ; its table byte y gives a screen line y + 1 from active to
        ; 256 - h (the sprites past that line wrap to the top and show
        ; their last rows): y - (active - 1) below 257 - h - active,
        ; unsigned. The others get their screen line, their record and
        ; the opaque rows of their tiles (a record flagged when it has
        ; none: it cannot collide), and are chained by first row, with a
        ; bit for each group of 16 rows holding a chain. The lines each
        ; sprite shows (spr_vis) are only written when some line holds
        ; more than eight sprites: nothing reads them otherwise.
        sub     r9,r9,#1                ; r9 = active - 1 (zoom is reloaded after)
        rsb     r7,r8,#256
        subs    r7,r7,r9                ; r7 = 257 - h - active, 0 when no sprite
        movmi   r7,#0                   ; can be off the screen
        mov     r4,#0                   ; r4 = sprite
        mov     r5,#0                   ; r5 = groups of 16 rows with a chain
pa_loop
        ldrb    r0,[r10,r4]
        sub     lr,r0,r9
        cmp     lr,r7
        bhs     pa_on                   ; on the screen
        cmp     r0,#0xD0                ; the terminator is off the screen in the
        beq     pa_done                 ; 192-line mode, on it in the others
pa_next
        add     r4,r4,#1
        cmp     r4,#64
        bne     pa_loop
pa_done
        str     r4,[r6,#S_N]            ; sprites before the terminator
        b       pa_end
pa_on
        add     r0,r0,#1
        rsb     lr,r8,#256
        cmp     r0,lr
        subgt   r0,r0,#256              ; screen line of the first row
        add     r1,r6,r4,lsl #2
        str     r0,[r1,#S_Y]
        movs    r1,r0
        movmi   r1,#0                   ; first line on the screen
        add     r2,r0,r8
        cmp     r2,r9
        addgt   r2,r9,#1                ; last line + 1
        mov     r1,r1,lsl #16
        orr     r1,r1,r2,lsl #24        ; the record so far
        add     r0,r0,#32
        add     lr,sp,#F_HEAD
        ldrb    r2,[lr,r0]
        add     r3,r4,#1
        strb    r3,[lr,r0]              ; the sprite heads the chain of its row
        add     lr,sp,#F_NEXT
        strb    r2,[lr,r4]
        mov     r0,r0,lsr #4
        mov     lr,#1
        orr     r5,r5,lr,lsl r0         ; the group of the row holds a chain
        add     lr,r10,r4,lsl #1
        ldrb    r0,[lr,#0x80]
        orr     r1,r1,r0,lsl #8         ; | x << 8
        orr     r1,r1,r4                ; | sprite
        ldrb    r2,[lr,#0x81]
        and     lr,r12,#0x100
        tst     r12,#0x10000
        beq     pa_short
        bic     r2,r2,#1
        add     r2,r2,lr                ; first tile of the tall sprite, even:
        ; the dirty bytes, cache entries and rows of its two tiles are one
        ; halfword of the words holding those of four tiles.
        bic     r0,r2,#3
        add     lr,r11,r0
        ldr     lr,[lr,#V_DIRTY]
        add     r0,r6,r0
        ldr     r3,[r0,#S_TOK]
        ldr     r0,[r0,#S_TROWS]
        tst     r2,#2
        movne   lr,lr,lsl #16
        movne   r3,r3,lsl #16
        movne   r0,r0,lsl #16           ; the pair in the top halfword
        movs    lr,lr,lsr #16           ; neither tile written since
        moveq   r3,r3,lsr #16
        eoreq   r3,r3,#0x100
        cmpeq   r3,#1                   ; and both entries valid
        moveq   r3,r0,lsr #16           ; the rows of the two
        beq     pa_flag
        bl      tile_rows               ; else both computed, into the cache
        mov     r3,lr
        add     r2,r2,#1
        bl      tile_rows
        orr     r3,r3,lr,lsl #8
        b       pa_flag
pa_short
        add     r2,r2,lr                ; the tile of the sprite
        TROWS   r3
pa_flag
        cmp     r3,#0
        orreq   r1,r1,#0x80             ; no opaque pixel
        add     lr,sp,#F_REC
        str     r1,[lr,r4,lsl #2]
        b       pa_next
pa_end
        mov     r4,#0                   ; r4 = flags

        ; The records of the sprites on the screen by first row, from the
        ; chains of pass A (each row's chain is in reverse table order,
        ; which nothing below minds), read in the groups of 16 rows that
        ; hold one.
        mov     r7,r5                   ; r7 = groups left
        add     r2,sp,#F_HEAD
        add     r3,sp,#F_ORDER          ; r3 = next ordered record
        add     r8,sp,#F_REC
        add     r9,sp,#F_NEXT
        MACRO
        CHAIN   $shift                  ; the sprites starting on the row of
        [ $shift = 24                   ; byte $shift of r0
        movs    r12,r0,lsr #24
        |
        [ $shift = 0
        ands    r12,r0,#0xFF
        |
        mov     r12,r0,lsr #$shift
        ands    r12,r12,#0xFF
        ]
        ]
        beq     %F6
5       sub     r12,r12,#1              ; the sprite
        ldr     lr,[r8,r12,lsl #2]
        str     lr,[r3],#4
        ldrb    r12,[r9,r12]
        cmp     r12,#0
        bne     %B5
6
        MEND

co_group
        movs    r7,r7,lsr #1            ; C: the group holds a chain
        bcs     co_gwords
        add     r2,r2,#16
        bne     co_group                ; Z: no group left
        b       co_gdone
co_gwords
        add     r1,r2,#16               ; the end of the group
co_oword
        ldr     r0,[r2],#4              ; four rows of heads
        cmp     r0,#0
        beq     co_onext
        CHAIN   24
        CHAIN   16
        CHAIN   8
        CHAIN   0
co_onext
        cmp     r2,r1
        bne     co_oword
        b       co_group
co_gdone

        ; The ordered records with an opaque pixel, into spr_order (the
        ; order kept). From the ninth record on, the sprites being ordered
        ; by first row, nine of them share a line exactly when the first
        ; screen line of some sprite is before the line after the last
        ; row of the sprite eight places before it.
        ldr     r9,[r6,#S_ZOOM]
        add     r2,sp,#F_ORDER          ; r2 = the ordered records
        mov     r7,r3                   ; r7 = their end
        add     r3,r6,#0x700
        add     r3,r3,#S_ORDER-0x700    ; r3 = spr_order
        mov     r5,#0                   ; r5 = sprites kept
        cmp     r2,r7
        beq     cp_done                 ; no sprite on the screen
        sub     r1,r7,r2
        cmp     r1,#32
        movhi   r1,#32
        add     r1,r2,r1                ; the end of the first eight
cp_first
        ldr     r0,[r2],#4
        tst     r0,#0x80
        streq   r0,[r3],#4
        addeq   r5,r5,#1
        cmp     r2,r1
        bne     cp_first
        cmp     r2,r7
        beq     cp_done
cp_rest
        ldr     r0,[r2],#4
        ldr     r1,[r2,#-36]            ; the record eight places before
        mov     lr,r0,lsl #8
        mov     lr,lr,lsr #24           ; first screen line
        cmp     lr,r1,lsr #24           ; before the end of that record:
        orrlo   r4,r4,#ST_OVERFLOW      ; nine sprites on the line
        tst     r0,#0x80
        streq   r0,[r3],#4
        addeq   r5,r5,#1
        cmp     r2,r7
        bne     cp_rest
cp_done
        str     r5,[r6,#S_NORD]
        tst     r4,#ST_OVERFLOW
        beq     co_collide

        ; Pass C: on the lines with more than eight sprites, in table
        ; order, the ninth and later lose the line.
        ldr     r7,[r6,#S_N]            ; r7 = sprites before the terminator
        ; Each sprite starts with the lines it has on the screen (a sprite
        ; with none is hidden entirely).
        mov     r5,#0
pc_vis
        add     lr,sp,#F_REC
        ldr     r1,[lr,r5,lsl #2]
        movs    r2,r1,lsr #24           ; last line + 1 (0: off the screen)
        moveq   r0,#0
        beq     pc_vstore
        mov     r1,r1,lsl #8
        mov     r1,r1,lsr #24           ; first line
        sub     r2,r2,r1                ; lines on the screen
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
        add     lr,sp,#F_REC
        ldr     r1,[lr,r5,lsl #2]
        movs    r2,r1,lsr #24           ; last line + 1
        beq     pc_next
        mov     r1,r1,lsl #8
        mov     r1,r1,lsr #24           ; first line
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
        ldr     r5,[r6,#S_NORD]

        ; Collisions among the sprites of spr_order.
co_collide
        cmp     r5,#2
        blo     done                    ; one sprite with opaque pixels at most
        ; Pairs: a against each b after it whose first line is before the
        ; last line of a. The columns meet when dx = x of b - x of a is
        ; within a sprite width w either way: with K = (w << 8) - 0x80,
        ; when (dx << 8) + K - 0x80 is below 2K (unsigned), which the x of
        ; a, kept less K - 0x80, gives with one compare.
        MACRO
$l      NEAR    $next                   ; lr = (dx << 8) + K - 0x80, r12 = b
$l      stmfd   sp!,{r0-r3}
        and     r0,r0,#0xFF
        and     r1,r12,#0xFF
        sub     r2,lr,r8
        add     r2,r2,#0x80
        mov     r2,r2,asr #8            ; dx
        bl      pair_test
        movs    r12,r0
        ldmfd   sp!,{r0-r3}
        beq     $next
        orr     r4,r4,#ST_COLLIDE
        b       done
        MEND

        MACRO
        PAIRA   $vis,$next              ; a: r1 = last line + 1 << 16, r2 = x
        ldr     r0,[r7],#4
        [ $vis = 1
        and     r12,r0,#0xFF
        add     r12,r6,r12,lsl #2
        ldr     r12,[r12,#S_VIS]
        cmp     r12,#0
        beq     $next                   ; hidden on every line
        ]
        mov     r1,r0,lsr #24
        mov     r1,r1,lsl #16
        and     r2,r0,#0xFF00
        sub     r2,r2,r8
        add     r2,r2,#0x80
        MEND

        mov     r0,#8
        mov     r8,r0,lsl r9
        mov     r8,r8,lsl #8
        sub     r8,r8,#0x80             ; r8 = K
        add     r7,r6,#0x700
        add     r7,r7,#S_ORDER-0x700    ; r7 = a
        add     r5,r7,r5,lsl #2         ; r5 = the end of the list
        tst     r4,#ST_OVERFLOW
        bne     cv_i                    ; some sprites lose lines
co_i
        add     r3,r7,#4                ; r3 = b
        cmp     r3,r5
        bhs     done
        PAIRA   0,co_i
co_j
        ldr     r12,[r3],#4
        and     lr,r12,#0xFF0000
        cmp     lr,r1
        bhs     co_i                    ; b starts after a ends, so do the rest
        and     lr,r12,#0xFF00
        sub     lr,lr,r2
        cmp     lr,r8,lsl #1
        blo     co_near
co_jnext
        cmp     r3,r5
        bne     co_j
        b       co_i
co_near NEAR    co_jnext

        ; The same where some sprites lose lines: those hidden on every
        ; line are left out.
cv_i
        add     r3,r7,#4
        cmp     r3,r5
        bhs     done
        PAIRA   1,cv_i
cv_j
        ldr     r12,[r3],#4
        and     lr,r12,#0xFF0000
        cmp     lr,r1
        bhs     cv_i
        and     lr,r12,#0xFF00
        sub     lr,lr,r2
        cmp     lr,r8,lsl #1
        bhs     cv_jnext
        and     r0,r12,#0xFF
        add     r0,r6,r0,lsl #2
        ldr     r0,[r0,#S_VIS]
        cmp     r0,#0
        ldrne   r0,[r7,#-4]             ; a again
        bne     cv_near
cv_jnext
        cmp     r3,r5
        bne     cv_j
        b       cv_i
cv_near NEAR    cv_jnext

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
        add     r12,sp,#36+F_REC        ; the caller pushed r0-r3 as well
        ldr     r3,[r12,r0,lsl #2]
        ldr     r12,[r12,r1,lsl #2]
        mov     r4,r12,lsr #24          ; end: the lower of the two last lines + 1
        cmp     r4,r3,lsr #24
        movhi   r4,r3,lsr #24
        mov     r5,r12,lsl #8
        mov     r5,r5,lsr #24           ; line: the first line of b
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
        ; Rows with opaque pixels of the two (from the cache, which pass A
        ; filled for the sprites on the screen), aligned on their lines:
        ; when none meet there is nothing to test (zoomed sprites skip
        ; this).
        cmp     r9,#0
        bne     pt_line
        stmfd   sp!,{r2,r4}
        ldrb    r4,[r11,#V_REG+1]
        tst     r4,#2
        add     lr,r6,#S_TROWS
        ldrb    r3,[lr,r7]              ; the rows of a
        ldrb    r12,[lr,r8]             ; of b
        beq     pt_rows
        add     lr,lr,#1                ; the second tiles of tall sprites
        ldrb    r4,[lr,r7]
        orr     r3,r3,r4,lsl #8
        ldrb    r4,[lr,r8]
        orr     r12,r12,r4,lsl #8
pt_rows
        ldr     r4,[r1,#S_Y]
        ldr     r2,[r0,#S_Y]
        subs    r4,r4,r2                ; first row of b - first row of a
        rsbmi   r4,r4,#0
        movpl   r12,r12,lsl r4
        movmi   r3,r3,lsl r4
        tst     r3,r12
        ldmfd   sp!,{r2,r4}
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
