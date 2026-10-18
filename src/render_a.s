; render_a.s - hot loops of the cel renderer (render.c) in assembly.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  render_nt_scan
        EXPORT  render_dirty_scan
        EXPORT  render_sprites
        EXPORT  render_entries
        EXPORT  render_tile_cells
        EXPORT  render_rebuild_cells
        EXPORT  render_stale_cells
        EXPORT  render_tile_cells_part
        EXPORT  render_rebuild_cells_part
        EXPORT  render_tile_conv
        EXPORT  render_rect_pieces
        EXPORT  render_band_pieces
        EXPORT  render_scroll_lbands
        EXPORT  render_sprite_runs
        EXPORT  render_prio_patches
        EXPORT  render_prio_groups
        EXPORT  cell_draw
        EXPORT  cell_conv
        EXPORT  conv_rows
        EXPORT  cell_change
        EXPORT  cd_prio

; CCB fields (graphics.h).
CCB_SOURCE      EQU     8
CCB_PLUT        EQU     12
CCB_XPOS        EQU     16
CCB_YPOS        EQU     20
CCB_PRE0        EQU     52
CCB_PRE1        EQU     56
CCB_SIZE        EQU     68

; layer fields (render_layer, render.h; offsets checked in render.c).
R_TILES         EQU     0
R_TILESF        EQU     4
R_TOK           EQU     8
R_BITMAP        EQU     12
R_PRIOBM        EQU     16
R_XTAB          EQU     20
R_SHADOW        EQU     24
R_STAMP         EQU     28
R_STAMPNOW      EQU     32
R_NEXT          EQU     36
R_HEAD          EQU     40
R_PREV          EQU     44
R_WLIST         EQU     48
R_NTBASE        EQU     56
R_PRIOROWS      EQU     68
R_PRIOMIN       EQU     100
R_PRIOMAX       EQU     132
R_PRIODIRTY     EQU     164
R_PGN           EQU     208
R_PGROW0        EQU     212
R_PGNROWS       EQU     244
R_PGCOL0        EQU     276
R_PGCOL1        EQU     308
R_STCELLS       EQU     196
R_STTILECELLS   EQU     200
R_STTILESCONV   EQU     204
R_STALE         EQU     340
R_STALEROWS     EQU     344
R_SHOWNROWS     EQU     348
R_NROWS         EQU     352
R_NCELLS        EQU     356

; piece_ctx fields (render.h).
PC_C            EQU     0
PC_END          EQU     4
PC_BMH          EQU     8
PC_PLUT         EQU     12

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
SA_HEIGHT       EQU     44

;----------------------------------------------------------------------------
; uint32 render_nt_scan(const uint32 *nt, const uint32 *shadow,
;                       const uint32 *mask, uint32 *out, uint32 chunks)
;
; Compares the chunks of 8 words of the name table (56, or 64 for a table
; of 32 rows) with the shadow copy, for the chunks whose bit is set in
; mask (two words, chunk 0 = bit 0 of the first); appends the index of
; each differing word to out and returns their count. Nothing is written
; to the shadow.
;----------------------------------------------------------------------------
render_nt_scan
        stmfd   sp!,{r4-r11,lr}
        ldr     r10,[sp,#36]            ; chunks
        ldr     r12,[r2]                ; chunks 0-31
        ldr     r4,[r2,#4]              ; chunks 32-63
        sub     r5,r10,#32              ; chunks of the second mask word
        stmfd   sp!,{r3,r4,r5}
        mov     r2,r3                   ; output pointer
        mov     r3,#0                   ; word index
        mov     r10,#32                 ; chunks left with the first word
scan_next
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
        b       scan_second
scan_clean
        add     r0,r0,#32
        add     r1,r1,#32
        add     r3,r3,#8
        subs    r10,r10,#1
        bne     scan_next
scan_second
        ldr     r10,[sp,#8]             ; then the chunks of the second word
        cmp     r10,#0
        beq     scan_done
        ldr     r12,[sp,#4]
        mov     r4,#0
        str     r4,[sp,#8]
        b       scan_next
scan_done
        ldmfd   sp!,{r0,r1,r12}
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
; CCB *render_sprites(const uint8 *sat, uint32 n, CCB *cel,
;                     render_sprite_args *a)
;
; Sets the source, X and Y of one cel per sprite among sprites 0 to n - 1
; that reaches the screen (a->height lines; a sprite entirely above or
; below it gets no cel), in table order from cel; appends to a->need the
; tiles (first tile of the sprite) whose conversion is missing (a->t_ok
; bit 0 clear for the tile, or for the second one of a tall sprite),
; a->needed of them, and returns the cel after the last one set up.
;----------------------------------------------------------------------------
render_sprites
        stmfd   sp!,{r4-r11,lr}
        ldmia   r3,{r4-r10}             ; tiles, t_ok, tbase, tmask, xoff, need, tall
        ldr     r11,[r3,#SA_YWRAP]
        ldr     lr,[r3,#SA_HEIGHT]
        add     r10,r5,r10              ; t_ok + 1 for tall sprites, else t_ok
        orr     r11,r11,lr,lsl #16
        stmfd   sp!,{r3,r9}             ; args, start of the need list
        mov     r3,r11                  ; r3 = Y wrap | height << 16
        add     r11,r0,#0x80            ; x / tile pairs
        movs    r1,r1
        beq     spr_done
spr_loop
        ldrb    r12,[r0],#1             ; y
        and     lr,r3,#0xFF
        add     r12,r12,#1
        cmp     r12,lr
        subgt   r12,r12,#256            ; a sprite past the wrap shows at the top
        cmp     r12,r3,lsr #16
        bge     spr_skip                ; below the screen
        sub     lr,lr,#256              ; - lines per sprite
        cmp     r12,lr
        ble     spr_skip                ; above it
        mov     r12,r12,lsl #16
        str     r12,[r2,#CCB_YPOS]
        ldrb    r12,[r11],#1            ; x
        ldrb    lr,[r11],#1             ; tile
        sub     r12,r12,r8
        mov     r12,r12,lsl #16
        str     r12,[r2,#CCB_XPOS]
        and     lr,lr,r7
        add     lr,lr,r6
        ldrb    r12,[r5,lr]             ; conversion of the tile(s) present?
        tst     r12,#1
        ldrneb  r12,[r10,lr]
        tstne   r12,#1
        streq   lr,[r9],#4
        add     lr,r4,lr,lsl #6
        str     lr,[r2,#CCB_SOURCE]
        add     r2,r2,#CCB_SIZE
        subs    r1,r1,#1
        bne     spr_loop
        b       spr_done
spr_skip
        add     r11,r11,#2
        subs    r1,r1,#1
        bne     spr_loop
spr_done
        ldmfd   sp!,{r1,r12}
        sub     r12,r9,r12
        mov     r12,r12,lsr #2
        str     r12,[r1,#SA_NEEDED]
        mov     r0,r2
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; Cells.
;
; Registers held across a walk over cells (render_entries, render_tile_cells,
; render_stale_cells, render_rebuild_cells and their resumable forms):
; r9 = layer, r10 = VRAM, r11 = cursor, r4-r8 = fields of the layer each
; walk hoists (shadow, stamp...), which the drawing routine preserves. The
; walks stamp and count the cells they draw.
;
; cell_draw: draws entry r1 into cell r0 of the bitmap (and of the priority
; bitmap when the entry has the priority bit): the rows of the tile store
; the entry needs (plain, or mirrored for a horizontal flip) are copied
; with the palette bit OR-ed into every pixel, in reverse order for a
; vertical flip. A store found missing is filled by cell_conv, which
; converts the tile into the store and into the cell at once. Preserves
; r4-r9 and r11, clobbers r0-r3, r12 and lr; the usual cell (no priority
; bit, the store present) touches no memory but the rows.
;----------------------------------------------------------------------------
NONE            EQU     1024            ; end of a cell list
CELL_ROW        EQU     256             ; bytes per bitmap row

        MACRO
        PALBITS $d,$e                   ; $d = 0x10 in every pixel when entry $e has the palette bit
        tst     $e,#0x800
        mov     $d,#0
        movne   $d,#0x10
        orrne   $d,$d,$d,lsl #8
        orrne   $d,$d,$d,lsl #16
        MEND

        MACRO
        CELLOFF $d,$c,$t                ; $d = byte offset of cell $c in a bitmap ($t clobbered)
        mov     $d,$c,lsr #5
        mov     $d,$d,lsl #11
        and     $t,$c,#31
        add     $d,$d,$t,lsl #3
        MEND

; One row of a copy: r2 = source (the next row up or down), r3 =
; destination (the next row follows), r12 = palette bits; ROWCOPYP steps
; its source by lr and writes the priority bitmap (r8) as well.
        MACRO
        ROWCOPY $step
        ldmia   r2,{r0,r1}
        orr     r0,r0,r12
        orr     r1,r1,r12
        $step   r2,r2,#8
        stmia   r3,{r0,r1}
        add     r3,r3,#CELL_ROW
        MEND

        MACRO
        ROWCOPYP
        ldmia   r2,{r0,r1}
        orr     r0,r0,r12
        orr     r1,r1,r12
        add     r2,r2,lr
        stmia   r3,{r0,r1}
        add     r3,r3,#CELL_ROW
        stmia   r8,{r0,r1}
        add     r8,r8,#CELL_ROW
        MEND

cell_draw
        bic     r12,r1,#0xFE00          ; tile
        ldr     r3,[r9,#R_TOK]
        ldrb    r3,[r3,r12]             ; its stores present
        tst     r1,#0x200
        ldreq   r2,[r9,#R_TILES]        ; the store the entry needs: mirrored for a horizontal flip
        ldrne   r2,[r9,#R_TILESF]
        add     r2,r2,r12,lsl #6        ; its rows
        moveq   r12,#1                  ; the store's bit in t_ok
        movne   r12,#2
        tst     r3,r12
        beq     cell_conv               ; missing: converted into the store and the cell
        CELLOFF r3,r0,r12
        tst     r1,#0x1000
        bne     cd_prio
        ldr     r12,[r9,#R_BITMAP]
        add     r3,r3,r12
        PALBITS r12,r1
        tst     r1,#0x400
        bne     cd_down
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        ROWCOPY add
        mov     pc,lr
cd_down                                 ; vertical flip: last source row first
        add     r2,r2,#56
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        ROWCOPY sub
        mov     pc,lr
cd_prio
        stmfd   sp!,{r8,lr}
        ldr     r8,[r9,#R_PRIOBM]
        ldr     r12,[r9,#R_BITMAP]
        add     r8,r8,r3
        add     r3,r3,r12
        PALBITS r12,r1
        mov     lr,#8
        tst     r1,#0x400
        addne   r2,r2,#56               ; vertical flip: last source row first
        mvnne   lr,#7
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ROWCOPYP
        ldmfd   sp!,{r8,pc}

;----------------------------------------------------------------------------
; Tile conversion: from the VDP's four bit planes (4 bytes per row, plane
; 0 first, bit 7 the first pixel) to 8 bpp rows of two words, pixel 0 in
; the top byte of the first word. Each row is looked up in the expansion
; table by pairs of planes: the entry for a pair of nibbles (plane a in
; bits 4-7, plane b in bits 0-3) is the word of their four pixels, plane a
; at bit 0 and plane b at bit 1 of each; the high nibbles of planes 0 and
; 1 give pixels 0-3, their low nibbles pixels 4-7, and planes 2 and 3 are
; OR-ed in two bits up. The mirrored table (for the store of horizontally
; flipped tiles) holds the pixels of a group in reverse order, so that the
; low nibbles make the first word of a row.
;
; cell_conv: entered from cell_draw with r0 = cell, r1 = entry, r2 = the
; rows of the store to fill, r12 = its bit in t_ok: converts the tile into
; the store and draws the cell from the rows on the way. render_tile_conv
; (C entry) converts into the store only.
;
; conv_rows: r4 = planar rows, r9 = table, r2 = store rows; r10 says what
; else the rows go to (1: the cell at r3 and the priority bitmap at r8,
; 2: the cell only, 0: nothing), r12 = the step between cell rows, lr =
; palette bits; r11 = rows left. The conditional stores read the flags
; of the compare of r10 made in each row.
;----------------------------------------------------------------------------
        MACRO
        CONVROW $h,$l                   ; $h = word of pixels 0-3, $l = pixels 4-7
        ldr     r5,[r4],#4              ; p0 p1 p2 p3, p0 in the top byte
        and     r6,r5,#0xF0000000
        and     r7,r5,#0x00F00000
        orr     r6,r7,r6,lsr #4         ; high nibbles of p0 and p1, at bits 20-27
        ldr     $h,[r9,r6,lsr #18]
        and     r6,r5,#0x0F000000
        and     r7,r5,#0x000F0000
        orr     r6,r7,r6,lsr #4         ; low nibbles of p0 and p1, at bits 16-23
        ldr     $l,[r9,r6,lsr #14]
        and     r6,r5,#0xF000
        and     r7,r5,#0xF0
        orr     r6,r7,r6,lsr #4         ; high nibbles of p2 and p3, at bits 4-11
        ldr     r7,[r9,r6,lsr #2]
        orr     $h,$h,r7,lsl #2
        and     r6,r5,#0x0F00
        and     r7,r5,#0x0F
        orr     r6,r7,r6,lsr #4         ; low nibbles of p2 and p3, at bits 0-7
        ldr     r7,[r9,r6,lsl #2]
        orr     $l,$l,r7,lsl #2
        stmia   r2!,{r0,r1}
        cmp     r10,#1
        orr     r0,r0,lr
        orr     r1,r1,lr
        stmcsia r3,{r0,r1}
        addcs   r3,r3,r12
        stmeqia r8,{r0,r1}
        addeq   r8,r8,r12
        subs    r11,r11,#1
        MEND

cell_conv
        str     lr,[sp,#-4]!
        stmfd   sp!,{r4-r11}
        bic     lr,r1,#0xFE00           ; tile
        ldr     r4,[r9,#R_TOK]          ; t_ok[tile] |= bit
        ldrb    r5,[r4,lr]
        orr     r5,r5,r12
        strb    r5,[r4,lr]
        ldr     r4,[r9,#R_STTILESCONV]
        add     r4,r4,#1
        str     r4,[r9,#R_STTILESCONV]
        add     r4,r10,lr,lsl #5        ; planar rows
        ldr     r5,[r9,#R_XTAB]
        cmp     r12,#2
        addeq   r5,r5,#1024             ; the mirrored table
        CELLOFF r6,r0,r7
        ldr     r3,[r9,#R_BITMAP]
        ldr     r8,[r9,#R_PRIOBM]
        add     r3,r3,r6
        add     r8,r8,r6
        mov     r9,r5
        PALBITS lr,r1
        mov     r12,#CELL_ROW
        tst     r1,#0x400
        addne   r3,r3,#7*CELL_ROW       ; vertical flip: the cell's last row first
        addne   r8,r8,#7*CELL_ROW
        rsbne   r12,r12,#0
        tst     r1,#0x1000
        moveq   r10,#2                  ; the cell only
        movne   r10,#1                  ; the cell and the priority bitmap
        mov     r11,#8
        tst     r1,#0x200
        bne     conv_rows_m
conv_rows
        CONVROW r0,r1
        bne     conv_rows
        b       conv_done
conv_rows_m
        CONVROW r1,r0
        bne     conv_rows_m
conv_done
        ldmfd   sp!,{r4-r11}
        ldr     pc,[sp],#4

;----------------------------------------------------------------------------
; void render_tile_conv(render_layer *l, uint32 t, uint32 flip, const uint8 *vram)
;
; Converts tile t into the layer's tile store (the mirrored store with
; flip), marks the store of the tile converted and counts the conversion.
;----------------------------------------------------------------------------
render_tile_conv
        str     lr,[sp,#-4]!
        stmfd   sp!,{r4-r11}
        mov     r9,r0
        mov     lr,r1                   ; tile
        mov     r6,r2                   ; flip
        mov     r11,#1
        mov     r11,r11,lsl r2          ; store bit
        ldr     r4,[r9,#R_TOK]
        ldrb    r5,[r4,lr]
        orr     r5,r5,r11
        strb    r5,[r4,lr]
        ldr     r4,[r9,#R_STTILESCONV]
        add     r4,r4,#1
        str     r4,[r9,#R_STTILESCONV]
        ldr     r5,[r9,#R_XTAB]
        teq     r6,#0
        ldreq   r2,[r9,#R_TILES]
        ldrne   r2,[r9,#R_TILESF]
        addne   r5,r5,#1024
        add     r2,r2,lr,lsl #6         ; store rows
        add     r4,r3,lr,lsl #5         ; planar rows
        mov     r9,r5
        mov     r10,#0                  ; the store only
        mov     r11,#8
        teq     r6,#0
        beq     conv_rows
        b       conv_rows_m

;----------------------------------------------------------------------------
; cell_change: cell r0 goes from entry r2 to entry r1: re-linked in the
; cell list of its tile when the tile changed, the priority count of its
; row and the row's dirty flag updated when the priority bit changed (the
; cell cleared in the priority bitmap when the bit went away), no longer
; stale, then drawn. Preserves r4-r9 and r11 like cell_draw.
;----------------------------------------------------------------------------
cell_change
        stmfd   sp!,{r0,r1,r2,lr}       ; cell, new entry, old entry
        eor     r3,r1,r2
        movs    r12,r3,lsl #23          ; tile bits 0-8 differ?
        beq     cc_prio
        stmfd   sp!,{r4,r5}
        bic     r2,r2,#0xFE00           ; old tile
        bic     r1,r1,#0xFE00           ; new tile
        add     r4,r9,#R_NEXT
        ldmia   r4,{r4,r12,lr}          ; next, head, prev (in register order)
        ldr     r5,[lr,r0,lsl #2]       ; p = prev[cell]
        ldr     r3,[r4,r0,lsl #2]       ; n = next[cell]
        cmp     r5,#NONE
        streq   r3,[r12,r2,lsl #2]      ; head[old] = n
        strne   r3,[r4,r5,lsl #2]       ; next[p] = n
        cmp     r3,#NONE
        strne   r5,[lr,r3,lsl #2]       ; prev[n] = p
        ldr     r3,[r12,r1,lsl #2]      ; h = head[new]
        mov     r5,#NONE
        str     r5,[lr,r0,lsl #2]       ; prev[cell] = NONE
        str     r3,[r4,r0,lsl #2]       ; next[cell] = h
        cmp     r3,#NONE
        strne   r0,[lr,r3,lsl #2]       ; prev[h] = cell
        str     r0,[r12,r1,lsl #2]      ; head[new] = cell
        ldmfd   sp!,{r4,r5}
        ldmib   sp,{r1,r2}              ; the entries again
        eor     r3,r1,r2
cc_prio
        tst     r3,#0x1000
        beq     cc_draw
        mov     r2,r0,lsr #5            ; row
        add     r3,r9,#R_PRIOROWS
        ldrb    r12,[r3,r2]
        tst     r1,#0x1000
        addne   r12,r12,#1
        subeq   r12,r12,#1
        strb    r12,[r3,r2]
        add     r3,r9,#R_PRIODIRTY
        mov     r12,#1
        strb    r12,[r3,r2]
        tst     r1,#0x1000
        bne     cc_draw
        CELLOFF r2,r0,r3                ; the bit went away: clear the cell
        ldr     r3,[r9,#R_PRIOBM]
        add     r3,r3,r2
        mov     r1,#0
        mov     r2,#0
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
        add     r3,r3,#CELL_ROW
        stmia   r3,{r1,r2}
cc_draw
        ldr     r3,[r9,#R_STALE]        ; drawn below: no longer stale
        mov     r12,#0
        strb    r12,[r3,r0]
        ldmfd   sp!,{r0,r1,r2,lr}
        b       cell_draw

;----------------------------------------------------------------------------
; void render_entries(render_layer *l, const uint8 *vram, uint32 n)
;
; For each of the n name table word indices in l->wlist: the word is
; copied to the shadow and each of its two entries that differs from the
; shadow is stamped and goes through cell_change.
;----------------------------------------------------------------------------
render_entries
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r11,[r9,#R_WLIST]
        add     r0,r11,r2,lsl #2
        str     r0,[sp,#-4]!            ; end of the list
        ldr     r5,[r9,#R_NTBASE]
        add     r5,r10,r5               ; name table
        ldr     r6,[r9,#R_SHADOW]
        ldr     r7,[r9,#R_STAMP]
        ldr     r8,[r9,#R_STAMPNOW]
        mov     r4,#0                   ; cells drawn
re_loop
        ldr     r0,[sp]
        cmp     r11,r0
        beq     re_done
        ldr     r0,[r11],#4             ; word index
        ldr     r1,[r5,r0,lsl #2]       ; new word
        ldr     r2,[r6,r0,lsl #2]       ; old word
        str     r1,[r6,r0,lsl #2]
        mov     r0,r0,lsl #1            ; first cell of the word
        mov     r3,r1,lsr #24           ; first entry: low byte first
        and     r12,r1,#0x00FF0000
        orr     r3,r3,r12,lsr #8
        mov     r12,r2,lsr #24
        and     lr,r2,#0x00FF0000
        orr     r12,r12,lr,lsr #8
        cmp     r3,r12
        beq     re_second
        stmfd   sp!,{r0,r1,r2}
        strb    r8,[r7,r0]              ; stamped
        add     r4,r4,#1
        mov     r1,r3
        mov     r2,r12
        bl      cell_change
        ldmfd   sp!,{r0,r1,r2}
re_second
        and     r3,r1,#0xFF00           ; second entry
        and     r12,r1,#0xFF
        mov     r3,r3,lsr #8
        orr     r3,r3,r12,lsl #8
        and     r12,r2,#0xFF00
        and     lr,r2,#0xFF
        mov     r12,r12,lsr #8
        orr     r12,r12,lr,lsl #8
        cmp     r3,r12
        beq     re_loop
        add     r0,r0,#1
        strb    r8,[r7,r0]
        add     r4,r4,#1
        mov     r1,r3
        mov     r2,r12
        bl      cell_change
        b       re_loop
re_done
        add     sp,sp,#4
        ldr     r0,[r9,#R_STCELLS]
        add     r0,r0,r4
        str     r0,[r9,#R_STCELLS]
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_tile_cells(render_layer *l, const uint8 *vram, uint32 t)
;
; Draws the cells of the list of tile t that are not stamped in this
; update, with their entry from the shadow; counts them in st.cells and
; st.tile_cells. A cell of a row the update does not show (shown_rows) is
; left stale instead, for render_stale_cells.
;----------------------------------------------------------------------------
        MACRO
        LEAVESTALE                      ; cell r11 of row r1: stale until its row is shown
        ldr     r0,[r9,#R_STALE]
        mov     r2,#1
        strb    r2,[r0,r11]
        ldr     r0,[r9,#R_STALEROWS]
        orr     r0,r0,r2,lsl r1
        str     r0,[r9,#R_STALEROWS]
        MEND

        MACRO
        CELLENTRY                       ; r1 = entry of cell r11 from the shadow r4
        add     r1,r4,r11,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8
        MEND

render_tile_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r4,[r9,#R_HEAD]
        ldr     r11,[r4,r2,lsl #2]      ; first cell of the list
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6,r7}        ; shadow, stamp, stamp_now, next
        ldr     r0,[r9,#R_SHOWNROWS]
        str     r0,[sp,#-4]!
        mov     r8,#0                   ; cells drawn
tc_loop
        cmp     r11,#NONE
        beq     tc_done
        ldrb    r0,[r5,r11]
        cmp     r0,r6
        beq     tc_next                 ; drawn in this update already
        ldr     r0,[sp]
        mov     r1,r11,lsr #5           ; row shown?
        mov     r0,r0,lsr r1
        tst     r0,#1
        bne     tc_draw
        LEAVESTALE
        b       tc_next
tc_draw
        CELLENTRY
        mov     r0,r11
        strb    r6,[r5,r11]             ; stamped
        add     r8,r8,#1
        bl      cell_draw
tc_next
        ldr     r11,[r7,r11,lsl #2]
        b       tc_loop
tc_done
        add     sp,sp,#4
        ldr     r0,[r9,#R_STCELLS]
        ldr     r1,[r9,#R_STTILECELLS]
        add     r0,r0,r8
        add     r1,r1,r8
        str     r0,[r9,#R_STCELLS]
        str     r1,[r9,#R_STTILECELLS]
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; uint32 render_tile_cells_part(render_layer *l, const uint8 *vram, uint32 t,
;                               uint32 from, uint32 budget)
;
; render_tile_cells for the list of tile t from cell from on (NONE: from
; the head of the list), stopping once budget cells have been drawn.
; Returns the cell to continue from, NONE when the list is done. Used by
; the catch-up of the picture in spare time, which looks at the clock
; between two calls; the cells drawn are no longer stale.
;----------------------------------------------------------------------------
render_tile_cells_part
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r0,[sp,#36]             ; budget
        cmp     r3,#NONE
        ldreq   r4,[r9,#R_HEAD]
        ldreq   r11,[r4,r2,lsl #2]      ; from the head of the list
        movne   r11,r3
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6,r7}        ; shadow, stamp, stamp_now, next
        ldr     r1,[r9,#R_SHOWNROWS]
        stmfd   sp!,{r0,r1}             ; budget, rows shown
        mov     r8,#0                   ; cells drawn
tp_loop
        cmp     r11,#NONE
        beq     tp_done
        ldrb    r0,[r5,r11]
        cmp     r0,r6
        beq     tp_next                 ; drawn in this update already
        ldr     r0,[sp,#4]
        mov     r1,r11,lsr #5           ; row shown?
        mov     r0,r0,lsr r1
        tst     r0,#1
        bne     tp_draw
        LEAVESTALE
        b       tp_next
tp_draw
        ldr     r0,[sp]
        cmp     r0,#0                   ; budget spent: continue from this cell
        beq     tp_done
        sub     r0,r0,#1
        str     r0,[sp]
        ldr     r0,[r9,#R_STALE]
        mov     r1,#0
        strb    r1,[r0,r11]
        CELLENTRY
        mov     r0,r11
        strb    r6,[r5,r11]             ; stamped
        add     r8,r8,#1
        bl      cell_draw
tp_next
        ldr     r11,[r7,r11,lsl #2]
        b       tp_loop
tp_done
        add     sp,sp,#8
        ldr     r0,[r9,#R_STCELLS]
        ldr     r1,[r9,#R_STTILECELLS]
        add     r0,r0,r8
        add     r1,r1,r8
        str     r0,[r9,#R_STCELLS]
        str     r1,[r9,#R_STTILECELLS]
        mov     r0,r11
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_stale_cells(render_layer *l, const uint8 *vram)
;
; Draws the stale cells of the rows the update shows (those not stamped
; yet), from the shadow; those rows hold no stale cell afterwards.
;----------------------------------------------------------------------------
render_stale_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r0,[r9,#R_STALEROWS]
        ldr     r1,[r9,#R_SHOWNROWS]
        and     r11,r0,r1               ; rows to visit
        bic     r0,r0,r1
        str     r0,[r9,#R_STALEROWS]
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6}           ; shadow, stamp, stamp_now
        ldr     r7,[r9,#R_STALE]
        mov     r8,#0                   ; cell
sc_rows
        movs    r11,r11,lsr #1
        bcc     sc_skip_row
sc_cells
        ldrb    r0,[r7,r8]
        teq     r0,#0
        beq     sc_next_cell
        mov     r0,#0
        strb    r0,[r7,r8]
        ldrb    r0,[r5,r8]
        cmp     r0,r6
        beq     sc_next_cell            ; drawn in this update already
        add     r1,r4,r8,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8
        mov     r0,r8
        strb    r6,[r5,r8]              ; stamped
        bl      cell_draw
        ldr     r0,[r9,#R_STCELLS]
        ldr     r1,[r9,#R_STTILECELLS]
        add     r0,r0,#1
        add     r1,r1,#1
        str     r0,[r9,#R_STCELLS]
        str     r1,[r9,#R_STTILECELLS]
sc_next_cell
        add     r8,r8,#1
        tst     r8,#31
        bne     sc_cells
        b       sc_row_done
sc_skip_row
        add     r8,r8,#32
sc_row_done
        teq     r11,#0
        bne     sc_rows
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_rebuild_cells(render_layer *l, const uint8 *vram)
;
; Links every cell of the table (ncells) into the list of its tile (the
; heads must hold NONE), counts the priority cells of each row and draws
; every cell, from the entries of the shadow.
;
; uint32 render_rebuild_cells_part(render_layer *l, const uint8 *vram,
;                                  uint32 from, uint32 budget)
;
; The same for cells from to from + budget - 1 (bounded by ncells), so
; that a rebuild can be spread over the spare time of several frames;
; returns the next cell, ncells once the table is done.
;----------------------------------------------------------------------------
render_rebuild_cells
        mov     r2,#0
        ldr     r3,[r0,#R_NCELLS]
render_rebuild_cells_part
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        mov     r11,r2                  ; cell
        ldr     r0,[r9,#R_NCELLS]
        add     r3,r2,r3
        cmp     r3,r0
        movhi   r3,r0
        cmp     r11,r3
        bhs     rb_done
        stmfd   sp!,{r2,r3}             ; first cell, last cell + 1
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6}           ; shadow, stamp, stamp_now
rb_loop
        add     r1,r4,r11,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8         ; entry
        bic     r2,r1,#0xFE00           ; tile
        add     r7,r9,#R_NEXT
        ldmia   r7,{r7,r8,r12}          ; next, head, prev
        ldr     r3,[r8,r2,lsl #2]       ; h = head[tile]
        mov     r0,#NONE
        str     r0,[r12,r11,lsl #2]     ; prev[cell] = NONE
        str     r3,[r7,r11,lsl #2]      ; next[cell] = h
        cmp     r3,#NONE
        strne   r11,[r12,r3,lsl #2]     ; prev[h] = cell
        str     r11,[r8,r2,lsl #2]      ; head[tile] = cell
        tst     r1,#0x1000
        beq     rb_draw
        mov     r2,r11,lsr #5           ; a priority cell: counted in its row
        add     r3,r9,#R_PRIOROWS
        ldrb    r0,[r3,r2]
        add     r0,r0,#1
        strb    r0,[r3,r2]
rb_draw
        mov     r0,r11
        strb    r6,[r5,r11]             ; stamped
        bl      cell_draw
        add     r11,r11,#1
        ldr     r0,[sp,#4]
        cmp     r11,r0
        bne     rb_loop
        ldmfd   sp!,{r1,r2}
        sub     r2,r2,r1                ; cells drawn
        ldr     r0,[r9,#R_STCELLS]
        add     r0,r0,r2
        str     r0,[r9,#R_STCELLS]
rb_done
        mov     r0,r11
        ldmfd   sp!,{r4-r11,pc}


;----------------------------------------------------------------------------
; uint32 render_rect_pieces(piece_ctx *p, const uint8 *bm, uint32 bx0,
;                           uint32 bx1, uint32 by0, uint32 by1, uint32 lb,
;                           uint32 vs)
;
; The usual case of rect_pieces (render.c): a band the scroll inhibit bits
; do not split (register 0 bit 7 clear, bit 6 clear or the band below line
; 16), the whole width for window. The rectangle of bitmap columns bx0 to bx1 - 1 and
; rows by0 to by1 - 1 is shown on the lines of line band lb (ya | yb << 8
; | hs << 16) as pieces appended to p (next cel, capacity, bitmap lines
; for the vertical wrap, PLUT of the pieces), split at the row and column
; wraps, the part from column (bx0 + hs) & 255 before the part wrapped to
; the left edge. Returns the number of pieces dropped for lack of cels.
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
        ldr     lr,[r0,#PC_PLUT]
        str     lr,[r12,#CCB_PLUT]
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
        ldr     r1,[r0,#PC_BMH]         ; bitmap lines (r1 is free: bm is on the stack)
        add     r3,r4,r1,lsl #1
        sub     r3,r3,r7                ; s0: screen line of by0, modulo bm_h
rp_mod
        cmp     r3,r1
        subhs   r3,r3,r1
        bhs     rp_mod
        mov     r7,#0                   ; pieces dropped
        mov     r12,r3                  ; lines s0 to min(s0 + rows, bm_h) - 1
        add     lr,r3,r9
        cmp     lr,r1
        movhi   lr,r1
        mov     r2,r4
        PART
        ldr     r1,[r0,#PC_BMH]
        add     lr,r3,r9                ; the rest wraps to the top
        cmp     lr,r1
        bls     rp_done
        sub     lr,lr,r1
        mov     r12,#0
        sub     r2,r1,r3
        add     r2,r2,r4
        PART
rp_done
        mov     r0,r7
        ldmfd   sp!,{r1,r4-r11,pc}

;----------------------------------------------------------------------------
; uint32 render_scroll_lbands(const uint32 *log, uint32 n, uint32 top,
;                             uint32 hl, uint32 *out)
;
; The line bands of the usual frame (the display on, one name table, one
; palette band) straight from the n writes to the horizontal scroll
; recorded at log (line << 8 | value), as vdp_bands and line_bands make
; them one after the other: top is the value of the first band; a write on
; the first line of the last band replaces its value, and merges the band
; with the one before it when they become equal; a write of another value
; on a later line starts a band. hl is the picture height | the layer bit
; << 24. A band is written once it can no longer change (one band behind
; the log), as ya | yb << 8 | value << 16 | layer << 24, those starting at
; or below the picture height dropped and the others cut there. Returns
; the words written | the number of bands << 16.
;----------------------------------------------------------------------------
        MACRO
        SLEMIT  $t,$e,$v                ; the band $t to $e (cut at h), value $v
        cmp     $t,r10
        bhs     %F1
        cmp     $e,r10
        movls   r7,$e
        movhi   r7,r10
        orr     r7,$t,r7,lsl #8
        orr     r7,r7,$v,lsl #16
        orr     r7,r7,r3
        str     r7,[r8],#4
1
        MEND

render_scroll_lbands
        stmfd   sp!,{r4-r11,lr}
        ldr     r8,[sp,#36]             ; out
        mov     r11,r8
        and     r10,r3,#0xFF            ; h
        and     r3,r3,#0xFF000000       ; the layer bit
        add     r1,r0,r1,lsl #2         ; the end of the log
        and     r2,r2,#0xFF             ; v1: the value of the last band
        mov     r4,#0                   ; t1: its first line
        mvn     r5,#0                   ; t0: the band before it (-1: none to write)
        mov     r6,#0                   ; v0: its value
        mov     r9,#1                   ; bands
sl_loop
        cmp     r0,r1
        beq     sl_end
        ldr     r12,[r0],#4
        mov     lr,r12,lsr #8           ; line
        and     r12,r12,#0xFF           ; value
        cmp     lr,r4
        beq     sl_same
        bcc     sl_loop                 ; before the last band (the lines only grow)
        cmp     r12,r2
        beq     sl_loop                 ; the same value: no new band
        cmp     r5,#0
        blt     sl_shift
        SLEMIT  r5,r4,r6                ; the band before the last is final
sl_shift
        mov     r5,r4
        mov     r6,r2
        mov     r4,lr
        mov     r2,r12
        add     r9,r9,#1
        b       sl_loop
sl_same
        mov     r2,r12
        cmp     r5,#0
        blt     sl_loop
        cmp     r2,r6
        bne     sl_loop
        mov     r4,r5                   ; merged with the band before it; the one
        mvn     r5,#0                   ; before that is written already
        sub     r9,r9,#1
        b       sl_loop
sl_end
        cmp     r5,#0
        blt     sl_last
        SLEMIT  r5,r4,r6
sl_last
        SLEMIT  r4,r10,r2
        sub     r0,r8,r11
        mov     r0,r0,lsr #2
        orr     r0,r0,r9,lsl #16
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; uint32 render_band_pieces(render_band_args *a, const uint32 *lb)
;
; The background pieces of the line bands at lb (ya | yb << 8 | hs << 16 |
; layer << 24 | palette band << 25), read up to a word of all ones, appended
; at a->c: for each band what render_rect_pieces makes of the whole bitmap
; of its layer, with the PLUT of its palette band (looked up again only when
; the layer or the palette band changes from one band to the next). The
; lines at and below the screen line s0 that shows bitmap row 0 come before
; those above it, and each part is the piece from screen column hs, then
; the piece wrapped to the left edge (its first pixels, before the word
; boundary of the source, clipped). Each cel is chained to the cel after
; it. Returns the number of pieces dropped for lack of cels.
;----------------------------------------------------------------------------
BA_C            EQU     0
BA_END          EQU     4
BA_BMH          EQU     8
BA_BM0          EQU     16
BA_PLUTS        EQU     24

        MACRO
$l      BPIECES                         ; source row r2, y r5, PRE0 r6, hs lr
        cmp     r0,r8
        beq     $l.full
        mov     r4,lr,lsl #16           ; x: the piece from column hs
        rsb     r7,lr,#255              ; its width - 1
        orr     r7,r7,#0x3E0000
        orr     r7,r7,#0x1000           ; PRE1: 64 - 2 words per row, width - 1
        add     r1,r0,#CCB_SIZE
        stmib   r0,{r1-r5}              ; next cel, source, PLUT, x, y
        add     r1,r0,#CCB_PRE0-4
        stmib   r1,{r6,r7}              ; PRE0, PRE1
        add     r0,r0,#CCB_SIZE
        cmp     lr,#0
        beq     $l.end                  ; nothing wrapped
        cmp     r0,r8
        beq     $l.fullb
        rsb     r1,lr,#256              ; bitmap column at screen column 0
        and     r7,r1,#3                ; pixels before its word boundary
        sub     r1,r1,r7
        add     r2,r2,r1
        rsb     r4,r7,#0
        mov     r4,r4,lsl #16           ; x
        add     r7,r7,lr
        sub     r7,r7,#1                ; width - 1: columns 0 to hs - 1 and those pixels
        orr     r7,r7,#0x3E0000
        orr     r7,r7,#0x1000
        add     r1,r0,#CCB_SIZE
        stmib   r0,{r1-r5}
        add     r1,r0,#CCB_PRE0-4
        stmib   r1,{r6,r7}
        add     r0,r0,#CCB_SIZE
$l.end
        MEND

        MACRO
$l      BPFULL                          ; no cel left for the pieces of $l
$l.full
        ldr     r1,[sp]
        add     r1,r1,#1
        cmp     lr,#0
        addne   r1,r1,#1                ; the wrapped piece too
        str     r1,[sp]
        b       $l.end
$l.fullb
        ldr     r1,[sp]
        add     r1,r1,#1
        str     r1,[sp]
        b       $l.end
        MEND

render_band_pieces
        stmfd   sp!,{r0,r4-r11,lr}
        mov     r9,r1                   ; line bands
        ldmia   r0,{r0,r8,r10,r11}      ; next cel, capacity, bitmap lines, vs
        rsb     r11,r11,r10,lsl #1      ; s0: the screen line of bitmap row 0
bp_mod
        cmp     r11,r10
        subhs   r11,r11,r10
        bhs     bp_mod
        mov     r6,#0                   ; pieces dropped
        mov     r7,#0x100               ; no layer and palette band yet
        stmfd   sp!,{r6,r7}             ; then a at [sp, #8]
bp_band
        ldr     r4,[r9],#4
        ldr     r7,[sp,#4]
        cmp     r7,r4,lsr #24
        bne     bp_key                  ; another layer or palette band, or the end
bp_go
        mov     lr,r4,lsr #16
        and     lr,lr,#0xFF             ; hs
        mov     r6,r4,lsr #8
        and     r6,r6,#0xFF             ; yb
        and     r5,r4,#0xFF             ; ya
        cmp     r6,r11
        bls     bp_up                   ; the band ends at or above line s0
        cmp     r5,r11
        movlo   r5,r11                  ; its lines from s0 at the earliest
        sub     r6,r6,r5
        add     r2,r10,r5,lsl #8        ; show bitmap rows from line - s0
        mov     r5,r5,lsl #16           ; y
        sub     r6,r6,#1
        mov     r6,r6,lsl #6
        orr     r6,r6,#5                ; PRE0: lines - 1, 8 bpp
bplo    BPIECES
        ldrb    r5,[r9,#-1]             ; ya again (the low byte: big endian)
        cmp     r5,r11
        bhs     bp_band                 ; no line above s0
        sub     r6,r11,r5               ; lines ya to s0 - 1
        b       bp_up1
bp_up
        sub     r6,r6,r5                ; lines ya to yb - 1
bp_up1
        add     r2,r12,r5,lsl #8        ; show bitmap rows from line + bm_h - s0
        mov     r5,r5,lsl #16
        sub     r6,r6,#1
        mov     r6,r6,lsl #6
        orr     r6,r6,#5
bphi    BPIECES
        b       bp_band

bp_key
        mov     r7,r4,lsr #24
        cmp     r7,#0xFF
        beq     bp_done
        str     r7,[sp,#4]
        ldr     r6,[sp,#8]              ; a
        and     r1,r7,#1
        add     r1,r6,r1,lsl #2
        ldr     r1,[r1,#BA_BM0]         ; bitmap of the layer
        ldr     r3,[r6,#BA_PLUTS]
        mov     r7,r7,lsr #1            ; palette band
        add     r7,r7,r7,lsl #1
        add     r3,r3,r7,lsl #6         ; its PLUTs: 96 entries of 2 bytes
        sub     r10,r1,r11,lsl #8       ; bitmap row 0 less s0 rows
        ldr     r7,[r6,#BA_BMH]
        sub     r7,r7,r11
        add     r12,r1,r7,lsl #8        ; bitmap row bm_h - s0
        b       bp_go

bplo    BPFULL
bphi    BPFULL

bp_done
        ldr     r6,[sp,#8]
        str     r0,[r6,#BA_C]
        ldr     r0,[sp]
        add     sp,sp,#12
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
S_PARTIAL       EQU     12
S_VIS           EQU     272
S_ROWS          EQU     528
S_ORDER         EQU     0x814
S_NORD          EQU     0x914

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
; screen with an opaque pixel (the VDP's spr_order list), the part of each
; strip under it becomes a cel of its own, with the PLUT of the strip,
; appended to a->p. The rectangle of each strip is packed in a word first,
; and the strips reaching each row of eight lines are noted in a bit mask
; (the first 32 strips), so that a sprite only tests the strips of its
; rows; the strips beyond the first 32 are tested one by one. Gives up
; with 0 when the pixels of the patches exceed a->limit or the cels run
; out; else returns 1 with the pixels in a->area.
;----------------------------------------------------------------------------
PA_STRIPS       EQU     0
PA_NSTRIPS      EQU     4
PA_XOFF         EQU     8
PA_V            EQU     12
PA_SAT          EQU     16
PA_LIMIT        EQU     20
PA_AREA         EQU     24
PA_P            EQU     28
PA_HEIGHT       EQU     32

; Stack frame.
PF_RECT         EQU     0               ; 128 words: strip k as x0 | x1 - 1 << 8 |
                                        ; y0 << 16 | y1 - 1 << 24 (clipped to the screen)
PF_MASK         EQU     512             ; 32 words: bit k: strip k (k < 32) reaches
                                        ; lines 8 r to 8 r + 7
PF_SPR          EQU     640             ; xoff | sprite width << 8 | partial << 16
PF_END          EQU     644             ; end of the sprite list
PF_SIZE         EQU     648

        ; PTEST: skips to $miss when the strip packed in r2 does not meet
        ; the sprite's box r5 .. r4 - 1, r7 .. r6 - 1. Clobbers r3.
        MACRO
        PTEST   $miss
        cmp     r7,r2,lsr #24           ; y1 - 1 < sy0
        bhi     $miss
        and     r3,r2,#0xFF0000
        cmp     r3,r6,lsl #16           ; y0 >= sy1
        bhs     $miss
        and     r3,r2,#0xFF
        cmp     r3,r4                   ; x0 >= sx1
        bhs     $miss
        and     r3,r2,#0xFF00
        cmp     r3,r5,lsl #8            ; x1 - 1 < sx0
        blo     $miss
        MEND

render_prio_patches
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#PF_SIZE
        mov     r11,r0                  ; args
        mov     r10,#0                  ; area
        mov     r0,#0
        mov     r1,#0
        mov     r2,#0
        mov     r3,#0
        add     r12,sp,#PF_MASK
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        stmia   r12!,{r0-r3}
        ; The rectangles of the strips, and the rows each reaches.
        ldr     r3,[r11,#PA_STRIPS]
        ldr     r2,[r11,#PA_NSTRIPS]
        mov     r12,#CCB_SIZE
        mla     r2,r12,r2,r3            ; end of the strips
        mov     r8,#1                   ; the strip's bit (0 from the 33rd on)
        add     r9,sp,#PF_RECT
pk_loop
        cmp     r3,r2
        bhs     pk_done
        ldr     r0,[r3,#CCB_YPOS]
        mov     r0,r0,asr #16           ; first line of the strip
        ldr     r1,[r3,#CCB_PRE0]
        mov     r1,r1,lsr #6
        add     r1,r0,r1                ; its last line
        cmp     r1,#255
        movgt   r1,#255
        ldr     r12,[r3,#CCB_XPOS]
        mov     r12,r12,asr #16         ; first column (a piece may start left of the screen)
        ldr     lr,[r3,#CCB_PRE1]
        mov     lr,lr,lsl #21
        mov     lr,lr,lsr #21
        add     lr,lr,r12               ; its last column
        cmp     lr,#255
        movgt   lr,#255
        cmp     r12,#0
        movlt   r12,#0
        orr     r12,r12,lr,lsl #8
        orr     r12,r12,r0,lsl #16
        orr     r12,r12,r1,lsl #24
        str     r12,[r9],#4
        cmp     r8,#0
        beq     pk_next
        mov     r0,r0,lsr #3            ; its rows
        mov     r1,r1,lsr #3
        sub     r1,r1,r0
        add     lr,sp,#PF_MASK
        add     lr,lr,r0,lsl #2
pk_rows
        ldr     r0,[lr]
        orr     r0,r0,r8
        str     r0,[lr],#4
        subs    r1,r1,#1
        bpl     pk_rows
        movs    r8,r8,lsl #1
pk_next
        add     r3,r3,#CCB_SIZE
        b       pk_loop
pk_done
        ldr     r9,[r11,#PA_V]
        add     r9,r9,#0x4800
        add     r9,r9,#V_SPR-0x4800     ; &spr_n
        ldr     r0,[r9,#S_PARTIAL]      ; spr_vis is only valid in a frame
        ldr     r1,[r11,#PA_XOFF]       ; where some sprite loses lines
        ldr     r2,[r9,#S_ZOOM]
        mov     r3,#8
        mov     r3,r3,lsl r2            ; sprite width
        orr     r1,r1,r3,lsl #8
        orr     r1,r1,r0,lsl #16
        str     r1,[sp,#PF_SPR]
        ldr     r0,[r9,#S_NORD]
        add     r8,r9,#0x800
        add     r8,r8,#S_ORDER-0x800    ; r8 = the sprites, by first row
        add     r0,r8,r0,lsl #2
        str     r0,[sp,#PF_END]
pp_sprite
        ldr     r0,[sp,#PF_END]
        cmp     r8,r0
        bhs     pp_ok
        ldr     r0,[r8],#4              ; sprite | x << 8 | first << 16 | last + 1 << 24
        ldr     r1,[sp,#PF_SPR]
        tst     r1,#0xFF0000
        beq     pp_shown
        and     r2,r0,#0x3F
        add     r2,r9,r2,lsl #2
        ldr     r2,[r2,#S_VIS]
        cmp     r2,#0
        beq     pp_sprite               ; hidden on every line
pp_shown
        mov     r7,r0,lsl #8
        mov     r7,r7,lsr #24           ; r7 = sy0: its first screen line
        mov     r6,r0,lsr #24           ; r6 = sy1: its last screen line + 1
        ldr     r2,[r11,#PA_HEIGHT]
        cmp     r6,r2
        movgt   r6,r2
        cmp     r7,r6
        bge     pp_sprite
        mov     r2,r0,lsr #8
        and     r2,r2,#0xFF             ; x of the table
        and     r3,r1,#0xFF
        sub     r2,r2,r3                ; x on the screen
        mov     r3,r1,lsr #8
        and     r3,r3,#0xFF             ; width
        add     r4,r2,r3
        cmp     r4,#256
        movgt   r4,#256                 ; r4 = sx1
        movs    r5,r2
        movmi   r5,#0                   ; r5 = sx0
        cmp     r5,r4
        bge     pp_sprite
        ; The strips of its rows, then those beyond the first 32.
        mov     r0,r7,lsr #3
        sub     r1,r6,#1
        mov     r1,r1,lsr #3
        sub     r1,r1,r0                ; rows - 1
        add     lr,sp,#PF_MASK
        add     lr,lr,r0,lsl #2
        mov     r0,#0
pp_rows
        ldr     r2,[lr],#4
        orr     r0,r0,r2
        subs    r1,r1,#1
        bpl     pp_rows
        mov     r1,#0                   ; strip
pp_bits
        movs    r0,r0
        beq     pp_rest
        tst     r0,#0xFF
        moveq   r0,r0,lsr #8
        addeq   r1,r1,#8
        beq     pp_bits
        tst     r0,#1
        beq     pp_bnext
        add     r2,sp,#PF_RECT
        ldr     r2,[r2,r1,lsl #2]
        PTEST   pp_bnext
        bl      pp_patch
pp_bnext
        mov     r0,r0,lsr #1
        add     r1,r1,#1
        b       pp_bits
pp_rest
        ldr     r0,[r11,#PA_NSTRIPS]
        mov     r1,#32
        cmp     r1,r0
        bhs     pp_sprite
        add     r12,sp,#PF_RECT+128
pp_rloop
        ldr     r2,[r12],#4
        PTEST   pp_rnext
        bl      pp_patch
pp_rnext
        add     r1,r1,#1
        cmp     r1,r0
        blo     pp_rloop
        b       pp_sprite
pp_fail
        add     sp,sp,#24+PF_SIZE       ; the registers pp_patch saved, the frame
        mov     r0,#0
        ldmfd   sp!,{r4-r11,pc}
pp_ok
        str     r10,[r11,#PA_AREA]
        mov     r0,#1
        add     sp,sp,#PF_SIZE
        ldmfd   sp!,{r4-r11,pc}

; pp_patch: the part of strip r1 (its rectangle packed in r2), which meets
; the sprite's box (r5 .. r4 - 1, r7 .. r6 - 1), becomes a cel. Preserves
; r0, r1, r4, r5 and r12; leaves through pp_fail when the pixels exceed
; the limit or the cels run out.
pp_patch
        stmfd   sp!,{r0,r1,r4,r5,r12,lr}
        ldr     r3,[r11,#PA_STRIPS]
        add     r3,r3,r1,lsl #6
        add     r3,r3,r1,lsl #2         ; the strip's cel
        mov     r0,r2,lsl #8
        mov     r0,r0,lsr #24           ; first line of the strip
        cmp     r0,r7
        movlt   r0,r7                   ; iy0
        mov     r1,r2,lsr #24
        add     r1,r1,#1                ; its end
        cmp     r1,r6
        movgt   r1,r6                   ; iy1
        and     r12,r2,#0xFF            ; first column of the strip on the screen
        cmp     r12,r5
        movlt   r12,r5                  ; ix0
        mov     lr,r2,lsr #8
        and     lr,lr,#0xFF
        add     lr,lr,#1                ; its end
        cmp     lr,r4
        movgt   lr,r4                   ; ix1
        ; A patch: lines r0 to r1 - 1, columns r12 to lr - 1 of the strip.
        sub     r1,r1,r0                ; rows
        sub     lr,lr,r12               ; columns
        mla     r10,r1,lr,r10
        ldr     r2,[r11,#PA_LIMIT]
        cmp     r10,r2
        bhi     pp_fail
        ldr     r2,[r11,#PA_P]
        ldmia   r2,{r4,r5}              ; next cel, capacity
        cmp     r4,r5
        beq     pp_fail
        add     r5,r4,#CCB_SIZE
        str     r5,[r2]                 ; the cel after the patch
        ldr     r5,[r3,#CCB_PLUT]       ; the PLUT of its strip
        str     r5,[r4,#CCB_PLUT]
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
        ldmfd   sp!,{r0,r1,r4,r5,r12,pc}

;----------------------------------------------------------------------------
; void render_prio_groups(render_layer *l)
;
; The priority strip groups of a layer: runs of consecutive name table
; rows holding priority cells, each with the union of the column ranges
; of its rows (pg_row0, pg_nrows, pg_col0, pg_col1, pg_n). The column
; range of a row whose priority cells changed (prio_dirty) is found again
; from the shadow name table first: two entries per word, the priority
; bit in bit 4 of the high byte of each (bits 20 and 4 on this CPU), the
; row holding at least one such cell. A row whose range was just found
; starts a group of its own, as the strips it was in are stale.
;----------------------------------------------------------------------------
render_prio_groups
        stmfd   sp!,{r4-r9,lr}
        ldr     r1,[r0,#R_NROWS]        ; rows
        mov     r2,#0                   ; row
        mov     r3,#0                   ; groups
        add     r4,r0,#R_PRIOROWS
        add     r5,r0,#R_PRIODIRTY
pg_loop
        cmp     r2,r1
        bhs     pg_done
        ldrb    r6,[r4,r2]
        cmp     r6,#0
        addeq   r2,r2,#1
        beq     pg_loop                 ; no priority cell in the row
        ldrb    r6,[r5,r2]
        cmp     r6,#0
        blne    pg_range
        mov     r7,r2                   ; first row of the group
        add     lr,r0,#R_PRIOMIN
        ldrb    r8,[lr,r2]              ; its first column
        add     lr,r0,#R_PRIOMAX
        ldrb    r9,[lr,r2]
        add     r9,r9,#1                ; its last column + 1
pg_more
        add     r2,r2,#1
        cmp     r2,r1
        bhs     pg_emit
        ldrb    r6,[r4,r2]
        cmp     r6,#0
        beq     pg_emit
        ldrb    r6,[r5,r2]
        cmp     r6,#0
        bne     pg_emit
        add     lr,r0,#R_PRIOMIN
        ldrb    r6,[lr,r2]
        cmp     r6,r8
        movlo   r8,r6
        add     lr,r0,#R_PRIOMAX
        ldrb    r6,[lr,r2]
        add     r6,r6,#1
        cmp     r6,r9
        movhi   r9,r6
        b       pg_more
pg_emit
        add     lr,r0,#R_PGROW0
        strb    r7,[lr,r3]
        sub     r6,r2,r7
        add     lr,r0,#R_PGNROWS
        strb    r6,[lr,r3]
        add     lr,r0,#R_PGCOL0
        strb    r8,[lr,r3]
        add     lr,r0,#R_PGCOL1
        strb    r9,[lr,r3]
        add     r3,r3,#1
        b       pg_loop
pg_done
        str     r3,[r0,#R_PGN]
        ldmfd   sp!,{r4-r9,pc}

; pg_range: the first and last column of row r2 with a priority cell,
; from the shadow, into prio_min and prio_max; clears the row's dirty
; byte. Clobbers r6-r9 and r12.
pg_range
        ldr     r6,[r0,#R_SHADOW]
        add     r6,r6,r2,lsl #6         ; the row's 16 words
        mov     r7,r6
        mov     r8,#0x00100000
        orr     r8,r8,#0x10             ; the bit of both entries
pgr_lo
        ldr     r9,[r7],#4
        tst     r9,r8
        beq     pgr_lo
        sub     r7,r7,r6
        sub     r7,r7,#4
        mov     r7,r7,lsr #1            ; two entries per word
        tst     r9,#0x00100000
        addeq   r7,r7,#1                ; the second entry alone
        add     r12,r0,#R_PRIOMIN
        strb    r7,[r12,r2]
        add     r7,r6,#64
pgr_hi
        ldr     r9,[r7,#-4]!
        tst     r9,r8
        beq     pgr_hi
        sub     r7,r7,r6
        mov     r7,r7,lsr #1
        tst     r9,#0x10
        addne   r7,r7,#1                ; the second entry
        add     r12,r0,#R_PRIOMAX
        strb    r7,[r12,r2]
        mov     r9,#0
        strb    r9,[r5,r2]
        mov     pc,lr

        END
