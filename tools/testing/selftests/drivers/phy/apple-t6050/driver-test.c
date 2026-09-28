// SPDX-License-Identifier: GPL-2.0-only
/* Actual driver source with explicit QEMU-only power/MMIO acknowledgements. */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/soc/apple/tunable.h>
#include <linux/soc/apple/dp-tunnel.h>
#include <linux/vmalloc.h>
static bool fail_ack;
static int refs[3], events[32], nevents;
static struct device domains[3];
static void *reset_bank;
static int test_get(struct device *d)
{
 int i=d-domains;
 if(i<0 || i>2)return -EINVAL;
 if(i==2 && (readl(reset_bank)&3)!=3)return -EINVAL;
 refs[i]++; events[nevents++]=i+1;return 0;
}
static int test_put(struct device *d)
{
 int i=d-domains;
 if(i<0 || i>2 || refs[i]!=1)return -EINVAL;
 refs[i]--;events[nevents++]=-(i+1);return 0;
}
#undef readl_poll_timeout
#define readl_poll_timeout(addr,val,cond,delay,timeout) ({ \
 u32 old=readl(addr); writel((old & ~s->mask) | (fail_ack ? s->value^s->mask : s->value),addr); \
 val=readl(addr); (cond) ? 0 : -ETIMEDOUT; })
#define pm_runtime_resume_and_get test_get
#define pm_runtime_put_sync_suspend test_put
#undef module_platform_driver
#define module_platform_driver(x)
#undef EXPORT_SYMBOL_GPL
#define EXPORT_SYMBOL_GPL(x)
#define apple_t6050_dp_link_config test_link_config
#include "atc-t6050-dp.c"
#define CHECK(x) do {if(!(x)){pr_err("T6050 integrated check failed line%d: %s\n",__LINE__,#x);ret=-EINVAL;goto out;}}while(0)
static int __init test_init(void)
{
 struct t6050_dp dp={.rate=-1};struct phy phy={.ops=&t6050_dp_ops};
 union phy_configure_opts opts={};unsigned int pclk;bool uhbr;int ret=0;
 void *mem=vzalloc(44*0x10000);struct device *dev=root_device_register("t6050-model");
 if(!mem)return -ENOMEM;
 if(IS_ERR(dev)){vfree(mem);return PTR_ERR(dev);}
 dp.dev=dev;mutex_init(&dp.lock);phy_set_drvdata(&phy,&dp);
 for(int i=0;i<44;i++){dp.banks[i]=mem+i*0x10000;dp.resources[i].end=0x3fff;}
 reset_bank=dp.banks[39];
 for(int i=0;i<3;i++)dp.domain[i].dev=&domains[i];
 for(int i=0;i<4;i++)memcpy(dp.training[i],dp_training_default,sizeof(dp_training_default));
 CHECK(t6050_dp_validate(&phy,PHY_MODE_DP,0,&opts)==0 && opts.dp.link_rate==8100 && opts.dp.lanes==4);
 fail_ack=true;CHECK(t6050_dp_set_mode(&phy,PHY_MODE_DP,0)==-ETIMEDOUT);
 CHECK(dp.faulted && !dp.enabled && refs[0]==1 && refs[1]==1);
 CHECK(t6050_dp_set_mode(&phy,PHY_MODE_DP,0)==-EIO);
 memset(mem,0,44*0x10000);memset(refs,0,sizeof(refs));memset(dp.domain,0,sizeof(dp.domain));
 for(int i=0;i<3;i++)dp.domain[i].dev=&domains[i];
 dp.faulted=false;fail_ack=false;nevents=0;
 for(int cycle=0;cycle<2;cycle++){
  CHECK(t6050_dp_set_mode(&phy,PHY_MODE_DP,0)==0);
  CHECK(dp.enabled && refs[0]==1 && refs[1]==1 && refs[2]==1);
  CHECK(test_link_config(&phy,&pclk,&uhbr)==-ENOLINK);
  for(int r=0;r<4;r++){
   memset(&opts,0,sizeof(opts));opts.dp.set_rate=1;opts.dp.link_rate=link_rates[r];
   CHECK(t6050_dp_configure(&phy,&opts)==0 && dp.rate==r);
   CHECK(test_link_config(&phy,&pclk,&uhbr)==0 && pclk==1 && !uhbr);
   for(int v=0;v<4;v++)for(int p=0;p<4-v;p++){
    memset(&opts,0,sizeof(opts));opts.dp.set_voltages=1;opts.dp.lanes=4;
    for(int l=0;l<4;l++){opts.dp.voltage[l]=v;opts.dp.pre[l]=p;}
    CHECK(t6050_dp_configure(&phy,&opts)==0);
   }
   opts.dp.voltage[0]=4;CHECK(t6050_dp_configure(&phy,&opts)==-EINVAL);
   memset(&opts,0,sizeof(opts));opts.dp.set_rate=1;opts.dp.link_rate=123;
   CHECK(t6050_dp_configure(&phy,&opts)==-EINVAL && dp.rate==r);
  }
  CHECK(t6050_dp_set_mode(&phy,PHY_MODE_INVALID,0)==0);
  CHECK(!dp.enabled && dp.rate==-1 && !refs[0] && !refs[1] && !refs[2]);
  CHECK(events[cycle*6]==1 && events[cycle*6+1]==2 && events[cycle*6+2]==3);
  CHECK(events[cycle*6+3]==-3 && events[cycle*6+4]==-2 && events[cycle*6+5]==-1);
 }
 pr_info("AURORA_T6050_INTEGRATED_TEST_PASS: actual driver, four rates, two power cycles, drive settings, timeout and validation\n");
out: root_device_unregister(dev);vfree(mem);return ret;
}
static void __exit test_exit(void){}
module_init(test_init);module_exit(test_exit);
