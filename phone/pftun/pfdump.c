#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/pfvar.h>
static int fd;
static void addr(struct pf_rule_addr *a){
  char b[64]="any";
  if(a->addr.type==PF_ADDR_ADDRMASK){
    int z=1; for(int i=0;i<4;i++) if(a->addr.v.a.addr.addr32[i]) z=0;
    if(!z) inet_ntop(AF_INET,&a->addr.v.a.addr.v4addr,b,64);
    if(!z||a->addr.v.a.mask.addr32[0]) printf("%s/%08x",b,ntohl(a->addr.v.a.mask.addr32[0])); else printf("any");
  } else if(a->addr.type==PF_ADDR_TABLE) printf("<%s>",a->addr.v.tblname);
  else if(a->addr.type==PF_ADDR_DYNIFTL) printf("(%s)",a->addr.v.ifname);
  else printf("t%d",a->addr.type);
}
static void dump(const char*anchor,int depth){
  struct pfioc_rule pr; int act[]={PF_PASS,PF_DROP,PF_SCRUB,PF_NOSCRUB,PF_NAT,PF_NONAT,PF_BINAT,PF_NOBINAT,PF_RDR,PF_NORDR,PF_NAT64,PF_NONAT64};
  const char*nm[]={"pass","drop","scrub","noscrub","nat","nonat","binat","nobinat","rdr","nordr","nat64","nonat64"};
  for(int k=0;k<12;k++){
    memset(&pr,0,sizeof pr); strlcpy(pr.anchor,anchor,sizeof pr.anchor); pr.rule.action=act[k];
    if(ioctl(fd,DIOCGETRULES,&pr)<0) continue;
    unsigned n=pr.nr, t=pr.ticket;
    for(unsigned i=0;i<n;i++){
      struct pfioc_rule r; memset(&r,0,sizeof r); strlcpy(r.anchor,anchor,sizeof r.anchor); r.ticket=t; r.nr=i; r.rule.action=act[k];
      if(ioctl(fd,DIOCGETRULE,&r)<0){perror("getrule");continue;}
      struct pf_rule*p=&r.rule;
      printf("%*s[%s] %s dir=%d if=%s%s af=%d proto=%d q=%d keep=%d rt=%d src=",depth*2,"",anchor,nm[k],p->direction,p->ifname,p->ifnot?"(not)":"",p->af,p->proto,p->quick,p->keep_state,p->rt);
      addr(&p->src); printf(" dst="); addr(&p->dst);
      printf(" ports=%d-%d pkts=%llu anchor_call=%s\n",p->rpool.proxy_port[0],p->rpool.proxy_port[1],p->packets[0]+p->packets[1],r.anchor_call);
    }
  }
  struct pfioc_ruleset rs; memset(&rs,0,sizeof rs); strlcpy(rs.path,anchor,sizeof rs.path);
  if(ioctl(fd,DIOCGETRULESETS,&rs)==0){
    unsigned n=rs.nr;
    for(unsigned i=0;i<n;i++){
      struct pfioc_ruleset r2; memset(&r2,0,sizeof r2); strlcpy(r2.path,anchor,sizeof r2.path); r2.nr=i;
      if(ioctl(fd,DIOCGETRULESET,&r2)<0) continue;
      char sub[1024]; snprintf(sub,sizeof sub,"%s%s%s",anchor,*anchor?"/":"",r2.name);
      printf("%*sANCHOR %s\n",depth*2,"",sub); dump(sub,depth+1);
    }
  }
}
int main(){
  fd=open("/dev/pf",O_RDWR); if(fd<0){perror("open /dev/pf");return 1;}
  struct pf_status st; memset(&st,0,sizeof st);
  if(ioctl(fd,DIOCGETSTATUS,&st)==0) printf("pf running=%d states=%u ifname=%s\n",st.running,st.states,st.ifname);
  dump("",0); return 0;
}
