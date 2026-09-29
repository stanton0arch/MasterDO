; render_a.s - hot loops of the cel renderer (render.c) in assembly.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  render_nt_scan
        EXPORT  render_dirty_scan
        EXPORT  render_sprites
        EXPORT  render_cell_copy

; CCB fields (graphics.h).
CCB_SOURCE      EQU     8
CCB_XPOS        EQU     16
CCB_YPOS        EQU     20
CCB_SIZE        EQU     68

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
; void render_cell_copy(uint32 *dst, const uint32 *src, uint32 pal,
;                       int32 step, uint32 *prio)
;
; Copies the 8 rows of two words of a converted tile into a cell of the
; 256-byte-wide bitmap at dst, OR-ing the palette bits into every pixel;
; src moves by step bytes per row (8, or -8 from the last row for a
; vertical flip). The same rows go to prio when it is not 0.
;----------------------------------------------------------------------------
        MACRO
        CELLROW $off
        ldmia   r1,{r4,r5}
        orr     r4,r4,r2
        orr     r5,r5,r2
        add     r1,r1,r3
        stmia   r0,{r4,r5}
        add     r0,r0,#256
        MEND

        MACRO
        CELLROWP $off
        ldmia   r1,{r4,r5}
        orr     r4,r4,r2
        orr     r5,r5,r2
        add     r1,r1,r3
        stmia   r0,{r4,r5}
        add     r0,r0,#256
        stmia   r12,{r4,r5}
        add     r12,r12,#256
        MEND

render_cell_copy
        stmfd   sp!,{r4,r5,lr}
        ldr     r12,[sp,#12]            ; prio
        teq     r12,#0
        bne     cell_prio
        CELLROW
        CELLROW
        CELLROW
        CELLROW
        CELLROW
        CELLROW
        CELLROW
        CELLROW
        ldmfd   sp!,{r4,r5,pc}
cell_prio
        CELLROWP
        CELLROWP
        CELLROWP
        CELLROWP
        CELLROWP
        CELLROWP
        CELLROWP
        CELLROWP
        ldmfd   sp!,{r4,r5,pc}

        END
