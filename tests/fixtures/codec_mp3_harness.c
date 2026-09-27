#include "mp3.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc,char **argv) {
    if(argc!=3)return 2;
    char error[128];
    if(psvr2_mp3_sample_rate(NULL) || psvr2_mp3_channels(NULL))return 7;
    psvr2_mp3_close(NULL);
    errno=0;
    if(psvr2_mp3_open(NULL,error,sizeof(error)) || errno!=EINVAL || !error[0])return 7;
    errno=0;
    if(psvr2_mp3_read_frames(NULL,NULL,0)!=-1 || errno!=EINVAL)return 7;
    Psvr2Mp3Decoder *d=psvr2_mp3_open(argv[1],error,sizeof(error));
    if(!d){fprintf(stderr,"%s errno=%d\n",error,errno);return 3;}
    unsigned channels=psvr2_mp3_channels(d),rate=psvr2_mp3_sample_rate(d);
    size_t chunk=(size_t)strtoul(argv[2],NULL,10);
    if(!chunk || chunk>8192){psvr2_mp3_close(d);return 2;}
    int16_t *pcm=malloc(chunk*channels*sizeof(*pcm));
    if(!pcm){psvr2_mp3_close(d);return 2;}
    if(psvr2_mp3_read_frames(d,NULL,0)!=0)return 7;
    errno=0;
    if(psvr2_mp3_read_frames(d,NULL,1)!=-1 || errno!=EINVAL)return 7;
    errno=0;
    if(psvr2_mp3_read_frames(d,pcm,SIZE_MAX)!=-1 || errno!=EINVAL)return 7;
    uint64_t total=0;int status=0;
    for(;;){
        int64_t n=psvr2_mp3_read_frames(d,pcm,chunk);
        if(n<0){
            int failure=errno;
            fprintf(stderr,"decode errno=%d\n",failure);
            if(psvr2_mp3_read_frames(d,pcm,chunk)!=-1 || errno!=failure){status=7;break;}
            status=4;break;
        }
        if(!n){if(psvr2_mp3_read_frames(d,pcm,chunk)!=0)status=7;break;}
        if((uint64_t)n>chunk){status=7;break;}
        if(fwrite(pcm,sizeof(*pcm),(size_t)n*channels,stdout)!=(size_t)n*channels){status=5;break;}
        total+=(uint64_t)n;
        if(total>UINT64_C(20000000)){status=6;break;}
    }
    fprintf(stderr,"rate=%u channels=%u frames=%" PRIu64 "\n",rate,channels,total);
    free(pcm);psvr2_mp3_close(d);return status;
}
