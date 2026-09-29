; smsmem_a.s - Sega mapper writes in assembly (see smsmem.c).
;
; Special write handler of the translator (ctx->write_a, conventions in
; z80jit.h) for the Master System memory map: writes to the ROM area are
; ignored, writes to the last page ($FF00-$FFFF) go to the system RAM
; mirror, and a write to a bank register ($FFFD-$FFFF) that changes the
; bank fills the read table of its slot here. The rare cases (the RAM
; mapping register $FFFC, cartridge RAM, a bank count that is not a power
; of two) go through the C callback (sms_mem_write).
;
; Layouts: CONTRACT with sms_mem (smsmem.h) and z80j_ctx (z80jit.h) - keep
; in sync (smsmem.c and z80jit.c check the offsets at compile time).

        AREA    |C$$code|, CODE, READONLY

        EXPORT  sms_mem_write_a

        IMPORT  z80j_write_generic
        IMPORT  z80j_write_result

; Context fields, relative to the global pointer.
SLOT_BANK       EQU     0x47C
WDATA           EQU     0x4B0
MRAM            EQU     0x1000

; sms_mem fields.
SM_ROM          EQU     24
SM_CARTRAM      EQU     36
SM_REG          EQU     40
SM_REMAPS       EQU     68
SM_BANKMASK     EQU     72

Z80J_WRITE_PAGING EQU   2

;----------------------------------------------------------------------------
; In: r0 = value, r12 = Z80 address (bits 16-31), r2 = return address,
; lr -> data words of the write stub.
;----------------------------------------------------------------------------
sms_mem_write_a
        cmp     r12,#0xC0000000
        movlo   pc,r2                   ; ROM area: the write is ignored
        stmfd   sp!,{r2,r4-r9,r12,lr}
        mov     r12,r12,lsr#16          ; address, $FF00-$FFFF
        and     r0,r0,#0xFF
        sub     r4,r12,#0xE000          ; system RAM mirror: mram + (address - $E000)
        add     r4,r4,r10
        add     r4,r4,#MRAM
        strb    r0,[r4]
        sub     r12,r12,#0xFF00
        subs    r12,r12,#0xFC           ; paging register 0-3
        blo     write_a_stay
        ldr     r1,[r10,#WDATA]         ; sms_mem
        add     r4,r1,r12,lsl#2
        ldr     r5,[r4,#SM_REG]
        cmp     r5,r0
        beq     write_a_stay            ; same value: the mapping stands
        teq     r12,#0                  ; $FFFC: RAM mapping control
        beq     write_a_c
        ldr     r5,[r1,#SM_CARTRAM]
        teq     r5,#0                   ; cartridge RAM present
        bne     write_a_c
        ldr     r5,[r1,#SM_BANKMASK]
        cmn     r5,#1                   ; bank count not a power of two
        beq     write_a_c
        str     r0,[r4,#SM_REG]
        and     r0,r0,r5                ; bank
        sub     r12,r12,#1              ; slot 0-2
        add     r4,r10,r12,lsl#2
        str     r0,[r4,#SLOT_BANK]
        ldr     r4,[r1,#SM_REMAPS]
        add     r4,r4,#1
        str     r4,[r1,#SM_REMAPS]
        ldr     r4,[r1,#SM_ROM]
        add     r4,r4,r0,lsl#14         ; host address of the bank
        sub     r2,r4,r12,lsl#14        ; minus the Z80 address of the slot
        ; Read table entries of the slot: page p is at r10 - 4 - 4 * p,
        ; so the 64 pages of slot s are the words from r10 - 256 * (s + 1)
        ; upwards (slot 0: its first four pages, the fixed 1 KiB, stay).
        add     r4,r12,#1
        sub     r4,r10,r4,lsl#8
        mov     r0,r2
        mov     r1,r2
        mov     r5,r2
        mov     r6,r2
        mov     r7,r2
        mov     r8,r2
        mov     r9,r2
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        stmia   r4!,{r0,r1,r2,r5-r9}
        teq     r12,#0
        stmneia r4!,{r0,r1,r2,r5-r9}
        stmeqia r4!,{r0,r1,r2,r5}
        mov     r0,#Z80J_WRITE_PAGING
        ldmfd   sp!,{r2,r4-r9,r12,lr}
        b       z80j_write_result
write_a_stay
        ldmfd   sp!,{r2,r4-r9,r12,lr}
        mov     pc,r2
write_a_c
        ldmfd   sp!,{r2,r4-r9,r12,lr}
        b       z80j_write_generic

        END
