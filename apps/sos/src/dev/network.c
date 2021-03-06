/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the BSD 2-Clause license. Note that NO WARRANTY is provided.
 * See "LICENSE_BSD2.txt" for details.
 *
 * @TAG(NICTA_BSD)
 */

/****************************************************************************
 *
 *      $Id: network.c,v 1.1 2003/09/10 11:44:38 benjl Exp $
 *
 *      Description: Initialise the network stack and NFS library.
 *
 *      Author:      Ben Leslie
 *
 ****************************************************************************/

#include "network.h"

#include <autoconf.h>

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <nfs/nfs.h>
#include <lwip/init.h>
#include <netif/etharp.h>
#include <ethdrivers/lwip_iface.h>
#include <ethdrivers/imx6EthernetCard.h>
#include <cspace/cspace.h>

#include "dev/dma.h"
#include "vm/mapping.h"
#include "ut_manager/ut.h"

#include "dev/nfs_dev.h"

#define verbose 0
#include <sys/debug.h>
#include <sys/panic.h>


#ifndef SOS_NFS_DIR
#  ifdef CONFIG_SOS_NFS_DIR
#    define SOS_NFS_DIR CONFIG_SOS_NFS_DIR
#  else
#    define SOS_NFS_DIR ""
#  endif
#endif

#define ARP_PRIME_TIMEOUT_MS     1000
#define ARP_PRIME_RETRY_DELAY_MS   10

extern const seL4_BootInfo* _boot_info;

static struct net_irq {
    int irq;
    seL4_IRQHandler cap;
} *_net_irqs = NULL;
static int _nirqs = 0;

static seL4_CPtr _irq_ep;

fhandle_t mnt_point = { { 0 } };

struct netif *_netif;

/*******************
 ***  OS support ***
 *******************/

static void *
sos_map_device(void* cookie, uintptr_t addr, size_t size, int cached, ps_mem_flags_t flags){
    (void)cookie;
    return map_device((void*)addr, size);
}

static void
sos_unmap_device(void *cookie, void *addr, size_t size) {
}

void 
sos_usleep(int usecs) {
    /* We need to spin because we do not as yet have a timer interrupt */
    while(usecs-- > 0){
        /* Assume 1 GHz clock */
        volatile int i = 1000;
        while(i-- > 0);
        seL4_Yield();
    }

    /* Handle pending network traffic */
    while(ethif_input(_netif));
}

/*******************
 *** IRQ handler ***
 *******************/
void 
network_irq(void) {
    int i;
    /* skip if the network was not initialised */
    if(_irq_ep == seL4_CapNull){
        return;
    }
    /* Loop through network irqs until we find a match */
    for(i = 0; i < _nirqs; i++){
        int err;
        ethif_handleIRQ(_netif, _net_irqs[i].irq);
        err = seL4_IRQHandler_Ack(_net_irqs[i].cap);
        assert(!err);
    }
}

static seL4_CPtr
enable_irq(int irq, seL4_CPtr aep) {
    seL4_CPtr cap;
    int err;
    /* Create an IRQ handler */
    cap = cspace_irq_control_get_cap(cur_cspace, seL4_CapIRQControl, irq);
    conditional_panic(!cap, "Failed to acquire and IRQ control cap");
    /* Assign to an end point */
    err = seL4_IRQHandler_SetEndpoint(cap, aep);
    conditional_panic(err, "Failed to set interrupt endpoint");
    /* Ack the handler before continuing */
    err = seL4_IRQHandler_Ack(cap);
    conditional_panic(err, "Failure to acknowledge pending interrupts");
    return cap;
}

/********************
 *** Network init ***
 ********************/

static void
network_prime_arp(struct ip_addr *gw){
    int timeout = ARP_PRIME_TIMEOUT_MS;
    struct eth_addr* eth;
    struct ip_addr* ip;
    while(timeout > 0){
        /* Send an ARP request */
        etharp_request(_netif, gw);
        /* Wait for the response */
        sos_usleep(ARP_PRIME_RETRY_DELAY_MS * 1000);
        if(etharp_find_addr(_netif, gw, &eth, &ip) == -1){
            timeout += ARP_PRIME_RETRY_DELAY_MS;
        }else{
            return;
        }
    }
}

void 
network_init(seL4_CPtr interrupt_ep) {
    struct ip_addr netmask, ipaddr, gw;
    struct eth_driver* eth_driver;
    const int* irqs;
    int err;
    int i;

    ps_io_mapper_t io_mapper = {
        .cookie = NULL,
        .io_map_fn = sos_map_device,
        .io_unmap_fn = sos_unmap_device
    };
    ps_dma_man_t dma_man = {
        .cookie = NULL,
        .dma_alloc_fn = sos_dma_malloc,
        .dma_free_fn = sos_dma_free,
        .dma_pin_fn = sos_dma_pin,
        .dma_unpin_fn = sos_dma_unpin,
        .dma_cache_op_fn = sos_dma_cache_op
    };

    ps_io_ops_t io_ops = {
        .io_mapper = io_mapper,
        .dma_manager = dma_man
    };

    _irq_ep = interrupt_ep;

    /* Extract IP from .config */
    dprintf(3, "\nInitialising network...\n\n");
    err = 0;
    err |= !ipaddr_aton(CONFIG_SOS_GATEWAY,      &gw);
    err |= !ipaddr_aton(CONFIG_SOS_IP     ,  &ipaddr);
    err |= !ipaddr_aton(CONFIG_SOS_NETMASK, &netmask);
    conditional_panic(err, "Failed to parse IP address configuration");
    dprintf(3, "  Local IP Address: %s\n", ipaddr_ntoa( &ipaddr));
    dprintf(3, "Gateway IP Address: %s\n", ipaddr_ntoa(     &gw));
    dprintf(3, "      Network Mask: %s\n", ipaddr_ntoa(&netmask));
    dprintf(3, "\n");

    /* low level initialisation */
    eth_driver = ethif_imx6_init(0, io_ops);
    assert(eth_driver);

    /* Initialise IRQS */
    irqs = ethif_enableIRQ(eth_driver, &_nirqs);
    _net_irqs = (struct net_irq*)calloc(_nirqs, sizeof(*_net_irqs));
    for(i = 0; i < _nirqs; i++){
        _net_irqs[i].irq = irqs[i];
        _net_irqs[i].cap = enable_irq(irqs[i], _irq_ep);
    }

    /* Setup the network interface */
    lwip_init();
    _netif = (struct netif*)malloc(sizeof(*_netif));
    assert(_netif != NULL);
    _netif = netif_add(_netif, &ipaddr, &netmask, &gw, 
                       eth_driver, ethif_init, ethernet_input);
    assert(_netif != NULL);
    netif_set_up(_netif);
    netif_set_default(_netif);

    /*
     * LWIP does not queue packets while waiting for an ARP response 
     * Generally this is okay as we block waiting for a response to our
     * request before sending another. On the other hand, priming the
     * table is cheap and can save a lot of heart ache 
     */
    network_prime_arp(&gw);

    /* initialise and mount NFS */
    if(strlen(SOS_NFS_DIR)) {
        /* Initialise NFS */
        int err;
        dprintf(3, "\nMounting NFS\n");
        if(!(err = nfs_init(&gw))){
            /* Print out the exports on this server */
            nfs_print_exports();
            if ((err = nfs_mount(SOS_NFS_DIR, &mnt_point))){
                dprintf(3, "Error mounting path '%s'!\n", SOS_NFS_DIR);
            }else{
                dprintf(3, "\nSuccessfully mounted '%s'\n", SOS_NFS_DIR);
            }
        }
        if(err){
            WARN("Failed to initialise NFS\n");
        }
    }else{
        WARN("Skipping Network initialisation since no mount point was "
             "specified\n");
    }
}
