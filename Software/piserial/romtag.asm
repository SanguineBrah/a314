; Resident tag for a314serial.device, and a fake segment list from which
; the device starts its worker process with CreateProc() (as in
; disk/romtag.asm).

    XDEF    _worker_seglist
    XREF    _worker_run

RTC_MATCHWORD:  equ    $4afc
RTF_AUTOINIT:   equ    (1<<7)
NT_DEVICE:      equ    3
VERSION:        equ    1
PRIORITY:       equ    0

        section    CODE,code

        moveq    #-1,d0
        rts

romtag:
        dc.w    RTC_MATCHWORD
        dc.l    romtag
        dc.l    endcode
        dc.b    RTF_AUTOINIT
        dc.b    VERSION
        dc.b    NT_DEVICE
        dc.b    PRIORITY
        dc.l    _device_name
        dc.l    _id_string
        dc.l    _auto_init_tables
endcode:

        cnop    0,4

        dc.l    16
_worker_seglist:
        dc.l    0
        jmp     _worker_run
