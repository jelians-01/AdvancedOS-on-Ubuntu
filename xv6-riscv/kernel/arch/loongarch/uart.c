#include "types.h"
#include "param.h"
#include "riscv.h"
#include "memlayout.h"
#include "defs.h"
#include "spinlock.h"
static struct spinlock txlock;
#define REG(n) (*(volatile uchar *)(UART0+(n)))
void uartinit(void) { REG(1)=0; REG(3)=3; REG(2)=7; initlock(&txlock,"uart"); }
void uartputc_sync(int c) {
  while(!(REG(5)&32)) ;
  REG(0)=c;
}
void uartwrite(char *buf,int n) {
  acquire(&txlock);
  for(int i=0;i<n;i++) uartputc_sync(buf[i]);
  release(&txlock);
}
int uartgetc(void) { return REG(5)&1 ? REG(0):-1; }
void uartintr(void) { int c; while((c=uartgetc())>=0) consoleintr(c); }
