/* @LICENSE(MUSLC_MIT) */

.text
.global dlsym
.type dlsym,@function
dlsym:
	mov (%rsp),%rdx
	jmp __dlsym
