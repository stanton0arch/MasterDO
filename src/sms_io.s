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

; Context field, relative to the global pointer.
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
V_DIRTY         EQU     0x200
V_VRAM          EQU     0x400

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
        moveq   r1,#0xBF
        beq     z80j_port_out_c         ; register write (r0, r2 unchanged)
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

;----------------------------------------------------------------------------
; Run of OUTI / OUTD to the data port. In: r0 = count (negative for OUTD),
; r7 = HL (bits 16-31), the source of the first byte. Video RAM runs are
; copied in one loop; colour RAM or a pending control word goes byte by
; byte through sms_vdp_data_w.
;----------------------------------------------------------------------------
sms_vdp_data_wn
        stmfd   sp!,{r3-r9,lr}
        mov     r12,#0x10000            ; source step
        movs    r0,r0
        rsbmi   r0,r0,#0
        rsbmi   r12,r12,#0
        ldr     r6,[r10,#MDATA]
        ldr     r2,[r6,#V_CTL]
        mov     r3,r7                   ; source address (bits 16-31)
        cmp     r2,#3
        bhs     wn_slow
        ldr     r1,[r6,#V_NDATAW]
        add     r1,r1,r0
        str     r1,[r6,#V_NDATAW]
        ldr     r1,[r6,#V_ADDR]
        add     r5,r6,#V_VRAM
        add     r8,r6,#V_DIRTY
        mov     r9,#1
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
