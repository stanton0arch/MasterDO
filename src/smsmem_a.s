; smsmem_a.s - Sega mapper writes in assembly (see smsmem.c).
;
; Special write handler of the translator (ctx->write_a, conventions in
; z80jit.h) for the Master System memory map: writes to the ROM area are
; ignored under the Sega mapper (they select banks under the Codemasters
; mapper: C callback), writes to the last page ($FF00-$FFFF) go to the
; system RAM mirror, and a write to a bank register of the Sega mapper
; ($FFFD-$FFFF) that changes the bank fills the read table of its slot
; here. The rare cases (the RAM mapping register $FFFC, slot 2 while it
; shows cartridge RAM, a bank count that is not a power of two) go
; through the C callback (sms_mem_write). sms_mem_write_t is the same
; handler for an image whose banks are apart (sms_mem.bank_tab): the host
; address of the bank comes from the table instead of the image's start
; (and C takes the banks absent from it).
;
; Layouts: CONTRACT with sms_mem (smsmem.h) and z80j_ctx (z80jit.h) - keep
; in sync (smsmem.c and z80jit.c check the offsets at compile time).

        AREA    |C$$code|, CODE, READONLY

        EXPORT  sms_mem_write_a
        EXPORT  sms_mem_write_t

        IMPORT  z80j_write_generic
        IMPORT  z80j_write_result

; Context fields, relative to the global pointer.
SLOT_BANK       EQU     0x47C
WDATA           EQU     0x4B0
MRAM            EQU     0x1000

; sms_mem fields.
SM_ROM          EQU     24
SM_BANKTAB      EQU     28
SM_REG          EQU     40
SM_REMAPS       EQU     68
SM_BANKMASK     EQU     72
SM_MAPPER       EQU     80

; sms_banks fields.
SB_LAST         EQU     1024

Z80J_WRITE_PAGING EQU   2

;----------------------------------------------------------------------------
; In: r0 = value, r12 = Z80 address (bits 16-31), r2 = return address,
; lr -> data words of the write stub.
;----------------------------------------------------------------------------
sms_mem_write_a
        cmp     r12,#0xC0000000
        bhs     write_a_ram
write_a_rom
        ldr     r1,[r10,#WDATA]         ; ROM area: ignored by the Sega
        ldr     r1,[r1,#SM_MAPPER]      ; mapper, a bank register otherwise
        teq     r1,#0
        moveq   pc,r2
        b       z80j_write_generic
write_a_ram
        stmfd   sp!,{r2,r4,r5,r12,lr}
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
        ldr     r5,[r1,#SM_MAPPER]
        teq     r5,#0                   ; plain RAM under the other mappers
        bne     write_a_stay
        add     r4,r1,r12,lsl#2
        ldr     r5,[r4,#SM_REG]
        cmp     r5,r0
        beq     write_a_stay            ; same value: the mapping stands
        teq     r12,#0                  ; $FFFC: RAM mapping control
        beq     write_a_c
        teq     r12,#3
        bne     write_a_bank
        ldr     r5,[r1,#SM_REG]
        tst     r5,#0x08                ; slot 2 showing cartridge RAM
        bne     write_a_c
write_a_bank
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
write_a_slot
        sub     r2,r4,r12,lsl#14        ; minus the Z80 address of the slot
        ; Read table entries of the slot (1 KiB pages): page p is at
        ; r10 - 4 - 4 * p, so the 16 pages of slot s are the words from
        ; r10 - 64 * (s + 1) upwards, the highest page first (slot 0: its
        ; first page, the fixed 1 KiB, is the last word and stays).
        add     r4,r12,#1
        sub     r4,r10,r4,lsl#6
        mov     r0,r2
        mov     r1,r2
        mov     r5,r2
        stmia   r4!,{r0,r1,r2,r5}
        stmia   r4!,{r0,r1,r2,r5}
        stmia   r4!,{r0,r1,r2,r5}
        teq     r12,#0
        stmneia r4,{r0,r1,r2,r5}
        stmeqia r4,{r0,r1,r2}
        mov     r0,#Z80J_WRITE_PAGING
        ldmfd   sp!,{r2,r4,r5,r12,lr}
        b       z80j_write_result
write_a_stay
        ldmfd   sp!,{r2,r4,r5,r12,lr}
        mov     pc,r2
write_a_c
        ldmfd   sp!,{r2,r4,r5,r12,lr}
        b       z80j_write_generic

;----------------------------------------------------------------------------
; The same for an image whose banks are apart (sms_banks), up to the
; bank's address: a bank absent from the table goes to C, which reads it
; into the bank cache; the bank leaving the slot and the bank coming are
; stamped with the paging count, the order of that cache. The read table
; is filled, and the exits taken, by the code above.
;----------------------------------------------------------------------------
sms_mem_write_t
        cmp     r12,#0xC0000000
        blo     write_a_rom
        stmfd   sp!,{r2,r4,r5,r12,lr}
        mov     r12,r12,lsr#16          ; address, $FF00-$FFFF
        and     r0,r0,#0xFF
        sub     r4,r12,#0xE000          ; system RAM mirror
        add     r4,r4,r10
        add     r4,r4,#MRAM
        strb    r0,[r4]
        sub     r12,r12,#0xFF00
        subs    r12,r12,#0xFC           ; paging register 0-3
        blo     write_a_stay
        ldr     r1,[r10,#WDATA]         ; sms_mem
        ldr     r5,[r1,#SM_MAPPER]
        teq     r5,#0                   ; plain RAM under the other mappers
        bne     write_a_stay
        add     r4,r1,r12,lsl#2
        ldr     r5,[r4,#SM_REG]
        cmp     r5,r0
        beq     write_a_stay            ; same value: the mapping stands
        teq     r12,#0                  ; $FFFC: RAM mapping control
        beq     write_a_c
        teq     r12,#3
        bne     write_t_bank
        ldr     r5,[r1,#SM_REG]
        tst     r5,#0x08                ; slot 2 showing cartridge RAM
        bne     write_a_c
write_t_bank
        ldr     r5,[r1,#SM_BANKMASK]    ; banks - 1, or the table's last entry
        and     r2,r0,r5                ; bank
        ldr     r5,[r1,#SM_BANKTAB]
        ldr     lr,[r5,r2,lsl#2]        ; host address of the bank
        teq     lr,#0
        beq     write_a_c               ; absent (read into the cache), or past the count
        str     r0,[r4,#SM_REG]
        sub     r12,r12,#1              ; slot 0-2
        add     r4,r10,r12,lsl#2
        ldr     r0,[r4,#SLOT_BANK]      ; the bank leaving the slot
        str     r2,[r4,#SLOT_BANK]
        ldr     r4,[r1,#SM_REMAPS]
        add     r4,r4,#1
        str     r4,[r1,#SM_REMAPS]
        add     r5,r5,#SB_LAST          ; both stamped with the paging count
        str     r4,[r5,r0,lsl#2]
        str     r4,[r5,r2,lsl#2]
        mov     r4,lr
        b       write_a_slot

        END
