.386

_DATA   SEGMENT BYTE PUBLIC USE32 'DATA'
_DATA   ENDS

DGROUP GROUP _DATA

_TEXT   SEGMENT BYTE PUBLIC USE32 'CODE'
        ASSUME  cs:_TEXT

        PUBLIC  MyReproThunk_
        PUBLIC  MyLatLonHook_

        ;--- C globals (cclinedump.c) -----------------------------
        EXTRN   _g_capEDI       :dword
        EXTRN   _g_real787E7    :dword
        ;--- C functions (cclinedump.c) ---------------------------
        EXTRN   CaptureRepro_   :near
        EXTRN   CaptureLatLon_  :near

;-----------------------------------------------------------------------------
; MyReproThunk
;
; Tail-call trampoline patched over "call sub_787E7" at IDA 0x78F98 inside
; UACalcBestLine. On entry EDI = the track segment being processed and the
; four cc-line globals are live. We stash EDI, snapshot the globals via the C
; helper CaptureRepro (which must NOT disturb the segment-loop registers), then
; tail-jump into the real sub_787E7. Because we were entered through the
; original CALL, sub_787E7's own RET returns straight back into UACalcBestLine,
; so the loop is undisturbed.
;
; ALL registers and flags are preserved across the C call.
;-----------------------------------------------------------------------------
MyReproThunk_:
                mov     ds:_g_capEDI,edi        ; stash segment ptr for CaptureRepro
                pushfd
                pushad
                call    CaptureRepro_
                popad
                popfd
                jmp     dword ptr ds:_g_real787E7   ; tail-jump to real sub_787E7

;-----------------------------------------------------------------------------
; MyLatLonHook
;
; Detour patched over "mov ax, dActCCLineArg2" (66 A1 B4 94 0C 00) at IDA
; 0x78A41 inside sub_787E7 -- the point just after lat/lon and the q30 cos/sin
; of the segment frame are computed. We snapshot those intermediates via
; CaptureLatLon, then leave AX = dActCCLineArg2 (CaptureLatLon's return value),
; exactly re-doing the instruction we replaced, so the following
; "or ax, dActCCLineArg2+2 / jnz" zero-test continues unchanged.
;
; The replaced instruction only writes AX, so we just preserve the other
; caller-clobbered regs (EBX/ECX/EDX -- ESI/EDI/EBP are __watcall callee-saved)
; and flags. AX is intentionally set from the C return; we do NOT pushad here
; (that would overwrite the return value).
;-----------------------------------------------------------------------------
MyLatLonHook_:
                pushfd
                push    ebx
                push    ecx
                push    edx
                call    CaptureLatLon_          ; -> EAX = dActCCLineArg2
                pop     edx
                pop     ecx
                pop     ebx
                popfd
                ret                             ; AX (=arg2 low) re-does the mov

_TEXT   ENDS
        END
