/******************************************************************************/
/* reader-tpacketv3.c  -- Reader using tpacketv3
 *
 * Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Ideas from
 * https://github.com/google/stenographer/tree/master/stenotype
 * https://www.kernel.org/doc/Documentation/networking/packet_mmap.txt
 * https://github.com/rusticata/suricata/blob/rust/src/runmode-af-packet.c
 * libpcap src/pcap-linux.c
 *
 */

#include "moloch.h"
extern MolochConfig_t        config;

#ifndef __linux
void reader_tpacketv3_init(char *UNUSED(name))
{
    LOGEXIT("tpacketv3 not supported");
}
#else

#include "pcap.h"
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <errno.h>
#include <poll.h>

#ifndef TPACKET3_HDRLEN
void reader_tpacketv3_init(char *UNUSED(name))
{
    LOGEXIT("tpacketv3 not supported");
}

#else

typedef struct {
    int                  fd;
    struct tpacket_req3  req;
    uint8_t             *map;
    struct iovec        *rd;
} MolochTPacketV3_t;

LOCAL MolochTPacketV3_t infos[MAX_INTERFACES];

LOCAL int numThreads;

extern MolochPcapFileHdr_t   pcapFileHeader;
LOCAL struct bpf_program    *bpf_programs[MOLOCH_FILTER_MAX];
LOCAL struct bpf_program     bpf;

LOCAL MolochReaderStats_t gStats;
LOCAL MOLOCH_LOCK_DEFINE(gStats);

LOCAL int nextPos;
LOCAL MOLOCH_LOCK_DEFINE(nextPos);

/******************************************************************************/
int reader_tpacketv3_stats(MolochReaderStats_t *stats)
{
    MOLOCH_LOCK(gStats);

    int i;

    struct tpacket_stats_v3 tpstats;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        socklen_t len = sizeof(tpstats);
        getsockopt(infos[i].fd, SOL_PACKET, PACKET_STATISTICS, &tpstats, &len);

        gStats.dropped += tpstats.tp_drops;
        gStats.total += tpstats.tp_packets;
    }
    *stats = gStats;
    MOLOCH_UNLOCK(gStats);
    return 0;
}
/******************************************************************************/
int reader_tpacketv3_should_filter(const MolochPacket_t *packet, enum MolochFilterType *type, int *index)
{
    int t, i;
    for (t = 0; t < MOLOCH_FILTER_MAX; t++) {
        for (i = 0; i < config.bpfsNum[t]; i++) {
            if (bpf_filter(bpf_programs[t][i].bf_insns, packet->pkt, packet->pktlen, packet->pktlen)) {
                *type = t;
                *index = i;
                return 1;
            }
        }
    }
    return 0;
}
/******************************************************************************/
static void *reader_tpacketv3_thread(gpointer tinfov)
{
    long tinfo = (long)tinfov;
    int info = tinfo >> 8;
    struct pollfd pfd;
    int pos = -1;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = infos[info].fd;
    pfd.events = POLLIN | POLLERR;
    pfd.revents = 0;

    MolochPacketBatch_t batch;

    while (!config.quitting) {
        if (pos == -1) {
            MOLOCH_LOCK(nextPos);
            pos = nextPos;
            nextPos = (nextPos + 1) % infos[info].req.tp_block_nr;
            MOLOCH_UNLOCK(nextPos);
        }

        if (config.debug > 1) {
            int i;
            int cnt = 0;
            int waiting = 0;

            for (i = 0; i < (int)infos[info].req.tp_block_nr; i++) {
                struct tpacket_block_desc *tbd = infos[info].rd[i].iov_base;
                if (tbd->hdr.bh1.block_status & TP_STATUS_USER) {
                    cnt++;
                    waiting += tbd->hdr.bh1.num_pkts;
                }
            }

            LOG("Stats pos:%d %lx %d %d", pos, tinfo, cnt, waiting);
        }

        struct tpacket_block_desc *tbd = infos[info].rd[pos].iov_base;

        // Wait until the block is owned by moloch
        if ((tbd->hdr.bh1.block_status & TP_STATUS_USER) == 0) {
            poll(&pfd, 1, -1);
            continue;
        }

        struct tpacket3_hdr *th;

        moloch_packet_batch_init(&batch);
        th = (struct tpacket3_hdr *) ((uint8_t *) tbd + tbd->hdr.bh1.offset_to_first_pkt);
        uint16_t p;
        for (p = 0; p < tbd->hdr.bh1.num_pkts; p++) {
            if (unlikely(th->tp_snaplen != th->tp_len)) {
                LOGEXIT("ERROR - Moloch requires full packet captures caplen: %d pktlen: %d\n"
                    "turning offloading off may fix, something like 'ethtool -K INTERFACE tx off sg off gro off gso off lro off tso off'",
                    th->tp_snaplen, th->tp_len);
            }

            MolochPacket_t *packet = MOLOCH_TYPE_ALLOC0(MolochPacket_t);
            packet->pkt           = (u_char *)th + th->tp_mac;
            packet->pktlen        = th->tp_len;
            packet->ts.tv_sec     = th->tp_sec;
            packet->ts.tv_usec    = th->tp_nsec/1000;

            moloch_packet_batch(&batch, packet);

            th = (struct tpacket3_hdr *) ((uint8_t *) th + th->tp_next_offset);
        }
        moloch_packet_batch_flush(&batch);

        tbd->hdr.bh1.block_status = TP_STATUS_KERNEL;
        pos = -1;
    }
    return NULL;
}
/******************************************************************************/
void reader_tpacketv3_start() {
    pcap_t *dpcap = pcap_open_dead(pcapFileHeader.linktype, pcapFileHeader.snaplen);

    int t;
    for (t = 0; t < MOLOCH_FILTER_MAX; t++) {
        if (config.bpfsNum[t]) {
            int i;
            if (bpf_programs[t]) {
                for (i = 0; i < config.bpfsNum[t]; i++) {
                    pcap_freecode(&bpf_programs[t][i]);
                }
            } else {
                bpf_programs[t] = malloc(config.bpfsNum[t]*sizeof(struct bpf_program));
            }
            for (i = 0; i < config.bpfsNum[t]; i++) {
                if (pcap_compile(dpcap, &bpf_programs[t][i], config.bpfs[t][i], 1, PCAP_NETMASK_UNKNOWN) == -1) {
                    LOG("ERROR - Couldn't compile filter: '%s' with %s", config.bpfs[t][i], pcap_geterr(dpcap));
                    exit(1);
                }
            }
            moloch_reader_should_filter = reader_tpacketv3_should_filter;
        }
    }

    int i;
    long tinfo;
    char name[100];
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        for (t = 0; t < numThreads; t++) {
            snprintf(name, sizeof(name), "moloch-pcap%d-%d", i, t);
            tinfo = (i << 8) | t;
            g_thread_new(name, &reader_tpacketv3_thread, (gpointer)tinfo);
        }
    }
}
/******************************************************************************/
void reader_tpacketv3_stop()
{
    int i;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        close(infos[i].fd);
    }
}
/******************************************************************************/
void reader_tpacketv3_init(char *UNUSED(name))
{
    int i;
    int blocksize = moloch_config_int(NULL, "tpacketv3BlockSize", 1<<21, 1<<16, 1<<31);
    numThreads = moloch_config_int(NULL, "tpacketv3NumThreads", 2, 1, 6);

    if (blocksize % getpagesize() != 0) {
        LOGEXIT("block size %d not divisible by pagesize %d", blocksize, getpagesize());
    }

    if (blocksize % 16384 != 0) {
        LOGEXIT("block size %d not divisible by 16384", blocksize);
    }

    pcapFileHeader.linktype = 1;
    pcapFileHeader.snaplen = MOLOCH_SNAPLEN;
    pcap_t *dpcap = pcap_open_dead(pcapFileHeader.linktype, pcapFileHeader.snaplen);

    if (config.bpf) {
        if (pcap_compile(dpcap, &bpf, config.bpf, 1, PCAP_NETMASK_UNKNOWN) == -1) {
            LOGEXIT("ERROR - Couldn't compile filter: '%s' with %s", config.bpf, pcap_geterr(dpcap));
        }
    }


    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        int ifindex = if_nametoindex(config.interface[i]);

        infos[i].fd = socket(AF_PACKET, SOCK_RAW, 0);

        int version = TPACKET_V3;
        if (setsockopt(infos[i].fd, SOL_PACKET, PACKET_VERSION, &version, sizeof(version)) < 0)
            LOGEXIT("Error setting TPACKET_V3, might need a newer kernel: %s", strerror(errno));


        memset(&infos[i].req, 0, sizeof(infos[i].req));
        infos[i].req.tp_block_size = blocksize;
        infos[i].req.tp_block_nr = numThreads*64;
        infos[i].req.tp_frame_size = 16384;
        infos[i].req.tp_frame_nr = (blocksize * infos[i].req.tp_block_nr) / infos[i].req.tp_frame_size;
        infos[i].req.tp_retire_blk_tov = 60;
        infos[i].req.tp_feature_req_word = 0;
        if (setsockopt(infos[i].fd, SOL_PACKET, PACKET_RX_RING, &infos[i].req, sizeof(infos[i].req)) < 0)
            LOGEXIT("Error setting PACKET_RX_RING: %s", strerror(errno));

        struct packet_mreq      mreq;
        memset(&mreq, 0, sizeof(mreq));
        mreq.mr_ifindex = ifindex;
        mreq.mr_type    = PACKET_MR_PROMISC;
        if (setsockopt(infos[i].fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
            LOGEXIT("Error setting PROMISC: %s", strerror(errno));

        struct sock_fprog       fcode;
        fcode.len = bpf.bf_len;
        fcode.filter = (struct sock_filter *)bpf.bf_insns;
        if (setsockopt(infos[i].fd, SOL_SOCKET, SO_ATTACH_FILTER, &fcode, sizeof(fcode)) < 0)
            LOGEXIT("Error setting SO_ATTACH_FILTER: %s", strerror(errno));

        infos[i].map = mmap64(NULL, infos[i].req.tp_block_size * infos[i].req.tp_block_nr,
                             PROT_READ | PROT_WRITE, MAP_SHARED | MAP_LOCKED, infos[i].fd, 0);
        infos[i].rd = malloc(infos[i].req.tp_block_nr * sizeof(*infos[i].rd));

        uint16_t j;
        for (j = 0; j < infos[i].req.tp_block_nr; j++) {
            infos[i].rd[j].iov_base = infos[i].map + (j * infos[i].req.tp_block_size);
            infos[i].rd[j].iov_len = infos[i].req.tp_block_size;
        }

        struct sockaddr_ll ll;
        memset(&ll, 0, sizeof(ll));
        ll.sll_family = PF_PACKET;
        ll.sll_protocol = htons(ETH_P_ALL);
        ll.sll_ifindex = ifindex;

        if (bind(infos[i].fd, (struct sockaddr *) &ll, sizeof(ll)) < 0)
            LOGEXIT("Error binding %s: %s", config.interface[i], strerror(errno));
    }

    if (i == MAX_INTERFACES) {
        LOGEXIT("Only support up to %d interfaces", MAX_INTERFACES);
    }

    moloch_reader_start         = reader_tpacketv3_start;
    moloch_reader_stop          = reader_tpacketv3_stop;
    moloch_reader_stats         = reader_tpacketv3_stats;
}
#endif // TPACKET_V3
#endif // _linux
