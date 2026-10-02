; render_a.s - hot loops of the cel renderer (render.c) in assembly.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  render_nt_scan
        EXPORT  render_dirty_scan
        EXPORT  render_sprites
        EXPORT  render_entries
        EXPORT  render_tile_cells
        EXPORT  render_rebuild_cells
        EXPORT  render_stale_cells
        EXPORT  render_tile_conv
        EXPORT  render_rect_pieces
        EXPORT  render_sprite_runs
        EXPORT  render_prio_patches

; CCB fields (graphics.h).
CCB_SOURCE      EQU     8
CCB_XPOS        EQU     16
CCB_YPOS        EQU     20
CCB_PRE0        EQU     52
CCB_PRE1        EQU     56
CCB_SIZE        EQU     68

; layer fields (render_layer, render.h; offsets checked in render.c).
R_BITMAP        EQU     0
R_PRIOBM        EQU     4
R_TILES         EQU     8
R_TILESF        EQU     12
R_SHADOW        EQU     16
R_TOK           EQU     20
R_HEAD          EQU     24
R_NEXT          EQU     28
R_PREV          EQU     32
R_WLIST         EQU     36
R_STAMP         EQU     44
R_STAMPNOW      EQU     48
R_XTAB          EQU     52
R_PRIOROWS      EQU     68
R_PRIODIRTY     EQU     152
R_NTBASE        EQU     56
R_STCELLS       EQU     180
R_STTILECELLS   EQU     184
R_STTILESCONV   EQU     188
R_STALE         EQU     308
R_STALEROWS     EQU     312
R_SHOWNROWS     EQU     316

; render_sprite_args fields (render.h).
SA_TILES        EQU     0
SA_TOK          EQU     4
SA_TBASE        EQU     8
SA_TMASK        EQU     12
SA_XOFF         EQU     16
SA_NEED         EQU     20
SA_TALL         EQU     24
SA_YWRAP        EQU     28
SA_END          EQU     32
SA_DROPPED      EQU     36
SA_NEEDED       EQU     40

;----------------------------------------------------------------------------
; uint32 render_nt_scan(const uint32 *nt, const uint32 *shadow,
;                       const uint32 *mask, uint32 *out)
;
; Compares the 56 chunks of 8 words of the name table with the shadow copy,
; for the chunks whose bit is set in mask (two words, chunk 0 = bit 0 of
; the first); appends the index (0-447) of each differing word to out and
; returns their count. Nothing is written to the shadow.
;----------------------------------------------------------------------------
render_nt_scan
        stmfd   sp!,{r4-r11,lr}
        ldr     r12,[r2]                ; chunks 0-31
        ldr     r4,[r2,#4]              ; chunks 32-55
        stmfd   sp!,{r3,r4}
        mov     r2,r3                   ; output pointer
        mov     r3,#0                   ; word index
        mov     r10,#56                 ; chunks left
scan_next
        cmp     r10,#24
        ldreq   r12,[sp,#4]
        movs    r12,r12,lsr #1
        bcc     scan_clean
        ldmia   r0!,{r4-r7}
        ldmia   r1!,{r8,r9,r11,lr}
        cmp     r4,r8
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r5,r9
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r6,r11
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r7,lr
        strne   r3,[r2],#4
        add     r3,r3,#1
        ldmia   r0!,{r4-r7}
        ldmia   r1!,{r8,r9,r11,lr}
        cmp     r4,r8
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r5,r9
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r6,r11
        strne   r3,[r2],#4
        add     r3,r3,#1
        cmp     r7,lr
        strne   r3,[r2],#4
        add     r3,r3,#1
        subs    r10,r10,#1
        bne     scan_next
        b       scan_done
scan_clean
        add     r0,r0,#32
        add     r1,r1,#32
        add     r3,r3,#8
        subs    r10,r10,#1
        bne     scan_next
scan_done
        ldmfd   sp!,{r0,r1}
        sub     r0,r2,r0
        mov     r0,r0,lsr #2
        ldmfd   sp!,{r4-r11,pc}


;----------------------------------------------------------------------------
; uint32 render_dirty_scan(uint8 *dirty, uint32 *out)
;
; Appends the index of every nonzero byte of the 512-byte dirty tile array
; to out and clears the array; returns the count.
;----------------------------------------------------------------------------
        MACRO
        CHECKW  $reg
        tst     $reg,#0xFF000000
        strne   lr,[r12],#4
        add     lr,lr,#1
        tst     $reg,#0x00FF0000
        strne   lr,[r12],#4
        add     lr,lr,#1
        tst     $reg,#0x0000FF00
        strne   lr,[r12],#4
        add     lr,lr,#1
        tst     $reg,#0x000000FF
        strne   lr,[r12],#4
        add     lr,lr,#1
        MEND

render_dirty_scan
        stmfd   sp!,{r4-r11,lr}
        mov     r12,r1                  ; output pointer
        mov     lr,#0                   ; tile index
dscan_group
        ldmia   r0,{r3-r10}
        orr     r11,r3,r4
        orr     r11,r11,r5
        orr     r11,r11,r6
        orr     r11,r11,r7
        orr     r11,r11,r8
        orr     r11,r11,r9
        orrs    r11,r11,r10
        addeq   r0,r0,#32
        addeq   lr,lr,#32
        beq     dscan_next
        CHECKW  r3
        CHECKW  r4
        CHECKW  r5
        CHECKW  r6
        CHECKW  r7
        CHECKW  r8
        CHECKW  r9
        CHECKW  r10
        mov     r3,#0
        mov     r4,#0
        mov     r5,#0
        mov     r6,#0
        mov     r7,#0
        mov     r8,#0
        mov     r9,#0
        mov     r10,#0
        stmia   r0!,{r3-r10}
dscan_next
        cmp     lr,#512
        bne     dscan_group
        sub     r0,r12,r1
        mov     r0,r0,lsr #2
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; uint32 render_sprites(const uint8 *sat, uint32 n, CCB *cel,
;                       const render_sprite_args *a)
;
; Sets the source, X and Y of the cels of sprites 0 to n - 1 from the
; sprite attribute table; appends to a->need the tiles (first tile of the
; sprite) whose conversion is missing (a->t_ok bit 0 clear for the tile,
; or for the second one of a tall sprite) and returns their count.
;----------------------------------------------------------------------------
render_sprites
        stmfd   sp!,{r4-r11,lr}
        ldmia   r3,{r4-r10}             ; tiles, t_ok, tbase, tmask, xoff, need, tall
        ldr     r3,[r3,#SA_YWRAP]
        add     r10,r5,r10              ; t_ok + 1 for tall sprites, else t_ok
        stmfd   sp!,{r3,r9}             ; Y wrap, start of the need list
        add     r3,r0,#0x80             ; x / tile pairs
        movs    r1,r1
        beq     spr_done
spr_loop
        ldrb    r11,[r3],#1             ; x
        ldrb    r12,[r3],#1             ; tile
        and     r12,r12,r7
        add     r12,r12,r6
        ldrb    lr,[r5,r12]             ; conversion of the tile(s) present?
        tst     lr,#1
        ldrneb  lr,[r10,r12]
        tstne   lr,#1
        streq   r12,[r9],#4
        add     lr,r4,r12,lsl #6
        str     lr,[r2,#CCB_SOURCE]
        sub     r11,r11,r8
        mov     r11,r11,lsl #16
        str     r11,[r2,#CCB_XPOS]
        ldrb    r12,[r0],#1             ; y
        ldr     lr,[sp]                 ; a sprite past the wrap shows at the top
        add     r12,r12,#1
        cmp     r12,lr
        subgt   r12,r12,#256
        mov     r12,r12,lsl #16
        str     r12,[r2,#CCB_YPOS]
        add     r2,r2,#CCB_SIZE
        subs    r1,r1,#1
        bne     spr_loop
spr_done
        ldmfd   sp!,{r0,r1}
        sub     r0,r9,r1
        mov     r0,r0,lsr #2
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; Cells. Common registers of the routines below: r9 = layer, r10 = VRAM.
;
; cell_draw: draws entry r1 into cell r0 of the bitmap (and of the priority
; bitmap when the entry has the priority bit): the tile is converted first
; when its store is missing (render_tile_conv, C), the palette bit is OR-ed
; into every pixel, the flips are applied by the copy (the mirrored tile
; store, the row order). The cell is stamped and counted. Preserves
; r9-r11, clobbers r0-r8 and r12.
;----------------------------------------------------------------------------
NONE            EQU     896             ; end of a cell list
CELL_ROW        EQU     256             ; bytes per bitmap row

        MACRO
        ROWCOPY
        ldmia   r3,{r0,r1}
        orr     r0,r0,r7
        orr     r1,r1,r7
        add     r3,r3,r4
        stmia   r6,{r0,r1}
        add     r6,r6,#CELL_ROW
        MEND

        MACRO
        ROWCOPYP
        ldmia   r3,{r0,r1}
        orr     r0,r0,r7
        orr     r1,r1,r7
        add     r3,r3,r4
        stmia   r6,{r0,r1}
        add     r6,r6,#CELL_ROW
        stmia   r8,{r0,r1}
        add     r8,r8,#CELL_ROW
        MEND

cell_draw
        stmfd   sp!,{r0,r1,lr}
        bic     r2,r1,#0xFE00           ; tile (entries are 16-bit)
        tst     r1,#0x200               ; flip bit -> store bit 1 or 2
        moveq   r3,#1
        movne   r3,#2
        ldr     r4,[r9,#R_TOK]
        ldrb    r5,[r4,r2]
        tst     r5,r3
        bne     cd_converted
        mov     r0,r9                   ; render_tile_conv(r, t, flip, vram)
        mov     r1,r2
        mov     r2,r3,lsr #1
        mov     r3,r10
        bl      render_tile_conv
        ldmia   sp,{r0,r1}
        bic     r2,r1,#0xFE00
cd_converted
        tst     r1,#0x200
        ldreq   r3,[r9,#R_TILES]
        ldrne   r3,[r9,#R_TILESF]
        add     r3,r3,r2,lsl #6         ; source: tile store + t * 64
        mov     r4,#8                   ; bytes per source row
        tst     r1,#0x400               ; vertical flip: last row first
        addne   r3,r3,#56
        mvnne   r4,#7
        mov     r5,r0,lsr #5            ; cell offset: row * 8 rows + column * 8
        and     r6,r0,#31
        mov     r5,r5,lsl #11
        add     r5,r5,r6,lsl #3
        ldr     r6,[r9,#R_BITMAP]
        add     r6,r6,r5
        mov     r7,#0x10                ; palette bits, one per pixel
        orr     r7,r7,r7,lsl #8
        orr     r7,r7,r7,lsl #16
        tst     r1,#0x800
        moveq   r7,#0
        tst     r1,#0x1000
        beq     cd_plain
        ldr     r8,[r9,#R_PRIOBM]
        add     r8,r8,r5
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        b       cd_stamp
cd_plain
        ROWCOPY
        ROWCOPY
        ROWCOPY
        ROWCOPY
        ROWCOPY
        ROWCOPY
        ROWCOPY
        ROWCOPY
cd_stamp
        ldmfd   sp!,{r0,r1,lr}
        ldr     r2,[r9,#R_STAMP]
        ldr     r3,[r9,#R_STAMPNOW]
        strb    r3,[r2,r0]
        ldr     r2,[r9,#R_STCELLS]
        add     r2,r2,#1
        str     r2,[r9,#R_STCELLS]
        mov     pc,lr

;----------------------------------------------------------------------------
; cell_change: cell r0 goes from entry r2 to entry r1: re-linked in the
; cell list of its tile when the tile changed, the priority count of its
; row and the row's dirty flag updated when the priority bit changed (the
; cell cleared in the priority bitmap when the bit went away), then drawn.
; Preserves r9-r11.
;----------------------------------------------------------------------------
cell_change
        stmfd   sp!,{r0,r1,lr}
        ldr     r4,[r9,#R_STALE]        ; drawn below: no longer stale
        mov     r5,#0
        strb    r5,[r4,r0]
        eor     r3,r1,r2
        movs    r4,r3,lsl #23           ; tile bits 0-8 differ?
        beq     cc_prio
        bic     r4,r2,#0xFE00           ; unlink from the old tile's list
        ldr     r5,[r9,#R_PREV]
        ldr     r6,[r9,#R_NEXT]
        ldr     r7,[r9,#R_HEAD]
        ldr     r8,[r5,r0,lsl #2]       ; p
        ldr     r12,[r6,r0,lsl #2]      ; n
        cmp     r8,#NONE
        streq   r12,[r7,r4,lsl #2]      ; head[old] = n
        strne   r12,[r6,r8,lsl #2]      ; next[p] = n
        cmp     r12,#NONE
        strne   r8,[r5,r12,lsl #2]      ; prev[n] = p
        bic     r4,r1,#0xFE00           ; link at the head of the new tile's list
        ldr     r8,[r7,r4,lsl #2]       ; h
        mov     r12,#NONE
        str     r12,[r5,r0,lsl #2]      ; prev[i] = NONE
        str     r8,[r6,r0,lsl #2]       ; next[i] = h
        cmp     r8,#NONE
        strne   r0,[r5,r8,lsl #2]       ; prev[h] = i
        str     r0,[r7,r4,lsl #2]       ; head[new] = i
cc_prio
        tst     r3,#0x1000
        beq     cc_draw
        mov     r4,r0,lsr #5            ; row
        add     r5,r9,#R_PRIOROWS
        ldrb    r6,[r5,r4]
        tst     r1,#0x1000
        addne   r6,r6,#1
        subeq   r6,r6,#1
        strb    r6,[r5,r4]
        add     r5,r9,#R_PRIODIRTY
        mov     r6,#1
        strb    r6,[r5,r4]
        tst     r1,#0x1000
        bne     cc_draw
        mov     r5,r0,lsr #5            ; the bit went away: clear the cell
        and     r6,r0,#31
        mov     r5,r5,lsl #11
        add     r5,r5,r6,lsl #3
        ldr     r6,[r9,#R_PRIOBM]
        add     r6,r6,r5
        mov     r7,#0
        mov     r8,#0
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
        add     r6,r6,#CELL_ROW
        stmia   r6,{r7,r8}
cc_draw
        ldmfd   sp!,{r0,r1,lr}
        b       cell_draw

;----------------------------------------------------------------------------
; void render_entries(renderer *r, const uint8 *vram, uint32 n)
;
; For each of the n name table word indices in r->wlist: the word is
; copied to the shadow and each of its two entries that differs from the
; shadow goes through cell_change.
;----------------------------------------------------------------------------
render_entries
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r11,[r9,#R_WLIST]
        ldr     r4,[r9,#R_NTBASE]
        add     r4,r10,r4               ; name table
        ldr     r5,[r9,#R_SHADOW]
        stmfd   sp!,{r2,r4,r5}          ; count, name table, shadow
re_loop
        ldmia   sp,{r2,r4,r5}
        subs    r2,r2,#1
        bmi     re_done
        str     r2,[sp]
        ldr     r0,[r11],#4             ; word index
        ldr     r6,[r4,r0,lsl #2]       ; new word
        ldr     r7,[r5,r0,lsl #2]       ; old word
        str     r6,[r5,r0,lsl #2]
        mov     r1,r6,lsr #24           ; first entry: low byte first
        and     r2,r6,#0x00FF0000
        orr     r1,r1,r2,lsr #8
        mov     r2,r7,lsr #24
        and     r3,r7,#0x00FF0000
        orr     r2,r2,r3,lsr #8
        cmp     r1,r2
        beq     re_second
        stmfd   sp!,{r0,r6,r7}
        mov     r0,r0,lsl #1
        bl      cell_change
        ldmfd   sp!,{r0,r6,r7}
re_second
        and     r1,r6,#0xFF00           ; second entry
        and     r2,r6,#0xFF
        mov     r1,r1,lsr #8
        orr     r1,r1,r2,lsl #8
        and     r2,r7,#0xFF00
        and     r3,r7,#0xFF
        mov     r2,r2,lsr #8
        orr     r2,r2,r3,lsl #8
        cmp     r1,r2
        beq     re_loop
        mov     r0,r0,lsl #1
        add     r0,r0,#1
        bl      cell_change
        b       re_loop
re_done
        add     sp,sp,#12
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_tile_cells(renderer *r, const uint8 *vram, uint32 t)
;
; Draws the cells of the list of tile t that are not stamped in this
; update, with their entry from the shadow; counts them in st.tile_cells.
; A cell of a row the update does not show (shown_rows) is left stale
; instead, for render_stale_cells.
;----------------------------------------------------------------------------
render_tile_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r4,[r9,#R_HEAD]
        ldr     r11,[r4,r2,lsl #2]
tc_loop
        cmp     r11,#NONE
        beq     tc_done
        ldr     r2,[r9,#R_STAMP]
        ldrb    r3,[r2,r11]
        ldr     r4,[r9,#R_STAMPNOW]
        and     r4,r4,#0xFF
        cmp     r3,r4
        beq     tc_next
        ldr     r2,[r9,#R_SHOWNROWS]
        mov     r3,r11,lsr #5           ; row shown?
        mov     r2,r2,lsr r3
        tst     r2,#1
        bne     tc_draw
        ldr     r2,[r9,#R_STALE]        ; no: stale until it is
        mov     r4,#1
        strb    r4,[r2,r11]
        ldr     r2,[r9,#R_STALEROWS]
        orr     r2,r2,r4,lsl r3
        str     r2,[r9,#R_STALEROWS]
        b       tc_next
tc_draw
        ldr     r2,[r9,#R_SHADOW]
        add     r2,r2,r11,lsl #1
        ldrb    r1,[r2]
        ldrb    r3,[r2,#1]
        orr     r1,r1,r3,lsl #8
        mov     r0,r11
        bl      cell_draw
        ldr     r2,[r9,#R_STTILECELLS]
        add     r2,r2,#1
        str     r2,[r9,#R_STTILECELLS]
tc_next
        ldr     r2,[r9,#R_NEXT]
        ldr     r11,[r2,r11,lsl #2]
        b       tc_loop
tc_done
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_stale_cells(renderer *r, const uint8 *vram)
;
; Draws the stale cells of the rows the update shows (those not stamped
; yet), from the shadow; those rows hold no stale cell afterwards.
;----------------------------------------------------------------------------
render_stale_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r4,[r9,#R_STALEROWS]
        ldr     r5,[r9,#R_SHOWNROWS]
        and     r11,r4,r5               ; rows to visit
        bic     r4,r4,r5
        str     r4,[r9,#R_STALEROWS]
        mov     r4,#0                   ; row
sc_rows
        movs    r11,r11,lsr #1
        bcc     sc_next_row
        mov     r5,r4,lsl #5            ; first cell of the row
        add     r6,r5,#32
sc_cells
        ldr     r2,[r9,#R_STALE]
        ldrb    r3,[r2,r5]
        teq     r3,#0
        beq     sc_next_cell
        mov     r3,#0
        strb    r3,[r2,r5]
        ldr     r2,[r9,#R_STAMP]
        ldrb    r3,[r2,r5]
        ldr     r2,[r9,#R_STAMPNOW]
        and     r2,r2,#0xFF
        cmp     r3,r2
        beq     sc_next_cell
        ldr     r2,[r9,#R_SHADOW]
        add     r2,r2,r5,lsl #1
        ldrb    r1,[r2]
        ldrb    r3,[r2,#1]
        orr     r1,r1,r3,lsl #8
        mov     r0,r5
        stmfd   sp!,{r4-r6}             ; cell_draw clobbers r0-r8
        bl      cell_draw
        ldmfd   sp!,{r4-r6}
        ldr     r2,[r9,#R_STTILECELLS]
        add     r2,r2,#1
        str     r2,[r9,#R_STTILECELLS]
sc_next_cell
        add     r5,r5,#1
        cmp     r5,r6
        bne     sc_cells
sc_next_row
        add     r4,r4,#1
        teq     r11,#0
        bne     sc_rows
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_tile_conv(render_layer *l, uint32 t, uint32 flip, const uint8 *vram)
;
; Converts tile t from the VDP's four bit planes (4 bytes per row) to 8
; bpp rows of two words in the layer's tile store (the mirrored store
; with flip), through the expansion tables: hi[p] spreads the high nibble
; of plane byte p into the bytes of a word, lo[p] its low nibble, and the
; planes are OR-ed in at bit 0 to 3 of each pixel. Marks the store of the
; tile converted and counts the conversion.
;----------------------------------------------------------------------------
        MACRO
        CONVROW
        ldr     r5,[r0],#4              ; p0 p1 p2 p3 (p0 in the top byte)
        mov     r6,r5,lsr #24
        ldr     r7,[r2,r6,lsl #2]       ; hi[p0]
        ldr     r8,[r3,r6,lsl #2]       ; lo[p0]
        mov     r6,r5,lsl #8
        mov     r6,r6,lsr #24
        ldr     r12,[r2,r6,lsl #2]
        orr     r7,r7,r12,lsl #1
        ldr     r12,[r3,r6,lsl #2]
        orr     r8,r8,r12,lsl #1
        mov     r6,r5,lsl #16
        mov     r6,r6,lsr #24
        ldr     r12,[r2,r6,lsl #2]
        orr     r7,r7,r12,lsl #2
        ldr     r12,[r3,r6,lsl #2]
        orr     r8,r8,r12,lsl #2
        and     r6,r5,#0xFF
        ldr     r12,[r2,r6,lsl #2]
        orr     r7,r7,r12,lsl #3
        ldr     r12,[r3,r6,lsl #2]
        orr     r8,r8,r12,lsl #3
        stmia   r4!,{r7,r8}
        MEND

render_tile_conv
        stmfd   sp!,{r4-r8,lr}
        ldr     r12,[r0,#R_STTILESCONV]
        add     r12,r12,#1
        str     r12,[r0,#R_STTILESCONV]
        ldr     r12,[r0,#R_TOK]         ; t_ok[t] |= 1 << flip
        ldrb    r5,[r12,r1]
        mov     r6,#1
        orr     r5,r5,r6,lsl r2
        strb    r5,[r12,r1]
        ldr     r12,[r0,#R_XTAB]
        teq     r2,#0
        ldreq   r4,[r0,#R_TILES]
        ldrne   r4,[r0,#R_TILESF]
        addne   r12,r12,#2048           ; the mirrored tables
        add     r4,r4,r1,lsl #6         ; destination: store + t * 64
        add     r0,r3,r1,lsl #5         ; source: vram + t * 32
        mov     r2,r12                  ; hi
        add     r3,r12,#1024            ; lo
        CONVROW
        CONVROW
        CONVROW
        CONVROW
        CONVROW
        CONVROW
        CONVROW
        CONVROW
        ldmfd   sp!,{r4-r8,pc}

;----------------------------------------------------------------------------
; uint32 render_rect_pieces(piece_ctx *p, const uint8 *bm, uint32 bx0,
;                           uint32 bx1, uint32 by0, uint32 by1, uint32 lb,
;                           uint32 vs)
;
; The usual case of rect_pieces (render.c): no scroll inhibit, the whole
; width for window. The rectangle of bitmap columns bx0 to bx1 - 1 and
; rows by0 to by1 - 1 is shown on the lines of line band lb (ya | yb << 8
; | hs << 16) as pieces appended to p (next cel at [p], capacity at
; [p + 4]), split at the row and column wraps, the part from column
; (bx0 + hs) & 255 before the part wrapped to the left edge. Returns the
; number of pieces dropped for lack of cels.
;----------------------------------------------------------------------------
        MACRO
        EMITP   $sa                     ; sy r3, height r4, src_y r2, sb r9
        ldr     r12,[r0]
        ldr     lr,[r0,#4]
        cmp     r12,lr
        addeq   r7,r7,#1
        beq     %F7
        add     lr,r12,#CCB_SIZE
        str     lr,[r0]
        sub     lr,$sa,r6
        and     lr,lr,#255              ; bitmap column of sa
        and     r5,lr,#3                ; pixels before the word boundary
        sub     lr,lr,r5
        ldr     r8,[sp,#20]             ; bitmap
        add     r8,r8,r2,lsl #8
        add     r8,r8,lr
        str     r8,[r12,#CCB_SOURCE]
        sub     r5,$sa,r5               ; x of the piece
        sub     r8,r9,r5                ; its width
        mov     r5,r5,lsl #16
        str     r5,[r12,#CCB_XPOS]
        mov     r5,r3,lsl #16
        str     r5,[r12,#CCB_YPOS]
        sub     r5,r4,#1
        mov     r5,r5,lsl #6
        orr     r5,r5,#5                ; PRE0: height - 1, 8 bpp
        str     r5,[r12,#CCB_PRE0]
        sub     r8,r8,#1
        orr     r8,r8,#0x3E0000         ; PRE1: 64 - 2 words per row, width - 1
        orr     r8,r8,#0x1000
        str     r8,[r12,#CCB_PRE1]
7
        MEND

        MACRO
        PART                            ; iy0 r12, iy1 lr, src_y r2
        stmfd   sp!,{r3,r4,r5,r8,r9}
        mov     r3,r12
        cmp     r3,r8
        movlo   r3,r8                   ; sy = max(iy0, ya)
        cmp     lr,r5
        movhi   lr,r5                   ; ey = min(iy1, yb)
        cmp     r3,lr
        bhs     %F9
        add     r2,r2,r3
        sub     r2,r2,r12               ; rows from src_y + sy - iy0
        sub     r4,lr,r3                ; height
        add     r9,r10,r11
        cmp     r9,#256
        movhi   r9,#256                 ; end of the part from column t0
        cmp     r10,r9
        bhs     %F8
        EMITP   r10
8
        add     r9,r10,r11
        subs    r9,r9,#256              ; the part wrapped to the left edge
        bls     %F9
        mov     r1,#0
        EMITP   r1
9
        ldmfd   sp!,{r3,r4,r5,r8,r9}
        MEND

render_rect_pieces
        stmfd   sp!,{r1,r4-r11,lr}
        add     r12,sp,#40
        ldmia   r12,{r4,r5,r6,r7}       ; by0, by1, lb, vs
        sub     r9,r5,r4                ; rows
        sub     r11,r3,r2               ; columns
        and     r8,r6,#0xFF             ; ya
        mov     r5,r6,lsr #8
        and     r5,r5,#0xFF             ; yb
        mov     r6,r6,lsr #16
        and     r6,r6,#0xFF             ; hs
        add     r10,r2,r6
        and     r10,r10,#0xFF           ; t0: screen column of bx0
        add     r3,r4,#448
        sub     r3,r3,r7                ; s0: screen line of by0, modulo 224
rp_mod
        cmp     r3,#224
        subhs   r3,r3,#224
        bhs     rp_mod
        mov     r7,#0                   ; pieces dropped
        mov     r12,r3                  ; lines s0 to min(s0 + rows, 224) - 1
        add     lr,r3,r9
        cmp     lr,#224
        movhi   lr,#224
        mov     r2,r4
        PART
        add     lr,r3,r9                ; the rest wraps to the top
        cmp     lr,#224
        bls     rp_done
        sub     lr,lr,#224
        mov     r12,#0
        rsb     r2,r3,#224
        add     r2,r2,r4
        PART
rp_done
        mov     r0,r7
        ldmfd   sp!,{r1,r4-r11,pc}

;----------------------------------------------------------------------------
; void render_rebuild_cells(renderer *r, const uint8 *vram)
;
; Links every cell into the list of its tile (the heads must hold NONE),
; counts the priority cells of each row and draws every cell, from the
; entries of the shadow.
;----------------------------------------------------------------------------
render_rebuild_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        mov     r11,#0
rb_loop
        ldr     r2,[r9,#R_SHADOW]
        add     r2,r2,r11,lsl #1
        ldrb    r1,[r2]
        ldrb    r3,[r2,#1]
        orr     r1,r1,r3,lsl #8
        bic     r4,r1,#0xFE00
        ldr     r5,[r9,#R_PREV]
        ldr     r6,[r9,#R_NEXT]
        ldr     r7,[r9,#R_HEAD]
        ldr     r8,[r7,r4,lsl #2]
        mov     r12,#NONE
        str     r12,[r5,r11,lsl #2]
        str     r8,[r6,r11,lsl #2]
        cmp     r8,#NONE
        strne   r11,[r5,r8,lsl #2]
        str     r11,[r7,r4,lsl #2]
        tst     r1,#0x1000
        beq     rb_draw
        mov     r4,r11,lsr #5
        add     r5,r9,#R_PRIOROWS
        ldrb    r6,[r5,r4]
        add     r6,r6,#1
        strb    r6,[r5,r4]
rb_draw
        mov     r0,r11
        bl      cell_draw
        add     r11,r11,#1
        cmp     r11,#NONE
        bne     rb_loop
        ldmfd   sp!,{r4-r11,pc}

        ;----------------------------------------------------------------------------
; CCB *render_sprite_runs(const uint8 *sat, const vdp_state *v, CCB *cel,
;                         render_sprite_args *a)
;
; Cels of the sprites evaluated by the VDP (spr_* fields of v) when some
; are hidden on some of their lines: one cel per run of shown lines of
; each sprite, reading the rows of the run; a zoomed sprite's run is
; rounded to whole doubled rows. Tiles whose conversion is missing are
; appended to a->need as in render_sprites; a->dropped counts the runs
; beyond a->end. Returns the cel after the last one set up.
;----------------------------------------------------------------------------
V_SPR           EQU     0x4874          ; vdp_state.spr_n, then:
S_N             EQU     0
S_H             EQU     4
S_ZOOM          EQU     8
S_Y             EQU     16
S_VIS           EQU     272

render_sprite_runs
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#24               ; need start, tile, x, y, end, k
        ldr     r4,[r3,#SA_TILES]
        ldr     r5,[r3,#SA_TOK]
        ldr     r6,[r3,#SA_TALL]
        add     r6,r5,r6                ; t_ok + 1 for tall sprites
        ldr     r9,[r3,#SA_NEED]
        str     r9,[sp,#0]
        ldr     lr,[r3,#SA_END]
        str     lr,[sp,#16]
        add     r11,r1,#0x4800
        add     r11,r11,#V_SPR-0x4800   ; &spr_n
        ldr     r10,[r11,#S_ZOOM]
        mov     r1,#0
        str     r1,[sp,#20]
rs_loop
        ldr     r1,[sp,#20]
        ldr     lr,[r11,#S_N]
        cmp     r1,lr
        bhs     rs_done
        add     lr,r11,r1,lsl #2
        ldr     r12,[lr,#S_VIS]
        ldr     lr,[lr,#S_Y]
        str     lr,[sp,#12]
        movs    r12,r12
        beq     rs_next
        ldr     lr,[r3,#SA_XOFF]
        add     r7,r0,r1,lsl #1
        ldrb    r8,[r7,#0x80]
        sub     r8,r8,lr
        mov     r8,r8,lsl #16
        str     r8,[sp,#8]
        ldrb    r7,[r7,#0x81]
        ldr     lr,[r3,#SA_TMASK]
        and     r7,r7,lr
        ldr     lr,[r3,#SA_TBASE]
        add     r7,r7,lr                ; tile
        str     r7,[sp,#4]
        ldrb    lr,[r5,r7]
        tst     lr,#1
        ldrneb  lr,[r6,r7]
        tstne   lr,#1
        streq   r7,[r9],#4              ; conversion missing
        ldr     lr,[r11,#S_H]
        cmp     lr,#32
        mvnhs   lr,#0
        movlo   r7,#1
        movlo   lr,r7,lsl lr
        sublo   lr,lr,#1                ; every line
        cmp     r12,lr
        bne     rs_partial
        ; The whole sprite: one cel.
        ldr     lr,[sp,#16]
        cmp     r2,lr
        beq     rs_full
        ldr     lr,[sp,#4]
        add     lr,r4,lr,lsl #6
        str     lr,[r2,#CCB_SOURCE]
        ldr     lr,[sp,#8]
        str     lr,[r2,#CCB_XPOS]
        ldr     lr,[sp,#12]
        mov     lr,lr,lsl #16
        str     lr,[r2,#CCB_YPOS]
        ldr     lr,[r11,#S_H]
        mov     lr,lr,lsr r10
        sub     lr,lr,#1
        mov     lr,lr,lsl #6
        orr     lr,lr,#5
        str     lr,[r2,#CCB_PRE0]
        add     r2,r2,#CCB_SIZE
        b       rs_next
rs_partial
        mov     r7,#0                   ; first line of the run
rs_run
        tst     r12,#1
        bne     rs_start
        mov     r12,r12,lsr #1
        add     r7,r7,#1
        b       rs_run
rs_start
        mov     r8,r7                   ; its end
rs_len
        tst     r12,#1
        beq     rs_end
        mov     r12,r12,lsr #1
        add     r8,r8,#1
        b       rs_len
rs_end
        cmp     r10,#0
        beq     rs_emit
        bic     r7,r7,#1
        tst     r8,#1
        addne   r8,r8,#1
        movne   r12,r12,lsr #1
rs_emit
        ldr     lr,[sp,#16]
        cmp     r2,lr
        beq     rs_full
        ldr     lr,[sp,#4]
        mov     lr,lr,lsl #6
        add     lr,r4,lr                ; tile store
        mov     r1,r7,lsr r10
        add     lr,lr,r1,lsl #3         ; row of the run
        str     lr,[r2,#CCB_SOURCE]
        ldr     lr,[sp,#8]
        str     lr,[r2,#CCB_XPOS]
        ldr     lr,[sp,#12]
        add     lr,lr,r7
        mov     lr,lr,lsl #16
        str     lr,[r2,#CCB_YPOS]
        sub     r1,r8,r7
        mov     r1,r1,lsr r10
        sub     r1,r1,#1
        mov     r1,r1,lsl #6
        orr     r1,r1,#5                ; PRE0: rows - 1, 8 bpp
        str     r1,[r2,#CCB_PRE0]
        add     r2,r2,#CCB_SIZE
        mov     r7,r8
        movs    r12,r12
        bne     rs_run
rs_next
        ldr     r1,[sp,#20]
        add     r1,r1,#1
        str     r1,[sp,#20]
        b       rs_loop
rs_full
        ldr     lr,[r3,#SA_DROPPED]
        add     lr,lr,#1
        str     lr,[r3,#SA_DROPPED]
rs_done
        ldr     r1,[sp,#0]
        sub     r1,r9,r1
        mov     r1,r1,lsr #2
        str     r1,[r3,#SA_NEEDED]
        mov     r0,r2
        add     sp,sp,#24
        ldmfd   sp!,{r4-r11,pc}



;----------------------------------------------------------------------------
; uint32 render_prio_patches(render_patch_args *a)
;
; The priority layer under the sprites: the strips of the frame (cels
; already set up, a->strips, a->nstrips) are screen rectangles each
; showing a contiguous part of a priority bitmap; for each sprite on the
; screen, the part of each strip under it becomes a cel of its own,
; appended to a->p. Gives up with 0 when the pixels of the patches
; exceed a->limit or the cels run out; else returns 1 with the pixels in
; a->area.
;----------------------------------------------------------------------------
PA_STRIPS       EQU     0
PA_NSTRIPS      EQU     4
PA_XOFF         EQU     8
PA_V            EQU     12
PA_SAT          EQU     16
PA_LIMIT        EQU     20
PA_AREA         EQU     24
PA_P            EQU     28

render_prio_patches
        stmfd   sp!,{r4-r11,lr}
        mov     r11,r0                  ; args
        mov     r10,#0                  ; area
        ldr     r9,[r11,#PA_V]
        add     r9,r9,#0x4800
        add     r9,r9,#V_SPR-0x4800     ; &spr_n
        mov     r8,#0                   ; sprite
pp_sprite
        ldr     r0,[r9,#S_N]
        cmp     r8,r0
        bhs     pp_ok
        add     r0,r9,r8,lsl #2
        ldr     r1,[r0,#S_VIS]
        cmp     r1,#0
        beq     pp_nexts                ; hidden on every line
        ldr     r0,[r0,#S_Y]
        ldr     r1,[r9,#S_H]
        add     r6,r0,r1
        cmp     r6,#192
        movgt   r6,#192                 ; r6 = sy1
        movs    r7,r0
        movmi   r7,#0                   ; r7 = sy0
        cmp     r7,r6
        bge     pp_nexts
        ldr     r0,[r11,#PA_SAT]
        add     r0,r0,r8,lsl #1
        ldrb    r0,[r0,#0x80]
        ldr     r1,[r11,#PA_XOFF]
        sub     r0,r0,r1                ; x
        ldr     r1,[r9,#S_ZOOM]
        mov     r2,#8
        mov     r2,r2,lsl r1
        add     r4,r0,r2
        cmp     r4,#256
        movgt   r4,#256                 ; r4 = sx1
        movs    r5,r0
        movmi   r5,#0                   ; r5 = sx0
        cmp     r5,r4
        bge     pp_nexts
        ldr     r3,[r11,#PA_STRIPS]
        ldr     r2,[r11,#PA_NSTRIPS]
        mov     r12,#CCB_SIZE
        mla     r2,r12,r2,r3            ; end of the strips
pp_strip
        cmp     r3,r2
        bhs     pp_nexts
        ldr     r0,[r3,#CCB_YPOS]
        mov     r0,r0,asr #16           ; first line of the strip
        ldr     r1,[r3,#CCB_PRE0]
        mov     r1,r1,lsr #6
        add     r1,r1,#1
        add     r1,r0,r1                ; its end
        cmp     r0,r7
        movlt   r0,r7                   ; iy0
        cmp     r1,r6
        movgt   r1,r6                   ; iy1
        cmp     r0,r1
        bge     pp_nextst
        ldr     r12,[r3,#CCB_XPOS]
        mov     r12,r12,asr #16         ; first column of the strip
        ldr     lr,[r3,#CCB_PRE1]
        mov     lr,lr,lsl #21
        mov     lr,lr,lsr #21
        add     lr,lr,#1
        add     lr,r12,lr               ; its end
        cmp     r12,r5
        movlt   r12,r5                  ; ix0
        cmp     lr,r4
        movgt   lr,r4                   ; ix1
        cmp     r12,lr
        bge     pp_nextst
        ; A patch: lines r0 to r1 - 1, columns r12 to lr - 1 of the strip.
        sub     r1,r1,r0                ; rows
        sub     lr,lr,r12               ; columns
        mla     r10,r1,lr,r10
        stmfd   sp!,{r2,r3,r4,r5}
        ldr     r2,[r11,#PA_LIMIT]
        cmp     r10,r2
        bhi     pp_fail
        ldr     r2,[r11,#PA_P]
        ldmia   r2,{r4,r5}              ; next cel, capacity
        cmp     r4,r5
        beq     pp_fail
        add     r5,r4,#CCB_SIZE
        str     r5,[r2]                 ; the cel after the patch
        ldr     r5,[r3,#CCB_YPOS]
        sub     r5,r0,r5,asr #16        ; rows into the strip
        mov     r0,r0,lsl #16
        str     r0,[r4,#CCB_YPOS]
        ldr     r0,[r3,#CCB_XPOS]
        sub     r0,r12,r0,asr #16       ; columns into the strip
        and     r2,r0,#3                ; pixels before the word boundary
        sub     r0,r0,r2
        add     r0,r0,r5,lsl #8
        ldr     r5,[r3,#CCB_SOURCE]
        add     r0,r0,r5
        str     r0,[r4,#CCB_SOURCE]
        sub     r12,r12,r2
        mov     r12,r12,lsl #16
        str     r12,[r4,#CCB_XPOS]
        add     lr,lr,r2
        sub     lr,lr,#1
        orr     lr,lr,#0x3E0000         ; PRE1: 64 - 2 words per row, width - 1
        orr     lr,lr,#0x1000
        str     lr,[r4,#CCB_PRE1]
        sub     r1,r1,#1
        mov     r1,r1,lsl #6
        orr     r1,r1,#5                ; PRE0: rows - 1, 8 bpp
        str     r1,[r4,#CCB_PRE0]
        ldmfd   sp!,{r2,r3,r4,r5}
pp_nextst
        add     r3,r3,#CCB_SIZE
        b       pp_strip
pp_nexts
        add     r8,r8,#1
        b       pp_sprite
pp_fail
        add     sp,sp,#16
        mov     r0,#0
        ldmfd   sp!,{r4-r11,pc}
pp_ok
        str     r10,[r11,#PA_AREA]
        mov     r0,#1
        ldmfd   sp!,{r4-r11,pc}

        END
