// SPDX-License-Identifier: GPL-2.0-only
static struct apple_dart_x1n1_domain dom, prior;
static struct x1n1_batch batches[2];
static struct apple_dart_x1n1 darts[2];
static struct apple_dart_x1n1_master master;
static struct device device={&master};
static void reset(unsigned num) {
 memset(&dom,0,sizeof(dom));memset(&prior,0,sizeof(prior));
 memset(hw,0,sizeof(hw));memset(root,0,sizeof(root));memset(enabled,0,sizeof(enabled));memset(leaves,0,sizeof(leaves));
 memset(darts,0,sizeof(darts));memset(&master,0,sizeof(master));
 fail_op=0;fail_sid=fail_dart=-1;fail_nth=seen=maps_sent=rings=map_rings=largest_map_batch=0;alloc_fail_after=-1;
 master.num_darts=num;
 for(unsigned i=0;i<2;i++){darts[i]=(struct apple_dart_x1n1){.num_streams=256,.dart_id=i,.dma_max=(1ul<<42)-1,.batch=&batches[i]};master.streams[i].dart=&darts[i];set_bit(0,master.streams[i].sids);}
 dom.domain.owner=prior.domain.owner=(void*)&apple_dart_x1n1_ops;
 dom.domain.ops=prior.domain.ops=apple_dart_x1n1_ops.default_domain_ops;
}
static void attach(void){assert(!apple_dart_x1n1_attach_dev(&dom.domain,&device,NULL));}
static void map(unsigned long iova,unsigned long pa,size_t pages){size_t done=0;assert(!apple_dart_x1n1_map_pages(&dom.domain,iova,pa,16384,pages,3,0,&done));assert(done==pages*16384);}
static void check(unsigned id,unsigned sid,unsigned long iova,unsigned long pa,size_t pages){for(unsigned i=0;i<pages;i++)assert(xa_load(&hw[id][sid],(iova>>14)+i)==xa_mk_value(x1n1_mapping_value(pa+i*16384,3)));}
int main(void){
 const unsigned long io=0x4000000,pa=0x10000000000UL;size_t done;
 reset(2);darts[0].dma_min=0x10000;darts[1].dma_min=0x20000;darts[1].dma_max=0x3fffffff;
 attach();assert(dom.domain.geometry.aperture_start==0x20000&&dom.domain.geometry.aperture_end==0x3fffffff);
 map(io,pa,64);check(0,0,io,pa,64);check(1,0,io,pa,64);assert(dom.mappings.count==64&&maps_sent==2);
 assert(apple_dart_x1n1_iova_to_phys(&dom.domain,io+57)==pa+57);
 assert(apple_dart_x1n1_unmap_pages(&dom.domain,io,16384,64,NULL)==64*16384);assert(!dom.mappings.count&&!hw[0][0].count&&!hw[1][0].count);
 map(io,pa,1);assert(dom.l2_attached.count==2); /* same block reused independently */
 puts("PASS: shared IOVA, one shadow, aperture intersection, per-DART leaves and reuse");
 reset(2);attach();fail_op=X1N1_DART_MAP;fail_dart=1;fail_nth=1;
 assert(apple_dart_x1n1_map_pages(&dom.domain,io,pa,16384,64,3,0,&done)==-EIO);assert(!done&&!dom.mappings.count&&!hw[0][0].count&&!hw[1][0].count);
 fail_op=0;map(io,pa,64);fail_op=X1N1_DART_UNMAP;fail_dart=1;fail_nth=1;seen=0;
 assert(!apple_dart_x1n1_unmap_pages(&dom.domain,io,16384,64,NULL));check(0,0,io,pa,64);check(1,0,io,pa,64);assert(dom.mappings.count==64);
 puts("PASS: second-DART map rollback and failed-unmap restoration");
 reset(2);set_bit(1,master.streams[1].sids);attach();map(io,pa,64);
 fail_op=X1N1_DART_UNMAP;fail_dart=1;fail_sid=1;fail_nth=1;
 assert(!apple_dart_x1n1_unmap_pages(&dom.domain,io,16384,64,NULL));check(0,0,io,pa,64);check(1,0,io,pa,64);check(1,1,io,pa,64);
 puts("PASS: failure inside a stream cohort restores both controllers");
 reset(2);attach();map(io,pa,1);set_bit(1,master.streams[0].sids);set_bit(1,master.streams[1].sids);
 fail_op=X1N1_DART_MAP;fail_dart=1;fail_sid=1;fail_nth=1;
 assert(apple_dart_x1n1_attach_dev(&dom.domain,&device,NULL)==-EIO);
 assert(!enabled[0][1]&&!enabled[1][1]&&!hw[0][1].count&&!hw[1][1].count);
 check(0,0,io,pa,1);check(1,0,io,pa,1);
 fail_op=0;attach();check(0,1,io,pa,1);check(1,1,io,pa,1);
 puts("PASS: cross-DART attachment rollback and retry, original SIDs preserved");
 reset(2);
 struct x1n1_retained_run run[2]={{.dva=io,.ipa=pa,.size=16384},{.dva=io,.ipa=pa+16384,.size=16384}};
 struct x1n1_retained_sid kept[2]={{.runs=&run[0],.count=1},{.runs=&run[1],.count=1}};
 darts[0].retained[0]=&kept[0];darts[1].retained[0]=&kept[1];attach();
 assert(apple_dart_x1n1_map_pages(&dom.domain,io,pa,16384,1,3,0,&done)==-EBUSY);
 assert(!apple_dart_x1n1_iova_to_phys(&dom.domain,io));run[1].ipa=pa;
 assert(apple_dart_x1n1_iova_to_phys(&dom.domain,io)==pa);
 puts("PASS: inherited mappings reserved on either controller; translation requires agreement");
 reset(2);for(unsigned i=0;i<256;i++){set_bit(i,master.streams[0].sids);set_bit(i,master.streams[1].sids);}attach();
 map(io,pa,1);assert(maps_sent==512&&map_rings==4&&largest_map_batch==255);
 assert(apple_dart_x1n1_unmap_pages(&dom.domain,io,16384,1,NULL)==16384);
 puts("PASS: 512 streams retain full 255-op batches across batch boundaries");
 reset(2);attach();const unsigned long crossing=0x1ff8000;map(crossing,pa,4);
 fail_op=X1N1_DART_UNMAP;fail_dart=1;fail_nth=2;
 assert(apple_dart_x1n1_unmap_pages(&dom.domain,crossing,16384,4,NULL)==32768);
 assert(dom.mappings.count==2&&hw[0][0].count==2&&hw[1][0].count==2);
 check(0,0,crossing+32768,pa+32768,2);check(1,0,crossing+32768,pa+32768,2);
 puts("PASS: partial unmap returns only the common prefix across 32-MiB boundaries");
 reset(2);for(unsigned i=0;i<256;i++){set_bit(i,master.streams[0].sids);set_bit(i,master.streams[1].sids);}attach();
 fail_op=X1N1_DART_MAP;fail_dart=1;fail_nth=256;
 assert(apple_dart_x1n1_map_pages(&dom.domain,io,pa,16384,1,3,0,&done)==-EIO);
 assert(!done&&!dom.mappings.count);
 for(unsigned d=0;d<2;d++)for(unsigned sid=0;sid<256;sid++)assert(!hw[d][sid].count);
 puts("PASS: second-DART failure across a 255-op batch boundary unwinds every SID");
 reset(2);attach();alloc_fail_after=3;
 assert(apple_dart_x1n1_map_pages(&dom.domain,io,pa,16384,4,3,0,&done)==-ENOMEM);
 assert(!done&&!dom.mappings.count&&!maps_sent);
 puts("PASS: allocation failure has no hardware mapping side effects");
 reset(2);run[0].dva=run[1].dva=io+0x4000000;run[0].ipa=run[1].ipa=pa;
 darts[0].retained[0]=&kept[0];darts[1].retained[0]=&kept[1];
 assert(!apple_dart_x1n1_attach_dev(&prior.domain,&device,NULL));
 assert(!apple_dart_x1n1_map_pages(&prior.domain,io,pa,16384,1,3,0,&done));
 assert(!apple_dart_x1n1_attach_dev(&dom.domain,&device,&prior.domain));
 assert(bitmap_empty(prior.streams[0].sids,256)&&bitmap_empty(prior.streams[1].sids,256));
 assert(!hw[0][0].count&&!hw[1][0].count);map(io,pa+16384,1);
 check(0,0,io,pa+16384,1);check(1,0,io,pa+16384,1);
 puts("PASS: retained-domain transfer reuses existing tables on both DARTs");
 reset(2);darts[0].retained[0]=&kept[0];darts[1].retained[0]=&kept[1];
 assert(!apple_dart_x1n1_attach_dev(&prior.domain,&device,NULL));
 assert(!apple_dart_x1n1_map_pages(&prior.domain,io,pa,16384,1,3,0,&done));
 fail_op=X1N1_DART_RETAINED_INFO;fail_dart=1;fail_nth=1;
 assert(apple_dart_x1n1_attach_dev(&dom.domain,&device,&prior.domain)==-EIO);
 assert(test_bit(0,prior.streams[0].sids)&&test_bit(0,prior.streams[1].sids));
 assert(bitmap_empty(dom.streams[0].sids,256)&&bitmap_empty(dom.streams[1].sids,256));
 check(0,0,io,pa,1);check(1,0,io,pa,1);
 puts("PASS: failed retained-domain transfer restores both original streams");
 reset(1);attach();map(io,pa,64);assert(maps_sent==1&&map_rings==1);assert(apple_dart_x1n1_unmap_pages(&dom.domain,io,16384,64,NULL)==64*16384);
 puts("PASS: single-DART bulk mapping still uses one map operation/doorbell");
 return 0;
}
