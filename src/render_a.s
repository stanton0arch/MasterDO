; render_a.s - hot loops of the cel renderer (render.c) in assembly.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  render_nt_scan
        EXPORT  render_dirty_scan
        EXPORT  render_sprites
        EXPORT  render_entries
        EXPORT  render_tile_cells
        EXPORT  render_rebuild_cells

        IMPORT  render_tile_conv

; CCB fields (graphics.h).
CCB_SOURCE      EQU     8
CCB_XPOS        EQU     16
CCB_YPOS        EQU     20
CCB_SIZE        EQU     68

; renderer fields (render.h; offsets checked in render.c).
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
R_PRIOROWS      EQU     68
R_PRIODIRTY     EQU     152
R_NTBASE        EQU     180
R_STCELLS       EQU     216
R_STTILECELLS   EQU     224

; render_sprite_args fields (render.h).
SA_TILES        EQU     0
SA_TOK          EQU     4
SA_TBASE        EQU     8
SA_TMASK        EQU     12
SA_XOFF         EQU     16
SA_NEED         EQU     20
SA_TALL         EQU     24

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
        add     r10,r5,r10              ; t_ok + 1 for tall sprites, else t_ok
        add     r3,r0,#0x80             ; x / tile pairs
        str     r9,[sp,#-4]!            ; start of the need list
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
        add     r12,r12,#1
        cmp     r12,#240
        subgt   r12,r12,#256
        mov     r12,r12,lsl #16
        str     r12,[r2,#CCB_YPOS]
        add     r2,r2,#CCB_SIZE
        subs    r1,r1,#1
        bne     spr_loop
spr_done
        ldr     r0,[sp],#4
        sub     r0,r9,r0
        mov     r0,r0,lsr #2
        ldmfd   sp!,{r4-r11,pc}

;----------------------------------------------------------------------------
; Cells. Common registers of the routines below: r9 = renderer, r10 = VRAM.
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

        END
