#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the T8142 production route/gate code against recorded MMIO writes.

--emit writes the standalone C harness for an ARM64/QEMU build. Registers
are mocked: this checks sequencing and isolation, not PHY or panel output.
"""
from pathlib import Path
import argparse, os, subprocess, tempfile
ap = argparse.ArgumentParser()
ap.add_argument('--emit', type=Path)
args = ap.parse_args()
root = Path(os.environ['KERNEL_SRC']) if 'KERNEL_SRC' in os.environ else next(
    p for p in Path(__file__).resolve().parents
    if (p/'drivers/mux/apple-display-crossbar.c').is_file())
s = (root/'drivers/mux/apple-display-crossbar.c').read_text()
structs = s[s.index('enum { MUX_DPPHY'):s.index('static u32 t602x_atc_bit')]
code = s[s.index('static const u8 t8142_dest_shift'):s.index('/*\n * Bring the clock/FIFO gates up')]
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
typedef uint32_t u32;
typedef uint8_t u8;
typedef int spinlock_t;
#define __iomem
#define BIT(n) (1U << (n))
#define WARN_ON(c) assert(!(c))
#define spin_lock_irqsave(l,f) do {(void)(l);(f)=0;}while(0)
#define spin_unlock_irqrestore(l,f) do {(void)(l);(void)(f);}while(0)
#define EXPORT_SYMBOL_GPL(s)
#define MUX_IDLE_DISCONNECT -2
struct device {};
struct mux_chip;
struct mux_control {struct mux_chip *chip;unsigned int index;};
struct mux_control_ops {int (*set)(struct mux_control *,int);};
struct mux_chip {const struct mux_control_ops *ops;void *priv;};
#define mux_chip_priv(c) ((c)->priv)
#define mux_control_get_index(m) ((m)->index)
static u32 registers[0x1000/4];
struct event {unsigned reg;u32 value;};
static struct event events[128];
static unsigned nevents;
static u32 readl(void *p) {return *(u32*)p;}
static void writel(u32 v,void *p) {
 assert(nevents<128);events[nevents++]=(struct event){(char*)p-(char*)registers,v};
 *(u32*)p=v;
}
static void udelay(unsigned us) {assert(us==1);}
'''
post = r'''
static struct apple_dpxbar xbar;
static struct mux_chip chip;
static struct mux_control mux[5];
static void reset(void) {
 memset(registers,0,sizeof(registers));memset(&xbar,0,sizeof(xbar));
 xbar.regs=registers;chip=(struct mux_chip){&apple_dpxbar_t8142_ops,&xbar};
 for(unsigned i=0;i<5;i++){xbar.selected_dispext[i]=-1;mux[i]=(struct mux_control){&chip,i};}
 nevents=0;
}
static void unchanged(int actual,int want,unsigned before) {assert(actual==want);assert(nevents==before);}
static void golden_sequence(void) {
 /* Native 27.0 90ca42c/90ca7f4/90caf64; ext3:0 -> DPPHY, PCLK3, UHBR.
  * Exact write order derived from firmware, including retained output-enable.
  */
 const struct event expected[]={
  {0x44,0x00600600},
  {0x04,0},{0x1c,0},{0x34,0},{0x08,0x40},
  {0x20,0x140000},{0x38,0x500},{0x48,4},{0x00,0x40},
  {0x14,0x40},{0x2c,0x100},{0x4c,0x100},{0x3c,0x40},
  {0x3c,0},{0x00,0},{0x14,0},{0x2c,0},{0x08,0},
  {0x20,0},{0x38,0},{0x04,0x40},{0x1c,0x40},{0x34,0x100}
 };
 reset();assert(!apple_dpxbar_set_t8142(&mux[0],6));
 assert(!apple_dpxbar_link_configure(&mux[0],3,true));
 assert(!apple_dpxbar_t8142_link(&mux[0],true));
 assert(!apple_dpxbar_t8142_link(&mux[0],false));
 assert(nevents==sizeof(expected)/sizeof(expected[0]));
 for(unsigned i=0;i<nevents;i++) {
  if(events[i].reg!=expected[i].reg || events[i].value!=expected[i].value)
   fprintf(stderr,"write %u: %x=%x expected %x=%x\n",i,events[i].reg,events[i].value,expected[i].reg,expected[i].value);
  assert(events[i].reg==expected[i].reg && events[i].value==expected[i].value);
 }
}
static void matrix(void) {
 /* Seed unrelated fields: selecting, enabling and releasing one route must
  * preserve them. The asymmetric DPIN3 fields catch signed 28-bit shifts.
  */
 const u32 masks[]={0x00f00f00,0x0000f00f,0x000f00f0,0xff000000,0x000000ff};
 const u32 shifts[]={8,0,4,24,0},shifts2[]={20,12,16,28,4};
 unsigned combinations=0;
 for(unsigned output=0;output<5;output++)for(int state=0;state<8;state++)
 for(unsigned clock=1;clock<=4;clock++)for(unsigned uhbr=0;uhbr<2;uhbr++) {
  reset();if(!output && (state&1)){unchanged(apple_dpxbar_set_t8142(&mux[output],state),-EINVAL,0);continue;}
  unsigned reg=output==4?0x40:0x44;registers[reg/4]=0xa5a5a5a5;
  assert(!apple_dpxbar_set_t8142(&mux[output],state));
  assert(registers[reg/4]==((0xa5a5a5a5&~masks[output])|((u32)state<<shifts[output])|((u32)state<<shifts2[output])));
  unchanged(apple_dpxbar_t8142_link(&mux[output],true),-EINVAL,nevents);
  assert(!apple_dpxbar_link_configure(&mux[output],clock,uhbr));
  registers[0x48/4]=0xf;
  assert(!apple_dpxbar_t8142_link(&mux[output],true));
  assert(registers[0x48/4]==(uhbr?0xf:(0xf&~BIT(clock-1))));
  unsigned n=nevents;
  unchanged(apple_dpxbar_t8142_link(&mux[output],true),0,n);
  unchanged(apple_dpxbar_link_configure(&mux[output],clock,uhbr),-EBUSY,n);
  assert(!apple_dpxbar_set_t8142(&mux[output],MUX_IDLE_DISCONNECT));
  assert(!xbar.enabled[output] && !xbar.pclk[output]);
  unchanged(apple_dpxbar_t8142_link(&mux[output],true),-ENODEV,nevents);
  combinations++;
 }
 assert(combinations==288);
}
static void isolation(void) {
 reset();assert(!apple_dpxbar_set_t8142(&mux[0],0));
 assert(!apple_dpxbar_link_configure(&mux[0],1,false));
 assert(!apple_dpxbar_t8142_link(&mux[0],true));
 unsigned n=nevents;
 unchanged(apple_dpxbar_set_t8142(&mux[4],0),-EBUSY,n);
 assert(!apple_dpxbar_set_t8142(&mux[4],7));
 assert(!apple_dpxbar_link_configure(&mux[4],1,true));
 unchanged(apple_dpxbar_t8142_link(&mux[4],true),-EBUSY,nevents);
 assert(!apple_dpxbar_link_configure(&mux[4],4,true));
 assert(!apple_dpxbar_t8142_link(&mux[4],true));
 assert(registers[0]==0x81 && registers[0x3c/4]==0x81);
 assert(!apple_dpxbar_set_t8142(&mux[4],MUX_IDLE_DISCONNECT));
 assert(registers[0]==1 && registers[0x3c/4]==1 && registers[0x0c/4]==1);
 assert(registers[0x24/4]==0x100 && registers[0x38/4]==0x100);
}
static void validation(void) {
 reset();
 unchanged(apple_dpxbar_link_configure(NULL,1,false),-EOPNOTSUPP,0);
 unchanged(apple_dpxbar_link_configure(&mux[0],1,false),-ENODEV,0);
 unchanged(apple_dpxbar_set_t8142(&mux[0],8),-EINVAL,0);
 unchanged(apple_dpxbar_set_t8142(&mux[0],-1),-EINVAL,0);
 assert(!apple_dpxbar_set_t8142(&mux[0],0));
 unchanged(apple_dpxbar_link_configure(&mux[0],0,false),-EINVAL,nevents);
 unchanged(apple_dpxbar_link_configure(&mux[0],5,false),-EINVAL,nevents);
}
int main(void) {
 (void)apple_dpxbar_names;
 golden_sequence();matrix();isolation();validation();
 puts("PASS: native T8142 write sequence, 288 route/clock/coding combinations, conflicts and output isolation");
 if(getpid()==1) {
  int fd=open("/mux-apple-display-crossbar.ko",O_RDONLY);
  if(fd<0 || syscall(SYS_finit_module,fd,"",0)) {perror("crossbar module");for(;;)pause();}
  close(fd);puts("AURORA_T8142_CROSSBAR_ABI_PASS");fflush(stdout);for(;;)pause();
 }
 return 0;
}
'''
source=pre+structs+code+post
if args.emit:
    args.emit.write_text(source)
else:
    with tempfile.TemporaryDirectory(prefix='apple-dpxbar-') as tmp:
        c=Path(tmp)/'test.c';exe=Path(tmp)/'test';c.write_text(source)
        subprocess.run([os.environ.get('HOSTCC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-O1','-g','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
