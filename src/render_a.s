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
        EXPORT  cd_prio
        EXPORT  re_prio
        EXPORT  render_stamp_wrap
        EXPORT  render_chain_sig

; CCB fields (graphics.h).
CCB_NEXT        EQU     4
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
R_NODE          EQU     36
R_HEAD          EQU     40
R_WLIST         EQU     44
R_NTBASE        EQU     52
R_PRIOROWS      EQU     64
R_PRIOMIN       EQU     96
R_PRIOMAX       EQU     128
R_PRIODIRTY     EQU     160
R_PGN           EQU     204
R_PGROW0        EQU     208
R_PGNROWS       EQU     240
R_PGCOL0        EQU     272
R_PGCOL1        EQU     304
R_STCELLS       EQU     192
R_STTILECELLS   EQU     196
R_STTILESCONV   EQU     200
R_STALEROWS     EQU     336
R_SHOWNROWS     EQU     340
R_SHOWNCOLS     EQU     440
R_STALECOLS     EQU     444
R_NROWS         EQU     344
R_NCELLS        EQU     348

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
; walks stamp and count the cells they draw, and only draw a cell when the
; picture shows both its row and its column (shown_rows, shown_cols): a
; cell off the picture is left stale instead (its stamp STALE, its bit in
; stale_cols[row], its row's bit in stale_rows) and render_stale_cells
; draws it once the picture shows it.
;
; cell_draw: draws entry r1 into cell r0 of the bitmap (and of the priority
; bitmap when the entry has the priority bit): the rows of the tile store
; the entry needs (plain, or mirrored for a horizontal flip) are copied
; with the palette bit OR-ed into every pixel, in reverse order for a
; vertical flip. A store found missing is filled by cell_conv, which
; converts the tile into the store and into the cell at once. Preserves
; r4-r11, clobbers r0-r3, r12 and lr; the usual cell (no priority
; bit, the store present) touches no memory but the rows.
;
; DRAWCELL is the form the walks use: the usual cell (no flip, no palette
; bit, no priority bit, the plain store present) is copied in place, two
; rows per load since the walks leave r0-r3, r12 and lr free, and the
; others go through cell_draw. Same clobbers as cell_draw.
;----------------------------------------------------------------------------
NONE            EQU     1024            ; end of a cell list
STALE           EQU     0xFF            ; stamp of a stale cell (RENDER_STALE)
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

; One row of a copy: r2 = source (the row, left past it by the load: the
; next row up or down), r3 = destination (the next row follows unless it
; is the last), r12 = palette bits (ROWCOPYC only); ROWCOPYP steps its
; source by lr and writes the priority bitmap (r8) as well.
        MACRO
        ROWCOPY $ld,$last
        $ld     r2!,{r0,r1}
        stmia   r3,{r0,r1}
        IF "$last" = ""
        add     r3,r3,#CELL_ROW
        ENDIF
        MEND

        MACRO
        ROWCOPYC $ld,$last
        $ld     r2!,{r0,r1}
        orr     r0,r0,r12
        orr     r1,r1,r12
        stmia   r3,{r0,r1}
        IF "$last" = ""
        add     r3,r3,#CELL_ROW
        ENDIF
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


        MACRO
        HIDDEN  $c,$label               ; to $label unless the picture shows the row and the column of cell $c (r2, r3 clobbered)
        ldr     r2,[r9,#R_SHOWNROWS]
        mov     r3,$c,lsr #5
        mov     r2,r2,lsr r3
        tst     r2,#1
        ldrne   r2,[r9,#R_SHOWNCOLS]
        andne   r3,$c,#31
        movne   r2,r2,lsr r3
        tstne   r2,#1
        beq     $label
        MEND

        MACRO
        LEAVESTALE $c,$st               ; cell $c (stamps at $st) waits stale: its row and column noted (r2, r3, r12 clobbered)
        mov     r12,#STALE
        strb    r12,[$st,$c]
        mov     r3,$c,lsr #5            ; its row
        ldr     r12,[r9,#R_STALEROWS]
        mov     r2,#1
        orr     r12,r12,r2,lsl r3
        str     r12,[r9,#R_STALEROWS]
        add     r12,r9,#R_STALECOLS
        add     r12,r12,r3,lsl #2
        and     r3,$c,#31               ; its column
        mov     r2,r2,lsl r3
        ldr     r3,[r12]
        orr     r3,r3,r2
        str     r3,[r12]
        MEND

        MACRO
        COPYROWS $src,$dst              ; the eight rows at $src, two per load, into the cell at $dst (r0-r3 clobbered)
        ldmia   $src!,{r0-r3}
        stmia   $dst,{r0,r1}
        add     $dst,$dst,#CELL_ROW
        stmia   $dst,{r2,r3}
        add     $dst,$dst,#CELL_ROW
        ldmia   $src!,{r0-r3}
        stmia   $dst,{r0,r1}
        add     $dst,$dst,#CELL_ROW
        stmia   $dst,{r2,r3}
        add     $dst,$dst,#CELL_ROW
        ldmia   $src!,{r0-r3}
        stmia   $dst,{r0,r1}
        add     $dst,$dst,#CELL_ROW
        stmia   $dst,{r2,r3}
        add     $dst,$dst,#CELL_ROW
        ldmia   $src,{r0-r3}
        stmia   $dst,{r0,r1}
        add     $dst,$dst,#CELL_ROW
        stmia   $dst,{r2,r3}
        MEND

        MACRO
        DRAWCELL                        ; cell r0 takes entry r1: the usual cell copied here, the others drawn by cell_draw
        tst     r1,#0x1E00              ; a flip, the palette bit or the priority bit
        bne     %F8
        ldmia   r9,{r2,r3,r12,lr}       ; the stores, t_ok, the bitmap
        bic     r3,r1,#0xFE00           ; tile
        ldrb    r12,[r12,r3]
        tst     r12,#1                  ; its plain store present?
        beq     %F7
        add     r12,r2,r3,lsl #6        ; its rows
        CELLOFF r3,r0,r2
        add     lr,lr,r3                ; the cell's rows
        COPYROWS r12,lr
        b       %F9
7
        add     r2,r2,r3,lsl #6         ; missing: converted into the store and the cell
        mov     r12,#1
        bl      cell_conv
        b       %F9
8
        bl      cell_draw
9
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
        tst     r1,#0x800
        bne     cd_pal
        tst     r1,#0x400
        bne     cd_down
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia
        ROWCOPY ldmia,last
        mov     pc,lr
cd_down                                 ; vertical flip: last source row first
        add     r2,r2,#60
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda
        ROWCOPY ldmda,last
        mov     pc,lr
cd_pal                                  ; the palette bit OR-ed into every pixel
        mov     r12,#0x10
        orr     r12,r12,r12,lsl #8
        orr     r12,r12,r12,lsl #16
        tst     r1,#0x400
        bne     cd_pal_down
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia
        ROWCOPYC ldmia,last
        mov     pc,lr
cd_pal_down
        add     r2,r2,#60
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda
        ROWCOPYC ldmda,last
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
; The row loops: r4 = planar rows, up to r10, r9 = table, r11 = 0x0F0F0F0F,
; r2 = store rows, r3 = cell rows (stepped by r12), lr = palette bits.
; conv_cell (the cell only) takes two rows per pass; conv_rows writes the
; priority bitmap at r8 as well, or nothing but the store when r3 is 0.
;----------------------------------------------------------------------------
        MACRO
        CONVPIX $h,$l,$s                ; $h = word of pixels 0-3, $l = pixels 4-7 of planar row $s
        and     r6,r11,$s,lsr #4        ; the high nibbles of p0-p3 (p0 in the top byte)
        orr     r6,r6,r6,lsr #4         ; pairs: p0:p1 in bits 16-23, p2:p3 in bits 0-7
        and     r7,r6,#0xFF0000
        ldr     $h,[r9,r7,lsr #14]
        and     r7,r6,#0xFF
        ldr     r7,[r9,r7,lsl #2]
        orr     $h,$h,r7,lsl #2
        and     r6,r11,$s               ; the low nibbles
        orr     r6,r6,r6,lsr #4
        and     r7,r6,#0xFF0000
        ldr     $l,[r9,r7,lsr #14]
        and     r7,r6,#0xFF
        ldr     r7,[r9,r7,lsl #2]
        orr     $l,$l,r7,lsl #2
        MEND

        MACRO
        CELLROW                         ; the row r0, r1 into the store and the cell
        stmia   r2!,{r0,r1}
        orr     r0,r0,lr
        orr     r1,r1,lr
        stmia   r3,{r0,r1}
        add     r3,r3,r12
        MEND

        MACRO
        CONVROW $h,$l
        ldr     r5,[r4],#4
        CONVPIX $h,$l,r5
        stmia   r2!,{r0,r1}
        teq     r3,#0
        orrne   r0,r0,lr
        orrne   r1,r1,lr
        stmneia r3,{r0,r1}
        addne   r3,r3,r12
        stmneia r8,{r0,r1}
        addne   r8,r8,r12
        cmp     r4,r10
        MEND

; cell_conv: the store of the entry's tile missing (r0 = cell, r1 = entry,
; r2 = the store's rows, r12 = its bit in t_ok, r10 = VRAM): the tile
; converted into the store and the cell drawn in the same pass. Entered
; from cell_draw, and directly by the walks for a plain store.
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
        add     r10,r4,#32              ; their end
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
        mov     r11,#0x0F
        orr     r11,r11,r11,lsl #8
        orr     r11,r11,r11,lsl #16
        mov     r12,#CELL_ROW
        tst     r1,#0x400
        addne   r3,r3,#7*CELL_ROW       ; vertical flip: the cell's last row first
        addne   r8,r8,#7*CELL_ROW
        rsbne   r12,r12,#0
        tst     r1,#0x1000
        bne     cv_prio
        tst     r1,#0x200
        bne     conv_cell_m
conv_cell
        ldmia   r4!,{r5,r8}             ; two planar rows
        CONVPIX r0,r1,r5
        CELLROW
        CONVPIX r0,r1,r8
        CELLROW
        cmp     r4,r10
        bne     conv_cell
        b       conv_done
conv_cell_m
        ldmia   r4!,{r5,r8}
        CONVPIX r1,r0,r5
        CELLROW
        CONVPIX r1,r0,r8
        CELLROW
        cmp     r4,r10
        bne     conv_cell_m
        b       conv_done
cv_prio                                 ; the cell and the priority bitmap
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
        add     r10,r4,#32              ; their end
        mov     r9,r5
        mov     r3,#0                   ; the store only
        mov     r11,#0x0F
        orr     r11,r11,r11,lsl #8
        orr     r11,r11,r11,lsl #16
        teq     r6,#0
        beq     conv_rows
        b       conv_rows_m

;----------------------------------------------------------------------------
; void render_entries(render_layer *l, const uint8 *vram, uint32 n)
;
; For each of the n name table word indices in l->wlist (an end mark is
; written after them): each of the word's two entries that differs from
; the shadow has its cell moved to the list of its new tile when the tile
; changed, its priority bookkeeping done when the priority bit changed,
; then, when the picture shows the cell, stamped (which ends a stale
; state) and drawn, else left stale; the word is then copied to the
; shadow. The changes are tested on the word as it lies in memory: an
; entry's low byte holds tile bits 0-7, its high byte tile bit 8 and the
; priority bit (bit 4).
;----------------------------------------------------------------------------
HEAD_BYTES      EQU     2048            ; the heads of the cell lists, before the nodes

        MACRO
        ENTRYCELL $tlo,$thi,$pr         ; cell r0 takes entry r1; r2 = bits of the word that changed
        tst     r2,#$pr
        blne    re_prio
        ands    r3,r2,#$thi             ; tile changed?
        tsteq   r2,#$tlo
        beq     %F1
        ldr     lr,[r9,#R_NODE]         ; the cell leaves the list of its old tile
        add     r3,lr,r0,lsl #3         ; its node
        ldmia   r3,{r2,r12}             ; next cell, the word holding the cell
        str     r2,[r12]
        cmp     r2,#NONE
        addne   r2,lr,r2,lsl #3
        strne   r12,[r2,#4]             ; the next cell is now held there
        bic     r2,r1,#0xFE00           ; and goes first in the list of its new tile
        add     r12,lr,r2,lsl #2
        sub     r12,r12,#HEAD_BYTES     ; the head of the tile
        ldr     r2,[r12]
        str     r0,[r12]
        stmia   r3,{r2,r12}
        cmp     r2,#NONE
        addne   r2,lr,r2,lsl #3
        strne   r3,[r2,#4]              ; the former first cell is held by the node
1
        MEND

render_entries
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r4,[r9,#R_WLIST]
        mvn     r0,#0
        str     r0,[r4,r2,lsl #2]       ; end mark
        ldr     r5,[r9,#R_NTBASE]
        add     r5,r10,r5               ; name table
        add     r6,r9,#R_SHADOW
        ldmia   r6,{r6,r7,r8}           ; shadow, stamps, stamp of the update (cells drawn above it)
re_loop
        ldr     r11,[r4],#4
        movs    r11,r11,lsl #2          ; byte offset of the word; the end mark is negative
        bmi     re_done
        ldr     r1,[r5,r11]             ; new word
        ldr     r2,[r6,r11]             ; old word
        eor     r2,r2,r1
        movs    r3,r2,lsr #16           ; first entry changed?
        beq     re_second
        mov     r0,r11,lsr #1           ; its cell
        mov     r3,r1,lsr #24           ; the entry, low byte first
        and     r12,r1,#0x00FF0000
        orr     r1,r3,r12,lsr #8
        ENTRYCELL 0x10000,0xFF000000,0x100000
        HIDDEN  r0,re_hide1
        strb    r8,[r7,r0]              ; stamped
        add     r8,r8,#256              ; counted
        DRAWCELL
re_first_done
        ldr     r1,[r5,r11]             ; the word again
        ldr     r2,[r6,r11]
        eor     r2,r2,r1
re_second
        str     r1,[r6,r11]             ; the word, as drawn
        movs    r3,r2,lsl #16           ; second entry changed?
        beq     re_loop
        mov     r0,r11,lsr #1
        add     r0,r0,#1                ; its cell
        and     r3,r1,#0xFF00
        and     r12,r1,#0xFF
        mov     r3,r3,lsr #8
        orr     r1,r3,r12,lsl #8
        ENTRYCELL 1,0xFF00,0x10
        HIDDEN  r0,re_hide2
        strb    r8,[r7,r0]              ; stamped
        add     r8,r8,#256              ; counted
        DRAWCELL
        b       re_loop
re_hide1
        LEAVESTALE r0,r7
        b       re_first_done
re_hide2
        LEAVESTALE r0,r7
        b       re_loop
re_done
        mov     r8,r8,lsr #8
        ldr     r0,[r9,#R_STCELLS]
        add     r0,r0,r8
        str     r0,[r9,#R_STCELLS]
        ldmfd   sp!,{r4-r11,pc}

; re_prio: cell r0 took entry r1, whose priority bit changed: the count of
; priority cells of its row and the row's dirty flag updated, the cell
; cleared in the priority bitmap when the bit went away. Preserves r0-r2
; and r4-r11.
re_prio
        stmfd   sp!,{r1,r2,lr}
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
        ldmnefd sp!,{r1,r2,pc}
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
        ldmfd   sp!,{r1,r2,pc}

;----------------------------------------------------------------------------
; void render_stamp_wrap(uint8 *stamp, uint32 nwords)
;
; ORs 0xFE into every byte of the nwords words of stamps (a multiple of 8):
; the stamps become RENDER_NEUTRAL, the stale cells stay RENDER_STALE.
;----------------------------------------------------------------------------
render_stamp_wrap
        stmfd   sp!,{r4-r9,lr}
        mov     r12,#0xFE
        orr     r12,r12,r12,lsl #8
        orr     r12,r12,r12,lsl #16
sw_loop
        ldmia   r0,{r2-r9}
        orr     r2,r2,r12
        orr     r3,r3,r12
        orr     r4,r4,r12
        orr     r5,r5,r12
        orr     r6,r6,r12
        orr     r7,r7,r12
        orr     r8,r8,r12
        orr     r9,r9,r12
        stmia   r0!,{r2-r9}
        subs    r1,r1,#8
        bhi     sw_loop
        ldmfd   sp!,{r4-r9,pc}

;----------------------------------------------------------------------------
; void render_tile_cells(render_layer *l, const uint8 *vram, uint32 t)
;
; Draws the cells of the list of tile t that are not stamped in this
; update, with their entry from the shadow; counts them in st.cells and
; st.tile_cells. A cell the picture does not show (shown_rows,
; shown_cols) is left stale instead, for render_stale_cells.
;
; uint32 render_tile_cells_part(render_layer *l, const uint8 *vram, uint32 t,
;                               uint32 from, uint32 budget)
;
; The same for the list of tile t from cell from on (NONE: from the head
; of the list), stopping once budget cells have been drawn. Returns the
; cell to continue from, NONE when the list is done. Used by the catch-up
; of the picture in spare time, which looks at the clock between two
; calls; the cells drawn are no longer stale.
;
; The walk: r4 = shadow, r5 = stamps, r6 = stamp_now | cells drawn << 8,
; r7 = nodes, r8 = the tile's plain store (bit 0 set until it is
; converted, which cell_draw does at the first plain cell), r9 = layer,
; r10 = bitmap (the VRAM, pushed at entry, is taken back from the stack
; for cell_draw), r11 = cell. Every cell of the list shows the tile, so the usual cell
; copies the one store without looking its tile up. A cell found stale
; is off the picture (render_stale_cells, run before the tile walks of an
; update, drew the stale cells the picture shows) and is passed over.
;----------------------------------------------------------------------------
        MACRO
        CELLENTRY                       ; r1 = entry of cell r11 from the shadow r4
        add     r1,r4,r11,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8
        MEND

        MACRO
        TCSETUP $t                      ; the walk registers for tile $t (r11 = first cell)
        ldr     r4,[r9,#R_TOK]
        ldrb    r4,[r4,$t]
        ldr     r8,[r9,#R_TILES]
        add     r8,r8,$t,lsl #6         ; the tile's plain store
        tst     r4,#1
        orreq   r8,r8,#1                ; not converted yet
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6,r7}        ; shadow, stamps, stamp_now, nodes
        ldr     r10,[r9,#R_BITMAP]
        MEND

        MACRO
        TCWALK  $p,$vo                  ; the walk; $p names its labels, the VRAM lies at [sp,#$vo]
tc_loop$p
        cmp     r11,#NONE
        beq     tc_done$p
        ldrb    r0,[r5,r11]
        and     r1,r6,#0xFF
        cmp     r0,r1
        cmpne   r0,#STALE
        beq     tc_next$p               ; drawn in this update already, or waiting off the picture
        HIDDEN  r11,tc_stale$p
        IF "$p" = "1"
        ldr     r0,[sp]
        cmp     r0,#0                   ; budget spent: continue from this cell
        beq     tc_done$p
        sub     r0,r0,#1
        str     r0,[sp]
        ENDIF
        CELLENTRY
        strb    r6,[r5,r11]             ; stamped
        add     r6,r6,#256              ; counted
        tst     r1,#0x1E00
        bne     tc_slow$p               ; a flip, the palette bit or the priority bit
        tst     r8,#1
        bne     tc_conv$p               ; the plain store missing
        CELLOFF lr,r11,r2
        add     lr,lr,r10               ; the cell's rows
        mov     r12,r8
        COPYROWS r12,lr
tc_next$p
        ldr     r11,[r7,r11,lsl #3]
        b       tc_loop$p
tc_stale$p
        LEAVESTALE r11,r5
        b       tc_next$p
tc_conv$p
        mov     r0,r11
        bic     r2,r8,#1                ; the store to fill
        mov     r12,#1
        ldr     r10,[sp,#$vo]           ; VRAM: cell_conv converts the store and draws the cell
        bl      cell_conv
        ldr     r10,[r9,#R_BITMAP]
        bic     r8,r8,#1                ; the plain store is present from now on
        b       tc_next$p
tc_slow$p
        mov     r0,r11
        ldr     r10,[sp,#$vo]
        bl      cell_draw
        ldr     r10,[r9,#R_BITMAP]
        b       tc_next$p
tc_done$p
        mov     r8,r6,lsr #8            ; cells drawn
        ldr     r0,[r9,#R_STCELLS]
        ldr     r1,[r9,#R_STTILECELLS]
        add     r0,r0,r8
        add     r1,r1,r8
        str     r0,[r9,#R_STCELLS]
        str     r1,[r9,#R_STTILECELLS]
        MEND

render_tile_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        str     r1,[sp,#-4]!            ; VRAM, for cell_draw
        ldr     r4,[r9,#R_HEAD]
        ldr     r11,[r4,r2,lsl #2]      ; first cell of the list
        TCSETUP r2
        TCWALK  0,0
        add     sp,sp,#4
        ldmfd   sp!,{r4-r11,pc}

render_tile_cells_part
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        ldr     r0,[sp,#36]             ; budget
        stmfd   sp!,{r0,r1}             ; the budget, the VRAM for cell_draw
        cmp     r3,#NONE
        ldreq   r4,[r9,#R_HEAD]
        ldreq   r11,[r4,r2,lsl #2]      ; from the head of the list
        movne   r11,r3
        TCSETUP r2
        TCWALK  1,4
        add     sp,sp,#8
        mov     r0,r11
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; void render_stale_cells(render_layer *l, const uint8 *vram)
;
; Draws the stale cells the picture shows (stale_cols of the rows in
; stale_rows, in the columns of shown_cols; those not stamped STALE any
; more were drawn since and are only forgotten), from the shadow. A row
; keeps its stale bit while cells of it wait in columns off the picture:
; such a row costs the pass a load and a test every update (about ten
; cycles).
;----------------------------------------------------------------------------
render_stale_cells
        stmfd   sp!,{r4-r11,lr}
        mov     r9,r0
        mov     r10,r1
        ldr     r0,[r9,#R_STALEROWS]
        ldr     r1,[r9,#R_SHOWNROWS]
        and     r11,r0,r1               ; rows to visit
        ldr     r8,[r9,#R_SHOWNCOLS]
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6}           ; shadow, stamps, stamp_now
        mov     r7,#0                   ; row
sc_rows
        movs    r11,r11,lsr #1
        bcc     sc_row_done
        add     r12,r9,#R_STALECOLS
        ldr     r2,[r12,r7,lsl #2]      ; the stale cells of the row
        ands    r3,r2,r8                ; those shown are drawn,
        bne     sc_row                  ; none: the row keeps waiting
sc_row_done
        add     r7,r7,#1
        teq     r11,#0
        bne     sc_rows
        ldmfd   sp!,{r4-r11,pc}
sc_row
        bic     r2,r2,r8
        str     r2,[r12,r7,lsl #2]      ; those off the picture stay
        teq     r2,#0
        ldreq   r2,[r9,#R_STALEROWS]    ; none left: the row is no longer stale
        moveq   r12,#1
        biceq   r2,r2,r12,lsl r7
        streq   r2,[r9,#R_STALEROWS]
        mov     r2,r3
        mov     r3,r7,lsl #5            ; the row's first cell
sc_bits                                 ; r2 = columns left, bit 0 for cell r3
        tst     r2,#0xFF
        moveq   r2,r2,lsr #8
        addeq   r3,r3,#8
        beq     sc_bits
        tst     r2,#0xF
        moveq   r2,r2,lsr #4
        addeq   r3,r3,#4
        tst     r2,#3
        moveq   r2,r2,lsr #2
        addeq   r3,r3,#2
        tst     r2,#1
        moveq   r2,r2,lsr #1
        addeq   r3,r3,#1
        ldrb    r0,[r5,r3]
        teq     r0,#STALE
        bne     sc_cell_done            ; drawn since its entry changed
        stmfd   sp!,{r2,r3}
        add     r1,r4,r3,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8         ; its entry
        mov     r0,r3
        strb    r6,[r5,r3]              ; stamped
        DRAWCELL
        ldr     r0,[r9,#R_STCELLS]      ; counted
        ldr     r1,[r9,#R_STTILECELLS]
        add     r0,r0,#1
        add     r1,r1,#1
        str     r0,[r9,#R_STCELLS]
        str     r1,[r9,#R_STTILECELLS]
        ldmfd   sp!,{r2,r3}
sc_cell_done
        movs    r2,r2,lsr #1            ; the next column
        add     r3,r3,#1
        bne     sc_bits
        b       sc_row_done

;----------------------------------------------------------------------------
; void render_rebuild_cells(render_layer *l, const uint8 *vram)
;
; Links every cell of the table (ncells) into the list of its tile (the
; heads must hold NONE), counts the priority cells of each row and draws
; every cell the picture shows, from the entries of the shadow; the
; others are left stale.
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
        str     r3,[sp,#-4]!            ; last cell + 1
        add     r4,r9,#R_SHADOW
        ldmia   r4,{r4,r5,r6,r7}        ; shadow, stamps, stamp_now, nodes
        mov     r8,#0                   ; cells drawn
rb_loop
        add     r1,r4,r11,lsl #1
        ldrb    r0,[r1]
        ldrb    r1,[r1,#1]
        orr     r1,r0,r1,lsl #8         ; entry
        bic     r2,r1,#0xFE00           ; tile
        add     r12,r7,r2,lsl #2
        sub     r12,r12,#HEAD_BYTES     ; its head
        ldr     r3,[r12]                ; the cell goes first in its list
        str     r11,[r12]
        add     r0,r7,r11,lsl #3        ; node of the cell
        stmia   r0,{r3,r12}
        cmp     r3,#NONE
        addne   r3,r7,r3,lsl #3
        strne   r0,[r3,#4]              ; the former first cell is held by the node
        tst     r1,#0x1000
        beq     rb_shown
        mov     r2,r11,lsr #5           ; a priority cell: counted in its row
        add     r3,r9,#R_PRIOROWS
        ldrb    r0,[r3,r2]
        add     r0,r0,#1
        strb    r0,[r3,r2]
rb_shown
        HIDDEN  r11,rb_stale
        mov     r0,r11
        strb    r6,[r5,r11]             ; stamped
        add     r8,r8,#1
        DRAWCELL
rb_next
        add     r11,r11,#1
        ldr     r0,[sp]
        cmp     r11,r0
        bne     rb_loop
        add     sp,sp,#4
        ldr     r0,[r9,#R_STCELLS]
        add     r0,r0,r8
        str     r0,[r9,#R_STCELLS]
rb_done
        mov     r0,r11
        ldmfd   sp!,{r4-r11,pc}
rb_stale
        LEAVESTALE r11,r5
        b       rb_next
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
S_ORDER         EQU     0x714
S_NORD          EQU     0x814

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
; appended to a->p. Strip by strip: the sprites are listed by first
; screen line, so a strip tests them from the start of the list and stops
; at the first one starting below it; the columns are compared in sprite
; table coordinates (the strip's shifted by a->xoff), and the box of a
; sprite is cut to the strip, which is within the picture. Each patch is
; chained to the cel after it (the last one to the cel past them). Gives
; up with 0 when the pixels of the patches exceed a->limit or the cels
; run out; else returns 1 with the pixels in a->area.
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
PF_SEND         EQU     0               ; end of the strips
PF_HEIGHT       EQU     4               ; picture lines
PF_XOFF         EQU     8               ; the sprite shift (register 0 bit 3)
PF_W            EQU     12              ; sprite width
PF_ORDER        EQU     16              ; the sprites, by first line
PF_OEND         EQU     20              ; end of the sprite list
PF_VIS          EQU     24              ; spr_vis when some sprite loses lines, else 0
PF_CEND         EQU     28              ; cel capacity
PF_LIMIT        EQU     32              ; pixel limit
PF_ARGS         EQU     36
PF_SIZE         EQU     40
PH_SAVED        EQU     24              ; registers pushed on a hit, above the frame

render_prio_patches
        stmfd   sp!,{r4-r11,lr}
        sub     sp,sp,#PF_SIZE
        str     r0,[sp,#PF_ARGS]
        ldr     r4,[r0,#PA_STRIPS]
        ldr     r1,[r0,#PA_NSTRIPS]
        mov     r2,#CCB_SIZE
        mla     r1,r2,r1,r4
        str     r1,[sp,#PF_SEND]
        ldr     r1,[r0,#PA_HEIGHT]
        str     r1,[sp,#PF_HEIGHT]
        ldr     r1,[r0,#PA_XOFF]
        str     r1,[sp,#PF_XOFF]
        ldr     r10,[r0,#PA_LIMIT]      ; r10 = pixels left before the limit
        str     r10,[sp,#PF_LIMIT]
        ldr     r1,[r0,#PA_P]
        ldmia   r1,{r5,r6}              ; r5 = next cel, capacity
        str     r6,[sp,#PF_CEND]
        ldr     r9,[r0,#PA_V]
        add     r9,r9,#0x4800
        add     r9,r9,#V_SPR-0x4800     ; &spr_n
        ldr     r1,[r9,#S_ZOOM]
        mov     r2,#8
        mov     r2,r2,lsl r1            ; sprite width
        str     r2,[sp,#PF_W]
        ldr     r1,[r9,#S_PARTIAL]      ; spr_vis is only valid in a frame
        cmp     r1,#0                   ; where some sprite loses lines
        addne   r1,r9,#S_VIS
        str     r1,[sp,#PF_VIS]
        ldr     r1,[r9,#S_NORD]
        add     r9,r9,#0x700
        add     r9,r9,#S_ORDER-0x700
        str     r9,[sp,#PF_ORDER]
        add     r1,r9,r1,lsl #2
        str     r1,[sp,#PF_OEND]
ps_strip
        ldr     r0,[sp,#PF_SEND]
        cmp     r4,r0
        bhs     ps_ok
        ; The strip's lines r6 .. r7 - 1 and, in sprite table coordinates,
        ; columns r8 .. r9 - 1, cut to the picture.
        ldr     r0,[r4,#CCB_YPOS]
        mov     r6,r0,asr #16           ; first line of the strip
        ldr     r0,[r4,#CCB_PRE0]
        mov     r0,r0,lsr #6
        add     r7,r6,r0
        add     r7,r7,#1                ; its end
        ldr     r0,[sp,#PF_HEIGHT]
        cmp     r7,r0
        movgt   r7,r0
        cmp     r6,r7
        bge     ps_next
        ldr     r0,[r4,#CCB_XPOS]
        mov     r8,r0,asr #16           ; first column (a piece may start left of the screen)
        ldr     r0,[r4,#CCB_PRE1]
        mov     r0,r0,lsl #21
        mov     r0,r0,lsr #21
        add     r9,r8,r0
        add     r9,r9,#1                ; its end
        cmp     r9,#256
        movgt   r9,#256
        cmp     r8,#0
        movlt   r8,#0
        ldr     r0,[sp,#PF_XOFF]
        add     r8,r8,r0
        add     r9,r9,r0
        ldr     r12,[sp,#PF_ORDER]
        ldr     r11,[sp,#PF_OEND]
        ldr     lr,[sp,#PF_W]
        ; The sprites ending above the strip come first in the list: four
        ; at a time while the fourth does.
ps_skip
        add     r0,r12,#16
        cmp     r0,r11
        bhi     ps_sprite
        ldr     r1,[r12,#12]
        cmp     r6,r1,lsr #24
        movhs   r12,r0
        bhs     ps_skip
ps_sprite
        cmp     r12,r11
        bhs     ps_next
        ldr     r0,[r12],#4             ; sprite | x << 8 | first << 16 | last + 1 << 24
        cmp     r6,r0,lsr #24           ; ends above the strip
        bhs     ps_sprite
        mov     r1,r0,lsl #8
        mov     r1,r1,lsr #24
        cmp     r1,r7                   ; starts below it: so do those after
        bge     ps_next
        mov     r1,r0,lsr #8
        and     r1,r1,#0xFF             ; x of the table
        cmp     r1,r9
        bhs     ps_sprite               ; right of the strip
        add     r2,r1,lr
        cmp     r2,r8
        bls     ps_sprite               ; left of it
        ldr     r3,[sp,#PF_VIS]
        cmp     r3,#0
        beq     ps_hit
        and     r2,r0,#0x3F
        ldr     r3,[r3,r2,lsl #2]
        cmp     r3,#0
        beq     ps_sprite               ; hidden on every line
ps_hit
        ; The part of the strip under the sprite: lines r2 .. r2 + r3 - 1,
        ; columns r1 .. r1 + r0 - 1 (table coordinates).
        mov     r2,r0,lsl #8
        mov     r2,r2,lsr #24
        cmp     r2,r6
        movlo   r2,r6                   ; first line of the patch
        mov     r3,r0,lsr #24
        cmp     r3,r7
        movhi   r3,r7                   ; its end
        sub     r3,r3,r2                ; rows
        add     r0,r1,lr
        cmp     r0,r9
        movhi   r0,r9                   ; end column
        cmp     r1,r8
        movlo   r1,r8                   ; first column
        sub     r0,r0,r1                ; columns
        stmfd   sp!,{r6-r9,r11,r12}
        mul     r6,r3,r0
        subs    r10,r10,r6              ; pixels left
        bmi     ps_fail
        ldr     r6,[sp,#PH_SAVED+PF_CEND]
        cmp     r5,r6
        beq     ps_fail
        ldr     r6,[sp,#PH_SAVED+PF_XOFF]
        sub     r1,r1,r6                ; first column on the screen
        ldr     r7,[r4,#CCB_XPOS]
        sub     r7,r1,r7,asr #16        ; columns into the strip
        and     r11,r7,#3               ; pixels before the word boundary
        sub     r7,r7,r11
        ldr     r6,[r4,#CCB_YPOS]
        sub     r6,r2,r6,asr #16        ; rows into the strip
        ldr     r9,[r4,#CCB_SOURCE]
        add     r6,r9,r6,lsl #8
        add     r6,r6,r7                ; source
        ldr     r7,[r4,#CCB_PLUT]       ; the PLUT of its strip
        sub     r8,r1,r11
        mov     r8,r8,lsl #16           ; x
        mov     r9,r2,lsl #16           ; y
        add     r12,r5,#CCB_SOURCE
        stmia   r12,{r6-r9}
        add     r11,r11,r0
        sub     r11,r11,#1
        orr     r11,r11,#0x3E0000       ; PRE1: 64 - 2 words per row, width - 1
        orr     r11,r11,#0x1000
        sub     r9,r3,#1
        mov     r9,r9,lsl #6
        orr     r9,r9,#5                ; PRE0: rows - 1, 8 bpp
        add     r12,r5,#CCB_PRE0
        stmia   r12,{r9,r11}
        add     r5,r5,#CCB_SIZE
        str     r5,[r5,#CCB_NEXT-CCB_SIZE]  ; chained to the cel after it
        ldmfd   sp!,{r6-r9,r11,r12}
        b       ps_sprite
ps_next
        add     r4,r4,#CCB_SIZE
        b       ps_strip
ps_fail
        add     sp,sp,#PH_SAVED+PF_SIZE
        mov     r0,#0
        ldmfd   sp!,{r4-r11,pc}
ps_ok
        ldr     r0,[sp,#PF_ARGS]
        ldr     r1,[sp,#PF_LIMIT]
        sub     r1,r1,r10
        str     r1,[r0,#PA_AREA]
        ldr     r1,[r0,#PA_P]
        str     r5,[r1]                 ; the cel after the patches
        mov     r0,#1
        add     sp,sp,#PF_SIZE
        ldmfd   sp!,{r4-r11,pc}

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
        tst     r2,#3
        bne     pg_row
        ldr     r6,[r4,r2]              ; four rows at once
        cmp     r6,#0
        addeq   r2,r2,#4
        beq     pg_loop                 ; no priority cell in them
pg_row
        ldrb    r6,[r4,r2]
        cmp     r6,#0
        addeq   r2,r2,#1
        beq     pg_loop                 ; no priority cell in the row
        ldrb    r6,[r5,r2]
        cmp     r6,#0
        blne    pg_range
        mov     r7,r2                   ; first row of the group
        add     r12,r0,#R_PRIOMIN
        add     lr,r0,#R_PRIOMAX
        ldrb    r8,[r12,r2]             ; its first column
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
        ldrb    r6,[r12,r2]
        cmp     r6,r8
        movlo   r8,r6
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

; uint32 render_chain_sig(const CCB *c, uint32 seed): signature of the cel
; chain from c (r0), followed through the next pointers up to NULL. Per
; cel the fields the renderer changes from frame to frame (source, PLUT,
; position, the horizontal step, the vertical step, the two preamble
; words; the flags only move CCB_LAST to the end of the chain) are mixed
; into one word c by additions and exclusive ors at different rotations,
; folded into the signature h by h ^= c, h += h << 10, h ^= h >> 6. Each
; step is a bijection of h, so a chain whose words differ for one cel
; always gets another signature; the shift down keeps two cels changed
; alike (sprites moving together) from cancelling, which a fold by
; multiplication alone does not (h * 33 never moves a change in the top
; bits down). About 38 cycles per cel.
render_chain_sig
        stmfd   sp!,{r4-r11,lr}
        movs    r0,r0
        beq     rcs_done
rcs_loop
        ldmib   r0,{r2-r10}             ; next, source, PLUT, x, y, HDX, HDY, VDX, VDY
        add     r12,r0,#CCB_PRE0
        ldmia   r12,{r11,r12}           ; PRE0, PRE1
        eor     r3,r3,r4,ror #7
        add     r3,r3,r5,ror #13
        eor     r3,r3,r6,ror #19
        add     r3,r3,r7,ror #3
        eor     r3,r3,r10,ror #23
        add     r3,r3,r11,ror #29
        eor     r3,r3,r12,ror #17
        eor     r1,r1,r3
        add     r1,r1,r1,lsl #10
        eor     r1,r1,r1,lsr #6
        movs    r0,r2
        bne     rcs_loop
rcs_done
        mov     r0,r1
        ldmfd   sp!,{r4-r11,pc}

        END
