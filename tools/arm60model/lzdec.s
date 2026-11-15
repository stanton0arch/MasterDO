; lzdec.s - decoder of the bank store's format (lzcomp.c: LZ4 block format,
; one 16 KiB bank per stream), run by the model's -lztest to measure what
; a bank miss would cost on the ARM60.
;
; Every load costs about 5.25 cycles and every store 6.4 on the ARM60, a
; byte as much as a word: the output is written in whole words. r1 is
; the next output byte and r3 the word at r1 AND NOT 3 as last stored, its
; top (r1 AND 3) bytes being output (big-endian). A copy of n bytes from
; a source s stores the words from r1 AND NOT 3 on: the first merges the
; kept bytes of r3 with the source, the others come from the source
; shifted to the output's alignment (two aligned loads per word, or one
; when source and output agree), and the bytes stored past r1 + n are
; rewritten by the next copy. A match copies word by word from 8 bytes
; back or more (each word read is already written); nearer matches and
; one or two literals go byte by byte.

        AREA    |C$$code|, CODE, READONLY

        EXPORT  lz4_dec_bank

;----------------------------------------------------------------------------
; COPYW: n = r5 (1 or more) bytes from r6 to r1; r3 as above. Uses r2,
; r3, r5-r10, r12, lr; keeps r0, r4, r11. $kind: lit (literals: the
; words go four at a time) or match (r4 nonzero when the source is 20
; bytes back or more: four words read before any is stored are then all
; written). Ends with a branch to $exit.
;----------------------------------------------------------------------------
        MACRO
$p      COPYW   $exit,$kind
        and     r7,r1,#3                ; output bytes already in the word
        sub     r6,r6,r7                ; source of the first word
        bic     r8,r1,#3
        add     r1,r1,r5
        add     r5,r5,r7
        add     r5,r5,#3
        mov     r5,r5,lsr#2             ; words to store
        mov     r7,r7,lsl#3
        mvn     lr,#0
        mov     r7,lr,lsr r7            ; bytes of the first word taken from the source
        ands    r9,r6,#3
        bic     r6,r6,#3
        bic     r3,r3,r7                ; output bytes kept
        beq     $p.al
        mov     r9,r9,lsl#3
        rsb     r10,r9,#32
        ldmia   r6!,{r2,r12}
        mov     lr,r2,lsl r9
        orr     lr,lr,r12,lsr r10
        and     lr,lr,r7
        orr     lr,lr,r3
        str     lr,[r8],#4
        subs    r5,r5,#1
        beq     $p.end
        mov     r2,r12
        cmp     r5,#4
        blo     $p.s1
        [ "$kind" = "match"
        teq     r4,#0
        beq     $p.s1
        ]
$p.s4   ldmia   r6!,{r3,r7,r12,lr}
        mov     r2,r2,lsl r9
        orr     r2,r2,r3,lsr r10
        mov     r3,r3,lsl r9
        orr     r3,r3,r7,lsr r10
        mov     r7,r7,lsl r9
        orr     r7,r7,r12,lsr r10
        mov     r12,r12,lsl r9
        orr     r12,r12,lr,lsr r10
        stmia   r8!,{r2,r3,r7,r12}
        mov     r2,lr
        sub     r5,r5,#4
        cmp     r5,#4
        bhs     $p.s4
        cmp     r5,#0
        moveq   lr,r12
        beq     $p.end
$p.s1   ldr     r12,[r6],#4
        mov     lr,r2,lsl r9
        orr     lr,lr,r12,lsr r10
        str     lr,[r8],#4
        mov     r2,r12
        subs    r5,r5,#1
        bne     $p.s1
        mov     r3,lr
        b       $exit
$p.al   ldr     lr,[r6],#4
        and     lr,lr,r7
        orr     lr,lr,r3
        str     lr,[r8],#4
        subs    r5,r5,#1
        beq     $p.end
        cmp     r5,#4
        blo     $p.a1
        [ "$kind" = "match"
        teq     r4,#0
        beq     $p.a1
        ]
$p.a4   ldmia   r6!,{r2,r3,r7,lr}
        stmia   r8!,{r2,r3,r7,lr}
        sub     r5,r5,#4
        cmp     r5,#4
        bhs     $p.a4
        cmp     r5,#0
        beq     $p.end
$p.a1   ldr     lr,[r6],#4
        str     lr,[r8],#4
        subs    r5,r5,#1
        bne     $p.a1
$p.end  mov     r3,lr
        b       $exit
        MEND

;----------------------------------------------------------------------------
; lz4_dec_bank: r0 = compressed bank, r1 = output (word aligned, 16 KiB).
; Returns r0 = end of the compressed bank.
;----------------------------------------------------------------------------
lz4_dec_bank
        stmfd   sp!,{r4-r11,lr}
        add     r11,r1,#0x4000          ; end of the output
        mov     r3,#0
seq
        ldrb    r4,[r0],#1              ; token
        movs    r5,r4,lsr#4             ; literals
        and     r4,r4,#15               ; match length - 4
        beq     match
        cmp     r5,#15
        bleq    extlen
        mov     r6,r0
        add     r0,r0,r5
        cmp     r5,#3
        blo     lit_bytes
lc      COPYW   lit_end,lit
lit_bytes
        ldrb    r7,[r6],#1
        strb    r7,[r1],#1
        subs    r5,r5,#1
        bne     lit_bytes
        bic     r8,r1,#3
        ldr     r3,[r8]
lit_end
        cmp     r1,r11
        bhs     done
match
        ldrb    r6,[r0],#1
        ldrb    r7,[r0],#1
        mov     r5,r4
        orr     r7,r6,r7,lsl#8          ; offset
        cmp     r5,#15
        bleq    extlen
        add     r5,r5,#4
        sub     r6,r1,r7                ; source
        cmp     r7,#8
        blo     near
        cmp     r7,#20
        movlo   r4,#0                   ; near: word by word
        movhs   r4,#1
mc      COPYW   seq,match

; Matches less than 8 bytes back. 1 and 4 back, most of them (fills and
; repeated words of graphics): every whole word of the copy is the same
; word, the first merged with the kept bytes; the others byte by byte.
near
        cmp     r7,#4
        cmpne   r7,#1
        bne     near_bytes
        bic     r8,r1,#3
        cmp     r7,#1
        ldreqb  r2,[r1,#-1]
        ldrne   r2,[r8,#-4]             ; 4 back: the word before, merged below
        orreq   r2,r2,r2,lsl#8
        orreq   r2,r2,r2,lsl#16
        and     r9,r1,#3
        add     r1,r1,r5
        add     r5,r5,r9
        add     r5,r5,#3
        mov     r5,r5,lsr#2             ; words to store
        mov     r9,r9,lsl#3
        mvn     lr,#0
        mov     r9,lr,lsr r9
        bic     r3,r3,r9
        and     lr,r2,r9
        orr     lr,lr,r3                ; first word
        cmp     r7,#4
        moveq   r2,lr                   ; 4 back: every word is the first
        str     lr,[r8],#4
        subs    r5,r5,#1
        beq     fill_end
        mov     r6,r2
        mov     r7,r2
        mov     r10,r2
        mov     lr,r2
fill4
        cmp     r5,#4
        blo     fill1
        stmia   r8!,{r2,r6,r7,r10}
        subs    r5,r5,#4
        bne     fill4
        b       fill_end
fill1
        str     r2,[r8],#4
        subs    r5,r5,#1
        bne     fill1
fill_end
        mov     r3,lr
        b       seq
near_bytes
        ldrb    r7,[r6],#1
        strb    r7,[r1],#1
        subs    r5,r5,#1
        bne     near_bytes
        bic     r8,r1,#3
        ldr     r3,[r8]
        b       seq
done
        ldmfd   sp!,{r4-r11,pc}

; Extra length bytes, added to r5 until one is not 255.
extlen
        ldrb    r8,[r0],#1
        add     r5,r5,r8
        cmp     r8,#255
        beq     extlen
        mov     pc,lr

        END
