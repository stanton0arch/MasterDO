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

        IMPORT  z80j_port_out_c

; Context fields, relative to the global pointer.
LINE            EQU     0x460
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
V_FRAMEBASE     EQU     0x68
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400
V_HSN           EQU     0x4400
V_HSLOG         EQU     0x4404
VDP_HS_LOG      EQU     256

VDP_PENDING     EQU     0x100

;----------------------------------------------------------------------------
; OUT to the data port. In: r0 = value. Out: r0 = 0 (stay in the block).
; Codes 0 to 2 write video RAM (and mark the tile dirty), code 3 colour RAM;
; the write also loads the read buffer and ends a pending control word.
;----------------------------------------------------------------------------
sms_vdp_data_w
        ldr     r12,[r10,#MDATA]
        ldr     r2,[r12,#V_CTL]
        and     r0,r0,#0xFF
        ldr     r1,[r12,#V_ADDR]
        str     r0,[r12,#V_BUFFER]
        cmp     r2,#3
        bhs     data_w_slow
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
data_w_slow                             ; code 3, or a control word pending
        bic     r2,r2,#VDP_PENDING
        str     r2,[r12,#V_CTL]
        cmp     r2,#3
        bne     data_w_vram
        and     r2,r1,#31
        add     r2,r2,r12
        and     r0,r0,#0x3F
        strb    r0,[r2,#V_CRAM]
        mov     r0,#1
        str     r0,[r12,#V_CRAMDIRTY]
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

; Horizontal scroll register (the one written on every line by raster
; effects): recorded with the first line it affects, as vdp_reg_write
; does, without leaving the assembly. A write that ends the previous
; frame (negative time) takes the C path.
ctrl_reg8
        ldr     r1,[r10,#LINE]
        ldr     r0,[r12,#V_FRAMEBASE]
        sub     r1,r1,r0
        mov     r0,#228
        mul     r1,r0,r1                ; T-states of the frame at the stretch start
        subs    r1,r1,r2,asr#8          ; T-state of the access
        bmi     ctrl_reg8_slow
        mov     r1,r1,lsr#2             ; line = (t / 4) * 36793 >> 21
        mov     r0,#0x8F00
        orr     r0,r0,#0xB9
        mul     r1,r0,r1
        mov     r1,r1,lsr#21
        add     r1,r1,#1                ; shows from the next line on
        cmp     r1,#192
        bhs     ctrl_reg8_store
        add     r2,r12,#V_HSN           ; the log lies past the video RAM
        ldr     r0,[r2]
        cmp     r0,#VDP_HS_LOG
        bhs     ctrl_reg8_store
        add     r0,r0,#1
        str     r0,[r2]
        add     r2,r2,r0,lsl#2          ; entry r0 - 1 of hs_log (hs_n is just below it)
        ldr     r0,[r12,#V_LATCH]
        orr     r1,r0,r1,lsl#8
        str     r1,[r2]
ctrl_reg8_store
        ldr     r2,[r12,#V_LATCH]
        strb    r2,[r12,#V_REG+8]
        orr     r2,r2,#0x800            ; address = (8 << 8) | latch, code 2
        str     r2,[r12,#V_ADDR]
        mov     r1,#2
        str     r1,[r12,#V_CTL]
        b       ctrl_count
ctrl_reg8_slow
        mov     r0,#0x88
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
        ldr     r2,[r12,#V_CTL]
        cmp     r2,#3
        bhs     wn_slow_entry
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
        mov     r4,r0
        mov     r5,r12
wn_slow_loop
        mvn     r2,r3,lsr#24
        ldr     r2,[r10,r2,lsl#2]
        ldrb    r0,[r2,r3,lsr#16]
        add     r3,r3,r5
        bl      sms_vdp_data_w
        subs    r4,r4,#1
        bne     wn_slow_loop
        mov     r0,#0
        ldmfd   sp!,{r3-r9,pc}

        END
