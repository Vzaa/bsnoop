/* snoop.c - Berkley snoop protocol module
 *
 * (C) 2005-2006 Ivan Keberlein <ikeberlein@users.sourceforge.net>
 * 		 KNET Ltd. http://www.isp.kz
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public Licence version 2 as
 * published by Free Software Foundation.
 *
 * See COPYING for details
 *
 */
#include <linux/init.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/spinlock.h>
#include <linux/netdevice.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/netfilter.h>	// hook register/unregister
#include <linux/netfilter_ipv4.h>	// for enum nf_ip_hook_priorities
#include <linux/netfilter_bridge.h>
#include <linux/slab.h>
#include <linux/jhash.h>
#include <net/tcp.h>
#include <asm/bitops.h>
/*#include <asm/semaphore.h>*/
#include <asm/unaligned.h>
#include "snoop.h"

#if 0
#define DBG(args...)	printk(args)
#else
#define DBG(args...)
#endif

#if 0
#define INFO(args...)	printk(args)
#else
#define INFO(args...)
#endif

#define DBG_SEQ_ACK(X)	DBG(" SA<%lu,%lu>", \
        (long unsigned int)((X)->last_seq - (X)->isn), \
        (long unsigned int)((X)->last_ack - (X)->isn))

#define AUTHOR	"Ivan Keberlein <ikeberlein@users.sourceforge.net>"

MODULE_DESCRIPTION
("Partial implementation of Berkley snoop protocol for GNU/Linux");
MODULE_AUTHOR(AUTHOR);
MODULE_LICENSE("GPL");

static char *wh_dev = NULL;
module_param(wh_dev, charp, 0);
MODULE_PARM_DESC(wh_dev,
        "Device name wich WIRELESS hosts are connected to");

static char *fh_dev = NULL;
module_param(fh_dev, charp, 0);
MODULE_PARM_DESC(fh_dev, "Device name wich FIXED hosts are connected to");

static u32 retransmit_mark = 255;
module_param(retransmit_mark, uint, 0);
MODULE_PARM_DESC(retransmit_mark,
        "Netfilter mark for retransmitted packets");

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
static struct kmem_cache *conntrack_cachep;
static struct kmem_cache *pkt_cachep;
#else
static kmem_cache_t *conntrack_cachep;
static kmem_cache_t *pkt_cachep;
#endif

static u32 snoop_hash_rnd;
static u32 snoop_htable_size;
static u32 snoop_conn_max;
static sn_hash_bucket_t *sn_htable;

static snoop_stats_t snoop_stats = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

static int (*nf_forward) (struct sk_buff *);

static inline int get_optlen(struct tcphdr * tcph);
static inline unsigned char * get_opt_ptr(struct sk_buff * skb, int optlen);
static int extract_ts_option(unsigned char * ptr, int length, u32 * ts_a, u32 * ts_b);
static int update_ts_option(unsigned char * ptr, int length, u32 ts_a, u32 ts_b);

static void sn_forward(sn_packet_t * pkt)
{
    struct sk_buff *clone;
    if (!nf_forward)
        return;
    clone = skb_copy(pkt->skb, GFP_ATOMIC);

    if (!clone)
        return;

    pkt->send_time = jiffies;
    nf_forward(clone);
}

static void sn_retransmit(sn_packet_t * pkt)
{
    if (pkt->rxmit_count >= SNOOP_RXMIT_MAX)
        return;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,19)
    pkt->skb->mark = retransmit_mark;
#else
    pkt->skb->nfmark = retransmit_mark;
#endif
    sn_forward(pkt);
    pkt->rxmit_count++;
    snoop_stats.local_rxmits++;
}

    static inline u32
snoop_hash(u32 wh_addr, u16 wh_port, u32 fh_addr, u16 fh_port)
{
    u32 str[] = { wh_addr, (wh_port << 16) | fh_port, fh_addr };
    return jhash(str, sizeof(str), snoop_hash_rnd) % snoop_htable_size;
}

static inline sn_conntrack_t *sn_conntrack_lookup(u32 hash, u32 wh_addr,
        u16 wh_port, u32 fh_addr,
        u16 fh_port)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
    struct hlist_node *n;
#endif
    sn_conntrack_t *ct;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
    hlist_for_each_entry(ct, n, &sn_htable[hash].list, link) {
#else
    hlist_for_each_entry(ct, &sn_htable[hash].list, link) {
#endif
        if (ct->waddr != wh_addr
                || ct->faddr != fh_addr
                || ct->wport != wh_port || ct->fport != fh_port)
            continue;
        return ct;
    }

    return NULL;
}

static inline void sn_conntrack_add(sn_conntrack_t * ct)
{
    hlist_add_head(&ct->link, &sn_htable[ct->hash].list);
}

static inline void sn_conntrack_del(sn_conntrack_t * ct)
{
    if (ct->first_ack != NULL) {
        kfree_skb(ct->first_ack);
        ct->first_ack = NULL;
    }
    hlist_del(&ct->link);
}

static inline void destroy_packet(sn_packet_t * pkt)
{
    if (pkt->skb)
        kfree_skb(pkt->skb);
    kmem_cache_free(pkt_cachep, pkt);
}

static int free_pkt_list(sn_conntrack_t * ct)
{
    sn_packet_t *cur, *next;

    list_for_each_entry_safe(cur, next, &ct->pkt_list, list) {
        list_del(&cur->list);
        destroy_packet(cur);
    }
    return 1;
}

static inline void sn_conntrack_get(sn_conntrack_t * ct)
{
    atomic_inc(&ct->use_count);
}

static inline int sn_conntrack_put(sn_conntrack_t * ct)
{
    if (atomic_dec_and_test(&ct->use_count)) {
        free_pkt_list(ct);
        kmem_cache_free(conntrack_cachep, ct);
        return 1;
    }
    return 0;
}

static void connection_timeout(unsigned long arg)
{
    sn_conntrack_t *ct = (sn_conntrack_t *) arg;

    DBG("SNOOP%i:%s [", smp_processor_id(), __FUNCTION__);

    write_lock(&sn_htable[ct->hash].lock);

    if (test_and_set_bit(FL_SNOOP_DESTROYING, &ct->flags) > 0) {
        write_unlock(&sn_htable[ct->hash].lock);
        sn_conntrack_put(ct);
        DBG("OK]\n");
        return;
    }

    sn_conntrack_del(ct);
    write_unlock(&sn_htable[ct->hash].lock);

    if (del_timer(&ct->rto_timer))
        sn_conntrack_put(ct);

    sn_conntrack_put(ct);

    DBG("OK]\n");
}

static inline sn_packet_t *conntrack_head_packet(sn_conntrack_t * ct)
{
    if (list_empty(&ct->pkt_list))
        return NULL;

    return list_entry(ct->pkt_list.next, sn_packet_t, list);
}

static int restart_retransmit_timer(sn_conntrack_t * ct)
{
    sn_packet_t *pkt = conntrack_head_packet(ct);

    if (!pkt || pkt->seq != ct->last_ack) {
        if (del_timer(&ct->rto_timer))
            sn_conntrack_put(ct);
        return 0;
    }

    if (!timer_pending(&ct->rto_timer))
        sn_conntrack_get(ct);

    mod_timer(&ct->rto_timer, pkt->send_time + 2 * ct->rtt);
    return 1;
}

static void retransmit_timeout(unsigned long arg)
{
    sn_conntrack_t *ct = (sn_conntrack_t *) arg;
    sn_packet_t *pkt;

    write_lock(&sn_htable[ct->hash].lock);
    if (test_bit(FL_SNOOP_DESTROYING, &ct->flags)) {
        write_unlock(&sn_htable[ct->hash].lock);
        sn_conntrack_put(ct);
        return;
    }
    write_unlock(&sn_htable[ct->hash].lock);

    spin_lock(&ct->lock);
    pkt = conntrack_head_packet(ct);

    if (pkt && pkt->seq == ct->last_ack) {
        snoop_stats.rto++;
        sn_retransmit(pkt);
        DBG("SNOOP%i:RTO % 9i [JIF %lu]\n",
                smp_processor_id(), ct->last_ack - ct->isn, jiffies);
        mod_timer(&ct->rto_timer, pkt->send_time + 2 * ct->rtt);
        spin_unlock(&ct->lock);
    } else {
        spin_unlock(&ct->lock);
        sn_conntrack_put(ct);
    }
}

static sn_packet_t *pkt_create(sn_conntrack_t * ct, pkt_info_t * pkt_info)
{
    sn_packet_t *result;

    result = kmem_cache_alloc(pkt_cachep, GFP_ATOMIC);
    if (!result) {
        printk(KERN_ERR "SNOOP: no memory for new PKT\n");
        return NULL;
    }

    result->ct = ct;
    result->seq = pkt_info->seq;
    result->size = pkt_info->size;
    result->send_time = jiffies;
    result->sender_rxmit = 0;
    result->rxmit_count = 0;
    result->skb = skb_copy(pkt_info->skb, GFP_ATOMIC);

    return result;
}

static inline sn_packet_t *ct_enqueue_packet(sn_conntrack_t * ct,
        struct list_head *head,
        pkt_info_t * pkt_info)
{
    sn_packet_t *cur;
    sn_packet_t *pkt = pkt_create(ct, pkt_info);

    if (pkt == NULL)
        return NULL;

    if (ct->pkt_count >= SNOOP_CACHE_MAX) {
        return NULL;
    }

    if (ct->pkt_count == 0) {
        list_add_tail(&pkt->list, head);
    }
    else
    {
        int added = 0;
        list_for_each_entry_reverse(cur, head, list) {
            if (after(pkt->seq, cur->seq)) {
                list_add(&pkt->list, &cur->list);
                added = 1;
                break;
            }
        }
        if (!added) {
            list_add_tail(&pkt->list, head);
        }
    }
    ct->pkt_count++;

    return pkt;
}

static void ct_insert_packet(sn_conntrack_t * ct, pkt_info_t * pkt_info)
{
    sn_packet_t *pkt = NULL, *cur = NULL;

    if (!pkt_info->size)
        return;

    snoop_stats.input_pkts++;
    if (before(ct->last_seq,  pkt_info->seq)) {	/* new segment */
        if (ct->pkt_count >= SNOOP_CACHE_MAX) {
            // Remove oldest packet
            sn_packet_t *pkt = conntrack_head_packet(ct);
            if (pkt) {
                list_del(&pkt->list);
                destroy_packet(pkt);
                ct->pkt_count--;
            } else {
                printk(KERN_ERR
                        "SNOOP: pkt_count has reached SNOOP_CACHE_MAX, but cache seems to be empty\n");
                ct->pkt_count = 0;
            }
        }
        pkt = ct_enqueue_packet(ct, &ct->pkt_list, pkt_info);
        if (pkt) {
            ct->last_seq = pkt_info->seq + pkt_info->size - 1;
            DBG("STORED %ib (%i)", pkt_info->size, ct->pkt_count);
        }
        return;
    }
    snoop_stats.sender_rxmits++;

    if (list_empty(&ct->pkt_list)) {
        pkt = ct_enqueue_packet(ct, &ct->pkt_list, pkt_info);
        DBG("SNDR.RXMIT %ib (%i)", pkt_info->size, ct->pkt_count);
    } else
        list_for_each_entry(cur, &ct->pkt_list, list) {
            if (cur->seq == pkt_info->seq) {
                if (cur->size < pkt_info->size) {	// replace packet payload
                    kfree_skb(cur->skb);
                    cur->skb = skb_copy(pkt_info->skb, GFP_ATOMIC);
                }
                pkt = cur;
                DBG("SNDR.RXMIT %ib", pkt_info->size);
                break;
            }

            if (after(cur->seq, pkt_info->seq)) {
                pkt = ct_enqueue_packet(ct, &cur->list, pkt_info);
                DBG("SNDR.RXMIT %ib (%i)", pkt_info->size, ct->pkt_count);
                break;
            }
        }

    if (pkt) {
        pkt->sender_rxmit = 1;
        pkt->rxmit_count = 0;
    }
}

inline static void restart_idle_timer(sn_conntrack_t * ct)
{
    mod_timer(&ct->tmo_timer, jiffies + SNOOP_CONN_TIMEO * HZ);
}

static u32 find_next_ack_seq(u32 last_ack, sn_conntrack_t * entry)
{
    sn_packet_t *cur;
    u32 next_ack = last_ack;

    list_for_each_entry(cur, &entry->pkt_list, list) {
        if (before(cur->seq, last_ack))
            continue;

        if (next_ack != cur->seq) {
            return next_ack;
        }
        next_ack = cur->seq + cur->size;
    }
    return next_ack;
}


static int cmp_packets(void *priv, struct list_head *a, struct list_head *b)
{
        sn_packet_t * pkt_a;
        sn_packet_t * pkt_b;

        pkt_a = list_entry(a, sn_packet_t, list);
        pkt_b = list_entry(b, sn_packet_t, list);

        if (before(pkt_a->seq, pkt_a->seq))
                return -1;

        if (after(pkt_a->seq, pkt_a->seq))
                return 1;

        return 0;
}

static unsigned int snoop_data(pkt_info_t * pkt_info)
{
    struct sk_buff * rep_ack = NULL;
    struct iphdr *iph;
    struct tcphdr *tcph, _tcph;
    unsigned int result = NF_ACCEPT;
    u32 hash =
        snoop_hash(pkt_info->daddr, pkt_info->dport, pkt_info->saddr,
                pkt_info->sport);
    sn_conntrack_t *entry;

    read_lock(&sn_htable[hash].lock);
    entry = sn_conntrack_lookup(hash,
            pkt_info->daddr, pkt_info->dport,
            pkt_info->saddr, pkt_info->sport);
    if (!entry) {
        read_unlock(&sn_htable[hash].lock);
        return NF_ACCEPT;
    }
    sn_conntrack_get(entry);
    read_unlock(&sn_htable[hash].lock);

    spin_lock(&entry->lock);
    restart_idle_timer(entry);

    DBG("SNOOP%i:DAT % 9i [", smp_processor_id(),
            pkt_info->seq - entry->isn);

    if (pkt_info->syn) {
        entry->state = SNOOP_WAITACK;
        entry->last_seq = pkt_info->seq;
        entry->isn = pkt_info->seq;
        DBG("WAIT_ACK");
    } else {
        int cong = 1;
        int ahead = 0;
        result = NF_ACCEPT;
        /*printk("%u %u %u %u\n", pkt_info->seq, entry->last_ack_gen + pkt_info->size, entry->last_ack_gen, pkt_info->size);*/
        if (pkt_info->seq >= entry->last_ack_gen && !(entry->pkt_count >= SNOOP_CACHE_MAX - 10))
        {
            ct_insert_packet(entry, pkt_info);
        }
        else
        {
            result = NF_DROP;
            ahead = 1;
        }

        if (pkt_info->seq == entry->last_ack_gen)
        {
            cong = 0;
            /*if (entry->last_ack_gen - entry->last_ack > 2000000) {*/
        }
        else {
            // FIXME: send ACK with last_ack to sender
            // although this is not very common path of execution
            // result = NF_DROP;
            DBG("OLD");
        }
        if (entry->first_ack && !cong && !ahead) {
            if (pkt_info->size > 0) {
                rep_ack = skb_copy(entry->first_ack, GFP_ATOMIC);
                iph = ip_hdr(rep_ack);
                tcph = skb_header_pointer(rep_ack, iph->ihl << 2, sizeof(_tcph), &_tcph);
                tcph->check = 0;
                /*u16 check = 0;*/
                /*u16 old_check = tcph->check;*/
                /*check = tcp_v4_check(rep_ack->len - (iph->ihl << 2), iph->saddr, iph->daddr, csum_partial(tcph, tcph->doff << 2, rep_ack->csum));*/
                /*DBG("\n0x%x 0x%x %d 0x%x\n", check, old_check, rep_ack->len, rep_ack->csum);*/

                tcph->seq = htonl(pkt_info->ack_seq);
                entry->last_ack_gen = find_next_ack_seq(pkt_info->seq + pkt_info->size, entry);
                /*entry->last_ack_gen = pkt_info->seq + pkt_info->size;*/
                tcph->ack_seq = htonl(entry->last_ack_gen);

                tcph->window = htons(entry->last_window);
#define TS_UPDATE_TEST
#ifdef TS_UPDATE_TEST
                u32 ts_a = 0, ts_b = 0;
                int ts_field = 0;
                if (pkt_info->opt != NULL) {
                    ts_field = extract_ts_option(pkt_info->opt, pkt_info->optlen, &ts_a, &ts_b);
                }
                unsigned char * opts = get_opt_ptr(rep_ack, get_optlen(tcph));
                if (opts != NULL && ts_field) {
                    update_ts_option(opts, get_optlen(tcph), ts_b, ts_a);
                }
#endif
                /* calculate updated checksum */
                tcph->check = tcp_v4_check(rep_ack->len - (iph->ihl << 2), iph->saddr, iph->daddr, csum_partial(tcph, tcph->doff << 2, rep_ack->csum));
                DBG("ACK INSERT SRC:%d.%d.%d.%d. DST:%d.%d.%d.%d sport:%d dport:%d\n",
                        ((unsigned char*)&(iph->saddr))[0],
                        ((unsigned char*)&(iph->saddr))[1],
                        ((unsigned char*)&(iph->saddr))[2],
                        ((unsigned char*)&(iph->saddr))[3],
                        ((unsigned char*)&(iph->daddr))[0],
                        ((unsigned char*)&(iph->daddr))[1],
                        ((unsigned char*)&(iph->daddr))[2],
                        ((unsigned char*)&(iph->daddr))[3],
                        pkt_info->dport,
                        pkt_info->sport
                   );
                int res;
                res = nf_forward(rep_ack);
                INFO("%d: reply with rel ack_seq:%u\n", res, (pkt_info->seq + pkt_info->size) - entry->isn);
                /*kfree_skb(rep_ack);*/
            }
        }
        else if (entry->first_ack && (ahead)) {
            result = NF_DROP;
        }
        else if (entry->first_ack && (cong)) {
            rep_ack = skb_copy(entry->first_ack, GFP_ATOMIC);
            iph = ip_hdr(rep_ack);
            tcph = skb_header_pointer(rep_ack, iph->ihl << 2, sizeof(_tcph), &_tcph);
            tcph->check = 0;
            if (cong) {
                /*INFO("congestion? %u %u %u %d %d\n", entry->last_ack - entry->isn, pkt_info->seq - entry->isn, entry->last_ack_gen - entry->isn, entry->pkt_count, ahead);*/
                INFO("congestion? %u difference\n", pkt_info->seq - entry->last_ack_gen);
            }
            else if (ahead) {
                INFO("ahead? %u %u %u %d %d\n", entry->last_ack - entry->isn, pkt_info->seq - entry->isn, entry->last_ack_gen - entry->isn, entry->pkt_count, ahead);
            }

            tcph->seq = htonl(pkt_info->ack_seq);
            tcph->ack_seq = htonl(entry->last_ack_gen);
            tcph->window = htons(entry->last_window);
#ifdef TS_UPDATE_TEST
            u32 ts_a = 0, ts_b = 0;
            int ts_field = 0;
            if (pkt_info->opt != NULL) {
                ts_field = extract_ts_option(pkt_info->opt, pkt_info->optlen, &ts_a, &ts_b);
            }
            unsigned char * opts = get_opt_ptr(rep_ack, get_optlen(tcph));
            if (opts != NULL && ts_field) {
                update_ts_option(opts, get_optlen(tcph), ts_b, ts_a);
            }
#endif
            /* calculate updated checksum */
            tcph->check = tcp_v4_check(rep_ack->len - (iph->ihl << 2), iph->saddr, iph->daddr, csum_partial(tcph, tcph->doff << 2, rep_ack->csum));
            int res;
            res = nf_forward(rep_ack);
            INFO("%d reply dup ack with rel ack_seq:%u\n", res,  (entry->last_ack_gen) - entry->isn);
            /*kfree_skb(rep_ack);*/
        }
    }

    DBG_SEQ_ACK(entry);
    spin_unlock(&entry->lock);

    sn_conntrack_put(entry);

    DBG(" ]\n");
    return result;
}

static void conntrack_create(pkt_info_t * pkt_info)
{
    u32 hash =
        snoop_hash(pkt_info->saddr, pkt_info->sport, pkt_info->daddr,
                pkt_info->dport);
    sn_conntrack_t *entry;
    printk("TRACK TCP, SRC:%d.%d.%d.%d. DST:%d.%d.%d.%d sport:%d dport:%d\n",
            ((unsigned char*)&(pkt_info->saddr))[3],
            ((unsigned char*)&(pkt_info->saddr))[2],
            ((unsigned char*)&(pkt_info->saddr))[1],
            ((unsigned char*)&(pkt_info->saddr))[0],
            ((unsigned char*)&(pkt_info->daddr))[3],
            ((unsigned char*)&(pkt_info->daddr))[2],
            ((unsigned char*)&(pkt_info->daddr))[1],
            ((unsigned char*)&(pkt_info->daddr))[0],
            pkt_info->sport,
            pkt_info->dport
       );
    DBG("SNOOP:%s [ ", __FUNCTION__);

    write_lock(&sn_htable[hash].lock);
    entry = sn_conntrack_lookup(hash,
            pkt_info->daddr, pkt_info->dport,
            pkt_info->saddr, pkt_info->sport);
    if (entry) {
        if (!test_and_set_bit(FL_SNOOP_DESTROYING, &entry->flags)) {
            sn_conntrack_del(entry);

            if (del_timer(&entry->tmo_timer))
                sn_conntrack_put(entry);

            if (del_timer(&entry->rto_timer))
                sn_conntrack_put(entry);

            write_unlock(&sn_htable[hash].lock);
            sn_conntrack_put(entry);
        }
    } else {
        write_unlock(&sn_htable[hash].lock);
    }

    entry = kmem_cache_alloc(conntrack_cachep, GFP_ATOMIC);
    if (!entry) {
        printk(KERN_ERR
                "SNOOP: Unable to allocate space for new connection\n");
        return;
    }

    memset((void *) entry, '\0', sizeof(sn_conntrack_t));

    INIT_HLIST_NODE(&entry->link);
    entry->hash = hash;
    spin_lock_init(&entry->lock);
    atomic_set(&entry->use_count, 1);	// 1st user - list, 2nd user - tmo timer
    entry->flags = 0;
    entry->state = SNOOP_SYNSENT;
    entry->waddr = pkt_info->saddr;
    entry->faddr = pkt_info->daddr;
    entry->wport = pkt_info->sport;
    entry->fport = pkt_info->dport;
    entry->rtt = msecs_to_jiffies(SNOOP_DEFAULT_RTT);
    entry->risn = pkt_info->seq;
    entry->first_ack = NULL;

    init_timer(&entry->tmo_timer);
    entry->tmo_timer.function = connection_timeout;
    entry->tmo_timer.data = (unsigned long) entry;
    mod_timer(&entry->tmo_timer, jiffies + SNOOP_CONN_TIMEO * HZ);

    init_timer(&entry->rto_timer);
    entry->rto_timer.function = retransmit_timeout;
    entry->rto_timer.data = (unsigned long) entry;

    INIT_LIST_HEAD(&entry->pkt_list);

    write_lock(&sn_htable[hash].lock);
    sn_conntrack_add(entry);
    snoop_stats.connections++;
    write_unlock(&sn_htable[hash].lock);

    DBG("SYNSENT]\n");
}

static void rtt_calc(sn_conntrack_t * ct, unsigned long pkt_send_time)
{
    unsigned long diff = jiffies - pkt_send_time;

    if (diff < 1)
        diff = 1;

    ct->rtt = (80 * ct->rtt + 20 * diff) / 100;
    if (ct->rtt < msecs_to_jiffies(SNOOP_MIN_RTT))
        ct->rtt = msecs_to_jiffies(SNOOP_MIN_RTT);
    /*ct->rtt = msecs_to_jiffies(SNOOP_MIN_RTT);*/

    DBG(" RTT<%lu/%lu>", ct->rtt, diff);
}

static int extract_sack_option(pkt_info_t * pkt_info, struct tcp_sack_block *sb)
{
    int result = 0;
    unsigned char *ptr;
    int length = pkt_info->optlen;
    int i, j;

    if (!length)
        return 0;

    ptr = pkt_info->opt;

    while (length > 0) {
        int opcode = *ptr++;
        int opsize;
        int i;

        if (opcode == TCPOPT_EOL)
            break;

        switch (opcode) {
            case TCPOPT_NOP:
                length--;
                continue;

            case TCPOPT_SACK:
                opsize = *ptr++;
                result = (opsize - 2) / 8;

                if (result > 4)
                    result = 4;

                for (i = 0; i < result; i++) {
                    sb[i].start_seq = ntohl(get_unaligned((__u32 *) ptr));
                    sb[i].end_seq = ntohl(get_unaligned((__u32 *) (ptr + 4)));
                    ptr += 8;
                }
                break;

            default:
                opsize = *ptr++;
                break;
        }

        length -= opsize;
        ptr += opsize - 2;
    }

    /* sort */
    for (j = 0; j < result - 1; j++) {
        for (i = j + 1; i < result; i++) {
            if (before(sb[i].start_seq, sb[j].start_seq)) {
                struct tcp_sack_block tmp = sb[i];
                sb[i] = sb[j];
                sb[j] = tmp;
            }
        }
    }

    return result;
}

static int extract_ts_option(unsigned char * ptr, int length, u32 * ts_a, u32 * ts_b)
{
    int result = 0;
    if (!length)
        return result;

    while (length > 0) {
        int opcode = *ptr++;
        int opsize;

        if (opcode == TCPOPT_EOL)
            break;

        switch (opcode) {
            case TCPOPT_NOP:
                length--;
                continue;

            case TCPOPT_TIMESTAMP:
                opsize = *ptr++;
                *ts_a = ntohl(*((u32*) (ptr)));
                *ts_b = ntohl(*((u32*) (ptr + 4)));
                result = 1;
                break;

            default:
                opsize = *ptr++;
                break;
        }

        length -= opsize;
        ptr += opsize - 2;
    }
    return result;
}

static int update_ts_option(unsigned char * ptr, int length, u32 ts_a, u32 ts_b)
{
    int result = 0;
    if (!length)
        return result;

    while (length > 0) {
        int opcode = *ptr++;
        int opsize;

        if (opcode == TCPOPT_EOL)
            break;

        switch (opcode) {
            case TCPOPT_NOP:
                length--;
                continue;

            case TCPOPT_TIMESTAMP:
                opsize = *ptr++;
                *((u32*) (ptr)) = htonl(ts_a);
                *((u32*) (ptr + 4)) = htonl(ts_b);
                result = 1;
                break;

            default:
                opsize = *ptr++;
                break;
        }

        length -= opsize;
        ptr += opsize - 2;
    }
    return result;
}

static unsigned int snoop_clean_packets(sn_conntrack_t * ct, u32 ack_seq)
{
    unsigned long result = 0;
    int pkt_count = 0;
    int data_size = 0;
    sn_packet_t *cur, *next;

    list_for_each_entry_safe(cur, next, &ct->pkt_list, list) {
        if (ack_seq == cur->seq || before(ack_seq, cur->seq))
            break;

        list_del(&cur->list);
        ct->pkt_count--;
        if (cur->skb)
            kfree_skb(cur->skb);
        result = cur->send_time;

        data_size = cur->size;
        pkt_count++;
        kmem_cache_free(pkt_cachep, cur);
    }

    DBG(" FLUSH<%ipkt/%ib>", pkt_count, data_size);

    return result;
}

static unsigned int snoop_ack(pkt_info_t * pkt_info)
{
    int found = 0;
    int alloc_first_ack = 0;
    u32 hash =
        snoop_hash(pkt_info->saddr, pkt_info->sport, pkt_info->daddr,
                pkt_info->dport);
    unsigned int result = NF_ACCEPT;
    sn_conntrack_t *entry;

    read_lock(&sn_htable[hash].lock);
    entry = sn_conntrack_lookup(hash,
            pkt_info->saddr, pkt_info->sport,
            pkt_info->daddr, pkt_info->dport);
    if (!entry) {
        read_unlock(&sn_htable[hash].lock);
        return result;
    }
    sn_conntrack_get(entry);
    read_unlock(&sn_htable[hash].lock);

    spin_lock(&entry->lock);
    restart_idle_timer(entry);

    DBG(KERN_INFO "SNOOP%i:ACK % 9i [",
            smp_processor_id(), pkt_info->ack_seq - entry->isn);

    snoop_stats.acks++;

    if (entry->state == SNOOP_WAITACK) {
        entry->state = SNOOP_ESTABLISHED;
        entry->last_ack = pkt_info->ack_seq;
        entry->last_ack_gen = pkt_info->ack_seq;
        if (entry->first_ack != NULL) {
            kfree_skb(entry->first_ack);
        }
        entry->first_ack = skb_copy(pkt_info->skb, GFP_ATOMIC);
        if (entry->first_ack == NULL) {
            printk(KERN_ERR "SNOOP: cannot allocate ack\n");
        }
        entry->first_seq = pkt_info->ack_seq;
        snoop_stats.ack_resets++;
        alloc_first_ack = 1;
        DBG("ESTABLISHED");
    } else if (before(pkt_info->ack_seq, entry->last_ack)) {	// Spurious ACK
        DBG("SPU");
    } else if (before(entry->last_ack, pkt_info->ack_seq)) {	// new ACK
        unsigned long pkt_send_time = 0;

        DBG("NEW");

        snoop_stats.newacks++;

        pkt_send_time = snoop_clean_packets(entry, pkt_info->ack_seq);
        entry->last_ack = pkt_info->ack_seq;
        entry->dack_count = 0;

        if (pkt_send_time)
            rtt_calc(entry, pkt_send_time);

        restart_retransmit_timer(entry);
    } else if (pkt_info->ack_seq == entry->last_ack) {
        struct tcp_sack_block sb[4];
        int sb_count = extract_sack_option(pkt_info, sb);
        sn_packet_t *cur;
        INFO("dup ack %u\n", pkt_info->ack_seq - entry->isn);

        snoop_stats.dupacks++;

        if (sb_count) {
            int i;
            DBG("SACK(");

            for (i = 0; i < sb_count; i++) {
                DBG("[%lu,%lu]",
                        (unsigned long) (sb[i].start_seq - entry->isn),
                        (unsigned long) (sb[i].end_seq - entry->isn));
            }

            DBG(") ");
        }
#if 0
        if (entry->last_window != pkt_info->window || pkt_info->size > 0) {
            DBG("WUP");
            goto out;
        }
#endif
        DBG("DUP(%i)", entry->dack_count);

        INFO("%d in pkt list\n", entry->pkt_count);
        if (list_empty(&entry->pkt_list)) {
            DBG(" LE");
            entry->dack_count++;
            goto out;
        }

        /* retransmit all not SACKed packets */
        if (sb_count) {
            int rxmit_count = 0;
            int lost_length = 0;
            u32 left_edge = pkt_info->ack_seq;
            int i;
            printk(KERN_ERR "SACK is NOT SUPPORTED!!!\n");

            for (i = 0; i < sb_count; i++) {
                if (sb[i].end_seq < pkt_info->ack_seq)
                    continue;

                lost_length += sb[i].start_seq - left_edge;
                left_edge = sb[i].end_seq;
            }

            list_for_each_entry(cur, &entry->pkt_list, list) {
                int rxmit = 0;
                int i;

                if (before(cur->seq, pkt_info->ack_seq))
                    continue;

                left_edge = pkt_info->ack_seq;

                for (i = 0; i < sb_count; i++) {
                    if ((left_edge == cur->seq || before(left_edge, cur->seq))
                            && before(cur->seq + cur->size, sb[i].start_seq)) {
                        rxmit = 1;
                        lost_length -= cur->size;
                        break;
                    }
                    left_edge = sb[i].end_seq;
                }

                if ((jiffies - cur->send_time) < 2 * entry->rtt)
                    continue;

                if (rxmit) {
                    sn_retransmit(cur);
                    rxmit_count++;
                }
            }

            if (rxmit_count) {
                DBG(" SACK RXMITS(%i/%i)", rxmit_count, lost_length);
            }

            if (lost_length <= 0) {
                result = NF_DROP;
                snoop_stats.dupacks_dropped++;
                goto out;
            }
        }


        if (entry->last_window != pkt_info->window || pkt_info->size > 0) {
            DBG(" WUP");
            snoop_stats.win_updates++;
            goto out;
        }

        int retx_cnt = 0;

        list_for_each_entry(cur, &entry->pkt_list, list) {
            int rxmit = 0;
            int rxmit_prev = 0;
            int i;

            if (before(cur->seq, pkt_info->ack_seq))
                continue;

            if ((pkt_info->ack_seq == cur->seq )) {
                rxmit = 1;
                found = 1;
            }

            if (after(cur->seq, pkt_info->ack_seq)) {
                rxmit = 1;
                retx_cnt++;
            }

            if (retx_cnt > 3) {
                break;
            }

            if ((jiffies - cur->send_time) < 2 * entry->rtt)
                continue;

            if (found && rxmit) {
                INFO("retransmit %u\n", cur->seq - entry->isn);
                sn_retransmit(cur);
                break;
                continue;
            }

            if (cur->sender_rxmit) {
                INFO("sender retransmit %u\n", cur->seq - entry->isn);
                sn_retransmit(cur);
            }
        }

        if (found == 0) {
            INFO("%u not found in cache\n", pkt_info->ack_seq - entry->isn);
        }

        cur = list_entry(entry->pkt_list.next, sn_packet_t, list);
        if (after(cur->seq, pkt_info->ack_seq)) {	/* nothing: forward as is */
            DBG(" HB");
            entry->dack_count++;
            snoop_stats.cache_misses++;
            goto out;
        }

        /*if (cur->seq != pkt_info->ack_seq) {	[> pass ack to sender <]*/
            /*DBG("!!MS!!");*/
            /*snoop_stats.cache_misses++;*/
            /*goto out;*/
        /*}*/


        /*if ((jiffies - cur->send_time) > 2 * entry->rtt) {*/
            /*DBG(" RXMIT(%lu)", jiffies - cur->send_time);*/
            /*sn_retransmit(cur);*/
        /*}*/
        entry->dack_count++;

        snoop_stats.dupacks_dropped++;
        result = NF_DROP;
    }
out:
    if (!alloc_first_ack && !pkt_info->psh && pkt_info->size == 0) {
        result = NF_DROP;
        snoop_stats.dropped_acks++;
    }
    else {
        INFO("ack not dropped\n");
    }
    entry->last_window = pkt_info->window;
    DBG_SEQ_ACK(entry);
    spin_unlock(&entry->lock);

    sn_conntrack_put(entry);

    DBG("]\n");

    return result;
}

static void conntrack_destroy(enum sn_pkt_origin origin, pkt_info_t * pkt_info)
{
    sn_conntrack_t *entry;
    u32 wh_addr, fh_addr;
    u16 wh_port, fh_port;
    u32 hash;

    switch (origin) {
        case SNOOP_FROM_WH:
            wh_addr = pkt_info->saddr;
            fh_addr = pkt_info->daddr;
            wh_port = pkt_info->sport;
            fh_port = pkt_info->dport;
            break;

        case SNOOP_FROM_FH:
            fh_addr = pkt_info->saddr;
            wh_addr = pkt_info->daddr;
            fh_port = pkt_info->sport;
            wh_port = pkt_info->dport;
            break;

        default:
            return;
    }

    hash = snoop_hash(wh_addr, wh_port, fh_addr, fh_port);
    write_lock(&sn_htable[hash].lock);
    entry = sn_conntrack_lookup(hash, wh_addr, wh_port, fh_addr, fh_port);
    if (!entry || test_and_set_bit(FL_SNOOP_DESTROYING, &entry->flags)) {
        write_unlock(&sn_htable[hash].lock);
        return;
    }
    sn_conntrack_get(entry);
    sn_conntrack_del(entry);
    write_unlock(&sn_htable[hash].lock);

    spin_lock(&entry->lock);

    DBG("SNOOP: %s use_count(%i)\n",
            __FUNCTION__, atomic_read(&entry->use_count));

    if (del_timer(&entry->tmo_timer)) {
        sn_conntrack_put(entry);
        DBG("SNOOP: %s removed tmo_timer, U(%i)\n",
                __FUNCTION__, atomic_read(&entry->use_count));
    }

    if (del_timer(&entry->rto_timer)) {
        sn_conntrack_put(entry);
        DBG("SNOOP: %s removed rto_timer, U(%i)\n",
                __FUNCTION__, atomic_read(&entry->use_count));
    }

    spin_unlock(&entry->lock);
    sn_conntrack_put(entry);
}

static unsigned int process_pkt(enum sn_pkt_origin origin, pkt_info_t * pkt_info)
{
    unsigned int result = NF_ACCEPT;


    switch (origin) {
        case SNOOP_FROM_WH:
            if (pkt_info->rst || pkt_info->fin) {	// FIN or RST
                conntrack_destroy(origin, pkt_info);
                break;
            }

            if (pkt_info->syn && pkt_info->ack)	// SYN,ACK
                break;

            if (pkt_info->syn) {	// SYN
                conntrack_create(pkt_info);
                break;
            }

            if (pkt_info->ack)	// ACK
                result = snoop_ack(pkt_info);

            break;

        case SNOOP_FROM_FH:
            if ((pkt_info->syn && pkt_info->ack) || pkt_info->ack)
                result = snoop_data(pkt_info);
            break;

        default:
            break;
    }

    return result;
}

static void setup_pkt_info(pkt_info_t * result, struct sk_buff *skb)
{
    struct iphdr *iph;
    struct tcphdr *tcph, _tcph;

    memset(result, '\0', sizeof(pkt_info_t));

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
    /*iph = ipip_hdr(skb);*/
    iph = ip_hdr(skb);
#else
    iph = skb->nh.iph;
#endif
    tcph = skb_header_pointer(skb, iph->ihl << 2, sizeof(_tcph), &_tcph);

    result->saddr = ntohl(iph->saddr);
    result->daddr = ntohl(iph->daddr);
    result->sport = ntohs(tcph->source);
    result->dport = ntohs(tcph->dest);
    result->seq = ntohl(tcph->seq);
    result->ack_seq = ntohl(tcph->ack_seq);
    result->window = ntohs(tcph->window);
    result->size =
        ntohs(iph->tot_len) - (iph->ihl << 2) - (tcph->doff << 2);
    result->ack = tcph->ack;
    result->psh = tcph->psh;
    result->rst = tcph->rst;
    result->syn = tcph->syn;
    result->fin = tcph->fin;

    if (result->size > 1460) {
        printk("too large! %d\n", result->size);
    }

    result->optlen = (tcph->doff << 2) - sizeof(_tcph);
    if (result->optlen) {
        result->opt = skb_header_pointer(skb,
                (iph->ihl << 2) + sizeof(_tcph),
                result->optlen, result->_opt);
        if (!result->opt) {
            printk(KERN_ERR "SNOOP: skb_header_pointer: error\n");
            result->optlen = 0;
        }
    }

    result->skb = skb;
}

static inline int get_optlen(struct tcphdr * tcph)
{
    return (tcph->doff << 2) - sizeof(struct tcphdr);
}

static inline unsigned char * get_opt_ptr(struct sk_buff * skb, int optlen)
{
    struct iphdr *iph;
    unsigned char * tmp;
	u8		_opt[40];
    iph = ip_hdr(skb);
    tmp = skb_header_pointer(skb, (iph->ihl << 2) + sizeof(struct tcphdr), optlen, _opt);
    if (tmp == NULL || tmp == _opt) {
        return NULL;
    }
    else {
        return tmp;
    }
}


static inline int is_fx_dev(const char *dev)
{
    return strcmp(fh_dev, dev) == 0;
}

static inline int is_wl_dev(const char *dev)
{
    return strcmp(wh_dev, dev) == 0;
}

static inline enum sn_pkt_origin get_origin(const char *in, const char *out)
{
    if (is_wl_dev(in) && is_fx_dev(out))
        return SNOOP_FROM_WH;

    if (is_wl_dev(out) && is_fx_dev(in))
        return SNOOP_FROM_FH;

    return SNOOP_UNKNOWN;
}

static unsigned int snoop_nf_hook(
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,11,0)
        const struct nf_hook_ops *ops,
#else
        unsigned int hook,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
        struct sk_buff *pskb,
#else
        struct sk_buff **pskb,
#endif
        const struct net_device *in,
        const struct net_device *out,
        int (*okfn) (struct sk_buff *))
{

    enum sn_pkt_origin pkt_origin;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
    /*struct iphdr *iph = ipip_hdr(pskb);*/
    struct iphdr *iph = ip_hdr(pskb);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
    struct iphdr *iph = ipip_hdr(*pskb);
#else
    struct iphdr *iph = (*pskb)->nh.iph;
#endif
    pkt_info_t pkt_info;
    if (iph->protocol != 6)
    {
        return NF_ACCEPT;
    }


    if (strcmp(in->name, out->name) == 0)
        return NF_ACCEPT;

    pkt_origin = get_origin(in->name, out->name);
    if (pkt_origin == SNOOP_UNKNOWN)
        return NF_ACCEPT;

    if (!nf_forward)
        nf_forward = okfn;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,24)
    setup_pkt_info(&pkt_info, pskb);
#else
    setup_pkt_info(&pkt_info, *pskb);
#endif

    return process_pkt(pkt_origin, &pkt_info);
}

static int snoop_htable_alloc(void)
{
    unsigned long size;
    int i;

#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
    snoop_hash_rnd = (u32) ((num_physpages ^ (num_physpages >> 7)) ^
            (jiffies ^ (jiffies >> 6)));

    snoop_htable_size = (num_physpages << PAGE_SHIFT) / 16384;
    snoop_htable_size /= sizeof(struct list_head);

    if (num_physpages > (1024 * 1024 * 1024 / PAGE_SIZE))
        snoop_htable_size = 8192;
#else
    snoop_hash_rnd = (u32) ((totalram_pages ^ (totalram_pages >> 7)) ^
            (jiffies ^ (jiffies >> 6)));

    snoop_htable_size = (totalram_pages << PAGE_SHIFT) / 16384;
    snoop_htable_size /= sizeof(struct list_head);

    if (totalram_pages > (1024 * 1024 * 1024 / PAGE_SIZE))
        snoop_htable_size = 8192;
#endif

    if (snoop_htable_size < 16)
        snoop_htable_size = 16;

    snoop_conn_max = 8 * snoop_htable_size;

    size = sizeof(sn_hash_bucket_t) * snoop_htable_size;
    sn_htable = (void *) __get_free_pages(GFP_KERNEL, get_order(size));
    if (!sn_htable) {		// FIXME: use vmalloc as alternate allocator (like in ip_conntrack)
        printk(KERN_ERR "Unable to create snoop hash table\n");
        return -ENOMEM;
    }

    for (i = 0; i < snoop_htable_size; i++) {
        rwlock_init(&sn_htable[i].lock);
        INIT_HLIST_HEAD(&sn_htable[i].list);
    }

    return 0;
}

void snoop_htable_free(void)
{
    unsigned long size = sizeof(sn_hash_bucket_t) * snoop_htable_size;
    free_pages((unsigned long) sn_htable, get_order(size));
}

static struct nf_hook_ops snoop_ops = {
    .hook = snoop_nf_hook,
    .owner = THIS_MODULE,
    /*.pf = PF_INET,*/
    .pf = PF_BRIDGE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,25)
    .hooknum = NF_IP_FORWARD,
#else
    /*.hooknum = NF_INET_FORWARD,*/
    .hooknum = NF_BR_FORWARD,
#endif
    .priority = NF_IP_PRI_FILTER + 1
};

static int snoop_init(void)
{
    int result = 0;

    if (wh_dev == NULL || fh_dev == NULL) {
        printk(KERN_ERR
                "You should provide WIRELESS and FIXED interface names\n");
        return -EINVAL;
    }

    result = snoop_htable_alloc();
    if (result < 0)
        return result;

    conntrack_cachep = kmem_cache_create("ip_snoop_conntrack",
            sizeof(sn_conntrack_t), 0, 0,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
            NULL,
#endif
            NULL);
    if (!conntrack_cachep) {
        printk(KERN_ERR "SNOOP: Unable to create conntrack slab cache\n");
        return -ENOMEM;
    }

    pkt_cachep =
        kmem_cache_create("ip_snoop_pkt", sizeof(sn_packet_t), 0, 0,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,23)
                NULL,
#endif
                NULL);
    if (!pkt_cachep) {
        printk(KERN_ERR "SNOOP: Unable to create packet slab cache\n");
        result = -ENOMEM;
        goto free_ct_cache;
    }

    result = nf_register_hook(&snoop_ops);
    if (result < 0) {
        printk(KERN_ERR "SNOOP: can't register netfilter hook\n");
        goto free_pkt_cache;
    }

    printk("Berkley snoop version %s: %s\n", VERSION, AUTHOR);

#if 0
    printk("Berkley snoop version %s (%u buckets, %d max)"
            " - %Zd bytes per connection\n", VERSION,
            snoop_htable_size, snoop_conn_max, sizeof(sn_conntrack_t));

    printk("using %lu(0x%0X) nfmark for retransmitted segments\n",
            (unsigned long) retransmit_mark, retransmit_mark);
#endif
    return result;

free_pkt_cache:
    kmem_cache_destroy(pkt_cachep);
free_ct_cache:
    kmem_cache_destroy(conntrack_cachep);
    return result;
}

static void snoop_exit(void)
{
    int i;
    nf_unregister_hook(&snoop_ops);

    for (i = 0; i < snoop_htable_size; i++) {
        struct hlist_node *pos;
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
        struct hlist_node *n;
#endif
        sn_conntrack_t *ct;

        if (hlist_empty(&sn_htable[i].list))
            continue;

        write_lock(&sn_htable[i].lock);
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3,11,0)
        hlist_for_each_entry_safe(ct, pos, n, &sn_htable[i].list, link) {
#else
        hlist_for_each_entry_safe(ct, pos, &sn_htable[i].list, link) {
#endif
            if (test_and_set_bit(FL_SNOOP_DESTROYING, &ct->flags))
                continue;
            sn_conntrack_get(ct);
            sn_conntrack_del(ct);

            if (del_timer(&ct->tmo_timer))
                sn_conntrack_put(ct);

            if (del_timer(&ct->rto_timer))
                sn_conntrack_put(ct);

            sn_conntrack_put(ct);
        }
        write_unlock(&sn_htable[i].lock);
    }

    for (i = 0; i < snoop_htable_size; i++) {
        struct hlist_head *list = &sn_htable[i].list;
        rwlock_t *lock = &sn_htable[i].lock;

        read_lock(lock);

        while (!hlist_empty(list)) {
            read_unlock(lock);

            DBG("SNOOP%i:EXT [waiting for bucket (%04i) become empty]\n",
                    smp_processor_id(), i);

            schedule();
            read_lock(lock);
        }

        read_unlock(lock);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,17)
    kmem_cache_destroy(pkt_cachep);
    kmem_cache_destroy(conntrack_cachep);
#else
    if (kmem_cache_destroy(pkt_cachep) < 0)
        printk(KERN_WARNING "SNOOP: error destroying pkt_cachep\n");

    if (kmem_cache_destroy(conntrack_cachep) < 0)
        printk(KERN_WARNING "SNOOP: error destroying conntrack_cachep\n");
#endif
    snoop_htable_free();

    printk("Berkley snoop version %s unloaded, stats follows\n", VERSION);
    printk("SNOOP: Total %lu connections served\n",
            snoop_stats.connections);
    printk("SNOOP: Input packets %lu, %lu retransmitted\n",
            snoop_stats.input_pkts, snoop_stats.sender_rxmits);
    printk
        ("SNOOP: ACKs %lu total, %lu NEW, %lu DUP, %lu WUPD, %lu DROPPED_DUPACKS %lu DROPPED_ACKS %lu RESETS\n",
         snoop_stats.acks, snoop_stats.newacks, snoop_stats.dupacks,
         snoop_stats.win_updates, snoop_stats.dupacks_dropped, snoop_stats.dropped_acks, snoop_stats.ack_resets);
    printk
        ("SNOOP: Local retransmissions %lu, retransmission timeouts %lu, cache misses %lu\n",
         snoop_stats.local_rxmits, snoop_stats.rto,
         snoop_stats.cache_misses);
}

module_init(snoop_init);
module_exit(snoop_exit);
