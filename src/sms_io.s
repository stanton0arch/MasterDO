; sms_io.s - port handlers of the Master System in assembly, for the hot
; paths: VDP data port writes and reads, VDP control port writes, status
; and counter reads, runs of OUTI / OUTD to the data port (the usual way
; to upload tiles), and PSG writes.
;
; The handlers follow the port handler conventions of z80jit.h: called by
; the generated code with r10 = global pointer, arguments in r0-r2, r7 =
; HL for runs; r3-r11 are preserved, r0-r2, r12 and lr may be lost. The
; vdp_state lies at a fixed offset from the context (SMS_VDP_OFFSET of
; sms.h: the machine is allocated right after it; the global pointer is
; the context's write table, 0x400 into it).
;
; Layouts: CONTRACT with z80j_ctx (z80jit.h) and vdp_state (vdp.h) - keep
; in sync (z80jit.c and vdp.c check the offsets at compile time).

        AREA    |C$$code|, CODE, READONLY

        EXPORT  sms_vdp_data_w
        EXPORT  sms_vdp_data_r
        EXPORT  sms_vdp_ctrl_w
        EXPORT  sms_vdp_data_wn
        EXPORT  sms_vdp_stat_r
        EXPORT  sms_vdp_event_a
        EXPORT  sms_vdp_vc_r
        EXPORT  sms_vdp_hc_r
        EXPORT  sms_psg_w

        IMPORT  z80j_port_out_c
        IMPORT  z80j_port_in_c

; Context fields, relative to the global pointer.
IRQ_LINE        EQU     0x454
LINE            EQU     0x460
EVENT_LINE      EQU     0x464
VDP_OFFSET      EQU     0x44000         ; the vdp_state: SMS_VDP_OFFSET of sms.h from the
                                        ; context, less the read table below the global pointer

; vdp_state fields.
V_ACC           EQU     0x00
V_BUFFER        EQU     0x08
V_CRAMDIRTY     EQU     0x0C
V_NDATAR        EQU     0x10
V_NCTRLW        EQU     0x14
V_NSTATR        EQU     0x18
V_CRAM          EQU     0x20
V_REG           EQU     0x40
V_STATUS        EQU     0x50
V_LINEFLAG      EQU     0x54
V_LINES         EQU     0x5C
V_VCJUMP        EQU     0x60
V_VCBACK        EQU     0x64
V_FRAMEBASE     EQU     0x68
V_VBLDONE       EQU     0x6C
V_LCK           EQU     0x70
V_LCVAL         EQU     0x74
V_HC            EQU     0x7C
V_ACTIVE        EQU     0x58
V_NCOUNTR       EQU     0x168
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400
V_HSN           EQU     0x4400          ; the logs lie past the video RAM
V_NTN           EQU     0x4804
V_CRN           EQU     0x4F84          ; the colour RAM log follows its count
VDP_HS_LOG      EQU     256
VDP_NT_LOG      EQU     16
VDP_CR_LOG      EQU     64

; Machine fields past the vdp_state (sms_machine, sms.h; checked in sms.c).
M_PSGREG        EQU     0x5200          ; psg_reg[8], psg_latch just below
M_PSGW          EQU     0x5228          ; io.psg_w

; The access word (vdp.h): the address in bits 18-31, the code in bits
; 14-15, the pending and slow marks, the data port writes of the frame in
; bits 0-13.
ACC_ADDR1       EQU     0x40000         ; one in the address field
ACC_CODE3       EQU     0xC000
ACC_PENDING     EQU     0x10000
ACC_SLOW        EQU     0x20000

; $r = the vdp_state.
        MACRO
        VDPR    $r
        add     $r,r10,#VDP_OFFSET
        MEND

        MACRO
        VDP
        VDPR    r12
        MEND

; The pending control word of the access word $a ended; the slow mark
; stays for code 3 (colour RAM).
        MACRO
        UNPEND  $a
        bic     $a,$a,#ACC_PENDING+ACC_SLOW
        tst     $a,#0x4000
        tstne   $a,#0x8000
        orrne   $a,$a,#ACC_SLOW
        MEND

; The address field of the access word := $a (0x4000 wraps to 0), the
; rest kept; $t clobbered, $s = the vdp_state.
        MACRO
        SETADDR $a,$t,$s
        ldr     $t,[$s,#V_ACC]
        mov     $t,$t,lsl #14
        mov     $t,$t,lsr #14
        orr     $t,$t,$a,lsl #18
        str     $t,[$s,#V_ACC]
        MEND

;----------------------------------------------------------------------------
; OUT to the data port. In: r0 = value, r2 = T-states left. Out: r0 = 0
; (stay in the block). Codes 0 to 2 write video RAM (and mark the tile
; dirty), code 3 colour RAM; the write also loads the read buffer and ends
; a pending control word. A colour written during the active display is
; recorded with the line it shows from (the next one), for the picture.
; The usual write (video RAM, no control word pending) is the hot path of
; every tile upload: one load of the access word, three stores, the
; address and the count advanced in that word.
;----------------------------------------------------------------------------
sms_vdp_data_w
        VDP
        ldr     r1,[r12,#V_ACC]
        and     r0,r0,#0xFF
        tst     r1,#ACC_SLOW
        bne     data_w_slow
        str     r0,[r12,#V_BUFFER]
data_w_vram
        add     r2,r12,#V_VRAM
        strb    r0,[r2,r1,lsr#18]
        add     r2,r12,#V_DIRTY
        mov     r0,#1
        strb    r0,[r2,r1,lsr#23]
data_w_next
        add     r1,r1,#ACC_ADDR1        ; the next address (wrapping by itself)
        add     r1,r1,#1                ; one more write
        str     r1,[r12,#V_ACC]
        mov     r0,#0
        mov     pc,lr
data_w_slow                             ; code 3, or a control word pending
        str     r0,[r12,#V_BUFFER]
        UNPEND  r1
        beq     data_w_vram             ; a pending control word ended: video RAM
        stmfd   sp!,{r1,r3,r4}
        and     r0,r0,#0x3F
        mov     r3,r1,lsr#18
        and     r3,r3,#31               ; colour index
        add     r4,r3,r12
        strb    r0,[r4,#V_CRAM]         ; colour RAM
        mov     r4,#1
        str     r4,[r12,#V_CRAMDIRTY]
        ldr     r4,[r12,#V_VBLDONE]     ; past the active display: not recorded
        teq     r4,#0
        bne     cram_done
        ldr     r4,[r10,#LINE]
        ldr     r1,[r12,#V_FRAMEBASE]
        sub     r4,r4,r1
        mov     r1,#228
        mul     r4,r1,r4                ; T-states of the frame at the stretch end
        subs    r4,r4,r2,asr#8          ; T-state of the access
        bmi     cram_done               ; it ends the previous frame
        mov     r4,r4,lsr#2             ; line = (t / 4) * 36793 >> 21
        mov     r1,#0x8F00
        orr     r1,r1,#0xB9
        mul     r4,r1,r4
        mov     r4,r4,lsr#21
        add     r4,r4,#1                ; shows from the next line on
        ldr     r1,[r12,#V_ACTIVE]
        cmp     r4,r1
        bhs     cram_done
        add     r2,r12,#0x4F00
        ldr     r1,[r2,#V_CRN-0x4F00]
        cmp     r1,#VDP_CR_LOG
        bhs     cram_done
        add     r1,r1,#1
        str     r1,[r2,#V_CRN-0x4F00]
        add     r2,r2,r1,lsl#2          ; entry r1 - 1 of the log
        orr     r0,r0,r3,lsl#8
        orr     r0,r0,r4,lsl#16
        str     r0,[r2,#V_CRN-0x4F00]
cram_done
        ldmfd   sp!,{r1,r3,r4}
        b       data_w_next

;----------------------------------------------------------------------------
; IN from the data port: returns the read buffer, which is then loaded from
; the next address.
;----------------------------------------------------------------------------
sms_vdp_data_r
        VDP
        ldr     r1,[r12,#V_ACC]
        ldr     r0,[r12,#V_BUFFER]
        add     r2,r12,#V_VRAM
        ldrb    r2,[r2,r1,lsr#18]
        str     r2,[r12,#V_BUFFER]
        UNPEND  r1
        add     r1,r1,#ACC_ADDR1
        str     r1,[r12,#V_ACC]
        ldr     r2,[r12,#V_NDATAR]
        add     r2,r2,#1
        str     r2,[r12,#V_NDATAR]
        mov     pc,lr

;----------------------------------------------------------------------------
; IN from the status port. In: r1 = port, r2 = T-states left. Out: r0 =
; status. Reading clears the flags and the interrupt line. With line
; interrupts enabled every underflow of the line counter is an event, so
; no line interrupt flag can be waiting in the lazy counter and the read
; needs no counter update. With them disabled an underflow since the last
; update may have set the flag, which the read clears: the lazy counter is
; first brought to the lines done (lc_sync of vdp.c) in its simple case,
; and through the C path when it crossed an underflow.
;----------------------------------------------------------------------------
sms_vdp_stat_r
        VDP
        ldrb    r0,[r12,#V_REG]         ; register 0
        tst     r0,#0x10
        beq     stat_lc
stat_clear
        ldr     r0,[r12,#V_STATUS]
        mov     r2,#0
        str     r2,[r12,#V_STATUS]
        str     r2,[r12,#V_LINEFLAG]
        str     r2,[r10,#IRQ_LINE]
        ldr     r1,[r12,#V_NSTATR]
        add     r1,r1,#1
        str     r1,[r12,#V_NSTATR]
        ldr     r2,[r12,#V_ACC]
        tst     r2,#ACC_PENDING
        moveq   pc,lr
        UNPEND  r2                      ; a pending control word ends
        str     r2,[r12,#V_ACC]
        mov     pc,lr
stat_lc
        ldr     r0,[r10,#LINE]
        ldr     r1,[r12,#V_FRAMEBASE]
        sub     r0,r0,r1
        mov     r1,#228
        mul     r0,r1,r0                ; T-states of the frame at the stretch end
        subs    r0,r0,r2,asr#8          ; T-state of the access
        ble     stat_clear              ; no line done yet: k = 0
        mov     r0,r0,lsr#2             ; k = (t / 4) * 36793 >> 21
        mov     r1,#0x8F00
        orr     r1,r1,#0xB9
        mul     r0,r1,r0
        mov     r0,r0,lsr#21
        ldr     r1,[r12,#V_LCK]
        cmp     r0,r1
        bls     stat_clear              ; k <= lc_k: nothing to do
        str     r3,[sp,#-4]!
        ldr     r3,[r12,#V_ACTIVE]      ; a = active lines
        cmp     r1,r3
        bhi     stat_lc_tail            ; lc_k > a: no countdown
        cmp     r0,r3
        addhi   r3,r3,#1
        movls   r3,r0                   ; end = min(k, a + 1)
        sub     r3,r3,r1                ; n = end - lc_k
        ldr     r1,[r12,#V_LCVAL]
        cmp     r3,r1
        bhi     stat_lc_slow            ; underflow since the last update: C
        sub     r1,r1,r3
        str     r1,[r12,#V_LCVAL]
stat_lc_tail
        ldr     r3,[r12,#V_ACTIVE]
        add     r3,r3,#1
        cmp     r0,r3
        ldrhib  r1,[r12,#V_REG+10]      ; past line a + 1 the counter reloads
        strhi   r1,[r12,#V_LCVAL]
        str     r0,[r12,#V_LCK]
        ldr     r3,[sp],#4
        b       stat_clear
stat_lc_slow
        ldr     r3,[sp],#4
        mov     r1,#0xBF
        b       z80j_port_in_c          ; r2 unchanged

;----------------------------------------------------------------------------
; IN from the V counter (even ports $40-$7F) and the H counter (odd ports).
; In: r2 = T-states left. The beam position comes from the access time as
; in vdp_beam (vdp.c): the line of the frame (an access that ends the
; previous frame counts from its start) and the T-state in the line; the V
; counter then jumps back as the display mode says, the H counter comes
; from the table of a line.
;----------------------------------------------------------------------------
        MACRO
        BEAM                            ; out: r2 = line, r1 = T-state in it
        ldr     r0,[r10,#LINE]
        ldr     r1,[r12,#V_FRAMEBASE]
        sub     r0,r0,r1
        mov     r1,#228
        mul     r0,r1,r0                ; T-states of the frame at the stretch end
        subs    r0,r0,r2,asr#8          ; t: T-state of the access
        ldrmi   r1,[r12,#V_LINES]
        movmi   r2,#228
        mlami   r0,r1,r2,r0             ; t + lines * 228
        mov     r2,r0,lsr#2             ; line = (t / 4) * 36793 >> 21
        mov     r1,#0x8F00
        orr     r1,r1,#0xB9
        mul     r2,r1,r2
        mov     r2,r2,lsr#21
        mov     r1,#228
        mul     r1,r2,r1
        sub     r1,r0,r1                ; T-state in the line
        ldr     r0,[r12,#V_LINES]
        cmp     r2,r0
        subhs   r2,r2,r0
        MEND

; The V counter only needs the line: the lines at the stretch end less
; ceil(left / 228), computed as (ceil(left / 4) + 56) / 57 by one
; multiplication (exact within a frame), as the control port does; the
; line count and the jump of the display mode come with one LDM.
sms_vdp_vc_r
        add     r0,r2,#0x300
        mov     r0,r0,lsr#10
        add     r0,r0,#56
        mov     r1,#0x8F00
        orr     r1,r1,#0xB9
        mul     r2,r1,r0                ; lines back << 21
        ldr     r0,[r10,#LINE]
        sub     r0,r0,r2,lsr#21         ; the line of the access
        VDP
        ldr     r1,[r12,#V_FRAMEBASE]
        subs    r0,r0,r1                ; from the start of the frame
        add     r12,r12,#V_LINES
        ldmia   r12,{r1,r2,r12}         ; lines, vc_jump, vc_back
        addmi   r0,r0,r1                ; an access that ends the previous frame
        cmp     r0,r1
        subhs   r0,r0,r1
        cmp     r0,r2
        subhi   r0,r0,r12
        and     r0,r0,#0xFF
        VDP
        ldr     r1,[r12,#V_NCOUNTR]
        add     r1,r1,#1
        str     r1,[r12,#V_NCOUNTR]
        mov     pc,lr

sms_vdp_hc_r
        VDP
        BEAM
        add     r1,r1,r12
        ldrb    r0,[r1,#V_HC]
        ldr     r1,[r12,#V_NCOUNTR]
        add     r1,r1,#1
        str     r1,[r12,#V_NCOUNTR]
        mov     pc,lr

;----------------------------------------------------------------------------
; OUT to the PSG (ports $40-$7F), as psg_write of sms.c: a latch byte (bit
; 7 set) selects a register and gives its low 4 bits; a data byte gives the
; high 6 bits of a tone register, or the whole value of the others (the
; volumes and the noise register take 4 bits).
;----------------------------------------------------------------------------
sms_psg_w
        VDPR    r12
        add     r12,r12,#M_PSGREG
        ldr     r1,[r12,#M_PSGW-M_PSGREG]
        add     r1,r1,#1
        str     r1,[r12,#M_PSGW-M_PSGREG]
        and     r0,r0,#0xFF
        tst     r0,#0x80
        movne   r1,r0,lsr#4
        andne   r1,r1,#7
        strne   r1,[r12,#-4]            ; the latch
        ldreq   r1,[r12,#-4]
        tst     r1,#1
        bne     psg_4bits               ; a volume
        cmp     r1,#6
        beq     psg_4bits               ; the noise register
        ldr     r2,[r12,r1,lsl#2]       ; a tone period: 10 bits
        tst     r0,#0x80
        bicne   r2,r2,#0x0F
        andne   r0,r0,#0x0F
        andeq   r2,r2,#0x0F
        andeq   r0,r0,#0x3F
        moveq   r0,r0,lsl#4
        orr     r0,r0,r2
        str     r0,[r12,r1,lsl#2]
        mov     r0,#0
        mov     pc,lr
psg_4bits
        and     r0,r0,#0x0F
        str     r0,[r12,r1,lsl#2]
        mov     r0,#0
        mov     pc,lr

;----------------------------------------------------------------------------
; OUT to the control port. In: r0 = value, r1 = port, r2 = T-states left.
; The first byte of a control word sets bits 0-7 of the address (where the
; second byte finds it); the second one sets the code and bits 8-13, and a
; read code loads the read buffer. Register writes go to the machine's C
; handler (vdp_control_write), which may raise the interrupt line or move
; the next event, but for the registers raster effects write on every
; line (ctrl_reg8 and the next ones).
;----------------------------------------------------------------------------
sms_vdp_ctrl_w
        VDP
        and     r0,r0,#0xFF
        ldr     r1,[r12,#V_ACC]
        tst     r1,#ACC_PENDING
        bne     ctrl_second
        bic     r1,r1,#0x03FC0000       ; bits 0-7 of the address: the byte
        orr     r1,r1,r0,lsl#18
        orr     r1,r1,#ACC_PENDING+ACC_SLOW
        str     r1,[r12,#V_ACC]
ctrl_count
        ldr     r1,[r12,#V_NCTRLW]
        add     r1,r1,#1
        str     r1,[r12,#V_NCTRLW]
        mov     r0,#0
        mov     pc,lr
ctrl_second                             ; r1 = access word, the first byte in bits 18-25
        cmp     r0,#0x88
        beq     ctrl_reg8
        tst     r0,#0x40
        bne     ctrl_data
        tst     r0,#0x80
        beq     ctrl_data
        cmp     r0,#0x8A
        beq     ctrl_reg10
        cmp     r0,#0x82
        beq     ctrl_reg2
        mov     r1,#0xBF
        b       z80j_port_out_c         ; other register writes (r0, r2 unchanged)
ctrl_data                               ; codes 0, 1 and 3
        bic     r1,r1,#0xFC000000       ; address bits 8-13
        bic     r1,r1,#0x0003C000       ; code, pending and slow marks
        orr     r1,r1,r0,lsl#26         ; address bits 8-13 from the byte
        and     r0,r0,#0xC0
        orr     r1,r1,r0,lsl#8          ; the code
        cmp     r0,#0xC0
        orreq   r1,r1,#ACC_SLOW         ; colour RAM writes take the slow path
        teq     r0,#0
        bne     ctrl_addr
        mov     r0,r1,lsr#18            ; read code: read ahead
        add     r2,r12,#V_VRAM
        ldrb    r0,[r2,r0]
        str     r0,[r12,#V_BUFFER]
        add     r1,r1,#ACC_ADDR1        ; (0x4000 wraps to 0 in the word)
ctrl_addr
        str     r1,[r12,#V_ACC]
        b       ctrl_count

; Registers written by raster effects on every line, handled here as
; vdp_reg_write does: the horizontal scroll (8) and the name table base
; (2) are recorded with the first line they affect, the line counter
; reload (10) brings the lazy counter up to date first. A write that ends
; the previous frame (negative time), or a counter update that crosses
; an underflow, takes the C path with the value and the time unchanged.
; Only the second bytes $82, $88 and $8A come here (address bits 12-13
; clear).

; r1 := the line of the access (lines done before it, N set if negative):
; the lines at the stretch end less ceil(left / 228), computed as
; (ceil(left / 4) + 56) / 57 by a multiplication (exact within a frame).
; r2 (T-states left) is kept, r0 lost.
        MACRO
        ACCLINE
        add     r0,r2,#0x300
        mov     r0,r0,lsr#10
        add     r0,r0,#56
        mov     r1,#0x8F00
        orr     r1,r1,#0xB9
        mul     r0,r1,r0                ; lines back << 21
        ldr     r1,[r10,#LINE]
        sub     r1,r1,r0,lsr#21
        ldr     r0,[r12,#V_FRAMEBASE]
        subs    r1,r1,r0
        MEND

; Register $n := the first byte (left in r2), the address := ($n << 8) |
; that byte, code 2, the data writes of the frame kept (r0 lost).
        MACRO
        REGSTORE $n
        ldr     r0,[r12,#V_ACC]
        mov     r2,r0,lsl#6
        mov     r2,r2,lsr#24            ; the first byte
        strb    r2,[r12,#V_REG+$n]
        mov     r0,r0,lsl#18
        mov     r0,r0,lsr#18
        orr     r0,r0,#0x8000
        orr     r0,r0,r2,lsl#18
        orr     r0,r0,#$n:SHL:26
        str     r0,[r12,#V_ACC]
        MEND

; Appends (r1 << 8) | r2 to the log at $off of the vdp_state ($max
; entries, its count first) when the line r1 is in the active display.
        MACRO
        RASTERLOG $off,$max
        ldr     r0,[r12,#V_ACTIVE]
        cmp     r1,r0
        bhs     ctrl_count
        orr     r1,r2,r1,lsl#8
        add     r2,r12,#$off :AND: 0xFF00
        add     r2,r2,#$off :AND: 0xFF
        ldr     r0,[r2]
        cmp     r0,#$max
        bhs     ctrl_count
        add     r0,r0,#1
        str     r0,[r2],r0,lsl#2        ; to the new entry (the count is just below the log)
        str     r1,[r2]
        b       ctrl_count
        MEND

ctrl_reg8
        ACCLINE
        bmi     ctrl_slow8
        REGSTORE 8
        add     r1,r1,#1                ; shows from the next line on
        RASTERLOG V_HSN,VDP_HS_LOG
ctrl_slow8
        mov     r0,#0x88
        mov     r1,#0xBF
        b       z80j_port_out_c

ctrl_reg2
        ACCLINE
        bmi     ctrl_slow2
        REGSTORE 2
        add     r1,r1,#1
        RASTERLOG V_NTN,VDP_NT_LOG
ctrl_slow2
        mov     r0,#0x82
        mov     r1,#0xBF
        b       z80j_port_out_c

; Line counter: lc_sync(k) of vdp.c with k = the lines done (r1), in its
; simple case, before the register changes.
ctrl_reg10
        ACCLINE
        bmi     ctrl_slow10
        str     r3,[sp,#-4]!
        ldr     r0,[r12,#V_LCK]
        cmp     r1,r0
        bls     ctrl_lc_done            ; k <= lc_k: nothing to do
        ldr     r3,[r12,#V_ACTIVE]      ; a = active lines
        cmp     r0,r3
        bhi     ctrl_lc_tail            ; lc_k > a: no countdown
        cmp     r1,r3
        addhi   r3,r3,#1
        movls   r3,r1                   ; end = min(k, a + 1)
        sub     r3,r3,r0                ; n = end - lc_k
        ldr     r0,[r12,#V_LCVAL]
        cmp     r3,r0
        bhi     ctrl_lc_slow            ; underflow since the last update: C
        sub     r0,r0,r3
        str     r0,[r12,#V_LCVAL]
ctrl_lc_tail
        ldr     r3,[r12,#V_ACTIVE]
        add     r3,r3,#1
        cmp     r1,r3
        ldrhib  r0,[r12,#V_REG+10]      ; past line a + 1 the counter reloads
        strhi   r0,[r12,#V_LCVAL]
        str     r1,[r12,#V_LCK]
ctrl_lc_done
        ldr     r3,[sp],#4
        REGSTORE 10
        b       ctrl_count
ctrl_lc_slow
        ldr     r3,[sp],#4
ctrl_slow10
        mov     r0,#0x8A
        mov     r1,#0xBF
        b       z80j_port_out_c

;----------------------------------------------------------------------------
; Run of OUTI / OUTD to the data port. In: r0 = count (negative for OUTD),
; r7 = HL (bits 16-31), the source of the first byte. Video RAM runs are
; copied in one loop, by words when the alignment allows it; colour RAM
; or a pending control word goes byte by byte through sms_vdp_data_w.
; Mixed runs (OUTI and OUT (C),r) are handled at data_wm.
;----------------------------------------------------------------------------
sms_vdp_data_wn
        cmp     r0,#0x40000000          ; OUTD runs are negative
        bge     data_wm                 ; mixed run (OUTI and OUT (C),r)
        VDP
        ldr     r1,[r12,#V_ACC]
        tst     r1,#ACC_SLOW
        bne     wn_slow_entry           ; r2 = T-states left at the end
        cmp     r0,#1
        blo     wn_general              ; OUTD
        cmp     r0,#4
        bhi     wn_general
        ; Short forward run (1-4 bytes, the usual name table update):
        ; nothing is saved but the return address.
        mov     r2,r7,lsr#16
        and     r2,r2,#0xFF
        cmp     r2,#0xFC
        bhi     wn_general              ; the source may cross a page
        add     r2,r0,r1,lsr#18         ; destination end
        cmp     r2,#0x4000
        bhi     wn_general              ; the destination may wrap
        str     lr,[sp,#-4]!
        add     lr,r1,r0                ; the bytes counted
        mov     lr,lr,lsl#14
        mov     lr,lr,lsr#14
        orr     lr,lr,r2,lsl#18         ; the address after the run (0x4000 wraps to 0)
        str     lr,[r12,#V_ACC]
        mov     r1,r1,lsr#18            ; the address
        sub     r2,r2,#1
        add     lr,r12,#V_DIRTY
        strb    r0,[lr,r1,lsr#5]        ; first and last tile written
        strb    r0,[lr,r2,lsr#5]
        mvn     lr,r7,lsr#24
        ldr     lr,[r10,lr,lsl#2]
        add     lr,lr,r7,lsr#16         ; host source
        add     r1,r1,r12
        add     r1,r1,#V_VRAM           ; host destination
wn_short_loop
        ldrb    r2,[lr],#1
        strb    r2,[r1],#1
        subs    r0,r0,#1
        bne     wn_short_loop
        str     r2,[r12,#V_BUFFER]      ; the last byte written: read buffer
        ldr     pc,[sp],#4
wn_slow_entry
        stmfd   sp!,{r3-r9,lr}
        mov     r12,#0x10000            ; source step
        movs    r0,r0
        rsbmi   r0,r0,#0
        rsbmi   r12,r12,#0
        VDPR    r6
        mov     r3,r7                   ; source address (bits 16-31)
        b       wn_slow
wn_general
        stmfd   sp!,{r3-r9,lr}
        mov     r12,#0x10000            ; source step
        movs    r0,r0
        rsbmi   r0,r0,#0
        rsbmi   r12,r12,#0
        VDPR    r6
        mov     r3,r7                   ; source address (bits 16-31)
        ldr     r1,[r6,#V_ACC]
        add     r1,r1,r0                ; the bytes counted (the address is stored again below)
        str     r1,[r6,#V_ACC]
        mov     r1,r1,lsr#18            ; the address
        add     r5,r6,#V_VRAM
        add     r8,r6,#V_DIRTY
        mov     r9,#1
        ; Forward run whose source stays in one page and whose destination
        ; does not wrap: the source page entry and the dirty marks are
        ; taken once, and the bytes are copied by words when both ends are
        ; word aligned. The other runs go byte by byte below.
        teq     r12,#0x10000
        bne     wn_loop
        mov     r4,r3,lsr#16
        and     r4,r4,#0xFF
        add     r4,r4,r0
        cmp     r4,#0x100
        bhi     wn_loop
        add     r4,r1,r0                ; destination end
        cmp     r4,#0x4000
        bhi     wn_loop
        SETADDR r4,r2,r6                ; address after the run
        mov     r2,r1,lsr#5             ; tiles written: first to last
        sub     r4,r4,#1
        mov     r4,r4,lsr#5
wn_dirty
        strb    r9,[r8,r2]
        add     r2,r2,#1
        cmp     r2,r4
        bls     wn_dirty
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        add     r2,r2,r3,lsr#16         ; host source
        add     r4,r5,r1                ; host destination
        orr     r1,r1,r3,lsr#16
        tst     r1,#3
        bne     wn_bytes
        cmp     r0,#8
        blo     wn_bytes
wn_w32
        subs    r0,r0,#32
        ldmhsia r2!,{r1,r3,r5,r7,r8,r9,r12,lr}
        stmhsia r4!,{r1,r3,r5,r7,r8,r9,r12,lr}
        bhs     wn_w32
        adds    r0,r0,#32               ; bytes left, 0-31
        beq     wn_fast_done
wn_w4
        subs    r0,r0,#4
        ldrhs   r1,[r2],#4
        strhs   r1,[r4],#4
        bhs     wn_w4
        adds    r0,r0,#4                ; 0-3
        beq     wn_fast_done
wn_bytes
        tst     r0,#1
        ldrneb  r1,[r2],#1
        strneb  r1,[r4],#1
        bics    r0,r0,#1
        beq     wn_fast_done
wn_b2
        ldrb    r1,[r2],#1
        ldrb    r3,[r2],#1
        strb    r1,[r4],#1
        strb    r3,[r4],#1
        subs    r0,r0,#2
        bne     wn_b2
wn_fast_done
        ldrb    r1,[r4,#-1]             ; the last byte written: read buffer
        str     r1,[r6,#V_BUFFER]
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}
wn_loop
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r4,[r2,r3,lsr#16]
        add     r3,r3,r12
        strb    r4,[r5,r1]
        strb    r9,[r8,r1,lsr#5]
        add     r1,r1,#1
        bic     r1,r1,#0x4000
        subs    r0,r0,#1
        bne     wn_loop
        SETADDR r1,r2,r6
        str     r4,[r6,#V_BUFFER]
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}
wn_slow
        ; Byte by byte, each with its own time (16 T-states per OUTI).
        mov     r4,r0
        mov     r5,r12
        sub     r8,r0,#1
        add     r8,r2,r8,lsl#12         ; T-states left at the first byte
wn_slow_loop
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r0,[r2,r3,lsr#16]
        add     r3,r3,r5
        mov     r2,r8
        bl      sms_vdp_data_w
        sub     r8,r8,#0x1000
        subs    r4,r4,#1
        bne     wn_slow_loop
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}

; Mixed run: r0 = Z80J_RUN_MIXED | mask << 13 | count << 8 | byte (see
; z80jit.h), r2 = T-states left at the end of the first element. Element k
; is the byte when bit k of the mask is set, else the next byte from HL.
; A video RAM run whose source stays in one page and whose destination
; does not wrap takes the page entry and the dirty marks once.
data_wm
        stmfd   sp!,{r3-r9,lr}
        VDPR    r6
        mov     r4,r0,lsr#8
        and     r4,r4,#0x1F             ; elements
        mov     r5,r0,lsr#13
        bic     r5,r5,#0x20000          ; mask, bit 0 = the next element
        and     r9,r0,#0xFF             ; byte of OUT (C),r
        ldr     r1,[r6,#V_ACC]
        tst     r1,#ACC_SLOW
        bne     wm_slow                 ; colour RAM, or a control word pending
        add     r1,r1,r4                ; the elements counted
        str     r1,[r6,#V_ACC]
        mov     r1,r1,lsr#18            ; the address
        add     r8,r1,r4                ; destination end
        cmp     r8,#0x4000
        bhi     wm_bytes
        mov     r2,r7,lsr#16
        and     r2,r2,#0xFF
        add     r2,r2,r4                ; source end, at worst
        cmp     r2,#0x100
        bhi     wm_bytes
        SETADDR r8,r2,r6
        mov     r2,r1,lsr#5             ; tiles written: first to last
        sub     r8,r8,#1
        mov     r8,r8,lsr#5
        add     r12,r6,#V_DIRTY
        mov     r0,#1
wm_dirty
        strb    r0,[r12,r2]
        add     r2,r2,#1
        cmp     r2,r8
        bls     wm_dirty
        mvn     r2,r7,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        add     r2,r2,r7,lsr#16         ; host source
        add     r12,r6,#V_VRAM
        add     r12,r12,r1              ; host destination
wm_fast
        movs    r5,r5,lsr#1             ; C: the byte of OUT (C),r
        ldrccb  r0,[r2],#1
        movcs   r0,r9
        strb    r0,[r12],#1
        subs    r4,r4,#1
        bne     wm_fast
        str     r0,[r6,#V_BUFFER]       ; the last byte written: read buffer
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}
wm_bytes                                ; byte by byte: r1 = address
        mov     r3,r7
        add     r8,r6,#V_DIRTY
        add     r12,r6,#V_VRAM
        mov     lr,#1
wm_byte
        movs    r5,r5,lsr#1
        movcs   r0,r9
        bcs     wm_put
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r0,[r2,r3,lsr#16]
        add     r3,r3,#0x10000
wm_put
        strb    r0,[r12,r1]
        strb    lr,[r8,r1,lsr#5]
        add     r1,r1,#1
        bic     r1,r1,#0x4000
        subs    r4,r4,#1
        bne     wm_byte
        str     r0,[r6,#V_BUFFER]
        SETADDR r1,r0,r6
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}
wm_slow                                 ; each byte through sms_vdp_data_w, with its time
        mov     r3,r7
        mov     r8,r2
wm_slow_loop
        movs    r5,r5,lsr#1
        movcs   r0,r9
        bcs     wm_slow_put
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r0,[r2,r3,lsr#16]
        add     r3,r3,#0x10000
wm_slow_put
        mov     r2,r8
        bl      sms_vdp_data_w
        tst     r5,#1                   ; time of the next element
        subne   r8,r8,#0xC00
        subeq   r8,r8,#0x1000
        subs    r4,r4,#1
        bne     wm_slow_loop
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}


;----------------------------------------------------------------------------
; Machine event at the end of a stretch (ctx->event_a): vdp_event in
; assembly. In: r0 = ctx->line, r10 = global pointer. Preserves r1-r11.
; Out: r0 = 0 when handled, with r12 = the new ctx->event_line; r0 = 1 when
; the C callback must run instead (the line counter crossed an underflow
; with lines left over, which needs a division): nothing is changed in
; that case. The timing fields are loaded together (frame base, VBlank
; done, line counter), registers 0-3 as one word.
;----------------------------------------------------------------------------
sms_vdp_event_a
        VDP
        stmfd   sp!,{r1-r7}
        add     r1,r12,#V_FRAMEBASE
        ldmia   r1,{r1-r4}              ; frame base, VBlank done, lc_k, lc_val
        ldr     r5,[r12,#V_ACTIVE]      ; a, active lines
        sub     r0,r0,r1                ; k = line of the frame
        ; lc_sync(k)
        cmp     r0,r3
        bls     ev_synced               ; k <= lc_k
        cmp     r3,r5
        bhi     ev_tail                 ; lc_k > a: no countdown
        cmp     r0,r5
        addhi   r6,r5,#1
        movls   r6,r0                   ; end = min(k, a + 1)
        sub     r6,r6,r3                ; n = end - lc_k
        subs    r4,r4,r6
        bcs     ev_tail                 ; no underflow
        cmn     r4,#1
        bne     ev_fallback             ; underflow with lines left over
        ldrb    r4,[r12,#V_REG+10]      ; underflow on the last line: reload
        mov     r6,#1
        str     r6,[r12,#V_LINEFLAG]
ev_tail
        add     r6,r5,#1
        cmp     r0,r6
        ldrhib  r4,[r12,#V_REG+10]      ; past line a + 1 the counter reloads
        mov     r3,r0                   ; lc_k = k
        add     r6,r12,#V_LCK
        stmia   r6,{r3,r4}
ev_synced
        ; VBlank flag at the end of the line after the active display
        cmp     r0,r5
        bls     ev_irq
        teq     r2,#0
        bne     ev_irq
        mov     r2,#1
        str     r2,[r12,#V_VBLDONE]
        ldr     r6,[r12,#V_STATUS]
        orr     r6,r6,#0x80
        str     r6,[r12,#V_STATUS]
ev_irq
        ; interrupt line = (VBlank flag and R1 bit 5) or (line flag and R0
        ; bit 4); registers 0-3 in one big-endian word: R0 in bits 24-31,
        ; R1 in bits 16-23
        ldr     r0,[r12,#V_REG]
        add     r6,r12,#V_STATUS
        ldmia   r6,{r6,r7}              ; status, line flag
        ands    r6,r0,r6,lsl#14         ; status bit 7 against R1 bit 5
        andeqs  r6,r0,r7,lsl#28         ; line flag against R0 bit 4
        movne   r6,#1
        str     r6,[r10,#IRQ_LINE]
        ; next event: VBlank, or the next counter underflow while line
        ; interrupts are enabled, else the end of the frame
        ldr     r6,[r12,#V_LINES]
        teq     r2,#0
        addeq   r6,r5,#1
        tst     r0,#0x10000000
        beq     ev_sched
        cmp     r3,r5
        bhi     ev_sched
        add     r7,r3,r4
        add     r7,r7,#1                ; underflow line
        add     r2,r5,#1
        cmp     r7,r2
        bhi     ev_sched
        cmp     r7,r6
        movlo   r6,r7
ev_sched
        add     r12,r6,r1
        str     r12,[r10,#EVENT_LINE]
        mov     r0,#0
        ldmfd   sp!,{r1-r7}
        mov     pc,lr
ev_fallback
        mov     r0,#1
        ldmfd   sp!,{r1-r7}
        mov     pc,lr

        END
