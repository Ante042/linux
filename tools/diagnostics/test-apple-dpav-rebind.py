#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run actual AFK announcements, ownership and DPAV admission across epochs.

Wire frames and the property interpreter are production code; the binary
property decoder receives a synthetic dictionary. Locks, DMA and OPEN sending
are host mocks. This does not qualify firmware, concurrency or display output.
"""
from pathlib import Path
import argparse, hashlib, os, shlex, subprocess, tempfile
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[2])
root = parser.parse_args().source_root.resolve()
base = root / 'drivers/gpu/drm/apple'
def function(file,signature):
    text=(base/file).read_text();start=text.index(signature);opening=text.index('{',start)
    while ';' in text[start:opening]:
        start=text.index(signature,start+1);opening=text.index('{',start)
    end,depth=opening+1,1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
prefix=r'''
#undef NDEBUG
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "afkv2-codec.h"
typedef int64_t s64;
typedef u64 dma_addr_t;
#define U16_MAX UINT16_MAX
#define EBUSY 16
#define ENOENT 2
#define MAX_PENDING_CMDS 16
#define AFK_MAX_CHANNEL 16
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x,v) ((x)=(v))
#define WARN_ON(x) (x)
#define bitmap_empty(p,n) (!*(p))
#define spin_lock_irqsave(p,f) do { assert(!*(p));*(p)=true;(f)=0; } while(0)
#define spin_unlock_irqrestore(p,f) do { (void)(f);assert(*(p));*(p)=false; } while(0)
static int test_bit(unsigned int bit,const unsigned long *p) { return !!(*p & (1UL<<bit)); }
static void bitmap_release_region(unsigned long *p,unsigned int bit,int order) { (void)order;*p&=~(1UL<<bit); }
#define le32_to_cpu(x) (x)
#define cpu_to_le32(x) (x)
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_info(...) ((void)0)
#define DCP_FIRMWARE_H17P 17
#define EPIC_TYPE_NOTIFY 0
#define EPIC_CAT_REPORT 0
#define max(a,b) ((a)>(b)?(a):(b))
#define strscpy(dst,src,n) snprintf(dst,n,"%s",(char *)(src))
#define trace_dcpavserv_init(...) ((void)0)
#define dma_rmb() ((void)0)
#define DRM_CONNECTOR_REGISTERED 1
#define DPAVSERV_ENDPOINT_V2 0x2a
#define DCP_IOMFB_METHODS_H17P_EXTERNAL_EXPERIMENTAL 2
#define EPIC_SUBTYPE_STD_SERVICE 0xc0
#define EPIC_SUBTYPE_ANNOUNCE_H17P 0x11
#define to_platform_device(x) (x)
#define ERR_PTR(x) ((void *)(intptr_t)(x))
#define IS_ERR(x) ((uintptr_t)(x)>(uintptr_t)-4096)
#define lockdep_assert_held(p) assert(*(p))
static bool *hpd_lock,*av_lock;
static void mutex_lock(bool *p) { if(p==hpd_lock) assert(!*av_lock);assert(!*p);*p=true; }
static void mutex_unlock(bool *p) { assert(*p);*p=false; }
static void complete(int *p) { (*p)++; }
static void reinit_completion(int *p) { *p=0; }
static void schedule_work(unsigned int *p) { (*p)++; }
struct apple_dcp;struct apple_dcp_afkep;struct apple_epic_service;
struct apple_epic_service_ops { char name[32];bool reusable;void (*init)(struct apple_epic_service *,const char *,const char *,s64);int (*prepare)(struct apple_epic_service *,const char *,const char *,s64);void (*teardown)(struct apple_epic_service *); };
struct epic_cmd_info { u16 tag;u8 type;size_t reply_length;void *rxbuf,*txbuf;dma_addr_t rxbuf_dma,txbuf_dma;size_t rxlen,txlen;u32 retcode;bool done,free_on_ack;int *completion; };
struct apple_epic_service {
    struct apple_dcp_afkep *ep;const struct apple_epic_service_ops *ops;
    bool lock,enabled,torndown;unsigned int users;u32 channel;u8 cmd_tag;
    unsigned long cmd_map[1];struct epic_cmd_info cmds[MAX_PENDING_CMDS];
    struct { u64 generation,epoch; } admission;void *cookie;
};
struct apple_dcp_afkep { struct apple_dcp *dcp;const struct apple_epic_service_ops *ops;bool wire_v2,inline_v2,match_epic_name;u32 endpoint;unsigned int num_channels;struct apple_epic_service services[AFK_MAX_CHANNEL]; };
struct apple_connector { void *dcp;bool connected,edid_invalidation_pending;struct { int registration_state; } base;unsigned int hotplug_wq; };
struct apple_dcp {
    void *dev;int fw_compat;struct apple_dcp_afkep *dcpavservep;struct apple_connector *connector;
    bool hpd_mutex,quiescing,typec_cable_connected;u64 typec_generation;unsigned int nr_modes;
    struct { int iomfb_method_profile; } hw;
    struct { bool lock,enabled;u64 epoch;int enable_completion;struct apple_epic_service *service; } dcpavserv;
};
struct epic_cmd { u32 retcode;u64 rxbuf,txbuf;u32 rxlen,txlen; };
static unsigned int dma_frees,opens;
static int open_error,open_action;
void dpavservep_detach(struct apple_dcp *dcp);
static void inject_owner(struct apple_dcp_afkep *ep,u32 pending_channel);
static void dma_free_coherent(void *dev,size_t size,void *p,dma_addr_t address)
{ (void)dev;assert(size && (uintptr_t)p==address);free(p);dma_frees++; }
static void afk_remove_service_debugfs(struct apple_epic_service *s) { (void)s; }
static void afk_populate_service_debugfs(struct apple_epic_service *s) { (void)s; }
static int afk_send_epic(struct apple_dcp_afkep *e,u32 channel,u16 tag,int type,int category,u8 subtype,const void *p,size_t n)
{
    assert(channel && !tag && type==EPIC_TYPE_NOTIFY && category==EPIC_CAT_REPORT && subtype==AFKV2_OPEN && !p && !n);
    opens++;
    assert(!e->dcp->dcpavserv.enabled && !e->dcp->dcpavserv.service);
    for(unsigned int i=0;i<e->num_channels;i++) {
        struct apple_epic_service *s=&e->services[i];
        if(s->channel==channel && !s->torndown) assert(!s->enabled && !s->users);
    }
    if(open_action==1) e->dcp->typec_generation++;
    if(open_action==2) dpavservep_detach(e->dcp);
    if(open_action==4) inject_owner(e,channel);
    if(open_action==5) e->dcp->typec_cable_connected=false;
    if(open_action==6) e->dcp->quiescing=true;
    if(open_action==3) {
        for(unsigned int i=0;i<e->num_channels;i++)
            if(e->services[i].channel==channel && !e->services[i].torndown) e->services[i].channel++;
    }
    return open_error;
}
static void afk_v2_callback(struct apple_dcp_afkep *e,const struct afkv2_frame *f,const struct afkv2_command *c) { (void)e;(void)f;(void)c;assert(!"unexpected callback"); }

struct item { const char *key,*value; s64 integer; };
struct dcp_parse_ctx { struct item *items;unsigned int count,index;bool key; };
struct iterator { struct dcp_parse_ctx *handle;unsigned int idx,len; };
#define kfree(p) free((void *)(p))
#define PTR_ERR(p) ((int)(intptr_t)(p))
static struct item announcement_items[3];
static int parse(u8 *p,size_t n,struct dcp_parse_ctx *ctx)
{ (void)p;assert(n==37);*ctx=(struct dcp_parse_ctx){.items=announcement_items,.count=3};return 0; }
static int iterator_begin(struct dcp_parse_ctx *h,struct iterator *it,bool dict)
{ assert(dict);h->index=0;h->key=true;*it=(struct iterator){.handle=h,.len=h->count};return 0; }
static char *parse_string(struct dcp_parse_ctx *h)
{
    struct item *i=&h->items[h->index];const char *p;
    if(h->key) { p=i->key;h->key=false; } else { p=i->value;h->index++;h->key=true; }
    if(!p) return ERR_PTR(-EINVAL);char *copy=malloc(strlen(p)+1);assert(copy);strcpy(copy,p);return copy;
}
static int parse_int(struct dcp_parse_ctx *h,s64 *out)
{ assert(!h->key);*out=h->items[h->index++].integer;h->key=true;return 0; }
static int skip(struct dcp_parse_ctx *h) { assert(!h->key);h->index++;h->key=true;return 0; }
'''
parts=[function('parser.c','int parse_epic_service_init('),function('afk.h','static inline struct apple_epic_service *afk_service_get('),
       function('afk.h','static inline void afk_service_put('),function('afk.h','static inline void afk_service_retire('),function('afk.h','static inline void afk_service_disable('),
       function('afk.h','static inline bool afk_service_matches('),function('afk.h','static inline bool afk_service_reclaimable('),
       function('afk.h','static inline bool\nafk_service_reinit('),function('afk.h','static inline void afk_service_discard('),
       function('afk.h','static inline void afk_service_publish('),
       function('epic/dpavservep.c','static void dcpavserv_notify_edid('),function('epic/dpavservep.c','static bool dcpavserv_identity('),
       function('epic/dpavservep.c','static bool dcpavserv_admission_current('),function('epic/dpavservep.c','static int dcpavserv_prepare('),
       function('epic/dpavservep.c','static void dcpavserv_init('),function('epic/dpavservep.c','static void dcpavserv_invalidate_edid('),
       function('epic/dpavservep.c','static void dcpavserv_teardown('),function('epic/dpavservep.c','void dpavservep_detach('),
       function('afk.c','static const struct apple_epic_service_ops *\nafk_match_service('),
       function('afk.c','static struct apple_epic_service *afk_epic_find_service('),
       function('afk.c','static void afk_recv_handle_init('),
       function('epic/dpavservep.c','static int dcpdpserv_prepare('),
       function('epic/dpavservep.c','static void dcpdpserv_teardown('),function('afk.c','static void afk_recv_handle_teardown('),
       function('afk.c','static void afk_recv_handle_reply('),function('afk.c','static void afk_v2_receive(')]
print('Production lifecycle functions SHA256:', hashlib.sha256('\n'.join(parts).encode()).hexdigest(), flush=True)
suffix=r'''
static struct apple_dcp d;
static struct apple_dcp_afkep ep;
static struct apple_connector con;
static const struct apple_epic_service_ops ops={.name="dcpav-service-epic",.reusable=true,.prepare=dcpavserv_prepare,.init=dcpavserv_init,.teardown=dcpavserv_teardown};
static const struct apple_epic_service_ops announcement_ops[]={
    {.name="dcpav-service-epic",.reusable=true,.prepare=dcpavserv_prepare,.init=dcpavserv_init,.teardown=dcpavserv_teardown},
    {.name="dcpdp-service-epic",.reusable=true,.prepare=dcpdpserv_prepare,.teardown=dcpdpserv_teardown},
    {0}
};
static const char *name="dcpav-service-epic",*class="AppleDCPDP2HDMI";
static const char *native_class="DCPDP13Service";
static struct apple_epic_service *prepare(u32 channel,s64 unit,const char *n,const char *c)
{
    for(unsigned int i=0;i<AFK_MAX_CHANNEL;i++) if(afk_service_reinit(&ep.services[i],&ep,&ops,channel)) {
        struct apple_epic_service *s=&ep.services[i];
        if(dcpavserv_prepare(s,n,c,unit)) { afk_service_discard(s);return NULL; }
        return s;
    }
    assert(!"no reusable slot");return NULL;
}
static void publish(struct apple_epic_service *s,s64 unit)
{ afk_service_publish(s);dcpavserv_init(s,name,class,unit); }
static void disconnect(void)
{
    mutex_lock(&d.hpd_mutex);d.typec_generation++;d.typec_cable_connected=false;mutex_unlock(&d.hpd_mutex);
    dpavservep_detach(&d);
}
static void connect(void)
{ mutex_lock(&d.hpd_mutex);d.typec_generation++;d.typec_cable_connected=true;mutex_unlock(&d.hpd_mutex); }
static void closed(u16 interface,u16 flags,u8 result)
{
    u8 packet[25];size_t written;
    assert(!afkv2_encode_frame(packet,sizeof(packet),1,interface,0,AFKV2_CLOSED,AFKV2_REPORT,flags,&result,1,&written));
    afk_v2_receive(&ep,0,0,packet,written);
}
static void announce(u16 channel,const char *compartment,const char *provider,s64 unit)
{
    u8 payload[69]={0},packet[93];size_t written;
    memcpy(payload,"synthetic",10);
    announcement_items[0]=(struct item){"compartment",compartment,0};
    announcement_items[1]=(struct item){"name",provider,0};
    announcement_items[2]=(struct item){"interface-id",NULL,unit};
    assert(!afkv2_encode_frame(packet,sizeof(packet),1,channel,0,EPIC_SUBTYPE_ANNOUNCE_H17P,AFKV2_REPORT,0,payload,sizeof(payload),&written));
    afk_v2_receive(&ep,0,0,packet,written);
}
static struct apple_epic_service *injected_owner;
static void inject_owner(struct apple_dcp_afkep *e,u32 pending_channel)
{
    assert(e==&ep);
    injected_owner=prepare(pending_channel+2,pending_channel+2,name,class);assert(injected_owner);
    publish(injected_owner,pending_channel+2);
    assert(d.dcpavserv.service==injected_owner && injected_owner->users==1);
}
static void repeated_announcements(void)
{
    ep.ops=announcement_ops;ep.match_epic_name=true;ep.num_channels=0;d.fw_compat=DCP_FIRMWARE_H17P;
    /* More than the entire slot budget; wrap channel identity as well. */
    for(unsigned int i=0;i<4*AFK_MAX_CHANNEL;i++) {
        u16 channel=1+4*(i%4);unsigned int before=opens;
        /* Flip, same-attachment revalidation, return flip; swap both providers. */
        bool native=((i%4==1 || i%4==2) != !!((i/4)%2));
        const char *provider=native?native_class:class;
        announce(channel,name,provider,channel);
        struct apple_epic_service *owner=d.dcpavserv.service;
        if(native) assert(owner && "native DP EDID provider must publish");
        assert(owner && owner->channel==channel && owner->users==1 && opens==before+1);
        assert(ep.num_channels<=2); /* Real allocator reuses, no slot exhaustion. */
        u64 epoch=d.dcpavserv.epoch;
        announce(channel,name,provider,channel); /* Active duplicate: no extra OPEN/pin. */
        assert(opens==before+1 && owner->users==1 && d.dcpavserv.epoch==epoch);
        announce(channel+2,"dcpdp-service-epic",provider,channel+2);
        assert(opens==before+1 && d.dcpavserv.service==owner && d.dcpavserv.epoch==epoch);
        if(i%4==1) {
            u64 generation=d.typec_generation;
            closed(channel,0x6a01,0); /* Revalidation retires service, not attachment. */
            assert(!d.dcpavserv.service && d.typec_generation==generation && d.dcpavserv.epoch>epoch);
        } else {
            disconnect();connect();
        }
    }
    assert(ep.num_channels==2); /* Auxiliary rejection leaves a reusable slot. */
    unsigned int before=opens;
    announce(13,name,"synthetic-wrong-class",13);assert(opens==before && !d.dcpavserv.service);
    char long_class[161];memset(long_class,'x',160);long_class[160]=0;
    announce(13,name,long_class,13);assert(opens==before && !d.dcpavserv.service);
    for(unsigned int variant=0;variant<2;variant++) {
        const char *provider=variant?native_class:class;
        assert(!prepare(0,0,name,provider));assert(!prepare(65536,65536,name,provider));
        assert(!prepare(13,-1,name,provider));assert(!prepare(13,0,name,provider));
        assert(!prepare(13,1,name,provider));assert(!prepare(13,13,NULL,provider));
        assert(!prepare(13,13,"dcpdp-service-epic",provider));
        announce(13,"wrong-compartment",provider,13);assert(opens==before && !d.dcpavserv.service);
        ep.endpoint=0x2c;announce(13,name,provider,13);assert(opens==before && !d.dcpavserv.service);ep.endpoint=0x2a;
        d.quiescing=true;announce(13,name,provider,13);assert(opens==before && !d.dcpavserv.service);d.quiescing=false;
        disconnect();announce(13,name,provider,13);assert(opens==before && !d.dcpavserv.service);connect();
    }
    for(unsigned int variant=0;variant<4;variant++) {
        const char *forged[]={"DCPDP13ServiceExtra","dcpdp13service","AppleDCPDP2HDMIExtra","unrelated-provider"};
        announce(13,name,forged[variant],13);assert(opens==before && !d.dcpavserv.service);
    }
    assert(!prepare(13,13,name,NULL));
    open_error=-EIO;announce(13,name,native_class,13);assert(opens==++before && !d.dcpavserv.service);open_error=0;
    for(unsigned int window=0;window<5;window++) {
        const int actions[]={1,2,3,5,6};open_action=actions[window];
        announce(13,name,native_class,13);
        assert(opens==++before && !d.dcpavserv.service && !d.dcpavserv.enabled);
        d.typec_cable_connected=true;d.quiescing=false;
    }
    open_action=4;announce(13,name,native_class,13);assert(opens==++before && d.dcpavserv.service==injected_owner);
    assert(injected_owner->users==1);dpavservep_detach(&d);assert(!injected_owner->users);
    open_action=0;announce(13,name,native_class,13);assert(opens==++before && d.dcpavserv.service);
    struct apple_epic_service *old=d.dcpavserv.service;
    assert(afk_service_get(old)==old);old->cmd_map[0]=1;
    disconnect();connect();
    announce(13,name,native_class,13); /* Same channel still owns a pending reply. */
    assert(opens==before && !d.dcpavserv.service && old->users==1 && old->channel==13);
    announce(1,name,class,1);assert(opens==++before && d.dcpavserv.service!=old);
    struct apple_epic_service *fresh=d.dcpavserv.service;u64 epoch=d.dcpavserv.epoch;
    dcpavserv_teardown(old);assert(d.dcpavserv.service==fresh && d.dcpavserv.epoch==epoch);
    old->cmd_map[0]=0;afk_service_put(old);disconnect();connect();
    announce(13,name,native_class,13);assert(opens==++before && d.dcpavserv.service);
    struct apple_epic_service *owner=d.dcpavserv.service;
    u64 owner_epoch=d.dcpavserv.epoch;
    announce(5,name,class,5); /* A second admitted identity cannot displace the owner. */
    assert(opens==before && d.dcpavserv.service==owner && d.dcpavserv.epoch==owner_epoch);
    dpavservep_detach(&d);
    /* All slots held by retired readers: fail safely, then recover after puts. */
    for(unsigned int i=0;i<AFK_MAX_CHANNEL;i++) {
        struct apple_epic_service *s=&ep.services[i];
        assert(afk_service_reinit(s,&ep,&ops,100+i));afk_service_publish(s);
        assert(afk_service_get(s)==s);afk_service_retire(s);
    }
    announce(13,name,native_class,13);assert(opens==before && !d.dcpavserv.service);
    for(unsigned int i=0;i<AFK_MAX_CHANNEL;i++) afk_service_put(&ep.services[i]);
    announce(13,name,native_class,13);assert(opens==++before && d.dcpavserv.service);
    dpavservep_detach(&d);
    assert(!d.hpd_mutex && !d.dcpavserv.lock);
    /* Legacy remains unit-zero and class-agnostic, with its original epoch fence. */
    ep.wire_v2=false;ep.endpoint=0x28;
    assert(!prepare(13,13,name,native_class));
    struct apple_epic_service *legacy=prepare(13,0,name,native_class);assert(legacy);
    dpavservep_detach(&d);afk_service_publish(legacy);dcpavserv_init(legacy,name,native_class,0);
    assert(!d.dcpavserv.service && legacy->torndown);
    legacy=prepare(13,0,name,native_class);assert(legacy);
    d.typec_generation++;afk_service_publish(legacy);dcpavserv_init(legacy,name,native_class,0);
    assert(d.dcpavserv.service==legacy && legacy->users==1);dpavservep_detach(&d);
    assert(dcpdpserv_prepare(legacy,"dcpdp-service-epic",native_class,0)==0);
    ep.wire_v2=true;ep.endpoint=0x2a;
    announce(U16_MAX,name,native_class,U16_MAX);assert(d.dcpavserv.service && d.dcpavserv.service->channel==U16_MAX);
    dpavservep_detach(&d);
    puts("PASS: actual AFKV2 HDMI/native DP alternating over 64 cycles: flip/revalidation/return, strict identity, stale OPEN generation/epoch/owner, auxiliary isolation, reader/command pins and legacy controls");
}
int main(void)
{
    d.hw.iomfb_method_profile=2;d.typec_cable_connected=true;d.typec_generation=1;d.nr_modes=1;d.connector=&con;d.dev=&d;
    con.dcp=&d;con.connected=true;con.base.registration_state=1;
    ep.dcp=&d;ep.wire_v2=true;ep.endpoint=0x2a;ep.num_channels=AFK_MAX_CHANNEL;d.dcpavservep=&ep;
    hpd_lock=&d.hpd_mutex;av_lock=&d.dcpavserv.lock;
    struct item h17p[]={{"compartment","dcpav-service-epic",0},{"name","AppleDCPDP2HDMI",0},{"interface-id",NULL,13}};
    struct dcp_parse_ctx parse_ctx={.items=h17p,.count=3};const char *parsed_name,*parsed_class;s64 parsed_unit=-1;
    assert(!parse_epic_service_init(&parse_ctx,&parsed_name,&parsed_class,&parsed_unit,true));
    assert(parsed_unit==13 && !strcmp(parsed_name,name) && !strcmp(parsed_class,class));
    struct apple_epic_service *proof=prepare(13,parsed_unit,parsed_name,parsed_class);assert(proof);afk_service_discard(proof);
    free((void *)parsed_name);free((void *)parsed_class);
    struct item legacy[]={{"EPICName","dcpav-service-epic",0},{"EPICProviderClass","legacy-class",0},{"EPICUnit",NULL,0}};
    parse_ctx=(struct dcp_parse_ctx){.items=legacy,.count=3};
    assert(!parse_epic_service_init(&parse_ctx,&parsed_name,&parsed_class,&parsed_unit,false));assert(parsed_unit==0);
    free((void *)parsed_name);free((void *)parsed_class);
    struct apple_epic_service *old=prepare(1,1,name,class);assert(old);publish(old,1);
    assert(d.dcpavserv.service==old && old->users==1);
    assert(afk_service_get(old)==old); /* Delayed EDID reader pins the retired slot. */
    old->cmds[0]=(struct epic_cmd_info){.tag=0,.type=0xc0,.rxlen=32,.txlen=32,.free_on_ack=true};
    old->cmds[0].rxbuf=calloc(1,32);old->cmds[0].txbuf=calloc(1,32);assert(old->cmds[0].rxbuf && old->cmds[0].txbuf);
    old->cmds[0].rxbuf_dma=(uintptr_t)old->cmds[0].rxbuf;old->cmds[0].txbuf_dma=(uintptr_t)old->cmds[0].txbuf;old->cmd_map[0]=1;
    disconnect();assert(!d.dcpavserv.service && !d.dcpavserv.enabled && old->users==1 && old->torndown && !old->enabled);
    assert(con.edid_invalidation_pending && !afk_service_get(old));
    assert(!afk_service_reinit(old,&ep,&ops,5));
    assert(!prepare(5,5,name,class)); /* No admission while physically disconnected. */
    connect();struct apple_epic_service *fresh=prepare(5,5,name,class);assert(fresh && fresh!=old);publish(fresh,5);
    assert(d.dcpavserv.service==fresh && fresh->users==1);
    u64 epoch=d.dcpavserv.epoch;
    dcpavserv_teardown(old);assert(d.dcpavserv.service==fresh && d.dcpavserv.epoch==epoch);
    closed(1,0x6a01,0);assert(d.dcpavserv.service==fresh && d.dcpavserv.epoch==epoch && !dma_frees);
    u8 oob[32],reply[56];size_t written;
    assert(!afkv2_encode_oob_command(oob,sizeof(oob),0,old->cmds[0].rxbuf_dma,32,old->cmds[0].txbuf_dma,32));afkv2_put32(oob+4,0);
    assert(!afkv2_encode_frame(reply,sizeof(reply),1,1,0,0xc0,AFKV2_RESPONSE,0,oob,sizeof(oob),&written));
    afk_v2_receive(&ep,0,0,reply,written);
    assert(dma_frees==2 && !old->cmd_map[0] && old->users==1 && d.dcpavserv.service==fresh && d.dcpavserv.epoch==epoch);
    assert(!afk_service_reinit(old,&ep,&ops,9));afk_service_put(old);
    afk_v2_receive(&ep,0,0,reply,written);assert(dma_frees==2); /* Duplicate late ACK. */
    disconnect();connect();struct apple_epic_service *next=prepare(9,9,name,class);assert(next);publish(next,9);
    assert(d.dcpavserv.service==next);
    closed(1,0x6a01,0);closed(5,0x6a01,0);assert(d.dcpavserv.service==next);
    disconnect();connect();next=prepare(13,13,name,class);assert(next);publish(next,13);assert(d.dcpavserv.service==next);
    closed(13,0x6a01,1);assert(d.dcpavserv.service==next); /* Nonzero result rejected. */
    closed(13,0x6a00,0);assert(d.dcpavserv.service==next); /* Wrong low flags rejected. */
    d.dcpavservep=NULL;closed(13,0x6a01,0);assert(d.dcpavserv.service==next);d.dcpavservep=&ep;
    closed(13,0x6a01,0);assert(!d.dcpavserv.service && !d.dcpavserv.enabled);
    u8 result_byte=0;struct afkv2_frame notification={.category=AFKV2_REPORT,.type=AFKV2_CLOSED,.interface_flags=1,.payload=&result_byte,.payload_size=1};
    assert(afkv2_closed_report(&notification));notification.token=1;assert(!afkv2_closed_report(&notification));
    notification.token=0;notification.payload_size=0;assert(!afkv2_closed_report(&notification));
    assert(!prepare(0,0,name,class));assert(!prepare(65536,65536,name,class));
    assert(!prepare(9,1,name,class));assert(!prepare(9,9,"wrong",class));assert(!prepare(9,9,name,"wrong"));
    struct apple_epic_service *pending=prepare(9,9,name,class);assert(pending);
    disconnect();connect();publish(pending,9);
    assert(!d.dcpavserv.service && pending->torndown && !pending->enabled && !pending->users);
    pending=prepare(9,9,name,class);assert(pending);
    mutex_lock(&d.hpd_mutex);d.typec_generation++;mutex_unlock(&d.hpd_mutex);
    publish(pending,9); /* Generation change alone, without registry retirement. */
    assert(!d.dcpavserv.service && pending->torndown && !pending->enabled);
    pending=prepare(13,13,name,class);assert(pending);
    dpavservep_detach(&d);publish(pending,13); /* Registry retirement with no physical change. */
    assert(!d.dcpavserv.service && pending->torndown && !pending->enabled);
    ep.wire_v2=false;ep.endpoint=0x28;d.hw.iomfb_method_profile=0;
    assert(!prepare(7,7,name,class));pending=prepare(7,0,name,NULL);assert(pending);publish(pending,0);
    assert(d.dcpavserv.service==pending && pending->users==1);dpavservep_detach(&d);
    assert(!pending->users && !d.hpd_mutex && !d.dcpavserv.lock);
    memset(&ep.services,0,sizeof(ep.services));ep.wire_v2=true;ep.endpoint=0x2a;d.hw.iomfb_method_profile=2;
    repeated_announcements();
    puts("PASS: actual dynamic 1/5/9/13 binding, forged identities, prepare/init epochs, physical retirement, reader pins, real old CLOSED/late ACK isolation and legacy unit 0");
}
'''
with tempfile.TemporaryDirectory(prefix='dpav-rebind-') as tmp:
    p=Path(tmp)/'test.c';p.write_text(prefix+'\n'.join(parts)+suffix)
    cc=shlex.split(os.environ.get('HOSTCC','clang'))
    subprocess.run(cc+['-std=gnu11','-Wall','-Werror','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                      '-I'+str(root/'tools/testing/selftests/drivers/gpu/apple-afkv2/host-include'),
                      '-I'+str(base),str(p),'-o',str(Path(tmp)/'test')],check=True)
    subprocess.run([str(Path(tmp)/'test')],check=True)
