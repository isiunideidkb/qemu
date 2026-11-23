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
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
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


//todo 主动生产端


//note 不像host的代码 这里的代码返回并不代表确实发生了通知 因为有太多因素（API）导致的 需要结合Client判断 才可以做到 返回就一定发生了！ 还需要进一步做更多的条件判断

//时序？我觉得大概率绝对是用的ioeventfd 发生后才通知
static void ivshmem_io_write(void *opaque, hwaddr addr,uint64_t val, unsigned size){}

//生产是一个主动消费的 这个是硬件处理被动执行的回调! 用于兜底 这个offset也是一个全局的! 这个不要加锁 不要解锁 加锁在生产端 解锁在客户端说 0的时候
//生产永远是主动地尝试写入offset(不计后果)同时 pci_set_irq(pci_dev,1),pci_set_irq(pci_dev,0); 然后硬件收到中断 被动执行ivshmem_io_read ivshmem_io_write_reply
static uint64_t ivshmem_io_read(void *opaque, hwaddr addr,
                                unsigned size)
{
    //只能从开头读4 8 字节
    if(size == 1 || size == 2) {
      printf("[HACK] MMIO received an illegal write,ignored\n");
      return 0;
    }
    if(addr != 0) { /* todo 高16字节地址的读取处理 代表一些扩展位 但是还没有定义 */
      printf("[HACK] MMIO received an illegal addr,ignored\n");
      return 0;
    }
    //不要锁住! 不要动锁! 直接 return offset
    return 0; /* return the offset! 64byte! */
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

//一般只有client才会看我有没有置回 1 以及看当前的状态 例如说是 0锁住 还是1松开 如果client读到任何其他的东西 这次的直接忽略 就代表host出错了
static uint64_t io_read_reply(void* opaque,hwaddr addr,unsigned size){
  //只能从开头读4 字节
  if(size != 4 || addr != 0) {
    printf("[HACK] MMIO-reply received an illegal read,ignored\n");
    abort();
  }
  PCIDevice* pci_dev = PCI_DEVICE(opaque);
  uint64_t bar_hw_addr = pci_bar_address(pci_dev,4,PCI_BASE_ADDRESS_SPACE_MEMORY|PCI_BASE_ADDRESS_MEM_PREFETCH,16);
  if (bar_hw_addr == PCI_BAR_UNMAPPED) {
    printf("[!!HACK!!] FATAL ERROR MMIO-reply cannot map BAR4\n");
    abort();  
  }
  unsigned long flag = 0xFFFFFFFFFFFFFFFF;
  cpu_physical_memory_read(bar_hw_addr,&flag,8); //无法保证一定发生
  return *flag;
}

//host写0 然后应该期待立即返回0 如果没有 代表大概率有问题
static void io_write_reply(void *opaque, hwaddr addr,uint64_t val, unsigned size)
{
  //只能从开头写4 字节
  if(addr != 0 || size != 4){
    printf("[HACK] MMIO-reply received an illegal write,ignored\n");
    return;
  }
  if(val == 0) {
    PCIDevice* pci_dev = PCI_DEVICE(opaque);
    uint64_t bar_hw_addr = pci_bar_address(pci_dev,4,PCI_BASE_ADDRESS_SPACE_MEMORY|PCI_BASE_ADDRESS_MEM_PREFETCH,16);
    if (bar_hw_addr == PCI_BAR_UNMAPPED) {
      printf("[!!HACK!!] FATAL ERROR MMIO-reply cannot map BAR4 to resume the Flag! \n");
      abort();
    }
    unsigned long resume_data = 1; //重新恢复到1
    cpu_physical_memory_write(bar_hw_addr,&resume_data,8);
    
    bar_hw_addr = pci_bar_address(pci_dev,0,PCI_BASE_ADDRESS_SPACE_MEMORY|PCI_BASE_ADDRESS_MEM_TYPE_64|PCI_BASE_ADDRESS_MEM_PREFETCH,16);
    if (bar_hw_addr == PCI_BAR_UNMAPPED) {
      printf("[!!HACK!!] FATAL ERROR MMIO-reply cannot map BAR4 to resume the Flag! \n");
      abort();
    }    
    resume_data = 0;
    cpu_physical_memory_write(bar_hw_addr,&resume_data,8); //无法保证一定发生
    /* release lock */  
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
  客户端编写指南  大致伪代码
  客户端在接收到中断后 可以尝试
  void give_up_times = 20;
  void handle_interrupt() {
  retry:
    give_up_times -= 1;
    if(give_up_times == 0)
      //give up
    bar = read(addr_of_bar0);
    if(0 == bar)
      goto retry;
    //存储bar
    if(read(addr_of_bar4) != 1) {
      if(read(addr_of_bar4) == 0xFFFFFFFF) //host的读发生失败了 略过这个
        exit;
      else if read(addr_of_bar4) != 1//有两种可能 不知道哪个乱写 写1直接 清空 
        write(addr_of_bar4,1), FATAL_WARNING("");
      else
        write(); //this is nortmal
    }
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
