/*
 * QEMU Host Memory Backend
 *
 * Copyright (C) 2013-2014 Red Hat Inc
 *
 * Authors:
 *   Igor Mammedov <imammedo@redhat.com>
 * Hacked by IA33 :)
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "system/hostmem.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object_interfaces.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include <dlfcn.h>
#include "qemu/event_notifier.h"
#include <poll.h>
#include <unistd.h>
#include <stdint.h>
#ifndef __unix__
#error "this hack only works at unix-like OS"
#endif

#pragma message "you're compiling hacked hostmem-ram.c Linux/Unixes only"
static MemoryRegion* global_rpc_mr = nullptr;

typedef struct { void* ptr; _Alignas(8) unsigned short err_code; } (*cust_rq_t)(unsigned long);
static bool
ram_backend_memory_alloc(HostMemoryBackend *backend, Error **errp)
{
/* HACKED using a load memory region from an existing virt addr */
 
  if(!(backend->is_rpc))
    goto ori;
  bool stat = true;
  printf("=====[HACK BEGIN]===== \n");
  if(backend->size > (((uint64_t)UINT32_MAX) + 1ULL)) {
    error_setg(errp,"[HACK.ERROR] size must smaller than 4GB size");
    stat = false;
    return stat;
  }  
  void* func_ptr = dlsym(RTLD_DEFAULT,"custom_request_addr");
  if(func_ptr == NULL) {
    error_setg(errp,"[HACK.ERROR] CAN NOT get the func ptr custom_request_addr");
    stat = false;
    return stat;
  }
  cust_rq_t func = (cust_rq_t)func_ptr;
  __typeof__(func(1ull)) result = func(backend->size);
  if( (result.ptr == NULL) ||
      (result.err_code != 0) ) {
    error_setg(errp,"[HACK.ERROR] invalid ret value from custom_request_addr");
    stat = false;
    return stat;
  }
  
  {
    //__attribute((cleanup(g_free)))
    const char* _name = host_memory_backend_get_name(backend);
    printf("[HACK.INFO] class: %s ptr: %p,size: %lu \n",_name,result.ptr,backend->size);
    memory_region_init_ram_device_ptr(&(backend->mr),OBJECT(backend),_name,backend->size,result.ptr);
    memory_ref(&(backend->mr));
  }
  
  printf("=====[HACK END]===== \n");
  return stat;

    
ori:
/* original code */
    g_autofree const char* name = NULL;
    name = host_memory_backend_get_name(backend);
    uint32_t ram_flags;
    if (!backend->size) {
        error_setg(errp, "can't create backend with size 0");
        return false;
    }
    
    ram_flags = backend->share ? RAM_SHARED : RAM_PRIVATE;
    ram_flags |= backend->reserve ? 0 : RAM_NORESERVE;
    ram_flags |= backend->guest_memfd ? RAM_GUEST_MEMFD : 0;
    return memory_region_init_ram_flags_nomigrate(&backend->mr, OBJECT(backend),
                                                  name, backend->size,
                                                  ram_flags, errp);
/* original code */

/* HACKED */    
}


//HACK
static bool hack_host_memory_backend_get_bool(Object* obj,Error** errp) {
  HostMemoryBackend* backend = MEMORY_BACKEND(obj);
  return backend->is_rpc;
}
static bool once_rpc_flag = false;
static void hack_host_memory_backend_set_bool(Object* obj,bool data,Error** errp) {
  if(!data) {
    error_setg(errp,"[HACK.ERROR] the bool flag can only set to on!");
    return;
  }
  HostMemoryBackend* backend = MEMORY_BACKEND(obj);
  if(!once_rpc_flag) { //this is the first call
    backend->is_rpc = true;
    once_rpc_flag = true;
  }
  else { //once_rpc_flag = true,once already completed,directly report error
    error_setg(errp,"[HACK.ERROR] the value must only be set once!");
  }
  return;
}
//HACK

static void
ram_backend_class_init(ObjectClass *oc, const void *data)
{
  HostMemoryBackendClass *bc = MEMORY_BACKEND_CLASS(oc);
  bc->alloc = ram_backend_memory_alloc;
}

static void
ram_backend_object_init(Object* obj)
{
  object_property_add_bool(obj,"is-rpc",
    hack_host_memory_backend_get_bool,
    hack_host_memory_backend_set_bool);
  object_property_set_description(obj, "is-rpc",
    "请输入文本 hyw 笑点解析,,,");
  HostMemoryBackendClass *backend = MEMORY_BACKEND(obj);  
  global_rpc_mr = &(backend->mr);
}

static void
ram_backend_object_finalize(Object obj)
{
  HostMemoryBackend* backend = MEMORY_BACKEND(obj);
  memory_unref(&(backend->mr));
  global_rpc_mr = nullptr;
  return;
}
static const TypeInfo ram_backend_info = {
    .name = TYPE_MEMORY_BACKEND_RAM,
    .parent = TYPE_MEMORY_BACKEND,
    .class_init = ram_backend_class_init,
    .instance_init = ram_backend_object_init,
    .instance_finalize = ram_backend_object_finalize
};

static void register_types(void)
{
    type_register_static(&ram_backend_info);
}

type_init(register_types);




/*
  -1 cannot map,boncebuffer copy happend
  0 false
  1 ok
*/
char __attribute__((gnu::visibility("default"),gnu::sysv_abi)) custom_memory_map(void* hw_addr,
                                                              void** addr_out,
                                                              unsigned long* plength_out,
                                                              bool is_write)
{
  MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;
  AddressSpace* as = &address_space_memory;
  AddressSpace _empty_example;
  memset(&_empty_example,0,sizeof(AddressSpace));
  MemoryRegion* final_flatview_mr;
  FlatView* fv;
  unsigned long len = *plength_out;
  unsigned long _xlat,l; //these are temp out param
  l = len;
  if(memcmp(as,&_empty_example,sizeof(AddressSpace)) == 0) {
    printf("[WARN] address_space_memory is nullptr\n");
    return 0;
  }
  if(len == 0 || hw_addr == NULL) {
    return 0;
  }
  RCU_READ_LOCK_GUARD();
  fv = address_space_to_flatview(as);
  final_flatview_mr = flatview_translate(fv,hw_addr,&_xlat,&l,is_write,attrs);
  if(!memory_access_is_direct(final_flatview_mr,is_write,attrs)) {
    printf("[WARN] coping happend \n");
    return -1;
  }
  memory_region_ref(final_flatview_mr);
  *plength_out = flatview_extend_translation(fv, hw_addr, len, final_flatview_mr, _xlat,
                                             l, is_write, attrs);
  fuzz_dma_read_cb(hw_addr, *plength_out, final_flatview_mr);
  *addr_out = qemu_ram_ptr_length(final_flatview_mr->ram_block, _xlat, plength_out, true, is_write);
  return 1;
}


/*

  -1 param error
  0 error
  1 ok

*/
char __attribute__((gnu::visibility("default"),gnu::sysv_abi)) custom_memory_unmap(void* host_virt_addr,
                                                              unsigned long length,
                                                              bool is_write,
                                                              unsigned long access_len)
{
  AddressSpace* as = &address_space_memory;
  AddressSpace _empty_example;
  memset(&_empty_example,0,sizeof(AddressSpace));
  if(length == 0)
    return -1;
  if((!is_write) && (access_len != 0))
    return -1;
  if(host_virt_addr == NULL)
    return -1;
  if(memcmp(as,&_empty_example) == 0,sizeof(AddressSpace)) {
    printf("[WARN] address_space_memory is nullptr\n");
    return 0;
  }
  address_space_unmap(as,host_virt_addr,length,is_write,access_len);
  return 1;
}


bool is_init = false;
struct __custom_table {
  struct __custom_table* next;
  void* start_addr;
  _Alignas(8) unsigned int length;
  _Alignas(8) bool is_monitored;
};
static struct __custom_table head = { 0 };

static bool custom_internal_check_qemu() {
wait_qemu:
  if(global_rpc_mr == NULL) {
    sleep(1);
    goto wait_qemu;
  }
  sleep(1);
//check if's the addr match?
  struct __custom_table* cur_node = &head;
  while(true) {
    if(cur_node == NULL) {
      break;
    }
//try memory_region_from_host
    unsigned long offset = 0;
    MemoryRegion* mr = memory_regoin_from_host(cur_node->start_addr,&offset);
    if(mr == NULL) {
      goto rollback;
    }
    cur_node = cur_node->next;
  }
  retrun true;
rollback:
  cur_node = &head;
  static struct __custom_table prev_node = (&head)->next;
  cur_node = cur_node->next;
  cur_node = cur_node->next;
  while(true) {
    free(prev_node);
    prev_node = cur_node;
    if(cur_node->next == NULL) {
      free(prev_node);
      break;
    }    
    cur_node = cur_node->next;    
  }
  return false;
}


/*
  trust the init_ptr,but do not trust the content
*/

bool __attribute__((gnu::visibility("default"),gnu::sysv_abi)) custom_request_init_monitor(struct {  void* next;  void* start_addr;  _Alignas(8) unsigned int length;}* init_ptr)
//init_ptr never be NULL
{
#ifdef _CONCURRENCY_ASSHOLE
   bool is_failure = false;
  __asm__ volatile (
    "mov %[addr],%%rcx \n\t"
    "mov $0,%%al \n\t"
    "mov $1,%%dl \n\t"
    "lock cmpxchgb %%dl,(%%rcx) \n\t" //modify al but discarded
    "mfence \n\t"
    "jz 1f \n\t"
    "movb $1,%[is_failure] \n\t" //if the is_init = true,set is_failure = true
    "1: \n\t" //if the is_init = false update the flag,if the function has no error,keep the flag true,otherwise the flag will turn back to false in the end
    "movb $1,(%%rcx) \n\t"  //the the flag to true,this operation was preceded to keep the lock,!!NEED TO RELEASE at error point!!
    : [is_failure]"=rm"(is_failure)
    : [addr]"rm"(&is_init)
    : "rcx","dl","al","memory"
  );
  if(is_failure) {
    return false;
  }
#else
  //by default,is_init are lazy.because of every call-in are serial
  if(is_init) {
    return false;
  }
#endif
  __typeof__(init_ptr) cur_node = (__typeof__(init_ptr))init_ptr->next;
  struct __custom_table* prev_node = &head;
  head.next = NULL;
  head.length = init_ptr->length;
  head.start_addr = init_ptr->start_addr;
  head.is_monitored = false;
  while(true) {
    if(cur_node == NULL) {
      break;
    }
    struct __custom_table* new_node = malloc(sizeof(struct __custom_table));
    if(new_node == NULL) {
#ifdef _CONCURRENCY_ASSHOLE //in this case is_init is always true,but failured ,thus, unlock the flag,change state to init-able
      is_init = false;
#endif
      return false;
    }
    new_node->start_addr = cur_node->start_addr;
    new_node->next = NULL;
    new_node->length = cur_node->length;
    new_node->is_monitored = false;

    cur_node = (__typeof__(init_ptr))cur_node->next;
    prev_node->next = new_node;
    prev_node = new_node;
  }
  if(custom_internal_check_qemu()) {
    is_init = true;
    return true;
  }
  else {
#ifdef _CONCURRENCY_ASSHOLE
    is_init = false;
#endif
    return false;
  }
}
//todo custom_request_init_monitor using global_rpc_mr check the monitor and initialize context


static void custom_internal_cleanup(struct __custom_table* matched_node) {
  if(matched_node == NULL) {
    return;
  }
  matched_node->is_monitored = false;
  return;
}

//todo custom_request_enter_monitor monitor specificed addr(host virt addr) using memory_region_add_evnetfd call custom_request_notify for accepting an event
bool __attribute__((gnu::visibility("default"),gnu::sysv_abi)) custom_request_enter_monitor(void* addr) {
  if(!is_init) return false;
  if(global_rpc_mr == NULL) return false;
  struct __custom_table* __attribute__((cleanup(custom_internal_cleanup))) matched_node = NULL;
  struct __custom_table* cur_node = &head;
  while(true) {
    if(cur_node == NULL) {
      break;
    }
    if(cur_node->start_addr == addr) {
      matched_node = cur_node;
    }
    cur_node = cur_node->next;
  }
  if(matched_node == NULL) {
    return false;
  }
#ifdef _CONCURRENCY_ASSHOLE
  bool is_failure = false;
  void* _is_addr = &(matched_node->is_monitored);
  __asm__ volatile (
    "mov %[addr],%%rcx \n\t"
    "mov $0,%%al \n\t"
    "mov $1,%%dl \n\t"
    "lock cmpxchgb %%dl,(%%rcx) \n\t"
    "mfence \n\t"
    "jz 1f \n\t"
    "movb $1,%[is_failure] \n\t"
    "1: \n\t"
    "movb $1,(%%rcx) \n\t"  //set to 1 first
    : [is_failure]"=rm"(is_failure)
    : [addr]"rm"(_is_addr)
    : "rcx","dl","al","memory"
  );
  if(is_failure) {
    return false;
  }
#else
  if(!(matched_node->is_monitored)) {
    return false;
  }
  matched_node->is_monitored = true;
#endif
  void* intial_ptr memory_region_get_ram_ptr(global_rpc_mr);
  if(initial_ptr == NULL) {
    return false;
  }
  unsigned long offset = ((unsigned long)addr - (unsigned long)initial_ptr);
  EventNotifier en;
  event_notifier_init(&en,0);
  memory_region_add_eventfd(global_rpc_mr,offset,1,true,0,&en);
  int efd = event_notifier_get_fd(&en);
  struct pollfd pfd = { .fd = efd, .events = POLLIN };
  unsigned long long _example_data = 0;  
  poll(&pfd, 1, -1);
  read(efd,&_example_data,8); //可以直接命中
  memory_region_del_eventfd(global_rpc_mr,offset,1,true,0,&en);
  event_notifier_cleanup(&en);
  return true;
}
