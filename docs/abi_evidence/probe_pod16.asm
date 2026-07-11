
probe_pod16.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <ret_pod16i>:
   0:	mov    rax,rcx
   3:	mov    DWORD PTR [rcx],0x1
   9:	mov    DWORD PTR [rcx+0x4],0x2
  10:	mov    DWORD PTR [rcx+0x8],0x3
  17:	mov    DWORD PTR [rcx+0xc],0x4
  1e:	ret

000000000000001f <ret_pod16ptr>:
  1f:	mov    rax,rcx
  22:	mov    QWORD PTR [rcx],0x0
  29:	mov    QWORD PTR [rcx+0x8],0x0
  31:	ret

0000000000000032 <ret_pod8i>:
  32:	movabs rax,0x200000001
  3c:	ret

000000000000003d <take_pod16i>:
  3d:	mov    rax,QWORD PTR [rcx]
  40:	mov    rdx,QWORD PTR [rcx+0x8]
  44:	mov    rcx,rax
  47:	sar    rcx,0x20
  4b:	add    eax,ecx
  4d:	add    eax,edx
  4f:	sar    rdx,0x20
  53:	add    eax,edx
  55:	ret
  56:	nop
  57:	nop
  58:	nop
  59:	nop
  5a:	nop
  5b:	nop
  5c:	nop
  5d:	nop
  5e:	nop
  5f:	nop
