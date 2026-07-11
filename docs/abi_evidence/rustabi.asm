
rustabi.o:     file format pe-x86-64


Disassembly of section .text:

0000000000000000 <mix>:
   0:	addss  xmm0,DWORD PTR [rdx]
   4:	addss  xmm0,DWORD PTR [rdx+0x4]
   9:	addss  xmm0,DWORD PTR [rdx+0x8]
   e:	ret

Disassembly of section .text:

0000000000000000 <mix2>:
   0:	movss  xmm0,DWORD PTR [rcx]
   4:	addss  xmm0,DWORD PTR [rcx+0x4]
   9:	addss  xmm0,DWORD PTR [rcx+0x8]
   e:	addss  xmm0,xmm1
  12:	ret

Disassembly of section .text:

0000000000000000 <ret_DD>:
   0:	mov    rax,rcx
   3:	movaps xmm0,XMMWORD PTR [rip+0x0]        # a <ret_DD+0xa>
   a:	movups XMMWORD PTR [rcx],xmm0
   d:	ret

Disassembly of section .text:

0000000000000000 <ret_FF>:
   0:	movabs rax,0x400000003f800000
   a:	ret

Disassembly of section .text:

0000000000000000 <ret_II>:
   0:	movabs rax,0x200000001
   a:	ret

Disassembly of section .text:

0000000000000000 <ret_Mat4>:
   0:	mov    rax,rcx
   3:	xorps  xmm0,xmm0
   6:	movups XMMWORD PTR [rcx+0x30],xmm0
   a:	movups XMMWORD PTR [rcx+0x20],xmm0
   e:	movups XMMWORD PTR [rcx+0x10],xmm0
  12:	movups XMMWORD PTR [rcx],xmm0
  15:	ret

Disassembly of section .text:

0000000000000000 <ret_Vec3>:
   0:	mov    rax,rcx
   3:	movsd  xmm0,QWORD PTR [rip+0x0]        # b <ret_Vec3+0xb>
   b:	movsd  QWORD PTR [rcx],xmm0
   f:	mov    DWORD PTR [rcx+0x8],0x40400000
  16:	ret

Disassembly of section .text:

0000000000000000 <ret_Vec4>:
   0:	mov    rax,rcx
   3:	movaps xmm0,XMMWORD PTR [rip+0x0]        # a <ret_Vec4+0xa>
   a:	movups XMMWORD PTR [rcx],xmm0
   d:	ret

Disassembly of section .text:

0000000000000000 <take_DD>:
   0:	movsd  xmm0,QWORD PTR [rcx]
   4:	addsd  xmm0,QWORD PTR [rcx+0x8]
   9:	ret

Disassembly of section .text:

0000000000000000 <take_FF>:
   0:	movd   xmm1,ecx
   4:	shr    rcx,0x20
   8:	movd   xmm0,ecx
   c:	addss  xmm0,xmm1
  10:	ret

Disassembly of section .text:

0000000000000000 <take_ID>:
   0:	cvtsi2sd xmm0,DWORD PTR [rcx]
   4:	addsd  xmm0,QWORD PTR [rcx+0x8]
   9:	ret

Disassembly of section .text:

0000000000000000 <take_IF>:
   0:	cvtsi2ss xmm0,ecx
   4:	shr    rcx,0x20
   8:	movd   xmm1,ecx
   c:	addss  xmm0,xmm1
  10:	ret

Disassembly of section .text:

0000000000000000 <take_II>:
   0:	mov    rax,rcx
   3:	shr    rax,0x20
   7:	add    eax,ecx
   9:	ret

Disassembly of section .text:

0000000000000000 <take_Mat4>:
   0:	movss  xmm0,DWORD PTR [rcx]
   4:	addss  xmm0,DWORD PTR [rcx+0x3c]
   9:	ret

Disassembly of section .text:

0000000000000000 <take_Vec3>:
   0:	movss  xmm0,DWORD PTR [rcx]
   4:	addss  xmm0,DWORD PTR [rcx+0x4]
   9:	addss  xmm0,DWORD PTR [rcx+0x8]
   e:	ret

Disassembly of section .text:

0000000000000000 <take_Vec4>:
   0:	movss  xmm0,DWORD PTR [rcx]
   4:	addss  xmm0,DWORD PTR [rcx+0x4]
   9:	addss  xmm0,DWORD PTR [rcx+0x8]
   e:	addss  xmm0,DWORD PTR [rcx+0xc]
  13:	ret
