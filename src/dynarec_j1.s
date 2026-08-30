; dynarec_j1.s - four blocks translated by hand, and the link that chains them.
;
; J0 measured one block called from C and found the boundary -- loading the
; emulated registers in and storing them back out -- costing 141.9 cycles, 45%
; of the block. This file is the answer to that: the registers come in once,
; a block ends by branching STRAIGHT INTO the next one, and the state goes back
; out only when the chain has nothing left to run.
;
; The region is the head of the hot loop of the SCF/CCF pre-test:
;
;   2A86  21 AA C0  LD HL,$C0AA     2A8E  AF        XOR A
;   2A89  AF        XOR A           2A8F  3D        DEC A
;   2A8A  3F        CCF             2A90  3E 00     LD A,$00
;   2A8B  CD 04 2B  CALL $2B04      2A92  3F        CCF
;                                   2A93  CD 04 2B  CALL $2B04
;   2A96  AF        XOR A
;   2A97  5F        LD E,A          2B04  keep_yxf, translated in J0 and
;   2A98  1D        DEC E                 carried over here unchanged in
;   2A99  3F        CCF                   substance
;   2A9A  CD 04 2B  CALL $2B04
;
; Four blocks, six links per pass: three straight runs and three entries into
; keep_yxf, whose RET is the interesting link -- its target is popped at run
; time, not written in the instruction, so it has to go through the table like
; any other. A translator could branch a CALL in hard; it can never do that to
; a RET, and the table exists for exactly that case.
;
; ---------------------------------------------------------------------------
; THE LINK, which is what this file exists to measure:
;
;     CMP  quota,#0        / BLE exit        -- time spent?
;     LDR  next,[table,z80addr,LSL #2]       -- where does that address live?
;     CMP  next,#0         / BEQ exit        -- translated at all?
;     ADD  links,links,#1  / MOV pc,next     -- go, without touching memory
;
; Six instructions and one memory read, against the forty-odd of a full
; boundary. The read is a full price access on a machine with no cache, and
; that is why the figure is measured rather than counted.
; ---------------------------------------------------------------------------
;
; Position independent, for the reason J0 gives: this code is copied into a
; buffer and entered there. No literal pool, no external symbol, every constant
; an ARM immediate, every branch local. The table holds absolute addresses
; INSIDE that buffer, and the C side is what fills it.
;
; Register plan:
;   a1 = T-states left    a2 = state out (held to the end)
;   a3 = scratch          a4 = the next Z80 address, and scratch inside a block
;   v1 = emulated memory  v2 = A   v3 = F   v4 = D   v5 = E
;   v6 = HL << 16         fp = SP << 16     ip = block table
;   lr = links traversed  (its caller value is on the stack)
;
; Struct offsets: CONTRACT with dynarec_j1.h - keep in sync.
;   +0 a +4 f +8 d +12 e +16 hl +20 sp +24 pc +28 quota +32 links +36 mem +40 table

        AREA    |C$$code|, CODE, READONLY

        EXPORT  dynarec_j1_chain
        EXPORT  dynarec_j1_chain_end
        EXPORT  dynarec_j1_at_2a86
        EXPORT  dynarec_j1_at_2a8e
        EXPORT  dynarec_j1_at_2a96
        EXPORT  dynarec_j1_at_2a9d
        EXPORT  dynarec_j1_at_2b04

ST_A     EQU    0
ST_F     EQU    4
ST_D     EQU    8
ST_E     EQU    12
ST_HL    EQU    16
ST_SP    EQU    20
ST_PC    EQU    24
ST_QUOTA EQU    28
ST_LINKS EQU    32
ST_MEM   EQU    36
ST_TABLE EQU    40

FLAG_C   EQU    0x01
FLAG_N   EQU    0x02
FLAG_PV  EQU    0x04
FLAG_H   EQU    0x10
FLAG_Z   EQU    0x40
FLAG_SYX EQU    0xA8            ; S, and the two undocumented bits
FLAG_53  EQU    0x28            ; the two undocumented bits alone
FLAG_SZPV EQU   0xC4            ; what CCF keeps of the old F

; =========================================================================
; The link. a4 must hold the Z80 address to go to.
; =========================================================================
        MACRO
        J1_LINK
        CMP     a1, #0
        BLE     j1_exit
        LDR     a3, [ip, a4, LSL #2]
        CMP     a3, #0
        BEQ     j1_exit
        ADD     lr, lr, #1
        MOV     pc, a3
        MEND

; -------------------------------------------------------------------------
; XOR A -- the accumulator against itself, so the result is zero whatever it
; held, and the flags that follow are constant: zero set, parity of zero even,
; half carry cleared by the logical group, carry cleared.
;
; This is not specialising on an operand -- there is no operand. Computing it
; would be computing a constant.
; -------------------------------------------------------------------------
        MACRO
        J1_XOR_A
        MOV     v2, #0
        MOV     v3, #0x44
        MEND

; -------------------------------------------------------------------------
; CCF -- the complement of the carry, and the old carry moves into the half
; carry, which is how the part records what it just undid (z80_ops.h:857-876).
; The two undocumented bits come from A, not from the result.
;
; The TST reads the OLD carry and the writes below set no flags, so the
; condition survives the rebuilding of F underneath it.
; -------------------------------------------------------------------------
        MACRO
        J1_CCF
        AND     a3, v3, #FLAG_SZPV
        TST     v3, #FLAG_C
        AND     v3, v2, #FLAG_53
        ORR     v3, v3, a3
        ORRNE   v3, v3, #FLAG_H
        ORREQ   v3, v3, #FLAG_C
        MEND

; -------------------------------------------------------------------------
; DEC r -- carry untouched, subtract flag set, and the half carry and overflow
; both read the value BEFORE the decrement (z80_ops.h:525-541).
; -------------------------------------------------------------------------
        MACRO
        J1_DEC_R $r
        MOV     a3, $r
        SUB     $r, $r, #1
        AND     $r, $r, #0xFF
        AND     v3, v3, #FLAG_C
        ORR     v3, v3, #FLAG_N
        AND     a4, $r, #FLAG_SYX
        ORR     v3, v3, a4
        CMP     $r, #0
        ORREQ   v3, v3, #FLAG_Z
        CMP     a3, #0x80
        ORREQ   v3, v3, #FLAG_PV
        TST     a3, #0x0F
        ORREQ   v3, v3, #FLAG_H
        MEND

; -------------------------------------------------------------------------
; CALL $2B04 -- the return address pushed high byte first, then the target
; handed to the link. Sixteen bit values live in the top half of a register,
; so a decrement of the stack pointer is one instruction and the wrap at
; 0xFFFF is the overflow out of bit 31.
; -------------------------------------------------------------------------
        MACRO
        J1_CALL_2B04 $rethi, $retlo
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        MOV     a4, #$rethi
        STRB    a4, [v1, a3]
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        MOV     a4, #$retlo
        STRB    a4, [v1, a3]
        MOV     a4, #0x2B00
        ORR     a4, a4, #0x04
        MEND

; RET -- the popped address becomes the link's input.
        MACRO
        J1_RET
        MOV     a3, fp, LSR #16
        LDRB    a4, [v1, a3]
        ADD     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        LDRB    a3, [v1, a3]
        ADD     fp, fp, #0x10000
        ORR     a4, a4, a3, LSL #8
        MEND

; INC (HL) -- carried over from J0, where it was checked against the core.
        MACRO
        J1_INC_HL_IND
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

; AND n -- carried over from J0, general form, not specialised on the mask.
        MACRO
        J1_AND_N $n
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
; The body of keep_yxf from 0x2B0F to its RET, reached twice per entry.
;
; The T-states are charged by path, because the path is what the branches
; decide: 34 when the first branch is taken, 52 when it is not and the second
; is, 70 when neither is. Those are the same three figures the interpreter
; counts, which is what makes the two costs comparable.
; -------------------------------------------------------------------------
        MACRO
        J1_BODY $tag
        TST     v3, #FLAG_Z
        BNE     j1_pa_$tag                      ; 2B0F  JR Z,$2B18
        SUB     a1, a1, #24                     ; 7 + 11 + 6
        J1_INC_HL_IND                           ; 2B11  INC (HL)
        ADD     v6, v6, #0x10000                ; 2B12  INC HL
        TST     v3, #FLAG_Z
        BEQ     j1_pb_$tag                      ; 2B13  JR NZ,$2B19
        SUB     a1, a1, #46                     ; 7 + 11 + 12 + 6 + 10
        J1_INC_HL_IND                           ; 2B15  INC (HL)
        B       j1_l19_$tag                     ; 2B16  JR $2B19
j1_pb_$tag
        SUB     a1, a1, #28                     ; 12 + 6 + 10
        B       j1_l19_$tag
j1_pa_$tag
        SUB     a1, a1, #34                     ; 12 + 6 + 6 + 10
        ADD     v6, v6, #0x10000                ; 2B18  INC HL
j1_l19_$tag
        ADD     v6, v6, #0x10000                ; 2B19  INC HL
        J1_RET                                  ; 2B1A  RET
        MEND

; =========================================================================
; dynarec_j1_chain(const j1_state_t *in, j1_state_t *out)
;
; Brings the emulated registers in, dispatches on the starting address, and
; does not come back until the time is spent or the next address has no
; translation.
; =========================================================================
dynarec_j1_chain
        STMFD   sp!, {v1-v6, fp, lr}
        LDR     v1, [a1, #ST_MEM]
        LDR     ip, [a1, #ST_TABLE]
        LDR     v2, [a1, #ST_A]
        LDR     v3, [a1, #ST_F]
        LDR     v4, [a1, #ST_D]
        LDR     v5, [a1, #ST_E]
        LDR     a3, [a1, #ST_HL]
        MOV     v6, a3, LSL #16
        LDR     a3, [a1, #ST_SP]
        MOV     fp, a3, LSL #16
        LDR     a4, [a1, #ST_PC]
        LDR     a3, [a1, #ST_QUOTA]
        MOV     lr, #0
        MOV     a1, a3
        LDR     a3, [ip, a4, LSL #2]
        CMP     a3, #0
        BEQ     j1_exit
        MOV     pc, a3

; -------------------------------------------------------------------------
; The one way out. Everything the chain holds goes back to the struct,
; including where it stopped and what it has left -- the caller resumes from
; exactly there.
; -------------------------------------------------------------------------
j1_exit
        STR     v2, [a2, #ST_A]
        STR     v3, [a2, #ST_F]
        STR     v4, [a2, #ST_D]
        STR     v5, [a2, #ST_E]
        MOV     a3, v6, LSR #16
        STR     a3, [a2, #ST_HL]
        MOV     a3, fp, LSR #16
        STR     a3, [a2, #ST_SP]
        STR     a4, [a2, #ST_PC]
        STR     a1, [a2, #ST_QUOTA]
        STR     lr, [a2, #ST_LINKS]
        STR     v1, [a2, #ST_MEM]
        STR     ip, [a2, #ST_TABLE]
        LDMFD   sp!, {v1-v6, fp, pc}

; =========================================================================
; 0x2A86 -- LD HL,$C0AA / XOR A / CCF / CALL $2B04.  10+4+4+17 = 35
; =========================================================================
dynarec_j1_at_2a86
        SUB     a1, a1, #35
        MOV     v6, #0x00AA0000                 ; LD HL,$C0AA
        ORR     v6, v6, #0xC0000000
        J1_XOR_A
        J1_CCF
        J1_CALL_2B04 0x2A, 0x8E
        J1_LINK

; =========================================================================
; 0x2A8E -- XOR A / DEC A / LD A,$00 / CCF / CALL $2B04.  4+4+7+4+17 = 36
; =========================================================================
dynarec_j1_at_2a8e
        SUB     a1, a1, #36
        J1_XOR_A
        J1_DEC_R v2
        MOV     v2, #0x00                       ; LD A,$00
        J1_CCF
        J1_CALL_2B04 0x2A, 0x96
        J1_LINK

; =========================================================================
; 0x2A96 -- XOR A / LD E,A / DEC E / CCF / CALL $2B04.  4+4+4+4+17 = 33
; =========================================================================
dynarec_j1_at_2a96
        SUB     a1, a1, #33
        J1_XOR_A
        MOV     v5, v2                          ; LD E,A
        J1_DEC_R v5
        J1_CCF
        J1_CALL_2B04 0x2A, 0x9D
        J1_LINK

; =========================================================================
; 0x2A9D -- XOR A / LD A,$FF / CCF / CALL $2B04.  4+7+4+17 = 32
;
; The block that turns the memory path on. The three above leave A at zero, so
; the two undocumented bits CCF copies out of A are clear, and keep_yxf takes
; its short path twice and touches no memory at all. Here A is 0xFF, both bits
; are set, and both of keep_yxf's increments run. Without this block the region
; would measure -- and check -- only the cheap half of the loop.
; =========================================================================
dynarec_j1_at_2a9d
        SUB     a1, a1, #32
        J1_XOR_A
        MOV     v2, #0xFF                       ; LD A,$FF
        J1_CCF
        J1_CALL_2B04 0x2A, 0xA4
        J1_LINK

; =========================================================================
; 0x2B04 -- keep_yxf. PUSH AF 11, POP DE 10, LD A,E 4, AND 7, CALL 17 = 49
; before the first body; 4 + 7 = 11 between the two.
; =========================================================================
dynarec_j1_at_2b04
        SUB     a1, a1, #49
        SUB     fp, fp, #0x10000                ; 2B04  PUSH AF
        MOV     a3, fp, LSR #16
        STRB    v2, [v1, a3]
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        STRB    v3, [v1, a3]

        MOV     a3, fp, LSR #16                 ; 2B05  POP DE
        LDRB    v5, [v1, a3]
        ADD     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        LDRB    v4, [v1, a3]
        ADD     fp, fp, #0x10000

        MOV     v2, v5                          ; 2B06  LD A,E
        J1_AND_N 0x20                           ; 2B07  AND $20

        SUB     fp, fp, #0x10000                ; 2B09  CALL $2B0F
        MOV     a3, fp, LSR #16
        MOV     a4, #0x2B
        STRB    a4, [v1, a3]
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        MOV     a4, #0x0C
        STRB    a4, [v1, a3]

        J1_BODY call

        SUB     a1, a1, #11
        MOV     v2, v5                          ; 2B0C  LD A,E
        J1_AND_N 0x08                           ; 2B0D  AND $08

        J1_BODY fall
        J1_LINK

dynarec_j1_chain_end

        END
