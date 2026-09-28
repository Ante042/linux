#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Actual pcIO serializer and GPIO IRQ group routing, with native byte fixtures."""
from pathlib import Path
import os,subprocess,tempfile
root=next(p for p in Path(__file__).resolve().parents if (p/'drivers/gpio/gpio-macsmc.c').exists())
gpio=(root/'drivers/gpio/gpio-macsmc.c').read_text();irq=(root/'drivers/pinctrl/pinctrl-apple-gpio.c').read_text()
smc=(root/'drivers/mfd/macsmc.c').read_text()
a=smc.index('int apple_smc_get_key_info(');metadata=smc[a:smc.index('\n}',a)+2]
def func(s,name):
 a=s.index(name+'(');a=s.rfind('\nstatic ',0,a)+1;b=s.index('\n}',a)+2;return s[a:b]
pre=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
typedef uint8_t u8;typedef uint32_t u32;typedef uint64_t u64;
#define SMC_KEY(x) 0x7063494f
#define GPIO_LINE_DIRECTION_OUT 0
#define BIT(n) (1UL<<(n))
#define REG_GPIOx_GRP 0x70000
#define FIELD_PREP(mask,v) ((v)<<__builtin_ctz(mask))
#define FIELD_GET(mask,v) (((v)&(mask))>>__builtin_ctz(mask))
#define REG_IRQ(g,x) (0x800+0x40*(g)+4*((x)>>5))
struct gpio_chip{void*data;unsigned ngpio;struct{void*domain;}irq;};
static void*gpiochip_get_data(struct gpio_chip*g){return g->data;}
typedef u32 smc_key;
#define SMC_MSG_GET_KEY_INFO 0x13
struct apple_smc{bool mutex;struct{void*iomem;}shmem;};
struct apple_smc_key_info{u32 type_code;u8 size,flags;};
struct mock_guard{bool*held;};
static struct mock_guard lock(bool*p){assert(!*p);*p=true;return (struct mock_guard){p};}
static void unlock(struct mock_guard*g){assert(*g->held);*g->held=false;}
#define guard(type) struct mock_guard __attribute__((cleanup(unlock))) g=lock
static struct apple_smc*metadata_smc;static int metadata_result;
static int apple_smc_cmd_locked(struct apple_smc*s,unsigned cmd,smc_key key,unsigned a,unsigned b,void*p){assert(s->mutex&&cmd==0x13);return metadata_result;}
static void memcpy_fromio(void*d,void*s,size_t n){assert(metadata_smc->mutex);memcpy(d,s,n);}
static u32 get_unaligned_be32(void*p){u8*b=p;return (u32)b[0]<<24|(u32)b[1]<<16|(u32)b[2]<<8|b[3];}
struct macsmc_gpio{struct apple_smc*smc;u64*output_commands;};
static u8 sent[8];static int smc_result;
static void put_unaligned_le64(u64 v,void*p){for(int i=0;i<8;i++)((u8*)p)[i]=v>>(8*i);}
static int apple_smc_write(struct apple_smc*s,u32 key,const void*p,size_t n){assert(key==0x7063494f&&n==8);memcpy(sent,p,8);return smc_result;}
struct apple_gpio_pinctrl{void*base;u8 irq_group_base;struct gpio_chip gpio_chip;u32 pins[85];};
struct apple_gpio_irq_group{struct apple_gpio_pinctrl*pctl;u8 index;};
struct irq_data{struct gpio_chip*chip;unsigned hwirq;};
struct irq_chip{};
struct irq_desc{void*data;struct irq_chip chip;};
static void*irq_data_get_irq_chip_data(struct irq_data*d){return d->chip;}
static void*irq_desc_get_handler_data(struct irq_desc*d){return d->data;}
static struct irq_chip*irq_desc_get_chip(struct irq_desc*d){return &d->chip;}
static void apple_gpio_set_reg(struct apple_gpio_pinctrl*p,unsigned pin,u32 mask,u32 v){p->pins[pin]=(p->pins[pin]&~mask)|v;}
static u32 apple_gpio_get_reg(struct apple_gpio_pinctrl*p,unsigned pin){return p->pins[pin];}
static int inputs,unmasks,enters,exits;
static void apple_gpio_direction_input(struct gpio_chip*g,unsigned pin){inputs++;}
static void apple_gpio_irq_unmask(struct irq_data*d){unmasks++;}
static void chained_irq_enter(struct irq_chip*c,struct irq_desc*d){enters++;}
static void chained_irq_exit(struct irq_chip*c,struct irq_desc*d){exits++;}
static u32 readl_relaxed(void*p){return *(u32*)p;}
static void*ack_address;static u32 ack_value;
static void writel(u32 v,void*p){ack_address=p;ack_value=v;}
static unsigned handled[3],nhandled;
static void generic_handle_domain_irq(void*d,unsigned pin){assert(nhandled<3);handled[nhandled++]=pin;}
#define for_each_set_bit(bit,p,n) for((bit)=0;(bit)<(n);(bit)++)if(*(p)&BIT(bit))
'''
source=pre+metadata+func(gpio,'macsmc_pcio_set')+func(gpio,'macsmc_pcio_get')+func(gpio,'macsmc_pcio_get_direction')+func(irq,'apple_gpio_irq_startup')+func(irq,'apple_gpio_irq_ack')+func(irq,'apple_gpio_irq_handler')+r'''
int main(void){
u64 commands[]={0x001a000000000000ULL,0x000f000000800000ULL};struct apple_smc smc;
struct macsmc_gpio m={&smc,commands};struct gpio_chip gc={.data=&m};
/* Fixtures follow the captured native pKW8 copy + low16 replacement. */
const u8 expected[2][8]={{1,0,0,0,0,0,0x1a,0},{1,0,0x80,0,0,0,0x0f,0}};
for(int i=0;i<2;i++){
 smc_result=0;assert(!macsmc_pcio_set(&gc,i,1)&&!memcmp(sent,expected[i],8));
 assert(!macsmc_pcio_set(&gc,i,0)&&!sent[0]&&!sent[1]&&!memcmp(sent+2,expected[i]+2,6));
 smc_result=8;assert(!macsmc_pcio_set(&gc,i,1));
 smc_result=-EIO;assert(macsmc_pcio_set(&gc,i,1)==-EIO);
}
assert(macsmc_pcio_get(&gc,0)==-EOPNOTSUPP&&macsmc_pcio_get_direction(&gc,0)==GPIO_LINE_DIRECTION_OUT);
for(int g=0;g<=6;g++){
 u32 mmio[0x1000/4]={0};struct apple_gpio_pinctrl p={.base=mmio,.irq_group_base=g};
 p.gpio_chip.data=&p;p.gpio_chip.ngpio=85;
 struct irq_data d={&p.gpio_chip,14};p.pins[14]=0xa5a5a5a5;
 assert(!apple_gpio_irq_startup(&d)&&FIELD_GET(REG_GPIOx_GRP,p.pins[14])==(unsigned)g);
 assert((p.pins[14]&~REG_GPIOx_GRP)==(0xa5a5a5a5&~REG_GPIOx_GRP));
 apple_gpio_irq_ack(&d);assert(ack_address==(void*)mmio+REG_IRQ(g,14)&&ack_value==BIT(14));
 mmio[REG_IRQ(g,14)/4]=BIT(14);mmio[REG_IRQ(g,50)/4]=BIT(18);mmio[REG_IRQ(g,84)/4]=BIT(20);
 struct apple_gpio_irq_group group={&p,g};struct irq_desc desc={.data=&group};nhandled=0;
 apple_gpio_irq_handler(&desc);assert(nhandled==3&&handled[0]==14&&handled[1]==50&&handled[2]==84);
}
assert(inputs==7&&unmasks==7&&enters==7&&exits==7);
u8 info_bytes[]={8,'u','i','6','4',0x40};struct apple_smc_key_info info;
smc.mutex=false;smc.shmem.iomem=info_bytes;metadata_smc=&smc;metadata_result=6;
assert(apple_smc_get_key_info(&smc,0x7063494f,&info)==6&&!smc.mutex);
assert(info.size==8&&info.flags==0x40&&info.type_code==0x75693634);
metadata_result=-EIO;info.size=17;assert(apple_smc_get_key_info(&smc,0x7063494f,&info)==-EIO&&!smc.mutex&&info.size==17);
metadata_result=0;assert(!apple_smc_get_key_info(&smc,0x7063494f,NULL)&&!smc.mutex);
puts("APPLE_HDMI_CONTROLS_TEST_PASS: native pcIO bytes, errors, IRQ groups0-6, acknowledgement routing, locked SMC metadata copy");
return 0;
}
'''
if os.environ.get('TEST_SOURCE'):Path(os.environ['TEST_SOURCE']).write_text(source)
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(source)
 subprocess.run(['clang','-std=gnu11','-O1','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
