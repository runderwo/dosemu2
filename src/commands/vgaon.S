# vgaon.S, for dosemu 0.49, 4/16/93
# Robert Sanders, gt8134b@prism.gatech.edu
#
# this is dangerous!

.code16
.text
	.globl	_start16
_start16:

#include "detect.h"

	movb $3, %al    # dos helper I/O perms
	movw $0x70, %bx # starting port
	movw $2, %cx    # # of ports
	stc             # set means get I/Operms
	int $DOS_HELPER_INT
	movb $4, %al
	movb $1, %bl
	int $DOS_HELPER_INT
	movb $3, %al    # dos helper I/O perms
	movw $0x70, %bx # starting port
	movw $2, %cx    # # of ports
	clc            # clear means release I/Operms
	int $DOS_HELPER_INT
	int $0x20  # exit

# note: the VGA permissions are left on.


