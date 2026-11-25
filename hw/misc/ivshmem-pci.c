/*
 * Inter-VM Shared Memory PCI device.
 *
 * Author:
 *      Cam Macdonell <cam@cs.ualberta.ca>
 *
 * Based On: cirrus_vga.c
 *          Copyright (c) 2004 Fabrice Bellard
 *          Copyright (c) 2004 Makoto Suzuki (suzu)
 *
 *      and rtl8139.c
 *          Copyright (c) 2006 Igor Kovalenko
 *
 * This code is licensed under the GNU GPL v2.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-properties-system.h"
#include "system/kvm.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/event_notifier.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "chardev/char-fe.h"
#include "system/hostmem.h"
#include "qapi/visitor.h"

#include "hw/misc/ivshmem.h"
#include "qom/object.h"

#define IVSHMEM_DEBUG 0
#define IVSHMEM_DPRINTF(fmt, ...)                       \
    do {                                                \
        if (IVSHMEM_DEBUG) {                            \
            printf("IVSHMEM: " fmt, ## __VA_ARGS__);    \
        }                                               \
    } while (0)

#define TYPE_IVSHMEM_COMMON "ivshmem-common"
typedef struct IVShmemState IVShmemState;
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_COMMON,
                         TYPE_IVSHMEM_COMMON)

#define TYPE_IVSHMEM_PLAIN "ivshmem-plain"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_PLAIN,
                         TYPE_IVSHMEM_PLAIN)

#define TYPE_IVSHMEM_DOORBELL "ivshmem-doorbell"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM_DOORBELL,
                         TYPE_IVSHMEM_DOORBELL)

#define TYPE_IVSHMEM "ivshmem"
DECLARE_INSTANCE_CHECKER(IVShmemState, IVSHMEM,
                         TYPE_IVSHMEM)


struct IVShmemState {
    PCIDevice parent_obj;
    HostMemoryBackend *hostmem;
    MemoryRegion ivshmem_mmio;   /* BAR 0 写的 */
    MemoryRegion *ivshmem_bar2;  /* BAR 2 */
    MemoryRegion ivshmem_reply; /* BAR4 客户端写回执 */
    qemu_irq irq_obj;
};


//readonly,write has no side effect
static void ivshmem_io_write(void *opaque, hwaddr addr,uint64_t val, unsigned size){
  printf("[!!HACK!!] WARN DO NOT WRITE BAR0(no size effect)");
  return;
}


static unsigned long* consume_shared_v = NULL;

//readonly
static uint64_t ivshmem_io_read(void *opaque, hwaddr addr,
                                unsigned size)
{
    //只能从开头读
    if(addr != 0) {
      printf("[!!HACK!!] illegal reading for BAR0(no size effect)");
      return 0;
    }
    //为什么这里是NULL 没有初始化就要abort 后面的reply写入没有初始化的也没有?
    //不同于write reply，read offset是非常重要的 绝大多数信息的持有者都是我们这边 客户端错误写了 我们可以判断并且忽略 那我错误地发送了呢?这种情况客户端不知道这个回应是否是合法的 并且大概率整个操作都是非法的 需要abort 只需要在这里abort
    return consume_shared_v ? *consume_shared_v : printf("[!HACK!] FATAL ERROR read for a un-consumed queue"),abort(),0 ;
}
static const MemoryRegionOps ivshmem_mmio_ops = {
    .read = ivshmem_io_read,
    .write = ivshmem_io_write, // note 尝试NULL 因为这个确实没啥用
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};


//对齐
static unsigned long client_reply_v = 1;


//rw
static uint64_t io_read_reply(void* opaque,hwaddr addr,unsigned size){
  if(qddr != 0)
    return 0;
  return client_reply_v;
}

//rw
static void io_write_reply(void *opaque, hwaddr addr,uint64_t val, unsigned size)
{
  //只能从开头写4 字节
  if(addr != 0 || size != 4){
    printf("[HACK] MMIO-reply received an illegal write(no side effect)\n");
    return;
  }
  if(val == 0) {
    //    client_reply_v = 1; //resume 实际上我们压根不需要这个
    
    //todo event_queue.return_back_and_drop(consume_shared_v);
    //todo 如果本身就没有消费你就先来了 打印警告(no side effect) 就是不需要去notify wait_client_writeback了
    //todo notify wait_client_writeback
  }
}
static const MemoryRegionOps ivshmem_mmio_ops_reply = {
    .read = ivshmem_io_read_reply,
    .write = ivshmem_io_write_reply,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};


/*
  客户端编写指南  大致伪代码 i386
  客户端在接收到中断后 可以尝试
  atomic if == using cmpxchg or xchg
  bool is_init = false;
  unsigned int old_v;
  unsigned short giveup_time = 5;
  void handle_interrupt() {
head:
    if(giveup_time == 0)
      //giveup
    atomic if(!is_init)
      bar0,old_v = read(addr_of_bar0);
      is_init = 1;
    else {
      bar0 = read(addr_of_bar0);
      if(old_v == bar0)
        giveup_time--, goto head;
      old_v = bar0;
      giveup_time = 5;
    }
    //存储bar
    //write(1,addr_of_bar4)
    //cmpxchg : 判断这个地址是否真的是1 如果是 就的确放到FIFO 事件正式成功 不是 直接out 但是还是需要LOG警告出来的 因为miss了 只有可能是清理失败了 没有报错然后用之前的 正好失败
    //对于这种情况 写入失败了 这个offset大概率也停摆了 直接BSOD 遇到不一样的直接BSOD
  }
*/

static void ivshmem_reset(DeviceState *d)
{
    IVShmemState *s = IVSHMEM_COMMON(d);
}

static void ivshmem_common_realize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = IVSHMEM_COMMON(dev);
    uint8_t *pci_conf;

    pci_conf = dev->config;
    pci_conf[PCI_COMMAND] = PCI_COMMAND_IO | PCI_COMMAND_MEMORY;
    pci_conf[PCI_INTERRUPT_PIN] = 3; //分配IRQ3
    s->irq_obj = pci_allocate_irq(dev);
    memory_region_init_io(&s->ivshmem_mmio, OBJECT(s), &ivshmem_mmio_ops, s,
                          "ivshmem-mmio", 16);

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->ivshmem_mmio);

    memory_region_init_io(&s->ivshmem_reply, OBJECT(s), &ivshmem_mmio_ops_reply, s,
                          "ivshmem-mmio-reply", 16);

    pci_register_bar(dev, 4, PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &s->ivshmem_reply);		     

		     

    /* plain mode: shared memory comes from a hostmem backend */

    if (s->hostmem != NULL) {
        IVSHMEM_DPRINTF("using hostmem\n");

        s->ivshmem_bar2 = host_memory_backend_get_memory(s->hostmem);
        host_memory_backend_set_mapped(s->hostmem, true);
    }

    pci_register_bar(PCI_DEVICE(s), 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     s->ivshmem_bar2);
}

static void ivshmem_exit(PCIDevice *dev)
{
    IVShmemState *s = IVSHMEM_COMMON(dev);

    if(s->hostmem && memory_region_is_mapped(s->ivshmem_bar2)) {
      host_memory_backend_set_mapped(s->hostmem, false);
    }
    qemu_free_irq(s->irq_obj);
}

static void ivshmem_common_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_common_realize;
    k->exit = ivshmem_exit;

    /* config_write left as default (pci_default_write_config) */

    k->vendor_id = PCI_VENDOR_ID_IVSHMEM;
    k->device_id = PCI_DEVICE_ID_IVSHMEM;
    k->class_id = PCI_CLASS_MEMORY_RAM;
    k->revision = 1;
    device_class_set_legacy_reset(dc, ivshmem_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    dc->desc = "Inter-VM shared memory";
}

static const TypeInfo ivshmem_common_info = {
    .name          = TYPE_IVSHMEM_COMMON,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IVShmemState),
    .abstract      = true,
    .class_init    = ivshmem_common_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ivshmem_plain_realize(PCIDevice *dev, Error **errp)
{
    IVShmemState *s = IVSHMEM_COMMON(dev);

    if (!s->hostmem) {
        error_setg(errp, "You must specify a 'memdev'");
        return;
    } else if (host_memory_backend_is_mapped(s->hostmem)) {
        error_setg(errp, "can't use already busy memdev: %s",
                   object_get_canonical_path_component(OBJECT(s->hostmem)));
        return;
    }

    ivshmem_common_realize(dev, errp);
}

static const Property ivshmem_plain_properties[] = {
    DEFINE_PROP_LINK("memdev", IVShmemState, hostmem, TYPE_MEMORY_BACKEND,
                     HostMemoryBackend *)
};

static void ivshmem_plain_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_plain_realize;
    device_class_set_props(dc, ivshmem_plain_properties);
}

static const TypeInfo ivshmem_plain_info = {
    .name          = TYPE_IVSHMEM_PLAIN,
    .parent        = TYPE_IVSHMEM_COMMON,
    .instance_size = sizeof(IVShmemState),
    .class_init    = ivshmem_plain_class_init,
};

/* ===== DOORBELL (disabled) ===== */
static void ivshmem_doorbell_init(Object *obj)
{
    printf("[HACK] do not use doorbell");
    abort();
}

static void ivshmem_doorbell_realize(PCIDevice *dev, Error **errp)
{
    printf("[HACK] do not use doorbell");
    abort();
}

static void ivshmem_doorbell_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = ivshmem_doorbell_realize;
}

static const TypeInfo ivshmem_doorbell_info = {
    .name          = TYPE_IVSHMEM_DOORBELL,
    .parent        = TYPE_IVSHMEM_COMMON,
    .instance_size = sizeof(IVShmemState),
    .instance_init = ivshmem_doorbell_init,
    .class_init    = ivshmem_doorbell_class_init,
};

static void ivshmem_register_types(void)
{
    type_register_static(&ivshmem_common_info);
    type_register_static(&ivshmem_plain_info);
    type_register_static(&ivshmem_doorbell_info);
}

type_init(ivshmem_register_types)
