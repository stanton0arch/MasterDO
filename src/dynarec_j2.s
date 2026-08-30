; dynarec_j2.s - the same chain as J1, in four variants, to find out which of
; two things costs.
;
; J1 chained the blocks and the boundary's share fell from 0.865 to 0.207
; cycles per T-state, divided by four. But the total only fell to 1.52 where
; that amortisation alone predicted 1.26, leaving 0.261 unexplained between two
; suspects it could not separate:
;
;   the LINK   -- seven instructions, one of them a load from a 256 KB table
;   the FLAGS  -- DEC costs 13 native instructions for 4 T-states, AND n costs
;                 10 for 7, INC (HL) costs 14 for 11
;
; So the chain is emitted four times, with the two levers switched on and off
; independently. Four figures on the same region and the same code layout, one
; lever apart, close the question rather than reporting a gain over it.
;
;   variant 0 : neither  (J1's forms, the in-file baseline)
;   variant 1 : link only
;   variant 2 : flags only
;   variant 3 : both
;
; ---------------------------------------------------------------------------
; THE LINK, reduced from seven instructions to two:
;
;     BLE  exit                        -- the block's last quota subtraction is
;     LDR  pc,[table,z80addr,LSL #2]      a SUBS, so no CMP is needed here; and
;                                         an untranslated entry HOLDS THE EXIT
;                                         ADDRESS, so no test is needed either
;
; and a link whose target is written in the instruction -- a CALL, a JR, a JP --
; does not touch memory at all:
;
;     BLE  exit
;     B    block
;
; Four of this region's eight links are of that kind. Only a RET keeps the
; table, because only a RET has a target it pops rather than carries.
; ---------------------------------------------------------------------------
;
; Position independent, as in J0 and J1: no literal pool, no external symbol,
; every constant an ARM immediate, every branch local.
;
; Register plan:
;   a1 = T-states left     a2 = flag tables (the caller's out pointer is on the
;   a3 = scratch                stack, picked up again at the exit)
;   a4 = next Z80 address, and scratch inside a block
;   v1 = emulated memory   v2 = A   v3 = F   v4 = D   v5 = E
;   v6 = HL << 16          fp = SP << 16     ip = block table
;
; Struct offsets: CONTRACT with dynarec_j2.h - keep in sync.
;   +0 a +4 f +8 d +12 e +16 hl +20 sp +24 pc +28 quota +32 mem +36 table
;   +40 flags
;
; Flag table layout, one byte per value, all three in one block:
;   +0    decf[v]   what DEC writes, except the carry, from the value BEFORE
;   +256  sz53p[r]  sign, zero, the two undocumented bits and parity, from the
;                   result -- what the logical group needs
;   +512  incf[r]   what INC writes, except the carry, from the value AFTER

        AREA    |C$$code|, CODE, READONLY

        EXPORT  dynarec_j2_chain
        EXPORT  dynarec_j2_chain_end
        EXPORT  dynarec_j2_exit
        EXPORT  dynarec_j2_v0_2a86
        EXPORT  dynarec_j2_v0_2a8e
        EXPORT  dynarec_j2_v0_2a96
        EXPORT  dynarec_j2_v0_2a9d
        EXPORT  dynarec_j2_v0_2b04
        EXPORT  dynarec_j2_v1_2a86
        EXPORT  dynarec_j2_v1_2a8e
        EXPORT  dynarec_j2_v1_2a96
        EXPORT  dynarec_j2_v1_2a9d
        EXPORT  dynarec_j2_v1_2b04
        EXPORT  dynarec_j2_v2_2a86
        EXPORT  dynarec_j2_v2_2a8e
        EXPORT  dynarec_j2_v2_2a96
        EXPORT  dynarec_j2_v2_2a9d
        EXPORT  dynarec_j2_v2_2b04
        EXPORT  dynarec_j2_v3_2a86
        EXPORT  dynarec_j2_v3_2a8e
        EXPORT  dynarec_j2_v3_2a96
        EXPORT  dynarec_j2_v3_2a9d
        EXPORT  dynarec_j2_v3_2b04

ST_A     EQU    0
ST_F     EQU    4
ST_D     EQU    8
ST_E     EQU    12
ST_HL    EQU    16
ST_SP    EQU    20
ST_PC    EQU    24
ST_QUOTA EQU    28
ST_MEM   EQU    32
ST_TABLE EQU    36
ST_FLAGS EQU    40

FLAG_C   EQU    0x01
FLAG_N   EQU    0x02
FLAG_PV  EQU    0x04
FLAG_H   EQU    0x10
FLAG_Z   EQU    0x40
FLAG_SYX EQU    0xA8
FLAG_53  EQU    0x28
FLAG_SZPV EQU   0xC4

TAB_DEC  EQU    0
TAB_SZ53P EQU   256
TAB_INC  EQU    512

; =========================================================================
; The two links. a4 holds the Z80 address either way, so that an exit taken
; here reports where execution is to resume.
; =========================================================================

; A target the instruction carries: no table, no memory.
        MACRO
        J2_LINK_TO $lk, $label
        IF $lk = 1
        BLE     dynarec_j2_exit
        B       $label
        ELSE
        CMP     a1, #0
        BLE     dynarec_j2_exit
        LDR     a3, [ip, a4, LSL #2]
        CMP     a3, #0
        BEQ     dynarec_j2_exit
        MOV     pc, a3
        ENDIF
        MEND

; A target the chain has to look up, which is what a RET leaves behind.
        MACRO
        J2_LINK_DYN $lk
        IF $lk = 1
        BLE     dynarec_j2_exit
        LDR     pc, [ip, a4, LSL #2]
        ELSE
        CMP     a1, #0
        BLE     dynarec_j2_exit
        LDR     a3, [ip, a4, LSL #2]
        CMP     a3, #0
        BEQ     dynarec_j2_exit
        MOV     pc, a3
        ENDIF
        MEND

; The quota, spent. At the end of a block and nowhere else, because the reduced
; link reads the condition it leaves behind.
        MACRO
        J2_SPEND $lk, $n
        IF $lk = 1
        SUBS    a1, a1, #$n
        ELSE
        SUB     a1, a1, #$n
        ENDIF
        MEND

; =========================================================================
; The forms, computed or read out of a table.
; =========================================================================

; XOR A -- a constant either way. There is no operand to specialise on.
        MACRO
        J2_XOR_A
        MOV     v2, #0
        MOV     v3, #0x44
        MEND

; CCF -- depends on the old F and on A, so no table indexed by one byte serves
; it. Unchanged, and it is why the flag lever cannot reach everything.
        MACRO
        J2_CCF
        AND     a3, v3, #FLAG_SZPV
        TST     v3, #FLAG_C
        AND     v3, v2, #FLAG_53
        ORR     v3, v3, a3
        ORRNE   v3, v3, #FLAG_H
        ORREQ   v3, v3, #FLAG_C
        MEND

; DEC r -- thirteen instructions, or six and one read.
        MACRO
        J2_DEC_R $fl, $r
        MOV     a3, $r
        SUB     $r, $r, #1
        AND     $r, $r, #0xFF
        AND     v3, v3, #FLAG_C
        IF $fl = 1
        LDRB    a4, [a2, a3]
        ORR     v3, v3, a4
        ELSE
        ORR     v3, v3, #FLAG_N
        AND     a4, $r, #FLAG_SYX
        ORR     v3, v3, a4
        CMP     $r, #0
        ORREQ   v3, v3, #FLAG_Z
        CMP     a3, #0x80
        ORREQ   v3, v3, #FLAG_PV
        TST     a3, #0x0F
        ORREQ   v3, v3, #FLAG_H
        ENDIF
        MEND

; AND n -- ten instructions, or four and one read. The half carry is the
; instruction's own and is not in the table; the parity is.
        MACRO
        J2_AND_N $fl, $n
        AND     v2, v2, #$n
        IF $fl = 1
        ADD     a3, v2, #TAB_SZ53P
        LDRB    v3, [a2, a3]
        ORR     v3, v3, #FLAG_H
        ELSE
        AND     v3, v2, #FLAG_SYX
        ORR     v3, v3, #FLAG_H
        CMP     v2, #0
        ORREQ   v3, v3, #FLAG_Z
        EOR     a3, v2, v2, LSR #4
        EOR     a3, a3, a3, LSR #2
        EOR     a3, a3, a3, LSR #1
        TST     a3, #1
        ORREQ   v3, v3, #FLAG_PV
        ENDIF
        MEND

; INC (HL) -- fourteen instructions, or nine and one read.
        MACRO
        J2_INC_HL_IND $fl
        MOV     a3, v6, LSR #16
        LDRB    a4, [v1, a3]
        ADD     a4, a4, #1
        AND     a4, a4, #0xFF
        STRB    a4, [v1, a3]
        AND     v3, v3, #FLAG_C
        IF $fl = 1
        ADD     a3, a4, #TAB_INC
        LDRB    a3, [a2, a3]
        ORR     v3, v3, a3
        ELSE
        AND     a3, a4, #FLAG_SYX
        ORR     v3, v3, a3
        CMP     a4, #0
        ORREQ   v3, v3, #FLAG_Z
        TST     a4, #0x0F
        ORREQ   v3, v3, #FLAG_H
        CMP     a4, #0x80
        ORREQ   v3, v3, #FLAG_PV
        ENDIF
        MEND

; CALL $2B04 -- the return address pushed, then the target left in a4.
        MACRO
        J2_CALL_2B04 $rethi, $retlo
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

; RET -- the popped address becomes the link's input. Sets no condition, so a
; SUBS made before it still speaks at the link.
        MACRO
        J2_RET
        MOV     a3, fp, LSR #16
        LDRB    a4, [v1, a3]
        ADD     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        LDRB    a3, [v1, a3]
        ADD     fp, fp, #0x10000
        ORR     a4, a4, a3, LSL #8
        MEND

; -------------------------------------------------------------------------
; keep_yxf's body, 0x2B0F to its RET, charged by path: 34 when the first
; branch is taken, 52 when it is not and the second is, 70 when neither is.
;
; Each path's LAST subtraction is the one the link reads, so it is placed after
; everything that could disturb a condition -- after the second INC (HL) on the
; long path, not before it.
; -------------------------------------------------------------------------
        MACRO
        J2_BODY $lk, $fl, $tag
        TST     v3, #FLAG_Z
        BNE     j2_pa_$tag
        SUB     a1, a1, #24
        J2_INC_HL_IND $fl
        ADD     v6, v6, #0x10000
        TST     v3, #FLAG_Z
        BEQ     j2_pb_$tag
        J2_INC_HL_IND $fl
        J2_SPEND $lk, 46
        B       j2_l19_$tag
j2_pb_$tag
        J2_SPEND $lk, 28
        B       j2_l19_$tag
j2_pa_$tag
        J2_SPEND $lk, 34
        ADD     v6, v6, #0x10000
j2_l19_$tag
        ADD     v6, v6, #0x10000
        J2_RET
        MEND

; =========================================================================
; One whole variant of the region: four straight blocks and keep_yxf.
; =========================================================================
        MACRO
        J2_VARIANT $lk, $fl, $v

; 0x2A86 -- LD HL,$C0AA / XOR A / CCF / CALL $2B04.  10+4+4+17 = 35
dynarec_j2_$v._2a86
        MOV     v6, #0x00AA0000
        ORR     v6, v6, #0xC0000000
        J2_XOR_A
        J2_CCF
        J2_CALL_2B04 0x2A, 0x8E
        J2_SPEND $lk, 35
        J2_LINK_TO $lk, dynarec_j2_$v._2b04

; 0x2A8E -- XOR A / DEC A / LD A,$00 / CCF / CALL $2B04.  4+4+7+4+17 = 36
dynarec_j2_$v._2a8e
        J2_XOR_A
        J2_DEC_R $fl, v2
        MOV     v2, #0x00
        J2_CCF
        J2_CALL_2B04 0x2A, 0x96
        J2_SPEND $lk, 36
        J2_LINK_TO $lk, dynarec_j2_$v._2b04

; 0x2A96 -- XOR A / LD E,A / DEC E / CCF / CALL $2B04.  4+4+4+4+17 = 33
dynarec_j2_$v._2a96
        J2_XOR_A
        MOV     v5, v2
        J2_DEC_R $fl, v5
        J2_CCF
        J2_CALL_2B04 0x2A, 0x9D
        J2_SPEND $lk, 33
        J2_LINK_TO $lk, dynarec_j2_$v._2b04

; 0x2A9D -- XOR A / LD A,$FF / CCF / CALL $2B04.  4+7+4+17 = 32
; The block that turns the memory path on: the two undocumented bits CCF copies
; out of A are what keep_yxf branches on, and only here is A not zero.
dynarec_j2_$v._2a9d
        J2_XOR_A
        MOV     v2, #0xFF
        J2_CCF
        J2_CALL_2B04 0x2A, 0xA4
        J2_SPEND $lk, 32
        J2_LINK_TO $lk, dynarec_j2_$v._2b04

; 0x2B04 -- keep_yxf. 49 before the first body, 11 between the two.
dynarec_j2_$v._2b04
        SUB     a1, a1, #49
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        STRB    v2, [v1, a3]
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        STRB    v3, [v1, a3]

        MOV     a3, fp, LSR #16
        LDRB    v5, [v1, a3]
        ADD     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        LDRB    v4, [v1, a3]
        ADD     fp, fp, #0x10000

        MOV     v2, v5
        J2_AND_N $fl, 0x20

        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        MOV     a4, #0x2B
        STRB    a4, [v1, a3]
        SUB     fp, fp, #0x10000
        MOV     a3, fp, LSR #16
        MOV     a4, #0x0C
        STRB    a4, [v1, a3]

        J2_BODY 0, $fl, $v.call

        SUB     a1, a1, #11
        MOV     v2, v5
        J2_AND_N $fl, 0x08

        J2_BODY $lk, $fl, $v.fall
        J2_LINK_DYN $lk
        MEND

; =========================================================================
; dynarec_j2_chain(const j2_state_t *in, j2_state_t *out)
;
; The caller's out pointer goes on the stack so that a2 can hold the flag
; tables for the whole run; it is picked up again at the exit.
; =========================================================================
dynarec_j2_chain
        STMFD   sp!, {a2, v1-v6, fp, lr}
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
        LDR     a2, [a1, #ST_FLAGS]
        MOV     a1, a3
        LDR     pc, [ip, a4, LSL #2]

; -------------------------------------------------------------------------
; The one way out, shared by the four variants -- nothing in it depends on
; which one was running. Every untranslated entry of the table holds this
; address, which is what lets the reduced link be a load straight into the
; program counter with nothing to test.
; -------------------------------------------------------------------------
dynarec_j2_exit
        LDR     a2, [sp], #4
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
        STR     v1, [a2, #ST_MEM]
        STR     ip, [a2, #ST_TABLE]
        LDMFD   sp!, {v1-v6, fp, pc}

        J2_VARIANT 0, 0, v0
        J2_VARIANT 1, 0, v1
        J2_VARIANT 0, 1, v2
        J2_VARIANT 1, 1, v3

dynarec_j2_chain_end

        END
