#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "download_protocol.h"

int main(void)
{
    const char *hosts=" one.example, allowed.example ,other.example";
    assert(download_url_host_allowed("https://ALLOWED.example:9443/fw.bin?x=1", hosts));
    assert(!download_url_host_allowed("https://allowed.example:pass@other.invalid/fw.bin", hosts));
    assert(!download_url_host_allowed("https://allowed.example:pass@other.invalid/fw.bin", ""));
    assert(!download_url_host_allowed("https://allowed.example.invalid/fw.bin", hosts));
    assert(!download_url_host_allowed("http://allowed.example/fw.bin", hosts));
    assert(!download_url_host_allowed("https:///fw.bin", ""));
    assert(download_url_host_allowed("https://127.0.0.1/fw.bin", ""));
    size_t start=0,end=0,total=0;
    assert(download_parse_content_range("bytes 65536-199999/200000", &start,&end,&total));
    assert(start==65536 && end==199999 && total==200000);
    const char *bad[]={"bytes 0-/20","bytes -1-2/20","bytes 2-1/20","bytes 0-20/20",
        "bytes 0-0/0","bytes 0-0/","bytes 0-0/1x","bytes 999999999999999999999999-1/2"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
        assert(!download_parse_content_range(bad[i],&start,&end,&total));
    download_response_headers_t headers={0};
    download_response_header(&headers,"eTaG","\"revision-1\"");
    download_response_header(&headers,"CONTENT-RANGE","bytes 4-7/8");
    assert(!headers.invalid && strcmp(headers.content_range,"bytes 4-7/8")==0);
    download_response_header(&headers,"ETag","\"revision-1\"");
    assert(!headers.invalid);
    download_response_header(&headers,"ETag","\"revision-2\"");
    assert(headers.invalid);
    memset(&headers,0,sizeof(headers));
    char too_long[129];memset(too_long,'x',128);too_long[128]=0;
    download_response_header(&headers,"ETag",too_long);
    assert(headers.invalid && headers.etag[0]==0);
    puts("PASS: URL authority, strict Content-Range and bounded response headers");
    return 0;
}
