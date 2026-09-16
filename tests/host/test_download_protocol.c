#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "download_protocol.h"

int main(void)
{
    char migrated[128];
    assert(download_server_url("https://8.133.215.254:8443/a?sig=8443", false, migrated, sizeof(migrated)));
    assert(strcmp(migrated, "https://8.133.215.254:18443/a?sig=8443") == 0);
    const char *unchanged[] = {"https://8.133.215.254:18443/a", "https://other:8443/a",
        "https://8.133.215.254:84430/a", "https://8.133.215.254:8443@other/a",
        "http://8.133.215.254:8443/a", "https://other/a?url=https://8.133.215.254:8443/a"};
    for (unsigned i=0; i<sizeof(unchanged)/sizeof(unchanged[0]); ++i) {
        assert(download_server_url(unchanged[i], false, migrated, sizeof(migrated)));
        assert(strcmp(migrated, unchanged[i]) == 0);
    }
    const char *old = "https://8.133.215.254:8443";
    assert(download_server_url(old, true, migrated, sizeof(migrated)));
    assert(strcmp(migrated, old) == 0);
    assert(!download_server_url(old, false, migrated, strlen(old)+1));
    assert(download_server_url(old, false, migrated, strlen(old)+2));
    assert(strcmp(migrated, "https://8.133.215.254:18443") == 0);
    assert(!download_server_url(NULL, false, migrated, sizeof(migrated)));
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
