
probe_ret16.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <ret_pod16>:
   0:	mov    rax,rcx
   3:	mov    DWORD PTR [rcx],0x1
   9:	mov    DWORD PTR [rcx+0x4],0x2
  10:	mov    DWORD PTR [rcx+0x8],0x3
  17:	mov    DWORD PTR [rcx+0xc],0x4
  1e:	ret

000000000000001f <ret_pod16b>:
  1f:	mov    rax,rcx
  22:	mov    QWORD PTR [rcx],0x1
  29:	mov    QWORD PTR [rcx+0x8],0x2
  31:	ret

0000000000000032 <ret_podDD>:
  32:	mov    rax,rcx
  35:	mov    rdx,QWORD PTR [rip+0x0]        # 3c <ret_podDD+0xa>
  3c:	mov    QWORD PTR [rcx],rdx
  3f:	mov    rdx,QWORD PTR [rip+0x8]        # 4e <ret_big12+0x3>
  46:	mov    QWORD PTR [rcx+0x8],rdx
  4a:	ret

000000000000004b <ret_big12>:
  4b:	mov    rax,rcx
  4e:	mov    DWORD PTR [rcx],0x1
  54:	mov    DWORD PTR [rcx+0x4],0x2
  5b:	mov    DWORD PTR [rcx+0x8],0x3
  62:	ret

0000000000000063 <drive_ret>:
  63:	sub    rsp,0x68
  67:	lea    rcx,[rsp+0x50]
  6c:	call   0 <ret_pod16>
  71:	lea    rcx,[rsp+0x40]
  76:	call   1f <ret_pod16b>
  7b:	lea    rcx,[rsp+0x30]
  80:	call   32 <ret_podDD>
  85:	lea    rcx,[rsp+0x24]
  8a:	call   4b <ret_big12>
  8f:	mov    rax,QWORD PTR [rsp+0x50]
  94:	mov    rdx,QWORD PTR [rsp+0x58]
  99:	mov    rax,QWORD PTR [rsp+0x40]
  9e:	mov    rdx,QWORD PTR [rsp+0x48]
  a3:	mov    rax,QWORD PTR [rsp+0x30]
  a8:	mov    rdx,QWORD PTR [rsp+0x38]
  ad:	add    rsp,0x68
  b1:	ret
  b2:	nop
  b3:	nop
  b4:	nop
  b5:	nop
  b6:	nop
  b7:	nop
  b8:	nop
  b9:	nop
  ba:	nop
  bb:	nop
  bc:	nop
  bd:	nop
  be:	nop
  bf:	nop
