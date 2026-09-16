"""Run the actual logger register/profile and coherent-read code with fake I2C."""
import argparse
from pathlib import Path
from test_recovery_paths import run_case

parser = argparse.ArgumentParser()
parser.add_argument('--cc', required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
args.root = Path(__file__).resolve().parents[2]
args.out.mkdir(parents=True, exist_ok=True)
run_case(args, 'imu_logger_driver', r'''
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NOT_FINISHED 0x10c
#define QMI8658_CTRL2 3
#define QMI8658_CTRL3 4
#define QMI8658_CTRL7 8
static uint8_t regs[128];
static bool fail_read, mismatch;
static unsigned read_count, race_on;
static esp_err_t board_imu_set_enabled(bool enabled){regs[8]=enabled?3:0;return ESP_OK;}
static esp_err_t write_reg(uint8_t r,uint8_t v){regs[r]=v;return ESP_OK;}
static esp_err_t read_regs(uint8_t r,uint8_t *p,size_t n){
    if(fail_read)return ESP_FAIL;
    if(++read_count==race_on)++regs[0x30];
    memcpy(p,regs+r,n);if(mismatch)p[0]^=1;return ESP_OK;
}
''', [('main/hardware/qmi8658_shared.c', ['le_i16', 'board_imu_logger_configure',
                                       'board_imu_logger_read'])], r'''
int main(void){
    assert(board_imu_logger_configure()==ESP_OK);
    assert(regs[3]==0x36 && regs[4]==0x66 && regs[6]==0 && regs[8]==3);
    mismatch=true;assert(board_imu_logger_configure()==ESP_ERR_INVALID_RESPONSE);
    assert(regs[8]==0);mismatch=false;
    regs[0x30]=0xfe;regs[0x31]=0xff;regs[0x32]=0xff;
    regs[0x35]=0;regs[0x36]=0x80;regs[0x37]=0xff;regs[0x38]=0x7f;
    uint32_t counter;int16_t raw[6];read_count=0;
    assert(board_imu_logger_read(&counter,raw)==ESP_OK);
    assert(counter==0xfffffe && raw[0]==-32768 && raw[1]==32767);
    read_count=0;race_on=2;assert(board_imu_logger_read(&counter,raw)==ESP_ERR_NOT_FINISHED);
    read_count=0;race_on=3;assert(board_imu_logger_read(&counter,raw)==ESP_ERR_NOT_FINISHED);
    fail_read=true;assert(board_imu_logger_read(&counter,raw)==ESP_FAIL);
    assert(board_imu_logger_read(NULL,raw)==ESP_ERR_INVALID_ARG);
    puts("PASS: logger profile, raw conversion, timestamp race and I2C failures");return 0;
}
''')

run_case(args, 'imu_logger_peer', r'''
#define CONFIG_LWIP_IPV6 1
#define AF_INET 2
#define AF_INET6 10
struct in_addr { uint8_t bytes[4]; };
struct in6_addr { uint8_t s6_addr[16]; };
struct sockaddr_storage { unsigned ss_family; uint8_t storage[64]; };
struct sockaddr_in { unsigned sin_family; struct in_addr sin_addr; };
struct sockaddr_in6 { unsigned sin6_family; struct in6_addr sin6_addr; };
static bool mapped(const struct in6_addr *a){
    static const uint8_t prefix[12]={0,0,0,0,0,0,0,0,0,0,255,255};
    return memcmp(a->s6_addr,prefix,12)==0;
}
#define IN6_IS_ADDR_V4MAPPED(a) mapped(a)
static const char *inet_ntop(int af,const void *src,char *dst,size_t size){
    const uint8_t *b=src;assert(af==AF_INET);
    int n=snprintf(dst,size,"%u.%u.%u.%u",b[0],b[1],b[2],b[3]);
    return n>=0 && (size_t)n<size?dst:NULL;
}
''', [('main/diagnostics/imu_logger.c', ['format_peer_ipv4'])], r'''
int main(void){
    struct sockaddr_storage peer={0};char ip[16];
    struct sockaddr_in *v4=(struct sockaddr_in*)&peer;
    v4->sin_family=AF_INET;
    const uint8_t addr[4]={192,168,137,70};memcpy(v4->sin_addr.bytes,addr,4);
    assert(format_peer_ipv4(&peer,ip,sizeof(ip)) && strcmp(ip,"192.168.137.70")==0);
    memset(&peer,0,sizeof(peer));
    struct sockaddr_in6 *v6=(struct sockaddr_in6*)&peer;v6->sin6_family=AF_INET6;
    v6->sin6_addr.s6_addr[10]=255;v6->sin6_addr.s6_addr[11]=255;
    memcpy(v6->sin6_addr.s6_addr+12,addr,4);
    assert(format_peer_ipv4(&peer,ip,sizeof(ip)) && strcmp(ip,"192.168.137.70")==0);
    assert(!format_peer_ipv4(&peer,ip,4));
    v6->sin6_addr.s6_addr[10]=0;assert(!format_peer_ipv4(&peer,ip,sizeof(ip)));
    peer.ss_family=0;assert(!format_peer_ipv4(&peer,ip,sizeof(ip)));
    puts("PASS: native and IPv4-mapped IPv6 client addresses");return 0;
}
''')
