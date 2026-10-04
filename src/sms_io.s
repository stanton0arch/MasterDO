; sms_io.s - port handlers of the Master System in assembly, for the hot
; paths: VDP data port writes and reads, VDP control port writes, and runs
; of OUTI / OUTD to the data port (the usual way to upload tiles).
;
; The handlers follow the port handler conventions of z80jit.h: called by
; the generated code with r10 = global pointer, arguments in r0-r2, r7 =
; HL for runs; r3-r11 are preserved, r0-r2, r12 and lr may be lost. The
; context's mdata field points to the vdp_state.
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

        IMPORT  z80j_port_out_c
        IMPORT  z80j_port_in_c

; Context fields, relative to the global pointer.
IRQ_LINE        EQU     0x454
LINE            EQU     0x460
EVENT_LINE      EQU     0x464
MDATA           EQU     0x4A8

; vdp_state fields.
V_ADDR          EQU     0x00
V_CTL           EQU     0x04
V_LATCH         EQU     0x08
V_BUFFER        EQU     0x0C
V_CRAMDIRTY     EQU     0x10
V_NDATAW        EQU     0x14
V_NDATAR        EQU     0x18
V_NCTRLW        EQU     0x1C
V_CRAM          EQU     0x20
V_REG           EQU     0x40
V_STATUS        EQU     0x50
V_LINEFLAG      EQU     0x54
V_LINES         EQU     0x5C
V_FRAMEBASE     EQU     0x68
V_VBLDONE       EQU     0x6C
V_LCK           EQU     0x70
V_LCVAL         EQU     0x74
V_ACTIVE        EQU     0x160
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400
V_HSN           EQU     0x4400          ; the logs lie past the video RAM
V_NTN           EQU     0x4804
V_NSTATR        EQU     0x4870
V_CRN           EQU     0x4F84          ; the colour RAM log follows its count
VDP_HS_LOG      EQU     256
VDP_NT_LOG      EQU     16
VDP_CR_LOG      EQU     64

VDP_PENDING     EQU     0x100

;----------------------------------------------------------------------------
; OUT to the data port. In: r0 = value, r2 = T-states left. Out: r0 = 0
; (stay in the block). Codes 0 to 2 write video RAM (and mark the tile
; dirty), code 3 colour RAM; the write also loads the read buffer and ends
; a pending control word. A colour written during the active display is
; recorded with the line it shows from (the next one), for the picture.
;----------------------------------------------------------------------------
sms_vdp_data_w
        ldr     r12,[r10,#MDATA]
        ldr     r1,[r12,#V_CTL]
        and     r0,r0,#0xFF
        str     r0,[r12,#V_BUFFER]
        cmp     r1,#3
        bhs     data_w_slow
        ldr     r1,[r12,#V_ADDR]
data_w_vram
        add     r2,r12,#V_VRAM
        strb    r0,[r2,r1]
        add     r2,r12,#V_DIRTY
        mov     r0,#1
        strb    r0,[r2,r1,lsr#5]
data_w_next
        add     r1,r1,#1
        bic     r1,r1,#0x4000
        str     r1,[r12,#V_ADDR]
        ldr     r2,[r12,#V_NDATAW]
        add     r2,r2,#1
        str     r2,[r12,#V_NDATAW]
        mov     r0,#0
        mov     pc,lr
data_w_slow                             ; r1 = code 3, or a control word pending
        bic     r1,r1,#VDP_PENDING
        str     r1,[r12,#V_CTL]
        cmp     r1,#3
        ldr     r1,[r12,#V_ADDR]
        bne     data_w_vram
        and     r0,r0,#0x3F
        and     r1,r1,#31
        add     r1,r1,r12
        strb    r0,[r1,#V_CRAM]         ; colour RAM
        mov     r1,#1
        str     r1,[r12,#V_CRAMDIRTY]
        ldr     r1,[r12,#V_VBLDONE]     ; past the active display: not recorded
        teq     r1,#0
        ldr     r1,[r12,#V_ADDR]
        bne     data_w_next
        stmfd   sp!,{r1,r3,r4}
        and     r3,r1,#31               ; colour index
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
        ldr     r12,[r10,#MDATA]
        ldr     r1,[r12,#V_ADDR]
        ldr     r0,[r12,#V_BUFFER]
        add     r2,r12,#V_VRAM
        ldrb    r2,[r2,r1]
        str     r2,[r12,#V_BUFFER]
        add     r1,r1,#1
        bic     r1,r1,#0x4000
        str     r1,[r12,#V_ADDR]
        ldr     r2,[r12,#V_CTL]
        bic     r2,r2,#VDP_PENDING
        str     r2,[r12,#V_CTL]
        ldr     r2,[r12,#V_NDATAR]
        add     r2,r2,#1
        str     r2,[r12,#V_NDATAR]
        mov     pc,lr

;----------------------------------------------------------------------------
; IN from the status port. In: r1 = port, r2 = T-states left. Out: r0 =
; status. Reading clears the flags and the interrupt line. With line
; interrupts enabled every underflow of the line counter is an event, so
; no line interrupt flag can be waiting in the lazy counter and the read
; needs no counter update; with them disabled the C path brings the
; counter up to date first (a flag may be waiting there).
;----------------------------------------------------------------------------
sms_vdp_stat_r
        ldr     r12,[r10,#MDATA]
        ldrb    r0,[r12,#V_REG]         ; register 0
        tst     r0,#0x10
        beq     z80j_port_in_c          ; line interrupts off (r1, r2 intact)
        ldr     r0,[r12,#V_STATUS]
        mov     r2,#0
        str     r2,[r12,#V_STATUS]
        str     r2,[r12,#V_LINEFLAG]
        str     r2,[r10,#IRQ_LINE]
        ldr     r2,[r12,#V_CTL]
        bic     r2,r2,#VDP_PENDING
        str     r2,[r12,#V_CTL]
        add     r2,r12,#0x4800
        ldr     r1,[r2,#V_NSTATR-0x4800]
        add     r1,r1,#1
        str     r1,[r2,#V_NSTATR-0x4800]
        mov     pc,lr

;----------------------------------------------------------------------------
; OUT to the control port. In: r0 = value, r1 = port, r2 = T-states left.
; The first byte of a control word sets bits 0-7 of the address; the second
; one sets the code and bits 8-13, and a read code loads the read buffer.
; Register writes go to the machine's C handler (vdp_control_write), which
; may raise the interrupt line or move the next event.
;----------------------------------------------------------------------------
sms_vdp_ctrl_w
        ldr     r12,[r10,#MDATA]
        and     r0,r0,#0xFF
        ldr     r1,[r12,#V_CTL]
        tst     r1,#VDP_PENDING
        bne     ctrl_second
        str     r0,[r12,#V_LATCH]
        orr     r1,r1,#VDP_PENDING
        str     r1,[r12,#V_CTL]
        ldr     r1,[r12,#V_ADDR]
        bic     r1,r1,#0xFF
        orr     r1,r1,r0
        str     r1,[r12,#V_ADDR]
        b       ctrl_count
ctrl_second
        mov     r1,r0,lsr#6             ; code
        cmp     r1,#2
        bne     ctrl_data
        and     r1,r0,#0x0F
        cmp     r1,#8
        beq     ctrl_reg8
        cmp     r1,#10
        beq     ctrl_reg10
        cmp     r1,#2
        beq     ctrl_reg2
        mov     r1,#0xBF
        b       z80j_port_out_c         ; register write (r0, r2 unchanged)
ctrl_data
        str     r1,[r12,#V_CTL]         ; pending cleared
        ldr     r2,[r12,#V_LATCH]
        and     r0,r0,#0x3F
        orr     r2,r2,r0,lsl#8
        teq     r1,#0
        bne     ctrl_addr
        add     r0,r12,#V_VRAM          ; read code: read ahead
        ldrb    r0,[r0,r2]
        str     r0,[r12,#V_BUFFER]
        add     r2,r2,#1
        bic     r2,r2,#0x4000
ctrl_addr
        str     r2,[r12,#V_ADDR]
ctrl_count
        ldr     r1,[r12,#V_NCTRLW]
        add     r1,r1,#1
        str     r1,[r12,#V_NCTRLW]
        mov     r0,#0
        mov     pc,lr

; Registers written by raster effects on every line, handled here as
; vdp_reg_write does: the horizontal scroll (8) and the name table base
; (2) are recorded with the first line they affect, the line counter
; reload (10) brings the lazy counter up to date first. A write that ends
; the previous frame (negative time), or a counter update that crosses
; an underflow, takes the C path.
; r1 = register (2, 8 or 10), r2 = T-states left; r0 is free.
ctrl_reg8
ctrl_reg2
ctrl_reg10
        str     r1,[sp,#-4]!
        ldr     r1,[r10,#LINE]
        ldr     r0,[r12,#V_FRAMEBASE]
        sub     r1,r1,r0
        mov     r0,#228
        mul     r1,r0,r1                ; T-states of the frame at the stretch end
        subs    r1,r1,r2,asr#8          ; T-state of the access
        bmi     ctrl_reg_slow
        mov     r1,r1,lsr#2             ; line = (t / 4) * 36793 >> 21
        mov     r0,#0x8F00
        orr     r0,r0,#0xB9
        mul     r1,r0,r1
        mov     r1,r1,lsr#21            ; lines done (the access is on this line)
        ldr     r0,[sp]
        cmp     r0,#10
        beq     ctrl_lc
        add     r1,r1,#1                ; shows from the next line on
        ldr     r2,[r12,#V_ACTIVE]
        cmp     r1,r2
        bhs     ctrl_reg_store
        cmp     r0,#8
        bne     ctrl_log2
        add     r2,r12,#V_HSN
        ldr     r0,[r2]
        cmp     r0,#VDP_HS_LOG
        bhs     ctrl_reg_restore
        b       ctrl_log
ctrl_log2
        add     r2,r12,#0x4800
        add     r2,r2,#V_NTN-0x4800
        ldr     r0,[r2]
        cmp     r0,#VDP_NT_LOG
        bhs     ctrl_reg_restore
ctrl_log
        add     r0,r0,#1
        str     r0,[r2]
        add     r2,r2,r0,lsl#2          ; entry r0 - 1 of the log (its count is just below it)
        ldr     r0,[r12,#V_LATCH]
        orr     r1,r0,r1,lsl#8
        str     r1,[r2]
ctrl_reg_restore
        ldr     r0,[sp]
ctrl_reg_store
        ; r0 = register: reg[r0] = latch, address = (r0 << 8) | latch, code 2
        ldr     r2,[r12,#V_LATCH]
        add     r1,r12,#V_REG
        strb    r2,[r1,r0]
        orr     r2,r2,r0,lsl#8
        str     r2,[r12,#V_ADDR]
        mov     r1,#2
        str     r1,[r12,#V_CTL]
        add     sp,sp,#4
        b       ctrl_count
; Line counter: lc_sync(k) with k = lines done (r1), in its simple case.
; lc_k in r0 afterwards; the register is free again once the fallback is
; ruled out.
ctrl_lc
        ldr     r0,[r12,#V_LCK]
        cmp     r1,r0
        bls     ctrl_lc_store           ; k <= lc_k: nothing to do
        ldr     r2,[r12,#V_ACTIVE]      ; a = active lines
        cmp     r0,r2
        bhi     ctrl_lc_tail            ; lc_k > a: no countdown
        cmp     r1,r2
        addhi   r2,r2,#1
        movls   r2,r1                   ; end = min(k, a + 1)
        sub     r2,r2,r0                ; n = end - lc_k
        ldr     r0,[r12,#V_LCVAL]
        cmp     r2,r0
        bhi     ctrl_reg_slow           ; underflow since the last update: C
        sub     r0,r0,r2
        str     r0,[r12,#V_LCVAL]
ctrl_lc_tail
        ldr     r2,[r12,#V_ACTIVE]
        add     r2,r2,#1
        cmp     r1,r2
        ldrhib  r0,[r12,#V_REG+10]      ; past line a + 1 the counter reloads
        strhi   r0,[r12,#V_LCVAL]
        str     r1,[r12,#V_LCK]
ctrl_lc_store
        mov     r0,#10
        b       ctrl_reg_store
ctrl_reg_slow
        ldr     r0,[sp],#4
        orr     r0,r0,#0x80             ; the second byte: 0x80 | register
        mov     r1,#0xBF
        b       z80j_port_out_c

;----------------------------------------------------------------------------
; Run of OUTI / OUTD to the data port. In: r0 = count (negative for OUTD),
; r7 = HL (bits 16-31), the source of the first byte. Video RAM runs are
; copied in one loop, by words when the alignment allows it; colour RAM
; or a pending control word goes byte by byte through sms_vdp_data_w.
;----------------------------------------------------------------------------
sms_vdp_data_wn
        ldr     r12,[r10,#MDATA]
        ldr     r1,[r12,#V_CTL]
        cmp     r1,#3
        bhs     wn_slow_entry           ; r2 = T-states left at the end
        cmp     r0,#1
        blo     wn_general              ; OUTD
        cmp     r0,#4
        bhi     wn_general
        ; Short forward run (1-4 bytes, the usual name table update):
        ; nothing is saved but the return address.
        ldr     r1,[r12,#V_ADDR]
        mov     r2,r7,lsr#16
        and     r2,r2,#0xFF
        cmp     r2,#0xFC
        bhi     wn_general              ; the source may cross a page
        add     r2,r1,r0
        cmp     r2,#0x4000
        bhi     wn_general              ; the destination may wrap
        str     lr,[sp,#-4]!
        str     r2,[r12,#V_ADDR]
        sub     r2,r2,#1
        add     lr,r12,#V_DIRTY
        strb    r0,[lr,r1,lsr#5]        ; first and last tile written
        strb    r0,[lr,r2,lsr#5]
        ldr     r2,[r12,#V_NDATAW]
        add     r2,r2,r0
        str     r2,[r12,#V_NDATAW]
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
        ldr     r6,[r10,#MDATA]
        mov     r3,r7                   ; source address (bits 16-31)
        b       wn_slow
wn_general
        stmfd   sp!,{r3-r9,lr}
        mov     r12,#0x10000            ; source step
        movs    r0,r0
        rsbmi   r0,r0,#0
        rsbmi   r12,r12,#0
        ldr     r6,[r10,#MDATA]
        mov     r3,r7                   ; source address (bits 16-31)
        ldr     r1,[r6,#V_NDATAW]
        add     r1,r1,r0
        str     r1,[r6,#V_NDATAW]
        ldr     r1,[r6,#V_ADDR]
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
        str     r4,[r6,#V_ADDR]         ; address after the run
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
        str     r1,[r6,#V_ADDR]
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


;----------------------------------------------------------------------------
; Machine event at the end of a stretch (ctx->event_a): vdp_event in
; assembly. In: r10 = global pointer; r0-r2 and r12 free. Out: r0 = 0 when
; handled, 1 when the C callback must run instead (the line counter
; crossed an underflow with lines left over, which needs a division):
; nothing is changed in that case.
;----------------------------------------------------------------------------
sms_vdp_event_a
        ldr     r12,[r10,#MDATA]
        stmfd   sp!,{r3,r4,r5}
        ldr     r3,[r12,#V_ACTIVE]      ; r3 = a, active lines
        ldr     r0,[r10,#LINE]
        ldr     r1,[r12,#V_FRAMEBASE]
        sub     r0,r0,r1                ; k = line of the frame
        ; lc_sync(k)
        ldr     r1,[r12,#V_LCK]
        cmp     r0,r1
        bls     ev_synced               ; k <= lc_k
        cmp     r1,r3
        bhi     ev_tail                 ; lc_k > a: no countdown
        cmp     r0,r3
        addhi   r2,r3,#1
        movls   r2,r0                   ; end = min(k, a + 1)
        sub     r2,r2,r1                ; n = end - lc_k
        ldr     r4,[r12,#V_LCVAL]
        cmp     r2,r4
        bls     ev_count                ; no underflow
        add     r5,r4,#1
        cmp     r2,r5
        bne     ev_fallback             ; underflow with lines left over
        ldrb    r4,[r12,#V_REG+10]      ; underflow on the last line: reload
        mov     r5,#1
        str     r5,[r12,#V_LINEFLAG]
        b       ev_store
ev_count
        sub     r4,r4,r2
ev_store
        str     r4,[r12,#V_LCVAL]
ev_tail
        add     r2,r3,#1
        cmp     r0,r2
        ldrhib  r4,[r12,#V_REG+10]      ; past line a + 1 the counter reloads
        strhi   r4,[r12,#V_LCVAL]
        str     r0,[r12,#V_LCK]
ev_synced
        ; VBlank flag at the end of the line after the active display
        cmp     r0,r3
        bls     ev_irq
        ldr     r1,[r12,#V_VBLDONE]
        teq     r1,#0
        bne     ev_irq
        mov     r1,#1
        str     r1,[r12,#V_VBLDONE]
        ldr     r1,[r12,#V_STATUS]
        orr     r1,r1,#0x80
        str     r1,[r12,#V_STATUS]
ev_irq
        ; interrupt line = (VBlank flag and R1 bit 5) or (line flag and R0 bit 4)
        mov     r4,#0
        ldr     r1,[r12,#V_STATUS]
        tst     r1,#0x80
        ldrneb  r2,[r12,#V_REG+1]
        tstne   r2,#0x20
        movne   r4,#1
        ldr     r1,[r12,#V_LINEFLAG]
        teq     r1,#0
        ldrneb  r2,[r12,#V_REG]
        tstne   r2,#0x10
        movne   r4,#1
        str     r4,[r10,#IRQ_LINE]
        ; next event: VBlank, or the next counter underflow while line
        ; interrupts are enabled, else the end of the frame
        ldr     r1,[r12,#V_LINES]
        ldr     r2,[r12,#V_VBLDONE]
        teq     r2,#0
        addeq   r1,r3,#1
        ldrb    r2,[r12,#V_REG]
        tst     r2,#0x10
        beq     ev_sched
        ldr     r2,[r12,#V_LCK]
        cmp     r2,r3
        bhi     ev_sched
        ldr     r4,[r12,#V_LCVAL]
        add     r2,r2,r4
        add     r2,r2,#1                ; underflow line
        add     r4,r3,#1
        cmp     r2,r4
        bhi     ev_sched
        cmp     r2,r1
        movlo   r1,r2
ev_sched
        ldr     r2,[r12,#V_FRAMEBASE]
        add     r1,r1,r2
        str     r1,[r10,#EVENT_LINE]
        mov     r0,#0
        ldmfd   sp!,{r3,r4,r5}
        mov     pc,lr
ev_fallback
        mov     r0,#1
        ldmfd   sp!,{r3,r4,r5}
        mov     pc,lr

        END
