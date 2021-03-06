;|===========================================================================|
;|                                                                           |
;| MSXPi Interface                                                           |
;|                                                                           |
;| Version : 0.7                                                             |
;|                                                                           |
;| Copyright (c) 2015-2016 Ronivon Candido Costa (ronivon@outlook.com)       |
;|                                                                           |
;| All rights reserved                                                       |
;|                                                                           |
;| Redistribution and use in source and compiled forms, with or without      |
;| modification, are permitted under GPL license.                            |
;|                                                                           |
;|===========================================================================|
;|                                                                           |
;| This file is part of MSXPi Interface project.                             |
;|                                                                           |
;| MSX PI Interface is free software: you can redistribute it and/or modify  |
;| it under the terms of the GNU General Public License as published by      |
;| the Free Software Foundation, either version 3 of the License, or         |
;| (at your option) any later version.                                       |
;|                                                                           |
;| MSX PI Interface is distributed in the hope that it will be useful,       |
;| but WITHOUT ANY WARRANTY; without even the implied warranty of            |
;| MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             |
;| GNU General Public License for more details.                              |
;|                                                                           |
;| You should have received a copy of the GNU General Public License         |
;| along with MSX PI Interface.  If not, see <http://www.gnu.org/licenses/>. |
;|===========================================================================|
;
; File history :
; 0.7    : Replaced CHKPIRDY retries to $FFFF
;          Removed the RESET when PI is not responding. This is now responsability
;           of the calling function, which might opt to do something else.
; 0.6c   : Initial version commited to git
;

; Inlude file for other sources in the project
;
; ==================================================================
; BASIC I/O FUNCTIONS STARTS HERE.
; These are the lower level I/O routines available, and must match
; the I/O functions implemented in the CPLD.
; Other than using these functions you will have to create your
; own commands, using OUT/IN directly to the I/O ports.
; ==================================================================

;-----------------------
; SENDIFCMD_B          |
;-----------------------
SENDIFCMD_B:
            OUT     (6),A       ; Send data, or command
            RET
;-----------------------
; CHKPIRDY_B           |
;-----------------------
CHKPIRDY_B:
            LD      BC,0FFFFH
CHKPIRDY0_B:
            IN      A,(6)           ; Verify SPIRDY register on the MSXInterface
            OR	    A
            RET     Z               ; RDY signal is zero, Pi App FSM is ready for next command/byte
            DEC     BC              ; Pi not ready, wait a little bit
            LD      A,B
            OR      C
            JR      NZ,CHKPIRDY0_B
            SCF
CHKPIRDYOK_B:
            RET

;-----------------------
; READBYTE_B           |
;-----------------------
READBYTE_B:
            XOR     A           ; do not use XOR to preserve C flag state
            OUT     (6),A       ; Send READ command to the Interface
            JR      READPIBYTE_B

;-----------------------
; TRANSFBYTE_B         |
;-----------------------
TRANSFBYTE_B:
            PUSH    AF
            CALL    CHKPIRDY_B    ; registers A,BC and FLAGS are modified
            POP     AF
            OUT     (7),A       ; Send data, or command
READPIBYTE_B:
            CALL    CHKPIRDY_B      ;Wait Interface transfer data to PI and Pi App processing
            IN      A,(7)       ; read byte
            RET                 ; Return in A the byte received




