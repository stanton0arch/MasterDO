; dynarec_j0.s - one Z80 block translated by hand into ARM60 instructions.
;
; The block is keep_yxf of the instruction test image, twenty-three bytes at
; 0x2B04, reproduced here opcode by opcode:
;
;   2B04  F5        PUSH AF
;   2B05  D1        POP DE
;   2B06  7B        LD A,E
;   2B07  E6 20     AND $20
;   2B09  CD 0F 2B  CALL $2B0F
;   2B0C  7B        LD A,E
;   2B0D  E6 08     AND $08
;   2B0F  28 07     JR Z,$2B18
;   2B11  34        INC (HL)
;   2B12  23        INC HL
;   2B13  20 04     JR NZ,$2B19
;   2B15  34        INC (HL)
;   2B16  18 01     JR $2B19
;   2B18  23        INC HL
;   2B19  23        INC HL
;   2B1A  C9        RET
;
; It is a translation and not an imitation: every store the block makes into
; emulated memory is made here, in the same order and to the same address,
; the pushed return address included. What an interpreter would leave behind
; and what this leaves behind have to be the same bytes, or the figure this
; file exists to produce is worth nothing.
;
; ---------------------------------------------------------------------------
; Two properties this code is written to keep, both load bearing:
;
; POSITION INDEPENDENT. It is copied into a buffer at run time and entered
; there. So: no literal pool, no external symbol, no absolute reference to
; itself -- every constant is an ARM immediate and every branch is local and
; PC relative. Adding one LDR of a literal would silently break it, because
; the pool sits outside the copied span and the load would read whatever the
; buffer happens to be followed by.
;
; SIXTEEN BIT REGISTERS LIVE IN THE TOP HALF of their ARM register. HL and SP
; are held shifted left by sixteen, so that incrementing is one ADD and the
; wrap at 0xFFFF is the natural overflow out of bit 31 rather than a masking
; instruction after every arithmetic. An address is recovered with one LSR,
; which is the price, and it is paid once per access instead of once per
; increment.
; ---------------------------------------------------------------------------
;
; Register plan, for the whole of both routines:
;   a1 = state in (read at entry, free afterwards)
;   a2 = state out (held to the end)
;   a3 = scratch    a4 = scratch, then the address the final RET popped
;   v1 = flat base of emulated memory
;   v2 = A          v3 = F          v4 = D          v5 = E
;   v6 = HL << 16   ip = SP << 16
;
; Struct offsets: CONTRACT with dynarec_j0.h - keep in sync.
;   +0 a  +4 f  +8 d  +12 e  +16 hl  +20 sp  +24 pc  +28 mem

        AREA    |C$$code|, CODE, READONLY

        EXPORT  dynarec_j0_block
        EXPORT  dynarec_j0_block_end
        EXPORT  dynarec_j0_stub
        EXPORT  dynarec_j0_stub_end

ST_A    EQU     0
ST_F    EQU     4
ST_D    EQU     8
ST_E    EQU     12
ST_HL   EQU     16
ST_SP   EQU     20
ST_PC   EQU     24
ST_MEM  EQU     28

; Z80 flag bits, as this project's core lays them out (src/z80.h).
FLAG_C  EQU     0x01
FLAG_PV EQU     0x04
FLAG_H  EQU     0x10
FLAG_Z  EQU     0x40
; S, Y and X are copied straight out of a result by one mask.
FLAG_SYX EQU    0xA8

; =========================================================================
; The load and store that bracket every block. A translated block starts by
; bringing the emulated registers into ARM registers and ends by putting them
; back; that is what a block boundary costs, and it is measured rather than
; assumed -- the stub at the bottom of this file is these two sequences with
; nothing between them.
; =========================================================================

        MACRO
        J0_ENTER
        LDR     v1, [a1, #ST_MEM]
        LDR     v2, [a1, #ST_A]
        LDR     v3, [a1, #ST_F]
        LDR     v4, [a1, #ST_D]
        LDR     v5, [a1, #ST_E]
        LDR     a3, [a1, #ST_HL]
        MOV     v6, a3, LSL #16
        LDR     a3, [a1, #ST_SP]
        MOV     ip, a3, LSL #16
        MEND

        MACRO
        J0_LEAVE
        STR     v2, [a2, #ST_A]
        STR     v3, [a2, #ST_F]
        STR     v4, [a2, #ST_D]
        STR     v5, [a2, #ST_E]
        MOV     a3, v6, LSR #16
        STR     a3, [a2, #ST_HL]
        MOV     a3, ip, LSR #16
        STR     a3, [a2, #ST_SP]
        STR     a4, [a2, #ST_PC]
        STR     v1, [a2, #ST_MEM]
        MEND

; -------------------------------------------------------------------------
; AND n -- A &= n, and F rebuilt whole.
;
; S, Y and X come out of the result by one mask; H is set by definition; N and
; C are cleared, which building F from nothing already does. Z and parity are
; the two that cost.
;
; The parity fold is three EORs rather than a lookup table: a table would be a
; literal pool reference, which this file cannot have, and on a machine with
; no cache three register operations beat a load anyway.
;
; Deliberately NOT specialised on the immediate. Both immediates this block
; uses have a single bit, so parity would collapse into the zero test and two
; instructions would do -- a real translator would take that, and the figure
; would then describe this block rather than translated code in general. The
; conservative form is the one worth measuring.
; -------------------------------------------------------------------------
        MACRO
        J0_AND_N $n
        AND     v2, v2, #$n
        AND     v3, v2, #FLAG_SYX
        ORR     v3, v3, #FLAG_H
        CMP     v2, #0
        ORREQ   v3, v3, #FLAG_Z
        EOR     a3, v2, v2, LSR #4
        EOR     a3, a3, a3, LSR #2
        EOR     a3, a3, a3, LSR #1
        TST     a3, #1
        ORREQ   v3, v3, #FLAG_PV
        MEND

; -------------------------------------------------------------------------
; INC (HL) -- read, add one, write back, and flags.
;
; C is the one flag this instruction leaves alone, so F is rebuilt from it
; rather than from nothing. H is the low nibble having wrapped to zero, and PV
; is the one value that overflows a signed byte, 0x7F becoming 0x80.
; -------------------------------------------------------------------------
        MACRO
        J0_INC_HL_IND
        MOV     a3, v6, LSR #16
        LDRB    a4, [v1, a3]
        ADD     a4, a4, #1
        AND     a4, a4, #0xFF
        STRB    a4, [v1, a3]
        AND     v3, v3, #FLAG_C
        AND     a3, a4, #FLAG_SYX
        ORR     v3, v3, a3
        CMP     a4, #0
        ORREQ   v3, v3, #FLAG_Z
        TST     a4, #0x0F
        ORREQ   v3, v3, #FLAG_H
        CMP     a4, #0x80
        ORREQ   v3, v3, #FLAG_PV
        MEND

; -------------------------------------------------------------------------
; The block body from 0x2B0F to the RET at 0x2B1A. It is reached twice per
; call of keep_yxf -- once through the CALL at 0x2B09, once by falling through
; from 0x2B0D -- so it is written out twice below rather than called.
;
; Inlining it is what a translator does with a call whose target is known, and
; it is why this file has no BL: the emulated stack still gets its push and
; its pop, because the guest can read that stack, but the real control flow is
; ARM's own.
;
; The conditional branch at 0x2B13 tests the Z left by INC (HL) at 0x2B11 --
; INC HL in between touches no flag. That is the carry from the low byte of
; the counter into its high byte, and reading it as anything else would put
; the whole block on the wrong path once every two hundred and fifty six
; increments.
; -------------------------------------------------------------------------
        MACRO
        J0_BODY $tag
        TST     v3, #FLAG_Z
        BNE     j0_l18_$tag                     ; 2B0F  JR Z,$2B18
        J0_INC_HL_IND                           ; 2B11  INC (HL)
        ADD     v6, v6, #0x10000                ; 2B12  INC HL
        TST     v3, #FLAG_Z
        BEQ     j0_l19_$tag                     ; 2B13  JR NZ,$2B19
        J0_INC_HL_IND                           ; 2B15  INC (HL)
        B       j0_l19_$tag                     ; 2B16  JR $2B19
j0_l18_$tag
        ADD     v6, v6, #0x10000                ; 2B18  INC HL
j0_l19_$tag
        ADD     v6, v6, #0x10000                ; 2B19  INC HL
        ; 2B1A  RET -- pop the sixteen bit address, low byte first.
        MOV     a3, ip, LSR #16
        LDRB    a4, [v1, a3]
        ADD     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        LDRB    a3, [v1, a3]
        ADD     ip, ip, #0x10000
        ORR     a4, a4, a3, LSL #8
        MEND

; =========================================================================
; dynarec_j0_block(const j0_state_t *in, j0_state_t *out)
;
; One call is one execution of keep_yxf, from the state in `in`, leaving the
; state in `out` and its side effects in emulated memory. Reading the input
; from one struct and writing the output to another is what lets the timing
; loop call it a hundred thousand times without re-priming anything between
; two calls: every call starts from the same registers.
; =========================================================================
dynarec_j0_block
        STMFD   sp!, {v1-v6, lr}
        J0_ENTER

        ; 2B04  PUSH AF -- A at SP-1, F at SP-2.
        SUB     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        STRB    v2, [v1, a3]
        SUB     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        STRB    v3, [v1, a3]

        ; 2B05  POP DE -- E low, D high.
        MOV     a3, ip, LSR #16
        LDRB    v5, [v1, a3]
        ADD     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        LDRB    v4, [v1, a3]
        ADD     ip, ip, #0x10000

        MOV     v2, v5                          ; 2B06  LD A,E
        J0_AND_N 0x20                           ; 2B07  AND $20

        ; 2B09  CALL $2B0F -- push 0x2B0C, high byte first.
        SUB     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        MOV     a4, #0x2B
        STRB    a4, [v1, a3]
        SUB     ip, ip, #0x10000
        MOV     a3, ip, LSR #16
        MOV     a4, #0x0C
        STRB    a4, [v1, a3]

        J0_BODY call                            ; 2B0F..2B1A, first pass

        MOV     v2, v5                          ; 2B0C  LD A,E
        J0_AND_N 0x08                           ; 2B0D  AND $08

        J0_BODY fall                            ; 2B0F..2B1A, second pass

        ; a4 now holds what the second RET popped: the address keep_yxf
        ; returns to, which is the block's exit PC and the last thing the
        ; checker compares.
        J0_LEAVE
        LDMFD   sp!, {v1-v6, pc}
dynarec_j0_block_end

; =========================================================================
; dynarec_j0_stub(const j0_state_t *in, j0_state_t *out)
;
; The two boundary sequences with no block between them. Timed over the same
; number of calls as the block above, it says what a block boundary costs on
; this machine, and the subtraction says what the translated body costs. It is
; copied into the same buffer and entered the same way, so the call itself is
; on both sides of that subtraction and cancels.
; =========================================================================
dynarec_j0_stub
        STMFD   sp!, {v1-v6, lr}
        J0_ENTER
        MOV     a4, #0
        J0_LEAVE
        LDMFD   sp!, {v1-v6, pc}
dynarec_j0_stub_end

        END
