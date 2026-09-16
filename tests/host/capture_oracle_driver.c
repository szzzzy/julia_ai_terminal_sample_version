#include "local_capture.h"
#include <stdio.h>
#include <string.h>
static lc_floor_t f;
static double db[750];
int main(void)
{
    char op;
    lc_floor_reset(&f,-60);
    while(scanf(" %c",&op)==1) {
        long long ms; double value; unsigned n;
        if(op=='R') { if(scanf("%lf",&value)!=1)return 2;lc_floor_reset(&f,value); }
        else if(op=='F') {
            if(scanf("%lld %lf %u",&ms,&value,&n)!=3)return 2;
            lc_floor_frame(&f,value,n!=0,ms);
        } else if(op=='S') {
            if(scanf("%lld %u",&ms,&n)!=2 || n>750)return 2;
            for(unsigned i=0;i<n;i++)if(scanf("%lf",db+i)!=1)return 2;
            lc_floor_feed_segment(&f,db,n,ms);
        } else if(op=='P') {
            int16_t pcm[320];int v;
            for(unsigned i=0;i<320;i++){if(scanf("%d",&v)!=1)return 2;pcm[i]=(int16_t)v;}
            printf("%.12f\n",lc_rms_dbfs(pcm,320));continue;
        } else return 2;
        printf("%.12f\n",f.bg);
    }
    return 0;
}
