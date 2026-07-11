
rustabi2.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <ret_pod16i>:
   0:	mov    rax,rcx
   3:	movaps xmm0,XMMWORD PTR [rip+0x0]        # a <ret_pod16i+0xa>
   a:	movups XMMWORD PTR [rcx],xmm0
   d:	ret

Disassembly of section .text:

0000000000000000 <ret_pod16ptr>:
   0:	mov    rax,rcx
   3:	xorps  xmm0,xmm0
   6:	movups XMMWORD PTR [rcx],xmm0
   9:	ret

Disassembly of section .text:

0000000000000000 <ret_pod24>:
   0:	mov    rax,rcx
   3:	mov    DWORD PTR [rcx],0x1
   9:	movabs rcx,0x4000000000000000
  13:	mov    QWORD PTR [rax+0x8],rcx
  17:	mov    WORD PTR [rax+0x10],0x3
  1d:	ret

Disassembly of section .text:

0000000000000000 <ret_pod8i>:
   0:	movabs rax,0x200000001
   a:	ret

Disassembly of section .text:

0000000000000000 <take_pod16i>:
   0:	movdqu xmm0,XMMWORD PTR [rcx]
   4:	pshufd xmm1,xmm0,0xee
   9:	paddd  xmm1,xmm0
   d:	pshufd xmm0,xmm1,0x55
  12:	paddd  xmm0,xmm1
  16:	movd   eax,xmm0
  1a:	ret
